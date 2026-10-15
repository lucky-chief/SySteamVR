//============ Shiyan Naked Monitor OpenVR driver ============
#include "naked_hmd_device_driver.h"

#include "driverlog.h"

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

static const float kPi = 3.14159265358979323846f;

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

// Tangent of half of an angle given in degrees.
static float HalfTanFromDegrees( float flDegrees )
{
	return std::tan( flDegrees * 0.5f * kPi / 180.0f );
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

	half_tan_h_ = HalfTanFromDegrees( config_.fov_horizontal_deg );

	if ( config_.fov_vertical_deg > 0.0f )
	{
		half_tan_v_ = HalfTanFromDegrees( config_.fov_vertical_deg );
	}
	else
	{
		// Derive the vertical FOV from the horizontal FOV and the per-eye aspect
		// ratio so the vertical scale is geometrically correct for a flat panel.
		const float eye_width = static_cast< float >( resolved_window_width_ ) * 0.5f;
		const float eye_height = static_cast< float >( resolved_window_height_ );
		half_tan_v_ = half_tan_h_ * ( eye_height / eye_width );
	}

	const float vertical_fov_deg = 2.0f * std::atan( half_tan_v_ ) * 180.0f / kPi;
	DriverLog( "shiyan_nakedmonitor: FOV horizontal=%.1f deg, vertical=%.1f deg.",
		config_.fov_horizontal_deg, vertical_fov_deg );
}

bool NakedDisplayComponent::IsDisplayOnDesktop()
{
	// Extended-desktop mode: the compositor renders into a normal window which
	// we place on the SBS panel.
	return true;
}

bool NakedDisplayComponent::IsDisplayRealDisplay()
{
	// Not a "real" display from the runtime's point of view; prevents it from
	// trying to drive a display output directly.
	return false;
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
}

void NakedDisplayComponent::GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
	// Note: per the OpenVR docs, "top" is the tangent to the top (+Y) edge and
	// "bottom" is the tangent to the bottom (-Y) edge. For a symmetric frustum
	// the sign convention does not matter.
	*pfLeft = -half_tan_h_;
	*pfRight = half_tan_h_;
	*pfTop = -half_tan_v_;
	*pfBottom = half_tan_v_;
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
	display_config.fov_horizontal_deg = GetFloatOrDefault( kDisplaySection, "fov_horizontal_deg", display_config.fov_horizontal_deg );
	display_config.fov_vertical_deg = GetFloatOrDefault( kDisplaySection, "fov_vertical_deg", display_config.fov_vertical_deg );

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
	pose.qRotation.w = 1.0;

	// Fixed viewer position (metres). x=right, y=up, z=back.
	pose.vecPosition[ 0 ] = 0.0;
	pose.vecPosition[ 1 ] = seated_height_meters_;
	pose.vecPosition[ 2 ] = 0.0;

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

void NakedHMDDeviceDriver::EnterStandby()
{
	DriverLog( "shiyan_nakedmonitor: entering standby." );
}

void NakedHMDDeviceDriver::Deactivate()
{
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}
