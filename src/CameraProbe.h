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
// CLAUDE.md rule 8: no logging, allocation, file I/O or locks in the hook. The
// replacement writes into a fixed-size table with atomics and nothing else.
// drain() runs on the init thread and does the printing.

#pragma once

#include <cstdint>   // BUILD 245: player_camera() returns a uint64_t, and Cam2.cpp includes this header first

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
bool base_pitch_geo(float* out);        // BUILD 289 (log only): pitch of the engine camera base, radians, up positive
uint64_t aim_count(int axis);           // BUILD 289 (log only): setter calls seen on this axis
uint64_t aim_lastobj();                 // BUILD 292 (log only): the object SetPitch was last called on
int  aim_arm(int axis, float delta_engine_units);
// 2026-09-01: the engine's last INCOMING setter value (the absolute aim angle
// the engine itself passed in, before any bump), from the asm stub's captured
// bits. For the look-reversal probe line; read-only, any thread.
float aim_lastval(int axis);

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

// BUILD 260: THE VEHICLE INSTRUMENT, log only. One sample of the quantities that could define "in a vehicle" and
// "which way the vehicle faces", read at 1 Hz by the veh: line (VRMirror.cpp). root_ax / head_ax are three world-space
// unit axes (the model X, Y, Z axes rotated into the world) packed row-major, so index a*3+c is world component c of
// axis a. cam_dist is the character-origin-to-engine-camera distance at the last anchor check, taken before the 12 m
// reach test. Nothing here changes any write.
struct VehSample {
    bool  have_obj;
    bool  anchored;
    bool  origin_ok;
    bool  root_ok;
    bool  head_ok;
    bool  veh_on;        // BUILD 261: the vehicle compose is active
    uint32_t veh_edges;  // its transitions
    uint32_t veh_rootfail;  // seated frames the pose root could not be read
    float veh_smooth;    // BUILD 262: the heading tracker's alpha (0 = off)
    uint32_t pre_writes, pre_kept, pre_lost;   // BUILD 263: early writes made, survived to the selector, put back by the engine
    uint32_t vb_used, vb_missed;               // BUILD 264: vehicle frames whose selector took the graph-entry compose / composed itself
    float cam_dist;      // -1 = never read
    float dz;            // head bone height above the character origin, 0 when unread
    float origin[3];     // character origin, world
    float root_ax[9];    // pose root (the owner entity's body orientation)
    float head_ax[9];    // head bone world orientation
};
bool veh_sample(VehSample* s);

// BUILD 261: THE VEHICLE CAMERA. While a vehicle is detected (seated head height, anchor live, the pose root readable,
// the engine camera far from the character) the head is composed onto the body's heading instead of the engine camera,
// so the game's rear-view swing cannot turn the view, and the aim injection stands down (VRMirror.cpp aim_pump).
// cfg veh_camera, default 1; 0 is the rollback and takes effect at once. veh_on() is one relaxed load, any thread.
void set_veh_camera(int on);
// BUILD 262: cfg veh_smooth, the alpha of the alpha-beta tracker on the body heading (0 off, default 0.40, clamped to 0..1).
void set_veh_smooth(float alpha);
float veh_smooth();
// BUILD 262: writes GRWVR\vehyaw-<pid>.csv from the per-frame ring of the vehicle compose. Init thread only.
void veh_dump_csv();
// BUILD 263: THE EARLY WRITE. veh_composed() hands the last composed rotation (3x3 row-major) while the vehicle compose is
// on, so Cam2.cpp can put it into both cameras' rotation rows at the graph-frame builder entry, BEFORE the engine's visibility
// work, which culls for the rotation it finds there. veh_note_prewrite() tells the selector side to check, on its next frame,
// whether the engine left the rows alone (the pre_kept / pre_lost counters in the veh: line).
// BUILD 264: veh_begin_frame() replaces veh_composed(): it composes the frame's vehicle rotation NOW (head, body heading, tracker)
// and returns it for the early write; the frame's selector calls then use exactly that rotation, head and tag orientation.
bool veh_begin_frame(float out[9]);

