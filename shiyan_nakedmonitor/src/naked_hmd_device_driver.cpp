//============ Shiyan Naked Monitor OpenVR driver ============
#include "naked_hmd_device_driver.h"

#include "driverlog.h"
#include "eye_tracker.h"
#include "virtual_head.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Settings sections. Namespaced with the driver name to avoid collisions.
static const char *kMainSection = "driver_shiyan_nakedmonitor";
static const char *kDisplaySection = "shiyan_nakedmonitor_display";

//-----------------------------------------------------------------------------
// Off-axis (general perspective) projection.
//-----------------------------------------------------------------------------
// Screen-space convention (eye tracker + OpenXR runtime): origin at the panel
// centre, +x right, +y up, +z toward the viewer; the panel plane is z = 0 and
// eye[2] is this eye's distance to it. Every frustum plane passes through the
// matching panel edge as seen from THAT eye, so each eye gets its own
// asymmetric (off-axis) frustum and the panel becomes the zero-parallax plane.
//
// OpenVR naming is inverted (Driver_API_Documentation.md): pfTop is the tan of
// the -Y (bottom) clipping plane, pfBottom the tan of the +Y (top) plane.
static void ComputeOffAxisTangents( const float eye[ 3 ], float screenWidth, float screenHeight,
	float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
	// GetCurrentEyes already clamps to >= 0.05 m; this is belt and braces so a
	// zero/negative distance can never flip the frustum.
	const float distance = ( eye[ 2 ] > 0.05f ) ? eye[ 2 ] : 0.05f;

	*pfLeft = ( -screenWidth * 0.5f - eye[ 0 ] ) / distance;
	*pfRight = ( screenWidth * 0.5f - eye[ 0 ] ) / distance;
	*pfTop = ( -screenHeight * 0.5f - eye[ 1 ] ) / distance;  // -Y edge
	*pfBottom = ( screenHeight * 0.5f - eye[ 1 ] ) / distance; // +Y edge
}

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static int32_t GetInt32OrDefault( const char *pchSection, const char *pchKey, int32_t unDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const int32_t value = vr::VRSettings()->GetInt32( pchSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : unDefault;
}

static float GetFloatOrDefault( const char *pchSection, const char *pchKey, float flDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const float value = vr::VRSettings()->GetFloat( pchSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : flDefault;
}

static bool GetBoolOrDefault( const char *pchSection, const char *pchKey, bool bDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const bool value = vr::VRSettings()->GetBool( pchSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : bDefault;
}

static void GetStringOrDefault( const char *pchSection, const char *pchKey, const char *pchDefault, std::string &out )
{
	char buffer[ 1024 ] = {};
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	vr::VRSettings()->GetString( pchSection, pchKey, buffer, sizeof( buffer ), &error );
	out = ( error == vr::VRSettingsError_None ) ? buffer : pchDefault;
}

//-----------------------------------------------------------------------------
// A minimal, valid chaperone description so SteamVR does not require the user
// to run Room Setup for a fixed-viewpoint HMD. The raw tracking origin sits on
// the floor (y = 0); the head is later placed at the configured seated height.
//-----------------------------------------------------------------------------
static std::string BuildChaperoneJson( uint64_t unUniverseId, float flPlayAreaMeters )
{
	const std::string area = std::to_string( flPlayAreaMeters );

	std::string json = "{";
	json += "\"jsonid\":\"chaperone_info\",";
	// Included as well because the driver docs spell this key "json_id" while the
	// on-disk .vrchap files use "jsonid"; the parser ignores the unused one.
	json += "\"json_id\":\"chaperone_info\",";
	json += "\"version\":5,";
	json += "\"time\":\"2026-01-01T00:00:00.000Z\",";
	json += "\"universes\":[{";
	json += "\"collision_bounds\":[],";
	json += "\"play_area\":[" + area + "," + area + "],";
	json += "\"seated\":{\"translation\":[0.0,0.0,0.0],\"yaw\":0.0},";
	json += "\"standing\":{\"translation\":[0.0,0.0,0.0],\"yaw\":0.0},";
	json += "\"universeID\":" + std::to_string( unUniverseId );
	json += "}]}";
	return json;
}

//-----------------------------------------------------------------------------
// Locate a monitor whose native resolution matches the panel, so the compositor
// window can be placed on it automatically.
//-----------------------------------------------------------------------------
#ifdef _WIN32
namespace
{
struct MonitorSearch
{
	int width;
	int height;
	bool found;
	int32_t x;
	int32_t y;
};

BOOL CALLBACK EnumMonitorProc( HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam )
{
	MonitorSearch *pSearch = reinterpret_cast<MonitorSearch *>( lParam );

	MONITORINFO info = {};
	info.cbSize = sizeof( info );
	if ( GetMonitorInfoA( hMonitor, &info ) )
	{
		const int width = info.rcMonitor.right - info.rcMonitor.left;
		const int height = info.rcMonitor.bottom - info.rcMonitor.top;
		if ( width == pSearch->width && height == pSearch->height )
		{
			pSearch->found = true;
			pSearch->x = info.rcMonitor.left;
			pSearch->y = info.rcMonitor.top;
			return FALSE; // stop enumerating
		}
	}
	return TRUE;
}

struct MonitorMatch
{
	bool found = false;
	int32_t x = 0;
	int32_t y = 0;
	int32_t width = 0;
	int32_t height = 0;
	std::string name;
};

static std::string ToLowerCopy( const std::string &value )
{
	std::string lowered = value;
	std::transform( lowered.begin(), lowered.end(), lowered.begin(),
		[]( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
	return lowered;
}

// monitorFriendlyDeviceName is WCHAR[]; narrow it for logging/comparison.
static std::string WideToUtf8( const wchar_t *value )
{
	if ( value == nullptr || value[ 0 ] == L'\0' )
	{
		return std::string();
	}
	const int bytes = WideCharToMultiByte( CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr );
	if ( bytes <= 1 )
	{
		return std::string();
	}
	std::string out( static_cast< size_t >( bytes ), '\0' );
	WideCharToMultiByte( CP_UTF8, 0, value, -1, &out[ 0 ], bytes, nullptr, nullptr );
	out.resize( static_cast< size_t >( bytes - 1 ) ); // drop the terminator
	return out;
}
} // namespace

static bool FindMonitorByResolution( int width, int height, int32_t *pOutX, int32_t *pOutY )
{
	MonitorSearch search = { width, height, false, 0, 0 };
	EnumDisplayMonitors( nullptr, nullptr, EnumMonitorProc, reinterpret_cast<LPARAM>( &search ) );
	if ( search.found )
	{
		*pOutX = search.x;
		*pOutY = search.y;
	}
	return search.found;
}

//-----------------------------------------------------------------------------
// Find a monitor by its EDID friendly name (e.g. "SYS_A_1E320H").
// QueryDisplayConfig reports physical desktop coordinates regardless of the
// process DPI-awareness, so the returned position is always usable as-is.
//-----------------------------------------------------------------------------
static bool FindMonitorByName( const std::string &nameSubstring, MonitorMatch *pOutMatch )
{
	if ( nameSubstring.empty() )
	{
		return false;
	}

	UINT32 unPathCount = 0;
	UINT32 unModeCount = 0;
	if ( GetDisplayConfigBufferSizes( QDC_ONLY_ACTIVE_PATHS, &unPathCount, &unModeCount ) != ERROR_SUCCESS )
	{
		return false;
	}

	std::vector<DISPLAYCONFIG_PATH_INFO> paths( unPathCount );
	std::vector<DISPLAYCONFIG_MODE_INFO> modes( unModeCount );
	if ( QueryDisplayConfig( QDC_ONLY_ACTIVE_PATHS, &unPathCount, paths.data(), &unModeCount, modes.data(), nullptr ) != ERROR_SUCCESS )
	{
		return false;
	}

	const std::string needle = ToLowerCopy( nameSubstring );
	for ( UINT32 i = 0; i < unPathCount; ++i )
	{
		DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
		targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		targetName.header.size = sizeof( targetName );
		targetName.header.adapterId = paths[ i ].targetInfo.adapterId;
		targetName.header.id = paths[ i ].targetInfo.id;
		if ( DisplayConfigGetDeviceInfo( &targetName.header ) != ERROR_SUCCESS )
		{
			continue;
		}

		const std::string friendly = WideToUtf8( targetName.monitorFriendlyDeviceName );
		if ( ToLowerCopy( friendly ).find( needle ) == std::string::npos )
		{
			continue;
		}

		const UINT32 unModeIdx = paths[ i ].sourceInfo.modeInfoIdx;
		if ( unModeIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || unModeIdx >= unModeCount )
		{
			continue;
		}
		const DISPLAYCONFIG_MODE_INFO &mode = modes[ unModeIdx ];
		if ( mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE )
		{
			continue;
		}

		pOutMatch->found = true;
		pOutMatch->x = mode.sourceMode.position.x;
		pOutMatch->y = mode.sourceMode.position.y;
		pOutMatch->width = static_cast<int32_t>( mode.sourceMode.width );
		pOutMatch->height = static_cast<int32_t>( mode.sourceMode.height );
		pOutMatch->name = friendly;
		return true; // first matching target wins
	}
	return false;
}
#else
static bool FindMonitorByResolution( int, int, int32_t *, int32_t * )
{
	return false;
}
#endif

//-----------------------------------------------------------------------------
// Pre-startup panel preparation (see header)
//-----------------------------------------------------------------------------
void PreparePanelBeforeDeviceAdd()
{
	if ( !GetBoolOrDefault( kDisplaySection, "eye_tracking", true ) )
	{
		DriverLog( "shiyan_nakedmonitor: eye tracking disabled - panel stays in 2D mode." );
		return;
	}

	EyeTracker::Start();

	// 1) Wait for the vendor Register (network round trip to HvHost; 10+ s when
	//    the service is cold). Bounded so a dead service cannot stall vrserver.
	if ( !EyeTracker::WaitForRegister( 30.0 ) )
	{
		DriverLog( "shiyan_nakedmonitor: eye-tracking Register failed or timed out - continuing "
				   "in 2D mode with default eye positions." );
		return;
	}

	// 2) The tracker only streams usable eye positions while the panel is in
	//    3D (SBS) mode - same sequence as the OpenXR runtime's session start.
	EyeTracker::Enter3DMode();

#ifdef _WIN32
	// 3) OPTIONAL (default off): 3D mode can re-time the panel (3840x2160 ->
	//    7680x2160) on some setups. vrserver samples GetWindowBounds and
	//    GetEyeOutputViewport exactly once, right after the device is added, so
	//    IF the resolution is expected to change we poll until the named monitor
	//    already reports the configured size. Currently the panel is driven at
	//    its current resolution, so this wait stays disabled (wait_3d_resolution).
	if ( GetBoolOrDefault( kDisplaySection, "wait_3d_resolution", false ) )
	{
		std::string monitorName;
		GetStringOrDefault( kDisplaySection, "monitor_name_substring", "", monitorName );
		const int32_t expectedWidth = GetInt32OrDefault( kDisplaySection, "window_width", 7680 );
		const int32_t expectedHeight = GetInt32OrDefault( kDisplaySection, "window_height", 2160 );

		MonitorMatch match = {};
		for ( int attempt = 0; attempt < 40; ++attempt ) // up to 10 s
		{
			if ( FindMonitorByName( monitorName, &match )
				 && match.width == expectedWidth && match.height == expectedHeight )
			{
				DriverLog( "shiyan_nakedmonitor: panel now at 3D resolution %dx%d at (%d, %d).",
					match.width, match.height, match.x, match.y );
				return;
			}
			::Sleep( 250 );
		}
		DriverLog( "shiyan_nakedmonitor: WARNING - panel did not reach %dx%d within 10 s (now %dx%d); "
				   "window/viewport geometry may be wrong for 3D mode.",
			expectedWidth, expectedHeight, match.width, match.height );
	}
#endif
}

//-----------------------------------------------------------------------------
// NakedDisplayComponent
//-----------------------------------------------------------------------------
NakedDisplayComponent::NakedDisplayComponent( const NakedDisplayConfig &config )
	: config_( config )
{
	resolved_window_x_ = config_.window_x;
	resolved_window_y_ = config_.window_y;
	resolved_window_width_ = config_.window_width;
	resolved_window_height_ = config_.window_height;

	if ( config_.auto_place_window )
	{
		// Make EnumDisplayMonitors return physical coordinates even when the
		// host process (vrserver) is DPI-unaware. No-op if already aware.
		SetProcessDPIAware();

		// 1) Preferred: match the panel by its monitor name. The panel changes
		//    resolution between 2D and 3D modes, so the name is the reliable
		//    identifier; the window is then sized to the current mode.
		MonitorMatch match;
		if ( FindMonitorByName( config_.monitor_name_substring, &match ) )
		{
			resolved_window_x_ = match.x;
			resolved_window_y_ = match.y;
			resolved_window_width_ = match.width;
			resolved_window_height_ = match.height;
			DriverLog( "shiyan_nakedmonitor: auto-placed window on monitor '%s' at (%d, %d), size %dx%d (matched by name).",
				match.name.c_str(), resolved_window_x_, resolved_window_y_,
				resolved_window_width_, resolved_window_height_ );
		}
		else
		{
			// 2) Fallback: exact resolution match.
			if ( !config_.monitor_name_substring.empty() )
			{
				DriverLog( "shiyan_nakedmonitor: no active monitor matched name '%s'; trying resolution %dx%d.",
					config_.monitor_name_substring.c_str(), config_.window_width, config_.window_height );
			}

			int32_t monitor_x = 0;
			int32_t monitor_y = 0;
			if ( FindMonitorByResolution( config_.window_width, config_.window_height, &monitor_x, &monitor_y ) )
			{
				resolved_window_x_ = monitor_x;
				resolved_window_y_ = monitor_y;
				DriverLog( "shiyan_nakedmonitor: auto-placed window on monitor at (%d, %d) for %dx%d.",
					resolved_window_x_, resolved_window_y_, config_.window_width, config_.window_height );
			}
			else
			{
				DriverLog( "shiyan_nakedmonitor: no monitor matched %dx%d; falling back to configured position (%d, %d).",
					config_.window_width, config_.window_height, config_.window_x, config_.window_y );
			}
		}
	}

	// NOTE: the frustum is no longer configured here. GetProjectionRaw derives
	// an off-axis (general perspective) frustum every call from the physical
	// panel size and the current eye-tracker positions (see
	// ComputeOffAxisTangents), so the old symmetric FOV settings are gone.
}

bool NakedDisplayComponent::IsDisplayOnDesktop()
{
	// Extended-desktop mode: the compositor renders into a normal window which
	// we place on the SBS panel.
	return true;
}

bool NakedDisplayComponent::IsDisplayRealDisplay()
{
	// A REAL display, not a fictional one: our HMD is an actual monitor on the
	// desktop. Returning true makes vrcompositor treat the output as a real
	// panel (window placement + full stereo present into the window); with
	// false (as in Valve's old sample) the compositor falls back to a
	// single-eye debug/mirror present and the SBS split never reaches the
	// screen. See ValveSoftware/openvr issues #200 and #958.
	return true;
}

void NakedDisplayComponent::GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight )
{
	*pnWidth = static_cast< uint32_t >( config_.render_width );
	*pnHeight = static_cast< uint32_t >( config_.render_height );
}

void NakedDisplayComponent::GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
	const uint32_t half_width = static_cast< uint32_t >( resolved_window_width_ ) / 2;

	*pnY = 0;
	*pnWidth = half_width;
	*pnHeight = static_cast< uint32_t >( resolved_window_height_ );

	bool is_left = ( eEye == vr::Eye_Left );
	if ( config_.swap_eyes )
	{
		is_left = !is_left;
	}

	// Left eye on the left half, right eye on the right half: this is a plain
	// side-by-side layout, which is exactly what the SBS panel expects.
	*pnX = is_left ? 0 : half_width;

	// Log the first few calls so the compositor's actual viewport requests are
	// visible in vrserver.txt (they are not logged anywhere else).
	static int sViewportLogCount = 0;
	if ( sViewportLogCount < 6 )
	{
		++sViewportLogCount;
		DriverLog( "shiyan_nakedmonitor: GetEyeOutputViewport(%s) -> (%u, %u) %ux%u.",
			is_left ? "left" : "right", *pnX, *pnY, *pnWidth, *pnHeight );
	}
}

void NakedDisplayComponent::GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
	// Off-axis projection: build the frustum from the physical panel geometry
	// and this frame's (eye-tracked) eye position. Ported from the OpenXR
	// runtime's PopulateView(), with OpenVR's inverted top/bottom naming.
	float eyeLeft[ 3 ] = {};
	float eyeRight[ 3 ] = {};
	EyeTracker::GetCurrentEyes( eyeLeft, eyeRight );
	float screenWidth = 0.0f;
	float screenHeight = 0.0f;
	EyeTracker::GetScreenSize( &screenWidth, &screenHeight );

	const float *eye = ( eEye == vr::Eye_Left ) ? eyeLeft : eyeRight;
	ComputeOffAxisTangents( eye, screenWidth, screenHeight, pfLeft, pfRight, pfTop, pfBottom );

	static bool sLogged[ 2 ] = { false, false };
	const int eyeIndex = ( eEye == vr::Eye_Left ) ? 0 : 1;
	if ( !sLogged[ eyeIndex ] )
	{
		sLogged[ eyeIndex ] = true;
		DriverLog( "shiyan_nakedmonitor: GetProjectionRaw(%s) off-axis: eye=(%+.4f, %+.4f, %.4f) m, "
				   "panel=%.4fx%.4f m -> left=%.4f right=%.4f top=%.4f bottom=%.4f",
			eyeIndex == 0 ? "left" : "right", eye[ 0 ], eye[ 1 ], eye[ 2 ],
			screenWidth, screenHeight, *pfLeft, *pfRight, *pfTop, *pfBottom );
	}
}

vr::DistortionCoordinates_t NakedDisplayComponent::ComputeDistortion( vr::EVREye eEye, float fU, float fV )
{
	// Identity. The SBS panel has no lens, so no pre-warping must be applied.
	vr::DistortionCoordinates_t coordinates = {};
	coordinates.rfRed[ 0 ] = fU;
	coordinates.rfRed[ 1 ] = fV;
	coordinates.rfGreen[ 0 ] = fU;
	coordinates.rfGreen[ 1 ] = fV;
	coordinates.rfBlue[ 0 ] = fU;
	coordinates.rfBlue[ 1 ] = fV;
	return coordinates;
}

void NakedDisplayComponent::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
	*pnX = resolved_window_x_;
	*pnY = resolved_window_y_;
	*pnWidth = static_cast< uint32_t >( resolved_window_width_ );
	*pnHeight = static_cast< uint32_t >( resolved_window_height_ );

	static int sBoundsLogCount = 0;
	if ( sBoundsLogCount < 2 )
	{
		++sBoundsLogCount;
		DriverLog( "shiyan_nakedmonitor: GetWindowBounds -> (%d, %d) %ux%u.",
			*pnX, *pnY, *pnWidth, *pnHeight );
	}
}

bool NakedDisplayComponent::ComputeInverseDistortion( vr::HmdVector2_t *pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV )
{
	// Let SteamVR infer an estimate from ComputeDistortion (which is identity here).
	return false;
}

//-----------------------------------------------------------------------------
// Window placement watchdog
//-----------------------------------------------------------------------------
// SteamVR creates the compositor's "Headset Window" itself and has been
// observed to leave it parked at (32767, 32767) - outside every monitor -
// while the vrmonitor mirror keeps rendering normally, so the picture
// silently never reaches the panel. Re-assert our resolved bounds whenever
// they differ.
#ifdef _WIN32
static bool IsProcessNamed( DWORD unPid, const wchar_t *pchExeName )
{
	HANDLE hProcess = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, unPid );
	if ( hProcess == nullptr )
	{
		return false;
	}

	wchar_t path[ MAX_PATH ] = {};
	DWORD unSize = MAX_PATH;
	const BOOL ok = QueryFullProcessImageNameW( hProcess, 0, path, &unSize );
	CloseHandle( hProcess );
	if ( !ok )
	{
		return false;
	}

	const wchar_t *pchBase = path;
	for ( const wchar_t *p = path; *p != L'\0'; ++p )
	{
		if ( *p == L'\\' || *p == L'/' )
		{
			pchBase = p + 1;
		}
	}
	return _wcsicmp( pchBase, pchExeName ) == 0;
}

