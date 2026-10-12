// Census.h - BUILD 242: the camera / view machinery census. OBSERVATION ONLY.
//
// THE QUESTION. The mod alternates eyes (one fresh eye per present). Whether the
// engine could be made to render BOTH eyes in one frame depends on how many
// cameras it already renders per frame, in which views, onto which surfaces, and
// how the per-camera task pool is used. A static read of the TU25 binary
// (C:\Claude\scratch-tu25-render\REPORT.md) named the functions; this build
// counts them at runtime and dumps what they are handed.
//
// WHAT IS HOOKED (four bodies, all reached through E9 + int3 thunk slots):
//   0  0x0D642F60  top-level graphics-frame graph builder (thunk 0x013626A0)
//   1  0x0E11ACA0  Renderer vtable +0x88, the PER-CAMERA entry (thunk 0x0154B900)
//   2  0x0E1212B0  BuildCmdList task body (thunk 0x0154EB50)
//   3  0x0E0FD6A0  ExecuteCmdList task body (thunk 0x0153F4A0)
// The slot head is swapped with ONE aligned 8-byte compare-exchange (the
// operand bytes go into the dead padding first), the redirect is a tail-jump
// stub (ProbeStub.asm, grwxr_census_entry_N), so the real body sees its own
// rsp, return address and arguments. Nothing in the engine's data is written.
//
// WHEN. Like the anatomy capture (Anatomy.h) but later, so the two windows do
// not overlap: the hooks go in 50 s after the first stereo layer and the window
// of 240 presents opens in the same tick; after the window closes the thunks are
// put back and the dump is written. One window per run.
//
// RULE 8. The recorder logs nothing, takes no lock and allocates nothing. Every
// dereference of engine memory is VirtualQuery-checked and SEH-guarded. Counters
// and event tables are preallocated statics written with relaxed atomics; a
// background thread writes GRWVR\census-<pid>.txt once the window closes.
//
// RULE 7. Only the TU25 build is known (table keyed by PE timestamp and size).
// Each target's full 16-byte thunk slot and 16 body bytes are compared to the
// bytes read from the TU25 GRW.exe on disk; ANY mismatch installs nothing for
// that target, logged once. The other targets still install.
//
// cfg: census_capture = 0 disables it. The code default is ON.

#pragma once

#include <cstdint>

namespace grwxr {
namespace census {

void set_enabled(bool on);   // cfg census_capture, any thread
bool enabled();

// Render thread, after xrEndFrame submitted a projection layer. One relaxed load
// per frame, and one store the first time.
void on_stereo_submit();

// Render thread, from the Present hook, after the frame counter was bumped.
void on_present(unsigned long long frames);

// Init thread, once a second. Installs the hooks, opens the window, and hands
// the finished window to the dump thread.
void tick();

// BUILD 243: the second-camera experiment (Cam2.h) shares hook 0, the graph-frame
// builder thunk, so the two can run together. Init thread only.
//   acquire_graph_hook: runs the same TU25 gate and the same two-phase verified
//     install as the census, independent of any OpenXR session, then makes the stub
//     call `cb` on the builder's thread at every builder entry, BEFORE the builder
//     body runs. After this the census never restores hook 0. false = nothing was
//     written and *why says what refused.
//   release_graph_hook: stops the callback and drops the stub back to the pass-through
//     fast path. The redirect itself stays in the thunk slot (harmless).
bool acquire_graph_hook(void (*cb)(), const char** why);
void release_graph_hook();

// BUILD 245: which ProjShadow0..7 pass descriptors the BuildCmdList hook (hook 2) has seen.
//   projshadow_watch(true)  starts collecting; false stops and clears. Off by default. Init thread.
//   projshadow_take(&live)  returns the bit mask (bit i = ProjShadow<i>) seen since the last take and
//                           clears it. *live is true only while hook 2 is actually recording (the
//                           census window is open); when false an empty mask means "not observed",
//                           NOT "no ProjShadow pass ran".
void     projshadow_watch(bool on);
uint32_t projshadow_take(bool* live);

}  // namespace census
}  // namespace grwxr
