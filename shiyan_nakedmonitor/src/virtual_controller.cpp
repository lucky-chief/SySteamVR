//============ Shiyan Naked Monitor OpenVR driver ============
#include "virtual_controller.h"

#include "driverlog.h"
#include "eye_tracker.h"
#include "mouse_ray.h"
#include "virtual_head.h"

#include <algorithm>
#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Settings sections.
static const char *kControllerSection = "shiyan_nakedmonitor_controller";
static const char *kDisplaySectionForController = "shiyan_nakedmonitor_display";

// Key bindings (ported from SyOpenXRRuntime's vr_sim / natural_mouse):
//   LMB -> trigger          RMB -> grip                M -> application_menu
//   Z/X -> a/b              MMB (held) -> head look    WASD/QE -> head walk
//   arrows -> joystick x/y  wheel -> joystick y pulse  G -> toggle mode
//   Shift -> walk 3x
static constexpr float kWheelStickPulseSeconds = 0.15f;
static constexpr float kMaxPitchRadians = 1.5f; // ~86 degrees (fallback aim)

static float GetFloatSetting( const char *pchSection, const char *pchKey, float flDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const float value = vr::VRSettings()->GetFloat( pchSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : flDefault;
}

static bool GetBoolSetting( const char *pchSection, const char *pchKey, bool bDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const bool value = vr::VRSettings()->GetBool( pchSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : bDefault;
}

static void GetStringSetting( const char *pchSection, const char *pchKey, const char *pchDefault, std::string &out )
{
	char buffer[ 1024 ] = {};
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	vr::VRSettings()->GetString( pchSection, pchKey, buffer, sizeof( buffer ), &error );
	out = ( error == vr::VRSettingsError_None ) ? buffer : pchDefault;
}

ShiyanNakedController::ShiyanNakedController()
{
	GetStringSetting( kControllerSection, "serial_number", "ShiyanNakedController-0001", serial_number_ );
	GetStringSetting( kControllerSection, "model_number", "Shiyan Virtual Controller", model_number_ );

	offset_x_ = GetFloatSetting( kControllerSection, "offset_x", offset_x_ );
	offset_y_ = GetFloatSetting( kControllerSection, "offset_y", offset_y_ );
	offset_z_ = GetFloatSetting( kControllerSection, "offset_z", offset_z_ );
	mouse_sensitivity_ = GetFloatSetting( kControllerSection, "mouse_sensitivity", mouse_sensitivity_ );
	wheel_invert_ = GetBoolSetting( kControllerSection, "wheel_invert", wheel_invert_ );

	screen_center_height_m_ = GetFloatSetting( kDisplaySectionForController, "screen_center_height_m", screen_center_height_m_ );
	screen_distance_m_ = GetFloatSetting( kDisplaySectionForController, "screen_distance_m", screen_distance_m_ );

	DriverLog( "shiyan_nakedmonitor: virtual controller '%s' (offset %.2f, %.2f, %.2f; sensitivity %.5f).",
		serial_number_.c_str(), offset_x_, offset_y_, offset_z_, mouse_sensitivity_ );
}

vr::EVRInitError ShiyanNakedController::Activate( uint32_t unObjectId )
{
	device_index_ = unObjectId;
	active_ = true;

	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer( device_index_ );

	vr::VRProperties()->SetStringProperty( container, vr::Prop_ModelNumber_String, model_number_.c_str() );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ManufacturerName_String, "Shiyan" );
	vr::VRProperties()->SetInt32Property( container, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_RightHand );

	// Input profile (declares the components below for the bindings UI) and
	// controller type used by Automatic Rebinding (resources/input/*.json).
	vr::VRProperties()->SetStringProperty( container, vr::Prop_InputProfilePath_String,
		"{shiyan_nakedmonitor}/input/shiyan_controller_profile.json" );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ControllerType_String, "shiyan_controller" );

	auto createBoolean = [container]( const char *pchPath, vr::VRInputComponentHandle_t &handle ) -> bool {
		return vr::VRDriverInput()->CreateBooleanComponent( container, pchPath, &handle ) == vr::VRInputError_None;
	};

	bool ok = true;
	ok &= vr::VRDriverInput()->CreateScalarComponent( container, "/input/trigger/value",
			   &components_[ Comp_TriggerValue ], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided ) == vr::VRInputError_None;
	ok &= createBoolean( "/input/trigger/click", components_[ Comp_TriggerClick ] );
	ok &= vr::VRDriverInput()->CreateScalarComponent( container, "/input/grip/value",
			   &components_[ Comp_GripValue ], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided ) == vr::VRInputError_None;
	ok &= createBoolean( "/input/grip/click", components_[ Comp_GripClick ] );
	ok &= createBoolean( "/input/a/click", components_[ Comp_AClick ] );
	ok &= createBoolean( "/input/b/click", components_[ Comp_BClick ] );
	ok &= createBoolean( "/input/application_menu/click", components_[ Comp_AppMenuClick ] );
	ok &= vr::VRDriverInput()->CreateScalarComponent( container, "/input/joystick/x",
			   &components_[ Comp_JoystickX ], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided ) == vr::VRInputError_None;
	ok &= vr::VRDriverInput()->CreateScalarComponent( container, "/input/joystick/y",
			   &components_[ Comp_JoystickY ], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided ) == vr::VRInputError_None;
	ok &= vr::VRDriverInput()->CreateHapticComponent( container, "/output/haptic", &components_[ Comp_Haptic ] ) == vr::VRInputError_None;

	if ( !ok )
	{
		DriverLog( "shiyan_nakedmonitor: WARNING - failed to create one or more controller input components." );
	}

	DriverLog( "shiyan_nakedmonitor: virtual controller activated (index %u, role right hand).", device_index_ );
	return vr::VRInitError_None;
}