// Locate the compositor output window. The title is not localized.
static HWND FindCompositorHeadsetWindow()
{
	HWND window = FindWindowW( nullptr, L"Headset Window" );
	if ( window == nullptr )
	{
		return nullptr;
	}

	DWORD unPid = 0;
	GetWindowThreadProcessId( window, &unPid );
	if ( unPid == 0 || !IsProcessNamed( unPid, L"vrcompositor.exe" ) )
	{
		return nullptr;
	}
	return window;
}
#endif

void NakedDisplayComponent::EnforceWindowPlacement()
{
#ifdef _WIN32
	if ( !config_.auto_place_window )
	{
		return;
	}

	// RunFrame runs at the display rate; check a few times per second.
	if ( ++placement_check_count_ < 30 )
	{
		return;
	}
	placement_check_count_ = 0;

	// Window coordinates must be physical pixels; switch this thread to
	// Per-Monitor V2 DPI awareness for the duration of the check.
	typedef HANDLE ( WINAPI *PFN_SetThreadDpiAwarenessContext )( HANDLE );
	auto setContext = reinterpret_cast< PFN_SetThreadDpiAwarenessContext >(
		GetProcAddress( GetModuleHandleW( L"user32.dll" ), "SetThreadDpiAwarenessContext" ) );
	HANDLE previousContext = nullptr;
	if ( setContext != nullptr )
	{
		previousContext = setContext( reinterpret_cast< HANDLE >( static_cast< INT_PTR >( -4 ) ) ); // PER_MONITOR_AWARE_V2
	}

	HWND window = FindCompositorHeadsetWindow();
	if ( window != nullptr )
	{
		RECT rect = {};
		if ( GetWindowRect( window, &rect ) )
		{
			const LONG expectedRight = static_cast< LONG >( resolved_window_x_ + resolved_window_width_ );
			const LONG expectedBottom = static_cast< LONG >( resolved_window_y_ + resolved_window_height_ );
			if ( rect.left != resolved_window_x_ || rect.top != resolved_window_y_ ||
				 rect.right != expectedRight || rect.bottom != expectedBottom )
			{
				// SWP_ASYNCWINDOWPOS: post instead of send, so a compositor
				// thread that is currently waiting on vrserver cannot deadlock.
				SetWindowPos( window, nullptr, resolved_window_x_, resolved_window_y_,
					resolved_window_width_, resolved_window_height_,
					SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS );
				DriverLog( "shiyan_nakedmonitor: moved misplaced 'Headset Window' from (%ld, %ld)-(%ld, %ld) to (%d, %d) %dx%d.",
					rect.left, rect.top, rect.right, rect.bottom,
					resolved_window_x_, resolved_window_y_, resolved_window_width_, resolved_window_height_ );
			}
		}
	}

	if ( setContext != nullptr && previousContext != nullptr )
	{
		setContext( previousContext );
	}
#endif
}

