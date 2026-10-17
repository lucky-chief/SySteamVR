//============ Shiyan Naked Monitor OpenVR driver ============
#include "device_provider.h"

#include "driverlog.h"
#include "eye_tracker.h"
#include "ime_switch.h"
#include "raw_input.h"
#include "vr_sim.h"

static bool GetBoolSetting( const char *pchSection, const char *pchKey, bool bDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const bool value = vr::VRSettings()->GetBool( pchSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : bDefault;
}

vr::EVRInitError NakedDeviceProvider::Init( vr::IVRDriverContext *pDriverContext )
{
	// Initialise the server driver context so we can call into the runtime.
	VR_INIT_SERVER_DRIVER_CONTEXT( pDriverContext );

	// Register the eye tracker and put the panel into 3D (SBS) mode BEFORE the
	// HMD is added: vrserver samples window bounds / viewports exactly once at
	// add time and they must already reflect the 3D-mode resolution.
	PreparePanelBeforeDeviceAdd();

	hmd_device_ = std::make_unique< NakedHMDDeviceDriver >();

	if ( !vr::VRServerDriverHost()->TrackedDeviceAdded(
			hmd_device_->GetSerialNumber().c_str(), vr::TrackedDeviceClass_HMD, hmd_device_.get() ) )
	{
		DriverLog( "shiyan_nakedmonitor: failed to add HMD device." );
		return vr::VRInitError_Driver_Unknown;
	}

	DriverLog( "shiyan_nakedmonitor: HMD device added." );

	// The input simulator places the hands relative to the seated base height.
	VrSim::SetHeadBaseHeight( hmd_device_->GetSeatedHeightMeters() );
	VrSim::Init();

	// --- Virtual keyboard/mouse controllers (left + right hand) -----------
	if ( GetBoolSetting( "shiyan_nakedmonitor_controller", "enabled", true ) )
	{
		raw_input_started_ = RawInput::Start();
		if ( !raw_input_started_ )
		{
			DriverLog( "shiyan_nakedmonitor: WARNING - raw input capture failed to start; "
					   "controllers will idle with no key/mouse data." );
		}

		controller_left_ = std::make_unique< ShiyanNakedController >( 0 );
		if ( !vr::VRServerDriverHost()->TrackedDeviceAdded(
				controller_left_->GetSerialNumber().c_str(), vr::TrackedDeviceClass_Controller, controller_left_.get() ) )
		{
			DriverLog( "shiyan_nakedmonitor: failed to add left controller device." );
			controller_left_ = nullptr;
		}
		else
		{
			DriverLog( "shiyan_nakedmonitor: left controller device added." );
		}

		controller_right_ = std::make_unique< ShiyanNakedController >( 1 );
		if ( !vr::VRServerDriverHost()->TrackedDeviceAdded(
				controller_right_->GetSerialNumber().c_str(), vr::TrackedDeviceClass_Controller, controller_right_.get() ) )
		{
			DriverLog( "shiyan_nakedmonitor: failed to add right controller device." );
			controller_right_ = nullptr;
		}
		else
		{
			DriverLog( "shiyan_nakedmonitor: right controller device added." );
		}
	}

	return vr::VRInitError_None;
}

const char *const *NakedDeviceProvider::GetInterfaceVersions()
{
	return vr::k_InterfaceVersions;
}

bool NakedDeviceProvider::ShouldBlockStandbyMode()
{
	return false;
}

void NakedDeviceProvider::RunFrame()
{
	// Advance the eye tracker: settles the background Register, applies the
	// SDK panel size, reads eye positions and eases the fallback blend.
	EyeTracker::Pump();

	// Keyboard/mouse snapshot. VrSim routes it to the simulated head/hands and
	// zeroes the mouse deltas it consumed.
	RawInput::State input = RawInput::Consume();
	VrSim::Update( input );

	// H (edge-triggered): show/hide the OS pointer over the direct-mode output.
	const bool h_down = input.keys[ 'H' ];
	if ( h_down && !prev_h_down_ && hmd_device_ != nullptr )
	{
		hmd_device_->ToggleCursor();
	}
	prev_h_down_ = h_down;

	// Keep the VR windows on the English keyboard layout so typing in VR does
	// not produce Chinese IME text (see ime_switch.h). Internally throttled.
	ImeSwitch::Update();

	if ( hmd_device_ != nullptr )
	{
		// Publish the (virtual-head-modulated) pose.
		hmd_device_->UpdatePose();

		// Keep the off-axis frustum + eye-to-head in sync with the tracked eyes.
		hmd_device_->PushDisplayTransforms();

		// Keep the compositor output window on the 3D panel.
		hmd_device_->EnforceWindowPlacement();
	}

	// --- Virtual controllers: pull this frame's routed hand state ----------
	if ( controller_left_ != nullptr )
	{
		controller_left_->RunFrame();
	}
	if ( controller_right_ != nullptr )
	{
		controller_right_->RunFrame();
	}

	// Drain the event queue (haptics go to the controllers) so it does not back up.
	vr::VREvent_t event = {};
	while ( vr::VRServerDriverHost()->PollNextEvent( &event, sizeof( vr::VREvent_t ) ) )
	{
		if ( controller_left_ != nullptr )
		{
			controller_left_->ProcessEvent( event );
		}
		if ( controller_right_ != nullptr )
		{
			controller_right_->ProcessEvent( event );
		}
	}
}

void NakedDeviceProvider::EnterStandby()
{
}

void NakedDeviceProvider::LeaveStandby()
{
}

void NakedDeviceProvider::Cleanup()
{
	controller_left_ = nullptr;
	controller_right_ = nullptr;
	hmd_device_ = nullptr;

	// Stop the raw-input capture thread before vrserver tears the context down.
	if ( raw_input_started_ )
	{
		RawInput::Shutdown();
		raw_input_started_ = false;
	}

	// Unregister the eye-tracking SDK (leaves an in-flight background Register
	// alone, see EyeTracker::Shutdown).
	EyeTracker::Shutdown();
}
