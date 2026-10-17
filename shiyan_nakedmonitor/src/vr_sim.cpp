//============ Shiyan Naked Monitor OpenVR driver ============
#include "vr_sim.h"

#include "driverlog.h"
#include "mouse_ray.h"
#include "quat_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static const char *kSimSection = "shiyan_nakedmonitor_controller";

// Reference values from SyOpenXRRuntime/src/xr_input.cpp.
static constexpr float kHeadMaxYaw = 2.618f;   // +-150 deg
static constexpr float kHeadMaxPitch = 1.309f; // +-75 deg
static constexpr float kHandMaxYaw = 2.0f;     // +-~115 deg
static constexpr float kHandMaxPitch = 1.0f;   // +-~57 deg

static constexpr float kHeadMoveSpeed = 0.8f; // m/s
static constexpr float kHandMoveSpeed = 0.6f; // m/s
static constexpr float kWheelMoveStep = 0.1f; // m per notch (hand target)
static constexpr float kMouseTranslateMetersPerScreen = 2.0f;
static constexpr float kWheelStickPulseSeconds = 0.15f;

// Gamepad B: held longer than this = 3x speed, shorter = secondary face click.
static constexpr float kGamepadLongPressSeconds = 0.30f;
static constexpr float kGamepadTapPulseSeconds = 0.12f;

static constexpr float kHandResetDistance = 1.0f;
static constexpr float kHandResetHalfSpacing = 0.25f;

static constexpr float kCursorRecenterMargin = 0.1f;

static float GetFloatSetting( const char *pchKey, float flDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const float value = vr::VRSettings()->GetFloat( kSimSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : flDefault;
}

static bool GetBoolSetting( const char *pchKey, bool bDefault )
{
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	const bool value = vr::VRSettings()->GetBool( kSimSection, pchKey, &error );
	return ( error == vr::VRSettingsError_None ) ? value : bDefault;
}

static void GetStringSetting( const char *pchKey, const char *pchDefault, std::string &out )
{
	char buffer[ 1024 ] = {};
	vr::EVRSettingsError error = vr::VRSettingsError_None;
	vr::VRSettings()->GetString( kSimSection, pchKey, buffer, sizeof( buffer ), &error );
	out = ( error == vr::VRSettingsError_None ) ? buffer : pchDefault;
}

namespace
{

struct SimState
{
	bool initialized = false;

	bool natural_mouse = false;
	VrSim::Target target = VrSim::Target::Head;

	// Head (walk offset on top of the seated base).
	float head_yaw = 0.0f;
	float head_pitch = 0.0f;
	float head_offset[ 3 ] = {};

	// Hands: absolute in tracking space, relative to the seated base on y.
	float hand_yaw[ 2 ] = { 0.0f, 0.0f };
	float hand_pitch[ 2 ] = { 0.0f, 0.0f };
	float hand_pos[ 2 ][ 3 ] = { { -kHandResetHalfSpacing, -0.25f, -0.35f },
		{ kHandResetHalfSpacing, -0.25f, -0.35f } };

	// natural_mouse only: extra aim offset on top of the mouse ray
	// (gamepad LB + right stick rotates the primary hand).
	float hand_ray_yaw[ 2 ] = { 0.0f, 0.0f };
	float hand_ray_pitch[ 2 ] = { 0.0f, 0.0f };

	VrSim::HandInput hand_input[ 2 ];

	bool mouse_translate = false;
	bool cursor_recenter = true;
	bool wheel_invert = false;
	bool head_look_invert = true;

	float head_look_gain = 1.0f;
	float head_move_gain = 1.0f;
	float hand_look_gain = 1.0f;
	float hand_move_gain = 1.0f;

	// Right stick look rates (radians/second).
	float stick_yaw_rate = 2.27f;   // 130 deg/s
	float stick_pitch_rate = 1.40f; // 80 deg/s

	int primary_hand = 1; // natural_mouse: buttons go here (0 = left, 1 = right)

	float wheel_stick_seconds = 0.0f;
	float wheel_stick_sign = 0.0f;

	// Z reset: hands are placed this many metres in front of the head
	// ("hand_reset_distance_m") and this many metres above/below it
	// ("hand_reset_height_offset_m", negative = lower).
	float hand_reset_distance = kHandResetDistance;
	float hand_reset_height_offset = -0.3f;

	// Edge detection.
	bool last_g = false;
	bool last_1 = false;
	bool last_2 = false;
	bool last_3 = false;
	bool last_tab = false;
	bool last_f = false;
	bool last_z = false;
	bool last_pad_mode = false;
	bool last_pad_reset = false;
	bool last_pad_prev = false;
	bool last_pad_next = false;

	// Gamepad B: short tap sends the secondary face button, a long hold is the
	// 3x speed modifier (see kGamepadLongPressSeconds).
	bool pad_b_down = false;
	bool pad_b_long = false;
	float pad_b_tap_pulse = 0.0f;
	std::chrono::steady_clock::time_point pad_b_press{};
	bool target_logged = false;

	// SetCursorPos after an edge recenter synthesizes mouse movement that raw
	// input also sees; swallow it for a couple of frames.
	int swallow_frames = 0;

	std::chrono::steady_clock::time_point last_time{};
};

SimState g_sim;
VrSim::GamepadInput g_gamepad;
float g_head_base_height = 1.5f;

float WrapPi( float angle )
{
	while ( angle > 3.14159265f )
	{
		angle -= 2.0f * 3.14159265f;
	}
	while ( angle < -3.14159265f )
	{
		angle += 2.0f * 3.14159265f;
	}
	return angle;
}

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
			g_sim.swallow_frames = 2;
		}
	}
}

