# Shiyan Naked Monitor — OpenVR driver

Virtual OpenVR/SteamVR driver that exposes the naked-eye 3D (autostereoscopic)
side-by-side panel as a SteamVR headset.

## How it works

```
Game -> SteamVR compositor -> this driver (registered as an HMD)
     -> full-screen SBS desktop window -> naked-eye 3D panel -> stereo image
```

* The HMD is registered with `TrackedDeviceClass_HMD` and exposes a single
  `IVRDisplayComponent` in **extended-desktop mode**. The compositor renders the
  left eye into the left half and the right eye into the right half of the
  window, which is a plain side-by-side image.
* Distortion is **identity** (the panel has no lens).
* The pose is **static** ("cinema mode"): there is no tracking yet.

## Naming

Valve's docs want the driver name lowercase and identical for the folder, the
manifest `name`, and the DLL (`driver_<name>.dll`). The product name
`Shiyan_NakedMonitor` was therefore normalised to **`shiyan_nakedmonitor`**
internally; the friendly name shown in SteamVR is the `model_number`
("Shiyan Naked Monitor"). If you prefer the exact original casing, rename the
folder, the manifest `name`, the CMake `OUTPUT_NAME`, and re-run `vrpathreg`.

## Layout

```
shiyan_nakedmonitor/                 <- register THIS folder with vrpathreg
├── driver.vrdrivermanifest
├── CMakeLists.txt
├── src/                             <- source (not needed at runtime)
├── resources/
│   ├── settings/default.vrsettings  <- all tunables
│   └── localization/localization.json
└── bin/win64/driver_shiyan_nakedmonitor.dll   <- build output
```

## Build

```powershell
cmake -A x64 -B build
cmake --build build --config Release
```

The DLL is written to `bin/win64/driver_shiyan_nakedmonitor.dll`.
(If the SDK is not at `../openvr-master`, pass `-DOPENVR_ROOT=<path>`.)

## Install / register

```powershell
# Run from the SteamVR bin folder (contains vrpathreg.exe)
vrpathreg.exe adddriver "<absolute path>\shiyan_nakedmonitor"
```

Then start SteamVR. The window is placed automatically on the monitor whose
resolution equals `window_width x window_height`; otherwise it uses
`window_x/window_y`.

## Configuration

All parameters live in `resources/settings/default.vrsettings`. To change them
without touching the shipped defaults, put the same keys in `steamvr.vrsettings`
under the same sections.

| Key | Meaning |
|---|---|
| `window_width` / `window_height` | Full SBS panel resolution (7680x2160) |
| `render_width` / `render_height` | Per-eye render target (3840x2160) |
| `auto_place_window` | Auto-find the panel monitor by resolution |
| `window_x` / `window_y` | Manual placement when auto is off |
| `display_frequency` | Panel refresh, **must be > 0** |
| `ipd_meters` | Panel's *designed* stereo baseline (0.065), not the wearer's IPD |
| `eye_depth_meters` | Viewer-to-panel distance (0.5) |
| `vsync_to_photons` | Compositor-to-photon latency estimate |
| `seated_height_meters` | Fixed eye height for the static pose |
| `fov_horizontal_deg` | Per-eye horizontal FOV (total angle) |
| `fov_vertical_deg` | Per-eye vertical FOV; `0` = derive from horizontal + aspect |
| `swap_eyes` | Set true if left/right come out mirrored |
| `is_on_desktop_property` | Keep **false**. `true` triggers the SteamVR error -202 prompt |
| `display_debug_mode` | `true` = compositor uses a window (default). `false` = exclusive fullscreen |
| `display_mode` | `extended` (default) = compositor-owned desktop window; `direct` = the driver owns the output window + D3D11 swap chain |
| `direct_color_mode` | `srgb` (default) = sRGB SRV + sRGB swap chain, so the hardware does the linear<->gamma round trip; `raw` = byte-exact copy (non-sRGB SRV + non-sRGB swap chain) |
| `direct_vsync` | `true` (default) = `Present` blocks on vblank, pacing the compositor to the refresh. Only used when `display_mode = direct` |
| `direct_window_topmost` | `false` (default) = the output window behaves like a normal window, so Alt+Tab and other windows work (the window itself appears in Alt+Tab). `true` keeps the VR image above everything (for a dedicated panel), which hides other windows behind it |
| `direct_hide_cursor` | Initial OS-pointer state over the output window (`true` = hidden, so the desktop cursor is not part of the SBS image). Press **H** at runtime to toggle show/hide |

