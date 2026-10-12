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

// 2026-09-01: cfg voice_guide, default 0. Whether the guided anchor test
// ANNOUNCES itself once per session. The test still exists and End still
// starts it; this only stops it inviting the tester into a different
// procedure than the one they are actually running.
void set_voice_guide(int on);

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

// 2026-08-13, for the two-handed weapon.
//
// ctrl_pos_l: the LEFT controller's position, engine world space, from the
// same pass and basis as the right. With both hands in one space the weapon's
// direction can be the line between them, which is how a real long gun is
// actually pointed: the rear hand is the pivot, the front hand aims it.
//
// ctrl_up: the RIGHT controller's up vector. The aim ray is a direction, and a
// direction has no twist about itself, so roll is thrown away the moment the
// controller quaternion is collapsed to a ray. This carries it back. Roll is
// taken from the REAR hand deliberately: it is the hand that actually rolls a
// weapon, and it keeps roll independent of hand separation.
void set_ctrl_pos_l(const float p[3], bool ok);
bool ctrl_pos_l(float out[3]);
void set_ctrl_up(const float u[3], bool ok);
bool ctrl_up(float out[3]);

// BUILD 158, CX-017: the LEFT controller's forward and up, engine world
// space, from the same pass and basis as ctrl_pos_l. Two VECTORS rather than
// the quaternion, deliberately: the XR-to-engine basis change is a signed
// permutation verified for vectors only (see the ctrl_up publish note in
// VRMirror.cpp), and a frame built from forward and up carries everything a
// wrist needs. Consumed by hands_apply so the free hand's wrist can follow
// the real wrist instead of freezing in the animation's brace pose.
void set_ctrl_vecs_l(const float fwd[3], const float up[3], bool ok);
bool ctrl_vecs_l(float fwd[3], float up[3]);

// BUILD 162: the LEFT controller's GRIP-pose +Y and +Z axes, engine world
// space, same pass and basis as ctrl_vecs_l. The grip pose is anatomical by
// OpenXR specification (+X the palm normal, -Z little finger to thumb through
// the curled fingers, +Y by the right-hand rule), which is what lets the hand
// bone be placed absolutely instead of by delta from an arbitrary latch.
void set_ctrl_gripf_l(const float y[3], const float z[3], bool ok);
bool ctrl_gripf_l(float y[3], float z[3]);

// Build 72: the direction the GAME is aiming, engine world space, published in
// the same per-frame pass as the controller ray. The weapon rotation is the
// delta between the two, so it needs no knowledge of the engine's axis order.
void set_view_fwd(const float dir[3], bool ok);
bool view_fwd(float out[3]);
// BUILD 289: shots seen by the shot-direction hook (rewritten plus left alone), all time; log only.
uint32_t shots_total();

// Build 84: WHERE THE BARREL ACTUALLY POINTS, engine world space.
//
// This is not the controller ray. It is the FINAL filtered, two-hand-blended,
// rate-limited target that grwxr_wgun_apply aligns the gun-root bone's +Y onto,
// published from that function after the filter. After the swing the barrel IS
// this vector, so it is the only direction in the process that is guaranteed to
// match what the player sees the gun doing.
//
// It exists so the aim and the drawn weapon come from ONE source. Deriving the
// aim from ctrl_ray instead would reintroduce exactly the divergence class that
// produced the tester's ADS complaint: two consumers filtering the same input
// differently and drifting apart.
//
// Stale-safe: published every wgun call, and the reader checks freshness by
// generation, because with wgun = 0 nothing writes it and a held stale barrel
// would aim the gun at where it pointed minutes ago.
void set_barrel_dir(const float dir[3], bool ok);
bool barrel_dir(float out[3]);

// BUILD 137: the same vector, read WITHOUT consuming the freshness token.
//
// barrel_dir() above keeps its "have I seen this generation" memory in a single
// function-static, which makes it a SINGLE-CONSUMER accessor: a second caller
// would have its reads answered "stale" whenever the first one got there first,
// and on this hardware the first one runs at frame rate. A shot rewrite built
// on it would therefore do nothing on almost every shot, for a reason no log
// line would ever explain. That is the failure mode this project keeps paying
// for, so the second consumer gets its own memory instead.
//
// Hands back the vector and the generation it belongs to. gen == 0 means the
// weapon writer has never published, which is not the same as "not fresh" and
// is why the count is returned rather than a bare bool.
bool barrel_dir_peek(float out[3], uint32_t* gen);

// BUILD 153b: the same barrel direction expressed in the CAMERA'S OWN AXES
// (x right, y forward, z up), published by the weapon writer using the basis
// it already holds, in the same instant.
//
// DRAWING ONLY, and it exists because of a measured bug: a world vector
// re-projected through a basis captured at a different instant swings by the
// rotation between those instants, which the owner saw as the reticle moving
// OPPOSITE a character turn while the gaze ring stayed correct. Consuming
// this needs no conversion, so that class of error cannot recur.
//
// Nothing may AIM from this. The shot chain takes the world vector, and the
// hard baseline guards that path.
void set_barrel_local(const float dir[3], bool ok);
bool barrel_local_peek(float out[3], uint32_t* gen);

// BUILD 153: the PLAYER'S OWN muzzle in engine world space. This is build
// 137's interlock anchor (player-flagged), not the "most recent, any owner"
// muzzle the A/B uses. False until it resolves once.
//
// **IT IS NOT A PER-FRAME VALUE, AND THAT COST A BUILD.** It is published from
// grwxr_muzpt_post, the MUZZLE GETTER hook, and the engine calls that when it
// wants a muzzle point, which in practice means while shooting. Between shots
// it holds a STALE WORLD POSITION. Build 153's reticle drew a ring from it and
// the owner reported the ring "gets turned around if the character moves, it
// will end up behind the player": the ring was pinned to where he last fired
// while he walked away from it. The direction was live; the origin was not.
//
// So: fine for anything that happens AT a shot, wrong for anything drawn or
// evaluated every frame. A per-frame consumer wants a live origin (the camera
// from camera::base_frame) or its own freshness counter.
//
// DRAWING ONLY besides that. It is unsynchronised, so a torn read costs a
// marker a few centimetres for one frame. Anything deciding where a BULLET
// goes must use the interlock itself, never this.
bool player_muzzle(float out[3]);