void *ShiyanNakedController::GetComponent( const char *pchComponentNameAndVersion )
{
	return nullptr; // controller: no display/direct-mode components
}

void ShiyanNakedController::DebugRequest( char const *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize )
{
	if ( unResponseBufferSize >= 1 )
	{
		pchResponseBuffer[ 0 ] = 0;
	}
}

vr::DriverPose_t ShiyanNakedController::GetPose()
{
	vr::DriverPose_t pose = {};
	pose.qWorldFromDriverRotation.w = 1.0;
	pose.qDriverFromHeadRotation.w = 1.0;
	pose.deviceIsConnected = true;
	pose.poseIsValid = false;
	pose.result = vr::TrackingResult_Uninitialized;
	return pose;
}

void ShiyanNakedController::Deactivate()
{
	active_ = false;
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}

void ShiyanNakedController::EnterStandby()
{
}

//-----------------------------------------------------------------------------
// Input (key table ported from the OpenXR runtime simulator)
//-----------------------------------------------------------------------------
void ShiyanNakedController::ApplyInput( const RawInput::State &input )
{
	// --- vr_sim fallback aim (mouse deltas steer yaw/pitch) ----------------
	// In natural_mouse mode the deltas normally belong to the pointer/head
	// (VirtualHead::Update already zeroes the ones it consumed); the fallback
	// only integrates whatever is left, e.g. when the headset window is not
	// found and the ray cannot be computed.
	const bool naturalMouse = VirtualHead::IsNaturalMouse();
	if ( !naturalMouse )
	{
		// Tracking space: -Z forward, +Y up, positive yaw = turn left,
		// positive pitch = look up.
		aim_yaw_ -= static_cast< float >( input.dx ) * mouse_sensitivity_;
		aim_pitch_ -= static_cast< float >( input.dy ) * mouse_sensitivity_;
		aim_pitch_ = std::max( -kMaxPitchRadians, std::min( kMaxPitchRadians, aim_pitch_ ) );
		while ( aim_yaw_ > 3.14159265f )
		{
			aim_yaw_ -= 2.0f * 3.14159265f;
		}
		while ( aim_yaw_ < -3.14159265f )
		{
			aim_yaw_ += 2.0f * 3.14159265f;
		}
	}

	// --- Trigger / grip ----------------------------------------------------
	const float trigger_value = input.left ? 1.0f : 0.0f;

	// --- Buttons -----------------------------------------------------------
	const bool menu_pressed = input.keys[ 'M' ];
	const bool a_pressed = input.keys[ 'Z' ]; // VK_Z
	const bool b_pressed = input.keys[ 'X' ]; // VK_X

	// --- Thumbstick: arrow keys + wheel pulse on Y ------------------------
	float joy_x = ( input.keys[ VK_RIGHT ] ? 1.0f : 0.0f ) - ( input.keys[ VK_LEFT ] ? 1.0f : 0.0f );
	float joy_y = ( input.keys[ VK_UP ] ? 1.0f : 0.0f ) - ( input.keys[ VK_DOWN ] ? 1.0f : 0.0f );
	if ( input.wheel != 0 )
	{
		wheel_pulse_seconds_ = kWheelStickPulseSeconds;
		wheel_pulse_sign_ = ( static_cast< float >( input.wheel ) > 0.0f ? 1.0f : -1.0f )
			* ( wheel_invert_ ? -1.0f : 1.0f );
	}
	if ( joy_y == 0.0f && wheel_pulse_seconds_ > 0.0f )
	{
		joy_y = wheel_pulse_sign_;
	}

	vr::VRDriverInput()->UpdateScalarComponent( components_[ Comp_TriggerValue ], trigger_value, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_TriggerClick ], input.left, 0.0 );
	vr::VRDriverInput()->UpdateScalarComponent( components_[ Comp_GripValue ], input.right ? 1.0f : 0.0f, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_GripClick ], input.right, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_AClick ], a_pressed, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_BClick ], b_pressed, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_AppMenuClick ], menu_pressed, 0.0 );
	vr::VRDriverInput()->UpdateScalarComponent( components_[ Comp_JoystickX ], joy_x, 0.0 );
	vr::VRDriverInput()->UpdateScalarComponent( components_[ Comp_JoystickY ], joy_y, 0.0 );
}

