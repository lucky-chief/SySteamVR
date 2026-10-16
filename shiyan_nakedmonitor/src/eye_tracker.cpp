//============ Shiyan Naked Monitor OpenVR driver ============
#include "eye_tracker.h"

#include "driverlog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

// Eye-tracking bridge for the OpenVR driver. Ported from the validated
// OpenXR runtime project (SyOpenXRRuntime):
//   src/binocular_camera.cpp   - DLL loading / exports / async Register
//   src/xr_eyetracking_win.cpp - gating, fallback blending, screen size
// Behaviour (including every "why" comment about the vendor DLL) is kept in
// sync with that project; only logging (DriverLog) and the OpenVR driver
// lifecycle differ.

namespace
{

//-----------------------------------------------------------------------------
// Vendor DLL bridge (port of binocular_camera.cpp)
//-----------------------------------------------------------------------------

// Vendor camera_position_t: 12 floats (raw + filtered), returned by value.
// 31.5" 4K panel; front 6 = raw values (we use filter_level 0).
struct camera_position_t
{
	float lCamx, lCamy, lCamz;
	float rCamx, rCamy, rCamz;
	float lCamxFiltered, lCamyFiltered, lCamzFiltered;
	float rCamxFiltered, rCamyFiltered, rCamzFiltered;
};

// 视延科技 exports (x64, success 1 / failure -1, details via GetError()).
int ( *pRegister )( void ) = nullptr;
int ( *pUnregister )( void ) = nullptr;
camera_position_t ( *pGetCameraPosition )( int ) = nullptr;
int ( *pSetDisplayMode )( int ) = nullptr;
int ( *pGetTrackedState )( void ) = nullptr;

// Optional exports (absent => nullptr, registration still succeeds).
int ( *pGetScreenSize )( float * ) = nullptr;
int ( *pGetDisplayMode )( void ) = nullptr;
int ( *pGetError )( void ) = nullptr;

HMODULE hDll = nullptr;
bool dllLoaded = false;   // DLL loaded and required exports resolved
bool registered = false;  // Register() succeeded
std::mutex g_vendorMutex;

// Async registration state. The thread entry point IS the vendor's own
// Register, so the thread never executes code from this module - it can finish
// cleanly in ntdll even if the driver DLL unloads first.
enum RegisterStatus
{
	kRegisterIdle = 0,
	kRegisterPending = 1,
	kRegisterOk = 2,
	kRegisterFailed = 3,
};
std::atomic<int> g_registerStatus{ kRegisterIdle };
HANDLE g_registerThread = nullptr;

bool NarrowToWide( const std::string &narrow, std::wstring *out )
{
	if ( out == nullptr )
	{
		return false;
	}
	if ( narrow.empty() )
	{
		out->clear();
		return true;
	}
	const int need = ::MultiByteToWideChar( CP_UTF8, 0, narrow.c_str(), -1, nullptr, 0 );
	if ( need <= 0 )
	{
		return false;
	}
	std::wstring buffer( static_cast< size_t >( need ), L'\0' );
	::MultiByteToWideChar( CP_UTF8, 0, narrow.c_str(), -1, &buffer[ 0 ], need );
	buffer.resize( static_cast< size_t >( need - 1 ) );
	*out = buffer;
	return true;
}

// Directory of this driver DLL (bin/win64).
bool ModuleDirectory( std::string *outDir )
{
	HMODULE module = nullptr;
	if ( !::GetModuleHandleExW(
			 GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			 reinterpret_cast< LPCWSTR >( &ModuleDirectory ),
			 &module ) )
	{
		return false;
	}
	wchar_t modulePath[ MAX_PATH ] = {};
	const DWORD length = ::GetModuleFileNameW( module, modulePath, MAX_PATH );
	if ( length == 0 || length >= MAX_PATH )
	{
		return false;
	}
	std::wstring full( modulePath );
	const size_t slash = full.find_last_of( L"\\/" );
	if ( slash == std::wstring::npos )
	{
		return false;
	}
	const std::wstring dir = full.substr( 0, slash + 1 );
	char narrow[ MAX_PATH * 2 ] = {};
	const int converted = ::WideCharToMultiByte(
		CP_UTF8, 0, dir.c_str(), -1, narrow, sizeof( narrow ), nullptr, nullptr );
	if ( converted <= 0 )
	{
		return false;
	}
	*outDir = narrow;
	return true;
}

bool FileExists( const std::string &path )
{
	if ( path.empty() )
	{
		return false;
	}
	std::wstring wide;
	if ( !NarrowToWide( path, &wide ) )
	{
		return false;
	}
	const DWORD attrib = ::GetFileAttributesW( wide.c_str() );
	return attrib != INVALID_FILE_ATTRIBUTES && !( attrib & FILE_ATTRIBUTE_DIRECTORY );
}

// LOAD_WITH_ALTERED_SEARCH_PATH for absolute paths so hv.dll next to
// BinocularCameraConfig.dll resolves; bare names use the default search.
HMODULE LoadCandidate( const std::string &candidate )
{
	if ( candidate.empty() )
	{
		return nullptr;
	}
	const bool hasSeparator =
		candidate.find( '\\' ) != std::string::npos || candidate.find( '/' ) != std::string::npos;
	std::wstring wide;
	if ( !NarrowToWide( candidate, &wide ) )
	{
		return nullptr;
	}
	if ( hasSeparator )
	{
		if ( !FileExists( candidate ) )
		{
			return nullptr;
		}
		return ::LoadLibraryExW( wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );
	}
	return ::LoadLibraryW( wide.c_str() );
}

void CleanupVendorLocked()
{
	if ( hDll != nullptr )
	{
		::FreeLibrary( hDll );
		hDll = nullptr;
	}
	pRegister = nullptr;
	pUnregister = nullptr;
	pGetCameraPosition = nullptr;
	pSetDisplayMode = nullptr;
	pGetTrackedState = nullptr;
	pGetScreenSize = nullptr;
	pGetDisplayMode = nullptr;
	pGetError = nullptr;
	registered = false;
	dllLoaded = false;
}

// Try several candidate paths and resolve the five required exports.
bool InitDllLocked()
{
	if ( dllLoaded )
	{
		return true;
	}

	std::string moduleDir;
	ModuleDirectory( &moduleDir );

	std::string candidates[ 8 ];
	int count = 0;
	if ( const char *env = std::getenv( "SHIYAN_BINOCULAR_DLL" ) )
	{
		if ( env[ 0 ] != '\0' )
		{
			candidates[ count++ ] = env;
		}
	}
	if ( !moduleDir.empty() )
	{
		// Copied next to the driver DLL by the CMake POST_BUILD step.
		candidates[ count++ ] = moduleDir + "BinocularCameraConfig.dll";
		// Development layout: <project>\libs\eye_tracking\ (moduleDir = bin\win64\).
		candidates[ count++ ] = moduleDir + "..\\..\\libs\\eye_tracking\\BinocularCameraConfig.dll";
	}
	candidates[ count++ ] = "BinocularCameraConfig.dll"; // PATH / working dir

	for ( int i = 0; i < count; ++i )
	{
		hDll = LoadCandidate( candidates[ i ] );
		if ( hDll != nullptr )
		{
			break;
		}
	}
	if ( hDll == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: BinocularCameraConfig.dll not found (last error=%lu)",
			static_cast< unsigned long >( ::GetLastError() ) );
		return false;
	}

