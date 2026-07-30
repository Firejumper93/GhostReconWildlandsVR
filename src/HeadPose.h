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

namespace grwxr {
namespace headpose {

// Render thread, once per presented frame. R is a 3x3 rotation, row-major,
// game basis, row-vector convention.
void publish(const float R[9]);

// Stop the camera write (session STOPPING/EXITING, shutdown). read() returns
// false until the next publish, so the game's own camera takes back over.
void disable();

// Engine thread. Copies the latest published rotation into R and returns
// true. Returns false if nothing has been published, the channel is disabled,
// or the read tore; the caller must then leave the camera untouched.
bool read(float R[9]);

// Engine thread, from the proj[2] hook: the vertical field of view the frame
// is being rendered with, in radians.
void publish_fov(float fovy_radians);

// Render thread. The latest published fovy in radians, or `fallback` if
// nothing has been published yet this run.
float read_fov(float fallback);

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

// BUILD 10b.1: frame-to-eye identity FIFO. The engine pipelines frame builds
// ahead of their Presents by an unknown (and possibly varying) depth, so the
// present side must NOT derive the eye of the backbuffer image from frame
// parity. Instead the camera hook pushes one tag per built frame (the eye it
// offset the camera toward), and the present side pops exactly one tag per
// present. Frames present in build order, so the oldest tag always names the
// image being presented, whatever the pipeline depth. Single producer
// (engine thread), single consumer (render thread), lock-free ring.
void push_eye_tag(int eye);   // engine thread, once per built frame
int  pop_eye_tag();           // render thread, once per present; -1 if none
                              // (no camera write happened for this frame:
                              // menus, loading; treat the image as mono)
int  eye_tag_depth();         // diagnostic: current ring occupancy

// BUILD 10b.2 diagnostics: how many pops found a tag vs came up empty (mono).
// A large mono count during play means the tag stream is starving and the
// stereo routing cannot be trusted.
unsigned long long pops_tagged();
unsigned long long pops_mono();

}  // namespace headpose
}  // namespace grwxr
