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

// Build 19: the aim injection surface (supersedes build 17's one-shot bump).
// axis 0 = yaw, 1 = pitch, deltas in the ENGINE's radian units. aim_arm
// queues one delta for consume-once application to the next engine setter
// call: returns 1 armed, 0 busy (the previous delta has not been consumed
// yet), -1 hook not installed. aim_pending says whether a delta is still
// queued, which is how the render-thread pump knows a previously armed delta
// was absorbed and can be added to the accounting it publishes through
// headpose::set_aim_cum.
bool aim_available();
bool aim_pending(int axis);
int  aim_arm(int axis, float delta_engine_units);

// Build 39.1: the player camera's world position, read from the camera object
// itself (Camera+0x000 row 3) rather than from the VR-only published copy.
//
// RETURNS FALSE when no camera position is available (no mode-0 camera seen
// yet, or the read faulted). A correlation probe MUST check this: comparing
// distances against a zero position silently produces "nothing is near the
// player", which is indistinguishable from a real negative result.
//
// The read is deliberately unsynchronized, so a caller on another thread can
// see a position torn between two frames. That error is centimetres.
//
// BUILD 98: BOTH OF THESE NOW PREFER THE PER-FRAME ENGINE LATCH. Read the long
// comment above base_frame_engine in CameraProbe.cpp before changing either.
// The short version: since build 89 we WRITE Camera+0x000 row 3 about three
// times a frame, so reading it live returns whichever of two values, 1.9 m
// apart, the read happened to land between. Everything these two feed is ours,
// and all of it wants the engine's own camera, not the row we are editing.
bool base_pos(float out[3]);

// Build 45: the player camera's full world frame, read from the camera
// object (Camera+0x000). rot is the 3x3 rotation packed row-major:
// row 0 = right, row 1 = forward, row 2 = up (row-vector convention,
// world z up; docs/RE-notes.md "camera pose (world) matrix"). pos is the
// translation row. Returns false when no mode-0 camera has been seen, the
// read faults, the position fails the build-40 sanity bounds, or any
// rotation row is not unit length (mid-swap garbage reads fine; hazard from
// build 39.1 defect 1). Unsynchronized like base_pos; a torn frame is
// centimetres/millidegrees and callers using it for marker placement
// tolerate that.
bool base_frame(float rot[9], float pos[3]);

// Build 64: weapon-skeleton identifier. set_wskel arms the 1 Hz census and
// candidate pick on the drain thread (cfg wskel, hot-reload); off clears the
// pick. wskel_marker returns the picked weapon-skeleton instance's live
// world position (guarded per-frame re-read, so the marker tracks the gun);
// false while disarmed or between valid picks.
void set_wskel(bool on);
bool wskel_marker(float out[3]);

// Build 65: the weapon-skeleton write test (cfg wskel_write). While on and
// the pick is a DRAWN gun, the skeleton recorder adds +0.30 m of height to
// the instance origin and its copy each update, hard-capped; a rising edge
// resets the cap. Requires wskel on for the pick to exist.
// Build 66: the value selects the write target as well as arming it.
//   0 = off, 1 = instance origin (+0x120/+0x250, build 65's proven negative),
//   2 = pose root translation ([[pick+0x238]+0x08], the model-space carrier).
void set_wskel_write(int mode);

// Build 67: THE GUN-ROOT BONE WRITE (cfg wgun, wgun_dz). While armed, the
// player's gun-root bone translation is offset by wgun_dz metres at
// Skeleton::PublishAttachments, the instant before the engine composes the
// held weapon's placement from it. mode: 0 off, 1 Fake_gunroot (visual),
// 2 FakeGunRoot_Gameplay (authoritative twin). One binary question: does the
// rendered gun move.
// Build 80 adds mode 3: ROTATE node 10 so the bone's +Y (the barrel, VERIFIED
// build 79) is set ON the controller ray, absolutely rather than as a delta
// from the game's aim. Modes 1 and 2 remain the verified constant lift.
void set_wgun(int mode, float dz);

// Build 80: skeleton rule 5's filter and clamp for the rotation. smooth is the
// one-pole weight per call (0.01..1.0, default 0.25); maxstep_deg hard-caps the
// per-call angular step (default 5.0, which at ~144 calls/s is 720 deg/s: far
// faster than a hand moves, slow enough that a tracking dropout cannot snap the
// gun across the world in a single frame). cfg wgun_smooth, wgun_maxstep_deg.
void set_wgun_filter(float smooth, float maxstep_deg);

// 2026-08-13. TWO-HANDED AIM: the barrel points from the rear hand toward the
// front hand instead of along the rear controller's own forward, blended in by
// hand separation so it degrades to the one-handed behaviour when the hands
// come together (where a two-point direction goes noisy). cfg wgun_twohand.
//
// ROLL: twisting the wrist twists the gun about its barrel. Applied as a
// rotation about the aim axis, which fixes that axis exactly, so roll cannot
// move the point of aim. wgun_roll_deg trims the unknown constant offset
// between the model's up axis and the controller's. cfg wgun_roll,
// wgun_roll_deg.
void set_wgun_twohand(int on);
void set_wgun_roll(int on, float trim_deg);

// Build 81: gun POSITION rides the controller, so the weapon can be raised to
// the eye instead of pivoting where the animation left it. Applies only in
// wgun mode 3 and is OFF by default, so the verified rotation cannot regress
// behind it. scale is metres of gun per metre of hand; clamp_m is a hard cap
// on how far the gun may sit from the engine's own placement, which bounds
// every possible tracking failure; smooth is the one-pole weight per call.
// cfg wgun_pos, wgun_pos_scale, wgun_pos_clamp, wgun_pos_smooth.
void set_wgun_pos(int on, float scale, float clamp_m, float smooth);

