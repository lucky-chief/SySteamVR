//============ Shiyan Naked Monitor OpenVR driver ============
#include "device_provider.h"

#include "driverlog.h"
#include "eye_tracker.h"
#include "focus_gate.h"
#include "gamepad_input.h"
#include "ime_switch.h"
#include "raw_input.h"
#include "vr_sim.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

	// Physical gamepad (XInput), mapped onto the virtual controllers in RunFrame.
	GamepadInput::Init();

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

	// --- Virtual gamepad ----------------------------------------------------
	// Not implemented by this driver: SteamVR's built-in "gamepad" driver is the
	// supported path (enable driver_gamepad and disable Steam Input for SteamVR).

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

	// --- Physical gamepad (XInput) -> virtual controllers ------------------
	// See gamepad_input.h: the pad drives the same simulated head/hands as the
	// keyboard/mouse, because a custom driver cannot use "/user/gamepad" and the
	// built-in gamepad bindings only cover a few SteamVR Home actions.
	GamepadInput::State pad = GamepadInput::Poll();

	// --- Focus gate --------------------------------------------------------
	// The raw input capture is global (RIDEV_INPUTSINK), so without this the
	// user's normal desktop typing would drive the virtual head/hands. Only
	// feed the simulator while the VR view / a SteamVR window / a fullscreen
	// app owns the foreground (see focus_gate.h). Deltas are still consumed
	// above, so refocusing never produces a jump, and held keys are dropped
	// as released (a zeroed state) while unfocused.
	if ( !FocusGate::IsInputActive() )
	{
		input = RawInput::State{};
		pad = GamepadInput::State{};
	}

	// Back = keyboard V (help overlay); Y = keyboard G (interaction mode).
	if ( pad.back && !prev_pad_back_ && hmd_device_ != nullptr )
	{
		hmd_device_->ToggleHelpOverlay();
	}
	prev_pad_back_ = pad.back;

	VrSim::GamepadInput sim_pad;
	sim_pad.connected = pad.connected;
	sim_pad.moveX = pad.leftX;
	sim_pad.moveY = pad.leftY;
	sim_pad.lookX = pad.rightX;
	sim_pad.lookY = pad.rightY;
	sim_pad.trigger = pad.rightTrigger;  // RT
	sim_pad.grip = pad.leftTrigger;      // LT
	sim_pad.facePrimary = pad.a;
	sim_pad.buttonB = pad.b;
	sim_pad.system = pad.start;
	sim_pad.modeToggle = pad.y;
	sim_pad.reset = pad.rightStickClick;
	sim_pad.lookModifier = pad.shoulderLeft;  // LB
	sim_pad.moveModifier = pad.shoulderRight; // RB
	sim_pad.prevTarget = pad.dpadLeft;
	sim_pad.nextTarget = pad.dpadRight;
	sim_pad.thumbY = ( pad.dpadUp ? 1.0f : 0.0f ) - ( pad.dpadDown ? 1.0f : 0.0f );

	// Key-hint overlay: V or Back shows/hides it; while visible, A/D,
	// Left/Right, the d-pad or the left stick page through the images and are
	// withheld from the simulator (no movement, no target switching).
	const bool v_down = input.keys[ 'V' ];
	if ( v_down && !prev_v_down_ && hmd_device_ != nullptr )
	{
		hmd_device_->ToggleHelpOverlay();
	}
	prev_v_down_ = v_down;

	if ( hmd_device_ != nullptr && hmd_device_->IsHelpVisible() )
	{
		const bool stick_left = pad.connected && pad.leftX < -0.6f;
		const bool stick_right = pad.connected && pad.leftX > 0.6f;
		const bool page_left = input.keys[ 'A' ] || input.keys[ VK_LEFT ] || pad.dpadLeft || stick_left;
		const bool page_right = input.keys[ 'D' ] || input.keys[ VK_RIGHT ] || pad.dpadRight || stick_right;
		if ( page_left && !prev_help_left_ )
		{
			hmd_device_->NextHelpPage( -1 );
		}
		if ( page_right && !prev_help_right_ )
		{
			hmd_device_->NextHelpPage( 1 );
		}
		prev_help_left_ = page_left;
		prev_help_right_ = page_right;

		input.keys[ 'A' ] = false;
		input.keys[ 'D' ] = false;
		input.keys[ VK_LEFT ] = false;
		input.keys[ VK_RIGHT ] = false;

		// Paging must not also move the target or switch it.
		sim_pad.moveX = 0.0f;
		sim_pad.prevTarget = false;
		sim_pad.nextTarget = false;
	}
	else
	{
		prev_help_left_ = false;
		prev_help_right_ = false;
	}

	VrSim::SetGamepad( sim_pad );
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
