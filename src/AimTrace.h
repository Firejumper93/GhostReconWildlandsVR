// AimTrace.h - build 50: THE AIM-READER CENSUS. Log-only.
//
// THE QUESTION IT ANSWERS
//
// "The gun rides the controller" needs the SHOT to leave the barrel we point,
// independent of the view. docs/HANDOFF.md session 24 settled that in this
// engine the aim IS the camera, so anything that steers aim also turns the
// view, which the user's standing constraint forbids. The way out is to find
// where the shot direction is READ, and override only that.
//
// docs/RE-notes.md "THE ABSOLUTE AIM ANGLE EXISTS" gives us the object to
// watch: a persistent absolute yaw/pitch pair reached as [context+0x20]+0x48,
// written by the look input AND by recoil (the strongest evidence available
// that it is the authoritative aim heading rather than a camera-only value).
// Its four accessors are all virtual-dispatch stubs:
//
//     get yaw   vtable +0x5B0    set yaw   vtable +0x570   (hooked, build 17)
//     get pitch vtable +0x5F0    set pitch vtable +0x5D0   (hooked, build 19)
//
// This module hooks the two GETTERS and records WHO CALLS THEM, by caller
// return address, with hit counts. Nothing is modified: each stub re-emulates
// its own dispatch and tail-jumps to the real accessor, exactly as build 17's
// setter stubs have done since session 19 with no anti-tamper reaction.
//
// WHY NOT THE HARDWARE-BREAKPOINT TRACER
//
// The designed-since-session-24 plan was the Cyberpunk DR0 access tracer
// (reference/cyberpunk-vr-port/src/red4ext_plugin/weapon/weapon_aim_hook.h
// lines 18-109): debug registers on all threads via Toolhelp32 +
// SetThreadContext, single-step exceptions caught in a VEH. It answers the
// same question, but debug registers are a classic anti-debug tripwire and
// this title ships Denuvo, whereas Cyberpunk does not. The getter census gets
// the same list (callers of the aim angle, with counts, gated to the trigger)
// through a hook family with 26 sessions of clean history on THIS binary.
// The DR0 tracer remains the fallback and is still designed; take it only if
// this census comes back empty, which would itself be the finding that the
// shot reads its direction by some route other than these accessors.
//
// READING THE RESULT
//
// Each row is one calling code address, as an RVA, with three counts:
//   n=     every call from that site,
//   fire=  calls made while the merged right trigger was held (a shot window),
//   plr=   calls whose object is the one the LOOK INPUT writes, i.e. the
//          player's own aim angle rather than an AI unit's.
// A per-frame camera or renderer read has a large n and a fire count in
// proportion to how long the trigger was held. THE PER-SHOT READER IS THE ROW
// WITH A SMALL n WHOSE HITS ARE ALMOST ALL INSIDE fire, at roughly one or two
// per round fired. That is the same discriminator the reference used.
//
// RULE 8. The recorder runs on the game's own threads. It does no logging, no
// allocation, no locks and no calls: a bounded linear scan of a fixed table
// and plain aligned stores. Rows are written without synchronisation, so two
// threads racing can duplicate a row or drop one increment. For a census that
// is a rounding error, and it buys us freedom from a lock on a hot path.

#pragma once

#include <cstdint>