// Build 99: WHERE THE GUN SITS IN YOUR HANDS.
//
// Build 81 put the gun ROOT on the right controller. The root is not the grip:
// it is wherever the model's origin happens to be, so the weapon sits at some
// fixed but arbitrary offset from your fist and no amount of aiming tuning
// fixes it. These are that offset, in the WEAPON'S OWN frame, so they rotate
// with the gun and mean the same thing at any angle.
//
//   fwd  metres along the barrel (+Y, [VERIFIED] build 79 as the bore axis).
//        POSITIVE pushes the gun forward through your hand, so your hand ends
//        up further back along the weapon.
//   lat  metres along the weapon's +X.
//   up   metres along the weapon's +Z.
//
// `two` is the front-hand grip, 0..1, and it is a different kind of thing. With
// both hands tracked the rear hand fixes position and the front hand fixes
// direction, so the handguard already points AT your left hand but is not
// necessarily AT it: real hands are not always the weapon's own spacing apart.
// This slides the weapon ALONG ITS OWN BARREL until the measured handguard
// point sits on the front hand. `[VERIFIED, 2026-08-13, 112 samples, stable to
// one millimetre]` that point is +0.481 m along the barrel from the root.
//
//   0.0  rear hand only, exactly the build 81 behaviour
//   1.0  the handguard lands on the front hand and the rear grip absorbs all
//        of the spacing error
//   0.5  the error splits between the hands, which is what it feels like to
//        hold a real rifle with your hands slightly off its natural spacing
//
// It is a translation along the aim axis, so like the roll trim it CANNOT move
// point of aim. That is algebra, not tuning.
//
// All four default to zero, so a build carrying this behaves exactly as the one
// before it until a key is pressed.
void set_wgun_grip(float fwd, float lat, float up, float two);
void get_wgun_grip(float* fwd, float* lat, float* up, float* two);

// Build 78: step wgun 0 -> 1 -> 2 -> 0 from a key (NUMPAD 5), so the tester can
// compare the two gun-root bones without removing the headset to edit a file.
// The write DISARMS immediately on every press and re-arms on the next census
// tick (up to 5 s), because the bone index the stub uses is only resolvable
// inside that tick: without the disarm, a press would keep writing the previous
// bone for those seconds and the tester would be judging a stale mode.
void cycle_wgun();

// Build 79: LOG ONLY (cfg wbaxis). While on, every census tick logs the dot
// product of each of the gun-root bone's three world axes with the direction
// the GAME is aiming. The axis that tracks the aim is the barrel, and its sign
// is the direction. Replaces build 74's scoring against camera forward, which
// returned a confident wrong answer because on a canted weapon the gun's right
// axis correlates with gaze better than its barrel does. Writes nothing.
void set_wbaxis(int on);

// Build 68: THE WEAPON PLACEMENT OVERRIDE (cfg wnode, wnode_dz). Substitutes
// the matrix the engine is about to commit for the held weapon, inside
// TransformNode::SetWorldTransform. Not a race: we are the writer.
//   0 = off
//   1 = lift the weapon by wnode_dz metres (mechanism test)
//   2 = the barrel follows the controller ray, position untouched
void set_wnode(int mode, float dz);

// Build 70: the SetWorldTransform census (cfg wnode_census). Observes every
// node the engine places and ranks them by distance to the player's
// Fake_gunroot, so the held weapon's node names itself instead of being
// guessed at by a pointer chain. Log only; run it with wnode = 0.
void set_wnode_census(int on);

// Build 72: mode 3's gate width, metres from the anchor part. Every placement
// inside it rotates with the gun. cfg wnode_radius.
void set_wnode_radius(float r);

// Build 75: cfg wnode_axis. -1 = calibrate automatically, 0..5 = force which
// signed basis row of a weapon part is its barrel (+row0,+row1,+row2,
// -row0,-row1,-row2). Cycling this in the headset settles the engine's
// convention in two minutes and with certainty.
void set_wnode_axis(int idx);

// Build 76: step to the next barrel-axis candidate. Bound to NUMPAD 4 so the
// tester can settle the engine's convention without removing the headset to
// edit a file, judging each candidate against the gun in front of him rather
// than from memory.
void cycle_wnode_axis();

// Build 18: while on, the SetHidden detour forces the head-visibility
// component hidden on every engine call (the engine re-asserts visibility
// every update, so this must be a standing override, not a one-shot). Driven
// from the first-person state each frame; off restores engine behaviour
// within a frame. Safe from any thread; a no-op if the hook did not install.
void set_head_hide(bool on);

// Build 96: zero the head-compose counters.
//
// The drain's "head compose:" line gates on frames != 0, and the counters are
// cumulative for the life of the process. So once the camera write has run at
// all, turning it OFF leaves that line printing its last totals forever, which
// reads as "still composing" when nothing is. That is exactly what
// grwxr-13140.log did at 19:52 onward: 41067 writes over 13695 frames, frozen,
// for a full minute after the write had stopped.
//
// The Numpad 0 toggle calls this on every press, for the same reason
// cycle_aim_barrel resets the barrel counters: the question the log answers is
// about the state just selected, not about a total carried over from the
// previous one. After a toggle to OFF the line becomes the idle branch, which
// names the consequence out loud.
void reset_head_telemetry();

}  // namespace camera
}  // namespace grwxr
