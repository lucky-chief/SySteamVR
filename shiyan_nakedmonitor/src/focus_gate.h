//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

//-----------------------------------------------------------------------------
// Input focus gate.
//
// The keyboard/mouse capture (raw_input) is deliberately global: RIDEV_INPUTSINK
// keeps receiving events while another window has focus, which is required for
// a VR view that is not the focused window. The downside is that typing or
// moving the mouse in the editor / browser also drives the simulated head and
// controllers.
//
// This gate decides whether input may reach the simulator right now:
//   * the driver's direct-mode output window owns the foreground (clicked the
//     VR view on the panel), or
//   * the foreground window belongs to a SteamVR process (dashboard, status
//     window, SteamVR Home, ...), or
//   * (optional) a fullscreen window is in front - VR apps usually show their
//     own mirror window fullscreen on a monitor.
//
// Anything else (the user working on the desktop) blocks the input, and the
// raw-input deltas are still consumed every frame so that refocusing the VR
// view never produces a jump.
//-----------------------------------------------------------------------------
namespace FocusGate
{

// Reads the "shiyan_nakedmonitor_focus" settings section. Idempotent.
void Init();

// Cached for ~100 ms; cheap to call every frame.
bool IsInputActive();

} // namespace FocusGate
