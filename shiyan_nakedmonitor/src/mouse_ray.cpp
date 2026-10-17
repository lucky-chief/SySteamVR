//============ Shiyan Naked Monitor OpenVR driver ============
#include "mouse_ray.h"

#include "driverlog.h"
#include "eye_tracker.h"
#include "quat_utils.h"
#include "vr_sim.h"

#include <algorithm>
#include <chrono>
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

	// Angular sweep gain: the cursor always maps onto a plane parallel to the
	// physical panel at (0, screen_center_height, -screen_distance), but a
	// scale > 1 makes that virtual aiming plane larger than the panel. The
	// ray's angular sweep is bounded by the panel silhouette seen from the
	// eye (about +/-21 deg horizontal at 0.9 m with the default panel), so
	// values > 1 are the way to reach beyond what is visible on the panel.
	float span_scale = 1.0f;

	// Panel geometry (screen space -> tracking space).
	float screen_width_m = 0.6981f;
	float screen_height_m = 0.3927f;
	float screen_center_height_m = 1.2f;
	float screen_distance_m = 0.5f;

	// Diagnostics: raw cursor + source area as seen by THIS process (the
	// driver runs DPI-aware, so it may differ from other apps' coordinates).
	int last_cursor_x = 0;
	int last_cursor_y = 0;
	int last_area_w = 0;
	int last_area_h = 0;
	bool debug_log = false; // ray_debug_log: log the aim angles (tuning aid)
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

