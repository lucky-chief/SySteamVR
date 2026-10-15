//============ Shiyan Naked Monitor OpenVR driver ============
#include "direct_mode.h"

#include "driverlog.h"

#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

namespace
{

const wchar_t *kOutputWindowClass = L"ShiyanNakedMonitorOutput";

// Settings section (same one the rest of the display config lives in).
const char *kDisplaySection = "shiyan_nakedmonitor_display";

template < typename T >
void SafeRelease( T *&p )
{
	if ( p != nullptr )
	{
		p->Release();
		p = nullptr;
	}
}

// A borderless output window has no interactive UI; the default proc is enough.
LRESULT CALLBACK OutputWndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
	return DefWindowProcW( hwnd, msg, wParam, lParam );
}

// DXGI format names are plain numbers; a couple of readable aliases help logs.
const char *FormatName( DXGI_FORMAT format )
{
	switch ( format )
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
	default: return "?";
	}
}

const char *ShareModeName( int mode )
{
	return ( mode == 1 ) ? "NT handle" : "legacy shared";
}

long long NowMs()
{
	return std::chrono::duration_cast< std::chrono::milliseconds >(
		std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// The compositor renders into _SRGB eye textures that are already display-ready.
// Sampling them through a matching (non-sRGB) SRV copies the bytes verbatim into
// our non-sRGB swap chain, so no gamma conversion happens anywhere.
DXGI_FORMAT StripSrgb( DXGI_FORMAT format )
{
	switch ( format )
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
	default: return format;
	}
}

// Full-screen triangle (SV_VertexID, no vertex buffer) sampling one eye texture
// over the region given by g_uvRect (xy = min uv, zw = size).
const char *kShaderSource = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain( uint id : SV_VertexID )
{
	VSOut o;
	float2 uv = float2( ( id << 1 ) & 2, id & 2 );
	o.uv = uv;
	o.pos = float4( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0 );
	return o;
}

Texture2D eyeTex : register( t0 );
SamplerState eyeSampler : register( s0 );
cbuffer UvRect : register( b0 ) { float4 g_uvRect; };

float4 PSMain( VSOut i ) : SV_Target
{
	float2 uv = g_uvRect.xy + i.uv * g_uvRect.zw;
	return float4( eyeTex.Sample( eyeSampler, uv ).rgb, 1.0 );
}
)";

} // namespace

NakedDirectModeComponent::NakedDirectModeComponent( int32_t window_x, int32_t window_y, int32_t window_width,
	int32_t window_height, std::string monitor_name )
	: window_x_( window_x )
	, window_y_( window_y )
	, window_width_( window_width )
	, window_height_( window_height )
	, monitor_name_( std::move( monitor_name ) )
{
	// "shared" (default) = legacy shared handle, no keyed mutex.
	// "nthandle"        = NT handle + keyed mutex (D3D11 11.1 strict sharing).
	char buffer[ 64 ] = {};
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	vr::VRSettings()->GetString( kDisplaySection, "direct_texture_share", buffer, sizeof( buffer ), &error );
	const std::string share_mode = ( error == vr::VRSettingsError_None ) ? buffer : "shared";
	preferred_share_mode_ = ( share_mode == "nthandle" ) ? 1 : 0;

	// Vsync caps the whole pipeline at the display refresh rate.
	error = vr::VRSettingsError_None;
	vsync_ = vr::VRSettings()->GetBool( kDisplaySection, "direct_vsync", &error );
	if ( error != vr::VRSettingsError_None )
	{
		vsync_ = true;
	}

	error = vr::VRSettingsError_None;
	swap_eyes_ = vr::VRSettings()->GetBool( kDisplaySection, "swap_eyes", &error );
	if ( error != vr::VRSettingsError_None )
	{
		swap_eyes_ = false;
	}

	error = vr::VRSettingsError_None;
	display_frequency_ = vr::VRSettings()->GetFloat( kDisplaySection, "display_frequency", &error );
	if ( error != vr::VRSettingsError_None || display_frequency_ < 1.0f )
	{
		display_frequency_ = 60.0f;
	}

	DriverLog( "shiyan_nakedmonitor: [direct] component created for window (%d, %d) %dx%d (monitor '%s', share '%s').",
		window_x_, window_y_, window_width_, window_height_, monitor_name_.c_str(), share_mode.c_str() );
}

NakedDirectModeComponent::~NakedDirectModeComponent()
{
	Shutdown();
}

void NakedDirectModeComponent::EnterCallback( const char *name )
{
	callback_started_ms_.store( NowMs() );
	current_callback_.store( name );
}

void NakedDirectModeComponent::LeaveCallback()
{
	current_callback_.store( nullptr );
	last_callback_ms_.store( NowMs() );
}

