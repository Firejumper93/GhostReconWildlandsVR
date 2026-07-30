// CameraProbe.h - PHASE 4 STEP 2: answer Q9, the main-camera discriminator.
//
// THE QUESTION
//
// Wildlands calls CalculatePerspectiveProjectionMatrix for more than one camera.
// Phase 5 replaces the projection for the camera the player looks through, and
// must leave every other one alone. So we need a test that says "this call is
// the main camera" using only what the hook can see.
//
// The reference implementation uses `farPlane > 1201.f` on Odyssey. That
// constant is certainly wrong here and must be measured, not guessed.
//
// WHAT THIS DOES
//
// Redirects the projection function's jump thunk (see ThunkHook.h) to a
// replacement that records (near, far, fovy, aspect, return address) and then
// calls the real function unchanged. It modifies nothing. The projection matrix
// the engine gets is byte-for-byte what it would have got with the mod absent.
//
// The return address is recorded because docs/RE-notes.md found exactly two call
// sites, in two different engine functions. Grouping the parameter tuples by
// call site tells us whether the camera categories the engine distinguishes
// (the EXTENDED FOV tooltip states third-person and aim-mode cameras are
// treated differently) show up as different call sites, different far planes,
// or neither.
//
// project rule 8: no logging, allocation, file I/O or locks in the hook. The
// replacement writes into a fixed-size table with atomics and nothing else.
// drain() runs on the init thread and does the printing.

#pragma once

namespace grwxr {
namespace camera {

// Locates the projection function by signature, verifies the thunk points at
// it, and installs the read-only probe. Returns false and logs loudly on any
// mismatch, leaving the game unmodified.
bool install();

// Called once per second from the init thread. Prints the table when it
// changes. Safe to call before install() or after a failed install.
void drain();

// Restores the thunk.
void uninstall();

}  // namespace camera
}  // namespace grwxr
