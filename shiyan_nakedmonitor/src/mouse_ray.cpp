//============ Shiyan Naked Monitor OpenVR driver ============
#include "mouse_ray.h"

#include "driverlog.h"
#include "eye_tracker.h"
#include "virtual_head.h"

#include <algorithm>
#include <cmath>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static const char *kRaySection = "shiyan_nakedmonitor_controller";
static const char *kDisplaySectionForRay = "shiyan_nakedmonitor_display";

static float GetFloatSetting( const char *pchSection, const char *pchKey, float flDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const float value = vr::VRSettings()->GetFloat( pchSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : flDefault;
}

static void GetStringSetting( const char *pchSection, const char *pchKey, const char *pchDefault, std::string &out )
{
	char buffer[ 1024 ] = {};
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	vr::VRSettings()->GetString( pchSection, pchKey, buffer, sizeof( buffer ), &error );
	out = ( error == vr::VRSettingsError_None ) ? buffer : pchDefault;
}

namespace
{

// Cursor -> panel mapping:
//   half_modulo    - each half of the SBS window is the same panel image;
//                    (x mod halfWidth) / halfWidth. WYSIWYG in both halves.
//   whole_window   - the full window width maps linearly to the panel (the
//                    OpenXR runtime's default).
//   left_half_clamp- left half linear, clamped at its right edge (the
//                    OpenXR runtime's "half-screen wrap" variant).
enum class CursorMapping
{
	HalfModulo,
	WholeWindow,
	LeftHalfClamp
};

// Where the cursor position is read from:
//   screen  - the whole virtual desktop (all monitors). The user's mouse
//             normally lives on the primary monitor, not on the panel's
//             Headset Window, so window-relative mapping would clamp the ray
//             to a panel edge and it would never move. Default.
//   window  - the Headset Window client rect (WYSIWYG under the cursor on
//             the panel), combined with the cursor_mapping option above.
enum class CursorSource
{
	Screen,
	Window
};

struct RayState
{
	bool initialized = false;
	CursorMapping mapping = CursorMapping::HalfModulo;
	CursorSource source = CursorSource::Screen;

	// Ray origin offsets relative to the centre eye (metres). Forward is
	// clamped to stay before the panel hit point; direction is always
	// recomputed towards the hit, so aiming stays exact with any offset.
	float forward_offset = 0.15f;
	float up_offset = 0.0f;

	// Panel geometry (screen space -> tracking space).
	float screen_width_m = 0.6981f;
	float screen_height_m = 0.3927f;
	float screen_center_height_m = 1.2f;
	float screen_distance_m = 0.5f;
};

RayState g_ray;

// -Z towards dir (shortest arc, up stays level) - port of QuatFromForward.
void QuatFromForward( const float dir[ 3 ], float outQuat[ 4 ] )
{
	const float len = std::sqrt( dir[ 0 ] * dir[ 0 ] + dir[ 1 ] * dir[ 1 ] + dir[ 2 ] * dir[ 2 ] );
	if ( len < 1e-6f )
	{
		outQuat[ 0 ] = 0.0f;
		outQuat[ 1 ] = 0.0f;
		outQuat[ 2 ] = 0.0f;
		outQuat[ 3 ] = 1.0f;
		return;
	}
	const float fx = dir[ 0 ] / len;
	const float fy = dir[ 1 ] / len;
	const float fz = dir[ 2 ] / len;

	// from = -Z; dot = -fz; axis = from x dir = (-fz*... ) computed explicitly.
	const float dot = std::max( -1.0f, std::min( 1.0f, -fz ) );
	float ax = 0.0f * fz - ( -1.0f ) * fy; // from.y*dir.z - from.z*dir.y = 0*fz - (-1)*fy
	float ay = ( -1.0f ) * fx - 0.0f * fz; // from.z*dir.x - from.x*dir.z
	float az = 0.0f * fy - 0.0f * fx;      // from.x*dir.y - from.y*dir.x
	const float axisLen = std::sqrt( ax * ax + ay * ay + az * az );
	if ( axisLen < 1e-6f )
	{
		if ( dot > 0.0f )
		{
			outQuat[ 0 ] = 0.0f;
			outQuat[ 1 ] = 0.0f;
			outQuat[ 2 ] = 0.0f;
			outQuat[ 3 ] = 1.0f;
		}
		else
		{
			outQuat[ 0 ] = 0.0f;
			outQuat[ 1 ] = 1.0f;
			outQuat[ 2 ] = 0.0f;
			outQuat[ 3 ] = 0.0f; // 180 degrees around +Y
		}
		return;
	}
	const float invAxisLen = 1.0f / axisLen;
	ax *= invAxisLen;
	ay *= invAxisLen;
	az *= invAxisLen;
	const float halfAngle = std::acos( dot ) * 0.5f;
	const float s = std::sin( halfAngle );
	outQuat[ 0 ] = ax * s;
	outQuat[ 1 ] = ay * s;
	outQuat[ 2 ] = az * s;
	outQuat[ 3 ] = std::cos( halfAngle );
}

// Cursor -> panel normalized coordinates (u right, v down). Clamped to the
// chosen source area; mapping selected by the cursor_mapping setting.
bool NormalizeCursorToPanel( float *outU, float *outV )
{
	POINT cursor = {};
	if ( !GetCursorPos( &cursor ) )
	{
		return false;
	}

	if ( g_ray.source == CursorSource::Screen )
	{
		// Primary monitor: the mouse's normal playground, mapped linearly
		// across the full panel (the reference runtime does the same).
		const int width = GetSystemMetrics( SM_CXSCREEN );
		const int height = GetSystemMetrics( SM_CYSCREEN );
		if ( width <= 1 || height <= 1 )
		{
			return false;
		}
		*outU = std::max( 0.0f, std::min( static_cast< float >( cursor.x ) / static_cast< float >( width ), 1.0f ) );
		*outV = std::max( 0.0f, std::min( static_cast< float >( cursor.y ) / static_cast< float >( height ), 1.0f ) );
		return true;
	}

	// --- Window source: cursor inside the compositor Headset Window --------
	HWND window = FindWindowW( nullptr, L"Headset Window" );
	if ( window == nullptr )
	{
		return false;
	}
	if ( !ScreenToClient( window, &cursor ) )
	{
		return false;
	}

	RECT rect = {};
	if ( !GetClientRect( window, &rect ) )
	{
		return false;
	}
	const float width = static_cast< float >( rect.right - rect.left );
	const float height = static_cast< float >( rect.bottom - rect.top );
	if ( width <= 1.0f || height <= 1.0f )
	{
		return false;
	}

	float x = std::max( 0.0f, std::min( static_cast< float >( cursor.x ), width - 1.0f ) );
	const float y = std::max( 0.0f, std::min( static_cast< float >( cursor.y ), height - 1.0f ) );

	switch ( g_ray.mapping )
	{
	case CursorMapping::HalfModulo:
	{
		// Each SBS half is the same panel image; the visual content jumps at
		// the half boundary and the ray target jumps with it - still WYSIWYG.
		const float halfWidth = width * 0.5f;
		x = std::fmod( x, halfWidth );
		*outU = x / halfWidth;
		break;
	}
	case CursorMapping::LeftHalfClamp:
	{
		const float halfWidth = width * 0.5f;
		*outU = std::max( 0.0f, std::min( x / halfWidth, 1.0f ) );
		break;
	}
	case CursorMapping::WholeWindow:
	default:
		*outU = x / width;
		break;
	}
	*outV = y / height;
	return true;
}

} // namespace

namespace MouseRay
{

void Init()
{
	if ( g_ray.initialized )
	{
		return;
	}
	g_ray.initialized = true;

	std::string mapping;
	GetStringSetting( kRaySection, "cursor_mapping", "half_modulo", mapping );
	if ( mapping == "whole_window" )
	{
		g_ray.mapping = CursorMapping::WholeWindow;
	}
	else if ( mapping == "left_half_clamp" )
	{
		g_ray.mapping = CursorMapping::LeftHalfClamp;
	}
	else
	{
		g_ray.mapping = CursorMapping::HalfModulo;
	}

	// Default "screen": the mouse normally lives on the primary monitor, so
	// window-relative mapping would clamp the ray to a panel edge.
	std::string source;
	GetStringSetting( kRaySection, "cursor_source", "screen", source );
	g_ray.source = ( source == "window" ) ? CursorSource::Window : CursorSource::Screen;

	g_ray.forward_offset = GetFloatSetting( kRaySection, "ray_origin_forward_m", g_ray.forward_offset );
	g_ray.up_offset = GetFloatSetting( kRaySection, "ray_origin_up_m", g_ray.up_offset );

	g_ray.screen_width_m = GetFloatSetting( kDisplaySectionForRay, "screen_width_m", g_ray.screen_width_m );
	g_ray.screen_height_m = GetFloatSetting( kDisplaySectionForRay, "screen_height_m", g_ray.screen_height_m );
	g_ray.screen_center_height_m = GetFloatSetting( kDisplaySectionForRay, "screen_center_height_m", g_ray.screen_center_height_m );
	g_ray.screen_distance_m = GetFloatSetting( kDisplaySectionForRay, "screen_distance_m", g_ray.screen_distance_m );

	DriverLog( "shiyan_nakedmonitor: mouse ray (source %s, mapping %s; origin offset %.2f m forward, %.2f m up).",
		source.c_str(), mapping.c_str(), g_ray.forward_offset, g_ray.up_offset );
}

bool ComputeAimPose( float outPosition[ 3 ], float outOrientation[ 4 ] )
{
	if ( !g_ray.initialized )
	{
		Init();
	}

	float u = 0.5f;
	float v = 0.5f;
	if ( !NormalizeCursorToPanel( &u, &v ) )
	{
		return false;
	}

	// Panel hit point in tracking space (panel centre at
	// (0, screen_center_height, -screen_distance), +z toward the viewer).
	const float target[ 3 ] = {
		( u - 0.5f ) * g_ray.screen_width_m,
		( 0.5f - v ) * g_ray.screen_height_m + g_ray.screen_center_height_m,
		-g_ray.screen_distance_m };

	// Centre eye in tracking space (screen-space eye midpoint translated by
	// the panel geometry, plus the virtual head's walk offset).
	float eyeLeft[ 3 ] = {};
	float eyeRight[ 3 ] = {};
	EyeTracker::GetCurrentEyes( eyeLeft, eyeRight );
	float walkOffset[ 3 ] = {};
	VirtualHead::GetPositionOffset( walkOffset );
	const float eyeMid[ 3 ] = {
		( eyeLeft[ 0 ] + eyeRight[ 0 ] ) * 0.5f,
		( eyeLeft[ 1 ] + eyeRight[ 1 ] ) * 0.5f + g_ray.screen_center_height_m + walkOffset[ 1 ],
		( eyeLeft[ 2 ] + eyeRight[ 2 ] ) * 0.5f - g_ray.screen_distance_m + walkOffset[ 2 ] };
	// Note: walkOffset.x folds into the eye midpoint x as well.
	const float originBase[ 3 ] = { eyeMid[ 0 ] + walkOffset[ 0 ], eyeMid[ 1 ], eyeMid[ 2 ] };

	float dirToTarget[ 3 ] = { target[ 0 ] - originBase[ 0 ],
							   target[ 1 ] - originBase[ 1 ],
							   target[ 2 ] - originBase[ 2 ] };
	const float distance = std::sqrt( dirToTarget[ 0 ] * dirToTarget[ 0 ]
									  + dirToTarget[ 1 ] * dirToTarget[ 1 ]
									  + dirToTarget[ 2 ] * dirToTarget[ 2 ] );

	// Origin offsets: forward along the ray (never past the hit point) and up
	// along tracking +Y. Direction is recomputed towards the hit below, so the
	// ray still lands exactly on the cursor's target.
	float origin[ 3 ] = { originBase[ 0 ], originBase[ 1 ], originBase[ 2 ] };
	float forward = g_ray.forward_offset;
	if ( distance > 1e-6f )
	{
		if ( forward > 0.0f )
		{
			forward = std::min( forward, distance * 0.95f );
		}
		const float scale = forward / distance;
		origin[ 0 ] += dirToTarget[ 0 ] * scale;
		origin[ 1 ] += dirToTarget[ 1 ] * scale;
		origin[ 2 ] += dirToTarget[ 2 ] * scale;
	}
	origin[ 1 ] += g_ray.up_offset;

	const float dir[ 3 ] = { target[ 0 ] - origin[ 0 ], target[ 1 ] - origin[ 1 ], target[ 2 ] - origin[ 2 ] };
	QuatFromForward( dir, outOrientation );
	outPosition[ 0 ] = origin[ 0 ];
	outPosition[ 1 ] = origin[ 1 ];
	outPosition[ 2 ] = origin[ 2 ];
	return true;
}

} // namespace MouseRay