void NakedDirectModeComponent::WatchdogMain()
{
	// Open our own log once, then only ever WriteFile on it (no runtime, no CRT
	// heap, no per-write open) so an otherwise-stuck process can still be
	// reported, and so heartbeats show exactly when this thread stops too.
	wchar_t dir[ MAX_PATH ] = {};
	wchar_t path[ MAX_PATH + 32 ] = {};
	HANDLE file = INVALID_HANDLE_VALUE;
	if ( GetTempPathW( MAX_PATH, dir ) != 0 )
	{
		swprintf_s( path, L"%sshiyan_direct_watchdog.log", dir );
		file = CreateFileW( path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
	}

	auto emit = [file]( const char *text ) {
		if ( file == INVALID_HANDLE_VALUE )
		{
			return;
		}
		DWORD written = 0;
		WriteFile( file, text, static_cast< DWORD >( strlen( text ) ), &written, nullptr );
		WriteFile( file, "\r\n", 2, &written, nullptr );
		FlushFileBuffers( file );
	};

	DriverLog( "shiyan_nakedmonitor: [direct] watchdog thread running." );
	emit( "watchdog started" );

	constexpr long long kStuckMs = 1000;
	char line[ 256 ] = {};
	long long last_heartbeat = 0;

	while ( !watchdog_stop_.load() )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
		if ( watchdog_stop_.load() )
		{
			break;
		}

		const long long now = NowMs();
		const char *op = current_callback_.load();
		const long long last = last_callback_ms_.load();

		if ( op != nullptr )
		{
			const long long elapsed = now - callback_started_ms_.load();
			if ( elapsed > kStuckMs )
			{
				snprintf( line, sizeof( line ), "STUCK inside '%s' for %lld ms", op, elapsed );
				emit( line );
			}
		}
		else if ( last != 0 && now - last > kStuckMs )
		{
			snprintf( line, sizeof( line ), "no direct-mode callback for %lld ms", now - last );
			emit( line );
		}

		if ( now - last_heartbeat >= 2000 )
		{
			last_heartbeat = now;
			snprintf( line, sizeof( line ), "hb op=%s sinceCb=%lldms", op != nullptr ? op : "(none)",
				last != 0 ? ( now - last ) : -1 );
			emit( line );
		}
	}

	emit( "watchdog stopped" );
	if ( file != INVALID_HANDLE_VALUE )
	{
		CloseHandle( file );
	}
}

void NakedDirectModeComponent::WindowThreadMain()
{
	HINSTANCE instance = GetModuleHandleW( nullptr );

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof( wc );
	wc.lpfnWndProc = OutputWndProc;
	wc.hInstance = instance;
	wc.lpszClassName = kOutputWindowClass;
	RegisterClassExW( &wc );

	HWND hwnd = CreateWindowExW( WS_EX_TOPMOST, kOutputWindowClass, L"Shiyan Naked Monitor Output",
		WS_POPUP | WS_VISIBLE, window_x_, window_y_, window_width_, window_height_,
		nullptr, nullptr, instance, nullptr );
	if ( hwnd != nullptr )
	{
		// Topmost so DWM keeps scanning it out (an occluded output window can
		// make DXGI Present block).
		SetWindowPos( hwnd, HWND_TOPMOST, window_x_, window_y_, window_width_, window_height_,
			SWP_NOACTIVATE | SWP_SHOWWINDOW );
	}
	window_ = hwnd;
	window_thread_id_.store( GetCurrentThreadId() );
	if ( window_ready_ != nullptr )
	{
		SetEvent( reinterpret_cast< HANDLE >( window_ready_ ) );
	}

	// Pump messages: a window that never pumps makes Windows flag its host
	// process (vrserver) as hung (AppHangB1) as soon as the user interacts.
	MSG msg;
	while ( !window_stop_.load() && GetMessageW( &msg, nullptr, 0, 0 ) > 0 )
	{
		TranslateMessage( &msg );
		DispatchMessageW( &msg );
	}

	if ( hwnd != nullptr )
	{
		DestroyWindow( hwnd );
	}
	UnregisterClassW( kOutputWindowClass, instance );
}

void NakedDirectModeComponent::ReleaseSet( SwapSet &set )
{
	for ( IDXGIKeyedMutex *mutex : set.mutexes )
	{
		SafeRelease( mutex );
	}
	for ( ID3D11ShaderResourceView *srv : set.srvs )
	{
		SafeRelease( srv );
	}
	for ( ID3D11Texture2D *texture : set.textures )
	{
		SafeRelease( texture );
	}
	for ( vr::SharedTextureHandle_t handle : set.handles )
	{
		if ( handle != INVALID_SHARED_TEXTURE_HANDLE )
		{
			CloseHandle( reinterpret_cast< HANDLE >( handle ) );
		}
	}
	set.mutexes.clear();
	set.srvs.clear();
	set.textures.clear();
	set.handles.clear();
}

bool NakedDirectModeComponent::SetOwnsSrv( const SwapSet &set, ID3D11ShaderResourceView *srv )
{
	if ( srv == nullptr )
	{
		return false;
	}
	for ( ID3D11ShaderResourceView *candidate : set.srvs )
	{
		if ( candidate == srv )
		{
			return true;
		}
	}
	return false;
}

