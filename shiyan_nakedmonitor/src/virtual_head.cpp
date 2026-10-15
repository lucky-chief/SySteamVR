//============ Shiyan Naked Monitor OpenVR driver ============
#include "virtual_head.h"

#include "driverlog.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static const char *kHeadSection = "shiyan_nakedmonitor_controller";

// Reference values ported from SyOpenXRRuntime/src/xr_input.cpp.
static constexpr float kHeadMaxPitch = 1.309f;  // ±75 degrees
static constexpr float kCursorRecenterMargin = 0.1f;
static constexpr float kDefaultHeadMoveSpeed = 0.8f; // m/s

static float GetFloatSetting( const char *pchSection, const char *pchKey, float flDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const float value = vr::VRSettings()->GetFloat( pchSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : flDefault;
}

static bool GetBoolSetting( const char *pchSection, const char *pchKey, bool bDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const bool value = vr::VRSettings()->GetBool( pchSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : bDefault;
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

struct HeadState
{
	bool initialized = false;

	bool natural_mouse = true;

	// Look (MMB + mouse), radians. Positive yaw = turn left, positive pitch = up.
	float yaw = 0.0f;
	float pitch = 0.0f;
	// Radians of head rotation per mouse pixel (half screen ~ 150 degrees at
	// the reference value 0.0025 on a 1920-wide screen).
	float look_sensitivity = 0.0025f;

	// Walk offset applied on top of the seated base pose, tracking space.
	float offset_x = 0.0f;
	float offset_y = 0.0f;
	float offset_z = 0.0f;
	float move_speed = kDefaultHeadMoveSpeed; // m/s

	bool cursor_recenter = true;
	// Swallow mouse deltas for N frames after an automatic cursor recenter:
	// SetCursorPos can synthesize mouse movement that raw input also sees.
	int swallow_frames = 0;

	// Edge detection for MMB / G.
	bool last_mmb_down = false;
	bool last_g_down = false;

	std::chrono::steady_clock::time_point last_time{};
};

HeadState g_head;

void RecenterCursorIfNearEdge()
{
	const int screenW = GetSystemMetrics( SM_CXSCREEN );
	const int screenH = GetSystemMetrics( SM_CYSCREEN );
	if ( screenW <= 0 || screenH <= 0 )
	{
		return;
	}

	POINT cursor = {};
	if ( !GetCursorPos( &cursor ) )
	{
		return;
	}

	const float marginX = static_cast< float >( screenW ) * kCursorRecenterMargin;
	const float marginY = static_cast< float >( screenH ) * kCursorRecenterMargin;
	if ( static_cast< float >( cursor.x ) < marginX
		 || static_cast< float >( cursor.x ) > static_cast< float >( screenW ) - marginX
		 || static_cast< float >( cursor.y ) < marginY
		 || static_cast< float >( cursor.y ) > static_cast< float >( screenH ) - marginY )
	{
		if ( SetCursorPos( screenW / 2, screenH / 2 ) )
		{
			// Ignore the synthetic movement raw input may deliver for the jump.
			g_head.swallow_frames = 2;
		}
	}
}

} // namespace

namespace VirtualHead
{

void Init()
{
	if ( g_head.initialized )
	{
		return;
	}
	g_head.initialized = true;

	std::string mode;
	GetStringSetting( kHeadSection, "interaction_mode", "natural_mouse", mode );
	g_head.natural_mouse = !( mode == "vr_sim" );

	g_head.look_sensitivity = GetFloatSetting( kHeadSection, "head_look_sensitivity", g_head.look_sensitivity );
	g_head.move_speed = GetFloatSetting( kHeadSection, "head_move_speed_mps", g_head.move_speed );
	g_head.cursor_recenter = GetBoolSetting( kHeadSection, "cursor_recenter", g_head.cursor_recenter );

	DriverLog( "shiyan_nakedmonitor: virtual head (mode %s; look %.5f rad/px, walk %.2f m/s, recenter %s).",
		g_head.natural_mouse ? "natural_mouse" : "vr_sim",
		g_head.look_sensitivity, g_head.move_speed,
		g_head.cursor_recenter ? "on" : "off" );
}

void Update( RawInput::State &input )
{
	if ( !g_head.initialized )
	{
		Init();
	}

	const auto now = std::chrono::steady_clock::now();
	float dt = 0.0f;
	if ( g_head.last_time != std::chrono::steady_clock::time_point{} )
	{
		dt = std::chrono::duration_cast< std::chrono::duration< float > >( now - g_head.last_time ).count();
		dt = std::max( 0.0f, std::min( dt, 0.1f ) ); // no teleports after a stall
	}
	g_head.last_time = now;

	if ( g_head.swallow_frames > 0 )
	{
		--g_head.swallow_frames;
		input.dx = 0;
		input.dy = 0;
	}

	// --- G: toggle interaction mode ---------------------------------------
	const bool g_down = input.keys[ 'G' ];
	if ( g_down && !g_head.last_g_down )
	{
		g_head.natural_mouse = !g_head.natural_mouse;
		DriverLog( "shiyan_nakedmonitor: interaction mode -> %s",
			g_head.natural_mouse ? "natural_mouse (cursor = panel pointer)"
								 : "vr_sim (mouse steers the aim angle)" );
	}
	g_head.last_g_down = g_down;

	// --- MMB held: incremental look + edge cursor recenter -----------------
	const bool mmb_down = input.middle;
	const bool looking = mmb_down;
	if ( looking )
	{
		// Tracking space: positive yaw = turn left => mouse right (dx>0) turns
		// right => subtract. Positive pitch = up => mouse down (dy>0) looks
		// down => subtract.
		g_head.yaw -= static_cast< float >( input.dx ) * g_head.look_sensitivity;
		g_head.pitch -= static_cast< float >( input.dy ) * g_head.look_sensitivity;
		g_head.pitch = std::max( -kHeadMaxPitch, std::min( kHeadMaxPitch, g_head.pitch ) );
		while ( g_head.yaw > 3.14159265f )
		{
			g_head.yaw -= 2.0f * 3.14159265f;
		}
		while ( g_head.yaw < -3.14159265f )
		{
			g_head.yaw += 2.0f * 3.14159265f;
		}

		// Keep the cursor from getting stuck at the screen edge: snap it back
		// to the centre and ignore the movement that jump generates.
		if ( g_head.cursor_recenter )
		{
			RecenterCursorIfNearEdge();
		}

		// Mouse deltas were consumed by the head; the controller must not use
		// them for its own aim this frame.
		input.dx = 0;
		input.dy = 0;
	}
	g_head.last_mmb_down = mmb_down;

	// --- WASD/QE: walk in the yaw plane ------------------------------------
	const float speedScale = input.keys[ VK_SHIFT ] ? 3.0f : 1.0f;
	const float step = g_head.move_speed * speedScale * dt;
	const bool fwd = input.keys[ 'W' ];
	const bool back = input.keys[ 'S' ];
	const bool right = input.keys[ 'D' ];
	const bool left = input.keys[ 'A' ];
	const bool down = input.keys[ 'Q' ];
	const bool up = input.keys[ 'E' ];

	// Forward/right on the horizontal plane of the current yaw:
	//   forward = Ry(yaw) * (0, 0, -1) = (-sin yaw, 0, -cos yaw)
	//   right   = Ry(yaw) * (1,  0,  0) = ( cos yaw, 0, -sin yaw)
	const float sinYaw = std::sin( g_head.yaw );
	const float cosYaw = std::cos( g_head.yaw );
	const float moveF = ( fwd ? 1.0f : 0.0f ) - ( back ? 1.0f : 0.0f );
	const float moveR = ( right ? 1.0f : 0.0f ) - ( left ? 1.0f : 0.0f );
	g_head.offset_x += ( -sinYaw * moveF + cosYaw * moveR ) * step;
	g_head.offset_z += ( -cosYaw * moveF - sinYaw * moveR ) * step;
	if ( down )
	{
		g_head.offset_y -= step;
	}
	if ( up )
	{
		g_head.offset_y += step;
	}
}

void GetYawPitch( float *outYaw, float *outPitch )
{
	if ( outYaw != nullptr )
	{
		*outYaw = g_head.yaw;
	}
	if ( outPitch != nullptr )
	{
		*outPitch = g_head.pitch;
	}
}

void GetPositionOffset( float out[ 3 ] )
{
	if ( out != nullptr )
	{
		out[ 0 ] = g_head.offset_x;
		out[ 1 ] = g_head.offset_y;
		out[ 2 ] = g_head.offset_z;
	}
}

bool IsNaturalMouse()
{
	return g_head.natural_mouse;
}

} // namespace VirtualHead
