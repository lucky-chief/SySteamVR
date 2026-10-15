//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include <array>

// Global keyboard/mouse capture for the virtual controller ("plan B"):
// a dedicated thread owns a message-only window (HWND_MESSAGE) and receives
// WM_INPUT from RegisterRawInputDevices with RIDEV_INPUTSINK, so button/key
// state and mouse deltas keep flowing even while SteamVR hides or clips the
// cursor (dashboard open, VR app focused). GetCursorPos polling would freeze
// in that situation, and wheel deltas are only available this way.
//
// The thread publishes into a mutex-protected snapshot; the driver's RunFrame
// calls Consume() once per frame. Mouse dx/dy/wheel are accumulated deltas -
// Consume() takes and resets them, so no motion is lost between frames.
namespace RawInput
{

struct State
{
	// Accumulated since the previous Consume() (pixels / wheel notches).
	long dx = 0;
	long dy = 0;
	long wheel = 0;

	bool left = false;
	bool right = false;
	bool middle = false;

	// Indexed by virtual-key code; true while the key is physically down.
	std::array<bool, 256> keys{};
};

// Idempotent: spawns the capture thread (which creates the message-only
// window and registers raw input). Returns false if the thread could not start.
bool Start();

// Posts WM_QUIT to the capture thread and joins it. Safe to call when never started.
void Shutdown();

// Atomically copies buttons/keys and takes (and resets) the accumulated deltas.
State Consume();

} // namespace RawInput