// BUILD 267: THE COMPOSE INSIDE THE ENGINE'S OWN CAMERA RECOMPUTE (MvpHook.h). While a vehicle is detected and the hook is live the
// first recompute entry of each build composes the frame once and writes both cameras before the engine reads them; the layer
// label is the exact head quaternion that build used. veh_hook_drives() is that state; veh_build_label() is the label lookup.
struct MvpStats {
    bool live;
    uint32_t calls[3];   // recompute entries: camera 0, camera 2, any other camera
    uint32_t builds_one, builds_two, cam2_first, same_tid, diff_tid, rows_eq, rows_ne, label_hit, label_miss;
    uint32_t site_a, site_b, site_other;   // BUILD 269: recompute entries of camera 0 and 2 by engine call site (A pre-update, B culling setup, other)
};
bool veh_hook_drives();
bool veh_build_label(uint64_t frame, float q_out[4]);
// BUILD 270: the recompute hook's engine-frame counter (g_build_seq), for the gtrec rows (GtRec.h). Any thread, log only.
uint32_t build_seq_now();
void set_mvp_observe(int on);
void set_mvp_drive(int on);
void mvp_stats(MvpStats* s);
void veh_note_prewrite(const float out[9]);
bool veh_on();
// head_roll_fix mirrored here: when set, the vehicle compose strips the head's roll so the picture matches the
// roll-free layer pose. Set by VRMirror.cpp set_head_roll_fix.
void set_head_roll_level(int on);

// BUILD 245: the camera pointer the selector / on_calc_mvp path has captured as
// the player's camera, 0 until one was. One relaxed load, any thread. Read by
// the second-camera experiment's gameplay gate (Cam2.cpp); it changes nothing.
uint64_t player_camera();

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

// 2026-08-31: THE HANDS HOLD THE GUN (cfg wgun_hands, default 1). Every call
// that moves the gun-root record measures the rigid change it made and applies
// that same change to the character's hand and finger records (21 bones a
// side, resolved by verified CRC32 name from the live rig each census tick).
// The animated grip is preserved exactly under the move: no IK, no placement
// constants, and switching it off simply stops the writes because the engine
// re-solves every record from animation each frame. Touches no shot code.
void set_wgun_hands(int on);

// BUILD 140: THE GRAB (cfg wgun_hands_grab, default 1). Headset-confirmed
// hands-on-gun, owner's correction: the idle animation must not FORCE both
// hands onto the weapon. The character's LEFT (foregrip) hand chain now rides
// the gun scaled by the two-hand authority weight: off hand down = the
// animation keeps it, off hand gripping = full ride, in between = a smooth
// magnetic grab that releases on the attach band's own edge. The right chain
// always rides. The animation itself is never touched; it is the grip source.
void set_wgun_hands_grab(int on);

// BUILD 141: HAND PRESENCE (cfg wgun_hand_free, default 1). Off the weapon,
// the character's left hand follows the LEFT CONTROLLER instead of the
// animation: position only (aimtrace publishes no left orientation yet), the
// wrist and fingers keep the animated pose, the reach hard-clamped to 1.20 m
// per call. The grab then blends between your real hand and the foregrip.
// This is the foundation for reach interactions (over the shoulder for the
// rifle, to the thigh for the sidearm); the grab-to-draw mechanic itself is
// a separate, later system. free= counts tracked calls, hfar= clamp bites.
void set_wgun_hand_free(int on);

// BUILD 158: THE WRIST (cfg wgun_wrist, default 1). CX-017, the brace-frozen
// hand. Supplies what 141 left out: off the weapon, the free hand's whole
// chain rotates about the hand bone by the delta between the left
// controller's frame now and its frame when the free path was entered. A
// delta, never an absolute mapping: the bone's own axis convention is
// unmeasured and a delta needs none. Latch drops on ride, dropout or wrist
// off, so every engagement starts at identity and cannot snap.
void set_wgun_wrist(int on);

