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

namespace grwxr {
namespace wp {

// Signature-scan the setter, verify the thunk, install the observer.
// A miss logs loudly and installs nothing (rule 7).
bool install();

// 1 Hz, init thread: log and reset the correlation table.
void drain();

}  // namespace wp
}  // namespace grwxr
