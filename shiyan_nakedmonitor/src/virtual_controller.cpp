//============ Shiyan Naked Monitor OpenVR driver ============
#include "virtual_controller.h"

#include "driverlog.h"
#include "gamepad_input.h"
#include "vr_sim.h"

#include <cstdio>

static const char *kControllerSection = "shiyan_nakedmonitor_controller";

static void GetStringSetting( const char *pchKey, const char *pchDefault, std::string &out )
{
	char buffer[ 1024 ] = {};
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	vr::VRSettings()->GetString( kControllerSection, pchKey, buffer, sizeof( buffer ), &error );
	out = ( error == vr::VRSettingsError_None ) ? buffer : pchDefault;
}

ShiyanNakedController::ShiyanNakedController( int hand )
	: hand_( ( hand == 0 ) ? 0 : 1 )
{
	const bool left = ( hand_ == 0 );
	char key[ 64 ] = {};

	std::snprintf( key, sizeof( key ), "serial_number_%s", left ? "left" : "right" );
	GetStringSetting( key, left ? "ShiyanNakedController-L" : "ShiyanNakedController-R", serial_number_ );

	std::snprintf( key, sizeof( key ), "model_number_%s", left ? "left" : "right" );
	GetStringSetting( key, left ? "Shiyan Virtual Controller (L)" : "Shiyan Virtual Controller (R)", model_number_ );

	DriverLog( "shiyan_nakedmonitor: virtual %s controller '%s'.",
		left ? "left" : "right", serial_number_.c_str() );
}

vr::EVRInitError ShiyanNakedController::Activate( uint32_t unObjectId )
{
	device_index_ = unObjectId;
	active_ = true;

	const bool left = ( hand_ == 0 );

	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer( device_index_ );

	vr::VRProperties()->SetStringProperty( container, vr::Prop_ModelNumber_String, model_number_.c_str() );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ManufacturerName_String, "Shiyan" );
	vr::VRProperties()->SetInt32Property( container, vr::Prop_ControllerRoleHint_Int32,
		left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand );

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
	// Left hand uses x/y, right hand uses a/b (same face primary/secondary slots).
	ok &= createBoolean( left ? "/input/x/click" : "/input/a/click", components_[ Comp_FacePrimary ] );
	ok &= createBoolean( left ? "/input/y/click" : "/input/b/click", components_[ Comp_FaceSecondary ] );
	ok &= createBoolean( "/input/application_menu/click", components_[ Comp_AppMenuClick ] );
	// System button: SteamVR opens/toggles the dashboard from this (the gamepad
	// Start button is routed here, so it does something in every app).
	ok &= createBoolean( "/input/system/click", components_[ Comp_SystemClick ] );
	ok &= vr::VRDriverInput()->CreateScalarComponent( container, "/input/joystick/x",
			   &components_[ Comp_JoystickX ], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided ) == vr::VRInputError_None;
	ok &= vr::VRDriverInput()->CreateScalarComponent( container, "/input/joystick/y",
			   &components_[ Comp_JoystickY ], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided ) == vr::VRInputError_None;
	ok &= createBoolean( "/input/joystick/click", components_[ Comp_JoystickClick ] );
	ok &= vr::VRDriverInput()->CreateHapticComponent( container, "/output/haptic", &components_[ Comp_Haptic ] ) == vr::VRInputError_None;

	if ( !ok )
	{
		DriverLog( "shiyan_nakedmonitor: WARNING - failed to create one or more %s controller input components.",
			left ? "left" : "right" );
	}

	DriverLog( "shiyan_nakedmonitor: virtual %s controller activated (index %u).",
		left ? "left" : "right", device_index_ );
	return vr::VRInitError_None;
}

void *ShiyanNakedController::GetComponent( const char *pchComponentNameAndVersion )
{
	(void)pchComponentNameAndVersion;
	return nullptr; // controller: no display/direct-mode components
}

