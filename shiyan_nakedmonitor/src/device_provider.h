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
	std::unique_ptr< ShiyanNakedController > controller_;
	bool raw_input_started_ = false;
};
