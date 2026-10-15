//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

// Eye tracking + physical panel size, ported from the validated OpenXR runtime
// project (D:\Projects\C++\SyOpenXRRuntime, see src/binocular_camera.cpp and
// src/xr_eyetracking_win.cpp for the original, battle-tested logic).
//
// Wraps 视延科技's BinocularCameraConfig.dll (plus its hv.dll dependency):
//   * Register takes 10+ seconds (vendor-side network round trip), so it MUST
//     run on a background thread - never on vrserver's frame loop,
//   * GetCameraPosition(filter=0) returns the two eye positions in MILLIMETRES
//     in screen space: origin = panel centre, +x right, +y up, +z toward the
//     viewer (default position: [-32, 0, 500] / [32, 0, 500] mm),
//   * GetScreenSize returns the panel's physical size in millimetres.
//
// This module converts to METRES and keeps a blended "current eyes" snapshot:
// valid tracking follows the tracker directly; on failure it eases back to the
// default eye position with a 1s time constant (identical to the OpenXR
// runtime's xrLocateViews behaviour, see BlendTowardEyePos there).

namespace EyeTracker
{

// Panel size defaults (metres), used until the SDK reports the real size.
// Supplied default for the 31.5" 4K panel: 0.6981 x 0.3927 m.
void SetDefaultScreenSize( float widthMeters, float heightMeters );

// Idempotent + non-blocking: starts the vendor Register on a background thread.
void Start();

// Called every driver frame: settles the async registration, fetches the panel
// size from the SDK once it is available, reads the tracker and advances the
// fallback blending.
void Pump();

// Blended eye positions in metres (clamped to the panel and a minimum distance
// to keep the frustum well-formed). Always succeeds; returns the default eye
// positions when tracking is unavailable.
void GetCurrentEyes( float outLeft[ 3 ], float outRight[ 3 ] );

// Panel physical size in metres (SDK value once registered, configured default
// before that).
void GetScreenSize( float *outWidthMeters, float *outHeightMeters );

bool IsEnabled();  // Start() has been called
bool IsReady();    // vendor Register finished successfully
bool IsTracking(); // last tracker state was "tracked"

// Unregister on driver shutdown. Leaves an in-flight background Register alone
// (mirrors shutdown_for_dll_unload in the reference project).
void Shutdown();

} // namespace EyeTracker