	pRegister = reinterpret_cast< int ( * )( void ) >( ::GetProcAddress( hDll, "Register" ) );
	pUnregister = reinterpret_cast< int ( * )( void ) >( ::GetProcAddress( hDll, "Unregister" ) );
	pGetCameraPosition = reinterpret_cast< camera_position_t ( * )( int ) >(
		::GetProcAddress( hDll, "GetCameraPosition" ) );
	pSetDisplayMode = reinterpret_cast< int ( * )( int ) >( ::GetProcAddress( hDll, "SetDisplayMode" ) );
	pGetTrackedState = reinterpret_cast< int ( * )( void ) >( ::GetProcAddress( hDll, "GetTrackedState" ) );

	if ( pRegister == nullptr || pUnregister == nullptr || pGetCameraPosition == nullptr ||
		 pSetDisplayMode == nullptr || pGetTrackedState == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: failed to resolve eye-tracking SDK exports" );
		CleanupVendorLocked();
		return false;
	}

	pGetScreenSize = reinterpret_cast< int ( * )( float * ) >( ::GetProcAddress( hDll, "GetScreenSize" ) );
	pGetDisplayMode = reinterpret_cast< int ( * )( void ) >( ::GetProcAddress( hDll, "GetDisplayMode" ) );
	pGetError = reinterpret_cast< int ( * )( void ) >( ::GetProcAddress( hDll, "GetError" ) );

