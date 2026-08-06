// WeaponProbe.h - build 39: READ-ONLY observer on the pooled placement
// subsystem's SetTransform.
//
// docs/RE-notes.md "THE POOLED PLACEMENT SUBSYSTEM BEHIND THE BONE GATHER
// CONSUMER" mapped this API offline: every placement goes through one
// int3-slot thunk (0x030AC6A0) into the setter at 0x13E5EA30, carrying
// (ctx, 24-bit handle, 0x40-byte transform, flags), where the transform is
// rows 0..2 rotation matrix and row 3 position. 22 call sites across the
// engine use it, so it is a general "place this object" API.
//
// The user's scope call (session 23): the end state is "the gun rides the
// controller". Bullets already follow the controller (builds 23-25); the
// weapon MODEL does not. If the weapon model is placed through this API,
// its handle is findable by correlation: the probe records which handles
// are set to positions near the camera, every second. The weapon (and its
// attachments) should be near-constant companions; a full miss exonerates
// this subsystem cheaply.
//
// READ-ONLY: the observer records and calls through unchanged. No logging,
// no allocation, no locks on the hot path (rule 8); the 1 Hz drain does the
// logging on the init thread.

#pragma once

#include <cstdint>

namespace grwxr {
namespace wp {

// Signature-scan the setter, verify the thunk, install the observer.
// A miss logs loudly and installs nothing (rule 7).
bool install();

// 1 Hz, init thread: log and reset the correlation table.
void drain();

// Build 45: candidate WATCH LIST, the "identify the weapon by looking at it"
// instrument (CURRENT-STATE, session 24: markers at candidate handle
// positions supersede weapon-swap correlation). drain() picks up to kWatch of
// the closest handles each second, sticky by key so a candidate keeps its
// slot (and therefore its marker colour) across seconds; the hot path keeps
// each watched handle's latest position current. marker() is read by the
// render thread: slot index IS the colour index. The position read is
// deliberately unsynchronized; a torn value is centimetres and this is a
// visual identification aid, not a solver input.
constexpr int kWatch = 6;
bool marker(int slot, float out_pos[3]);

// Build 47: the placement WRITE test. The headset run of 2026-08-05 identified
// the watched handles as BODY ATTACHMENT SOCKETS (MAGENTA stayed on the hand
// through a full holster and idle animation), so the experiment is: rewrite
// one handle's transform argument inside the setter call and observe what
// moves (the gun, the hand, both, or nothing). VRMirror pushes the target
// once per frame from cfg (hot-reloaded, so wp_write_slot=-1 is the instant
// kill switch):
//   handle 24-bit placement handle to write, 0 = use slot instead. Build 48:
//          colour slots RESHUFFLE mid-session (observed 2026-08-05, it made
//          the user write a different handle than intended), so the log-read
//          handle is the reliable way to name a target.
//   slot  -1 off, else the watch-slot/colour index to write
//   mode  1 = lift the handle `up` metres straight up (writability diagnostic)
//         2 = pin the handle's position to the right controller (engine world)
// Rotation is never touched in either mode; position row only. The push
// carries a tick so a stalled render thread stops the write within 250 ms.
void set_write(uint32_t handle, int slot, int mode, float up,
               const float ctrl_world[3], bool ctrl_ok);

}  // namespace wp
}  // namespace grwxr