//-----------------------------------------------------------------------------
// NakedHMDDeviceDriver
//-----------------------------------------------------------------------------
NakedHMDDeviceDriver::NakedHMDDeviceDriver()
{
	GetStringOrDefault( kMainSection, "model_number", "Shiyan Naked Monitor", model_number_ );
	GetStringOrDefault( kMainSection, "serial_number", "ShiyanNakedMonitor-0001", serial_number_ );

	NakedDisplayConfig display_config;
	display_config.window_x = GetInt32OrDefault( kDisplaySection, "window_x", display_config.window_x );
	display_config.window_y = GetInt32OrDefault( kDisplaySection, "window_y", display_config.window_y );
	display_config.window_width = GetInt32OrDefault( kDisplaySection, "window_width", display_config.window_width );
	display_config.window_height = GetInt32OrDefault( kDisplaySection, "window_height", display_config.window_height );
	display_config.render_width = GetInt32OrDefault( kDisplaySection, "render_width", display_config.render_width );
	display_config.render_height = GetInt32OrDefault( kDisplaySection, "render_height", display_config.render_height );
	display_config.auto_place_window = GetBoolOrDefault( kDisplaySection, "auto_place_window", display_config.auto_place_window );
	display_config.swap_eyes = GetBoolOrDefault( kDisplaySection, "swap_eyes", display_config.swap_eyes );
	GetStringOrDefault( kDisplaySection, "monitor_name_substring", "", display_config.monitor_name_substring );

	// --- Off-axis projection geometry --------------------------------------
	// Panel size seeds the tracker module; the SDK's GetScreenSize overrides it
	// as a "hardware fact" once the background Register finishes.
	const float screen_width_m = GetFloatOrDefault( kDisplaySection, "screen_width_m", 0.6981f );
	const float screen_height_m = GetFloatOrDefault( kDisplaySection, "screen_height_m", 0.3927f );
	screen_center_height_m_ = GetFloatOrDefault( kDisplaySection, "screen_center_height_m", screen_center_height_m_ );
	screen_distance_m_ = GetFloatOrDefault( kDisplaySection, "screen_distance_m", screen_distance_m_ );
	EyeTracker::SetDefaultScreenSize( screen_width_m, screen_height_m );

	// Eye tracking drives the eye positions used by the off-axis frustum (and
	// eye-to-head). When disabled, the default eye position [-32, 0, 500] /
	// [32, 0, 500] mm still produces a correct off-axis frustum.
	if ( GetBoolOrDefault( kDisplaySection, "eye_tracking", true ) )
	{
		EyeTracker::Start(); // non-blocking; Register runs on a background thread
	}
	else
	{
		DriverLog( "shiyan_nakedmonitor: eye tracking disabled by settings; using default eye positions." );
	}

	ipd_meters_ = GetFloatOrDefault( kDisplaySection, "ipd_meters", ipd_meters_ );
	eye_depth_meters_ = GetFloatOrDefault( kDisplaySection, "eye_depth_meters", eye_depth_meters_ );
	display_frequency_ = GetFloatOrDefault( kDisplaySection, "display_frequency", display_frequency_ );
	vsync_to_photons_ = GetFloatOrDefault( kDisplaySection, "vsync_to_photons", vsync_to_photons_ );
	seated_height_meters_ = GetFloatOrDefault( kDisplaySection, "seated_height_meters", seated_height_meters_ );

	is_on_desktop_property_ = GetBoolOrDefault( kDisplaySection, "is_on_desktop_property", is_on_desktop_property_ );
	display_debug_mode_ = GetBoolOrDefault( kDisplaySection, "display_debug_mode", display_debug_mode_ );

	universe_id_ = static_cast< uint64_t >( GetInt32OrDefault( kDisplaySection, "universe_id", static_cast< int32_t >( universe_id_ ) ) );
	provide_chaperone_ = GetBoolOrDefault( kDisplaySection, "provide_chaperone", provide_chaperone_ );
	play_area_meters_ = GetFloatOrDefault( kDisplaySection, "play_area_meters", play_area_meters_ );
	projection_push_mode_ = GetInt32OrDefault( kDisplaySection, "projection_push_mode", projection_push_mode_ );

	display_component_ = std::make_unique< NakedDisplayComponent >( display_config );

	DriverLog( "shiyan_nakedmonitor: model='%s' serial='%s'.", model_number_.c_str(), serial_number_.c_str() );
}

