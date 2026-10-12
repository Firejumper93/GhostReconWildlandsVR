// StallWatch.h - BUILD 235.
//
// When the first VR frame does not come back, the mod is not on the stack that
// is stuck, so no amount of our own logging can name what is holding it. This
// takes the only reading that can: suspend every thread in the process, read
// each one's instruction pointer, scan each stack for return addresses that
// fall inside a loaded module, and print the lot as module+offset.
//
// If a foreign OpenXR API layer is holding the render thread, its DLL name is
// in that output (ReShade64.dll, XR_APILAYER_MBUCCHIA_quad_views_foveated.dll,
// and so on) and the argument is over. If nothing foreign appears, the layer
// theory is dead for that machine and the reading says what is there instead.
//
// Three scars inherited from the Dishonored-VR project's own stall watchdog,
// all of which cost them sessions (handoff 4.7):
//   - do NOT filter out threads with no game frames on the stack. The other
//     side of a deadlock is usually parked in ntdll with none.
//   - snapshot EVERY thread, not just the one that looks stalled.
//   - never let the diagnostic exit silently. A blank section reads as an
//     answer, and theirs printed nothing five different ways.
//
// And one of our own: NOTHING is allocated and NOTHING is logged while threads
// are suspended. The logger takes a lock and the allocator takes a lock, and a
// suspended thread may be holding either. Everything is gathered into
// pre-allocated storage, every thread is resumed, and only then is a line
// written.
#pragma once

namespace grwxr {
namespace stallwatch {

// Take the reading and write it to the log. Safe to call from any thread that
// is NOT itself the stalled one (the heartbeat thread is the intended caller:
// it keeps ticking through every wedge we have on record).
//
// `why` is printed at the head of the report so the log says what triggered
// it. Returns the number of threads it managed to read.
int report(const char* why);

}  // namespace stallwatch
}  // namespace grwxr