//-----------------------------------------------------------------------------
// Window + D3D11 device + swap chain
//-----------------------------------------------------------------------------
bool NakedDirectModeComponent::EnsureInitialized()
{
	// Caller must hold mutex_.
	if ( initialized_ )
	{
		return true;
	}

	// Create the output window on its own message-pumping thread.
	if ( window_ready_ == nullptr )
	{
		window_ready_ = CreateEventW( nullptr, TRUE, FALSE, nullptr );
	}
	if ( !window_thread_.joinable() )
	{
		window_stop_.store( false );
		window_thread_ = std::thread( &NakedDirectModeComponent::WindowThreadMain, this );
	}
	if ( window_ready_ != nullptr )
	{
		WaitForSingleObject( reinterpret_cast< HANDLE >( window_ready_ ), 5000 );
	}
	const HWND hwnd = reinterpret_cast< HWND >( window_ );
	if ( hwnd == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] output window creation failed." );
		return false;
	}

	// Hardware device; BGRA support so the swap chain can use a common format.
	UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_10_0;
	HRESULT hr = D3D11CreateDevice( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_flags, levels,
		ARRAYSIZE( levels ), D3D11_SDK_VERSION, &device_, &obtained, &context_ );
	if ( FAILED( hr ) || device_ == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] D3D11CreateDevice failed (0x%08X).", static_cast< unsigned >( hr ) );
		Shutdown();
		return false;
	}

	IDXGIDevice *dxgi_device = nullptr;
	IDXGIAdapter *adapter = nullptr;
	IDXGIFactory *factory = nullptr;
	if ( FAILED( device_->QueryInterface( __uuidof( IDXGIDevice ), reinterpret_cast< void ** >( &dxgi_device ) ) )
		 || FAILED( dxgi_device->GetAdapter( &adapter ) )
		 || FAILED( adapter->GetParent( __uuidof( IDXGIFactory ), reinterpret_cast< void ** >( &factory ) ) ) )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] could not reach IDXGIFactory." );
		SafeRelease( factory );
		SafeRelease( adapter );
		SafeRelease( dxgi_device );
		Shutdown();
		return false;
	}

	DXGI_SWAP_CHAIN_DESC desc = {};
	desc.BufferDesc.Width = static_cast< UINT >( window_width_ );
	desc.BufferDesc.Height = static_cast< UINT >( window_height_ );
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.OutputWindow = hwnd;
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	hr = factory->CreateSwapChain( device_, &desc, &swapchain_ );
	SafeRelease( factory );
	SafeRelease( adapter );
	SafeRelease( dxgi_device );

	if ( FAILED( hr ) || swapchain_ == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] CreateSwapChain failed (0x%08X).", static_cast< unsigned >( hr ) );
		Shutdown();
		return false;
	}

	if ( !EnsureBackbufferView() || !EnsurePipeline() )
	{
		Shutdown();
		return false;
	}

	if ( !watchdog_thread_.joinable() )
	{
		watchdog_stop_.store( false );
		watchdog_thread_ = std::thread( &NakedDirectModeComponent::WatchdogMain, this );
	}

	initialized_ = true;
	DriverLog( "shiyan_nakedmonitor: [direct] D3D11 + swap chain ready (%dx%d, feature level 0x%04X).",
		window_width_, window_height_, static_cast< unsigned >( obtained ) );
	return true;
}

bool NakedDirectModeComponent::EnsureBackbufferView()
{
	if ( swapchain_ == nullptr || device_ == nullptr )
	{
		return false;
	}

	ID3D11Texture2D *backbuffer = nullptr;
	HRESULT hr = swapchain_->GetBuffer( 0, __uuidof( ID3D11Texture2D ), reinterpret_cast< void ** >( &backbuffer ) );
	if ( FAILED( hr ) || backbuffer == nullptr )
	{
		return false;
	}

	hr = device_->CreateRenderTargetView( backbuffer, nullptr, &backbuffer_rtv_ );
	SafeRelease( backbuffer );
	return SUCCEEDED( hr ) && backbuffer_rtv_ != nullptr;
}