// Presentation window lookup. In indirect mode SteamVR's compositor creates
// the "Headset Window"; in direct mode our own component creates a window
// (class ShiyanNakedMonitorOutput). Without this the window-relative cursor
// source would silently fall back to the vr_sim aim.
HWND FindPresentationWindow()
{
	// Direct mode window first: its class is ours and unambiguous.
	HWND window = FindWindowW( L"ShiyanNakedMonitorOutput", nullptr );
	if ( window != nullptr )
	{
		return window;
	}
	return FindWindowW( nullptr, L"Headset Window" );
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
		g_ray.last_cursor_x = cursor.x;
		g_ray.last_cursor_y = cursor.y;
		g_ray.last_area_w = width;
		g_ray.last_area_h = height;
		return true;
	}

	// --- Window source: cursor inside the presentation window --------------
	HWND window = FindPresentationWindow();
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
	g_ray.last_cursor_x = cursor.x;
	g_ray.last_cursor_y = cursor.y;
	g_ray.last_area_w = static_cast< int >( width );
	g_ray.last_area_h = static_cast< int >( height );

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
	g_ray.span_scale = GetFloatSetting( kRaySection, "ray_span_scale", g_ray.span_scale );
	if ( g_ray.span_scale < 0.1f )
	{
		g_ray.span_scale = 0.1f;
	}
	{
		vr::EVRSettingsError error = vr::VRSettingsError_None;
		g_ray.debug_log = vr::VRSettings()->GetBool( kRaySection, "ray_debug_log", &error );
		if ( error != vr::VRSettingsError_None )
		{
			g_ray.debug_log = false;
		}
	}

	g_ray.screen_width_m = GetFloatSetting( kDisplaySectionForRay, "screen_width_m", g_ray.screen_width_m );
	g_ray.screen_height_m = GetFloatSetting( kDisplaySectionForRay, "screen_height_m", g_ray.screen_height_m );
	g_ray.screen_center_height_m = GetFloatSetting( kDisplaySectionForRay, "screen_center_height_m", g_ray.screen_center_height_m );
	g_ray.screen_distance_m = GetFloatSetting( kDisplaySectionForRay, "screen_distance_m", g_ray.screen_distance_m );

	DriverLog( "shiyan_nakedmonitor: mouse ray (source %s, mapping %s; span x%.2f, origin offset %.2f m forward, %.2f m up).",
		source.c_str(), mapping.c_str(), g_ray.span_scale, g_ray.forward_offset, g_ray.up_offset );
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
	// span_scale grows the aiming plane beyond the physical panel, widening
	// the reachable angular range (1.0 = exact physical panel).
	const float target[ 3 ] = {
		( u - 0.5f ) * g_ray.screen_width_m * g_ray.span_scale,
		( 0.5f - v ) * g_ray.screen_height_m * g_ray.span_scale + g_ray.screen_center_height_m,
		-g_ray.screen_distance_m };

	// Physical centre eye in tracking space (screen-space eye midpoint
	// translated by the panel geometry) and the virtual head pose.
	float eyeLeft[ 3 ] = {};
	float eyeRight[ 3 ] = {};
	EyeTracker::GetCurrentEyes( eyeLeft, eyeRight );
	float walkOffset[ 3 ] = {};
	VrSim::GetHeadOffset( walkOffset );
	float headQuat[ 4 ] = { 0.0f, 0.0f, 0.0f, 1.0f };
	VrSim::GetHeadOrientation( headQuat );

	const float eyeMid[ 3 ] = {
		( eyeLeft[ 0 ] + eyeRight[ 0 ] ) * 0.5f,
		( eyeLeft[ 1 ] + eyeRight[ 1 ] ) * 0.5f + g_ray.screen_center_height_m,
		( eyeLeft[ 2 ] + eyeRight[ 2 ] ) * 0.5f - g_ray.screen_distance_m };

	// --- Ray in the HEAD frame, then composed with the head pose -----------
	// Port of the reference runtime ("ComposePose(headPose, localPose)"): the
	// panel point is expressed relative to the eye, the offsets are applied in
	// that same frame (so "15 cm in front of the eyes" stays in front of the
	// face), and the result is rotated by the head orientation. Turning the
	// head with MMB therefore swings the aim through the whole scene instead
	// of pinning it to the physical panel.
	const float localTarget[ 3 ] = { target[ 0 ] - eyeMid[ 0 ],
									 target[ 1 ] - eyeMid[ 1 ],
									 target[ 2 ] - eyeMid[ 2 ] };
	const float distance = std::sqrt( localTarget[ 0 ] * localTarget[ 0 ]
									  + localTarget[ 1 ] * localTarget[ 1 ]
									  + localTarget[ 2 ] * localTarget[ 2 ] );

	float localOrigin[ 3 ] = { 0.0f, 0.0f, 0.0f };
	if ( distance > 1e-6f )
	{
		float forward = g_ray.forward_offset;
		if ( forward > 0.0f )
		{
			forward = std::min( forward, distance * 0.95f );
		}
		const float scale = forward / distance;
		localOrigin[ 0 ] = localTarget[ 0 ] * scale;
		localOrigin[ 1 ] = localTarget[ 1 ] * scale;
		localOrigin[ 2 ] = localTarget[ 2 ] * scale;
	}
	localOrigin[ 1 ] += g_ray.up_offset;

	const float localDir[ 3 ] = { localTarget[ 0 ] - localOrigin[ 0 ],
								  localTarget[ 1 ] - localOrigin[ 1 ],
								  localTarget[ 2 ] - localOrigin[ 2 ] };

	float dir[ 3 ] = {};
	float rotatedOrigin[ 3 ] = {};
	Quat::RotateVector( headQuat, localDir, dir );
	Quat::RotateVector( headQuat, localOrigin, rotatedOrigin );

	const float origin[ 3 ] = { eyeMid[ 0 ] + walkOffset[ 0 ] + rotatedOrigin[ 0 ],
								eyeMid[ 1 ] + walkOffset[ 1 ] + rotatedOrigin[ 1 ],
								eyeMid[ 2 ] + walkOffset[ 2 ] + rotatedOrigin[ 2 ] };

	// Head orientation (for the diagnostics below).
	float headYaw = 0.0f;
	float headPitch = 0.0f;
	VrSim::GetHeadYawPitch( &headYaw, &headPitch );

	// Diagnostic (rate-limited): the reachable aim range is bounded by the
	// aiming plane seen from the eye; log the angles so the sweep can be
	// measured/tuned without a visual check.
	if ( g_ray.debug_log )
	{
		const float len = std::sqrt( dir[ 0 ] * dir[ 0 ] + dir[ 1 ] * dir[ 1 ] + dir[ 2 ] * dir[ 2 ] );
		if ( len > 1e-6f )
		{
			const float yawDeg = std::atan2( -dir[ 0 ], -dir[ 2 ] ) * 57.29578f;
			const float pitchDeg = std::asin( dir[ 1 ] / len ) * 57.29578f;
			static std::chrono::steady_clock::time_point lastLog{};
			static float lastYaw = 0.0f;
			static float lastPitch = 0.0f;
			static bool loggedOnce = false;
			const auto now = std::chrono::steady_clock::now();
			if ( !loggedOnce
				 || ( now - lastLog >= std::chrono::milliseconds( 250 )
					  && ( std::fabs( yawDeg - lastYaw ) > 0.25f || std::fabs( pitchDeg - lastPitch ) > 0.25f ) ) )
			{
				loggedOnce = true;
				lastLog = now;
				lastYaw = yawDeg;
				lastPitch = pitchDeg;
				DriverLog( "shiyan_nakedmonitor: aim ray (u=%.3f v=%.3f span x%.2f; cursor %d,%d of %dx%d; head yaw %+.1f pitch %+.1f) -> yaw %+.1f deg, pitch %+.1f deg",
					u, v, g_ray.span_scale,
					g_ray.last_cursor_x, g_ray.last_cursor_y, g_ray.last_area_w, g_ray.last_area_h,
					headYaw * 57.29578f, headPitch * 57.29578f,
					yawDeg, pitchDeg );
			}
		}
	}

	QuatFromForward( dir, outOrientation );
	outPosition[ 0 ] = origin[ 0 ];
	outPosition[ 1 ] = origin[ 1 ];
	outPosition[ 2 ] = origin[ 2 ];
	return true;
}

} // namespace MouseRay