// BUILD 162: THE ABSOLUTE WRIST (cfg wgun_wrist_abs, default 1). Off the
// weapon, the free hand's palm is laid onto the controller's OpenXR grip
// pose, whose axes are anatomical by specification, using the bone's own
// knuckle geometry as the matching anatomy. Stateless. 0 restores the 161
// delta route exactly.
void set_wgun_wrist_abs(int on);

// BUILD 163: THE FOREARM TAKES THE ROLL (cfg wgun_twist, default 1). The
// twist component of the hand's rotation about the forearm axis is spread
// along the forearm's twist helpers by their measured fraction from elbow
// to wrist, the way a real forearm carries roll. 0 restores 162 exactly.
void set_wgun_twist(int on);

// BUILD 167: the ForeArm bone's share of the hand's roll (cfg
// wgun_twist_share). BUILD 171: default 0.00. The four helpers ramp from
// this share at the elbow to the hand's whole roll at the wrist, and the
// second call on a buffer re-asserts what the first wrote over the engine's
// repaint, so the ForeArm no longer needs to carry any of it. Clamped 0 to 1.
void set_wgun_twist_share(float v);

// BUILD 169: the upper arm's share of the hand's roll (cfg wgun_twist_arm,
// default 0.33). The Arm bone rolls this fraction about its own axis, so a
// big pronation is split shoulder, elbow, wrist instead of landing on one
// joint. The elbow crease is (wgun_twist_share - wgun_twist_arm) x roll,
// the wrist crease (1 - wgun_twist_share) x roll. Clamped 0 to 1.
void set_wgun_twist_arm(float v);

// BUILD 176: the wrist-first arm solve (cfg wgun_wristfirst, default 1).
// The left forearm runs from the wrist up the hand's own long axis, a
// straight wrist by construction; the shoulder follows the elbow when the
// arm is short. 0 restores the 162 two-bone solve.
void set_wgun_wristfirst(int on);

// BUILD 178: the wrist limit in degrees (cfg wgun_wrist_limit, default 30,
// clamped 0..180). The two-bone elbow stands while the bend it implies at
// the wrist is within the limit; beyond it the forearm is rotated toward
// the hand's long axis by the excess. 0 = 176, 180 = 162.
void set_wgun_wrist_limit(float deg);

// BUILD 179: the shoulder cap in mm (cfg wgun_shoulder_max, default 80).
// The shoulder record travels at most this far toward the elbow; when the
// wrist limit would need more, the wrist bends past its limit instead.
void set_wgun_shoulder_max(float mm);
void set_arm_anchor(float on, float maxm);   // BUILD 213: the shoulder rides the view offset
void set_arm_pole(float on, float minm);     // BUILD 214: the elbow's pole falls back to anatomy
void set_pole_down(float left, float right); // BUILD 293: cfg arm_pole_down_l / arm_pole_down_r, the elbow leans toward down (two-handed)
void set_torso(float on, float dead_deg, float rate_dps, float hand_w, float max_deg);   // BUILD 294: cfg torso_turn, torso_dead, torso_rate, torso_hand_w, torso_max
void set_lean(int hinge, float theta_deg, float slide_m);   // BUILD 300: cfg lean_hinge, lean_theta, lean_slide
void set_pole_smooth(float on, float tau_ms, float rate_dps);   // BUILD 285: cfg arm_pole_smooth, arm_pole_tau_ms, arm_pole_rate

// BUILD 180: the collarbone turns about its root so its tip meets the
// moved shoulder joint (cfg wgun_clav_max, degrees, default 25; 0 off).
void set_wgun_clav_max(float deg);

// BUILD 183: the mount mirror (cfg wgun_mirror). The gun-root record is
// copied onto Fake_gunroot (1) and also onto Prop_RightHand and
// Prop_LeftHand (2) on every write, so whichever bone the engine attaches
// the held weapon to during a roll or a climb carries ours. 0 = build 182.
void set_wgun_mirror(int mode);

