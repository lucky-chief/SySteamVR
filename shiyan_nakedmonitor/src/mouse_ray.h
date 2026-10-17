//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

//-----------------------------------------------------------------------------
// Mouse-pointer ray (port of SyOpenXRRuntime's natural_mouse, see
// xr_input.cpp ComputeMouseRayPose / NormalizeCursorToPanel):
//
//   1. The cursor position is normalized to panel coordinates (u right,
//      v down). With cursor_source=screen (default) the whole virtual
//      desktop maps linearly onto the panel, so the mouse works from the
//      primary monitor. With cursor_source=window the compositor "Headset
//      Window" client rect is used instead, combined with cursor_mapping:
//      the default half_modulo treats each half of the SBS window as the
//      same panel image, so the ray lands exactly on the scene content
//      rendered under the cursor, in either half.
//   2. (u, v) is projected onto the physical panel plane (screen space
//      z = 0 -> tracking space z = -screen_distance).
//   3. The ray starts at the tracked centre-eye position (plus optional
//      forward/up offsets) and always points exactly at the panel hit point,
//      so any origin offset keeps "point at what you see" intact.
//
// Returns the aim pose in tracking space: position = ray origin,
// orientation = -Z along the ray.
//-----------------------------------------------------------------------------
namespace MouseRay
{

// Reads settings (cursor_mapping, ray_origin_forward_m, ray_origin_up_m and
// the panel geometry). Idempotent.
void Init();

// Computes the aim pose from the current cursor position and tracked eyes.
// Returns false (leaving outPose untouched) when no cursor/panel data is
// available.
bool ComputeAimPose( float outPosition[ 3 ], float outOrientation[ 4 ] );

// Head pose in tracking space: centre-eye position (panel geometry + walk
// offset) and head orientation. Used by the natural_mouse aim reset.
bool GetHeadPose( float outPosition[ 3 ], float outOrientation[ 4 ] );

} // namespace MouseRay