// BUILD 137: THE SHOT DIRECTION FOLLOWS THE BARREL. cfg shot_dir, default ON.
//
// Build 136 measured eleven shots and settled the question the project had been
// circling since build 59: the round already STARTS at the muzzle in hip fire
// (0.019 m out), and it is only the DIRECTION that is wrong, because the engine
// builds it as normalize(cameraSpacePoint - origin). One vector, written once
// per shot, is the whole defect.
//
// So this replaces that vector, in the caller's own buffer, at the moment
// SetShotLine receives it, with the direction the barrel is actually pointing.
// It is not a steering loop: nothing is nudged and nothing is re-measured, which
// is the difference between this and the eleven runs that never converged.
//
// Three interlocks, and all three must hold or the engine's own vector is left
// exactly as it was:
//   1. the shot must start within a few centimetres of the PLAYER'S OWN muzzle,
//      which is the owner test (an NPC's shot starts at the NPC's barrel) and
//      simultaneously excludes ADS shots, whose origin is the camera and whose
//      direction cannot be fixed without also moving the origin;
//   2. the weapon writer must have published this second, so a holstered or
//      unarmed gun cannot aim the shot at where it pointed a minute ago;
//   3. the resulting vector must be finite and non-degenerate.
// The engine's own magnitude is preserved, so the ONLY thing that changes is
// which way the ray points.
void set_shot_dir(bool on);
bool shot_dir();

// BUILD 154: THE VERTICAL. cfg shot_pitch, default ON.
//
// SHOT-2026-09-02 settled why build 137's direction write only ever fixed the
// horizontal: SetShotLine reads [r8] and [r8+4] and NEVER [r8+8]. The engine is
// z-up, so the elevation we wrote was never read at all. It is taken instead
// from the shot direction one step earlier, decomposed to a scalar in degrees,
// and handed to a trajectory generator that builds the ballistic arc BEFORE
// SetShotLine runs; SetShotLine then spins that finished arc through a yaw-only
// frame. Pitch is consumed before our write and yaw after it, which is the
// entire 34.6-deg-yes / 12.8-deg-no asymmetry measured over 30 shots.
//
// The two generators are hooked as OBSERVERS ONLY. At generator time there is
// no shot origin and no weapon pointer, so none of the interlocks that keep AI
// fire untouched can run there, and an ungated write would re-aim every NPC in
// the firefight. Instead the arguments are stashed, and the arc is rebuilt from
// the SetShotLine hook after sd_apply has already ruled the shot ours by the
// same build 137 gate. The rebuild calls the ENGINE'S OWN generator with its
// own saved arguments and only the elevation replaced, so nothing here
// reimplements ballistics.
//
// This moves the round, so the hard baseline governs it: ships with the ray
// probe on, re-proven in the headset, never assumed. 0 restores exact build
// 153 behaviour.
void set_shot_pitch(bool on);

// BUILD 152: THE ORIGIN WRITE. cfg shot_org, default ON.
//
// The 03:05 session on 2026-09-01 (grwxr-25048) proved live what the build-133
// comment predicted: the engine RELOCATES the player's shot origin to the
// camera in ordinary play (hybrid shoot / focus system). Eleven shots WROTE,
// then every shot's origin jumped from the muzzle to 0.31..0.55 m from the
// camera and stayed there, with the barrel as little as 5.6 deg off the gaze.
// The direction write stands down on those by design, so the rounds follow the
// engine's camera-converged aim: the owner's "acts right for the first couple
// of shots but it misses". This build repairs the shot instead of refusing it:
// origin back to the player's own muzzle, direction onto the barrel, both in
// the engine's own buffer at SetShotLine. A shot is repaired ONLY when all of
// these hold, and each test an AI shot could pass is paired with one it
// cannot:
//   1. the origin is NOT at our muzzle but IS within 0.60 m of the camera
//      (measured clusters: relocated 0.31..0.55, muzzle-origin 0.68..1.05);
//   2. the engine's direction is camera-convergent (dot > 0.5 with the view);
//      an AI firing point-blank AT the player points against the view;
//   3. the same barrel-freshness and finiteness interlocks as shot_dir.
// ADS shots become repaired shots too; that is the point. The hard baseline
// says the round must ALWAYS leave the muzzle, and until this build the mod
// only enforced the direction half of that sentence.
void set_shot_org(bool on);
bool shot_org();

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
// BUILD 105: the aiming-point A/B. Installed and drained SEPARATELY from the
// research probes, because those are gated on cfg aim_probes which is 0.
bool install_aimpoint();
// Build 107: End key. Clears the capture table and starts the spoken guide.
void guide_start();
void drain_aimpoint();
// cfg aimpoint_ab (0/1) and aimpoint_ab_m (metres of left/right alternation).
void set_aimpoint_ab(bool on, float shift_m);
bool aimpoint_ab();

void set_firing(bool held);

// Build 84: read it back. This is the merged gamepad's own fire state, so it
// is true for the Touch trigger and for a real pad alike, and it crosses at
// the same threshold the GAME uses rather than at one this mod invented.
bool firing();

// Restores both stubs.
void uninstall();

}  // namespace aimtrace
}  // namespace grwxr