// BUILD 184: the two roll probes, log only (cfg wroll_probe, default 1):
// gsurvive (the gun-root records at Present) and wroll (the placed weapon
// part's motion against our written rotation).
void set_wroll_probe(int on);
void set_wgun_roll_unflip(int on);   // BUILD 189: cancel the gun-up half turn
void set_wgun_own(int on);           // BUILD 190: own the bone
void set_fov_root(int on);           // BUILD 191: the fov root (the clouds)
void fovroot_log();                  // BUILD 191: its 1 Hz line
void set_fov_root_mode(int mode);    // BUILD 194: 0 hold all frame, 1 release at the consuming projection call
void set_fov_root_aimk(float k);     // BUILD 194: gain on injected head-aim deltas while armed
float fovroot_aim_gain();            // BUILD 194: that gain; RETIRED in 197, always 1.0
void set_fov_src(int on);            // BUILD 197: the source fov (the clouds): the engine authors the wide field itself
void fovsrc_log();                   // BUILD 197: its 1 Hz line
void set_fov_scene(int on);          // BUILD 198: the scene fov override (the clouds): every engine writer of the field stores fs_fov
void set_fov_scene_ads(int on);      // BUILD 198: release the override while the aim trigger is held
void set_ads_input(bool held);       // BUILD 198: the merged left trigger past XInput's threshold (XInputMerge)
void fovscene_log();                 // BUILD 198: its 1 Hz line
void sprint_sample();                // BUILD 200: the sprint probe, present thread, 10 Hz, reads only
void sprint_log();                   // BUILD 200: its drain, heartbeat thread, one line per sample
void set_sprint_probe(int on);       // BUILD 200: cfg sprint_probe
void set_move_input(float lx, float ly, unsigned short buttons);   // BUILD 200: the merged left stick and buttons (XInputMerge)
bool move_input(float* lx, float* ly, unsigned int* buttons);      // BUILD 209: read them back (the user's own stick, before the follow's is added)
bool body_head_world(float out[3]);                                // BUILD 209: the character's Head bone in world, from the armed skeleton
void apply_ctrl_lead(float pos[3]);  // BUILD 201: lead the camera position the controllers are placed from by wgun_lead frames
void set_wgun_lead(float k);         // BUILD 201: cfg wgun_lead, frames
void lead_log();                     // BUILD 201: its 1 Hz line
void set_gpair(int on);               // BUILD 277: cfg gpair, the gun pairing recorder (log only)
void gpair_note(int kind, const float gun[3], const float aux[3], const float cam[3], uint64_t id0 = 0, uint64_t id1 = 0);   // BUILD 277: kind 1 Present, 2 gun write; cam may be null (the latch). BUILD 279: 3 camera compose, 4 gun write head bone
void gpair_log();                     // BUILD 277: its drain, heartbeat thread
void set_wgun_pull(int on, float k, float max_mm);   // BUILD 283: cfg wgun_pull, wgun_pull_k, wgun_pull_max, the shoulder pull
void set_wgun_drawcam(int on);         // BUILD 282: cfg wgun_drawcam, the hands placed from the camera that draws them
void note_pub_cam(int hand, const float cam[3], const float pw[3]);   // BUILD 278: Present thread, the camera this hand was placed from (0 right, 1 left)
void late_log();                     // BUILD 278: its 1 Hz line

// BUILD 142: THE ARMS FOLLOW (cfg wgun_arms, default 1). Two-bone analytic
// elbow per side: the shoulder stays where the animation put it, the elbow
// lands on the reach circle on the side the animation's own elbow points
// (measured pole, no tuning constant), each segment carried by the shortest
// arc so twist and skinning survive. Segment lengths are running minimums
// from the live pose. Replaces stretching with a bent arm; the beyond-reach
// case goes straight-arm and counts on ikcl=, which is the number that says
// whether the wingspan scale is worth building. wgun_hide_arms=1 overrides
// this entirely.
void set_wgun_arms(int on);

