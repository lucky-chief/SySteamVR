//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "openvr_driver.h"

// Forward declarations so this header stays free of <d3d11.h>/<windows.h>.
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11SamplerState;
struct ID3D11Buffer;

//-----------------------------------------------------------------------------
// IVRDriverDirectModeComponent.
//
// In direct mode the driver - not the VR compositor - owns the output path:
// it creates the borderless window on the SBS panel, a D3D11 device and a swap
// chain, and presents the frame itself. There is therefore no compositor
// "Headset Window" to place or correct (the whole class of -202 / window-
// placement problems disappears).
//
// Stage 1: window + D3D11 device + swap chain + Present path (solid colour).
// Stage 2: SubmitLayer's per-eye textures are drawn side by side into the
//          backbuffer (the real SBS composition) and presented.
//-----------------------------------------------------------------------------
class NakedDirectModeComponent : public vr::IVRDriverDirectModeComponent
{
public:
	NakedDirectModeComponent( int32_t window_x, int32_t window_y, int32_t window_width, int32_t window_height,
		std::string monitor_name );
	~NakedDirectModeComponent();

	// ----- vr::IVRDriverDirectModeComponent ---------------------------------
	void CreateSwapTextureSet( uint32_t unPid, const SwapTextureSetDesc_t *pSwapTextureSetDesc, SwapTextureSet_t *pOutSwapTextureSet ) override;
	void DestroySwapTextureSet( vr::SharedTextureHandle_t sharedTextureHandle ) override;
	void DestroyAllSwapTextureSets( uint32_t unPid ) override;
	void GetNextSwapTextureSetIndex( vr::SharedTextureHandle_t sharedTextureHandles[ 2 ], uint32_t ( *pIndices )[ 2 ] ) override;
	void SubmitLayer( const SubmitLayerPerEye_t ( &perEye )[ 2 ] ) override;
	void Present( vr::SharedTextureHandle_t syncTexture ) override;
	void PostPresent( const Throttling_t *pThrottling ) override;
	void GetFrameTiming( vr::DriverDirectMode_FrameTiming *pFrameTiming ) override;

	// H hotkey: toggle the OS pointer over the output window. Requires direct
	// mode, otherwise it is a no-op.
	void ToggleCursor();

private:
	// Lazily creates the window / device / swap chain on first use. Safe to call
	// from any of the compositor callbacks.
	bool EnsureInitialized();
	bool EnsurePipeline();
	bool EnsureBackbufferView();
	void Shutdown();
	void DrawEye( int screen_half, int eye_index );
	void DrawSolidColor();
	ID3D11ShaderResourceView *FindSrv( vr::SharedTextureHandle_t handle );

	// Hang watchdog: logs which callback is stuck if one never returns.
	void EnterCallback( const char *name );
	void LeaveCallback();
	void WatchdogMain();

	// The output window lives on its own message-pumping thread. A window that
	// never pumps makes Windows flag its host (vrserver) as hung (AppHangB1) the
	// moment the user interacts with it.
	void WindowThreadMain();
	std::thread window_thread_;
	std::atomic< bool > window_stop_{ false };
	std::atomic< unsigned long > window_thread_id_{ 0 };
	void *window_ready_ = nullptr;   // HANDLE

	// RAII marker for the watchdog.
	struct CallbackScope
	{
		NakedDirectModeComponent *self;
		CallbackScope( NakedDirectModeComponent *s, const char *name ) : self( s ) { self->EnterCallback( name ); }
		~CallbackScope() { self->LeaveCallback(); }
	};

	int32_t window_x_ = 0;
	int32_t window_y_ = 0;
	int32_t window_width_ = 7680;
	int32_t window_height_ = 2160;
	std::string monitor_name_;

	std::mutex mutex_;

	void *window_ = nullptr;        // HWND (kept void* so <windows.h> stays in the .cpp)
	ID3D11Device *device_ = nullptr;
	ID3D11DeviceContext *context_ = nullptr;
	IDXGISwapChain *swapchain_ = nullptr;
	ID3D11RenderTargetView *backbuffer_rtv_ = nullptr;
	bool initialized_ = false;

	// --- Stage 2 render pipeline: full-screen quad, sample the eye texture ---
	ID3D11VertexShader *vertex_shader_ = nullptr;
	ID3D11PixelShader *pixel_shader_ = nullptr;
	ID3D11SamplerState *sampler_ = nullptr;
	ID3D11Buffer *uv_constant_buffer_ = nullptr;
	bool pipeline_ready_ = false;

	// Swap-texture sets handed to the compositor. Each set is one eye's ring of
	// shared textures; the key is the set's first handle.
	struct SwapSet
	{
		uint32_t pid = 0;              // process that created the set
		std::vector< vr::SharedTextureHandle_t > handles;
		std::vector< ID3D11Texture2D * > textures;
		std::vector< ID3D11ShaderResourceView * > srvs;
		uint32_t next = 0;
	};
	std::map< vr::SharedTextureHandle_t, SwapSet > swap_sets_;

	static void ReleaseSet( SwapSet &set );
	static bool SetOwnsSrv( const SwapSet &set, ID3D11ShaderResourceView *srv );

	// Latest SubmitLayer inputs, ready to be drawn by Present.
	ID3D11ShaderResourceView *eye_srv_[ 2 ] = { nullptr, nullptr };
	float eye_uv_rect_[ 2 ][ 4 ] = { { 0.0f, 0.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } };
	bool have_eye_textures_ = false;

	// Colour pipeline. 0 = sRGB SRV + sRGB swap chain (default; the hardware does
	// linear<->sRGB for free), 1 = "raw" (strip sRGB from the SRV, non-sRGB swap
	// chain, byte-exact copy). Set via "direct_color_mode".
	int color_mode_ = 0;

	// Present with vsync (presents block until vblank). Without it the compositor
	// free-runs and pegs the GPU. Set via "direct_vsync".
	bool vsync_ = true;

	// Panel refresh, used to pace frames from PostPresent (Present itself is
	// non-blocking, so an occluded window can never stall the compositor).
	float display_frequency_ = 60.0f;
	std::chrono::steady_clock::time_point next_frame_time_{};

	// Swap the two halves if the panel is wired mirrored. Set via "swap_eyes".
	bool swap_eyes_ = false;

	// Keep the output window above all other windows. Off by default: a topmost
	// full-screen window hides every other window and makes Alt+Tab look broken.
	// Set via "direct_window_topmost".
	bool topmost_ = false;

	// Whether the OS pointer is currently hidden over the output window. Starts
	// from "direct_hide_cursor" and is toggled by the H hotkey.
	bool cursor_hidden_ = true;

	// Sync-texture handshake (see Driver_API_Documentation.md). The compositor
	// hands Present() a shared texture whose keyed mutex the driver acquires to
	// signal "frame taken"; without it the compositor times out every frame.
	vr::SharedTextureHandle_t sync_texture_handle_ = INVALID_SHARED_TEXTURE_HANDLE;
	ID3D11Texture2D *sync_texture_ = nullptr;
	bool logged_sync_acquire_ = false;

	uint64_t frame_counter_ = 0;

	// --- Hang watchdog state ----------------------------------------------
	std::thread watchdog_thread_;
	std::atomic< bool > watchdog_stop_{ false };
	std::atomic< const char * > current_callback_{ nullptr };
	std::atomic< long long > callback_started_ms_{ 0 };
	std::atomic< long long > last_callback_ms_{ 0 };
};