### About the vertical FOV

`fov_vertical_deg` defaults to `0` (auto). With a 16:9 per-eye sub-image and a
60 deg horizontal FOV, the geometrically consistent vertical FOV is about
**36 deg**, not 60 deg. A mismatched vertical FOV stretches the world
vertically. Set an explicit value only if your panel truly is 60x60.

## Tuning checklist

1. Confirm the window lands on the SBS panel (`swap_eyes` if mirrored).
2. Verify depth: adjust `ipd_meters` toward the panel's real design baseline.
3. Verify FOV: match the panel's specified horizontal FOV.
4. If a game refuses to start, check that `display_frequency` > 0.

## Troubleshooting

### HMD stuck at "locating" / SteamVR keeps demanding Room Setup

A driver that supplies its own tracking **must** declare a tracking universe and
**should** provide its own chaperone (play area). Without them SteamVR cannot
relate the driver's raw tracking space to the seated/standing origins, so it
reports the HMD as "locating" and repeatedly asks for Room Setup — which can
never complete, because a fixed-viewpoint panel never moves.

The driver handles this by:
* setting `Prop_CurrentUniverseId_Uint64` (`universe_id`, default 2), and
* setting `Prop_DriverProvidedChaperoneJson_String` (`provide_chaperone`, default
  true) with a minimal chaperone whose floor is at raw `y = 0`.

Tunables: `universe_id` (never 0 or 1), `provide_chaperone`, `play_area_meters`.

### HMD keeps entering standby ("move the headset to wake")

SteamVR treats an HMD as "active" only when it moves, when a boolean input
changes, or when an app launches — and the HMD standby timeout defaults to 5
seconds. A fixed-viewpoint panel never moves, so it sleeps.

The driver avoids this by exposing an always-on `/proximity` boolean input
(`Prop_ContainsProximitySensor_Bool = true`). Per Valve's "Device sleep states"
documentation, **a device never goes to sleep while `/proximity` is true**, so
the panel stays awake indefinitely. No configuration needed.

If you ever want it to sleep normally, set `Prop_ContainsProximitySensor_Bool`
back to false (or stop updating the component).

### SteamVR error -202 ("Make sure your headset's display is in fullscreen mode")

`-202` is `VRInitError_Driver_HmdUnknown`, but SteamVR surfaces it as "the
compositor could not create its fullscreen window". It is NOT about the app's
window mode. Fixes, in order:

1. `is_on_desktop_property` must be **false** (Valve's sample does this; setting
   it true makes vrmonitor raise the warning).
2. `display_debug_mode` must be **true** so the compositor uses a borderless
   window instead of requesting exclusive fullscreen.
3. The window position/size must match a real monitor. Check the driver log line
   `shiyan_nakedmonitor: auto-placed window on monitor at (x, y)`. If it says
   "no monitor matched", either the panel is not a single 7680x2160 monitor on
   the Windows desktop, or DPI scaling changed the reported coordinates — set
   `auto_place_window=false` and enter `window_x`/`window_y` manually.
4. `display_frequency` must match the panel's actual refresh (e.g. 60).

Valve's own notes confirm that the `IVRDisplayComponent` "extended desktop" path
is buggy on current SteamVR (openvr issue #1727 has a maintainer saying no
commercial headset uses it). If the window keeps landing on the primary display
or stays red, the reliable fix is to switch the driver to
`IVRDriverDirectModeComponent` or `IVRVirtualDisplay`, where the driver owns the
window/texture instead of relying on the compositor's window placement.

## Direct mode (experimental)