bool NakedDirectModeComponent::EnsurePipeline()
{
	// Caller must hold mutex_.
	if ( pipeline_ready_ )
	{
		return true;
	}
	if ( device_ == nullptr )
	{
		return false;
	}

	ID3DBlob *vs_blob = nullptr;
	ID3DBlob *ps_blob = nullptr;
	ID3DBlob *errors = nullptr;

	HRESULT hr = D3DCompile( kShaderSource, std::strlen( kShaderSource ), "shiyan_direct", nullptr, nullptr,
		"VSMain", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vs_blob, &errors );
	if ( FAILED( hr ) )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] vertex shader compile failed: %s",
			errors != nullptr ? static_cast< const char * >( errors->GetBufferPointer() ) : "?" );
		SafeRelease( errors );
		SafeRelease( vs_blob );
		return false;
	}
	SafeRelease( errors );

	hr = D3DCompile( kShaderSource, std::strlen( kShaderSource ), "shiyan_direct", nullptr, nullptr,
		"PSMain", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &ps_blob, &errors );
	if ( FAILED( hr ) )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] pixel shader compile failed: %s",
			errors != nullptr ? static_cast< const char * >( errors->GetBufferPointer() ) : "?" );
		SafeRelease( errors );
		SafeRelease( vs_blob );
		SafeRelease( ps_blob );
		return false;
	}
	SafeRelease( errors );

	hr = device_->CreateVertexShader( vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader_ );
	SafeRelease( vs_blob );
	if ( FAILED( hr ) || vertex_shader_ == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] CreateVertexShader failed (0x%08X).", static_cast< unsigned >( hr ) );
		SafeRelease( ps_blob );
		return false;
	}

	hr = device_->CreatePixelShader( ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pixel_shader_ );
	SafeRelease( ps_blob );
	if ( FAILED( hr ) || pixel_shader_ == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] CreatePixelShader failed (0x%08X).", static_cast< unsigned >( hr ) );
		return false;
	}

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = FLT_MAX;
	hr = device_->CreateSamplerState( &sd, &sampler_ );
	if ( FAILED( hr ) || sampler_ == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] CreateSamplerState failed (0x%08X).", static_cast< unsigned >( hr ) );
		return false;
	}

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof( float ) * 4; // float4 uv rect
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = device_->CreateBuffer( &cbd, nullptr, &uv_constant_buffer_ );
	if ( FAILED( hr ) || uv_constant_buffer_ == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] constant buffer create failed (0x%08X).", static_cast< unsigned >( hr ) );
		return false;
	}

	pipeline_ready_ = true;
	DriverLog( "shiyan_nakedmonitor: [direct] SBS render pipeline ready." );
	return true;
}

void NakedDirectModeComponent::Shutdown()
{
	// Caller must hold mutex_ (or be the destructor).
	watchdog_stop_.store( true );
	if ( watchdog_thread_.joinable() )
	{
		watchdog_thread_.join();
	}

	for ( auto &entry : swap_sets_ )
	{
		ReleaseSet( entry.second );
	}
	swap_sets_.clear();

	eye_srv_[ 0 ] = nullptr;
	eye_srv_[ 1 ] = nullptr;
	have_eye_textures_ = false;

	SafeRelease( sync_texture_ );
	sync_texture_handle_ = INVALID_SHARED_TEXTURE_HANDLE;
	logged_sync_acquire_ = false;

	SafeRelease( uv_constant_buffer_ );
	SafeRelease( sampler_ );
	SafeRelease( pixel_shader_ );
	SafeRelease( vertex_shader_ );
	pipeline_ready_ = false;

	SafeRelease( backbuffer_rtv_ );
	SafeRelease( swapchain_ );
	SafeRelease( context_ );
	SafeRelease( device_ );

	// Stop the window thread (it destroys the window itself).
	window_stop_.store( true );
	if ( window_thread_.joinable() )
	{
		const DWORD tid = window_thread_id_.load();
		if ( tid != 0 )
		{
			PostThreadMessageW( tid, WM_QUIT, 0, 0 );
		}
		window_thread_.join();
	}
	if ( window_ready_ != nullptr )
	{
		CloseHandle( reinterpret_cast< HANDLE >( window_ready_ ) );
		window_ready_ = nullptr;
	}
	window_ = nullptr;

	initialized_ = false;
}

