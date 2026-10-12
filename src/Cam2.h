// Cam2.h - BUILD 243: the second-camera EXPERIMENT. Off by default.
//
// THE QUESTION. The mod renders one eye per game frame (alternate eye). Rendering
// both eyes per frame needs the engine to draw two cameras in one frame. REPORT.md
// section 9 (C:\Claude\scratch-tu25-render) read the TU25 binary and found that
// the main view's camera array can take a second Camera object (the engine's own
// AddCamera, view vtable +8) and that the per-frame graph builder then renders it
// like any other camera. This build tries exactly that on the DESKTOP: camera 0
// is given the left half of the screen and a new camera 2 the right half, 6.4 cm
// to the right of camera 0. No headset and no OpenXR session are involved.
//
// WHAT IT DOES (cfg cam2_experiment = 1; the code default is 0 = nothing at all):
//   1. Waits for frame 600, then verifies the first bytes of every engine
//      function and instruction it will call or read against the TU25 GRW.exe pins (read from the exe on
//      disk, read-only) and refuses, creating nothing, on any mismatch.
//   2. (BUILD 245: only after the gameplay gate below has held) Installs the graph-builder hook through
//      the census machinery (Census.h, acquire_graph_hook): the same verified two-phase thunk swap, no
//      session needed. The callback runs on the builder's own thread, before its body.
//   3. At that entry, once the main view and camera 0 exist and camera 0's
//      surface handle is valid and the view holds exactly ONE camera, it
//      creates camera 2 (engine factory), copies camera 0's configuration the way
//      the engine's own camera clone does, adds it to the view ONCE (there is no
//      RemoveCamera), and shares camera 0's surface handle. SEH-guarded; a fault
//      disables the experiment for the run and is never retried.
//   4. At every later entry: reads the two viewports back (camera 0 left half,
//      camera 2 right half, set once by the engine's SetViewport in build 245) and re-applies the
//      setter only on drift, re-syncs the surface only when camera 0's surface
//      object changes (build 244), and writes camera 2's
//      pose = camera 0's pose moved cam2_ipd metres along camera 0's right axis.
//
// KEYING. The mod's player-camera hooks (CameraProbe.cpp selector and
// on_calc_mvp) used to recognise the player camera by Camera+0x290 == 0 alone,
// which camera 2 also satisfies. They now skip any call whose camera pointer is
// camera 2 (second_camera() below), so camera 2 never toggles the eye state,
// never pushes an eye tag, never gets a head-pose write and never becomes
// g_player_cam. With the experiment off second_camera() is 0 and nothing changes.
//
// BUILD 245 (cfg keys below; every one is inert while cam2_experiment is 0):
//   ARM ONLY IN LIVE GAMEPLAY. Build 244 armed at frame 600, in a title or loading screen, and the
//   game then took ACCESS_VIOLATIONs in its own ProjShadow pass (the atlas at renderer+0xB60 was
//   null). The experiment now verifies its pins at frame 600 and then WAITS: the hook is not
//   installed, and camera 2 is not created, until the gameplay latch (Cam2.cpp, gate_eval) has held
//   continuously for cam2_arm_seconds (default 45) of presented frames. The latch is the mod's own
//   `wgun: ARMED` state plus a live skeleton post-update count, the main view with exactly one
//   mode-0 camera, and the selector-captured player camera equal to that camera.
//   READ-ONLY DISCRIMINATOR. From the moment the hook goes in (2 s before creation) every builder
//   entry records the renderer's ProjShadow state into a preallocated ring (renderer+0x8C0 byte,
//   +0xB60 atlas, +0xD90 culling object, +0xD98 slot, the ProjShadow count at cull+0x8570 for both
//   cameras). The last 300 rows are written to GRWVR\cam2diag-<pid>.txt by the init thread: every
//   5 s while recording, when the state becomes FAULTED/LOST, on the game-stopped-presenting signal
//   (on_stall) and at process detach (on_exit). Never from a faulting worker.
//   VIEWPORTS BY THE ENGINE'S SETTER (FUN_14d92b050): once after creation, re-applied only on drift.
//   cam2_noprojshadow = 1 clears bit 4 of renderer+0x8C0 at each builder entry while RUNNING.
//
// RULE 8. The builder callback logs nothing, takes no lock and allocates nothing
// of its own (the engine's factory allocates, on the engine's own thread, once).
// All logging is the 1 Hz tick() on the init thread.
//
// RULE 7. TU25 only (PE timestamp and SizeOfImage). Anything else installs nothing.

#pragma once

#include <cstdint>

