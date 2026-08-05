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

}  // namespace vr
}  // namespace grwxr