	dllLoaded = true;
	return true;
}

int BcRegister()
{
	std::lock_guard< std::mutex > lock( g_vendorMutex );
	if ( !InitDllLocked() || pRegister == nullptr )
	{
		return -1;
	}
	return pRegister();
}

int BcUnregister()
{
	std::lock_guard< std::mutex > lock( g_vendorMutex );
	if ( !dllLoaded || pUnregister == nullptr )
	{
		return -1;
	}
	const int result = pUnregister();
	registered = false;
	return result;
}

camera_position_t BcGetPosition( int filterLevel )
{
	std::lock_guard< std::mutex > lock( g_vendorMutex );
	camera_position_t empty = {};
	if ( !dllLoaded || pGetCameraPosition == nullptr )
	{
		return empty;
	}
	return pGetCameraPosition( filterLevel );
}

int BcSetDisplayMode( int mode )
{
	std::lock_guard< std::mutex > lock( g_vendorMutex );
	if ( !dllLoaded && !InitDllLocked() )
	{
		return -1;
	}
	if ( pSetDisplayMode == nullptr )
	{
		return -1;
	}
	return pSetDisplayMode( mode );
}

int BcGetTrackedState()
{
	std::lock_guard< std::mutex > lock( g_vendorMutex );
	if ( !dllLoaded || pGetTrackedState == nullptr )
	{
		return -1; // EYE_TRACK_UNREGISTERED
	}
	return pGetTrackedState();
}

bool BcGetScreenSizeMm( float *outWhMm, int count )
{
	if ( outWhMm == nullptr || count < 2 )
	{
		return false;
	}
	outWhMm[ 0 ] = 0.0f;
	outWhMm[ 1 ] = 0.0f;
	std::lock_guard< std::mutex > lock( g_vendorMutex );
	if ( !dllLoaded || pGetScreenSize == nullptr )
	{
		return false;
	}
	// The vendor implementation requires a completed Register + live link.
	if ( pGetScreenSize( outWhMm ) < 0 )
	{
		outWhMm[ 0 ] = 0.0f;
		outWhMm[ 1 ] = 0.0f;
		return false;
	}
	return true;
}

