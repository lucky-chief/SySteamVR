//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <chrono>
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

	// Match the panel by its EDID monitor name (case-insensitive substring)
	// instead of by resolution. The 3D panel changes resolution between 2D
	// (3840x2160) and 3D (7680x2160) modes, so the name is the only reliable
	// way to find it. Empty => fall back to resolution matching.
	std::string monitor_name_substring;

	// --- Off-axis (general perspective) projection -------------------------
	// The frustum is derived from the physical panel geometry and the current
	// (tracked) eye positions, replacing the old symmetric horizontal/vertical
	// FOV settings entirely:
	//   screen_width_m / screen_height_m      - panel size (SDK overrides)
	//   screen_center_height_m                - panel centre height above floor
	//   screen_distance_m                     - nominal eye-to-panel distance;
	//                                           locates the panel plane in the
	//                                           tracking space
	// float fov_horizontal_deg / fov_vertical_deg were removed accordingly.
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

	// Periodically verify that the compositor's "Headset Window" actually sits
	// at our resolved bounds and move it back if not. SteamVR has been observed
	// to park that window at (32767, 32767) - far outside the virtual desktop -
	// while the vrmonitor mirror keeps rendering normally, so the view silently
	// never reaches the panel. Called from the provider's RunFrame().
	void EnforceWindowPlacement();

private:
	NakedDisplayConfig config_;

	int32_t resolved_window_x_ = 0;
	int32_t resolved_window_y_ = 0;
	// Window size actually used. Starts at the configured size and is
	// replaced by the matched monitor's current resolution so the window
	// always fills the panel, whatever resolution the panel is in.
	int32_t resolved_window_width_ = 0;
	int32_t resolved_window_height_ = 0;

	// Frames until the next window-placement check (~90 Hz RunFrame).
	int placement_check_count_ = 0;
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

	// Keeps the compositor output window on the panel (see
	// NakedDisplayComponent::EnforceWindowPlacement).
	void EnforceWindowPlacement();

	// Pushes the current (eye-tracked) eye-to-head transforms and off-axis
	// projection to vrserver via IVRServerDriverHost::SetDisplayEyeToHead /
	// SetDisplayProjectionRaw whenever the eye positions changed noticeably.
	// The runtime only samples GetProjectionRaw once at startup, so live eye
	// tracking needs these pushes to take effect.
	void PushDisplayTransforms();

private:
	std::unique_ptr<NakedDisplayComponent> display_component_;

	std::string serial_number_;
	std::string model_number_;

	float ipd_meters_ = 0.065f;
	float eye_depth_meters_ = 0.5f;
	float display_frequency_ = 60.0f;
	float vsync_to_photons_ = 0.011f;
	float seated_height_meters_ = 1.2f;

	// --- Off-axis projection geometry (tracking space) ---------------------
	// The panel centre sits at (0, screen_center_height_m_, -screen_distance_m_)
	// with the viewer on the +z side; the eye tracker's screen-space coords are
	// translated by exactly that to obtain tracking-space eye positions.
	float screen_center_height_m_ = 1.2f;
	float screen_distance_m_ = 0.5f;

	// Change detection for PushDisplayTransforms (last pushed eye positions).
	float last_pushed_eyes_[ 6 ] = {};
	bool pushed_display_transforms_ = false;
	std::chrono::steady_clock::time_point last_push_log_time_{};

	// SetDisplayProjectionRaw HmdRect2 y-axis convention probe:
	//   0 = don't push the projection (runtime keeps re-querying GetProjectionRaw)
	//   1 = vTopLeft.y = +Y edge, vBottomRight.y = -Y edge (y-up, geometric)
	//   2 = vTopLeft.y = -Y edge, vBottomRight.y = +Y edge (y-down, pixel/UV habit)
	int projection_push_mode_ = 2;

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