//-----------------------------------------------------------------------------
// IVRDriverDirectModeComponent
//-----------------------------------------------------------------------------
void NakedDirectModeComponent::CreateSwapTextureSet( uint32_t unPid, const SwapTextureSetDesc_t *pSwapTextureSetDesc,
	SwapTextureSet_t *pOutSwapTextureSet )
{
	CallbackScope scope( this, "CreateSwapTextureSet" );

	if ( pSwapTextureSetDesc == nullptr || pOutSwapTextureSet == nullptr )
	{
		return;
	}

	// Leave a clean (invalid) set behind if anything below fails.
	*pOutSwapTextureSet = {};

	std::lock_guard< std::mutex > lock( mutex_ );
	if ( !EnsureInitialized() )
	{
		return;
	}

	const DXGI_FORMAT format = static_cast< DXGI_FORMAT >( pSwapTextureSetDesc->nFormat );

	D3D11_TEXTURE2D_DESC base = {};
	base.Width = pSwapTextureSetDesc->nWidth;
	base.Height = pSwapTextureSetDesc->nHeight;
	base.MipLevels = 1;
	base.ArraySize = 1;
	base.Format = format;
	base.SampleDesc.Count = 1; // shared textures cannot be multisampled
	base.Usage = D3D11_USAGE_DEFAULT;
	base.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	// mode 0: legacy shared handle (IDXGIResource::GetSharedHandle, unTextureFlags 0).
	// mode 1: NT handle (needs NTHANDLE | KEYEDMUTEX, CreateSharedHandle, flag set).
	auto buildSet = [&]( int mode, SwapSet &out, uint32_t &out_flags ) -> bool {
		D3D11_TEXTURE2D_DESC td = base;
		td.MiscFlags = ( mode == 1 )
			? ( D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX )
			: D3D11_RESOURCE_MISC_SHARED;

		for ( int i = 0; i < 3; ++i )
		{
			ID3D11Texture2D *texture = nullptr;
			HRESULT hr = device_->CreateTexture2D( &td, nullptr, &texture );
			if ( FAILED( hr ) || texture == nullptr )
			{
				DriverLog( "shiyan_nakedmonitor: [direct] CreateTexture2D failed (0x%08X) [%s] for %ux%u %s.",
					static_cast< unsigned >( hr ), ShareModeName( mode ), td.Width, td.Height, FormatName( format ) );
				ReleaseSet( out );
				return false;
			}

			HANDLE handle = nullptr;
			if ( mode == 1 )
			{
				IDXGIResource1 *resource = nullptr;
				if ( SUCCEEDED( texture->QueryInterface( __uuidof( IDXGIResource1 ), reinterpret_cast< void ** >( &resource ) ) )
					 && resource != nullptr )
				{
					resource->CreateSharedHandle( nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle );
					resource->Release();
				}
			}
			else
			{
				IDXGIResource *resource = nullptr;
				if ( SUCCEEDED( texture->QueryInterface( __uuidof( IDXGIResource ), reinterpret_cast< void ** >( &resource ) ) )
					 && resource != nullptr )
				{
					resource->GetSharedHandle( &handle );
					resource->Release();
				}
			}

			if ( handle == nullptr )
			{
				DriverLog( "shiyan_nakedmonitor: [direct] shared handle failed for texture %d [%s].", i, ShareModeName( mode ) );
				SafeRelease( texture );
				ReleaseSet( out );
				return false;
			}

			// SRV for the Stage 2 blit. The SRV uses the non-SRGB view so the
			// display-ready bytes pass through unchanged.
			D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
			svd.Format = StripSrgb( format );
			svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			svd.Texture2D.MostDetailedMip = 0;
			svd.Texture2D.MipLevels = 1;
			ID3D11ShaderResourceView *srv = nullptr;
			HRESULT srv_hr = device_->CreateShaderResourceView( texture, &svd, &srv );
			if ( FAILED( srv_hr ) )
			{
				srv_hr = device_->CreateShaderResourceView( texture, nullptr, &srv );
			}
			if ( FAILED( srv_hr ) || srv == nullptr )
			{
				DriverLog( "shiyan_nakedmonitor: [direct] CreateShaderResourceView failed (0x%08X).", static_cast< unsigned >( srv_hr ) );
				CloseHandle( reinterpret_cast< HANDLE >( handle ) );
				SafeRelease( texture );
				ReleaseSet( out );
				return false;
			}

			IDXGIKeyedMutex *keyed_mutex = nullptr;
			texture->QueryInterface( __uuidof( IDXGIKeyedMutex ), reinterpret_cast< void ** >( &keyed_mutex ) );

			out.textures.push_back( texture );
			out.handles.push_back( reinterpret_cast< vr::SharedTextureHandle_t >( handle ) );
			out.mutexes.push_back( keyed_mutex );
			out.srvs.push_back( srv );
		}

		out_flags = ( mode == 1 ) ? vr::VRSwapTextureFlag_Shared_NTHandle : 0;
		return true;
	};

	const int first_mode = ( resolved_share_mode_ >= 0 ) ? resolved_share_mode_ : preferred_share_mode_;
	SwapSet set;
	uint32_t flags = 0;
	bool ok = buildSet( first_mode, set, flags );

	// If the preferred sharing mode cannot even be created, try the other once
	// (and remember which one works for the rest of the session).
	if ( !ok && resolved_share_mode_ < 0 )
	{
		const int alt_mode = 1 - first_mode;
		DriverLog( "shiyan_nakedmonitor: [direct] %s sharing failed to create; retrying with %s.",
			ShareModeName( first_mode ), ShareModeName( alt_mode ) );
		set = SwapSet{};
		ok = buildSet( alt_mode, set, flags );
		if ( ok )
		{
			resolved_share_mode_ = alt_mode;
		}
	}
	if ( !ok )
	{
		return;
	}
	if ( resolved_share_mode_ < 0 )
	{
		resolved_share_mode_ = first_mode;
	}

	pOutSwapTextureSet->unTextureFlags = flags;
	for ( int i = 0; i < 3; ++i )
	{
		pOutSwapTextureSet->rSharedTextureHandles[ i ] = set.handles[ i ];
	}

	DriverLog( "shiyan_nakedmonitor: [direct] created swap texture set for pid %u: %ux%u %s (3 buffers, %s sharing).",
		unPid, base.Width, base.Height, FormatName( format ), ShareModeName( resolved_share_mode_ ) );

	set.pid = unPid;
	swap_sets_[ set.handles[ 0 ] ] = std::move( set );
}

