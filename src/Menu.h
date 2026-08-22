// Menu.h - the in-game tester panel: settings and capture control.
//
// WHY THIS EXISTS. Every tester-facing knob in this mod has lived in
// GRWVR\grwxr.cfg, and every capture and probe has lived on a bare numpad key
// polled with GetAsyncKeyState. That has two failure modes we have actually
// paid for:
//
//   MISFIRE   - a key does something the tester did not intend, in a state
//               where it is not safe. The 2026-08-05 lobby crash was a Numpad 8
//               double-tap against an unanchored pin and an unreadable head,
//               with nothing on screen to say either was true.
//   NO ACTIVATION
//             - a key is pressed and nothing happens, and the tester cannot
//               tell whether the probe armed, whether it is running, or whether
//               the build even contains it. The result is a wasted headset run
//               and a log we have to read afterwards to find out.
//
// The panel fixes both by making state visible before the action: every capture
// shows whether it is available, why not when it is not, and what it is doing
// once armed.
//
// PATTERN. Adapted from the Halo MCC VR mod (reference/Halo-MCC-VR,
// src/dll/menu.cpp, MIT, (c) 2026 pancreations), which renders Dear ImGui into
// an offscreen texture and shows it on a floating panel in the headset, with
// the controller ray as the pointer. cyberpunk-vr-port's imgui_overlay.cpp is
// the same shape. We follow it because it is proven in two shipping mods on
// this exact problem, and because a panel that lives in its own texture never
// pollutes the game image, the eye captures, or a recording.
//
// SETTINGS GO THROUGH THE FILE, DELIBERATELY. The panel does not call the
// dozen live setters scattered across HeadPose, VRMirror and CameraProbe.
// It edits GRWVR\grwxr.cfg and lets the existing hot reload (vr::poll_config,
// ~1 s) apply it, which is the same path a tester editing the file by hand
// already takes. One code path for configuration, no second source of truth
// that can drift from the file, and nothing new to keep in sync when a key is
// added. The cost is up to a second of latency on a slider, which for a
// settings menu is not a cost.
//
// RULE 8. render() runs on the render thread and does allocate, inside ImGui's
// own arena. This is a deliberate, stated exception, bounded three ways: it is
// called only while the panel is OPEN, so the closed hot path is byte-identical
// to a build without it; the font atlas, device objects and the offscreen
// target are all built in init() on the init thread; and all file I/O and all
// logging happen in poll(), on the init thread. A tester with the panel open is
// standing still in a menu, not measuring frame times.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

namespace grwxr {
namespace menu {

// Panel resolution. 1024x768 is the Halo MCC VR size, and at the panel's
// default 1.2 m distance and 0.9 m width it lands near one texel per headset
// pixel, so text stays legible without a supersampled panel.
inline constexpr int kMenuW = 1024;
inline constexpr int kMenuH = 768;

// Init thread. Builds the ImGui context, the font atlas and the offscreen
// render target on the game's own device. Returns false and logs on failure;
// the mod carries on without a panel (rule 7).
bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx);
void shutdown();
bool ready();

bool is_open();
void set_open(bool on);
bool toggle();

// The VR side owns the controller ray, because it is the side holding the
// poses. It reports where that ray crosses the panel, in normalised panel
// coordinates from the top-left, plus whether the trigger is down. hit=false
// means the ray is off the panel, which parks the cursor rather than leaving
// it stuck on the last widget it touched.
// BUILD 122: gamepad navigation for the panel, fed from XInputMerge with the
// MERGED Touch + physical pad state. The panel had NavEnableGamepad set from
// the day it was written but nothing ever fed it a key, and set_pointer has no
// callers, so until now it opened and could not be operated at all.
// lx/ly are -1..1, buttons is the XInput wButtons mask.
void set_nav(float lx, float ly, unsigned short buttons);

void set_pointer(bool hit, float u, float v, bool pressed);

// Render thread, called from inside Present while the game's pipeline state is
// already saved. Draws one frame of UI into the offscreen texture and returns
// its SRV, or nullptr when the panel is closed or unavailable. The ImGui DX11
// backend backs up and restores the full device state around its own draw, so
// this cannot leak state into the game's next frame.
ID3D11ShaderResourceView* render();
ID3D11Texture2D*          texture();

// Init thread, once per tick. Performs any pending grwxr.cfg write and emits
// the panel's log lines. Keeping both here is what lets render() obey rule 8.
void poll();

// ---------------------------------------------------------------------------
// CAPTURES
//
// A probe is registered here by id, and the module that owns it polls
// fire_pending() exactly as it already polls GetAsyncKeyState, so wiring a
// probe into the panel is one line in that module and changes nothing else.
// The module publishes what it is doing through set_state(), which is what
// turns "I pressed the key and nothing happened" into a status the tester can
// read before and after the press.
// ---------------------------------------------------------------------------

// BUILD 129: these are WIRED now. Until this build fire_pending() had no
// callers anywhere in the tree, so every row on the Captures page set a flag
// that nothing read and the buttons did nothing at all. It was found while
// reclaiming the numpad digits for presets, on the assumption that the panel
// already covered what the keys did. It did not.
enum ProbeId {
    kProbeWeaponDraw = 0,   // WeaponDraw. Was Numpad 7, now panel only.
    kProbePalette,          // PaletteProbe capture window. Numpad Minus, or panel.
    kProbeFirstPerson,      // the FP toggle. Was Numpad 8, now panel only. Lobby crash.
    kProbeRecenter,         // recenter the VR view. Also Home, and Space.
    kProbeCount
};

enum ProbeState {
    kIdle = 0,
    kArmed,
    kRunning,
    kDone,
    kFailed
};

// Owning module, any thread. `count` is free-form (draws seen, frames captured)
// and is shown beside the state.
void set_state(int id, int state, long long count);

// Owning module, its own poll. True exactly once per activation from the panel,
// so it composes with the existing key check instead of replacing it:
//     const bool fire = key_edge() || menu::fire_pending(menu::kProbeWeaponDraw);
bool fire_pending(int id);

// Owning module or the VR side. When a probe is not safe to run, say so and say
// why; the panel greys the button and shows the reason instead of letting the
// tester fire it. Passing nullptr marks it available again. The string must
// have static lifetime.
void set_unavailable(int id, const char* why);

}  // namespace menu
}  // namespace grwxr