namespace grwxr {
namespace cam2 {

void set_enabled(bool on);     // cfg cam2_experiment, any thread; latched at the first tick that sees it on
void set_ipd(float metres);    // cfg cam2_ipd, any thread
bool requested();

// BUILD 245. cfg cam2_arm_seconds (default 45, clamped to [1, 3600]): how long the gameplay latch must
// hold, continuously, before camera 2 may be created. cfg cam2_noprojshadow (default 0). Any thread.
void set_arm_seconds(float seconds);
void set_noprojshadow(bool on);

// BUILD 245: the dump triggers that live outside this file. Both are no-ops unless the experiment has
// started recording. Call them from the init thread (never from a faulting game worker, never from
// Present): on_stall from the heartbeat's "game stopped presenting" branch, on_exit from DLL detach.
void on_stall(const char* why);
void on_exit();

// The camera pointer of the second camera, 0 until it exists (and 0 forever when
// the experiment is off). Hot-path safe: one relaxed load.
uint64_t second_camera();

// ===========================================================================================
// BUILD 247: TRUE STEREO. The two engine cameras ARE the two eyes. cfg cam2_stereo (default 0).
//
// With cam2_stereo = 0 NOTHING below runs and every function here returns false / 0: the build is
// the build 246 behaviour. The owner sets, for a headset run (see the report of build 247):
//     cam2_experiment = 1      (creates camera 2, as before)
//     cam2_stereo = 1          (routes the two cameras to the two eyes)
//     cam2_noprojshadow = 1    (projected shadows off in this mode, the F-255 condition)
//     ipd_scale = 1.0          (the camera separation is 0.5 * measured IPD * ipd_scale per side)
//
//   camera 0 (the player camera, viewport (0,0,.5,1)) = the LEFT eye  = OpenXR view 0 = left half of the backbuffer.
//   camera 2 (viewport (.5,0,.5,1))                    = the RIGHT eye = OpenXR view 1 = right half.
// Both poses are written by the SAME function (CameraProbe.cpp write_pose_head), in the same call, from
// the same composed head pose, one on each side of the viewpoint along the camera's right axis. The
// builder callback no longer writes camera 2's pose in this mode.
// ===========================================================================================
void set_stereo(bool on);              // cfg cam2_stereo, any thread
void set_stereo_swap_eyes(bool on);    // cfg cam2_stereo_swap_eyes (default 0): left half to eye 1 and right half to eye 0
void set_stereo_aspect(float a);       // cfg cam2_stereo_aspect (default 0 = the half viewport's own aspect)
void set_stereo_mirror_fov(bool on);   // cfg cam2_stereo_mirror_fov (default 1): camera 0's +0x2B0 fov into camera 2 each frame
bool stereo_requested();               // the cfg value
bool stereo_live();                    // requested AND camera 2 RUNNING AND both viewports as set. Hot path: two relaxed loads
bool stereo_swap_eyes();
float stereo_aspect_override();        // 0 = none
uint64_t first_camera();               // camera 0 (the player camera) while RUNNING, else 0. Hot path: one relaxed load
bool stereo_warm();                    // true once camera 2 has rendered a few frames and the viewports have been steady

// Engine thread (the camera selector / on_calc_mvp hooks). No logging, no locks, no allocation.
//   stereo_publish_pose: the 16 floats camera 2 must hold for frame `frame`: latched and written to camera 2.
//   stereo_cam2_call:    camera 2's own selector call: counts the order of the two cameras' calls and re-asserts
//                        the latched pose when it is this frame's.
//   stereo_mirror:       camera 2 = camera 0's current pose moved cam2_ipd along its right axis (no head pose
//                        available: desktop, tracking lost). Reads camera 0 with an SEH-guarded copy.
void stereo_publish_pose(uint64_t frame, const float m2[16]);
void stereo_cam2_call(uint64_t frame);
// BUILD 267: camera 2's recompute entry (the hook): re-assert this build's latched pose.
void stereo_reassert_current();
void stereo_mirror(uint64_t cam0);

// Projection hook (persp2): the aspect the engine used for a camera this call. One relaxed load when off.
void stereo_note_proj(float aspect);

// BUILD 248: ONE FRUSTUM FOR BOTH EYES, AND EACH EYE PLACED FROM ITS OWN CAMERA.
// In a half-width viewport the engine converts each world projection's fov so that the HORIZONTAL extent is
// kept: its native 0.815 rad (full width) arrives at persp2 as 1.424 rad (75 deg across), and the mod's own
// 1.92 override arrives as 2.468 rad (137 deg across). The two alternated frame to frame and between the two
// cameras, so the eyes disagreed and the placement size jumped (black borders, a field of view too high,
// close-up doubling). The persp2 hook now forces every WORLD call in a half-width viewport to one target
// vertical fov sized to the headset's per-eye view, and the placement reads each camera's own projection.
void set_stereo_target_fovy(float fovy_half_viewport);   // Present thread, from the headset's per-eye fovs
float stereo_target_fovy();
// persp2 hook (engine thread, no logging): when `fovy`/`aspect` are a world projection in a half viewport,
// returns true and writes the forced fovy. `lo` is headpose::fs_fov_lo(): below it (ADS, scopes) it is left alone.
bool stereo_override_fovy(float fovy, float aspect, float lo, float* out);
// Camera + 0x420 is the projection persp2 stored for that camera: tangents of the half-extents it renders.
bool camera_proj_tans(uint64_t cam, float* tan_h, float* tan_v);
// Present thread, once per stereo frame: what the placement used for each eye (for the 1 Hz line).
void stereo_note_place(const float th[2], const float tv[2], const bool from_cam[2]);
// BUILD 251: the layer pose lag in use and how many stereo frames got a lagged orientation or found none in the ring (1 Hz line).
void stereo_note_lag(int lag, uint32_t used, uint32_t miss);

// Present thread, once per presented frame while cam2_stereo is requested.
enum StereoKind : int {
    kStereoAlt = 0,        // the alternate-eye path ran (tag 0/1, or stereo not live)
    kStereoBoth = 1,       // both eyes fresh from the two halves
    kStereoMonoLeft = 2,   // camera 2 not trusted for this frame (warm-up, no tag): the left half went to both eyes
    kStereoNotReady = 3,   // a stereo tag arrived but the half textures / blit were not usable: alternate path ran
    kStereoFailed = 4      // a draw or an image acquire failed after the first eye was drawn
};
void stereo_note_frame(int kind, int tag, uint64_t frame, const float pos[2][3], const float ori[2][4],
                       const float fov[2][4], float cfov, uint32_t eye_w, uint32_t eye_h, float aspect, bool poses_ok);
void stereo_note_reason(const char* literal);   // the first fallback reason, logged once (string literals only)

// Init thread, once a second. Verifies, installs, logs the 1 Hz "cam2:" lines.
void tick();

}  // namespace cam2
}  // namespace grwxr