// BUILD 143: THE VISIBLE-BARREL PITCH TRIM (cfg wgun_pitch_deg, default 0,
// +-45). The weapon mesh's barrel sits a constant pitch below the bone axis
// the swing aligns, so rounds print HIGH of the barrel the player sees (the
// long-standing no-trim offset, finally visible now that the rifle is
// shouldered). The trim rotates only the swing target: the shot line is
// untouched, positive degrees raise the visible barrel onto it. Converge at
// a wall: fire, raise the trim until the holes sit on the barrel.
void set_wgun_pitch(float deg);

// BUILD 144: THE RAY PROBE (cfg ray_probe, default 1, log only). One line a
// second: the controller ray before the two-hand blend, the published barrel
// direction after it, the camera forward, and the blend authority. Exists to
// separate three suspects for aim moving under a still hand: the head leaking
// through the controller-to-world mapping, the two-hand blend bending the
// ray toward the off hand, and plain arm droop. Protocol: brace the hand,
// sweep the head, read which number moves.
void set_ray_probe(int on);

// BUILD 145: THE GRIP-GATED HOLD (cfg wgun_two_grip, default 0 = geometry
// alone, exactly as before). At 1 the two-handed direction has authority
// only while the OFF hand physically squeezes its grip button (0.35..0.65
// smoothstep), like native VR titles. Multiplies into two_w after every
// existing band, so it can only remove authority; releasing the grip returns
// the confirmed one-handed aim on the same curve everything else uses. The
// VR layer publishes the off-hand squeeze per frame via set_grip_off and
// stops mapping that squeeze to a gamepad bumper while the gate owns it.
void set_wgun_two_grip(int on);
void set_grip_off(float v);

// BUILD 142: THE CALIBRATION CAPTURE (cfg hand_calib, momentary), log only.
// Hold the controllers straight out at shoulder height; the next 1 Hz tick
// logs controller span vs avatar span (shoulder width plus measured arm
// lengths) and the ratio. Applying a scale is a later build's decision,
// made from this number.
void set_hand_calib(int on);

// BUILD 140: BONE-LEVEL ARM HIDE (cfg wgun_hide_arms, default 0). The cure
// for the confirmed mesh stretching if hide_child pieces cannot separate the
// hand from the arm: both arms' ten bone records a side collapse onto the
// shoulder each write call, quaternions shrunk to 2 percent. Runtime only;
// off restores stock on the next animation solve. arms= counts engagement.
void set_wgun_hide_arms(int on);

// 2026-08-31: build 123's standing single change (cfg wgun_two_anchor,
// default 1). When the front hand measures as the RIGHT one, the position
// anchor crossfades to the rear (left) controller on the direction's own
// authority curve, and the handguard slide runs instead of standing down.
// 0 is byte-for-byte the build-123 behaviour.
void set_wgun_two_anchor(int on);

// 2026-08-31: hide ONE render piece of the character, the FP tree's helmet
// mechanism ported for the arms (cfg hide_child, hide_child2; -1 = off).
// Sets bit 0x0004 on one array-2 child of the latched head-visibility object,
// re-asserted at 1 Hz, with array 1 read first as a positive control. This is
// also the measurement for the question that decides the hands approach:
// step until an arm vanishes, then look at whether the hand went with it.
void set_hide_child(int slot, int idx);

// BUILD 120: the two-handed AGREEMENT GATE, as a live control.
// It refuses a two-handed frame when the line between the hands disagrees
// with the rear controller's own forward by more than acos(v). The
// 2026-08-20 headset run refused 70% of two-handed frames at the old
// hard-coded 0.35 (about 69 degrees), so this is the FIRST thing to move when
// two-handed will not hold. Band -1.00 (accept everything, gate off) to 1.00
// (perfect alignment only), which are the true limits of a cosine, not a
// guess about what a tester could need.
//
// BUILD 123 SUPERSEDES THAT ADVICE. The gate was not tight, it was reading the
// barrel backwards, and moving this was the wrong fix. Leave it at 0.35.
void  set_wgun_two_agree(float v);
float get_wgun_two_agree();