void NakedDirectModeComponent::DestroySwapTextureSet( vr::SharedTextureHandle_t sharedTextureHandle )
{
	CallbackScope scope( this, "DestroySwapTextureSet" );
	std::lock_guard< std::mutex > lock( mutex_ );

	// Find the set that owns this handle (only one handle needs to be passed).
	for ( auto it = swap_sets_.begin(); it != swap_sets_.end(); ++it )
	{
		SwapSet &set = it->second;
		bool owns = false;
		for ( vr::SharedTextureHandle_t handle : set.handles )
		{
			if ( handle == sharedTextureHandle )
			{
				owns = true;
				break;
			}
		}
		if ( !owns )
		{
			continue;
		}

		if ( SetOwnsSrv( set, eye_srv_[ 0 ] ) )
		{
			eye_srv_[ 0 ] = nullptr;
		}
		if ( SetOwnsSrv( set, eye_srv_[ 1 ] ) )
		{
			eye_srv_[ 1 ] = nullptr;
		}
		ReleaseSet( set );
		swap_sets_.erase( it );
		have_eye_textures_ = ( eye_srv_[ 0 ] != nullptr && eye_srv_[ 1 ] != nullptr );
		return;
	}
}

void NakedDirectModeComponent::DestroyAllSwapTextureSets( uint32_t unPid )
{
	CallbackScope scope( this, "DestroyAllSwapTextureSets" );
	std::lock_guard< std::mutex > lock( mutex_ );

	// Only purge the sets created by this pid (matching the documented
	// "purge all texture sets for a given process" semantic). Dropping every set
	// here silently invalidated the sets the current app still rendered into.
	size_t dropped = 0;
	for ( auto it = swap_sets_.begin(); it != swap_sets_.end(); )
	{
		if ( it->second.pid == unPid )
		{
			if ( SetOwnsSrv( it->second, eye_srv_[ 0 ] ) )
			{
				eye_srv_[ 0 ] = nullptr;
			}
			if ( SetOwnsSrv( it->second, eye_srv_[ 1 ] ) )
			{
				eye_srv_[ 1 ] = nullptr;
			}
			ReleaseSet( it->second );
			it = swap_sets_.erase( it );
			++dropped;
		}
		else
		{
			++it;
		}
	}

	have_eye_textures_ = ( eye_srv_[ 0 ] != nullptr && eye_srv_[ 1 ] != nullptr );
	DriverLog( "shiyan_nakedmonitor: [direct] DestroyAllSwapTextureSets(pid %u): dropped %zu set(s), %zu remain.",
		unPid, dropped, swap_sets_.size() );
}

void NakedDirectModeComponent::GetNextSwapTextureSetIndex( vr::SharedTextureHandle_t sharedTextureHandles[ 2 ],
	uint32_t ( *pIndices )[ 2 ] )
{
	CallbackScope scope( this, "GetNextSwapTextureSetIndex" );
	std::lock_guard< std::mutex > lock( mutex_ );

	for ( int eye = 0; eye < 2; ++eye )
	{
		uint32_t index = 0;
		for ( auto &entry : swap_sets_ )
		{
			SwapSet &set = entry.second;
			bool owns = false;
			for ( vr::SharedTextureHandle_t handle : set.handles )
			{
				if ( handle == sharedTextureHandles[ eye ] )
				{
					owns = true;
					break;
				}
			}
			if ( owns && !set.handles.empty() )
			{
				index = set.next;
				set.next = ( set.next + 1 ) % static_cast< uint32_t >( set.handles.size() );
				break;
			}
		}
		( *pIndices )[ eye ] = index;
	}
}

ID3D11ShaderResourceView *NakedDirectModeComponent::FindSrv( vr::SharedTextureHandle_t handle )
{
	// Caller must hold mutex_.
	for ( auto &entry : swap_sets_ )
	{
		SwapSet &set = entry.second;
		for ( size_t i = 0; i < set.handles.size(); ++i )
		{
			if ( set.handles[ i ] == handle && i < set.srvs.size() )
			{
				return set.srvs[ i ];
			}
		}
	}
	return nullptr;
}