vr::EVRInitError NakedHMDDeviceDriver::Activate( uint32_t unObjectId )
{
	device_index_ = unObjectId;

	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer( device_index_ );

	vr::VRProperties()->SetStringProperty( container, vr::Prop_ModelNumber_String, model_number_.c_str() );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_SerialNumber_String, serial_number_.c_str() );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ManufacturerName_String, "Shiyan" );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_TrackingSystemName_String, "shiyan_nakedmonitor" );

	// --- Tracking universe + chaperone ------------------------------------
	// Without these, SteamVR cannot relate our raw tracking space to the
	// seated/standing origins, so it reports the HMD as "locating" and keeps
	// demanding Room Setup, which can never complete for a non-moving panel.
	vr::VRProperties()->SetUint64Property( container, vr::Prop_CurrentUniverseId_Uint64, universe_id_ );
	if ( provide_chaperone_ )
	{
		const std::string chaperone = BuildChaperoneJson( universe_id_, play_area_meters_ );
		vr::VRProperties()->SetStringProperty( container, vr::Prop_DriverProvidedChaperoneJson_String, chaperone.c_str() );
		DriverLog( "shiyan_nakedmonitor: provided chaperone, universeID=%llu.", static_cast< unsigned long long >( universe_id_ ) );
	}

	// The panel's designed stereo baseline, NOT the wearer's real IPD.
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_UserIpdMeters_Float, ipd_meters_ );

	// Required: without a non-zero refresh rate VRCompositor will not start.
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_DisplayFrequency_Float, display_frequency_ );

	vr::VRProperties()->SetFloatProperty( container, vr::Prop_UserHeadToEyeDepthMeters_Float, eye_depth_meters_ );
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_SecondsFromVsyncToPhotons_Float, vsync_to_photons_ );

	// Avoids the vrmonitor "headset display is not fullscreen" warning
	// (SteamVR error -202). Must be false, exactly like Valve's sample driver.
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_IsOnDesktop_Bool, is_on_desktop_property_ );

	// true => the compositor uses a normal window instead of exclusive
	// fullscreen, so it never fails trying to take over a desktop panel.
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_DisplayDebugMode_Bool, display_debug_mode_ );

	// --- Keep the HMD awake forever ---------------------------------------
	// SteamVR considers an HMD "active" only when it moves, when a boolean
	// input changes, or when an app launches. A naked-eye panel never moves, so
	// it would be put to standby after ~5 seconds ("move the headset to wake").
	// Per Valve's "Device sleep states" documentation, a device with a
	// /proximity boolean component set to true will NEVER go to sleep.
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_ContainsProximitySensor_Bool, true );
	vr::VRDriverInput()->CreateBooleanComponent( container, "/proximity", &proximity_handle_ );
	if ( proximity_handle_ != vr::k_ulInvalidInputComponentHandle )
	{
		vr::VRDriverInput()->UpdateBooleanComponent( proximity_handle_, true, 0.0 );
		DriverLog( "shiyan_nakedmonitor: /proximity set to true (HMD will stay awake)." );
	}
	else
	{
		DriverLog( "shiyan_nakedmonitor: WARNING - failed to create /proximity component." );
	}

	return vr::VRInitError_None;
}