// Move `position` horizontally in the head's yaw plane (view space).
// forward/right are -1 .. 1 so the analog stick and the digital keys share
// the same code path.
void MoveInViewSpace( float position[ 3 ], float headYaw, float step, float forward, float right )
{
	const float sinYaw = std::sin( headYaw );
	const float cosYaw = std::cos( headYaw );
	const float forwardX = -sinYaw;
	const float forwardZ = -cosYaw;
	const float rightX = cosYaw;
	const float rightZ = -sinYaw;

	position[ 0 ] += ( forwardX * forward + rightX * right ) * step;
	position[ 2 ] += ( forwardZ * forward + rightZ * right ) * step;
}

} // namespace

namespace VrSim
{

const char *TargetName( Target target )
{
	switch ( target )
	{
	case Target::LeftHand: return "left hand";
	case Target::RightHand: return "right hand";
	default: return "head";
	}
}

void Init()
{
	if ( g_sim.initialized )
	{
		return;
	}
	g_sim.initialized = true;

	std::string mode;
	GetStringSetting( "interaction_mode", "vr_sim", mode );
	g_sim.natural_mouse = ( mode == "natural_mouse" );

	std::string primary;
	GetStringSetting( "primary_hand", "right", primary );
	g_sim.primary_hand = ( primary == "left" ) ? 0 : 1;

	g_sim.cursor_recenter = GetBoolSetting( "cursor_recenter", g_sim.cursor_recenter );
	g_sim.wheel_invert = GetBoolSetting( "wheel_invert", g_sim.wheel_invert );
	g_sim.head_look_invert = GetBoolSetting( "head_look_invert", g_sim.head_look_invert );

	g_sim.head_look_gain = GetFloatSetting( "sim_head_look", g_sim.head_look_gain );
	g_sim.head_move_gain = GetFloatSetting( "sim_head_move", g_sim.head_move_gain );
	g_sim.hand_look_gain = GetFloatSetting( "sim_hand_look", g_sim.hand_look_gain );
	g_sim.hand_move_gain = GetFloatSetting( "sim_hand_move", g_sim.hand_move_gain );

	// Stick look speed in degrees/second (clamped to 10 .. 360).
	float stickYawDeg = GetFloatSetting( "sim_stick_look_yaw", 130.0f );
	float stickPitchDeg = GetFloatSetting( "sim_stick_look_pitch", 80.0f );
	stickYawDeg = std::max( 10.0f, std::min( 360.0f, stickYawDeg ) );
	stickPitchDeg = std::max( 10.0f, std::min( 360.0f, stickPitchDeg ) );
	g_sim.stick_yaw_rate = stickYawDeg * 0.0174532925f;
	g_sim.stick_pitch_rate = stickPitchDeg * 0.0174532925f;

	// Z reset distance: keep it in a sane range (0.1 .. 3 m).
	g_sim.hand_reset_distance = GetFloatSetting( "hand_reset_distance_m", g_sim.hand_reset_distance );
	g_sim.hand_reset_distance = std::max( 0.1f, std::min( 3.0f, g_sim.hand_reset_distance ) );

	g_sim.hand_reset_height_offset = GetFloatSetting( "hand_reset_height_offset_m", g_sim.hand_reset_height_offset );
	g_sim.hand_reset_height_offset = std::max( -1.5f, std::min( 1.0f, g_sim.hand_reset_height_offset ) );

	// Initial orientation (degrees), useful for aiming presets / testing.
	const float yawDeg = GetFloatSetting( "head_yaw_deg", 0.0f );
	const float pitchDeg = GetFloatSetting( "head_pitch_deg", 0.0f );
	g_sim.head_yaw = yawDeg * 0.0174532925f;
	g_sim.head_pitch = pitchDeg * 0.0174532925f;

	DriverLog( "shiyan_nakedmonitor: input sim (mode %s; target %s; primary hand %s; Z reset %.2fm, height %+.2fm).",
		g_sim.natural_mouse ? "natural_mouse" : "vr_sim",
		TargetName( g_sim.target ), g_sim.primary_hand == 0 ? "left" : "right",
		g_sim.hand_reset_distance, g_sim.hand_reset_height_offset );
}

void SetHeadBaseHeight( float meters )
{
	g_head_base_height = meters;
}

void SetGamepad( const GamepadInput &input )
{
	g_gamepad = input;
}

void Update( RawInput::State &input )
{
	if ( !g_sim.initialized )
	{
		Init();
	}
	if ( g_sim.swallow_frames > 0 )
	{
		--g_sim.swallow_frames;
		input.dx = 0;
		input.dy = 0;
	}

	const auto now = std::chrono::steady_clock::now();
	float dt = 0.0f;
	if ( g_sim.last_time != std::chrono::steady_clock::time_point{} )
	{
		dt = std::chrono::duration_cast< std::chrono::duration< float > >( now - g_sim.last_time ).count();
		dt = std::max( 0.0f, std::min( dt, 0.1f ) );
	}
	g_sim.last_time = now;

	// --- gamepad B: tap = secondary face button, hold = 3x speed -----------
	const bool pad_b = g_gamepad.connected && g_gamepad.buttonB;
	bool pad_sprint = false;
	if ( pad_b && !g_sim.pad_b_down )
	{
		g_sim.pad_b_down = true;
		g_sim.pad_b_long = false;
		g_sim.pad_b_press = now;
	}
	else if ( pad_b && g_sim.pad_b_down && !g_sim.pad_b_long )
	{
		const float held = std::chrono::duration_cast< std::chrono::duration< float > >(
			now - g_sim.pad_b_press ).count();
		if ( held >= kGamepadLongPressSeconds )
		{
			g_sim.pad_b_long = true;
		}
	}
	else if ( !pad_b && g_sim.pad_b_down )
	{
		g_sim.pad_b_down = false;
		if ( !g_sim.pad_b_long )
		{
			// Released before the threshold: one short click.
			g_sim.pad_b_tap_pulse = kGamepadTapPulseSeconds;
		}
		g_sim.pad_b_long = false;
	}

	pad_sprint = pad_b && g_sim.pad_b_long;
	if ( g_sim.pad_b_tap_pulse > 0.0f )
	{
		g_sim.pad_b_tap_pulse = std::max( 0.0f, g_sim.pad_b_tap_pulse - dt );
	}
	const bool padFaceSecondary = g_sim.pad_b_tap_pulse > 0.0f;

	// --- G / Start: toggle interaction mode --------------------------------
	const bool g_down = input.keys[ 'G' ];
	const bool pad_mode = g_gamepad.connected && g_gamepad.modeToggle;
	if ( ( g_down && !g_sim.last_g ) || ( pad_mode && !g_sim.last_pad_mode ) )
	{
		g_sim.natural_mouse = !g_sim.natural_mouse;
		DriverLog( "shiyan_nakedmonitor: interaction mode -> %s",
			g_sim.natural_mouse ? "natural_mouse (cursor = panel pointer)"
								 : "vr_sim (mouse aims the active target)" );
	}
	g_sim.last_g = g_down;
	g_sim.last_pad_mode = pad_mode;

	// --- 1/2/3 + Tab: active target (vr_sim only) --------------------------
	const bool one = input.keys[ '1' ];
	const bool two = input.keys[ '2' ];
	const bool three = input.keys[ '3' ];
	const bool tab = input.keys[ VK_TAB ];
	const bool pad_prev = g_gamepad.connected && g_gamepad.prevTarget;
	const bool pad_next = g_gamepad.connected && g_gamepad.nextTarget;
	if ( !g_sim.natural_mouse )
	{
		if ( one && !g_sim.last_1 )
		{
			g_sim.target = Target::Head;
		}
		if ( two && !g_sim.last_2 )
		{
			g_sim.target = Target::LeftHand;
		}
		if ( three && !g_sim.last_3 )
		{
			g_sim.target = Target::RightHand;
		}
		if ( tab && !g_sim.last_tab )
		{
			g_sim.target = ( g_sim.target == Target::Head ) ? Target::LeftHand
				: ( g_sim.target == Target::LeftHand )  ? Target::RightHand
														: Target::Head;
		}
		// Gamepad d-pad left/right: previous / next target.
		if ( pad_next && !g_sim.last_pad_next )
		{
			g_sim.target = ( g_sim.target == Target::Head ) ? Target::LeftHand
				: ( g_sim.target == Target::LeftHand )  ? Target::RightHand
														: Target::Head;
		}
		if ( pad_prev && !g_sim.last_pad_prev )
		{
			g_sim.target = ( g_sim.target == Target::Head ) ? Target::RightHand
				: ( g_sim.target == Target::RightHand ) ? Target::LeftHand
														: Target::Head;
		}
	}
	g_sim.last_1 = one;
	g_sim.last_2 = two;
	g_sim.last_3 = three;
	g_sim.last_tab = tab;
	g_sim.last_pad_prev = pad_prev;
	g_sim.last_pad_next = pad_next;

	if ( !g_sim.target_logged )
	{
		g_sim.target_logged = true;
		DriverLog( "shiyan_nakedmonitor: sim target = %s (1/2/3 = head/left/right, Tab cycles).",
			TargetName( g_sim.target ) );
	}
	else
	{
		static Target s_lastTarget = Target::Head;
		if ( s_lastTarget != g_sim.target )
		{
			s_lastTarget = g_sim.target;
			DriverLog( "shiyan_nakedmonitor: sim target = %s.",
				TargetName( g_sim.target ) );
		}
	}

	// --- screen size (used for per-pixel sensitivities / translate speed) ---
	const float screenW = static_cast< float >( std::max( 1, GetSystemMetrics( SM_CXSCREEN ) ) );
	const float screenH = static_cast< float >( std::max( 1, GetSystemMetrics( SM_CYSCREEN ) ) );

	// --- F: mouse-translate mode -------------------------------------------
	const bool f_down = input.keys[ 'F' ];
	if ( f_down && !g_sim.last_f )
	{
		g_sim.mouse_translate = !g_sim.mouse_translate;
		DriverLog( "shiyan_nakedmonitor: mouse translate mode %s (F)",
			g_sim.mouse_translate ? "ON" : "OFF" );
	}
	g_sim.last_f = f_down;

	float mouse_move_right = 0.0f;
	float mouse_move_up = 0.0f;
	if ( g_sim.mouse_translate )
	{
		const float metersPerPixel = kMouseTranslateMetersPerScreen / screenW;
		mouse_move_right = static_cast< float >( input.dx ) * metersPerPixel;
		mouse_move_up = -static_cast< float >( input.dy ) * metersPerPixel;
	}

	// --- keys / sticks -----------------------------------------------------
	const bool padVertical = g_gamepad.connected && g_gamepad.moveModifier;
	const float stickForward = ( g_gamepad.connected && !padVertical ) ? g_gamepad.moveY : 0.0f;
	const float stickStrafe = ( g_gamepad.connected && !padVertical ) ? g_gamepad.moveX : 0.0f;
	const float keyForward = ( input.keys[ 'W' ] ? 1.0f : 0.0f ) - ( input.keys[ 'S' ] ? 1.0f : 0.0f );
	const float keyStrafe = ( input.keys[ 'D' ] ? 1.0f : 0.0f ) - ( input.keys[ 'A' ] ? 1.0f : 0.0f );
	const float moveForward = std::max( -1.0f, std::min( 1.0f, keyForward + stickForward ) );
	const float moveStrafe = std::max( -1.0f, std::min( 1.0f, keyStrafe + stickStrafe ) );
	const bool movingDown = input.keys[ 'Q' ];
	const bool movingUp = input.keys[ 'E' ];
	// RB + left stick (gamepad) or Q/E (keyboard) move vertically.
	const float moveVertical = ( padVertical ? g_gamepad.moveY : 0.0f )
		+ ( movingUp ? 1.0f : 0.0f ) - ( movingDown ? 1.0f : 0.0f );
	const float speedScale = ( input.keys[ VK_SHIFT ] || pad_sprint ) ? 3.0f : 1.0f;

	const bool select = input.left;
	const bool squeeze = input.right;
	const bool menu = input.keys[ 'M' ] || ( g_gamepad.connected && g_gamepad.menu );
	const bool headLookModifier = input.middle;

	const bool stickClick = input.keys[ 'R' ]
		&& !( input.keys[ VK_CONTROL ] && input.keys[ VK_MENU ] );
	const bool facePrimary = input.keys[ 'X' ] || ( g_gamepad.connected && g_gamepad.facePrimary );
	const bool faceSecondary = input.keys[ 'C' ] || padFaceSecondary;

	const float triggerValue = std::max( select ? 1.0f : 0.0f,
		g_gamepad.connected ? g_gamepad.trigger : 0.0f );
	const float gripValue = std::max( squeeze ? 1.0f : 0.0f,
		g_gamepad.connected ? g_gamepad.grip : 0.0f );

	const bool headIsTarget = ( g_sim.target == Target::Head );

	// --- head look (incremental) ------------------------------------------
	// vr_sim with the head as target, or MMB held while a hand is targeted.
	if ( !g_sim.mouse_translate && ( headLookModifier || ( !g_sim.natural_mouse && headIsTarget ) ) )
	{
		const float sensitivity = ( kHeadMaxYaw / ( screenW * 0.5f ) ) * g_sim.head_look_gain;
		const float sign = g_sim.head_look_invert ? 1.0f : -1.0f;
		g_sim.head_yaw += sign * static_cast< float >( input.dx ) * sensitivity;
		g_sim.head_pitch += sign * static_cast< float >( input.dy ) * sensitivity;
		g_sim.head_pitch = std::max( -kHeadMaxPitch, std::min( kHeadMaxPitch, g_sim.head_pitch ) );
		while ( g_sim.head_yaw > 3.14159265f )
		{
			g_sim.head_yaw -= 2.0f * 3.14159265f;
		}
		while ( g_sim.head_yaw < -3.14159265f )
		{
			g_sim.head_yaw += 2.0f * 3.14159265f;
		}

		if ( g_sim.cursor_recenter )
		{
			RecenterCursorIfNearEdge();
		}
		input.dx = 0;
		input.dy = 0;
	}

	// --- active hand aim (incremental) ------------------------------------
	// Mouse deltas turn the active hand relative to its current orientation, so
	// there is no angle ceiling and hitting the screen edge does not stop the
	// rotation (raw input keeps reporting movement). Z sets the base facing.
	if ( !g_sim.natural_mouse && !headIsTarget && !g_sim.mouse_translate && !headLookModifier )
	{
		const int hand = ( g_sim.target == Target::LeftHand ) ? 0 : 1;
		const float yawPerPixel = ( kHandMaxYaw / ( screenW * 0.5f ) ) * g_sim.hand_look_gain;
		const float pitchPerPixel = ( kHandMaxPitch / ( screenH * 0.5f ) ) * g_sim.hand_look_gain;
		g_sim.hand_yaw[ hand ] -= static_cast< float >( input.dx ) * yawPerPixel;
		g_sim.hand_pitch[ hand ] -= static_cast< float >( input.dy ) * pitchPerPixel;
		g_sim.hand_pitch[ hand ] = std::max( -kHeadMaxPitch, std::min( kHeadMaxPitch, g_sim.hand_pitch[ hand ] ) );
		while ( g_sim.hand_yaw[ hand ] > 3.14159265f )
		{
			g_sim.hand_yaw[ hand ] -= 2.0f * 3.14159265f;
		}
		while ( g_sim.hand_yaw[ hand ] < -3.14159265f )
		{
			g_sim.hand_yaw[ hand ] += 2.0f * 3.14159265f;
		}
	}

	// --- right stick -------------------------------------------------------
	// natural_mouse: stick turns the view; LB + stick turns the primary hand.
	// vr_sim:        stick turns the active target; LB + stick turns the view.
	if ( g_gamepad.connected && !g_sim.mouse_translate )
	{
		const bool rotateView = g_sim.natural_mouse ? !g_gamepad.lookModifier
			: ( g_gamepad.lookModifier || headIsTarget );
		const float lookScale = pad_sprint ? 3.0f : 1.0f;
		const float yawRate = g_sim.stick_yaw_rate * lookScale;
		const float pitchRate = g_sim.stick_pitch_rate * lookScale;

		if ( rotateView )
		{
			g_sim.head_yaw = WrapPi( g_sim.head_yaw
				- g_gamepad.lookX * yawRate * g_sim.head_look_gain * dt );
			g_sim.head_pitch += g_gamepad.lookY * pitchRate * g_sim.head_look_gain * dt;
			g_sim.head_pitch = std::max( -kHeadMaxPitch, std::min( kHeadMaxPitch, g_sim.head_pitch ) );
		}
		else if ( g_sim.natural_mouse )
		{
			const int hand = g_sim.primary_hand;
			g_sim.hand_ray_yaw[ hand ] = WrapPi( g_sim.hand_ray_yaw[ hand ]
				- g_gamepad.lookX * yawRate * g_sim.hand_look_gain * dt );
			g_sim.hand_ray_pitch[ hand ] += g_gamepad.lookY * pitchRate * g_sim.hand_look_gain * dt;
			g_sim.hand_ray_pitch[ hand ] = std::max( -kHeadMaxPitch,
				std::min( kHeadMaxPitch, g_sim.hand_ray_pitch[ hand ] ) );
		}
		else
		{
			const int hand = ( g_sim.target == Target::LeftHand ) ? 0 : 1;
			g_sim.hand_yaw[ hand ] = WrapPi( g_sim.hand_yaw[ hand ]
				- g_gamepad.lookX * yawRate * g_sim.hand_look_gain * dt );
			g_sim.hand_pitch[ hand ] += g_gamepad.lookY * pitchRate * g_sim.hand_look_gain * dt;
			g_sim.hand_pitch[ hand ] = std::max( -kHeadMaxPitch, std::min( kHeadMaxPitch, g_sim.hand_pitch[ hand ] ) );
		}
	}

	// --- Z / Back: reset both hands in front of the head ------------------
	const bool z_down = input.keys[ 'Z' ];
	const bool pad_reset = g_gamepad.connected && g_gamepad.reset && !g_sim.natural_mouse;
	if ( ( z_down && !g_sim.last_z ) || ( pad_reset && !g_sim.last_pad_reset ) )
	{
		float headQuat[ 4 ] = {};
		Quat::FromYawPitch( g_sim.head_yaw, g_sim.head_pitch, headQuat );
		const float forwardIn[ 3 ] = { 0.0f, 0.0f, -1.0f };
		const float rightIn[ 3 ] = { 1.0f, 0.0f, 0.0f };
		float forward[ 3 ] = {};
		float right[ 3 ] = {};
		Quat::RotateVector( headQuat, forwardIn, forward );
		Quat::RotateVector( headQuat, rightIn, right );

		const float headY = g_sim.head_offset[ 1 ];
		const float resetDistance = g_sim.hand_reset_distance;
		const float resetHeight = g_sim.hand_reset_height_offset;
		for ( int hand = 0; hand < 2; ++hand )
		{
			const float side = ( hand == 0 ) ? -kHandResetHalfSpacing : kHandResetHalfSpacing;
			g_sim.hand_pos[ hand ][ 0 ] = g_sim.head_offset[ 0 ] + forward[ 0 ] * resetDistance + right[ 0 ] * side;
			g_sim.hand_pos[ hand ][ 1 ] = headY + forward[ 1 ] * resetDistance + right[ 1 ] * side + resetHeight;
			g_sim.hand_pos[ hand ][ 2 ] = g_sim.head_offset[ 2 ] + forward[ 2 ] * resetDistance + right[ 2 ] * side;
			g_sim.hand_yaw[ hand ] = g_sim.head_yaw;
			g_sim.hand_pitch[ hand ] = g_sim.head_pitch;
			g_sim.hand_ray_yaw[ hand ] = 0.0f;
			g_sim.hand_ray_pitch[ hand ] = 0.0f;
		}

		DriverLog( "shiyan_nakedmonitor: Z reset both hands to %.2fm in front of the head (height %+.2fm, facing the head).",
			resetDistance, resetHeight );
	}
	g_sim.last_z = z_down;
	g_sim.last_pad_reset = pad_reset;

	// --- movement of the active target ------------------------------------
	auto applyMouseTranslate = [&]( float position[ 3 ] ) {
		if ( mouse_move_right != 0.0f )
		{
			// Lateral move in view space (A/D); reuse the yaw-plane right vector.
			position[ 0 ] += std::cos( g_sim.head_yaw ) * mouse_move_right;
			position[ 2 ] += -std::sin( g_sim.head_yaw ) * mouse_move_right;
		}
		if ( mouse_move_up != 0.0f )
		{
			position[ 1 ] += mouse_move_up;
		}
	};

	if ( g_sim.natural_mouse || headIsTarget )
	{
		const float step = kHeadMoveSpeed * g_sim.head_move_gain * speedScale * dt;
		MoveInViewSpace( g_sim.head_offset, g_sim.head_yaw, step, moveForward, moveStrafe );
		if ( moveVertical != 0.0f )
		{
			g_sim.head_offset[ 1 ] += step * moveVertical;
		}
		applyMouseTranslate( g_sim.head_offset );
	}
	else
	{
		const int hand = ( g_sim.target == Target::LeftHand ) ? 0 : 1;
		const float step = kHandMoveSpeed * g_sim.hand_move_gain * speedScale * dt;
		MoveInViewSpace( g_sim.hand_pos[ hand ], g_sim.head_yaw, step, moveForward, moveStrafe );
		if ( moveVertical != 0.0f )
		{
			g_sim.hand_pos[ hand ][ 1 ] += step * moveVertical;
		}
		applyMouseTranslate( g_sim.hand_pos[ hand ] );

		// Wheel = W/S for the active hand (0.1 m per notch).
		if ( input.wheel != 0 )
		{
			const float sign = g_sim.wheel_invert ? -1.0f : 1.0f;
			const float distance = static_cast< float >( input.wheel ) * kWheelMoveStep * g_sim.hand_move_gain * sign;
			MoveInViewSpace( g_sim.hand_pos[ hand ], g_sim.head_yaw, std::fabs( distance ),
				distance > 0.0f ? 1.0f : -1.0f, 0.0f );
		}
	}

	// --- thumbstick (arrows / d-pad up-down) + natural_mouse wheel pulse ---
	float thumbstickX = ( input.keys[ VK_RIGHT ] ? 1.0f : 0.0f ) - ( input.keys[ VK_LEFT ] ? 1.0f : 0.0f );
	float thumbstickY = ( input.keys[ VK_UP ] ? 1.0f : 0.0f ) - ( input.keys[ VK_DOWN ] ? 1.0f : 0.0f );
	if ( g_gamepad.connected )
	{
		thumbstickY = std::max( -1.0f, std::min( 1.0f, thumbstickY + g_gamepad.thumbY ) );
	}
	if ( g_sim.natural_mouse )
	{
		if ( input.wheel != 0 )
		{
			g_sim.wheel_stick_seconds = kWheelStickPulseSeconds;
			const float dir = ( input.wheel > 0 ) ? 1.0f : -1.0f;
			g_sim.wheel_stick_sign = dir * ( g_sim.wheel_invert ? -1.0f : 1.0f );
		}
		else if ( g_sim.wheel_stick_seconds > 0.0f )
		{
			g_sim.wheel_stick_seconds = std::max( 0.0f, g_sim.wheel_stick_seconds - dt );
		}
		if ( g_sim.wheel_stick_seconds > 0.0f && thumbstickY == 0.0f )
		{
			thumbstickY = g_sim.wheel_stick_sign;
		}
	}

	// --- route buttons to a hand ------------------------------------------
	const int buttonHand = g_sim.natural_mouse ? g_sim.primary_hand
		: ( g_sim.target == Target::LeftHand )  ? 0
		: ( g_sim.target == Target::RightHand ) ? 1
												: -1;
	const int menuHand = ( buttonHand >= 0 ) ? buttonHand : g_sim.primary_hand;

	for ( int hand = 0; hand < 2; ++hand )
	{
		HandInput &out = g_sim.hand_input[ hand ];
		out = HandInput{};
		const bool isActive = ( hand == buttonHand );
		out.trigger = isActive ? triggerValue : 0.0f;
		out.triggerClick = isActive && triggerValue > 0.5f;
		out.grip = isActive ? gripValue : 0.0f;
		out.gripClick = isActive && gripValue > 0.5f;
		out.facePrimary = isActive && facePrimary;
		out.faceSecondary = isActive && faceSecondary;
		out.menu = ( hand == menuHand ) && menu;
		out.thumbstickX = isActive ? thumbstickX : 0.0f;
		out.thumbstickY = isActive ? thumbstickY : 0.0f;
		out.thumbstickClick = isActive && stickClick;
	}
}

bool IsNaturalMouse()
{
	return g_sim.natural_mouse;
}

Target GetTarget()
{
	return g_sim.target;
}

void GetHeadYawPitch( float *outYaw, float *outPitch )
{
	if ( outYaw != nullptr )
	{
		*outYaw = g_sim.head_yaw;
	}
	if ( outPitch != nullptr )
	{
		*outPitch = g_sim.head_pitch;
	}
}

void GetHeadOffset( float out[ 3 ] )
{
	if ( out != nullptr )
	{
		out[ 0 ] = g_sim.head_offset[ 0 ];
		out[ 1 ] = g_sim.head_offset[ 1 ];
		out[ 2 ] = g_sim.head_offset[ 2 ];
	}
}

void GetHeadOrientation( float outQuat[ 4 ] )
{
	if ( outQuat != nullptr )
	{
		Quat::FromYawPitch( g_sim.head_yaw, g_sim.head_pitch, outQuat );
	}
}

void GetControllerPose( int hand, float outPosition[ 3 ], float outOrientation[ 4 ] )
{
	const int idx = ( hand == 0 ) ? 0 : 1;

	if ( g_sim.natural_mouse )
	{
		float rayPos[ 3 ] = {};
		float rayQuat[ 4 ] = {};
		if ( MouseRay::ComputeAimPose( rayPos, rayQuat ) )
		{
			for ( int i = 0; i < 3; ++i )
			{
				outPosition[ i ] = rayPos[ i ];
			}

			// Extra aim offset on top of the ray (gamepad LB + right stick).
			float offset[ 4 ] = {};
			Quat::FromYawPitch( g_sim.hand_ray_yaw[ idx ], g_sim.hand_ray_pitch[ idx ], offset );
			Quat::Multiply( offset, rayQuat, outOrientation );
			return;
		}
	}

	outPosition[ 0 ] = g_sim.hand_pos[ idx ][ 0 ];
	outPosition[ 1 ] = g_head_base_height + g_sim.hand_pos[ idx ][ 1 ];
	outPosition[ 2 ] = g_sim.hand_pos[ idx ][ 2 ];
	Quat::FromYawPitch( g_sim.hand_yaw[ idx ], g_sim.hand_pitch[ idx ], outOrientation );
}

void GetHandInput( int hand, HandInput *out )
{
	if ( out == nullptr )
	{
		return;
	}
	*out = g_sim.hand_input[ ( hand == 0 ) ? 0 : 1 ];
}

} // namespace VrSim