void NakedDirectModeComponent::SubmitLayer( const SubmitLayerPerEye_t ( &perEye )[ 2 ] )
{
	CallbackScope scope( this, "SubmitLayer" );
	std::lock_guard< std::mutex > lock( mutex_ );

	// perEye[Eye_Left=0] / perEye[Eye_Right=1]: the compositor hands back the
	// swap textures we created, so the SRVs are looked up directly (no open).
	for ( int eye = 0; eye < 2; ++eye )
	{
		eye_srv_[ eye ] = FindSrv( perEye[ eye ].hTexture );

		const vr::VRTextureBounds_t &bounds = perEye[ eye ].bounds;
		float u0 = bounds.uMin;
		float v0 = bounds.vMin;
		float u1 = bounds.uMax;
		float v1 = bounds.vMax;
		if ( !( u1 > u0 ) || !( v1 > v0 ) )
		{
			u0 = 0.0f;
			v0 = 0.0f;
			u1 = 1.0f;
			v1 = 1.0f;
		}
		eye_uv_rect_[ eye ][ 0 ] = u0;
		eye_uv_rect_[ eye ][ 1 ] = v0;
		eye_uv_rect_[ eye ][ 2 ] = u1 - u0;
		eye_uv_rect_[ eye ][ 3 ] = v1 - v0;
	}

	have_eye_textures_ = ( eye_srv_[ 0 ] != nullptr && eye_srv_[ 1 ] != nullptr );

	if ( !have_eye_textures_ )
	{
		// The compositor occasionally hands back handles we do not know (e.g.
		// after it rebuilt its swap sets). Rate-limited diagnostic.
		static uint64_t s_mismatch_count = 0;
		if ( ( s_mismatch_count++ % 600 ) == 0 )
		{
			DriverLog( "shiyan_nakedmonitor: [direct] SubmitLayer unmatched: L=0x%llX (%s) R=0x%llX (%s), %zu set(s) known.",
				static_cast< unsigned long long >( perEye[ 0 ].hTexture ), eye_srv_[ 0 ] != nullptr ? "ok" : "MISS",
				static_cast< unsigned long long >( perEye[ 1 ].hTexture ), eye_srv_[ 1 ] != nullptr ? "ok" : "MISS",
				swap_sets_.size() );
		}
	}
}

void NakedDirectModeComponent::DrawEye( int screen_half, int eye_index )
{
	const UINT half_width = static_cast< UINT >( window_width_ ) / 2;

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = static_cast< float >( screen_half * half_width );
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast< float >( half_width );
	viewport.Height = static_cast< float >( window_height_ );
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context_->RSSetViewports( 1, &viewport );

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if ( SUCCEEDED( context_->Map( uv_constant_buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped ) ) )
	{
		std::memcpy( mapped.pData, eye_uv_rect_[ eye_index ], sizeof( float ) * 4 );
		context_->Unmap( uv_constant_buffer_, 0 );
	}

	context_->PSSetShaderResources( 0, 1, &eye_srv_[ eye_index ] );
	context_->Draw( 3, 0 );
}

void NakedDirectModeComponent::DrawSolidColor()
{
	const float clear_color[ 4 ] = { 0.05f, 0.05f, 0.08f, 1.0f };
	context_->ClearRenderTargetView( backbuffer_rtv_, clear_color );
}