namespace grwxr {
namespace aimtrace {

// Verifies and redirects both getter dispatch stubs, using the RVAs selected
// by the build pin. Any byte mismatch installs nothing for that stub and logs
// loudly (rule 7); the rest of the mod is unaffected either way.
bool install();

// Called once per second from the init thread; prints the census. Silent
// unless cfg aim_trace is on. Safe before install() or after a failed one.
void drain();

// cfg aim_trace: 0 = record but stay quiet, 1 = print the census once a second.
// Recording always runs, so arming the log mid-session still shows the whole
// history rather than starting from zero.
void set_logging(bool on);

// Build 62: THE CONTROLLER RAY. VRMirror publishes the right controller's
// aim direction in ENGINE WORLD coordinates once per frame (the same
// head-local-to-game-basis mapping the hand markers and the build 47 write
// verified in the headset). ok=false means "not tracked this frame"; the
// last good ray is NOT held, a spawn with no fresh ray flies unmodified.
void set_ctrl_ray(const float dir[3], bool ok);

// Build 68: read back that ray (engine world space). False while no valid
// controller ray has been published.
bool ctrl_ray(float out[3]);

// Build 81: the right controller's POSITION in the same engine world space,
// published from the same pass as the ray so origin and direction can never
// disagree. False until a valid controller pose has been published.
void set_ctrl_pos(const float p[3], bool ok);
bool ctrl_pos(float out[3]);

// Build 72: the direction the GAME is aiming, engine world space, published in
// the same per-frame pass as the controller ray. The weapon rotation is the
// delta between the two, so it needs no knowledge of the engine's axis order.
void set_view_fwd(const float dir[3], bool ok);
bool view_fwd(float out[3]);

// cfg bullet_ctrl: 1 = every player round is relocated, frame by frame,
// onto the controller ray captured at its spawn. The engine keeps drop and
// drag (its own per-frame step length and deviation are preserved); only
// the LINE the round travels is replaced. 0 = never touch the round.
void set_bullet_ctrl(bool on);

// Build 51: THE DISCRIMINATING TEST. Adds a constant offset, in radians, to
// the aim angle THE SHOT READER SEES, and to nothing else: the camera, the
// look-input integrator and every other reader keep getting the true value,
// because the adjustment is gated on the two verified per-shot call sites.
//
// It is self-diagnosing, which is why it comes before any controller wiring:
//   bullets move, view does not  -> this IS the shot direction. Wire the
//                                   controller to it next, and the gun can
//                                   finally aim where your hand points.
//   nothing moves                -> the pair is read but not used for the
//                                   shot; the hunt moves one consumer down.
//   the view moves too           -> the site feeds the camera as well, so
//                                   this lever violates the standing
//                                   constraint and is abandoned.
// 0,0 disarms. Refuses to arm on an axis whose site did not verify.
void set_shot_offset(float yaw_rad, float pitch_rad);

// ALTERNATING MODE. The offset's sign flips on every round, so consecutive
// shots land either side of wherever the gun was pointed. This exists because
// the user has NO CROSSHAIR: with nothing to measure an offset against, the
// only readable question is "one hole or two", and alternation supplies its
// own reference. It also separates a real effect from a fixed misalignment,
// because a constant error cannot alternate.
void set_shot_alternate(bool on);

// Build 55: THE AIM ORIENTATION OVERRIDE. Rotates the quaternion that
// GetAimOrientation produces, which is the only function found so far that
// turns the aim SCALARS into a DIRECTION, and which the recoil node dirties
// every time it publishes. deg = 0 disarms; axis 0 = X, 1 = Y, 2 = Z.
//
// The discriminator: IMPACTS move and the VIEW does not means the ballistic
// direction is downstream of this, and we have a true 1:1 absolute aim
// primitive. If the view moves too, this is the camera basis instead, which
// is worth knowing but violates the standing constraint.
void set_aim_quat(float deg, int axis);

// Build 56: THE BULLET ITSELF. The projectile spawn copies [owner+0x140] into
// the round's m_vBulletSimulationDirection. This rotates that field in yaw for
// the duration of the spawn call and restores the engine's own value straight
// after, so no engine state is left modified. deg = 0 is log-only.
//
// If impacts move and the view does not, the shot direction is found AND
// writable, and "the gun rides the controller" becomes a matter of feeding
// this one vector from the controller pose.
void set_bullet_yaw(float deg);

// Build 82: step the spawn-direction yaw 0 -> +20 -> -20 from NUMPAD 4, so the
// "is this the shot direction" question can be answered in the headset. The
// opposite signs make the round its own reference: a constant misalignment
// cannot swap sides, so a real effect is distinguishable from a fixed error.
void cycle_bullet_yaw();

// Build 52, THE CONTROL. Retargets the override at an arbitrary call site
// (0 restores the build-pinned per-shot sites). Its purpose is to point the
// same machinery at a read the engine is KNOWN to act on, so that "nothing
// happened" can be told apart from "our write never landed". Candidates are
// checked exactly like the pinned sites, so a wrong address disarms.
void set_shot_sites(unsigned yaw_rva, unsigned pitch_rva);

// Whether at least one per-shot site verified on this binary.
bool shot_sites_ready();

// Called from the XInput merge with the right-trigger state the GAME sees,
// which is the merged Touch-or-pad value. Marks the shot window that the
// fire= column counts. One plain store; no allocation, no lock.
void set_firing(bool held);

// Restores both stubs.
void uninstall();

}  // namespace aimtrace
}  // namespace grwxr