`display_mode = "direct"` switches the HMD from `IVRDisplayComponent`
(extended desktop, compositor-owned window) to `IVRDriverDirectModeComponent`:
the driver creates its own borderless window on the panel, a D3D11 device and a
swap chain, and presents the frame itself. The compositor no longer creates its
"Headset Window", so the whole class of `-202` / window-placement problems
(e.g. the window parked at `(32767, 32767)`) disappears.

This is a **skeleton**, built in two stages:

1. **Stage 1 (done):** window + D3D11 device + swap chain + `Present` path.
2. **Stage 2 (done):** `SubmitLayer` resolves the compositor's per-eye textures
   (the same shared textures this driver created) and `Present` draws them side
   by side into the backbuffer - left eye into the left half, right eye into the
   right half - so the window shows the real SBS image. `swap_eyes` flips the
   halves. A tiny full-screen-quad shader (compiled at runtime via
   `d3dcompiler`) samples each eye texture; the texture bounds from
   `SubmitLayer` are honoured.

Limitations of the current skeleton:

* `CreateSwapTextureSet` allocates the shared textures the compositor renders
  into. The sharing flavour is configurable via `direct_texture_share`:
  - `shared` (default): legacy handle - `D3D11_RESOURCE_MISC_SHARED` +
    `IDXGIResource::GetSharedHandle`, `unTextureFlags = 0`.
  - `nthandle`: NT handle - `D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
    D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` + `CreateSharedHandle`,
    `VRSwapTextureFlag_Shared_NTHandle`. (The `KEYEDMUTEX` flag is mandatory:
    without it the runtime strictly validates and rejects the texture with
    `E_INVALIDARG`.)
  If the preferred mode cannot be created at all, it falls back to the other
  once and remembers which one worked.
* `Present` implements the documented sync-texture handshake: the compositor's
  sync texture is opened once via `OpenSharedResource`, its keyed mutex acquired
  (`AcquireSync(0, 10)`) to signal "frame taken", then released again before the
  vsync-blocking `Present`.
* The swap textures are shared with the legacy handle
  (`D3D11_RESOURCE_MISC_SHARED` + `IDXGIResource::GetSharedHandle`) - the only
  flavour this SteamVR build opens (`OpenSharedResource`). NT handles are rejected
  (`VRInitError_Compositor_OpenDriverDirectModeResolveTextures`) and keyed-mutex
  textures render black, so the textures carry no mutex and tearing remains
  possible.
* Colour: the eye textures are `_SRGB`. In the default `srgb` mode the SRV keeps
  the texture's own sRGB format (sampling returns linear) and the swap chain is
  `_SRGB` (the GPU re-encodes on write), i.e. a correct linear<->gamma round trip.
  `raw` mode instead drops the sRGB suffix everywhere for a byte-exact copy.
* Frame pacing is done by the swap chain's vsync (`direct_vsync`, default on).
  With vsync off the compositor free-runs and pegs the GPU.
* `DestroyAllSwapTextureSets` drops every set (the pid is not tracked).
* If `Present` fails with a device-removed/reset error, the output is torn down
  and rebuilt on the next call (the compositor's swap textures are *not*
  recreated - a known gap).

## Virtual controllers (keyboard + mouse)

Two `TrackedDevice_Controller` devices (left/right) are driven by a global
keyboard/mouse capture. `VrSim` (`src/vr_sim.cpp`) owns the simulated head and
both hands plus an "active target", ported from SyOpenXRRuntime's `vr_sim`:

* **vr_sim** (target based):
  * `1` / `2` / `3` activate **head / left hand / right hand**, `Tab` cycles;
  * mouse movement turns the **active hand incrementally** (no angle ceiling,
    and it keeps working at the screen edge because raw input reports movement);
    with the head as the target the mouse turns the head the same way (holding
    MMB does the same while a hand is active);
  * `WASD` / `QE` move the **active target** in view space, `Shift` = 3x;
  * `F` toggles mouse-translate mode (mouse moves the target sideways/up-down);
  * `Z` resets both hands to `hand_reset_distance_m` in front of the head
    (offset vertically by `hand_reset_height_offset_m`), facing the head;
  * wheel moves the active hand forward/back (0.1 m per notch).
