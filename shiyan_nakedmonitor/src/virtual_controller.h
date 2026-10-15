//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <chrono>
#include <string>

#include "openvr_driver.h"
#include "raw_input.h"

//-----------------------------------------------------------------------------
// Virtual right-hand controller driven by keyboard + mouse ("plan B": raw
// input arrives on a dedicated thread, see raw_input.h).
//
// Structure follows Valve's simplecontroller sample: the provider adds the
// device, Activate() creates the input components declared in the input
// profile, and the provider's RunFrame() feeds updates + publishes the pose.
//
// Aim: mouse deltas steer yaw/pitch (the dashboard laser follows it).
// Position: the tracked-eye midpoint (screen-space -> tracking-space via the
// panel geometry settings) plus a configurable offset, so the controller
// sits where the user actually is.
//-----------------------------------------------------------------------------
class ShiyanNakedController : public vr::ITrackedDeviceServerDriver
{
public:
	ShiyanNakedController();

	vr::EVRInitError Activate( uint32_t unObjectId ) override;
	void EnterStandby() override;
	void *GetComponent( const char *pchComponentNameAndVersion ) override;
	void DebugRequest( char const *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) override;
	vr::DriverPose_t GetPose() override;
	void Deactivate() override;

	const std::string &GetSerialNumber() const { return serial_number_; }

	// Per-frame: apply the latest raw-input snapshot to the input components
	// and the aim pose, then publish the pose to vrserver.
	void RunFrame( const RawInput::State &input );

	// Haptic events routed by the provider's event pump.
	void ProcessEvent( const vr::VREvent_t &event );

private:
	enum EComponent
	{
		Comp_TriggerValue = 0,
		Comp_TriggerClick,
		Comp_GripValue,
		Comp_GripClick,
		Comp_AClick,
		Comp_BClick,
		Comp_AppMenuClick,
		Comp_JoystickX,
		Comp_JoystickY,
		Comp_Haptic,
		Comp_Count
	};

	void ApplyInput( const RawInput::State &input );
	void PushPose();

	std::string serial_number_;
	std::string model_number_;

	// --- vr_sim fallback aim: mouse deltas steer yaw/pitch (radians).
	// Used when interaction_mode == vr_sim, or when the mouse ray is
	// unavailable (no headset window / cursor) in natural_mouse mode.
	float aim_yaw_ = 0.0f;
	float aim_pitch_ = 0.0f;

	// Mouse-wheel pulse that emulates pushing the thumbstick forward/back
	// (wheel = joystick y for pulse_seconds, refreshed by each notch).
	float wheel_pulse_seconds_ = 0.0f;
	float wheel_pulse_sign_ = 1.0f;
	bool wheel_invert_ = false;

	// Controller position = eye midpoint + this offset, in tracking space
	// (x right, y up, z back). vr_sim fallback pose only; natural_mouse
	// derives both position and orientation from the pointer ray.
	float offset_x_ = 0.1f;
	float offset_y_ = -0.2f;
	float offset_z_ = 0.1f;

	// Radians of aim per mouse pixel (vr_sim fallback aim).
	float mouse_sensitivity_ = 0.0025f;

	// Panel geometry (screen-space -> tracking-space), same settings keys the
	// HMD device uses.
	float screen_center_height_m_ = 1.2f;
	float screen_distance_m_ = 0.5f;

	vr::VRInputComponentHandle_t components_[ Comp_Count ] = {};

	uint32_t device_index_ = vr::k_unTrackedDeviceIndexInvalid;
	bool active_ = false;

	// Frame delta for the wheel pulse decay.
	std::chrono::steady_clock::time_point last_runframe_time_{};

	std::chrono::steady_clock::time_point last_haptic_log_{};
};