void ShiyanNakedController::DebugRequest( char const *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize )
{
	(void)pchRequest;
	if ( unResponseBufferSize >= 1 )
	{
		pchResponseBuffer[ 0 ] = 0;
	}
}

void ShiyanNakedController::Deactivate()
{
	active_ = false;
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}

void ShiyanNakedController::EnterStandby()
{
}

vr::DriverPose_t ShiyanNakedController::GetPose()
{
	vr::DriverPose_t pose = {};

	// These must be valid quaternions or the device will not appear.
	pose.qWorldFromDriverRotation.w = 1.0;
	pose.qDriverFromHeadRotation.w = 1.0;

	float position[ 3 ] = {};
	float orientation[ 4 ] = { 0.0f, 0.0f, 0.0f, 1.0f };
	VrSim::GetControllerPose( hand_, position, orientation );

	pose.vecPosition[ 0 ] = position[ 0 ];
	pose.vecPosition[ 1 ] = position[ 1 ];
	pose.vecPosition[ 2 ] = position[ 2 ];
	pose.qRotation.x = orientation[ 0 ];
	pose.qRotation.y = orientation[ 1 ];
	pose.qRotation.z = orientation[ 2 ];
	pose.qRotation.w = orientation[ 3 ];

	pose.poseIsValid = true;
	pose.deviceIsConnected = true;
	pose.result = vr::TrackingResult_Running_OK;
	pose.shouldApplyHeadModel = false;

	return pose;
}

void ShiyanNakedController::PushPose()
{
	vr::VRServerDriverHost()->TrackedDevicePoseUpdated( device_index_, GetPose(), sizeof( vr::DriverPose_t ) );
}

void ShiyanNakedController::ApplyInput()
{
	VrSim::HandInput input;
	VrSim::GetHandInput( hand_, &input );

	vr::VRDriverInput()->UpdateScalarComponent( components_[ Comp_TriggerValue ], input.trigger, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_TriggerClick ], input.triggerClick, 0.0 );
	vr::VRDriverInput()->UpdateScalarComponent( components_[ Comp_GripValue ], input.grip, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_GripClick ], input.gripClick, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_FacePrimary ], input.facePrimary, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_FaceSecondary ], input.faceSecondary, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_AppMenuClick ], input.menu, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_SystemClick ], input.system, 0.0 );
	vr::VRDriverInput()->UpdateScalarComponent( components_[ Comp_JoystickX ], input.thumbstickX, 0.0 );
	vr::VRDriverInput()->UpdateScalarComponent( components_[ Comp_JoystickY ], input.thumbstickY, 0.0 );
	vr::VRDriverInput()->UpdateBooleanComponent( components_[ Comp_JoystickClick ], input.thumbstickClick, 0.0 );
}

void ShiyanNakedController::RunFrame()
{
	if ( !active_ || device_index_ == vr::k_unTrackedDeviceIndexInvalid )
	{
		return;
	}

	ApplyInput();
	PushPose();
}

void ShiyanNakedController::ProcessEvent( const vr::VREvent_t &event )
{
	switch ( event.eventType )
	{
	case vr::VREvent_Input_HapticVibration:
		if ( event.data.hapticVibration.componentHandle == components_[ Comp_Haptic ] )
		{
			// The physical gamepad rumbles instead of the (virtual) controller.
			GamepadInput::Rumble( event.data.hapticVibration.fAmplitude,
				event.data.hapticVibration.fDurationSeconds );

			// Rate-limited: apps can pulse every frame.
			const auto now = std::chrono::steady_clock::now();
			if ( last_haptic_log_.time_since_epoch().count() == 0
				 || now - last_haptic_log_ >= std::chrono::seconds( 1 ) )
			{
				last_haptic_log_ = now;
				DriverLog( "shiyan_nakedmonitor: %s haptic pulse (duration %.3fs, freq %.1fHz, amp %.2f).",
					hand_ == 0 ? "left" : "right",
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