void NakedDirectModeComponent::Present( vr::SharedTextureHandle_t syncTexture )
{
	CallbackScope scope( this, "Present" );
	std::lock_guard< std::mutex > lock( mutex_ );
	if ( !EnsureInitialized() )
	{
		return;
	}

	const auto present_start = std::chrono::steady_clock::now();

	// --- Sync-texture handshake (Driver_API_Documentation.md) --------------
	// Open the compositor's sync texture (once per handle) and acquire its keyed
	// mutex: that acquire is how the driver signals it has taken the frame.
	// Without it the compositor logs "WaitForAcquire timed out" every frame.
	IDXGIKeyedMutex *sync_mutex = nullptr;
	if ( syncTexture != sync_texture_handle_ )
	{
		sync_texture_handle_ = syncTexture;
		SafeRelease( sync_texture_ );
		if ( syncTexture != INVALID_SHARED_TEXTURE_HANDLE )
		{
			const HRESULT open_hr = device_->OpenSharedResource( reinterpret_cast< HANDLE >( syncTexture ),
				__uuidof( ID3D11Texture2D ), reinterpret_cast< void ** >( &sync_texture_ ) );
			if ( FAILED( open_hr ) )
			{
				DriverLog( "shiyan_nakedmonitor: [direct] OpenSharedResource(sync) failed (0x%08X).",
					static_cast< unsigned >( open_hr ) );
			}
		}
	}

	bool sync_acquired = false;
	if ( sync_texture_ != nullptr )
	{
		sync_texture_->QueryInterface( __uuidof( IDXGIKeyedMutex ), reinterpret_cast< void ** >( &sync_mutex ) );
	}
	if ( sync_mutex != nullptr )
	{
		sync_acquired = SUCCEEDED( sync_mutex->AcquireSync( 0, 10 ) );
		if ( !logged_sync_acquire_ )
		{
			logged_sync_acquire_ = true;
			DriverLog( "shiyan_nakedmonitor: [direct] sync texture %s.",
				sync_acquired ? "acquired (handshake OK)" : "acquire FAILED" );
		}
	}

	const float clear_color[ 4 ] = { 0.05f, 0.05f, 0.08f, 1.0f };
	context_->ClearRenderTargetView( backbuffer_rtv_, clear_color );
	context_->OMSetRenderTargets( 1, &backbuffer_rtv_, nullptr );

	if ( pipeline_ready_ && have_eye_textures_ )
	{
		context_->IASetInputLayout( nullptr );
		context_->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
		context_->VSSetShader( vertex_shader_, nullptr, 0 );
		context_->PSSetShader( pixel_shader_, nullptr, 0 );
		context_->PSSetSamplers( 0, 1, &sampler_ );
		context_->VSSetConstantBuffers( 0, 1, &uv_constant_buffer_ );
		context_->PSSetConstantBuffers( 0, 1, &uv_constant_buffer_ );

		// Left half gets the left eye, right half the right eye (swapped if set).
		const int left_eye = swap_eyes_ ? 1 : 0;
		const int right_eye = 1 - left_eye;
		DrawEye( 0, left_eye );
		DrawEye( 1, right_eye );

		// Unbind the eye textures so the compositor can render into them again.
		ID3D11ShaderResourceView *null_srv = nullptr;
		context_->PSSetShaderResources( 0, 1, &null_srv );
	}
	else
	{
		DrawSolidColor();
	}

	// Non-blocking present; PostPresent paces the frame rate. A blocking
	// Present(1,0) can stall for seconds when the output window is occluded,
	// which starves the compositor's render thread ("Aborting WaitForPresent").
	// DXGI_PRESENT_DO_NOT_WAIT: never block on the swap chain. A blocking
	// Present (when the queue is full or the window is occluded) would hang
	// vrserver, which hosts this driver in-process (observed as AppHangB1).
	const HRESULT hr = swapchain_->Present( 0, DXGI_PRESENT_DO_NOT_WAIT );

	// Release the sync texture once the frame is out (documented order).
	if ( sync_mutex != nullptr )
	{
		if ( sync_acquired )
		{
			sync_mutex->ReleaseSync( 0 );
		}
		sync_mutex->Release();
	}

	if ( FAILED( hr ) && hr != DXGI_ERROR_WAS_STILL_DRAWING )
	{
		// DXGI_ERROR_DEVICE_REMOVED / RESET: drop everything so the next call
		// rebuilds. (Proper handling would recreate the swap textures too.)
		DriverLog( "shiyan_nakedmonitor: [direct] Present failed (0x%08X); resetting output.", static_cast< unsigned >( hr ) );
		Shutdown();
		return;
	}

	const auto present_ms = std::chrono::duration_cast< std::chrono::milliseconds >(
		std::chrono::steady_clock::now() - present_start ).count();
	if ( present_ms > 200 )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] WARNING: Present took %lld ms.", static_cast< long long >( present_ms ) );
	}

	if ( ( ++frame_counter_ % 300 ) == 1 )
	{
		DriverLog( "shiyan_nakedmonitor: [direct] presented frame %llu (%s).",
			static_cast< unsigned long long >( frame_counter_ ),
			( pipeline_ready_ && have_eye_textures_ ) ? "SBS from eye textures" : "solid colour" );
	}
}

void NakedDirectModeComponent::PostPresent( const Throttling_t *pThrottling )
{
	CallbackScope scope( this, "PostPresent" );
	(void)pThrottling;

	// Frame pacing lives here (Present is non-blocking). Bounded sleep so an
	// occluded/stalled output window can never block the compositor for long.
	if ( !vsync_ )
	{
		return;
	}

	const double hz = ( display_frequency_ > 1.0f ) ? static_cast< double >( display_frequency_ ) : 60.0;
	const auto frame_interval = std::chrono::microseconds( static_cast< long long >( 1000000.0 / hz ) );
	const auto now = std::chrono::steady_clock::now();

	if ( next_frame_time_.time_since_epoch().count() == 0 || next_frame_time_ < now )
	{
		next_frame_time_ = now + frame_interval;
	}
	else
	{
		next_frame_time_ += frame_interval;
	}

	const auto wait = next_frame_time_ - now;
	if ( wait > std::chrono::microseconds( 0 ) )
	{
		if ( wait < std::chrono::milliseconds( 100 ) )
		{
			std::this_thread::sleep_for( wait );
		}
		else
		{
			// Fell far behind (e.g. after a stall): resync rather than sleep.
			next_frame_time_ = now + frame_interval;
		}
	}
}

void NakedDirectModeComponent::GetFrameTiming( vr::DriverDirectMode_FrameTiming *pFrameTiming )
{
	if ( pFrameTiming == nullptr )
	{
		return;
	}

	CallbackScope scope( this, "GetFrameTiming" );
	std::lock_guard< std::mutex > lock( mutex_ );

	pFrameTiming->m_nSize = sizeof( vr::DriverDirectMode_FrameTiming );
	pFrameTiming->m_nNumFramePresents = 1;
	pFrameTiming->m_nNumMisPresented = 0;
	pFrameTiming->m_nNumDroppedFrames = 0;
	// Clear the requested-reprojection flags so the compositor does not read
	// them back as throttling settings.
	pFrameTiming->m_nReprojectionFlags = 0;
}
