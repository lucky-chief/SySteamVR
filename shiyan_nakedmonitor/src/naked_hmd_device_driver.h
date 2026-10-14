//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <memory>
#include <string>

#include "openvr_driver.h"

// Display configuration, loaded from the "shiyan_nakedmonitor_display" settings section.
struct NakedDisplayConfig
{
	int32_t window_x = 0;
	int32_t window_y = 0;
	int32_t window_width = 7680;
	int32_t window_height = 2160;

	int32_t render_width = 3840;
	int32_t render_height = 2160;

	bool auto_place_window = true;
	bool swap_eyes = false;

	float fov_horizontal_deg = 60.0f;
	// <= 0 means "derive from horizontal fov + per-eye aspect ratio".
	float fov_vertical_deg = 0.0f;
};

//-----------------------------------------------------------------------------
// IVRDisplayComponent: the runtime owns the display and renders into a
// full-screen desktop window (extended mode). For a side-by-side panel this
// window content is exactly an SBS image, so no extra processing is needed.
//-----------------------------------------------------------------------------
class NakedDisplayComponent : public vr::IVRDisplayComponent
{
public:
	explicit NakedDisplayComponent( const NakedDisplayConfig &config );

	// ----- vr::IVRDisplayComponent -----
	bool IsDisplayOnDesktop() override;
	bool IsDisplayRealDisplay() override;
	void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight ) override;
	void GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) override;
	void GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom ) override;
	vr::DistortionCoordinates_t ComputeDistortion( vr::EVREye eEye, float fU, float fV ) override;
	void GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) override;
	bool ComputeInverseDistortion( vr::HmdVector2_t *pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV ) override;

private:
	NakedDisplayConfig config_;

	int32_t resolved_window_x_ = 0;
	int32_t resolved_window_y_ = 0;

	float half_tan_h_ = 1.0f;
	float half_tan_v_ = 1.0f;
};

//-----------------------------------------------------------------------------
// The tracked device itself. For a fixed-viewpoint ("cinema mode") display the
// pose never changes, so no tracking thread is required.
//-----------------------------------------------------------------------------
class NakedHMDDeviceDriver : public vr::ITrackedDeviceServerDriver
{
public:
	NakedHMDDeviceDriver();

	vr::EVRInitError Activate( uint32_t unObjectId ) override;
	void EnterStandby() override;
	void *GetComponent( const char *pchComponentNameAndVersion ) override;
	void DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) override;
	vr::DriverPose_t GetPose() override;
	void Deactivate() override;

	const std::string &GetSerialNumber() const { return serial_number_; }

	// Called by our IServerTrackedDeviceProvider every frame.
	void UpdatePose();

private:
	std::unique_ptr<NakedDisplayComponent> display_component_;

	std::string serial_number_;
	std::string model_number_;

	float ipd_meters_ = 0.065f;
	float eye_depth_meters_ = 0.5f;
	float display_frequency_ = 60.0f;
	float vsync_to_photons_ = 0.011f;
	float seated_height_meters_ = 1.2f;

	// Prop_IsOnDesktop_Bool. Must stay false: setting it true makes vrmonitor
	// raise the "headset display is not fullscreen" warning (SteamVR error -202).
	bool is_on_desktop_property_ = false;

	// Prop_DisplayDebugMode_Bool. true => the compositor renders into a normal
	// borderless window instead of requesting exclusive fullscreen. This is what
	// lets a windowed output live on a desktop SBS panel and avoids error -202.
	bool display_debug_mode_ = true;

	// Always-on /proximity boolean input so SteamVR never puts the HMD to sleep.
	vr::VRInputComponentHandle_t proximity_handle_ = vr::k_ulInvalidInputComponentHandle;

	// Tracking universe + driver-provided chaperone. A driver that supplies its
	// own tracking must declare a universe and provide a play area, otherwise
	// SteamVR reports the HMD as "locating" and demands Room Setup forever.
	uint64_t universe_id_ = 2; // 0 is invalid, 1 is reserved for Oculus
	bool provide_chaperone_ = true;
	float play_area_meters_ = 1.0f;

	uint32_t device_index_ = vr::k_unTrackedDeviceIndexInvalid;
};
