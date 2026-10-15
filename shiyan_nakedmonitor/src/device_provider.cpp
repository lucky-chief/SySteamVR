//============ Shiyan Naked Monitor OpenVR driver ============
#include "device_provider.h"

#include "driverlog.h"
#include "eye_tracker.h"

vr::EVRInitError NakedDeviceProvider::Init( vr::IVRDriverContext *pDriverContext )
{
	// Initialise the server driver context so we can call into the runtime.
	VR_INIT_SERVER_DRIVER_CONTEXT( pDriverContext );

	hmd_device_ = std::make_unique< NakedHMDDeviceDriver >();

	if ( !vr::VRServerDriverHost()->TrackedDeviceAdded(
			hmd_device_->GetSerialNumber().c_str(), vr::TrackedDeviceClass_HMD, hmd_device_.get() ) )
	{
		DriverLog( "shiyan_nakedmonitor: failed to add HMD device." );
		return vr::VRInitError_Driver_Unknown;
	}

	DriverLog( "shiyan_nakedmonitor: HMD device added." );
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

	if ( hmd_device_ != nullptr )
	{
		// Publish our (static) pose. Cheap, and keeps the device in the "ready" state.
		hmd_device_->UpdatePose();

		// Keep the off-axis frustum + eye-to-head in sync with the tracked eyes.
		hmd_device_->PushDisplayTransforms();

		// Keep the compositor output window on the 3D panel.
		hmd_device_->EnforceWindowPlacement();
	}

	// Drain the event queue so it does not back up.
	vr::VREvent_t event = {};
	while ( vr::VRServerDriverHost()->PollNextEvent( &event, sizeof( vr::VREvent_t ) ) )
	{
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
	hmd_device_ = nullptr;

	// Unregister the eye-tracking SDK (leaves an in-flight background Register
	// alone, see EyeTracker::Shutdown).
	EyeTracker::Shutdown();
}
