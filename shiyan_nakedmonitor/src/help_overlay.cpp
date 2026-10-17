//============ Shiyan Naked Monitor OpenVR driver ============
#include "help_overlay.h"

#include "driverlog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <d3d11.h>

static const char *kHelpSection = "shiyan_nakedmonitor_help";

namespace
{

const char *kDefaultImages = "help_1.png";

template < typename T >
void SafeRelease( T *&p )
{
	if ( p != nullptr )
	{
		p->Release();
		p = nullptr;
	}
}

std::string Trim( const std::string &value )
{
	size_t begin = 0;
	size_t end = value.size();
	while ( begin < end && std::isspace( static_cast< unsigned char >( value[ begin ] ) ) )
	{
		++begin;
	}
	while ( end > begin && std::isspace( static_cast< unsigned char >( value[ end - 1 ] ) ) )
	{
		--end;
	}
	return value.substr( begin, end - begin );
}

std::wstring Widen( const std::string &value )
{
	if ( value.empty() )
	{
		return std::wstring();
	}
	const int length = MultiByteToWideChar( CP_UTF8, 0, value.c_str(), -1, nullptr, 0 );
	if ( length <= 0 )
	{
		return std::wstring();
	}
	std::wstring wide( static_cast< size_t >( length ), L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, value.c_str(), -1, &wide[ 0 ], length );
	wide.pop_back(); // drop the terminator
	return wide;
}

bool GetBoolSetting( const char *pchKey, bool bDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const bool value = vr::VRSettings()->GetBool( kHelpSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : bDefault;
}

float GetFloatSetting( const char *pchKey, float flDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const float value = vr::VRSettings()->GetFloat( kHelpSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : flDefault;
}

void GetStringSetting( const char *pchKey, const char *pchDefault, std::string &out )
{
	char buffer[ 2048 ] = {};
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	vr::VRSettings()->GetString( kHelpSection, pchKey, buffer, sizeof( buffer ), &error );
	out = ( error == vr::VRSettingsError_None ) ? buffer : pchDefault;
}

} // namespace

std::wstring HelpOverlay::ImageDirectory()
{
	// GetModuleFileNameW(nullptr) returns the HOST process (vrserver.exe), not
	// this DLL, so resolve our own module from a function address first.
	HMODULE module = nullptr;
	if ( !GetModuleHandleExW(
			 GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			 reinterpret_cast< LPCWSTR >( &HelpOverlay::ImageDirectory ), &module )
		 || module == nullptr )
	{
		return std::wstring();
	}

	wchar_t modulePath[ MAX_PATH ] = {};
	const DWORD length = GetModuleFileNameW( module, modulePath, MAX_PATH );
	if ( length == 0 || length >= MAX_PATH )
	{
		return std::wstring();
	}

	// Strip the file name: .../bin/win64/driver_....dll
	for ( DWORD i = length; i > 0; --i )
	{
		if ( modulePath[ i - 1 ] == L'\\' || modulePath[ i - 1 ] == L'/' )
		{
			modulePath[ i - 1 ] = L'\0';
			break;
		}
	}

	// .../bin/win64 -> .../resources/images
	std::wstring directory( modulePath );
	directory += L"\\..\\..\\resources\\images\\";
	return directory;
}

void HelpOverlay::Init()
{
	if ( initialized_ )
	{
		return;
	}
	initialized_ = true;
	ReadSettings();
}

bool HelpOverlay::ReadSettings()
{
	enabled_ = GetBoolSetting( "enabled", false );
	width_ratio_ = GetFloatSetting( "width_ratio", width_ratio_ );
	width_ratio_ = std::max( 0.1f, std::min( 1.0f, width_ratio_ ) );

	std::string images;
	GetStringSetting( "images", kDefaultImages, images );

	image_names_.clear();
	size_t start = 0;
	while ( start <= images.size() )
	{
		const size_t comma = images.find( ',', start );
		const std::string item = Trim( images.substr( start,
			( comma == std::string::npos ) ? std::string::npos : comma - start ) );
		if ( !item.empty() )
		{
			image_names_.push_back( item );
		}
		if ( comma == std::string::npos )
		{
			break;
		}
		start = comma + 1;
	}

	current_page_ = 0;
	if ( !enabled_ || image_names_.empty() )
	{
		return false;
	}

	DriverLog( "shiyan_nakedmonitor: help overlay: %zu page(s), width %.0f%% of a half screen.",
		image_names_.size(), width_ratio_ * 100.0f );
	return true;
}

void HelpOverlay::NextPage( int delta )
{
	if ( !visible_ || image_names_.empty() || delta == 0 )
	{
		return;
	}
	const int count = static_cast< int >( image_names_.size() );
	int next = static_cast< int >( current_page_ ) + delta;
	next %= count;
	if ( next < 0 )
	{
		next += count;
	}
	current_page_ = static_cast< size_t >( next );
}

bool HelpOverlay::DecodePng( const std::wstring &path, Page &page )
{
	IWICImagingFactory *factory = nullptr;
	IWICBitmapDecoder *decoder = nullptr;
	IWICBitmapFrameDecode *frame = nullptr;
	IWICFormatConverter *converter = nullptr;
	bool ok = false;

	HRESULT hr = CoCreateInstance( CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS( &factory ) );
	if ( FAILED( hr ) || factory == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: help overlay: CoCreateInstance(WIC) failed (0x%08X).",
			static_cast< unsigned >( hr ) );
		return false;
	}

	hr = factory->CreateDecoderFromFilename( path.c_str(), nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnLoad, &decoder );
	if ( FAILED( hr ) || decoder == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: help overlay: cannot open '%ls' (0x%08X).",
			path.c_str(), static_cast< unsigned >( hr ) );
	}
	else if ( FAILED( decoder->GetFrame( 0, &frame ) ) || frame == nullptr )
	{
		DriverLog( "shiyan_nakedmonitor: help overlay: cannot read the first frame of '%ls'.",
			path.c_str() );
	}
	else if ( FAILED( factory->CreateFormatConverter( &converter ) ) || converter == nullptr
			  || FAILED( converter->Initialize( frame, GUID_WICPixelFormat32bppBGRA,
					 WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom ) ) )
	{
		DriverLog( "shiyan_nakedmonitor: help overlay: cannot convert '%ls' to BGRA.", path.c_str() );
	}
	else
	{
		UINT width = 0;
		UINT height = 0;
		converter->GetSize( &width, &height );
		if ( width == 0 || height == 0 )
		{
			DriverLog( "shiyan_nakedmonitor: help overlay: '%ls' has an empty size.", path.c_str() );
		}
		else
		{
			page.width = width;
			page.height = height;
			page.bgra.resize( static_cast< size_t >( width ) * height * 4 );
			const HRESULT copy = converter->CopyPixels( nullptr, width * 4,
				static_cast< UINT >( page.bgra.size() ), page.bgra.data() );
			if ( FAILED( copy ) )
			{
				DriverLog( "shiyan_nakedmonitor: help overlay: CopyPixels failed for '%ls' (0x%08X).",
					path.c_str(), static_cast< unsigned >( copy ) );
				page.bgra.clear();
			}
			else
			{
				ok = true;
			}
		}
	}