* **natural_mouse** (`G` toggles): the mouse is a panel pointer, both hands aim
  along the eye -> cursor ray, `WASD` walks the head.
* Buttons in both modes: LMB = select/trigger, RMB = squeeze/grip, `M` = menu,
  `X` / `C` = face primary/secondary (left X/Y, right A/B), `R` = thumbstick
  click, arrow keys = thumbstick, wheel = thumbstick in natural_mouse.
* `H` shows/hides the OS pointer over the direct-mode output window.

Tunables live in `shiyan_nakedmonitor_controller`: `interaction_mode`,
`primary_hand`, `head_look_invert`, `sim_head_look`, `sim_head_move`,
`sim_hand_look`, `sim_hand_move`, `hand_reset_distance_m` / 
`hand_reset_height_offset_m` (Z reset distance and height relative to the head),
`cursor_recenter`, `ray_origin_*`, ...

## Gamepads (XInput -> virtual controllers)

A physical gamepad is polled with XInput inside `vrserver` and mapped onto the
same virtual left/right controllers that the keyboard/mouse drives. This is
deliberate:

* a custom driver cannot bind the reserved `/user/gamepad` alias
  (openvr issue #923), and
* SteamVR's built-in `driver_gamepad` default bindings only cover a handful of
  SteamVR Home actions (menu / teleport / turn), so most buttons do nothing in
  Home and in games.

| Gamepad | Action |
|---|---|
| Left stick | Move the active target (`WASD`) |
| Right stick | Turn the active target (head or hand) |
| `LT` / `RT` | Trigger / grip (active hand, analog) |
| `A` / `B` | Face primary / secondary |
| `X` | Application menu |
| `Y` | Show/hide the key-hint overlay (`V`) |
| `Back` | Reset both hands (`Z`) |
| `Start` | Toggle `vr_sim` <-> `natural_mouse` (`G`) |
| `LB` | Move 3x faster (`Shift`) |
| `RB` | Thumbstick click (`R`) |
| D-pad | Thumbstick |
| Rumble | `VREvent_Input_HapticVibration` -> `XInputSetState` |

Target switching stays on the keyboard (`Tab` / `1` / `2` / `3`); the pad has
no target-switch buttons.

Setup:

1. **Disable Steam Input for SteamVR** (Steam -> Library -> SteamVR ->
   Properties -> Controller -> Disable Steam Input; or set
   `UseSteamControllerConfig` to `0` for app `250820` in
   `userdata/<id>/config/localconfig.vdf`). Otherwise Steam grabs the pad and
   `XInputGetState` in `vrserver` returns `ERROR_DEVICE_NOT_CONNECTED`.
2. **Disable SteamVR's own gamepad driver** so it does not consume the pad too:
   `"driver_gamepad": { "enable": false }` in `steamvr.vrsettings`.
3. Keep the virtual controllers enabled:
   `"shiyan_nakedmonitor_controller": { "enabled": true }`.

Settings (`shiyan_nakedmonitor_gamepad`): `enabled`, `slot` (`-1` = first
connected pad), `deadzone`, `rumble`, `rumble_gain`.
`sim_stick_look_yaw` / `sim_stick_look_pitch` (degrees per second, default
130 / 80) live in `shiyan_nakedmonitor_controller`.

## Key-hint overlay (direct mode)

`V` shows/hides a set of PNG hint pages drawn over the SBS output. While the
overlay is visible, `A`/`D` or `Left`/`Right` page through the images (wrapping)
and those keys are withheld from the input simulator (no strafing/thumbstick).

Settings live in `shiyan_nakedmonitor_help`:

| Key | Meaning |
|---|---|
| `enabled` | Master switch |
| `images` | Comma-separated PNG file names, one page each, under `resources/images/` |
| `width_ratio` | Page width as a fraction of one eye half (default 0.9) |

Each page is centered in both eye halves and aspect-fit (never stretched); PNG
alpha is blended over the SBS image. Only available with `display_mode = direct`.

## Not yet implemented

* Real six-DoF tracking (the pose is simulated from keyboard/mouse).
* Physical controllers (both hands are virtual devices).