// 2026-08-31b: THE ATTACH BAND (cfg wgun_two_attach, metres, default 0.45).
// The upper release edge the authority band never had: beyond this hand
// separation the front hand fades out of the direction, the anchor and the
// slide together (release completes 0.15 m later, smoothstepped). Grounded in
// the record: real holds 0.105 to 0.409 m, the aim clips 0.240 to 0.283 m,
// the documented split at 0.507 m from "a hand holding nothing". 3.00
// effectively disables the release for the A/B.
void  set_wgun_two_attach(float m);
float get_wgun_two_attach();

// BUILD 123: which hand is the FRONT hand is MEASURED per frame instead of
// assumed to be the left one.
//
// `[VERIFIED, log grwxr-29396, 128 samples]` the tester's front hand is his
// RIGHT. Of the 11 samples with the hands more than 0.30 m apart, the median
// angle between the hand-to-hand line and the right controller's own aim was
// 140 degrees; flipped, the five widest read 39, 43, 32, 34 and 33 degrees.
// That inverted line is why the agreement gate refused every frame it ever saw
// (mean 90 deg, best 81 deg, threshold 70 deg, zero accepted).
//
// 1 = measure (default), 0 = the pre-123 fixed assumption. Handedness
// agnostic: a correct hold never trips the flip, so this cannot make one worse.
void  set_wgun_two_auto(int on);
int   get_wgun_two_auto();

// BUILD 124: restart the weapon direction and roll filters from the live value.
// Anything that can REVERSE the target direction must call this, or the one-pole
// slews the weapon through 180 degrees at the rate cap instead of snapping.
void  reseed_wgun();

// BUILD 125: THE EYE-SIGN A/B, +1 (every build since 10m) or -1.
//
// Build 93 established that positive ipd_scale displaces the LEFT eye to the
// RIGHT, "geometrically the swapped direction", and that the stored sign is
// [UNKNOWN] rather than wrong because build 10m could only measure the
// COMPOSITE of sign and frame-pairing parity. **No log line can settle it.**
//
// It matters because reversed stereo fuses at tiny disparity and hurts at
// large, which is exactly the reported shape: ipd_scale has only ever been
// persisted at 0.50 and 0.90, 1.00 has never been run, and the working value
// of 0.05 is a saturated hotkey reached in 4.1 seconds. If the sign is
// inverted, 0.05 is just the largest reversed disparity the eyes tolerate.
//
// Live, because fusion cannot be A/B'd across a relaunch: the eyes adapt over
// seconds and the comparison has to be back to back on the same view.
// BUILD 126: bypass the static grip offset so the gun parks ON the tracked
// hand position, for the "does the gun sit on the blob" test. The tuned grip
// values are untouched, so turning this off restores them exactly.
void  set_wgun_grip_bypass(int on);
int   get_wgun_grip_bypass();

void  set_ipd_sign(int s);
int   get_ipd_sign();

// 2026-09-01: THE HORIZON LOCK (cfg cam_level_horizon, default 1). Removes the
// ENGINE's camera roll from the base before the head is composed onto it, so
// the horizon stays level and your own head roll, which rides in H and never
// touches the base, becomes the only source of roll. Inert while
// cam_pose_rot = 0, since the mod then writes position only.
void  set_cam_level(int on);
int   get_cam_level();

// 2026-09-01: THE ARM SCALE (cfg hand_scale, default 1.00 = identity), also
// set live by a successful wingspan capture. Avatar reach over player reach,
// multiplying the FREE-HAND IK target about the shoulder so a full real reach
// maps onto a full avatar reach instead of being clipped by the anatomical
// bound. Refuses anything outside 0.5 to 2.0.
//
// It does NOT touch the two-hand aim, which reads the raw controller positions
// through aimtrace::ctrl_pos_l. Re-check that separation before widening this.
void  set_hand_scale(float s);
float get_hand_scale();