// Non-blocking: start the vendor Register on a background thread.
// The thread entry point is the vendor export itself (see g_registerStatus).
void BcBeginRegister()
{
	std::lock_guard< std::mutex > lock( g_vendorMutex );

	const int current = g_registerStatus.load();
	if ( current != kRegisterIdle )
	{
		return;
	}

	if ( !InitDllLocked() || pRegister == nullptr )
	{
		g_registerStatus.store( kRegisterFailed );
		return;
	}

#pragma warning( push )
#pragma warning( disable : 4191 ) // fn pointer -> LPTHREAD_START_ROUTINE (intentional)
	HANDLE thread = ::CreateThread( nullptr,
		0,
		reinterpret_cast< LPTHREAD_START_ROUTINE >( pRegister ),
		nullptr,
		0,
		nullptr );
#pragma warning( pop )
	if ( thread == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: failed to start background eye-tracking Register thread (err=%lu)",
			static_cast< unsigned long >( ::GetLastError() ) );
		g_registerStatus.store( kRegisterFailed );
		return;
	}

	// Deliberately not closed: the exit code is polled during registration and
	// the vendor DLL stays referenced for the lifetime of the process.
	g_registerThread = thread;
	g_registerStatus.store( kRegisterPending );
	DriverLog( "shiyan_nakedmonitor: eye-tracking Register running on a background thread "
			   "(non-blocking, takes ~10s)" );
}

int BcRegisterStatus()
{
	std::lock_guard< std::mutex > lock( g_vendorMutex );

	if ( g_registerStatus.load() != kRegisterPending )
	{
		return g_registerStatus.load();
	}
	if ( g_registerThread == nullptr )
	{
		return kRegisterPending;
	}
	if ( ::WaitForSingleObject( g_registerThread, 0 ) != WAIT_OBJECT_0 )
	{
		return kRegisterPending; // still inside the vendor network round trip
	}

	DWORD exitCode = 0xFFFFFFFFu;
	if ( !::GetExitCodeThread( g_registerThread, &exitCode ) )
	{
		DriverLog( "shiyan_nakedmonitor: GetExitCodeThread failed (err=%lu)",
			static_cast< unsigned long >( ::GetLastError() ) );
	}
	const int result = static_cast< int >( exitCode );

	if ( result >= 0 )
	{
		registered = true;
		g_registerStatus.store( kRegisterOk );
		DriverLog( "shiyan_nakedmonitor: eye-tracking SDK ready (Register=%d)", result );
	}
	else
	{
		// No FreeLibrary here: the DLL is kept referenced for the background
		// thread / process lifetime (loader-lock safety). No retries.
		g_registerStatus.store( kRegisterFailed );
		DriverLog( "shiyan_nakedmonitor: eye-tracking Register failed (%d); using default eye positions",
			result );
	}
	return g_registerStatus.load();
}

//-----------------------------------------------------------------------------
// Eye tracker layer (port of xr_eyetracking_win.cpp)
//-----------------------------------------------------------------------------

// Screen-space convention (vendor + OpenXR runtime): origin at panel centre,
// +x right, +y up, +z toward the viewer. Panel plane at z = 0.
constexpr float kFallbackLeftEye[ 3 ] = { -0.032f, 0.0f, 0.5f };
constexpr float kFallbackRightEye[ 3 ] = { 0.032f, 0.0f, 0.5f };

// Minimum eye-to-panel distance so a silly z never flips the frustum.
constexpr float kMinEyeDistanceZ = 0.05f;

// Plausibility floor for a TRACKED reading (metres): the vendor sometimes
// returns a valid state with a zero eye distance, which is unusable.
constexpr float kMinPlausibleEyeZ = 0.15f;

// Fallback easing time constant: only applies on tracking-failure -> default
// eye position; tracking-follows-directly otherwise.
constexpr float kBlendTimeConstantSeconds = 1.0f;

constexpr float kDefaultScreenWidthMeters = 0.6981f;  // 698.1 mm, 31.5" 4K panel
constexpr float kDefaultScreenHeightMeters = 0.3927f; // 392.7 mm

// Keep the eye inside the panel bounds so angleLeft/angleDown stay negative
// (the "screen corners" constraint documented in the OpenXR runtime Roadmap).
constexpr float kPanelEdgeMargin = 0.002f; // 2 mm

