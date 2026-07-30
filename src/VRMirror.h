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

// Called every frame from the Present hook. Copies the backbuffer and submits.
// Must not log (project rule 8).
void on_present(const d3d11::State& st);

void shutdown();

// True once a session exists and frames are being submitted.
bool active();

// Drain deferred diagnostics onto the init thread, same pattern as D3D11Hook:
// the render thread records, our own thread writes the log.
void drain_log();

}  // namespace vr
}  // namespace grwxr