	SafeRelease( converter );
	SafeRelease( frame );
	SafeRelease( decoder );
	SafeRelease( factory );
	return ok;
}

bool HelpOverlay::EnsureTextures( ID3D11Device *device, bool srgb )
{
	Init();
	if ( !enabled_ || image_names_.empty() || device == nullptr )
	{
		return false;
	}
	if ( textures_ready_ )
	{
		return !pages_.empty();
	}

	// WIC needs COM on this thread. The driver never calls CoUninitialize: the
	// process is short lived and the call is harmless (RPC_E_CHANGED_MODE just
	// means the thread already has COM).
	CoInitializeEx( nullptr, COINIT_MULTITHREADED );

	const std::wstring directory = ImageDirectory();
	pages_.assign( image_names_.size(), Page{} );

	bool any = false;
	for ( size_t i = 0; i < image_names_.size(); ++i )
	{
		Page &page = pages_[ i ];
		if ( !DecodePng( directory + Widen( image_names_[ i ] ), page ) )
		{
			continue;
		}

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = page.width;
		td.Height = page.height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA data = {};
		data.pSysMem = page.bgra.data();
		data.SysMemPitch = page.width * 4;

		const HRESULT hr = device->CreateTexture2D( &td, &data, &page.texture );
		if ( FAILED( hr ) || page.texture == nullptr )
		{
			DriverLog( "shiyan_nakedmonitor: help overlay: CreateTexture2D failed (0x%08X).",
				static_cast< unsigned >( hr ) );
			SafeRelease( page.texture );
			continue;
		}
		if ( FAILED( device->CreateShaderResourceView( page.texture, nullptr, &page.srv ) )
			 || page.srv == nullptr )
		{
			DriverLog( "shiyan_nakedmonitor: help overlay: CreateShaderResourceView failed." );
			SafeRelease( page.texture );
			continue;
		}

		DriverLog( "shiyan_nakedmonitor: help overlay: loaded '%s' (%ux%u).",
			image_names_[ i ].c_str(), page.width, page.height );
		any = true;
	}

	textures_ready_ = true;
	if ( !any )
	{
		DriverLog( "shiyan_nakedmonitor: help overlay: no page could be loaded; disabling." );
	}
	return any;
}

void HelpOverlay::ReleaseTextures()
{
	for ( Page &page : pages_ )
	{
		SafeRelease( page.srv );
		SafeRelease( page.texture );
	}
	pages_.clear();
	textures_ready_ = false;
}

bool HelpOverlay::GetPage( size_t page, float *outWidth, float *outHeight,
	ID3D11ShaderResourceView **outSrv )
{
	if ( pages_.empty() || page >= pages_.size() || pages_[ page ].srv == nullptr )
	{
		return false;
	}
	if ( outWidth != nullptr )
	{
		*outWidth = static_cast< float >( pages_[ page ].width );
	}
	if ( outHeight != nullptr )
	{
		*outHeight = static_cast< float >( pages_[ page ].height );
	}
	if ( outSrv != nullptr )
	{
		*outSrv = pages_[ page ].srv;
	}
	return true;
}
