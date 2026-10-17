//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

//-----------------------------------------------------------------------------
// Key-hint overlay (direct mode only).
//
// A list of PNG pages (one image per page) shown on top of the SBS output.
// V shows/hides it; while visible, A/D and Left/Right page through the images
// (wrapping). Pages are decoded with WIC and uploaded once, then drawn as a
// centered, aspect-fit quad in each eye half.
//
// Threading: all methods are expected to be called with the owning direct-mode
// component's mutex held.
//-----------------------------------------------------------------------------
class HelpOverlay
{
public:
	void Init(); // reads the "shiyan_nakedmonitor_help" settings (idempotent)

	bool IsEnabled() const { return enabled_; }
	bool IsVisible() const { return visible_; }
	void Toggle() { visible_ = enabled_ && !image_names_.empty() ? !visible_ : false; }
	void NextPage( int delta );
	size_t PageCount() const { return image_names_.size(); }
	size_t CurrentPage() const { return current_page_; }
	float WidthRatio() const { return width_ratio_; }

	// Lazily decodes the pages and uploads them to the device. `srgb` selects the
	// texture format that matches the backbuffer. Returns false when disabled or
	// when no page could be loaded.
	bool EnsureTextures( ID3D11Device *device, bool srgb );
	void ReleaseTextures();

	// Valid after EnsureTextures().
	bool GetPage( size_t page, float *outWidth, float *outHeight,
		ID3D11ShaderResourceView **outSrv );

private:
	struct Page
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> bgra; // kept for re-upload after a device reset
		ID3D11Texture2D *texture = nullptr;
		ID3D11ShaderResourceView *srv = nullptr;
	};

	bool ReadSettings();
	bool DecodePng( const std::wstring &path, Page &page );
	static std::wstring ImageDirectory();

	bool initialized_ = false;
	bool enabled_ = false;
	float width_ratio_ = 0.9f;
	std::vector<std::string> image_names_;
	std::vector<Page> pages_;
	bool textures_ready_ = false;
	bool visible_ = false;
	size_t current_page_ = 0;
};
