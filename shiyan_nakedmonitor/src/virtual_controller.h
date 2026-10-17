//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <chrono>
#include <string>

#include "openvr_driver.h"

//-----------------------------------------------------------------------------
// Virtual hand controller (left or right) driven by the keyboard/mouse input
// simulator (see vr_sim.h). One instance per hand; the provider adds both as
// TrackedDevice_Controller devices.
//
// The input simulator owns the shared head/hand state and routes the buttons to
// the active hand, so RunFrame() just pulls this hand's snapshot and publishes
// it. The pose comes from VrSim too (vr_sim: hand position/aim; natural_mouse:
// the eye -> cursor ray).
//-----------------------------------------------------------------------------
class ShiyanNakedController : public vr::ITrackedDeviceServerDriver
{
public:
	// hand: 0 = left, 1 = right.
	explicit ShiyanNakedController( int hand );

	vr::EVRInitError Activate( uint32_t unObjectId ) override;
	void EnterStandby() override;
	void *GetComponent( const char *pchComponentNameAndVersion ) override;
	void DebugRequest( char const *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) override;
	vr::DriverPose_t GetPose() override;
	void Deactivate() override;

	const std::string &GetSerialNumber() const { return serial_number_; }
	int GetHand() const { return hand_; }

	// Per-frame: apply the simulator's snapshot and publish the pose.
	void RunFrame();

	// Haptic events routed by the provider's event pump.
	void ProcessEvent( const vr::VREvent_t &event );

private:
	enum EComponent
	{
		Comp_TriggerValue = 0,
		Comp_TriggerClick,
		Comp_GripValue,
		Comp_GripClick,
		Comp_FacePrimary,
		Comp_FaceSecondary,
		Comp_AppMenuClick,
		Comp_SystemClick,
		Comp_JoystickX,
		Comp_JoystickY,
		Comp_JoystickClick,
		Comp_Haptic,
		Comp_Count
	};

	void ApplyInput();
	void PushPose();

	int hand_ = 1; // 0 = left, 1 = right
	std::string serial_number_;
	std::string model_number_;

	vr::VRInputComponentHandle_t components_[ Comp_Count ] = {};

	uint32_t device_index_ = vr::k_unTrackedDeviceIndexInvalid;
	bool active_ = false;

	std::chrono::steady_clock::time_point last_haptic_log_{};
};