// True once after a successful capture. VRMirror owns the first-person path
// (and the recenter folded into it), so the capture requests rather than calls.
bool  take_fp_request();

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
// BUILD 219: the grip bone. 1 reads wb-HandRight from the picked weapon's own
// rig at 1 Hz and moves the gun root by minus that offset so the grip lands
// on the controller (the wgun_grip_* trims add on top). 0 is build 218.
void set_wgun_grip_bone(int on);
int  get_wgun_grip_bone();
// BUILD 221: the PiP scope. pip_lens gives the eyepiece (wb-AimingPoint, or
// the cfg fallback) in engine world as the anchor last placed it; the present
// thread draws the magnified disc there. pip_block_ads withholds the left
// trigger from the game so it never enters its own ADS.
void set_pip(int on);
void set_pip_fallback(float lat, float fwd, float up);
void set_pip_block_ads(int on);
int  pip_block_ads();
bool pip_lens(float out[3]);
// BUILD 237: the scope is a LATCH on the left thumbstick click, not a held
// trigger. While the scope is DOWN the left trigger goes back to the game as
// its own ADS, which is the call path that hides the head: the engine only
// calls the visibility setter from the proximity test that runs in the ADS
// camera family, so withholding the trigger meant the head never hid.
// pip_toggle = 0 restores the pre-237 hold-the-trigger behaviour, L3 included.
void pip_toggle_press();
int  pip_user_on();
void set_pip_toggle(int on);
int  pip_toggle_enabled();
// BUILD 238: NATIVE VR ADS. The magnified sight picture is the GAME'S OWN, drawn
// full screen by 11d's flat-scope path, and the rifle aims it instead of the head.
// scope_hold_frames is the stability gate that stops an iron-sight ADS transition
// tripping that path on its way past mono_scope_fov: measured on this machine a
// settled magnified optic sits at 0.2549 rad and a transition sweeps through
// ~0.248, so an instantaneous test cannot tell them apart (see g_scope_native).
void set_scope_native(int on);
void set_scope_hold(int frames);
int  scope_native_aim();   // 1 = the mode is on AND the flat-scope path is engaged
int  scope_engaged();      // the latch alone, for the log line
// BUILD 222: the engine's per-eye camera shift along its right axis, metres,
// the same formula the camera write uses; eye 0 is the left frame.
float eye_shift(int eye);

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

// BUILD 224: the head hides at spawn without ADS. head_prelatch (default 1)
// promotes the pre-latch's candidate when its entity handle is the pinned
// player; head_reassert (default 1) holds the hidden bits against the
// engine's show-all path. head_hide_frame() is the per-frame re-assert,
// called right after set_head_hide() from the poll; no logging, no engine
// call, SEH-guarded reads and at most five u16 writes every 6th frame.
void set_head_prelatch(int on);
void set_head_reassert(int on);
void head_hide_frame();

// BUILD 225: the head hidden at all times, no F2, no trigger. head_always
// forces the hide regardless of the first-person flag; fp_auto turns first
// person on by itself once the pin is valid and near; head_pulse is the
// frames per phase of the automatic ADS pulse (0 = off), consumed by
// ads_pulse_value on the input thread after the pip_block_ads zeroing.
void set_head_always(int on);
void set_head_populate(int on);   // BUILD 226: call the engine's own fill routine on the held object
void set_fp_auto(int on);
void set_fp_user_off(int off);   // the F2 off edge; cleared when the pin changes
void set_head_pulse(int frames);
bool ads_pulse_value(unsigned char* p);

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

// BUILD 172: the arm survival probe, log only. Called from on_present on the
// render thread; rate-limited to 1 Hz inside; reads only.
void survive_sample();

// BUILD 184: the gun root and its mirrors at Present, same rules, log only
// (cfg wroll_probe).
void gun_survive_sample();

}  // namespace camera
}  // namespace grwxr
