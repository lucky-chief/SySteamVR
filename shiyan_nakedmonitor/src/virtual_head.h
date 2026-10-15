//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

#include "raw_input.h"

//-----------------------------------------------------------------------------
// Simulated head (ported from SyOpenXRRuntime's vr_sim / natural_mouse input
// simulator, trimmed to a single fixed-panel viewer):
//
//   MMB held + mouse  -> incremental look (yaw/pitch), with edge cursor
//                        recenter so it never "gets stuck" at the screen edge
//   WASD / QE         -> walk in the yaw plane (view space); Shift = 3x
//   G                 -> toggle interaction mode:
//                          natural_mouse (default): cursor = panel pointer,
//                            the controller aim ray goes eye -> cursor point
//                            on the panel (WYSIWYG "point at what you see")
//                          vr_sim: mouse deltas steer the aim angle directly
//                            (the old behaviour, kept as a fallback)
//
// The HMD pose is composed from this state: base (0, seated_height, 0) plus
// the walk offset, rotated by yaw/pitch. PushDisplayTransforms converts the
// tracked eyes into head space so the off-axis frustum keeps matching the
// physical panel.
//-----------------------------------------------------------------------------
namespace VirtualHead
{

// Reads settings (interaction_mode, head_move_speed_mps, ...). Idempotent.
void Init();

// Per-frame update from the provider's RunFrame(). Mutates `input`: when the
// mouse deltas were consumed by head look (MMB held or vr_sim aim) they are
// zeroed so the controller does not double-apply them; deltas are also zeroed
// for one frame after an automatic cursor recenter.
void Update( RawInput::State &input );

// --- HMD pose ---------------------------------------------------------------
// Yaw around +Y (positive = turn left), pitch around +X (positive = up).
void GetYawPitch( float *outYaw, float *outPitch );
// Walk offset in tracking space (metres), applied on top of the seated base.
void GetPositionOffset( float out[ 3 ] );

// --- Interaction mode -------------------------------------------------------
bool IsNaturalMouse();

} // namespace VirtualHead
