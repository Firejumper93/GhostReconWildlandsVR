// VRMirror.h - put the game's rendered frame into the headset.
//
// Phase 3 submitted the D3D11 backbuffer as a quad layer, a flat floating
// screen. Build 9 replaces that with a projection layer: both eyes share the
// one mono swapchain image, each view carries its located eye pose (from
// xrLocateViews) and the fov the game rendered with (published by the proj[2]
// hook through HeadPose). Combined with the build 8 head-rotation camera this
// is the first properly head-locked image: the world holds still while the
// head turns. Still mono, no stereo depth yet.
//
// Threading: OpenXR calls happen on the render thread from inside the Present
// callback, because that is the only thread with a valid device context and the
// only point where the finished frame exists. The frame loop is therefore driven
// by the game's own present rate, not by xrWaitFrame. That is a compromise we
// will have to revisit in phase 8 (frame timing), and it is noted rather than
// hidden.

#pragma once

#include "D3D11Hook.h"

namespace grwxr {
namespace vr {

// Bring up the OpenXR instance, session and swapchains using the device that
// phase 2 captured. Safe to call repeatedly; only the first call does work.
// Returns false and logs loudly on failure, leaving the game untouched.
bool init(const d3d11::State& st);

// Build 38: if init() created the session but the headset was not awake yet,
// call this once a second from the init thread. It begins the session as soon
// as the runtime reports READY and returns true on that one tick, so the
// caller can arm the present callback. Cheap and safe to call every tick.
bool poll_start();

// Called every frame from the Present hook. Copies the backbuffer and submits.
// Must not log (project rule 8).
void on_present(const d3d11::State& st);

void shutdown();

// True once a session exists and frames are being submitted.
bool active();

// Drain deferred diagnostics onto the init thread, same pattern as D3D11Hook:
// the render thread records, our own thread writes the log.
void drain_log();

// BUILD 13b: 1 Hz controller pose/trigger log line, heartbeat thread only.
// Silent until g_input_ok and at least one hand tracks.
void drain_input();

// Build 21: re-read grwxr.cfg when its mtime changes (live tuning without
// hotkeys). Init thread only: it does file I/O.
void poll_config();

// Build 86: live barrel-aim state for the in-headset panel.
//
// This exists because the tester is IN A HEADSET. A test that requires reading
// grwxr.log or editing a cfg file in a text editor is not a test he can run,
// and every barrel-aim result so far has been unobtainable for exactly that
// reason. The panel needs the same three facts the log line carries: did the
// loop run, did it use the real barrel or the fallback, and is the error
// converging.
//
// Plain relaxed reads of the atomics the aim block already writes. No lock and
// no allocation; called once per panel frame, never from a per-draw path.
struct BarrelStatus {
    int      mode;          // cfg aim_barrel: 0 off, 1 trigger-gated, 2 always
    int      src;           // 1 = real barrel, 2 = ctrl_ray fallback, 0 = none
    unsigned frames;        // frames the loop actually drove the aim
    unsigned nodir;         // wanted to drive, had no direction at all
    unsigned noview;        // wanted to drive, engine aim unreadable
    unsigned overcap;       // error exceeded aim_barrel_max and was refused
    float    err_yaw_deg;   // last applied error, degrees
    float    err_pitch_deg;
};
BarrelStatus barrel_status();

// Build 86: headset-reachable hotkeys, same pattern as the existing Numpad 4
// and Numpad 5 cyclers.
//
// The panel can DISPLAY these but cannot change them: menu::set_pointer has no
// caller, so the panel has no working input device. Until that is wired, a
// key press is the only way to change a setting without taking the headset
// off and editing grwxr.cfg in a text editor, and both of these settings are
// ones a test needs to flip back and forth to mean anything.
//
// Both targets are atomics that the aim path already reads with relaxed
// ordering, so an init-thread write is safe. Each logs its new state.
void cycle_aim_barrel();   // Numpad 1: 0 off -> 1 while firing -> 2 always
void toggle_aim_ads();     // Numpad 3: trigger-also-aims on/off (hip fire)

// Build 96: Numpad 0 toggles cam_selector_pose, the camera pose write.
//
// This is the control for the gun jitter reported 2026-08-15. The gun's world
// origin is camera::base_frame's position, which is Camera+0x000 row 3, and
// since build 89 we write that same row about three times a frame. Turning the
// write off makes the row single valued again, which is what build 88 (the
// confirmed motion control) ran on. If the jitter goes with it, the coupling is
// the cause; if it stays, the camera write is exonerated.
//
// It is a hotkey and not a cfg key only because the A/B has to be run WHILE
// walking, in the headset, which a text editor cannot do.
//
// TWO WARNINGS, both earned. First, hazard 24 says tuning belongs in the cfg;
// this is a diagnostic toggle for an open investigation, not tuning, and it
// comes back out with the investigation (rule 6). Second, and this is the
// NUMPAD 5 lesson from 2026-08-15: this key turns OFF first person and stereo
// separation, so a stray press degrades the mod while the tester is blind. Two
// mitigations, both deliberate: Numpad 0 is the double-width key at the bottom
// of the pad, unmistakable by feel and nowhere near the 1 / 3 / 4 block in
// constant use, and every press resets the head-compose counters so the very
// next "head compose:" line states which state is live instead of printing a
// frozen total from the previous one.
void toggle_cam_pose();    // Numpad 0: camera pose write (first person, stereo)

}  // namespace vr
}  // namespace grwxr