std::mutex g_stateMutex;
bool g_started = false;
bool g_ready = false;          // Register finished OK
bool g_trackingHealthy = false;
bool g_want3D = false;         // panel should be in 3D (SBS) mode
bool g_display3DApplied = false; // SetDisplayMode(3) already issued
float g_eyeL[ 3 ] = { kFallbackLeftEye[ 0 ], kFallbackLeftEye[ 1 ], kFallbackLeftEye[ 2 ] };
float g_eyeR[ 3 ] = { kFallbackRightEye[ 0 ], kFallbackRightEye[ 1 ], kFallbackRightEye[ 2 ] };
float g_screenW = kDefaultScreenWidthMeters;
float g_screenH = kDefaultScreenHeightMeters;
bool g_screenFromSdk = false;
std::chrono::steady_clock::time_point g_lastPumpTime;
bool g_lastPumpValid = false;

// Issue SetDisplayMode(3) once the panel is wanted in 3D AND the vendor
// Register has finished (never touch other exports while Register is pending -
// it may hold their internal locks).
void Apply3DModeIfReadyLocked()
{
	// NOTE: called without g_stateMutex held (it takes g_vendorMutex via Bc*).
	bool want = false;
	bool applied = false;
	{
		std::lock_guard< std::mutex > lock( g_stateMutex );
		want = g_want3D;
		applied = g_display3DApplied;
	}
	if ( !want || applied || BcRegisterStatus() != kRegisterOk )
	{
		return;
	}
	if ( BcSetDisplayMode( 3 /* DISPLAY_MODE_3D (SBS) */ ) >= 0 )
	{
		std::lock_guard< std::mutex > lock( g_stateMutex );
		g_display3DApplied = true;
		DriverLog( "shiyan_nakedmonitor: panel switched to 3D (SBS) mode (eye positions only "
				   "flow in this mode)" );
	}
	else
	{
		DriverLog( "shiyan_nakedmonitor: SetDisplayMode(3) failed; eye tracking may stay idle" );
	}
}

} // namespace

