//============ Shiyan Naked Monitor OpenVR driver ============
#include "naked_hmd_device_driver.h"

#include "driverlog.h"

#include <cmath>
#include <cstring>

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

	if ( config_.auto_place_window )
	{
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

	half_tan_h_ = HalfTanFromDegrees( config_.fov_horizontal_deg );

	if ( config_.fov_vertical_deg > 0.0f )
	{
		half_tan_v_ = HalfTanFromDegrees( config_.fov_vertical_deg );
	}
	else
	{
		// Derive the vertical FOV from the horizontal FOV and the per-eye aspect
		// ratio so the vertical scale is geometrically correct for a flat panel.
		const float eye_width = static_cast< float >( config_.window_width ) * 0.5f;
		const float eye_height = static_cast< float >( config_.window_height );
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
	const uint32_t half_width = static_cast< uint32_t >( config_.window_width ) / 2;

	*pnY = 0;
	*pnWidth = half_width;
	*pnHeight = static_cast< uint32_t >( config_.window_height );

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
	*pnWidth = static_cast< uint32_t >( config_.window_width );
	*pnHeight = static_cast< uint32_t >( config_.window_height );
}

bool NakedDisplayComponent::ComputeInverseDistortion( vr::HmdVector2_t *pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV )
{
	// Let SteamVR infer an estimate from ComputeDistortion (which is identity here).
	return false;
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

void NakedHMDDeviceDriver::EnterStandby()
{
	DriverLog( "shiyan_nakedmonitor: entering standby." );
}

void NakedHMDDeviceDriver::Deactivate()
{
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}