void ShiyanNakedController::PushPose()
{
	// --- Centre eye (screen space -> tracking space) ------------------------
	float eyeLeft[ 3 ] = {};
	float eyeRight[ 3 ] = {};
	EyeTracker::GetCurrentEyes( eyeLeft, eyeRight );
	const float midX = ( eyeLeft[ 0 ] + eyeRight[ 0 ] ) * 0.5f;
	const float midY = ( eyeLeft[ 1 ] + eyeRight[ 1 ] ) * 0.5f;
	const float midZ = ( eyeLeft[ 2 ] + eyeRight[ 2 ] ) * 0.5f;
	float walkOffset[ 3 ] = {};
	VirtualHead::GetPositionOffset( walkOffset );

	vr::DriverPose_t pose = {};
	pose.qWorldFromDriverRotation.w = 1.0;
	pose.qDriverFromHeadRotation.w = 1.0;

	// natural_mouse: aim pose = centre eye -> cursor point on the panel
	// (WYSIWYG pointer ray). Falls back to the vr_sim-style aim below when the
	// headset window / cursor is unavailable.
	float rayPos[ 3 ] = {};
	float rayQuat[ 4 ] = {};
	if ( VirtualHead::IsNaturalMouse() && MouseRay::ComputeAimPose( rayPos, rayQuat ) )
	{
		pose.qRotation.w = rayQuat[ 3 ];
		pose.qRotation.x = rayQuat[ 0 ];
		pose.qRotation.y = rayQuat[ 1 ];
		pose.qRotation.z = rayQuat[ 2 ];
		pose.vecPosition[ 0 ] = rayPos[ 0 ];
		pose.vecPosition[ 1 ] = rayPos[ 1 ];
		pose.vecPosition[ 2 ] = rayPos[ 2 ];
	}
	else
	{
		// Fallback (vr_sim aim / no ray): orientation from the accumulated
		// mouse-delta angles, position = centre eye + offset + walk.
		const float halfYaw = aim_yaw_ * 0.5f;
		const float halfPitch = aim_pitch_ * 0.5f;
		const float cy = std::cos( halfYaw );
		const float sy = std::sin( halfYaw );
		const float cp = std::cos( halfPitch );
		const float sp = std::sin( halfPitch );
		pose.qRotation.w = cy * cp;
		pose.qRotation.x = cy * sp;
		pose.qRotation.y = cp * sy;
		pose.qRotation.z = -sy * sp;

		pose.vecPosition[ 0 ] = midX + offset_x_ + walkOffset[ 0 ];
		pose.vecPosition[ 1 ] = midY + screen_center_height_m_ + offset_y_ + walkOffset[ 1 ];
		pose.vecPosition[ 2 ] = midZ - screen_distance_m_ + offset_z_ + walkOffset[ 2 ];
	}

	pose.poseIsValid = true;
	pose.deviceIsConnected = true;
	pose.result = vr::TrackingResult_Running_OK;
	pose.shouldApplyHeadModel = false;

	vr::VRServerDriverHost()->TrackedDevicePoseUpdated( device_index_, pose, sizeof( vr::DriverPose_t ) );
}

void ShiyanNakedController::RunFrame( const RawInput::State &input )
{
	if ( !active_ || device_index_ == vr::k_unTrackedDeviceIndexInvalid )
	{
		return;
	}

	// Frame delta for the wheel pulse decay.
	const auto now = std::chrono::steady_clock::now();
	if ( last_runframe_time_ != std::chrono::steady_clock::time_point{} )
	{
		const float dt = std::chrono::duration_cast< std::chrono::duration< float > >( now - last_runframe_time_ ).count();
		wheel_pulse_seconds_ = std::max( 0.0f, wheel_pulse_seconds_ - std::min( dt, 0.1f ) );
	}
	last_runframe_time_ = now;

	ApplyInput( input );
	PushPose();
}

void ShiyanNakedController::ProcessEvent( const vr::VREvent_t &event )
{
	switch ( event.eventType )
	{
	case vr::VREvent_Input_HapticVibration:
		if ( event.data.hapticVibration.componentHandle == components_[ Comp_Haptic ] )
		{
			// Rate-limited: apps can pulse every frame.
			const auto now = std::chrono::steady_clock::now();
			if ( last_haptic_log_.time_since_epoch().count() == 0
				 || now - last_haptic_log_ >= std::chrono::seconds( 1 ) )
			{
				last_haptic_log_ = now;
				DriverLog( "shiyan_nakedmonitor: haptic pulse (duration %.3fs, freq %.1fHz, amp %.2f).",
					event.data.hapticVibration.fDurationSeconds,
					event.data.hapticVibration.fFrequency,
					event.data.hapticVibration.fAmplitude );
			}
		}
		break;
	default:
		break;
	}
}
