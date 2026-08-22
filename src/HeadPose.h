// HeadPose.h - the channel between the VR side and the camera side.
//
// BUILD 8, forward direction: VRMirror computes the head rotation relative to
// its yaw-only reference, already converted to the game's basis (x right,
// y forward, z up, row-vector rows, docs/RE-notes.md "COORDINATE
// CONVENTIONS"), and publishes it here from the Present hook. The camera hook
// reads it at on_calc_mvp entry on an engine thread. Neither side may lock or
// allocate (project rule 8), so the slot is a seqlock: an atomic sequence
// counter around nine relaxed-atomic floats. A reader that catches the writer
// mid-update retries, and on the (rare) torn read simply skips that call,
// which costs at most one engine camera pass.
//
// BUILD 9, reverse direction: the proj[2] hook publishes the vertical field
// of view the engine is rendering with, so VRMirror can stamp the projection
// layer with the fov the submitted image actually has. One writer (engine
// thread), one reader (render thread), a single atomic float is enough: a
// one-frame-stale fovy costs one frame of reprojection error, which the
// compositor absorbs (docs/HANDOFF.md "BUILD 9").

#pragma once

#include <cstdint>   // build 22: the pad snapshot types below

namespace grwxr {
namespace headpose {

// Render thread, once per presented frame. R is a 3x3 rotation, row-major,
// game basis, row-vector convention. q_xr is the SAME head orientation as an
// absolute XR-space quaternion (x,y,z,w in the layer space), carried along so
// the camera hook can echo back exactly which orientation each built frame
// was composed with (BUILD 13a, render-pose submit).
void publish(const float R[9], const float q_xr[4]);

// Stop the camera write (session STOPPING/EXITING, shutdown). read() returns
// false until the next publish, so the game's own camera takes back over.
void disable();

// Engine thread. Copies the latest published rotation into R (and the
// matching XR-space quaternion into q_xr) and returns true. Returns false if
// nothing has been published, the channel is disabled, or the read tore; the
// caller must then leave the camera untouched.
bool read(float R[9], float q_xr[4]);

// Engine thread, from the proj[2] hook: the vertical field of view the frame
// is being rendered with, in radians.
void publish_fov(float fovy_radians);

// Render thread. The latest published fovy in radians, or `fallback` if
// nothing has been published yet this run.
float read_fov(float fallback);

// BUILD 118. Engine thread, same proj[2] hook: the live near and far clip
// planes in metres, read straight out of the perspective builder's arguments.
// Needed by XR_KHR_composition_layer_depth, whose nearZ/farZ must match the
// projection the content was rendered with.
//
// near_m > far_m MEANS THE ENGINE IS IN REVERSED-Z for this matrix: projbuild
// (lastrites 0x0D7C0610) implements reversed-Z by swapping these two
// arguments, so near then maps to NDC 1 and far to NDC 0. Callers must branch
// on the comparison rather than assume a convention.
void publish_clip(float near_m, float far_m);

// Render thread. False until the first publish, in which case the outputs are
// untouched. Engine defaults, for reference only, are near 0.1 and far 1200.0
// from Camera::Camera; the live values override them per frame.
bool read_clip(float* near_out, float* far_out);

// BUILD 10b. Render thread: the interpupillary distance in metres, measured
// each frame from the two located eye positions. Engine thread reads it to
// size the per-eye camera offset (1 world unit = 1 m, docs/RE-notes.md).
void publish_ipd(float ipd_meters);
float read_ipd(float fallback);

// BUILD 10c: user-adjustable multiplier on that offset, for tuning perceived
// stereo separation against the game's true world scale. 1.0 means the
// headset's measured IPD verbatim; 0.0 collapses to mono (a useful A/B).
// Written from the render thread (grwxr.cfg at init, Numpad hotkeys live),
// read by the camera hook on the engine thread.
void set_ipd_scale(float s);
float ipd_scale();

// BUILD 11b: fov-gated mono scope. When the published rendered fov drops
// below this threshold (radians), the camera hook collapses the eye offset
// to zero for those frames: magnified optics fuse and the baked-in reticle
// stops carrying eye parallax. Measured (session 11): 3x magnifier renders
// at 0.172, plain ADS at 0.49+, menus at 0.33+, so 0.30 separates magnified
// optics from everything else. Written at init (grwxr.cfg mono_scope_fov),
// read by the camera hook on the engine thread.
void set_mono_scope_fov(float radians);
float mono_scope_fov();

// BUILD 11c: first-person DEMO. When enabled, the camera hook pushes the
// camera position forward along the GAME camera's own forward axis (the
// base rotation, not the head-composed one, so the offset lands at the
// character regardless of where the head looks) by fp_forward meters,
// placing the viewpoint roughly inside the character's head. Known demo
// jank, documented and accepted: edge culling artifacts (visibility is
// decided upstream of our write, build 7), hair/eyelash leakage (the head
// is only invisible from inside via backface culling; the engine has no
// proximity hide, verified in photo mode session 11), and the usual aim
// decoupling. Toggled from the render thread (Numpad 8), read by the
// camera hook on the engine thread.
void set_fp_enabled(bool on);
bool fp_enabled();
void set_fp_forward(float meters);
float fp_forward();

// BUILD 11f: the third-person camera hangs off the RIGHT shoulder, so a
// pure forward push lands right of the head (user report). Side (positive =
// right, so the default is negative) and up offsets complete the placement,
// all in the base camera's own axes.
void set_fp_side(float meters);
float fp_side();
void set_fp_up(float meters);
float fp_up();

// Build 15e: ANCHORED FIRST PERSON. The 11c demo pushed the camera forward
// from the third-person camera, which drifts as that camera pitches and
// orbits ("gets out of whack when you control the character"). Instead the
// viewpoint is placed at the CHARACTER's own world origin (skeleton +0x120,
// [VERIFIED] 2026-08-01) plus fp_eye meters of world up, so it stays put
// while the player moves. fp_side/fp_up remain as fine trim in camera axes.
void set_fp_eye(float meters);
float fp_eye();

// Build 15e.3: anchored lateral centering, meters along the base camera's
// right axis (positive = right). Cfg key fp_anchor_side, Numpad 6/5 live
// while a pin exists.
void set_fp_anchor_side(float meters);
float fp_anchor_side();

// Build 16a: THE HEAD BONE. The character origin was always a placeholder
// anchor: it sits at the feet/body centre, ignores idle animation, and is at
// the wrong height in every non-standing stance (all three user-reported).
// The animated Head bone is the real viewpoint. Read path, [VERIFIED] by
// offline disassembly of the engine's own accessors (docs/RE-notes.md
// 2026-08-02 "THE POSE OBJECT"): [skeleton+0x238] is the per-character final
// Pose, [pose+0x178] its bone-transform buffer, and node i occupies 0x20
// bytes there: float4 translation at +0x00, float4 quaternion at +0x10. The
// node index comes from the rig's sorted name map, keyed on CRC32("Head").
//
// fp_head_anchor selects it over the origin anchor (cfg key, default on).
// fp_head_eye is the trim from the head JOINT to the eyes, in meters of world
// up; the head joint sits at the base of the skull, so a small positive value
// is expected rather than fp_eye's 0.85. It shares the Numpad 7/4 keys with
// fp_eye: whichever anchor is live is the one those keys tune, so no new key
// is added (hazard 24).
void set_fp_head_anchor(bool on);
bool fp_head_anchor();
void set_fp_head_eye(float meters);
float fp_head_eye();

// Build 89: route the camera pose write through the `selector` camera target
// when `on_calc_mvp` is not derived for this binary (cfg key
// cam_selector_pose, default OFF).
//
// write_pose_head is what composes the headset rotation onto the camera, what
// applies the per-eye IPD offset, and what captures the base transform that
// first person anchors against. It has exactly one caller, the on_calc_mvp
// recorder, so on a binary missing that row the headset is mono and the FP
// toggle does nothing. The selector receives the same camera object and is
// hooked; whether it runs early enough in the frame for the write to survive
// is [UNKNOWN] and is what the flag exists to test.
void set_cam_selector_pose(bool on);
bool cam_selector_pose();

// Build 90: does the camera write compose the headset rotation onto the
// camera basis, or does it only move the viewpoint (cfg key cam_pose_rot,
// default on, so a build that derives on_calc_mvp keeps its designed
// behaviour without a cfg change).
//
// Off is the answer to "the camera fights". Head look already reaches the
// view through the SetYaw/SetPitch absorb-and-inject path, so composing here
// is a second authority on one channel; the flat first-person mod writes the
// pose's fourth row only, for the same reason. With this off the write does
// nothing but place the viewpoint at the head bone and offset it per eye.
void set_cam_pose_rot(bool on);
bool cam_pose_rot();

// Build 91: THE VIEWPOINT PLACEMENT TRIO, ported from this author's own flat
// first-person mod for the same game, where each has a measured reason behind
// it. The VR camera write had none of them and took the animated
// head bone raw, three times a frame.
//
// fp_fwd    metres the eyes sit FORWARD of the head joint, which is at the base
//           of the skull. HORIZONTALIZED, so this frame's pitch never moves
//           this frame's position, and when the view looks near straight up or
//           down (where the horizontal forward collapses) the last good forward
//           is reused rather than dropping the offset and leaving the viewpoint
//           on the bare joint, inside the head mesh.
// fp_clamp  metres the viewpoint may stray horizontally from the character
//           origin, so an animation that whips the head (vault, melee) cannot
//           whip the view. 0.20 was measured TOO TIGHT: the sprint lean held
//           the head outside it continuously, and a clamped sustained lean
//           parks the camera inside the torso.
// fp_smooth / fp_smooth_z  EMA time constants in ms, horizontal and vertical.
//           Locomotion bobs the head bone hard. The filter runs in
//           ORIGIN-RELATIVE space, not world space: a world-space EMA lags a
//           moving target by tau times velocity, about 0.3 m at sprint, which
//           puts the camera back inside the torso. The origin carries the whole
//           locomotion velocity and is already smooth, so filtering only the
//           bone's deviation from it removes the bob with zero velocity lag,
//           and teleports pass through instantly because they move the origin
//           rather than the deviation.
void set_fp_fwd(float meters);
float fp_fwd();
void set_fp_clamp(float meters);
float fp_clamp();
void set_fp_smooth(float ms);
float fp_smooth();
void set_fp_smooth_z(float ms);
float fp_smooth_z();

// The player's Head node index inside the rig's pose buffer, resolved on the
// drain thread (1 Hz binary search over the rig name map) and consumed by the
// camera write on the engine thread. 0xFFFF means "not resolved": the camera
// write then falls back to the origin anchor.
void set_head_node(unsigned int idx);
unsigned int head_node();

// The pinned player character object. Published by the read-only probe's
// 1 Hz pin (which picks the character the chase camera is LOOKING AT, the
// discriminator that survives teammates standing close), consumed by the
// camera write, which re-reads the origin every frame. 0 = no pin.
void set_player_obj(unsigned long long p);
unsigned long long player_obj();

// BUILD 12a: FULLSCREEN. The proj[2] hook overrides the engine's rendered
// vertical fov with fs_fov (radians) whenever the incoming fov is in the
// world band (>= 0.60: on-foot 0.78..0.87, sprint/vehicle ~1.22, menus
// 1.57; plain ADS 0.49..0.52 and magnified optics stay untouched, so aim
// zoom and the flat-scope path survive). The existing live-fov blit then
// spreads the wider image across the eye canvases with no placement
// changes: 1.92 = 2*atan(1.428) covers the canvas's full downward tangent,
// filling the view. fs_enabled toggles the override (Numpad 1); fs_fov is
// grwxr.cfg fullscreen_fov, stepped live (Numpad 2 / Numpad +). Written on
// the render thread, read by the recorder on the engine thread.
void set_fs_enabled(bool on);
bool fs_enabled();
void set_fs_fov(float radians);
float fs_fov();

// BUILD 103: the band's LOWER edge, and why it is now a setting.
//
// `[VERIFIED, grwxr-29328.log and grwxr-29260.log]` plain ADS on this binary
// publishes 0.3300 rad, 18.9 degrees. That number falls into a gap between
// three of our own thresholds and receives no treatment at all:
//
//   below mono_scope_fov 0.30   the flat-scope path: camera write skipped,
//                               content redrawn at scope_display_fov. ADS is
//                               ABOVE this, so it does not apply.
//   0.60 .. 1.35                the fullscreen override. ADS is BELOW this,
//                               so it does not apply either.
//   result                      an 18.9 degree image drawn at 18.9 degrees of
//                               a roughly 100 degree display: a small picture
//                               in the middle of black, head-inert because
//                               aim injection also stops below 0.65.
//
// That is what the tester described as "even basic red dots go into the
// cinematic flat". Three of our numbers, not an engine behaviour.
//
// Lowering this to just above mono_scope_fov routes plain ADS through the
// fullscreen override, so aiming keeps the world's field of view and behaves
// like ordinary VR aiming, while magnified optics stay below mono_scope_fov
// and keep the flat-scope path build 11d verified. The default stays 0.60,
// exactly the old behaviour, because the value that is RIGHT has to come out
// of a headset rather than out of this comment.
void set_fs_fov_lo(float radians);
float fs_fov_lo();

// BUILD 10b.1: frame-to-eye identity FIFO. The engine pipelines frame builds
// ahead of their Presents by an unknown (and possibly varying) depth, so the
// present side must NOT derive the eye of the backbuffer image from frame
// parity. Instead the camera hook pushes one tag per built frame (the eye it
// offset the camera toward), and the present side pops exactly one tag per
// present. Frames present in build order, so the oldest tag always names the
// image being presented, whatever the pipeline depth. Single producer
// (engine thread), single consumer (render thread), lock-free ring.
// BUILD 13a: each tag also carries the absolute XR-space head orientation the
// frame's content was composed with (the q_xr the camera hook consumed), so
// the present side can submit the TRUE render pose instead of a freshly
// located one. Submitting a present-time pose the content was not rendered
// with makes the compositor's timewarp land each submission at a slightly
// different angle; with the build-to-present pipeline depth flapping 0..1
// (10b logs) the error oscillates per refresh: monocular 36 Hz stutter during
// head rotation, invisible when still (session 13 headset report, the exact
// discriminator pattern). Prior art: cyberpunk-vr-port "render-pose submit".
// BUILD 118-I, fault C instrument: the tag also carries the composed camera
// base for the frame (CameraProbe's g_base_pos, WITHOUT the per-eye offset)
// and that frame's guard state, so the present side can measure per-eye base
// disagreement directly. base may be null; flags bit0 = capture-guard rescue
// fired, bit2 = first person active.
void push_eye_tag(int eye, const float q_xr[4], const float base[3],
                  unsigned char flags);           // engine thread, once per
                                                  // built frame
int  pop_eye_tag(float q_xr[4], bool* q_ok,       // render thread, once per
                 float base_out[3] = nullptr,     // present; -1 if none (no
                 unsigned char* flags_out = nullptr);
                              // camera write happened for this frame: menus,
                              // loading; treat the image as mono). q_ok false
                              // on -1, and base_out/flags_out are zeroed.
int  eye_tag_depth();         // diagnostic: current ring occupancy

// BUILD 10b.2 diagnostics: how many pops found a tag vs came up empty (mono).
// A large mono count during play means the tag stream is starving and the
// stereo routing cannot be trusted.
unsigned long long pops_tagged();
unsigned long long pops_mono();

// Build 93: pushes dropped for a full ring. Any nonzero value means the eye
// parity flipped and stayed flipped, because the producer toggles the eye
// BEFORE it pushes. Never nonzero in any run measured so far.
unsigned long long tag_drops();

// Build 93: resynchronise the ring at session re-begin, so a headset doff does
// not leave one stale tag in flight and swap the eyes for the rest of the run.
void reset_eye_tags();

// Build 19: VR AIM INJECTION accounting. The render thread injects head
// yaw/pitch deltas into the engine's absolute aim pair (CameraProbe) and
// tracks here, in GEOMETRIC radians, how much the engine has absorbed so
// far. The camera hook subtracts these from the engine camera's own
// yaw/pitch before composing the full head rotation, so the view never
// double-applies what the engine already turned. Written on the render
// thread when a consumed injection is observed; read by the camera hook
// every call. A torn yaw/pitch pair costs one frame of sub-degree error.
// aim_cum returns false while both are zero (no injection ever absorbed),
// which is the camera hook's signal to use the raw base untouched.
void set_aim_cum(float yaw_geo, float pitch_geo);
bool aim_cum(float* yaw_geo, float* pitch_geo);

// Build 22: TOUCH-AS-GAMEPAD snapshot. The render thread publishes the Touch
// controllers' button and axis state once per sync; the XInputGetState detour
// (XInputMerge) reads it on the game's input thread and merges it into the
// pad state the game polls. Plain relaxed atomics: fields tear independently
// and one frame of skew is harmless for input.
//
// The button mask reuses the XINPUT wButtons bit layout verbatim (stable ABI
// constants) so the detour merges with a single OR:
enum : uint32_t {
    PAD_DPAD_UP    = 0x0001, PAD_DPAD_DOWN  = 0x0002,
    PAD_DPAD_LEFT  = 0x0004, PAD_DPAD_RIGHT = 0x0008,
    PAD_START      = 0x0010, PAD_BACK       = 0x0020,
    PAD_LTHUMB     = 0x0040, PAD_RTHUMB     = 0x0080,
    PAD_LB         = 0x0100, PAD_RB         = 0x0200,
    PAD_A          = 0x1000, PAD_B          = 0x2000,
    PAD_X          = 0x4000, PAD_Y          = 0x8000,
};

// axes: [0] left stick x, [1] left stick y, [2] right stick x, [3] right
// stick y (all -1..1, up/right positive, matching XInput), [4] left trigger,
// [5] right trigger (0..1). live=false clears the snapshot (doff, session
// park), which makes the detour a pure pass-through.
void set_touch_pad(uint32_t buttons, const float axes[6], bool live);
bool touch_pad(uint32_t* buttons, float axes[6]);  // false while not live

}  // namespace headpose
}  // namespace grwxr