void *NakedHMDDeviceDriver::GetComponent( const char *pchComponentNameAndVersion )
{
	if ( strcmp( pchComponentNameAndVersion, vr::IVRDisplayComponent_Version ) == 0 )
	{
		return display_component_.get();
	}
	return nullptr;
}

void NakedHMDDeviceDriver::DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize )
{
	if ( unResponseBufferSize >= 1 )
	{
		pchResponseBuffer[ 0 ] = 0;
	}
}

vr::DriverPose_t NakedHMDDeviceDriver::GetPose()
{
	vr::DriverPose_t pose = { 0 };

	// These must be valid quaternions or the device will not appear.
	pose.qWorldFromDriverRotation.w = 1.0;
	pose.qDriverFromHeadRotation.w = 1.0;

	// Virtual head (MMB look + WASD walk, see virtual_head.h): base position
	// at the seated height, offset by the walk, rotated by yaw/pitch.
	// Tracking space: -Z forward, +Y up, positive yaw = turn left.
	float yaw = 0.0f;
	float pitch = 0.0f;
	VirtualHead::GetYawPitch( &yaw, &pitch );
	const float halfYaw = yaw * 0.5f;
	const float halfPitch = pitch * 0.5f;
	const float cy = std::cos( halfYaw );
	const float sy = std::sin( halfYaw );
	const float cp = std::cos( halfPitch );
	const float sp = std::sin( halfPitch );
	// Ry(yaw) * Rx(pitch): w = cy*cp, x = cy*sp, y = cp*sy, z = -sy*sp.
	pose.qRotation.w = cy * cp;
	pose.qRotation.x = cy * sp;
	pose.qRotation.y = cp * sy;
	pose.qRotation.z = -sy * sp;

	float walkOffset[ 3 ] = {};
	VirtualHead::GetPositionOffset( walkOffset );

	// Viewer position (metres). x=right, y=up, z=back.
	pose.vecPosition[ 0 ] = walkOffset[ 0 ];
	pose.vecPosition[ 1 ] = seated_height_meters_ + walkOffset[ 1 ];
	pose.vecPosition[ 2 ] = walkOffset[ 2 ];

	pose.poseIsValid = true;
	pose.deviceIsConnected = true;
	pose.result = vr::TrackingResult_Running_OK;
	pose.shouldApplyHeadModel = true;

	return pose;
}

