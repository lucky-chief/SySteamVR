//============ Shiyan Naked Monitor OpenVR driver ============
#pragma once

//-----------------------------------------------------------------------------
// Keep Windows' input language English for the VR/SteamVR windows.
//
// Why: the system Chinese IME (Microsoft Pinyin) defaults to Chinese
// conversion mode, so typing into any freshly focused VR window produces
// Chinese text. This module watches the foreground window and, when it
// belongs to one of the configured VR processes, asks it to switch to the
// configured keyboard layout (default 00000409, US English) via
// WM_INPUTLANGCHANGEREQUEST - the input language is per-thread in the target
// process, so its IME stops composing CJK text.
//
// Rate-limited (a window may reset its own layout, so the request is
// re-asserted while it stays focused) and scoped to the configured process
// list, so normal desktop typing is unaffected.
//-----------------------------------------------------------------------------
namespace ImeSwitch
{

// Reads the "shiyan_nakedmonitor_ime" settings section. Idempotent.
void Init();

// Called from the provider's RunFrame(); internally throttled.
void Update();

} // namespace ImeSwitch
