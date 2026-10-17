//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <memory>

#include "naked_hmd_device_driver.h"
#include "openvr_driver.h"
#include "virtual_controller.h"

class NakedDeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
	vr::EVRInitError Init( vr::IVRDriverContext *pDriverContext ) override;
	const char *const *GetInterfaceVersions() override;

	void RunFrame() override;

	bool ShouldBlockStandbyMode() override;
	void EnterStandby() override;
	void LeaveStandby() override;

	void Cleanup() override;

private:
	std::unique_ptr< NakedHMDDeviceDriver > hmd_device_;
	std::unique_ptr< ShiyanNakedController > controller_left_;
	std::unique_ptr< ShiyanNakedController > controller_right_;
	bool raw_input_started_ = false;
	bool prev_h_down_ = false; // H hotkey edge detection (cursor toggle)
	bool prev_v_down_ = false; // V hotkey edge detection (help overlay)
	bool prev_help_left_ = false;  // A / Left paging edge
	bool prev_help_right_ = false; // D / Right paging edge
	bool prev_pad_back_ = false;   // gamepad Back (help overlay) edge
};