void NakedHMDDeviceDriver::UpdatePose()
{
	if ( device_index_ == vr::k_unTrackedDeviceIndexInvalid )
	{
		return;
	}

	// Re-assert the always-on proximity sensor so SteamVR never idles the HMD.
	if ( proximity_handle_ != vr::k_ulInvalidInputComponentHandle )
	{
		vr::VRDriverInput()->UpdateBooleanComponent( proximity_handle_, true, 0.0 );
	}

	vr::VRServerDriverHost()->TrackedDevicePoseUpdated( device_index_, GetPose(), sizeof( vr::DriverPose_t ) );
}

void NakedHMDDeviceDriver::EnforceWindowPlacement()
{
	if ( display_component_ != nullptr )
	{
		display_component_->EnforceWindowPlacement();
	}
}

void NakedHMDDeviceDriver::PushDisplayTransforms()
{
	if ( device_index_ == vr::k_unTrackedDeviceIndexInvalid )
	{
		return;
	}

	float eyeLeft[ 3 ] = {};
	float eyeRight[ 3 ] = {};
	EyeTracker::GetCurrentEyes( eyeLeft, eyeRight );

	// Only push when the eyes moved noticeably (0.5 mm) - this IPC is meant for
	// "the display changed", not for every single frame.
	constexpr float kPushEpsilonMeters = 0.0005f;
	bool changed = !pushed_display_transforms_;
	for ( int i = 0; i < 3 && !changed; ++i )
	{
		changed = std::fabs( eyeLeft[ i ] - last_pushed_eyes_[ i ] ) > kPushEpsilonMeters
			|| std::fabs( eyeRight[ i ] - last_pushed_eyes_[ 3 + i ] ) > kPushEpsilonMeters;
	}
	if ( !changed )
	{
		return;
	}
	pushed_display_transforms_ = true;
	last_pushed_eyes_[ 0 ] = eyeLeft[ 0 ];
	last_pushed_eyes_[ 1 ] = eyeLeft[ 1 ];
	last_pushed_eyes_[ 2 ] = eyeLeft[ 2 ];
	last_pushed_eyes_[ 3 ] = eyeRight[ 0 ];
	last_pushed_eyes_[ 4 ] = eyeRight[ 1 ];
	last_pushed_eyes_[ 5 ] = eyeRight[ 2 ];

	// --- Eye-to-head ------------------------------------------------------
	// Tracking space = screen-space coordinates translated to the panel centre
	// at (0, screen_center_height_m_, -screen_distance_m_); the head pose is
	// static at (0, seated_height_meters_, 0), so eyeToHead is the difference.
	auto fillEyeToHead = [this]( vr::HmdMatrix34_t &matrix, const float eye[ 3 ] ) {
		matrix = {};
		matrix.m[ 0 ][ 0 ] = 1.0f;
		matrix.m[ 1 ][ 1 ] = 1.0f;
		matrix.m[ 2 ][ 2 ] = 1.0f;
		matrix.m[ 0 ][ 3 ] = eye[ 0 ];
		matrix.m[ 1 ][ 3 ] = eye[ 1 ] + screen_center_height_m_ - seated_height_meters_;
		matrix.m[ 2 ][ 3 ] = eye[ 2 ] - screen_distance_m_;
	};
	vr::HmdMatrix34_t eyeToHeadLeft = {};
	vr::HmdMatrix34_t eyeToHeadRight = {};
	fillEyeToHead( eyeToHeadLeft, eyeLeft );
	fillEyeToHead( eyeToHeadRight, eyeRight );
	vr::VRServerDriverHost()->SetDisplayEyeToHead( device_index_, eyeToHeadLeft, eyeToHeadRight );

	// --- Off-axis projection ----------------------------------------------
	// Same tangents as GetProjectionRaw, packaged as per-eye rects. The y-axis
	// convention of SetDisplayProjectionRaw is undocumented; projection_push_mode
	// selects it so the correct one can be confirmed empirically (see header).
	// mode 0 => skip the projection push entirely.
	float screenWidth = 0.0f;
	float screenHeight = 0.0f;
	EyeTracker::GetScreenSize( &screenWidth, &screenHeight );

	if ( projection_push_mode_ != 0 )
	{
		const int yMode = projection_push_mode_;
		auto fillProjection = [screenWidth, screenHeight, yMode]( const float eye[ 3 ], vr::HmdRect2_t &rect ) {
			float left = 0.0f;
			float right = 0.0f;
			float top = 0.0f;    // -Y edge (OpenVR's inverted naming)
			float bottom = 0.0f; // +Y edge
			ComputeOffAxisTangents( eye, screenWidth, screenHeight, &left, &right, &top, &bottom );
			rect.vTopLeft.v[ 0 ] = left;
			rect.vBottomRight.v[ 0 ] = right;
			if ( yMode == 1 )
			{
				// y-up: vTopLeft is the upper-left corner (+Y edge on top).
				rect.vTopLeft.v[ 1 ] = bottom;
				rect.vBottomRight.v[ 1 ] = top;
			}
			else
			{
				// y-down: vTopLeft carries the numerically smaller ("upper") value.
				rect.vTopLeft.v[ 1 ] = top;
				rect.vBottomRight.v[ 1 ] = bottom;
			}
		};
		vr::HmdRect2_t projectionLeft = {};
		vr::HmdRect2_t projectionRight = {};
		fillProjection( eyeLeft, projectionLeft );
		fillProjection( eyeRight, projectionRight );
		vr::VRServerDriverHost()->SetDisplayProjectionRaw( device_index_, projectionLeft, projectionRight );
	}

	// Rate-limit the log: eye tracking makes this fire often while the user
	// moves; keep the first push plus one line per second.
	const auto now = std::chrono::steady_clock::now();
	if ( last_push_log_time_.time_since_epoch().count() == 0
		 || now - last_push_log_time_ >= std::chrono::seconds( 1 ) )
	{
		last_push_log_time_ = now;
		DriverLog( "shiyan_nakedmonitor: pushed display transforms: eyeL=(%+.4f, %+.4f, %.4f) "
				   "eyeR=(%+.4f, %+.4f, %.4f) panel=%.4fx%.4f (projection push mode %d)",
			eyeLeft[ 0 ], eyeLeft[ 1 ], eyeLeft[ 2 ],
			eyeRight[ 0 ], eyeRight[ 1 ], eyeRight[ 2 ],
			screenWidth, screenHeight, projection_push_mode_ );
	}
}

void NakedHMDDeviceDriver::EnterStandby()
{
	DriverLog( "shiyan_nakedmonitor: entering standby." );
}

void NakedHMDDeviceDriver::Deactivate()
{
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}
