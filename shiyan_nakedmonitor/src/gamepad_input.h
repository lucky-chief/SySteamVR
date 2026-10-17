//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <cstdint>

//-----------------------------------------------------------------------------
// Physical gamepad (XInput) reader.
//
// SteamVR's own "gamepad" driver cannot be used as the input path for this
// setup: a custom driver cannot bind the reserved "/user/gamepad" alias
// (openvr#923), and the built-in driver's default bindings only cover a few
// SteamVR Home actions, so most buttons do nothing in Home and in games.
//
// Instead the driver reads the pad with XInput and feeds the virtual left/right
// controllers (see vr_sim.h), which already have working input bindings.
//
// Steam Input must be disabled for SteamVR (app 250820), otherwise Steam grabs
// the pad and XInputGetState in vrserver returns ERROR_DEVICE_NOT_CONNECTED.
//-----------------------------------------------------------------------------
namespace GamepadInput
{

struct State
{
	bool connected = false;

	// Sticks, already deadzoned and rescaled: -1 .. 1, y is up/forward positive.
	float leftX = 0.0f;
	float leftY = 0.0f;
	float rightX = 0.0f;
	float rightY = 0.0f;

	// Triggers, 0 .. 1.
	float leftTrigger = 0.0f;
	float rightTrigger = 0.0f;

	bool a = false;
	bool b = false;
	bool x = false;
	bool y = false;
	bool back = false;
	bool start = false;
	bool guide = false;
	bool shoulderLeft = false;
	bool shoulderRight = false;
	bool leftStickClick = false;
	bool rightStickClick = false;
	bool dpadUp = false;
	bool dpadDown = false;
	bool dpadLeft = false;
	bool dpadRight = false;
};

// Reads settings (section "shiyan_nakedmonitor_gamepad"). Idempotent.
void Init();

// Polls the configured (or first connected) pad. Call once per frame.
State Poll();

// Starts/stops rumble; amplitude 0 .. 1. The pulse stops by itself after
// `seconds` (clamped to 0.02 .. 2 s) or on the next Rumble(0, ...) call.
void Rumble( float amplitude, float seconds );

} // namespace GamepadInput
