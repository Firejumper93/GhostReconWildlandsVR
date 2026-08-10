// WeaponDraw.h - name the weapon's own draw calls, log only.
//
// WHY THIS EXISTS (2026-08-09, user route)
// ----------------------------------------
// docs/PLAN-controller-gun.md step 5 ranks the ways to HIDE the real gun. Route
// A (the render node's own hidden bit) needs the runtime pointer from the wskel
// pick to that node, which is gated behind the Track B census. Route B, GPU-layer
// draw suppression, is NOT gated: the d3d11 draw detours are already deployed and
// catch every context (DrawHook.h). What route B has always been missing is the
// per-weapon draw FINGERPRINT, and the plan says that fingerprint needs "ONE
// RenderDoc capture of an equipped gun". RenderDoc is proven dead on this title
// across four routes (session 22), so this probe derives the fingerprint from the
// game itself instead.
//
// THE METHOD: a TIMELINE, which needs no symbols and no theory about what a
// weapon draw looks like. One keypress starts a 32 second recording. Every
// indexed draw sets the bit for the current second in a mask keyed by its INDEX
// COUNT. The user swaps weapon once, at any moment, and the analysis finds the
// swap itself: draws that stop at the same second are the weapon that was put
// away, draws that start there are the one taken out. Anything present for the
// whole run is scenery and is never listed.
//
// Holstering is NOT the transition to use: it RELOCATES the weapon model to the
// back or leg and keeps drawing it. Entering a vehicle does hide weapons but
// swaps the whole scene, which floods the result with unrelated changes. A
// weapon swap holds scene, pose and camera constant, so the weapon is the only
// thing that changes.
//
// What this names is the weapon plus its attachments, which is exactly the set
// we want to suppress: build 25's "floating suppressor" says attachments are
// the part the engine's own proximity cull leaves behind.
//
// WHY ONE KEYPRESS. Three runs were lost to a multi-press protocol (logs
// grwxr-9440, -23144, -26664): the user is in a headset and cannot see the log,
// so every press the protocol needed was another chance to lose the run. The
// recorder now times itself and locates the transition in the data.
//
// The index count is the whole fingerprint on purpose: it is the ONLY per-draw
// datum available without a COM call, and rule 8 forbids COM in a per-draw hook.
// The per-draw path here is one relaxed atomic load, a multiply, and one atomic
// increment. Collisions between two meshes with identical index counts are
// possible; the suppression build that consumes this must therefore verify in
// the headset (the wrong fingerprint hides the wrong object, visibly and
// reversibly), and can add a second discriminator later if one is needed.
//
// AUTHORITY: this file writes NOTHING. It cannot change what the game draws.

#pragma once

#include <cstdint>

#include "DrawHook.h"

namespace grwxr {
namespace weapondraw {

// Called from the single draw recorder for every detoured draw, on whatever
// thread the game draws from. Rule 8: no logging, no locks, no allocation, no
// COM. Inert (one relaxed load) until a bucket is armed.
void on_draw(drawhook::Kind kind, uint32_t index_count);

// 1 Hz watchdog thread. Owns the hotkey, the recording clock and all logging.
// NUMPAD 7 starts one 32 second run; nothing else is ever pressed.
void poll();

}  // namespace weapondraw
}  // namespace grwxr