namespace EyeTracker
{

void SetDefaultScreenSize( float widthMeters, float heightMeters )
{
	std::lock_guard< std::mutex > lock( g_stateMutex );
	if ( g_screenFromSdk )
	{
		return; // hardware fact already applied; settings only seed the default
	}
	if ( widthMeters > 0.0f )
	{
		g_screenW = widthMeters;
	}
	if ( heightMeters > 0.0f )
	{
		g_screenH = heightMeters;
	}
}

void Start()
{
	{
		std::lock_guard< std::mutex > lock( g_stateMutex );
		if ( g_started )
		{
			return;
		}
		g_started = true;
	}
	BcBeginRegister(); // non-blocking
}

bool WaitForRegister( double timeoutSeconds )
{
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds( static_cast< int64_t >( timeoutSeconds * 1000.0 ) );
	for ( ;; )
	{
		const int status = BcRegisterStatus();
		if ( status == kRegisterOk )
		{
			return true;
		}
		if ( status == kRegisterFailed )
		{
			return false;
		}
		if ( std::chrono::steady_clock::now() >= deadline )
		{
			return false;
		}
		::Sleep( 50 );
	}
}

void Enter3DMode()
{
	{
		std::lock_guard< std::mutex > lock( g_stateMutex );
		g_want3D = true;
	}
	Apply3DModeIfReadyLocked();
}

void Pump()
{
	{
		std::lock_guard< std::mutex > lock( g_stateMutex );
		if ( !g_started )
		{
			return;
		}
	}

	const int status = BcRegisterStatus();
	if ( status != kRegisterOk )
	{
		// IDLE / PENDING / FAILED: never touch the other vendor exports
		// (the background Register may hold their internal locks).
		return;
	}
	{
		std::lock_guard< std::mutex > lock( g_stateMutex );
		if ( !g_ready )
		{
			g_ready = true;
			DriverLog( "shiyan_nakedmonitor: binocular camera ready (background Register finished)" );
		}
	}

	// Late-registration path: apply a requested 3D switch once Register is done
	// (the pre-device-add path usually already did it).
	Apply3DModeIfReadyLocked();

	// Panel physical size: fetch once the SDK link is up; retry every frame
	// until it answers (same as the OpenXR runtime's PumpInit).
	{
		bool needScreen = false;
		{
			std::lock_guard< std::mutex > lock( g_stateMutex );
			needScreen = !g_screenFromSdk;
		}
		if ( needScreen )
		{
			float wh[ 2 ] = { 0.0f, 0.0f };
			if ( BcGetScreenSizeMm( wh, 2 ) && wh[ 0 ] > 0.0f && wh[ 1 ] > 0.0f )
			{
				std::lock_guard< std::mutex > lock( g_stateMutex );
				if ( !g_screenFromSdk )
				{
					g_screenW = wh[ 0 ] * 0.001f;
					g_screenH = wh[ 1 ] * 0.001f;
					g_screenFromSdk = true;
					DriverLog( "shiyan_nakedmonitor: panel size from eye-tracking SDK = %.1f x %.1f mm "
							   "(overrides settings default %.4f x %.4f m)",
						wh[ 0 ], wh[ 1 ], kDefaultScreenWidthMeters, kDefaultScreenHeightMeters );
				}
			}
		}
	}

	// Gate: only TRACKED frames use live eye positions; anything else eases
	// back to the default eye position (port of XrEyetracking::getEyePos).
	const int state = BcGetTrackedState();
	bool valid = ( state == 0 /* EYE_TRACK_TRACKED */ );
	bool implausible = false;

	float targetL[ 3 ] = { kFallbackLeftEye[ 0 ], kFallbackLeftEye[ 1 ], kFallbackLeftEye[ 2 ] };
	float targetR[ 3 ] = { kFallbackRightEye[ 0 ], kFallbackRightEye[ 1 ], kFallbackRightEye[ 2 ] };
	if ( valid )
	{
		const camera_position_t pos = BcGetPosition( 0 ); // 0 = raw, like Monado
		targetL[ 0 ] = pos.lCamx * 0.001f;
		targetL[ 1 ] = pos.lCamy * 0.001f;
		targetL[ 2 ] = pos.lCamz * 0.001f;
		targetR[ 0 ] = pos.rCamx * 0.001f;
		targetR[ 1 ] = pos.rCamy * 0.001f;
		targetR[ 2 ] = pos.rCamz * 0.001f;

		// The vendor occasionally reports TRACKED with a zero (or absurd) eye
		// distance. Accepting that collapses the eyes onto the panel plane:
		// the off-axis frustum blows up and the natural_mouse aim ray becomes
		// degenerate (it is bounded by the panel seen from the eye). Treat it
		// as a dropout and ease to the fallback instead.
		const bool finite = std::isfinite( targetL[ 2 ] ) && std::isfinite( targetR[ 2 ] );
		if ( !finite || targetL[ 2 ] < kMinPlausibleEyeZ || targetR[ 2 ] < kMinPlausibleEyeZ )
		{
			valid = false;
			implausible = true;
		}
	}

	const auto now = std::chrono::steady_clock::now();
	float dt = 0.0f;
	if ( g_lastPumpValid )
	{
		dt = std::min(
			std::chrono::duration_cast< std::chrono::nanoseconds >( now - g_lastPumpTime ).count() * 1e-9f,
			0.1f );
	}
	g_lastPumpTime = now;
	g_lastPumpValid = true;

	std::lock_guard< std::mutex > lock( g_stateMutex );
	if ( valid )
	{
		// Healthy: follow the tracker directly - blending here would only add
		// latency to the normal path.
		for ( int i = 0; i < 3; ++i )
		{
			g_eyeL[ i ] = targetL[ i ];
			g_eyeR[ i ] = targetR[ i ];
		}
	}
	else
	{
		// Ease only toward the fallback so a tracking dropout never jumps.
		const float alpha = 1.0f - std::exp( -dt / kBlendTimeConstantSeconds );
		for ( int i = 0; i < 3; ++i )
		{
			g_eyeL[ i ] += ( targetL[ i ] - g_eyeL[ i ] ) * alpha;
			g_eyeR[ i ] += ( targetR[ i ] - g_eyeR[ i ] ) * alpha;
		}
	}

	if ( g_trackingHealthy != valid )
	{
		g_trackingHealthy = valid;
		if ( valid )
		{
			DriverLog( "shiyan_nakedmonitor: eye tracking ACTIVE (eyeL=(%+.1f, %+.1f, %.0f) mm)",
				targetL[ 0 ] * 1000.0f, targetL[ 1 ] * 1000.0f, targetL[ 2 ] * 1000.0f );
		}
		else if ( implausible )
		{
			DriverLog( "shiyan_nakedmonitor: eye tracking data invalid (TRACKED but z=(%.0f, %.0f) mm); "
					   "easing to default eye positions",
				targetL[ 2 ] * 1000.0f, targetR[ 2 ] * 1000.0f );
		}
		else
		{
			DriverLog( "shiyan_nakedmonitor: eye tracking lost (state=%d); easing to default eye positions",
				state );
		}
	}
}

void GetCurrentEyes( float outLeft[ 3 ], float outRight[ 3 ] )
{
	std::lock_guard< std::mutex > lock( g_stateMutex );

	const float halfW = g_screenW * 0.5f - kPanelEdgeMargin;
	const float halfH = g_screenH * 0.5f - kPanelEdgeMargin;

	for ( int eye = 0; eye < 2; ++eye )
	{
		const float *src = ( eye == 0 ) ? g_eyeL : g_eyeR;
		float *dst = ( eye == 0 ) ? outLeft : outRight;
		dst[ 0 ] = std::max( -halfW, std::min( halfW, src[ 0 ] ) );
		dst[ 1 ] = std::max( -halfH, std::min( halfH, src[ 1 ] ) );
		dst[ 2 ] = std::max( kMinEyeDistanceZ, src[ 2 ] );
	}
}

void GetScreenSize( float *outWidthMeters, float *outHeightMeters )
{
	std::lock_guard< std::mutex > lock( g_stateMutex );
	if ( outWidthMeters != nullptr )
	{
		*outWidthMeters = g_screenW;
	}
	if ( outHeightMeters != nullptr )
	{
		*outHeightMeters = g_screenH;
	}
}

bool IsEnabled()
{
	std::lock_guard< std::mutex > lock( g_stateMutex );
	return g_started;
}

bool IsReady()
{
	std::lock_guard< std::mutex > lock( g_stateMutex );
	return g_ready;
}

bool IsTracking()
{
	std::lock_guard< std::mutex > lock( g_stateMutex );
	return g_trackingHealthy;
}

void Shutdown()
{
	{
		std::lock_guard< std::mutex > lock( g_stateMutex );
		g_want3D = false;
	}

	const int status = BcRegisterStatus();
	if ( status == kRegisterPending )
	{
		// Background Register still in flight: leave it alone (its thread entry
		// is the vendor export and finishes cleanly without us).
		DriverLog( "shiyan_nakedmonitor: background eye-tracking Register still in flight at "
				   "shutdown; leaving it to finish" );
		return;
	}

	// Mirror of shutdown_for_dll_unload in the reference project: drop the panel
	// back to 2D first, then Unregister.
	bool was3D = false;
	{
		std::lock_guard< std::mutex > lock( g_stateMutex );
		was3D = g_display3DApplied;
		g_display3DApplied = false;
	}
	if ( was3D )
	{
		BcSetDisplayMode( 2 /* DISPLAY_MODE_2D */ );
	}
	if ( status == kRegisterOk )
	{
		BcUnregister();
	}
}

} // namespace EyeTracker
