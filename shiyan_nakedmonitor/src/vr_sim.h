//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include "raw_input.h"

//-----------------------------------------------------------------------------
// Keyboard/mouse input simulator, ported from SyOpenXRRuntime's vr_sim
// (src/xr_input.cpp UpdateSimInput). It owns the simulated head and both hands
// plus the "active target" concept:
//
//   vr_sim (default):  mouse absolutely aims the ACTIVE hand, or turns the head
//                      incrementally when the head is the target.
//   natural_mouse:     mouse is a panel pointer; both hands aim along the eye ->
//                      cursor ray, the head is moved with WASD.
//
//   G          toggle vr_sim <-> natural_mouse
//   H          show/hide the OS pointer over the output window (provider)
//   1 / 2 / 3  activate head / left hand / right hand   (vr_sim)
//   Tab        cycle targets                              (vr_sim)
//   MMB        temporary incremental head look
//   WASD / QE  move the active target (view space), Shift = 3x
//   F          toggle mouse-translate mode (mouse moves the target)
//   Z          reset both hands to 1 m in front of the head
//   LMB/RMB/M  select / squeeze / menu
//   X / C      face primary / secondary (left X,Y - right A,B)
//   R          thumbstick click
//   wheel      hand target: move forward/back; natural_mouse: thumbstick pulse
//
// Positions are absolute in tracking space (the caller sets the seated base
// height); the HMD pose adds the walk offset on top of that base.
//-----------------------------------------------------------------------------
namespace VrSim
{

enum class Target
{
	Head = 0,
	LeftHand = 1,
	RightHand = 2,
};

const char *TargetName( Target target );

// One hand's routed button/axis state for this frame.
struct HandInput
{
	float trigger = 0.0f;
	bool triggerClick = false;
	float grip = 0.0f;
	bool gripClick = false;
	bool facePrimary = false;   // left X / right A
	bool faceSecondary = false; // left Y / right B
	bool menu = false;
	float thumbstickX = 0.0f;
	float thumbstickY = 0.0f;
	bool thumbstickClick = false;
};

// Physical gamepad, merged with the keyboard/mouse state every frame.
// Sticks are -1 .. 1 (x right, y up/forward positive), triggers 0 .. 1.
struct GamepadInput
{
	bool connected = false;
	float moveX = 0.0f; // left stick
	float moveY = 0.0f;
	float lookX = 0.0f; // right stick
	float lookY = 0.0f;
	float trigger = 0.0f; // LT
	float grip = 0.0f;    // RT
	bool facePrimary = false;
	bool buttonB = false;      // raw B: tap = face secondary, hold = 3x speed
	bool menu = false;         // Start
	bool modeToggle = false;   // L3: vr_sim <-> natural_mouse
	bool reset = false;        // R3: reset the controllers (vr_sim)
	bool lookModifier = false; // LB: right stick rotates the "other" thing
	bool moveModifier = false; // RB: left stick moves vertically
	bool prevTarget = false;   // d-pad left: previous target
	bool nextTarget = false;   // d-pad right: next target
	float thumbY = 0.0f;       // d-pad up/down -> thumbstick Y
};

// Installed by the provider once per frame before Update().
void SetGamepad( const GamepadInput &input );

void Init();

// Seated base height (metres): the y that the head sits at with a zero walk
// offset. Set by the HMD driver from its "seated_height_meters" setting.
void SetHeadBaseHeight( float meters );

// Per-frame update from the provider's RunFrame(). Consumes the mouse deltas it
// uses and leaves the rest for other consumers.
void Update( RawInput::State &input );

bool IsNaturalMouse();
Target GetTarget();

// --- Head (tracking space) --------------------------------------------------
void GetHeadYawPitch( float *outYaw, float *outPitch );
// Head orientation quaternion (x, y, z, w) = Ry(yaw) * Rx(pitch).
void GetHeadOrientation( float outQuat[ 4 ] );
// Walk offset on top of the seated base (metres).
void GetHeadOffset( float out[ 3 ] );

// --- Hands (tracking space) -------------------------------------------------
// aim/grip pose for one controller: natural_mouse -> the mouse ray, vr_sim ->
// the hand position with its absolute aim angles. hand: 0 = left, 1 = right.
void GetControllerPose( int hand, float outPosition[ 3 ], float outOrientation[ 4 ] );

void GetHandInput( int hand, HandInput *out );

} // namespace VrSim
