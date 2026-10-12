#include "VRMirror.h"
#include "AimTrace.h"
#include "Voice.h"
#include "CameraProbe.h"
#include "HeadPose.h"
#include "Log.h"
#include "Menu.h"
#include "WeaponProbe.h"
#include "Perf.h"   // BUILD 215
#include "Foveate.h"   // BUILD 216
#include "XInputMerge.h"
#include "XrLayers.h"   // BUILD 233: the API layer manifest scanner
#include "StallWatch.h"   // BUILD 235: the thread snapshot when the first frame stalls
#include "Anatomy.h"      // BUILD 241
#include "Census.h"       // BUILD 242
#include "CbDump.h"       // BUILD 246
#include "Cam2.h"         // BUILD 243: cfg cam2_experiment, cam2_ipd
#include "PassGuard.h"    // BUILD 259: cfg projshadow_guard
#include "EyeRec.h"       // BUILD 248: the eye recorder (diagnostic, off by default)
#include "GtRec.h"        // BUILD 270: the ground-truth stereo recorder (diagnostic, log only, off by default)

#include <cstring>   // memcpy for the overlay-gun constant buffer

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dxgi1_4.h>   // BUILD 215: IDXGIAdapter3::QueryVideoMemoryInfo, the process's VRAM
#include <psapi.h>     // BUILD 215: GetProcessMemoryInfo, on the init thread only
#include <cstdarg>
#include <cstdio>

#include <atomic>
#include <string>   // BUILD 233: the API layer manifest scan
#include <vector>
#include <cmath>

namespace grwxr {
namespace vr {
namespace {

XrInstance   g_instance   = XR_NULL_HANDLE;
XrSystemId   g_system     = XR_NULL_SYSTEM_ID;
XrSession    g_session    = XR_NULL_HANDLE;
XrSpace      g_space      = XR_NULL_HANDLE;
XrSpace      g_view_space = XR_NULL_HANDLE;
XrSpace      g_stage_space = XR_NULL_HANDLE;   // BUILD 205: the floor, for the head's height; may stay null

// BUILD 13b: motion-control step 1, INPUT PLUMBING ONLY. One action set with
// a per-hand aim pose and a per-hand trigger float, suggested for the Oculus
// Touch profile, synced and located every present, logged at 1 Hz from the
// heartbeat thread. No gameplay effect: this build only proves the mod can
// see the controllers. Any failure here logs loudly at init and leaves
// g_input_ok false; the rest of the mod runs exactly as before.
XrActionSet g_actionset = XR_NULL_HANDLE;
XrAction    g_act_aim   = XR_NULL_HANDLE;
XrAction    g_act_trig  = XR_NULL_HANDLE;
XrAction    g_act_grip  = XR_NULL_HANDLE;   // build 14e.2, the aim-steer gate
XrAction    g_act_gpose = XR_NULL_HANDLE;   // BUILD 162, the grip POSE (anatomical frame)
// Build 22: the rest of the Touch surface, for the XInput merge.
XrAction    g_act_stick    = XR_NULL_HANDLE;   // vector2f, both hands
XrAction    g_act_stickclk = XR_NULL_HANDLE;   // boolean, both hands
XrAction    g_act_btn_a    = XR_NULL_HANDLE;   // right hand
XrAction    g_act_btn_b    = XR_NULL_HANDLE;   // right hand
XrAction    g_act_btn_x    = XR_NULL_HANDLE;   // left hand
XrAction    g_act_btn_y    = XR_NULL_HANDLE;   // left hand
XrAction    g_act_menu     = XR_NULL_HANDLE;   // left hand
XrPath      g_hand[2]      = {XR_NULL_PATH, XR_NULL_PATH};
XrSpace     g_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
XrSpace     g_grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};   // BUILD 162
bool        g_input_ok = false;
// Hot path -> heartbeat handoff (rule 8: the present hook only stores plain
// atomics; the heartbeat thread formats and logs).
std::atomic<uint32_t> g_in_seen{0};   // bit0 = left tracked, bit1 = right
// BUILD 124: HANDEDNESS. Which PHYSICAL hand is the trigger and pivot hand.
// 1 = right (the default, and byte-identical to every build before this one),
// 0 = left.
//
// `[VERIFIED, log grwxr-28520, 84 samples]` the tester shoots LEFT HANDED. His
// LEFT trigger reached 1.00 and sat above the 0.55 threshold in 19 samples; his
// RIGHT trigger peaked at 0.60 and crossed once. ADS and FIRE both read the
// right trigger, so both reported `0 holds` and `0 pulls` in a run he spent
// testing ADS. `aim_fire` had been ENABLED the whole time and had never once
// fired, which is not a bug in the fire code: nothing was ever pulling the
// trigger it watches.
//
// The same fact explains the backwards two-hand barrel found in build 123: his
// front hand is his RIGHT because his left is on the grip. Both symptoms are
// one root cause, and this is his own report, "the left and right directions
// are reversed".
//
// The swap is applied ONCE, where the poses are stored, so all seventeen
// consumers that hardcode hand index 1 become correct together and none can be
// missed. Slot 1 means "main hand" from here down, not "right hand". The
// gamepad snapshot is deliberately NOT swapped: it is the game's own pad
// mapping and the game has its own rebinding for it.
std::atomic<int>      g_main_hand{1};
std::atomic<float>    g_in_pos[2][3] = {};
std::atomic<float>    g_in_ori[2][4] = {};
std::atomic<float>    g_in_trig[2]   = {};
std::atomic<float>    g_in_grip[2]   = {};   // build 14e.2
// BUILD 162: the grip POSE orientation per logical slot, and whether it was
// located this frame. Orientation only: the aim pose keeps the position.
std::atomic<float>    g_in_gori[2][4] = {};
std::atomic<bool>     g_in_gtrack[2]  = {};
std::atomic<int>      g_in_stick_rc[2] = {}; // build 22.1: (XrResult<<1)|isActive

// BUILD 14d: CONTROLLER AIM STEER. While the RIGHT trigger is held (the game
// never sees Touch input, so squeezing it fires nothing), the angle between
// the right controller's aim ray and the head's forward becomes a turn rate,
// injected as relative MOUSE input (SendInput). The game's own mouse-aim path
// does the turning, so ballistics, HUD and crosshair stay exactly true; the
// gun stays at view center by construction (real decoupled hand aim is the
// IK phase). Deadzone, gain and clamp per the controller-transform rule.
// Injection is gated on our process owning the foreground window, so it can
// never leak mouse input into another app.
std::atomic<bool>      g_steer_on{true};      // cfg aim_steer
float                  g_steer_deadzone = 2.0f;   // deg, per axis (14e: the
                                                  // grab reference removed
                                                  // posture bias, so tighter)
float                  g_steer_gain     = 8.0f;   // (deg/s) per deg past deadzone
float                  g_steer_maxrate  = 180.0f; // deg/s clamp
float                  g_steer_mpd      = 10.0f;  // mouse counts per degree
std::atomic<uint64_t>  g_steer_ticks{0};          // presents that injected
std::atomic<long long> g_steer_dx{0}, g_steer_dy{0};   // total counts injected
std::atomic<float>     g_steer_yaw_last{0.0f}, g_steer_pitch_last{0.0f};

// Build 19: VR head aim state (see the aim_pump comment block below for the
// design). Render thread only; the signs are cfg keys read at init.
bool  g_vraim_on       = false;         // Numpad Decimal toggles
// BUILD 41: cfg aim_on_start (default ON). Motion aim used to start OFF and
// the ONLY way to arm it was a blind Numpad Decimal press inside the headset.
// That is how a working build gets reported as "motion controls didn't work":
// the gamepad emulation is automatic, so everything FEELS connected while the
// headline feature is simply switched off. Presence of the feature must not
// depend on the user finding a key they cannot see.
std::atomic<int> g_aim_on_start{1};
std::atomic<bool> g_vraim_armed_once{false};
float g_aim_sign_yaw   = 1.0f;          // cfg aim_yaw_sign  (+1 or -1)
float g_aim_sign_pitch = 1.0f;          // cfg aim_pitch_sign
float g_aim_cum[2]     = {0.0f, 0.0f};  // absorbed by the engine, geometric rad
float g_aim_zero[2]    = {0.0f, 0.0f};  // head-angle baseline (re-zeroed while
                                        // off or fov-gated: no snap on resume)
float g_aim_out[2]     = {0.0f, 0.0f};  // armed, not yet observed consumed
bool  g_aim_outf[2]    = {false, false};
// Build 49 fix 2: recenter also RESYNCS the aim accounting. g_aim_cum
// survives a recenter, so after one the compose faces base-minus-cum while
// the character aims at base: the body runs off at exactly the accumulated
// angle (tonight's log had 140 deg in cum) and recentering visibly "never
// fixes it". Every head-reference recapture now sets this; aim_pump zeroes
// the accounting and re-captures the baseline as soon as no injection is in
// flight, which snaps view and body back into agreement.
std::atomic<bool> g_aim_resync{false};
// Build 49 fix 3: the stall kicker. The pump is PARASITIC (hazard 45): armed
// deltas wait for the engine's own aim-setter calls, which stop while the
// player is idle, so aim goes numb until the next shot. When a delta has
// been pending too long, inject one alternating +-1-count mouse move (net
// zero over pairs, ~0.03 deg momentary) purely to make the engine run its
// aim path and consume. Rate-limited, foreground-gated like steer_aim.
std::atomic<long> g_aim_kicks{0};       // lifetime, for the 1 Hz diag line

// Build 23: the aim SOURCE. The pump is source-agnostic (it drives the
// engine's absolute pair toward whatever target angles it is handed, and the
// camera hook always composes the view from the true head pose), so the gun
// following the controller is just a different target: the right Touch
// controller's aim ray, extracted in the SAME recentered reference frame and
// game basis as the head. Skeleton rule 5: the ray gets an EMA filter before
// it reaches the game; the pump's own 0.30 rad per-injection clamp and the
// build-20 cumulative-pitch bound already cap it.
std::atomic<int> g_aim_source{1};       // cfg aim_source: 0 head, 1 right controller
float g_ctl_smooth = 0.35f;             // cfg aim_ctrl_smooth, EMA alpha per frame
float g_ctl_yaw    = 0.0f;              // filtered controller angles, render thread
float g_ctl_pitch  = 0.0f;
bool  g_ctl_have   = false;             // never tracked yet: fall back to head
std::atomic<float> g_ctl_diag[4] = {};  // 23.1: ctl yaw/pitch, head yaw/pitch
std::atomic<bool>  g_ctl_diag_ok{false};

// 2026-09-01: THE LOOK-REVERSAL PROBE. The owner's 02:39 session report, "the
// head reversed left and right look", cannot be localized from the existing
// lines: aim: prints the engine's setter yaw with no head yaw beside it, and
// ctlaim: is gated on aim_source = 1, which this cfg does not run. One atomic
// feeds a 1 Hz aimref: line in drain_input that puts the XR head yaw, the
// engine's own view forward and the setter value side by side, so one slow
// head turn shows which leg reverses. Log-only; remove when it is explained.
std::atomic<float> g_aimref_head{0.0f};  // yaw handed to aim_pump, rad
// BUILD 289: THE RECOIL RECORDER (log only, cfg recoil_rec, code default ON for this measurement build). His word
// 2026-10-11: the view rides up with recoil and stays; recoil should move the gun and hands, not the head. Before any fix
// the project needs to know HOW the kick reaches the view: through the pitch the engine passes to SetPitch (aim_lastval),
// or through the camera base alone (g_base pitch), and how fast it comes back. One row per Present from aim_pump (no I/O
// there: a plain ring), written to GRWVR\recoil-<pid>.csv by the 1 Hz drain thread.
struct RcRow {
    uint32_t t_ms;      // GetTickCount64 low 32 bits
    uint32_t shots;     // shot-direction hook count, all time
    float    trig;      // right trigger 0..1
    float    eng;       // engine SetPitch incoming value, degrees
    float    base;      // engine camera base pitch (g_base), degrees
    float    head;      // the pitch handed to the pump (head, plus barrel error when barrel aim is live), degrees
    uint32_t calls;     // SetPitch calls so far
    // BUILD 292: the split of the pump's target, so the view following the barrel can be told from the head
    float    ep;        // barrel pitch error added this frame by aim_barrel (the clamped ep), degrees; 0 if the barrel block did not run
    float    barrel;    // the barrel's pitch, degrees (asin of its z); 0 if the block did not run
    float    view;      // the engine view's pitch the barrel block compared against, degrees; 0 if the block did not run
    float    cum;       // the pump's booked pitch (g_aim_cum[1]), degrees
    uint8_t  flags;     // bit0 barrel block ran this frame, bit1 pump live, bit2 aimtrace::firing(), bit3 delta in flight
    float    fov;       // the fov the pump gates on, radians (ADS and optics read low)
    uint32_t obj;      // BUILD 292: the object SetPitch was last called on (rcx, low 32 bits)
};
// BUILD 292: written by the barrel block, read by the recoil recorder (same thread, Present)
float g_rcx_barrel = 0.0f, g_rcx_view = 0.0f;
unsigned g_rcx_frames_seen = 0;
constexpr uint32_t kRcN = 4096;
RcRow                 g_rc[kRcN];
std::atomic<uint32_t> g_rc_w{0};            // total rows written by the Present thread
uint32_t              g_rc_r = 0;           // drain thread's read position
std::atomic<int>      g_rc_on{1};
static float g_cfg_aim_pitch_abs = 1.0f;   // BUILD 290: the pitch baseline is absolute (the engine follows the head's own pitch); code default ON
std::atomic<int>   g_veh_probe{1};       // BUILD 260: cfg veh_probe, the log-only vehicle instrument (the veh: line)

// ---------------------------------------------------------------------------
// BUILD 84: aim_barrel. THE BULLET LEAVES THE BARREL.
//
// This reopens a line the project closed on 2026-08-05, and it reopens it
// because the reason it was closed does not apply to this shape.
//
// WHAT WAS ALREADY PROVEN, and then mis-filed. `aim_source = 1` drove the
// engine's own aim from the controller and the tester reported it "sorta
// follows the reticle". Bullets DID move. It was retired because in this engine
// the aim IS the camera, so continuously chasing the hand dragged the view, and
// the user's standing constraint (2026-08-05) forbids exactly that. The
// campaign that followed went looking for a separate "authoritative shot
// direction" and eliminated three candidates with proof (projectile relocation,
// spawn direction owner+0x140, the per-shot aim reader at 0x1464DEDB). Those
// eliminations are sound. The conclusion drawn from them, that the shot
// direction has not been found, is not: it was found in build 23. It is the
// engine's aim, and camera::aim_arm writes it.
//
// WHY THIS SHAPE IS ALLOWED WHERE aim_source = 1 WAS NOT. The user refined the
// constraint the same night he set it: "aim following the controller DURING HIP
// FIRE is wanted and works. The line is between 'aim tracks my hand while I am
// shooting' (good) and 'the camera drifts toward my hand while I am doing
// nothing' (rejected)." This is gated on the RIGHT TRIGGER, so it is the first
// of those and never the second. It also matches the 0.9 requirement as stated:
// the direction is established at trigger depress.
//
// WHY THE VIEW SHOULD NOT MOVE. Build 19's camera compose already rebuilds the
// engine's base rotation with the absorbed aim (aim_cum) REMOVED before
// composing the head on top. That machinery exists, ships, and is
// headset-confirmed. Every radian injected here is accounted into aim_cum by
// the pump exactly as head-driven injection is, so the compose subtracts it and
// the view stays the head's. `[INFERRED]` This is the load-bearing assumption of
// the build and it is what the headset test has to falsify or confirm. If the
// view turns anyway, the mechanism is not the pump but something downstream
// (third-person camera orbit follows the body, which we do not override), and
// that is a different and much more specific problem than the one we had.
//
// THE LOOP IS CLOSED, not open. aimtrace::view_fwd is the engine's OWN aim
// direction and aimtrace::barrel_dir is where the gun points, both in engine
// world space from the same pass. The error between them is fed to the pump, so
// once the engine absorbs it the error goes to zero and stays there. Nothing
// integrates and nothing can ratchet, which is the failure mode both Halo MCC
// VR and the Cyberpunk port hit with incremental aim.
//
// IT ALSO ANSWERS "unchanged by any outside force". Because the loop re-drives
// the error every frame while the trigger is held, recoil, sway and any other
// engine nudge to the aim are corrected out on the next frame. That is not an
// extra feature, it falls out of running closed-loop instead of one-shot.
//
// KNOWN LIMIT, stated rather than discovered later: aim_pump stops injecting
// below the 0.65 world-fov gate, which is ADS and optics. So this is HIP FIRE
// ONLY on this build. Sighted fire needs the gate revisited and is not in it.
// ---------------------------------------------------------------------------
std::atomic<int>   g_aim_barrel{0};       // cfg aim_barrel: 0 off, 1 on
std::atomic<float> g_aim_barrel_max{0.60f};  // cfg aim_barrel_max, rad, hard cap
std::atomic<float> g_bar_diag[2] = {};    // last applied yaw/pitch error, rad
std::atomic<unsigned> g_bar_frames{0};    // frames the loop drove the aim
std::atomic<unsigned> g_bar_nodir{0};     // trigger held, no barrel and no ray
std::atomic<unsigned> g_bar_noview{0};    // trigger held, engine aim unreadable
std::atomic<unsigned> g_bar_clamp{0};     // error hit the cap (suspect a frame)
std::atomic<int>   g_bar_src{0};          // 1 = barrel_dir, 2 = ctrl_ray fallback

// THE TRIGGER IS TWO-STAGE, and this feature lives entirely in the second
// stage. Build 14f puts ADS on the right trigger past 0.55 and build 14h puts
// FIRE past 0.90. So by the time a round leaves, the game has been aiming down
// sights for the whole first half of the pull, the world fov has dropped, and
// aim_pump's own `live` gate (fov >= 0.65) has switched injection OFF.
//
// Gating this on "trigger held" would therefore have shipped a feature that is
// inert at exactly the instant it is supposed to act, and the headset result
// would have read as "barrel aim does nothing" when the real answer is "barrel
// aim never ran". That is the most expensive failure class in this project's
// ledger, so it is handled here rather than discovered in a test.
//
// g_bar_want lets aim_pump know the loop is actively driving, so it can keep
// injecting through ordinary ADS. The floor drops only to mono_scope_fov, so a
// real magnified optic (where hazard 25 says the compose must stand down and
// the engine owns the sight picture) is still protected exactly as before.
std::atomic<bool>  g_bar_want{false};     // barrel aim is driving this frame

// BUILD 14f: TRIGGER = AIM DOWN SIGHTS. The right trigger drives the game's
// own ADS binding (right-mouse hold), so the game's existing hip-to-ADS
// animation raises the arms to the red dot / scope: visible aim-driven arm
// motion with zero skeleton work. Real IK replaces this later. Hysteresis so
// the analog trigger cannot chatter the button; ANY guard failure (feature
// off, controller lost, focus lost, shutdown) releases a held button so the
// game and the desktop can never be left stuck aiming.
std::atomic<bool>     g_ads_on{true};    // cfg aim_ads
std::atomic<bool>     g_ads_rmb{false};  // right mouse currently held by us
std::atomic<uint32_t> g_ads_holds{0};    // times ADS engaged (for the log)

void ads_send(bool down) {
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    SendInput(1, &in, sizeof(in));
    g_ads_rmb.store(down, std::memory_order_relaxed);
    if (down) g_ads_holds.fetch_add(1, std::memory_order_relaxed);
}

void ads_input() {
    const bool held = g_ads_rmb.load(std::memory_order_relaxed);
    bool want = false;
    if (g_ads_on.load(std::memory_order_relaxed) &&
        (g_in_seen.load(std::memory_order_relaxed) & 2u)) {
        const float t = g_in_trig[1].load(std::memory_order_relaxed);
        want = held ? (t > 0.35f) : (t >= 0.55f);
    }
    if (want) {
        HWND  fg  = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid != GetCurrentProcessId()) want = false;
    }
    if (want != held) ads_send(want);
}

// BUILD 14h: FULL PULL = FIRE. The trigger's travel becomes a two-stage
// trigger: past 0.55 the ADS hold above raises the weapon, past 0.90 a
// left-mouse hold fires it. Hysteresis on both stages (fire releases at
// 0.80, still inside the ADS band), so easing off stops shooting but keeps
// aiming, exactly like a real trigger wall. Automatic weapons hold fire for
// as long as the pull is held. Same guard structure as ADS: any guard
// failure, session park, or shutdown force-releases the held button.
std::atomic<bool>     g_fire_on{true};    // cfg aim_fire
std::atomic<bool>     g_fire_lmb{false};  // left mouse currently held by us
std::atomic<uint32_t> g_fire_pulls{0};    // times fire engaged (for the log)

void fire_send(bool down) {
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
    g_fire_lmb.store(down, std::memory_order_relaxed);
    if (down) g_fire_pulls.fetch_add(1, std::memory_order_relaxed);
}

void fire_input() {
    const bool held = g_fire_lmb.load(std::memory_order_relaxed);
    bool want = false;
    if (g_fire_on.load(std::memory_order_relaxed) &&
        (g_in_seen.load(std::memory_order_relaxed) & 2u)) {
        const float t = g_in_trig[1].load(std::memory_order_relaxed);
        want = held ? (t > 0.80f) : (t >= 0.90f);
    }
    if (want) {
        HWND  fg  = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid != GetCurrentProcessId()) want = false;
    }
    if (want != held) fire_send(want);
}

// BUILD 10a: one swapchain per eye. Each presented frame lands in one eye
// (alternating); the other eye re-submits its previous image.
XrSwapchain g_swapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
std::vector<XrSwapchainImageD3D11KHR> g_images[2];
uint32_t g_sc_w = 0, g_sc_h = 0;

// Per-eye submission state, render thread only. The stale eye's view must
// carry the pose and fov its image was RENDERED with, not the current ones;
// the compositor reprojects it to display time from there. valid goes false
// at start and after a session re-begin, and the next frame bootstraps both
// eyes from the same image.
// BUILD 11a: cfov is the CONTENT vertical fov (radians) the image was
// rendered with, stored at capture so the stale eye re-draws at its own
// frame's angular size, not the current one (matters mid zoom).
struct EyeSub { XrPosef pose; XrFovf fov; bool valid; float cfov; };
EyeSub g_eye[2] = {};

// BUILD 10i: private per-eye copy of each eye's last frame. The stale eye is
// re-copied from here into a FRESHLY ACQUIRED swapchain image every present,
// so both swapchains go through acquire/copy/release every frame. Before 10i
// the stale eye re-referenced its last released image without re-acquiring;
// the 10h merge measurement (character merges at ipd_scale ~ -11, i.e. the
// two copies sit ~15 deg apart, exactly the Quest 3 per-eye fov asymmetry)
// says the compositor displaces one of the two alternating submissions by
// the recommended-fov offset, and the re-reference path is the one thing
// that distinguishes them. Created at init on the game's device; null if
// creation failed, which falls back to the old re-reference behaviour.
ID3D11Texture2D* g_last[2] = {nullptr, nullptr};

// BUILD 12c: desktop recording view. With fullscreen (12a) the backbuffer
// carries the wide-fov render plus the per-frame eye alternation, which
// makes desktop recordings warped and shivery. When enabled, the present
// path redraws the backbuffer from the LEFT eye's capture (steady image, no
// alternation), cropped to desktop_fov, so the flat screen looks like
// normal gameplay while the headset keeps the wide view. The RTV is cached
// keyed on the backbuffer pointer (ResizeBuffers is unhooked; a resize
// changes the pointer and forces recreation). Typed format per hazard 22.
// Known trade-off, deliberate: the crop cuts corner HUD out of recordings;
// desktop_fov tunes how much survives. Skipped when the content fov is
// already at or below desktop_fov (scoped frames, fullscreen OFF).
ID3D11RenderTargetView* g_desk_rtv = nullptr;
ID3D11Texture2D*        g_desk_rtv_key = nullptr;  // identity only, not owned
float g_desk_fov = 0.90f;   // cfg desktop_fov, 0 disables the feature
bool  g_desk_on  = true;    // Numpad / toggles (render thread only)

// BUILD 10L: the canvas. The Quest Link runtime IGNORES the submitted
// per-view fov (proven by build 10k: submitting 99 instead of 45 degrees
// vertical changed nothing on screen) and stretches each eye's image across
// that eye's recommended asymmetric frustum. The frustums are mirrored, so
// each eye's content was displaced ~7 deg toward its temple: the constant
// binocular divergence (diplopia) behind every "doubled" report since build
// 9, and the stretch was the fisheye. Fix: allocate the swapchains as
// per-eye CANVASES shaped like the recommended frustum, place the game image
// angle-correct inside (per-eye integer offsets from the frustum tangents,
// black elsewhere), submit the recommended fov, which is then the truth.
// Created lazily on the first present with valid views, because the
// tangents come from xrLocateViews, which needs a frame time. Known
// limitation, deliberate: content rendered far from the pi/4 reference fov
// (scoped zoom) still shows at the wrong apparent size; that needs a
// scaling blit or the projection override, later.
bool     g_canvas_ready = false;
uint32_t g_content_w = 0, g_content_h = 0;   // game backbuffer size
int64_t  g_sc_format  = 0;
XrFovf   g_eyefov[2]  = {};
int      g_dst_x[2] = {0, 0}, g_dst_y[2] = {0, 0};
std::vector<ID3D11RenderTargetView*> g_rtv[2];

// BUILD 11a: the scaling blit. 10L placed the content with an integer copy
// sized for the game's STATIONARY fov (pi/4), but the live fov breathes
// 0.78..0.83 in motion (a few percent of misplacement, the prime suspect for
// the residual object off-ness) and drops to ~0.17 scoped (zoomed content
// stretched across the whole 45 degree window). Now the content is DRAWN
// into the canvas each present, its destination rectangle sized from the fov
// the frame was actually rendered with (published live by the proj[2] hook).
// The draw is a fullscreen triangle whose viewport IS the destination rect;
// no vertex buffer, no constant buffer. All device objects are created once
// at init (rule 8, hazard 20); the per-acquire clear-to-black stays, so the
// borders stay black as the rect resizes. If any resource fails to create,
// g_blit_ok stays false and the 10L fixed copy path runs unchanged.
ID3D11VertexShader*       g_blit_vs   = nullptr;
ID3D11PixelShader*        g_blit_ps   = nullptr;
ID3D11PixelShader*        g_pip_ps    = nullptr;   // BUILD 221: the PiP disc
ID3D11Buffer*             g_pip_cb    = nullptr;   // BUILD 221: its uv centre and half extent

// 2026-08-12: the settings panel. Menu.cpp has always been able to draw itself
// into an offscreen texture, and dllmain already toggles it on F1, calls
// init() and calls poll(). Nothing ever called render() or put the result in
// front of the eyes, so the panel existed and was invisible. These two carry
// it: the SRV is produced ONCE per frame (render() draws an ImGui frame, so
// calling it per eye would draw two), then composited into both eye canvases.
ID3D11BlendState*         g_menu_blend = nullptr;
ID3D11ShaderResourceView* g_menu_srv   = nullptr;

// Half the panel's angular width, as a tangent. Menu.h sizes the panel 0.9 m
// wide at 1.2 m away, which is 0.45/1.2, and that lands near one texel per
// headset pixel at 1024 wide so the text stays legible. The height follows
// from the texture's aspect, so the panel can never come out stretched.
constexpr float kMenuHalfTanX = 0.375f;
// Build 24: the controller-aim reticle. A small dot drawn into each eye's
// canvas at the controller ray's angular offset from the view center, so
// hip-fire aim is VISIBLE the whole time (the game's own crosshair is
// screen-centered and cannot show it). Render thread only.
ID3D11PixelShader*        g_dot_ps    = nullptr;
bool  g_dot_enable = true;    // cfg aim_reticle, 0 hides it
// BUILD 42: HAND MARKERS. Two coloured blobs drawn where the controllers
// actually are, left blue / right orange. This is the "gizmo first" step both
// reference projects used (docs/REF-cyberpunk-ik-pattern.md section 6,
// docs/REF-halo-mcc-pattern.md): prove the controller-to-view maths with drawn
// geometry BEFORE betting a bone write on it. It touches no engine memory, no
// skeleton and no animation, so nothing in the game can fight it.
bool  g_hand_enable  = true;          // cfg hand_markers, 0 hides them
float g_head_pos[3]  = {0, 0, 0};     // head position, reference space
bool  g_head_pos_ok  = false;
// BUILD 204: TRUE 6DOF. The head position at the reference (captured with
// the yaw reference, so a recenter re-zeroes it), the key, the clamp and the
// counters of the 1 Hz hpos: line.
float g_head_ref_pos[3] = {0, 0, 0};
bool  g_head_ref_ok  = false;
std::atomic<int>      g_head_pos_on{0};       // cfg head_pos
std::atomic<float>    g_head_pos_max{0.75f};  // cfg head_pos_max, metres
// BUILD 300: THE LEAN. cfg lean (code default ON) turns the head offset on with a soft limit instead of the hard head_pos_max clamp:
// the horizontal offset eases to lean_max, the vertical to lean_down below and lean_up above; lean_soft is the fraction of each limit
// where the easing begins. Holding the left menu button for lean_hold_ms re-zeroes the lean origin and the view (the Home recenter);
// a shorter press is still the game's Start, sent on release.
std::atomic<int>      g_lean_mode{0};
std::atomic<float>    g_lean_max{0.40f}, g_lean_soft{0.75f}, g_lean_down{0.25f}, g_lean_up{0.10f};
std::atomic<int>      g_lean_hold_ms{800};
std::atomic<uint64_t> g_lean_soft_n{0};       // frames the horizontal offset was past where the easing begins
std::atomic<uint64_t> g_lean_rc_menu{0};      // recentres by holding the left menu button
std::atomic<uint64_t> g_lean_start_taps{0};   // short presses passed to the game as Start on release
std::atomic<uint64_t> g_hp_frames{0};         // frames a valid offset was published
std::atomic<uint64_t> g_hp_clamps{0};         // frames the offset was clamped to head_pos_max
std::atomic<uint64_t> g_hp_refs{0};           // reference captures
std::atomic<float>    g_hp_last[3] = {};      // the last published offset (x, f, u)
// BUILD 205: THE STANCE LOOP. Desired from the physical head height, actual
// from the character's head bone, the pad's B while they disagree.
std::atomic<int>      g_stance_on{0};         // cfg stance
std::atomic<float>    g_stance_height{0.0f};  // cfg stance_height: 0 = the running maximum since the recenter
std::atomic<float>    g_stance_crouch{0.75f}; // cfg stance_crouch: below this fraction of standing = crouch
std::atomic<float>    g_stance_prone{0.0f};   // cfg stance_prone: below this fraction = prone; 0 = prone is his (BUILD 208 default)
// BUILD 301: THE HYBRID CROUCH (his choice 2026-10-12: the view follows the real height, a real crouch past a threshold taps the game's
// crouch, standing back up releases it by itself, prone stays his). The threshold is in metres of drop below the standing height taken at
// the recentre (crouch_drop, his to tune), with crouch_rise metres of hysteresis on the way up. crouch_drop 0 = the old 205 ratio.
std::atomic<float>    g_crouch_drop{0.25f}, g_crouch_rise{0.10f};
float                 g_st_drop = 0.0f;       // the real drop below standing this frame, metres (log)
float    g_st_height   = 0.0f;   // the physical head height this frame, metres above the floor
float    g_st_hstand   = 0.0f;   // the standing height in use
float    g_st_dzmax    = 0.0f;   // the bone height's running maximum since the recenter (standing, about 1.6)
bool     g_st_stage_ok = false;
int      g_st_desired  = 0;      // 0 stand, 1 crouch, 2 prone; -1 unknown
int      g_st_actual   = -1;
std::atomic<uint64_t> g_st_taps{0};
std::atomic<uint64_t> g_st_holds{0};
std::atomic<uint64_t> g_st_fights{0};       // back-offs after four acts without effect
std::atomic<uint64_t> g_st_changes{0};      // actual stance changes seen
// BUILD 206: the standing height, calibrated. Sources in order: cfg
// stance_height; the F3 capture (four seconds of samples starting one
// second after the press, averaged); the automatic average over the three
// seconds starting one second after the recenter. Never the maximum.
uint64_t g_st_cal_from = 0, g_st_cal_until = 0;   // the F3 window
double   g_st_cal_sum = 0.0; unsigned g_st_cal_n = 0;
float    g_st_hf3 = 0.0f; bool g_st_hf3_ok = false;
uint64_t g_st_ref_ms = 0;                          // when the reference was captured
double   g_st_auto_sum = 0.0; unsigned g_st_auto_n = 0; bool g_st_auto_done = false;
int      g_st_src = 0;                             // 0 auto, 1 F4, 2 cfg
float    g_st_tallest = 0.0f;                      // BUILD 302: the tallest accepted recentre average this session; a new average is taken only within 0.10 m of it
bool     g_st_seated = false;                      // BUILD 207: the head bone in the seat band (0.55 to 0.85): no B, no compensation
std::atomic<uint64_t> g_st_manual{0};             // BUILD 207: his own B closed the act window
// BUILD 209: THE BODY FOLLOW. Beyond a deadband the left stick is pushed
// toward the head (head-local x and f map straight onto the stick, the
// game's move is camera-relative and the camera follows the head's yaw),
// only while his own stick is idle and he is not rushing (the LS click),
// in first person, with a stereo frame, not in a seat. While the follow
// moves the body, the 6DOF reference advances by the body's own motion
// mapped back into XR space (the inverse of the hands' conversion), so
// the view stays still in the world as the body walks under the head.
std::atomic<int>      g_follow_on{0};        // cfg follow
std::atomic<float>    g_follow_dead{0.20f};  // cfg follow_dead, metres
// BUILD 211: follow_gain and follow_cap are retired. The stick no longer
// meters speed (the game discarded every magnitude 209 and 210 could produce);
// one magnitude closes, and the body's own measured speed decides when to stop.
std::atomic<float>    g_follow_push{0.75f};  // cfg follow_push, the stick while closing
std::atomic<float>    g_follow_stop{0.10f};  // cfg follow_stop, seconds of stopping distance aimed at
std::atomic<float>    g_follow_vmax{2.0f};   // cfg follow_vmax, m/s above which it coasts
std::atomic<float>    g_follow_bleed{0.30f}; // cfg follow_bleed, m/s the settle takes out of the view
std::atomic<int>      g_fl_state{0};         // 0 idle, 1 closing, 2 coast, 3 settle
std::atomic<uint64_t> g_fl_closes{0};        // times it set out to close
std::atomic<uint64_t> g_fl_settles{0};       // times it reached the settle
std::atomic<uint64_t> g_fl_blocked{0};       // closes abandoned: the body did not move (a wall)
std::atomic<uint64_t> g_fl_bleeds{0};        // frames the settle moved the reference
std::atomic<float>    g_fl_bled{0.0f};       // metres taken out of the view by the settle, cumulative
std::atomic<float>    g_fl_vbody{0.0f};      // the character's horizontal speed, m/s
std::atomic<float>    g_fl_vhead{0.0f};      // his own head's horizontal speed in XR, m/s
// BUILD 212: WHERE EVERY RE-ZERO OF THE VIEW CAME FROM. The 211 run took six
// references in 110 seconds off a single F2 press (grwxr-18104.log, refs=6,
// five `aim: RESYNC (recenter)`), and a re-zero re-takes the YAW reference,
// which turns the whole world to wherever his head happens to point: his
// words, 2026-09-06 22:20, "the head has a weird rotational swivel to it".
// note() shares one buffer drained once a second, so two notes in a second
// lose the first and none of this was in the log. These are counters.
std::atomic<uint64_t> g_rc_local{0};      // runtime changed LOCAL, no usable pose: the view re-zeroed
std::atomic<uint64_t> g_rc_absorbed{0};   // runtime changed LOCAL with a pose: transformed, the view did not move
std::atomic<uint64_t> g_rc_stage{0};      // runtime moved the floor: the standing height re-takes, the view untouched
std::atomic<uint64_t> g_rc_other{0};      // a space we hold nothing in
std::atomic<uint64_t> g_rc_key{0};        // Home or Space
std::atomic<uint64_t> g_rc_fp{0};         // F2, the panel, or a hand calibration asking for first person
std::atomic<uint64_t> g_rc_begin{0};      // the session started
std::atomic<uint64_t> g_fl_frames{0};        // frames the follow pushed the stick
std::atomic<uint64_t> g_fl_advances{0};      // reference advances
std::atomic<uint64_t> g_fl_teleports{0};     // body jumps of 0.35 m or more in a frame, not advanced
std::atomic<uint64_t> g_fl_user{0};          // frames suspended for his own stick
std::atomic<uint64_t> g_fl_rush{0};          // frames suspended for the LS click
std::atomic<float>    g_fl_stick{0.0f};      // the last stick magnitude pushed
std::atomic<float>    g_fl_dist{0.0f};       // the last horizontal head offset
std::atomic<float>    g_fl_adv{0.0f};        // the last reference advance, metres
std::atomic<uint64_t> g_fl_nocam{0};         // BUILD 210: frames the follow stood aside for no camera frame
std::atomic<uint64_t> g_fl_far{0};           // BUILD 210: frames the offset read 3 m or more (a tracking jump), not followed
static void stance_calib_start() {
    const uint64_t now = GetTickCount64();
    g_st_cal_from = now + 1000;
    g_st_cal_until = now + 5000;
    g_st_cal_sum = 0.0; g_st_cal_n = 0;
}
bool  g_hand_on[2]   = {false, false};
float g_hand_tx[2][2] = {};           // [hand][eye] tangent from view centre
float g_hand_ty[2][2] = {};
float g_hand_tr[2][2] = {};           // [hand][eye] tangent radius
ID3D11PixelShader* g_hand_ps[2] = {nullptr, nullptr};   // 0 left, 1 right
std::atomic<bool>  g_in_track[2] = {};   // per hand: located this frame
// BUILD 45: WEAPON-CANDIDATE MARKERS. One coloured blob per watched
// placement handle (wp::marker), drawn at the handle's ENGINE world position
// converted through the game camera's own basis. This is the hand-marker
// instrument generalised from "controller position in XR space" to "any
// world position the engine gives us" (CURRENT-STATE, session 24): the user
// looks at which colour sits on the gun and that handle is the weapon.
// Read-only with respect to the engine; draws into our canvas only.
constexpr int kWpm = wp::kWatch;   // slot index = colour index, fixed
bool  g_wpm_enable = true;              // cfg wp_markers, 0 hides them
bool  g_wskel_enable = false;           // cfg wskel, build 64: WHITE marker
                                        // rides the weapon-skeleton pick
bool  g_wskel_marker = false;           // BUILD 227: cfg wskel_marker, the white
                                        // blob itself; the pick runs without it
bool  g_palette_probe = false;          // BUILD 227: cfg palette_probe
bool  g_wpm_on[kWpm] = {};
float g_wpm_tx[kWpm][2] = {};           // [slot][eye] tangent from centre
float g_wpm_ty[kWpm][2] = {};
float g_wpm_tr[kWpm][2] = {};
ID3D11PixelShader* g_wpm_ps[kWpm] = {};

// BUILD 153: THE EVIDENCE RETICLE. cfg barrel_reticle, default OFF.
//
// The owner's ask, 2026-09-01: "a small reticle that appears in vr and on the
// screen for screen recording evidence". The purpose is to make the standing
// complaint provable on video instead of arguable. Two headset sessions on
// build 152 logged 199 shots, every one why=WROTE with the published
// direction 0.0 degrees off the controller ray, while the gun pointed 76 to
// 92 degrees away from the head and the round still followed the head.
//
// So this draws TWO markers at the SAME distance:
//   slot 0  BARREL, from the player's muzzle along barrel_dir_peek, which is
//           the exact vector written into the shot at SetShotLine
//   slot 1  GAZE, from the camera along the camera's own forward
// Their on-screen separation IS the divergence, frame by frame, and the round
// lands on one of them. It is the rayprobe's raw-vs-cam number as a picture.
//
// WHAT IT IS NOT, and the log line says so too: a fixed-distance dot does NOT
// predict an impact at a different range, because the camera and the muzzle
// are not the same point. Set barrel_reticle_dist to roughly the target range.
//
// Angular size is FIXED rather than distance-scaled, unlike the wp markers.
// A reticle wants to look the same whatever it is over, and a 6 cm blob at
// 25 m would be invisible on video, which is the whole point of the feature.
//
// Purely additive drawing. It reads barrel_dir_peek and base_frame and writes
// nothing, so it cannot reach the shot chain the hard baseline guards.
constexpr int kRet = 2;                 // 0 = barrel, 1 = gaze
int   g_ret_mode   = 0;                 // cfg barrel_reticle: 0 off, 1 barrel, 2 both
float g_ret_dist   = 20.0f;             // cfg barrel_reticle_dist, metres
float g_ret_size   = 0.010f;            // half-tangent, about 1.1 deg across
bool  g_ret_on[kRet] = {};
float g_ret_tx[kRet][2] = {};
float g_ret_ty[kRet][2] = {};
ID3D11PixelShader* g_ret_ps[kRet] = {};
// BUILD 47: the placement write test (WeaponProbe.h set_write). cfg-driven,
// hot-reloaded, default OFF. slot picks the colour to write (0 RED 1 GREEN
// 2 YELLOW 3 MAGENTA 4 CYAN 5 WHITE, -1 off and the kill switch); mode 1
// lifts the handle wp_write_up metres straight up (the writability
// diagnostic), mode 2 pins its position to the right controller. Parsed on
// the init thread, read on the render thread; int/float tearing is not a
// hazard here and the wp:: side re-clamps everything.
int   g_wpw_slot = -1;
int   g_wpw_mode = 1;
float g_wpw_up   = 0.30f;
// Build 48: direct handle targeting (hex, 0 = off / use the slot). Colour
// slots proved to RESHUFFLE mid-session; the handle read off the wpm/wpw
// log lines is the stable name for a target within a session.
unsigned g_wpw_handle = 0;
// Build 51: the per-shot aim override, degrees, held here so either cfg key
// can be edited alone without the other resetting to zero.
float g_shot_yaw_deg   = 0.0f;
float g_shot_pitch_deg = 0.0f;
// Build 52: override target RVAs, 0 = the build-pinned per-shot sites.
unsigned g_shot_site_yaw   = 0;
unsigned g_shot_site_pitch = 0;
// Build 55: the aim orientation quaternion override.
float g_aq_deg  = 0.0f;
int   g_aq_axis = 2;
bool  g_dot_on = false;       // this frame: draw it
float g_dot_tx = 0.0f;        // tangent offsets from view center
float g_dot_ty = 0.0f;
// BUILD 221: the PiP scope, present thread. The disc sits where the eyepiece
// projects in each eye (the BUILD 45 route: engine world through base_frame,
// with the per-eye IPD shift), and shows the frame around the barrel's aim
// tangent (the dot's own numbers) at pip_zoom.
bool  g_pip_enable = false;   // cfg pip; off in code, the cfg turns it on
float g_pip_zoom   = 2.7f;    // cfg pip_zoom; 2.7 is the ACOG's own game value
float g_pip_radius = 0.016f;  // cfg pip_radius, metres, the glass's radius
bool  g_pip_show   = false;
float g_pip_tx[2] = {0.0f, 0.0f}, g_pip_ty[2] = {0.0f, 0.0f}, g_pip_tr[2] = {0.0f, 0.0f};
float g_pip_atx = 0.0f, g_pip_aty = 0.0f;   // the aim tangent, the source centre
// BUILD 237: THE STALE-EYE WOBBLE. g_pip_atx/aty above are ONE pair, computed
// once per Present from THIS frame's barrel direction, and then used to sample
// both eyes' discs. But under alternate-eye rendering only one eye's content
// is new each Present; the other is re-submitting the image it was given last
// time. So the stale eye was cropping a one-Present-old picture around this
// frame's aim point - a 36 Hz oscillation, magnified by the zoom, which is a
// large part of why the disc reads as unsteady rather than merely soft.
//
// The fix is to latch the pair PER EYE at the moment that eye's content is
// captured, and to sample each eye's disc with the pair its own picture was
// rendered with. Written on the present thread at the capture, read on the
// same thread at the draw.
float g_pip_atx_eye[2] = {0.0f, 0.0f};
float g_pip_aty_eye[2] = {0.0f, 0.0f};
ID3D11SamplerState*       g_blit_samp = nullptr;
ID3D11RasterizerState*    g_blit_rs   = nullptr;
ID3D11DepthStencilState*  g_blit_dss  = nullptr;
ID3D11ShaderResourceView* g_last_srv[2] = {nullptr, nullptr};
bool g_blit_ok = false;

// BUILD 118: does the runtime expose XR_KHR_composition_layer_depth, and did
// we enable it at instance creation. Latched once in init(), read-only after.
// CLAUDE.md names stale-eye reprojection as "the single most important delta
// from the reference"; a submitted depth layer is what lets the compositor do
// POSITIONAL timewarp instead of orientation-only, which is the architectural
// half of that requirement we can get without writing our own pass.
bool g_depth_ext = false;

// Per-eye canvas mapping, from create_canvases: tangent units to canvas
// pixels (sx, sy) and the canvas pixel of the optical center (cx, cy). The
// canvas texture spans exactly [tanL..tanR] x [tanU..tanD] as submitted, so
// these are exact, absorbing the even-rounding of the canvas dimensions.
float g_sx[2] = {}, g_sy[2] = {}, g_cx[2] = {}, g_cy[2] = {};

// 11a.2: RTV creation results (create_canvases). Never checked before; the
// clear path degraded silently on failure, the blit path cannot.
int  g_rtv_fail = 0;
long g_rtv_hr   = 0;

// 11a.2: one-shot pixel probe. A 1x1 staging texture; a few seconds in, one
// pixel of the blit source and one of the canvas after the draw are read
// back and noted, so the log says where black enters the chain.
ID3D11Texture2D* g_probe_tex  = nullptr;
bool             g_probe_done = false;

// The game's stationary vertical fov, the placement assumption 10L hardcoded
// and the fallback until the first proj[2] publish (menus, loading).
constexpr float kRefFov = 0.7853982f;   // pi/4

// BUILD 11e: magnified-scope display window. Angle-correct display of a
// zoomed fov is ZERO effective magnification (9.9 deg of world across
// 9.9 deg of view is what the naked eye sees): the 11a scope picture was
// "very far away" and unusable. A real scope spreads its narrow cone across
// a wide visual field, so below the mono_scope_fov threshold the content is
// drawn across this fixed window instead of its true angle. 6x content at
// 0.172 rad shown across 0.5236 rad reads as ~3x vs naked eye per axis;
// tune in cfg. 0 = angle-correct (disable).
float g_scope_disp_fov = 0.5236f;

XrSessionState g_state_xr = XR_SESSION_STATE_UNKNOWN;
std::atomic<bool> g_active{false};
std::atomic<bool> g_failed{false};
// Build 38: session created but not yet READY (headset asleep or idle). The
// init thread keeps polling and begins the session when the headset wakes,
// instead of the old behaviour of latching g_failed and needing a relaunch.
std::atomic<bool> g_pending{false};
// Render-thread only. Set when the session is unrecoverable (EXITING,
// LOSS_PENDING, or a failed re-begin); on_present becomes a no-op then.
bool g_dead = false;

// Deferred diagnostics: the render thread fills these, our own thread logs them.
std::atomic<bool> g_want_log{false};
char g_deferred[2048] = {};

// A staging copy of the backbuffer. The backbuffer may be multisampled or in a
// format the runtime will not accept directly, so we always go through our own
// texture rather than assuming we can CopyResource straight in.
ID3D11Texture2D* g_staging = nullptr;

// Build 67: gun-root write cfg, applied once the whole file is parsed so the
// mode and the offset always arrive at the setter together.
static float g_cfg_wgun    = 0.0f;
static float g_cfg_wnode    = 0.0f;
static float g_cfg_wnode_dz = 0.30f;
static float g_cfg_wgun_dz = 0.30f;
static float g_cfg_wbaxis  = 0.0f;   // build 79, log-only barrel-axis measure
// 2026-08-13: two-handed aim and barrel roll, both ON by default. Each falls
// back to the previous one-handed behaviour by itself when its inputs are
// missing, so defaulting them on cannot strand a user with a broken weapon.
static float g_cfg_wgun_twohand  = 1.0f;
// BUILD 120: the two-hand agreement gate. 0.35 is the old hard-coded value,
// so an untouched cfg reproduces build 119 exactly.
static float g_cfg_wgun_two_agree = 0.35f;
// 2026-08-31b: the attach band's release edge, metres of hand separation.
// 0.45 sits above every measured real hold (max 0.409) and below the one
// documented phantom-hand split (0.507). See CameraProbe.h.
static float g_cfg_wgun_two_attach = 0.45f;
// BUILD 123: measure which hand is in front rather than assume the left one.
// ON by default, because the assumption is verifiably wrong for this tester's
// hold and a correct hold never trips the flip. See CameraProbe.h.
static float g_cfg_wgun_two_auto  = 1.0f;
// BUILD 124: 1 = right handed (default, identical to every earlier build),
// 0 = left handed. See the long comment at g_main_hand.
static float g_cfg_main_hand      = 1.0f;
// BUILD 125: the eye-sign A/B. +1 is every build since 10m. See CameraProbe.h.
static float g_cfg_ipd_swap       = 0.0f;
// BUILD 126: the blob test. Bypasses the static grip offset so the gun parks on
// the tracked hand position. Diagnostic, default off, tuned values untouched.
static float g_cfg_wgun_grip_bypass = 0.0f;
// 2026-08-31: the hands hold the gun, and the rear-hand anchor. Both ON by
// default: the hands write degrades to "the animation's own hands" the moment
// its inputs are missing, and the anchor correction is inert until the front
// hand measures as the right one, which is the exact case it exists for.
static float g_cfg_wgun_hands      = 1.0f;
static float g_cfg_wgun_two_anchor = 1.0f;
// Build 140: the grab (headset-confirmed hands, owner's correction: idle must
// not force both hands on) and the bone-level arm hide (diagnostic for the
// confirmed stretch; hide_child pieces are the cleaner cure if they work).
static float g_cfg_wgun_hands_grab = 1.0f;
static float g_cfg_wgun_hide_arms  = 0.0f;
// Build 141: hand presence. Off the weapon, the left hand follows the left
// controller, position only. Foundation for the reach interactions.
static float g_cfg_wgun_hand_free  = 1.0f;
// Build 142: the arm solve (the cure for the stretch that keeps the arms
// visible) and the wingspan capture (log only; leave hand_calib at 0 in the
// file, set it to 1 from the tuner when standing in the pose).
static float g_cfg_wgun_arms       = 1.0f;
static float g_cfg_wgun_wrist      = 1.0f;   // BUILD 158, CX-017
static float g_cfg_wgun_wrist_abs  = 1.0f;   // BUILD 162, absolute palm; 0 = 161 delta
static float g_cfg_wgun_twist      = 1.0f;   // BUILD 163, forearm roll split; 0 = 162
static float g_cfg_wgun_twist_share = 0.00f; // BUILD 167/169/170/171, the ForeArm bone's share; 0 since 171
static float g_cfg_wgun_twist_arm   = 0.00f; // BUILD 169/170, the Arm bone's share
static float g_cfg_wgun_wristfirst  = 1.0f;  // BUILD 176, the wrist-first arm; 0 = 162
static float g_cfg_wgun_wrist_limit = 30.0f; // BUILD 178, degrees; 0 = 176, 180 = 162
static float g_cfg_wgun_shoulder_max = 80.0f; // BUILD 179, mm the shoulder may travel
static float g_cfg_wgun_clav_max     = 25.0f; // BUILD 180, degrees the collarbone may turn
static float g_cfg_wgun_mirror       = 1.0f;  // BUILD 183, the mount mirror; 0 = 182, 2 = plus the hand props
static float g_cfg_wroll_probe       = 1.0f;  // BUILD 184, the roll probes, log only
static float g_cfg_wgun_roll_unflip  = 0.0f;  // BUILD 189, cancel the gun-up half turn; REFUTED, off
static float g_cfg_wgun_own          = 1.0f;  // BUILD 190, own the bone
static float g_cfg_fov_root          = 0.0f;  // BUILD 191, the fov root (the clouds); off until the headset says
static float g_cfg_fov_root_mode     = 1.0f;  // BUILD 194, 1 = release at the consuming projection call
static float g_cfg_fov_root_aimk     = 1.0f;  // BUILD 194, gain on injected head-aim deltas while armed; RETIRED in 197
static float g_cfg_fov_src           = 0.0f;  // BUILD 197, the source fov (the clouds); off until the headset says
static std::atomic<int> g_cfg_layer_lag{3};   // BUILD 251: cfg layer_pose_lag (whole presents), see the block above submit_halves
static std::atomic<int> g_cfg_veh_depth{2};   // BUILD 267: cfg veh_build_depth, the Present the vehicle label's build is looked up at: now minus this.
                                              // BUILD 271: default 2, not 1. The recorder (gtrec) measured the drawn build at K-2 on every resolved frame, depth 1 labelled it with a head one Present newer (label error median 0.26 deg), depth 2 is exact (median 1e-6 deg) and he passed it (F-261, F-262, INV-30).
static std::atomic<int> g_cfg_veh_lag{2};     // BUILD 266: cfg veh_layer_lag, the layer lag while the vehicle compose is on
// BUILD 270: gtrec's cfg snapshot for its file header (GtRecFormat.h FileHdr::cfg, log only). The values that have a getter
// are filled at the end of load_config; mvp_drive, mvp_observe, cam2_ipd, veh_camera and gtrec_cb where their keys are
// parsed, starting from their code defaults.
static float g_gt_cfg[16] = {0, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 0, 0, 0, 0.064f, 0, 1.0f, 1.0f};
static float g_cfg_fs_fov_cfg        = 1.92f; // BUILD 249: the fullscreen_fov the cfg asked for; stereo borrows headpose::fs_fov and gives it back
static bool  g_fs_stereo_applied     = false; // BUILD 249: headpose::fs_fov currently holds the stereo value
static float g_cfg_fov_scene         = 1.0f;  // BUILD 198, the scene fov override (the clouds); BUILD 199: default 1, the headset said yes 2026-09-06
static float g_cfg_fov_scene_ads     = 1.0f;  // BUILD 198, release it while the aim trigger is held
static float g_cfg_ffr               = 0.0f;  // BUILD 216, fixed foveated rendering (NVIDIA VRS); off until the headset says
static float g_cfg_ffr_inner         = 0.6f;  // BUILD 216, full rate inside this fraction of the visible half-extent
static float g_cfg_ffr_mid           = 0.8f;  // BUILD 216, half rate to here
static float g_cfg_ffr_outer         = 1.0f;  // BUILD 216, quarter rate to here, one sixteenth beyond
static float g_cfg_ffr_horizontal    = 1.0f;  // BUILD 216, 1 = the half-rate ring is 2x1 (vrperfkit favorHorizontal)
static float g_cfg_ffr_cull          = 0.0f;  // BUILD 216, 1 = tiles beyond the visible edge are culled, not shaded
static float g_cfg_ffr_margin        = 0.05f; // BUILD 216, tangent units of slack beyond the visible edge
static float g_cfg_sprint_probe      = 1.0f;  // BUILD 200, the sprint probe, log only
static float g_cfg_wgun_lead         = 1.0f;  // BUILD 201, the controller lead in frames; 0 = build 200
static float g_cfg_gpair             = 0.0f;  // BUILD 277, the gun pairing recorder, log only
static float g_cfg_wgun_pull         = 0.0f;  // BUILD 283, 1 = the limb is carried toward the hand so the arm is never past wgun_pull_k of full extension; 0 = build 282 exactly
static float g_cfg_wgun_pull_k       = 0.97f; // BUILD 283, the fraction of L1+L2 the arm may reach
static float g_cfg_wgun_pull_max     = 250.0f; // BUILD 283, mm, the most the limb is carried
static float g_cfg_wgun_drawcam      = 0.0f;  // BUILD 282, 1 = the hands are placed from the camera that draws them; 0 = build 279 exactly
static float g_cfg_head_roll_fix     = 0.0f;  // BUILD 202, the head roll: the submitted pose carries no roll; off until the headset says
static float g_cfg_head_pos          = 0.0f;  // BUILD 204, true 6DOF: the head's translation moves the viewpoint; off until the headset says
static float g_cfg_head_pos_max      = 0.75f; // BUILD 204, the clamp on that translation, metres
static float g_cfg_stance            = 1.0f;  // BUILD 205, the stance loop; BUILD 301: code default ON (the hybrid crouch, his choice 2026-10-12)
static float g_cfg_crouch_drop       = 0.25f; // BUILD 301, metres the real head drops below standing before the game crouch is tapped (0 = the 205 ratio)
static float g_cfg_crouch_rise       = 0.10f; // BUILD 301, metres back up from that threshold before the game stands again (hysteresis)
static float g_cfg_stance_height     = 0.0f;  // BUILD 205, standing height override, metres; 0 = auto
static float g_cfg_stance_crouch     = 0.75f; // BUILD 205
static float g_cfg_stance_prone      = 0.0f;  // BUILD 205; BUILD 208: 0 = the head picks crouch only, prone stays the pad's
static float g_cfg_stance_calib      = 0.0f;  // BUILD 207, momentary: 1 starts the four-second standing height capture
static float g_cfg_follow            = 0.0f;  // BUILD 209, the body follow; off until the headset says
static float g_cfg_arm_pole          = 0.0f;  // BUILD 214: the elbow's pole falls back to anatomy
static float g_cfg_arm_pole_min      = 0.06f; // BUILD 214, metres of perpendicular
static float g_cfg_arm_pole_smooth   = 1.0f;  // BUILD 285: the animated elbow side is low-passed and rate-capped (code default ON)
static float g_cfg_torso_turn        = 1.0f;  // BUILD 294: the upper body turns with the head (code default ON)
static float g_cfg_torso_dead        = 45.0f; // BUILD 294, degrees
static float g_cfg_torso_rate        = 240.0f;// BUILD 294, degrees a second
static float g_cfg_torso_hand_w      = 0.35f; // BUILD 294, weight of the right controller bearing
static float g_cfg_torso_max         = 70.0f; // BUILD 294, degrees
static float g_cfg_lean              = 1.0f;  // BUILD 300: head offset with a soft limit (code default ON)
static float g_cfg_lean_max          = 0.40f; // BUILD 300, metres, horizontal
static float g_cfg_lean_soft         = 0.75f; // BUILD 300, fraction of each limit where the easing starts
static float g_cfg_lean_down         = 0.25f; // BUILD 300, metres of drop the view follows (the crouch is the next build)
static float g_cfg_lean_up           = 0.10f; // BUILD 300, metres of rise
static float g_cfg_lean_hold_ms      = 800.0f;// BUILD 300, ms the left menu button is held to recentre; 0 = Start is immediate, no hold
static float g_cfg_lean_hinge        = 1.0f;  // BUILD 300: the torso hinges toward the lean (0 = the view leans alone)
static float g_cfg_lean_theta        = 65.0f; // BUILD 300, degrees, the most the hinge turns
static float g_cfg_lean_slide        = 0.05f; // BUILD 300, metres the upper body may slide beyond the hinge
static float g_cfg_arm_pole_down_l   = 0.5f;  // BUILD 293: the left (support) elbow leans toward down by this fraction while two-handed
static float g_cfg_arm_pole_down_r   = 0.0f;  // BUILD 293: the same for the right elbow (off)
static float g_cfg_arm_pole_tau_ms   = 50.0f; // BUILD 285, low-pass time constant
static float g_cfg_arm_pole_rate     = 540.0f;// BUILD 285, deg/s, the most the elbow side may turn
static float g_cfg_arm_anchor        = 0.0f;  // BUILD 213, CX-025: the shoulder rides the view offset
static float g_cfg_arm_anchor_max    = 0.15f; // BUILD 213, metres (a cfg without the key gets this, not 0.35)
static float g_cfg_follow_dead       = 0.20f; // BUILD 209, metres
static float g_cfg_follow_push       = 0.75f; // BUILD 211, the stick while closing
static float g_cfg_follow_stop       = 0.10f; // BUILD 211, seconds of stopping distance
static float g_cfg_follow_vmax       = 2.0f;  // BUILD 211, m/s above which it coasts
static float g_cfg_follow_bleed      = 0.30f; // BUILD 211, m/s the settle takes out of the view
// BUILD 202: THE HEAD ROLL (CX-022). The content is composed with the camera's
// own orientation (cam_pose_rot = 0), which never rolls, while the layer pose
// the mod submits carries the head's full orientation, roll included; the
// compositor therefore shows a level image at a rolled pose and the horizon
// rides with the head (HEADROLL-2026-09-05-where-the-roll-sign-lives.md,
// sections 2, 3 and 5.1). With this on, the submitted orientation loses its
// twist about the view's own forward axis and keeps the swing (yaw and
// pitch), so the compositor sees the head rolled relative to the pose the
// image was rendered for and counter-rotates the image by the head roll: the
// horizon stays level in the world. Confined to the submit: no camera object,
// no base frame, no weapon, no aim pump. Cost: a rotated finite image shows
// black corners at large roll, as every reprojected port does.
std::atomic<int>      g_head_roll_fix{0};
std::atomic<uint64_t> g_roll_strips{0};
std::atomic<float>    g_roll_last_deg{0.0f};   // the roll removed at the last submit, degrees, signed
static void set_head_roll_fix(int on) {
    camera::set_head_roll_level(on ? 1 : 0);   // BUILD 261
    const int want = on ? 1 : 0;
    static bool s_said = false;
    const int prev = g_head_roll_fix.exchange(want, std::memory_order_relaxed);
    if (!s_said || prev != want) {
        s_said = true;
        LOG_INFO("roll: head roll fix %s (cfg head_roll_fix; build 203, the level pose: %s)", want ? "ON" : "off",
                 want ? "the submitted layer pose carries no roll, so the compositor levels "
                        "the horizon against the head's roll; the content, the camera, the "
                        "weapon and the aim are untouched"
                      : "the submitted pose carries the head's roll and the image is "
                        "head-locked in roll, the state every build before 202 had");
    }
}
static float g_cfg_hand_calib      = 0.0f;
// Build 143: the visible-barrel pitch trim, degrees. 0 is build-142 exactly.
static float g_cfg_wgun_pitch_deg  = 0.0f;
// Build 144: the ray probe, log only, on by default while the aim question
// is open; costs one log line a second.
static float g_cfg_ray_probe       = 1.0f;
// Build 145: the grip-gated hold. 0 is geometry-alone, exactly as before.
static float g_cfg_wgun_two_grip   = 0.0f;
// 2026-08-31: the hide-one-piece slots, FP port. -1 is off. Stepped from the
// tuner to find which array-2 child is which arm, and whether the hand
// survives hiding it, which is the measurement the hands plan turns on.
static float g_cfg_hide_child  = -1.0f;
static float g_cfg_hide_child2 = -1.0f;
static float g_cfg_head_prelatch = 1.0f;   // BUILD 224
static float g_cfg_head_reassert = 1.0f;   // BUILD 224
static float g_cfg_head_always   = 1.0f;   // BUILD 225
static float g_cfg_head_populate = 1.0f;   // BUILD 226
static float g_cfg_fp_auto       = 1.0f;   // BUILD 225
static float g_cfg_head_pulse    = 30.0f;  // BUILD 225, frames per phase; 226: 30 (six did not register)
static float g_cfg_aimpoint_m    = 5.0f;   // build 105
static float g_cfg_wgun_roll     = 1.0f;
static float g_cfg_wgun_roll_deg = 0.0f;
static float g_cfg_wgun_smooth  = 0.25f;   // build 80, rotation filter
static float g_cfg_wgun_maxstep = 5.00f;   // build 80, degrees per call
// Build 99: the grip offsets, all zero so a fresh install behaves as build 98.
static float g_cfg_wgun_grip_fwd = 0.0f;
static float g_cfg_wgun_grip_lat = 0.0f;
static float g_cfg_wgun_grip_up  = 0.0f;
static float g_cfg_wgun_grip_bone = 1.0f;  // BUILD 219: the grip bone, on
static float g_cfg_pip           = 0.0f;   // BUILD 221: the PiP scope; off in code, the cfg turns it on (review 3)
static float g_cfg_pip_zoom      = 2.7f;
static float g_cfg_pip_radius    = 0.016f;
static float g_cfg_pip_lat       = 0.0f;
static float g_cfg_pip_fwd       = 0.30f;
static float g_cfg_pip_up        = 0.04f;
static float g_cfg_pip_block_ads = 0.0f;
static float g_cfg_pip_toggle    = 1.0f;   // BUILD 237: L3 latches the scope
static float g_cfg_scope_native  = 0.0f;   // BUILD 238: off by default, it changes aiming
static float g_cfg_scope_hold    = 6.0f;   // BUILD 238: frames past the gate before it latches
static float g_cfg_wgun_grip_two = 0.0f;
static float g_cfg_wgun_pos        = 0.0f;   // build 81, position rides ctrl
static float g_cfg_wgun_pos_scale  = 1.0f;
static float g_cfg_wgun_pos_clamp  = 1.0f;
static float g_cfg_wgun_pos_smooth = 0.35f;
// Issue #2: on a machine with two GPUs (a CPU iGPU plus a discrete card) the
// game can bind to a different adapter than the headset runtime uses, and
// cross-adapter frame sharing hangs after one frame. Default ON, and ON when
// the key is absent, so users who UPDATE (and keep their old cfg without this
// key) still get the protection. adapter_check=0 forces past it.
static bool g_cfg_adapter_check = true;

// 2026-08-12: the frame-wait watchdog. See the block above on_present() for
// the full reasoning. xr_async_wait moves the blocking xrWaitFrame off the
// game's render thread so a runtime that never returns from it can no longer
// freeze the game. xr_wait_timeout_ms is how long the render thread will wait
// for the runtime before it gives up on VR for that one frame and lets the
// game present normally.
//
// DEFAULT ON, and on when the key is absent. A protection against the game
// freezing is not something a user should have to find in a text file and
// switch on: the people it saves are exactly the people whose game dies
// before they ever get far enough to care about config. xr_async_wait=0
// restores the old inline wait.
static std::atomic<int> g_cfg_async_wait{1};
static std::atomic<int> g_cfg_wait_timeout{100};

// BUILD 228: THE SWAPCHAIN-IMAGE WAIT BOUND (handoff 4.7 item 4).
//
// Both xrWaitSwapchainImage call sites waited XR_INFINITE_DURATION on the
// game's render thread, inside the canvas path that runs for the first time
// on the first frame. Seven runs from one tester on the shipped build 227
// wedge identically: the first Present never returns, so frames sticks at 1,
// xrEndFrame is never reached, the session never leaves READY, and eight to
// nine seconds later a watchdog kills the process. Measured: startup varies
// 18.4 to 21.9 s across runs while session-begun-to-death is 9.0, 8.1 and
// 9.1 s, so the clock starts at the stall, not at launch. xrWaitFrame was
// given a bound and a thread in 2026-08-12; these two never were.
//
// 0 restores the unbounded wait. The default is deliberately generous: a
// 72 Hz frame is 13.9 ms, so 300 ms is twenty frames of slack and still two
// orders below the watchdog that is killing the process today.
static std::atomic<int> g_cfg_img_timeout{300};

// BUILD 229: THE CANVAS CLAMP (handoff 4.7 item 4).
//
// Filled from xrEnumerateViewConfigurationViews at init, which this mod had
// never called. 0 means the enumeration failed or has not run, and the clamp
// then does nothing at all. canvas_clamp = 0 restores the pre-229 canvas.
static uint32_t g_clamp_from_w = 0, g_clamp_from_h = 0;  // pre-clamp canvas
static uint32_t g_rec_w = 0, g_rec_h = 0;   // recommendedImageRectWidth/Height
static uint32_t g_max_w = 0, g_max_h = 0;   // maxImageRectWidth/Height
static std::atomic<int> g_cfg_canvas_clamp{1};

// BUILD 233: UNIVERSAL OPENXR API LAYER SUPPRESSION (handoff 4.7).
//
// Until now the mod killed TWO layers by hardcoded name (OBS Mirror and
// Virtual Desktop's Oculus compatibility shim). That was written against one
// machine, and every machine since has had something else in the chain:
//
//   issue #7  (RTX 4070 SUPER, Quest 3, wedges on VDXR *and* SteamVR)
//             XR_APILAYER_reshade                       survived, 1 run
//   the AMD tester (RX 9060 XT, Quest 3, VDXR)
//             XR_APILAYER_MBUCCHIA_quad_views_foveated  survived, 7 runs
//   the Epic / RTX 5090 tester, which RUNS
//             nothing survived, 0 layers, 10,699 frames
//
// Two unrelated vendors, two unrelated layers, one signature. A hardcoded
// list cannot be made to cover that, and guessing the disable string is worse
// than useless: ReShade's has changed twice historically
// (DISABLE_XR_API_LAYER_reshade_1 -> DISABLE_XR_APILAYER_reshade ->
// DISABLE_XR_APILAYER_reshade_1), so a guessed name silently does nothing.
//
// So stop guessing and read the machine. The loader finds implicit layers
// through SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit under both HKLM and
// HKCU: each VALUE NAME is a manifest path and its DWORD is 0 for enabled,
// non-zero for disabled. Each manifest carries the name of the environment
// variable that disables it, in its own "disable_environment" field. Walk
// them, set every one, and log what was found either way - so the next
// tester's log NAMES the layer instead of us diffing two runs to find it.
//
// xr_layer_suppress = 0 restores the pre-233 behaviour (the two hardcoded
// names only). xr_layer_allow is a comma-separated list of layer names to
// leave alone, for the user who genuinely wants one in the path.
static std::atomic<int> g_cfg_layer_suppress{1};
static char g_cfg_layer_allow[512] = "";

// BUILD 234: if a layer is STILL in the chain after all of the above, do not
// start VR at all. See the block at the bail-out in init() for why this is
// decided there and not by a watchdog later.
static std::atomic<int> g_cfg_layer_bail{1};

// BUILD 235: how long the FIRST VR frame may take before the mod gives up on
// it, stands VR down and lets the game carry on flat. 0 restores the old
// unbounded wait, which is the behaviour that kills the game on startup.
// See the block above on_present() for why 5000 and not more.
static std::atomic<int> g_cfg_firstframe_ms{5000};

// BUILD 239: HOW MANY FRAMES THE GAME MUST HAVE PRESENTED BEFORE VR COMES UP.
//
// Every wedged log on record has the same order: the game enters Present for
// the very first time, and within a second and a half the init thread has
// started building an OpenXR session on that same device. The one machine that
// works had already completed eight presents by then. That ordering is the only
// structural difference the logs show, and it costs nothing to remove: waiting
// for thirty RETURNED presents is half a second on a healthy machine, and on a
// machine whose render thread never comes back it means no session is ever
// created, no swapchains are built on its device, and the mirror is never armed
// into a thread that has already gone. If our bring-up is contributing to the
// wedge, this takes it out of the window; if it is not, the run is unchanged.
// 0 restores the pre-239 behaviour exactly.
static std::atomic<int> g_cfg_init_frames{30};

// Render-thread counters, read once a second by drain_input (rule 8: the wait
// sites do no logging).
static std::atomic<unsigned> g_img_to_copy{0};
static std::atomic<unsigned> g_img_to_blit{0};

// BUILD 228: THE FIRST-FRAME PHASE COUNTER (handoff 4.7 item 1).
//
// D3D11Hook.cpp:106 increments frames on ENTRY to Present, before the VR
// callback runs. So a tester log stuck at frames=1 does not mean one frame
// completed: it means the FIRST frame was entered and never came back. Three
// instruments agree on that and none of them names the call: the heartbeat
// (frames frozen at 1), fovroot ticks (frozen at 3), and perf_log, which is
// silent because its guard pr.n == 0 && rg.n == 0 && cam_n == 0 holds every
// second. This is the missing name.
//
// One relaxed store per step on the render thread, no logging (rule 8). The
// heartbeat reads it and prints it when a second passes with no frame.
enum : int {
    kPhIdle = 0, kPhWaitFrame, kPhBeginFrame, kPhCreateSwapchain,
    kPhEnumImages, kPhAcquireCopy, kPhWaitCopy, kPhAcquireBlit,
    kPhWaitBlit, kPhEndFrame, kPhDone,
    // BUILD 230: the prologue, everything between Present entry and
    // xrBeginFrame. 228 reported "idle" for every second of every wedged
    // run, which means the thread never got as far as kPhBeginFrame.
    kPhEnter, kPhPollEvent, kPhBeginSession, kPhEndSession,
    kPhLocateView, kPhLocateStage, kPhSyncActions, kPhLocateAim,
    kPhLocateGrip, kPhActionState,
    // BUILD 232: the first 37 lines of on_present, which 230 still left
    // unmarked. A block in any of these also reported "idle".
    kPhBody, kPhFfrTick, kPhSamples, kPhCount
};
static std::atomic<int> g_phase{kPhIdle};
static std::atomic<int> g_phase_eye{-1};
inline void ph(int p, int eye = -1) {
    g_phase.store(p, std::memory_order_relaxed);
    g_phase_eye.store(eye, std::memory_order_relaxed);
}

XrDuration img_wait_timeout() {
    const int ms = g_cfg_img_timeout.load(std::memory_order_relaxed);
    return ms <= 0 ? XR_INFINITE_DURATION : (XrDuration)ms * 1000000LL;
}

// Build 10m: the ipd_scale the run started with (cfg value, or 1.0 with no
// cfg). Numpad * resets to this rather than a hardcoded 1.0, so reset means
// "back to my tuned value", not "back to untuned".
float g_ipd_default = 1.0f;

void note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_deferred, sizeof(g_deferred), fmt, ap);
    va_end(ap);
    g_want_log.store(true, std::memory_order_release);
}

// BUILD 119d: a SECOND deferred slot, for one-shot latched probes.
//
// note() is a SINGLE shared buffer drained once per second, so two notes in
// the same second silently lose the first. That is exactly what happened to
// build 119c's depth probe: it fired on the first blitting frame and was
// overwritten by the 11a.2 canvas probe before the 1 Hz drain ran, so the
// line never reached the log and the test produced nothing.
//
// A one-shot probe that reports a fact we cannot get any other way must not
// compete with a per-frame diagnostic for one buffer. This slot is written
// exactly once per process.
char              g_once_note[600] = {};
std::atomic<bool> g_once_want{false};

void note_once(const char* fmt, ...) {
    if (g_once_want.load(std::memory_order_acquire)) return;   // first wins
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_once_note, sizeof(g_once_note), fmt, ap);
    va_end(ap);
    g_once_want.store(true, std::memory_order_release);
}

// BUILD 10c: read the one user knob from GRWVR\grwxr.cfg at init. Missing
// file or key leaves the default. Lines are `key=value`; `#` starts a comment.
void load_config() {
    const std::wstring path = log::data_dir() + L"\\grwxr.cfg";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || !f) {
        LOG_INFO("VR: no grwxr.cfg, ipd_scale = 1.00 (F1 panel, or Numpad * adjusts)");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        float v = 0.0f;
        if (sscanf_s(line, " ipd_scale = %f", &v) == 1) {
            // Build 117: widened to match the hotkey band (0.00..3.00) so a
            // value the tester settles on with Numpad / and * can actually be
            // written back here. Negative is still accepted and still means
            // the eyes are swapped, which is a diagnostic, not a setting.
            if (v < -3.0f) v = -3.0f;
            if (v >  3.0f) v =  3.0f;
            headpose::set_ipd_scale(v);
        }
        // Build 19: engine-vs-geometric direction of the aim angle pair, +1
        // or -1. Not derivable offline; a wrong sign makes the view turn
        // the WRONG WAY on that axis while VR head aim is on. Session 20
        // headset calibration: the engine's yaw runs OPPOSITE our geometric
        // convention, so the shipped grwxr.cfg carries -1.
        if (sscanf_s(line, " aim_yaw_sign = %f", &v) == 1)
            g_aim_sign_yaw = v < 0.0f ? -1.0f : 1.0f;
        if (sscanf_s(line, " aim_pitch_sign = %f", &v) == 1)
            g_aim_sign_pitch = v < 0.0f ? -1.0f : 1.0f;
        // Build 11b: the mono-scope threshold, radians. 0 disables the gate.
        if (sscanf_s(line, " mono_scope_fov = %f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            if (v > 1.5f) v = 1.5f;
            headpose::set_mono_scope_fov(v);
        }
        // Build 11c: first-person demo forward push, meters.
        if (sscanf_s(line, " fp_forward = %f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            if (v > 4.0f) v = 4.0f;
            headpose::set_fp_forward(v);
        }
        // Build 11e: magnified-scope display window, radians. 0 = angle-correct.
        if (sscanf_s(line, " scope_display_fov = %f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            if (v > 1.2f) v = 1.2f;
            g_scope_disp_fov = v;
        }
        // Build 11f: first-person side and up offsets, meters.
        if (sscanf_s(line, " fp_side = %f", &v) == 1) {
            if (v < -2.0f) v = -2.0f;
            if (v >  2.0f) v =  2.0f;
            headpose::set_fp_side(v);
        }
        // Build 15e: anchored first-person eye height above the character
        // origin, meters (world up).
        if (sscanf_s(line, " fp_eye = %f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            if (v > 2.5f) v = 2.5f;
            headpose::set_fp_eye(v);
        }
        // Build 16a: the head-bone anchor, and the head-joint-to-eye trim it
        // uses instead of fp_eye. fp_head_anchor=0 forces the old origin
        // anchor, which is the one-line revert if the head bone misbehaves.
        if (sscanf_s(line, " fp_head_eye = %f", &v) == 1) {
            if (v < -0.5f) v = -0.5f;
            if (v >  1.0f) v =  1.0f;
            headpose::set_fp_head_eye(v);
        }
        if (sscanf_s(line, " fp_head_anchor = %f", &v) == 1)
            headpose::set_fp_head_anchor(v != 0.0f);
        // Build 89: write the camera pose from the `selector` target when
        // on_calc_mvp is not derived. Without it, on the Last Rites binary,
        // nothing composes the camera at all: no first person, no stereo.
        if (sscanf_s(line, " cam_selector_pose = %f", &v) == 1)
            headpose::set_cam_selector_pose(v != 0.0f);
        // Build 90: 0 makes the camera write POSITION ONLY, which is what
        // stops it fighting for a channel it does not own.
        if (sscanf_s(line, " cam_pose_rot = %f", &v) == 1)
            headpose::set_cam_pose_rot(v != 0.0f);
        // 2026-09-01: the horizon lock, which only bites while the line above
        // is 1. Levels the ENGINE's roll out of the base; your head's roll
        // rides in H and is untouched.
        if (sscanf_s(line, " cam_level_horizon = %f", &v) == 1)
            camera::set_cam_level(v >= 0.5f ? 1 : 0);
        // Build 91: the viewpoint placement trio. See HeadPose.h for why each
        // exists and what its default was measured against.
        if (sscanf_s(line, " fp_fwd = %f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            if (v > 0.5f) v = 0.5f;
            headpose::set_fp_fwd(v);
        }
        if (sscanf_s(line, " fp_clamp = %f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            if (v > 2.0f) v = 2.0f;
            headpose::set_fp_clamp(v);
        }
        if (sscanf_s(line, " fp_smooth = %f", &v) == 1) {
            if (v < 0.0f)   v = 0.0f;
            if (v > 500.0f) v = 500.0f;
            headpose::set_fp_smooth(v);
        }
        if (sscanf_s(line, " fp_smooth_z = %f", &v) == 1) {
            if (v < 0.0f)   v = 0.0f;
            if (v > 500.0f) v = 500.0f;
            headpose::set_fp_smooth_z(v);
        }
        // Build 15e.3: anchored lateral centering, meters along the base
        // camera's right axis.
        if (sscanf_s(line, " fp_anchor_side = %f", &v) == 1) {
            if (v < -1.0f) v = -1.0f;
            if (v >  1.0f) v =  1.0f;
            headpose::set_fp_anchor_side(v);
        }
        if (sscanf_s(line, " fp_up = %f", &v) == 1) {
            if (v < -2.0f) v = -2.0f;
            if (v >  2.0f) v =  2.0f;
            headpose::set_fp_up(v);
        }
        // Build 12a: fullscreen render fov, radians. 0 disables the override
        // entirely (the windowed 11f behaviour).
        if (sscanf_s(line, " fullscreen_fov = %f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            if (v > 2.5f) v = 2.5f;
            g_cfg_fs_fov_cfg = v;
            headpose::set_fs_fov(v);
            headpose::set_fs_enabled(v > 0.0f);
        }
        // Build 103: the lower edge of the band that override applies to.
        // 0.60 is the old hardcoded value. Lower it toward mono_scope_fov to
        // pull plain ADS into the override and out of the small-flat-window
        // presentation. See HeadPose.h at set_fs_fov_lo.
        if (sscanf_s(line, " fullscreen_fov_lo = %f", &v) == 1)
            headpose::set_fs_fov_lo(v);
        // FRAMES IN FLIGHT (AFR Rev 4 section 15.8, Luke Ross R.E.A.L. does the
        // same under alternate-frame stereo). 0 leaves the engine's own value
        // untouched, so this key is a no-op until it is set; 1..16 caps the
        // queue depth to tighten AER eye pairing. Read here so the cfg hot
        // reload can arm and disarm it live, without a relaunch.
        if (sscanf_s(line, " max_frame_latency = %f", &v) == 1)
            d3d11::request_max_frame_latency((int)v);
        // Build 12c: desktop recording view crop fov, radians. 0 disables.
        if (sscanf_s(line, " desktop_fov = %f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            if (v > 2.0f) v = 2.0f;
            g_desk_fov = v;
            g_desk_on  = v > 0.0f;
        }
        // Build 14d: controller aim steer. aim_steer 0 disables the whole
        // feature; the rest tune feel. Defaults are conservative.
        if (sscanf_s(line, " aim_steer = %f", &v) == 1) {
            g_steer_on.store(v > 0.0f, std::memory_order_relaxed);
        }
        if (sscanf_s(line, " aim_deadzone_deg = %f", &v) == 1) {
            if (v < 0.0f)  v = 0.0f;
            if (v > 30.0f) v = 30.0f;
            g_steer_deadzone = v;
        }
        if (sscanf_s(line, " aim_gain = %f", &v) == 1) {
            if (v < 0.0f)  v = 0.0f;
            if (v > 50.0f) v = 50.0f;
            g_steer_gain = v;
        }
        if (sscanf_s(line, " aim_max_rate = %f", &v) == 1) {
            if (v < 0.0f)   v = 0.0f;
            if (v > 720.0f) v = 720.0f;
            g_steer_maxrate = v;
        }
        if (sscanf_s(line, " aim_mouse_per_deg = %f", &v) == 1) {
            if (v < 0.0f)   v = 0.0f;
            if (v > 200.0f) v = 200.0f;
            g_steer_mpd = v;
        }
        // Build 14f: right trigger drives the game's ADS. 0 disables.
        if (sscanf_s(line, " aim_ads = %f", &v) == 1) {
            g_ads_on.store(v > 0.0f, std::memory_order_relaxed);
        }
        // Build 14h: full trigger pull fires (left-mouse hold). 0 disables.
        if (sscanf_s(line, " aim_fire = %f", &v) == 1) {
            g_fire_on.store(v > 0.0f, std::memory_order_relaxed);
        }
        // Build 22: Touch-as-gamepad merge. 0 makes the XInputGetState
        // detour a pure pass-through (hot-reloadable, so it doubles as the
        // instant kill switch).
        if (sscanf_s(line, " xinput_touch = %f", &v) == 1)
            xin::set_enabled(v > 0.0f);
        // Build 49: 0 removes stick pitch (right stick Y) from the game
        // entirely, so the head is the only thing that can pitch the view.
        // Flip to 1 for aircraft until the on-foot gate exists.
        if (sscanf_s(line, " stick_pitch = %f", &v) == 1)
            xin::set_stick_pitch(v > 0.0f);
        // Build 23: what the VR aim toggle drives. 0 = head (build 19),
        // 1 = right controller ray (hip-fire, the default).
        if (sscanf_s(line, " aim_source = %f", &v) == 1)
            g_aim_source.store(v >= 0.5f ? 1 : 0, std::memory_order_relaxed);
        if (sscanf_s(line, " aim_on_start = %f", &v) == 1)
            g_aim_on_start.store(v >= 0.5f ? 1 : 0, std::memory_order_relaxed);
        // Build 84: hip-fire barrel aim. Hot-reloadable on purpose, so it can
        // be armed and disarmed inside one session without a relaunch: this is
        // the first build that can move impacts off the head and the tester
        // needs to be able to A/B it against itself.
        if (sscanf_s(line, " aim_barrel = %f", &v) == 1)
            g_aim_barrel.store(v >= 1.5f ? 2 : (v >= 0.5f ? 1 : 0),
                               std::memory_order_relaxed);
        if (sscanf_s(line, " aim_barrel_max = %f", &v) == 1)
            g_aim_barrel_max.store(v > 0.01f && v < 1.5f ? v : 0.60f,
                                   std::memory_order_relaxed);
        if (sscanf_s(line, " hand_markers = %f", &v) == 1)
            g_hand_enable = v > 0.0f;
        // BUILD 227: the release switches.
        if (sscanf_s(line, " wskel_marker = %f", &v) == 1)
            g_wskel_marker = v > 0.0f;
        if (sscanf_s(line, " log_diag = %f", &v) == 1)
            ::grwxr::log::set_diag(v > 0.0f ? 1 : 0);
        if (sscanf_s(line, " palette_probe = %f", &v) == 1)
            g_palette_probe = v > 0.0f;
        // BUILD 241: the one-window frame-anatomy capture (Anatomy.h). The code
        // default is ON; 0 here turns it off for the run.
        if (sscanf_s(line, " anatomy_capture = %f", &v) == 1)
            anatomy::set_enabled(v > 0.0f);
        // BUILD 246: the one-window constant-buffer CONTENT capture (CbDump.h). Code default OFF; 1 turns it
        // on for the run. It installs the anatomy hooks by itself, so anatomy_capture may stay 0.
        if (sscanf_s(line, " cbdump_capture = %f", &v) == 1)
            cbdump::set_enabled(v > 0.0f);
        // BUILD 242: the one-window camera/view census (Census.h). Code default ON.
        if (sscanf_s(line, " census_capture = %f", &v) == 1)
            census::set_enabled(v > 0.0f);
        // BUILD 243: the second-camera experiment (Cam2.h). Code default OFF; 1 turns it on for the run.
        // cam2_ipd is the metres camera 2 sits to the right of camera 0 (default 0.064). Any finite value
        // BUILD 244: a finite value is clamped to [-1.0, 1.0] metres here and again where camera 2 uses it
        // (Cam2.cpp); anything non-finite is ignored. The value only places camera 2 along camera 0's right
        // axis, but an extreme one puts the camera far from the player and nobody has tested what the
        // engine does with that, so it is bounded rather than assumed harmless.
        if (sscanf_s(line, " cam2_experiment = %f", &v) == 1)
            cam2::set_enabled(v > 0.0f);
        if (sscanf_s(line, " cam2_ipd = %f", &v) == 1 && std::isfinite(v))
            cam2::set_ipd(v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v));
        // BUILD 245: camera 2 is created only after live gameplay (wgun ARMED, see Cam2.cpp gate_eval) has held
        // for this many seconds, continuously. Default 45; clamped to [1, 3600] in Cam2.cpp.
        if (sscanf_s(line, " cam2_arm_seconds = %f", &v) == 1 && std::isfinite(v))
            cam2::set_arm_seconds(v);
        // BUILD 245: 1 clears bit 4 of the byte at renderer+0x8C0 at each builder entry, only while camera 2 is
        // RUNNING (the ProjShadow passes are then not appended). Default 0.
        if (sscanf_s(line, " cam2_noprojshadow = %f", &v) == 1)
            cam2::set_noprojshadow(v > 0.0f);
        // BUILD 261: the vehicle camera. Default 1 (the intended state); 0 is the rollback and applies at once.
        if (sscanf_s(line, " veh_camera = %f", &v) == 1)
            camera::set_veh_camera(v > 0.0f ? 1 : 0);
        // BUILD 262: the alpha of the body-heading tracker (0 = raw heading, build 261; default 0.40).
        if (sscanf_s(line, " veh_smooth = %f", &v) == 1 && std::isfinite(v))
            camera::set_veh_smooth(v);
        // BUILD 260: the log-only vehicle instrument (the veh: line, CameraProbe.h VehSample). Default 1; it changes
        // no behaviour. 0 silences the line.
        if (sscanf_s(line, " veh_probe = %f", &v) == 1)
            g_veh_probe.store(v > 0.0f ? 1 : 0, std::memory_order_relaxed);
        // BUILD 259: 1 (the default) skips the game's ProjShadow pass body whenever the projector-shadow atlas
        // (renderer+0xB60) is NULL, which is the vehicle crash's null compare (PassGuard.h). 0 passes straight through.
        if (sscanf_s(line, " projshadow_guard = %f", &v) == 1)
            passguard::set_enabled(v > 0.0f);
        // BUILD 247: TRUE STEREO. The two engine cameras become the two eyes (Cam2.h). Code default OFF. Needs
        // cam2_experiment = 1 as well (camera 2 must exist); cam2_noprojshadow = 1 is the F-255 condition.
        //   cam2_stereo_swap_eyes: 1 sends the left half to eye 1 and the right half to eye 0 (wrong-eye-order A/B).
        //   cam2_stereo_aspect: 0 = each eye's content aspect is its half viewport's own (the default and the
        //     expected value); a value in [0.2, 5] overrides it, to isolate a squeezed or stretched picture.
        if (sscanf_s(line, " cam2_stereo = %f", &v) == 1)
            cam2::set_stereo(v > 0.0f);
        if (sscanf_s(line, " cam2_stereo_swap_eyes = %f", &v) == 1)
            cam2::set_stereo_swap_eyes(v > 0.0f);
        if (sscanf_s(line, " cam2_stereo_aspect = %f", &v) == 1)
            cam2::set_stereo_aspect(v);
        //   cam2_stereo_mirror_fov (default 1): camera 0's vertical fov field is copied into camera 2 each frame,
        //     so an ADS or sprint zoom reaches both eyes. 0 leaves camera 2's field alone (the A/B for a one-eye zoom).
        if (sscanf_s(line, " cam2_stereo_mirror_fov = %f", &v) == 1)
            cam2::set_stereo_mirror_fov(v > 0.0f);
        // BUILD 248: the eye recorder (EyeRec.h). Code default OFF; it records only trusted stereo frames, so never a menu or
        // a loading screen. eyerec_width = one eye's width in the recording, eyerec_every = every Nth frame,
        // eyerec_max_mb = hard disk cap. Files: eyerec-<pid> folder in GRWVR, f<frame>.jpg and frames.csv. A diagnostic: it never ships.
        // BUILD 251: layer_pose_lag, whole presents (see g_cfg_layer_lag). Default 3, the measured depth of this pipeline; 0 = the
        // tag's orientation as every build before 251. Hot reload.
        if (sscanf_s(line, " layer_pose_lag = %f", &v) == 1 && std::isfinite(v))
            g_cfg_layer_lag.store(v < 0.0f ? 0 : (v > 14.0f ? 14 : (int)v), std::memory_order_relaxed);
        // BUILD 266: the same, while the vehicle compose is on. Measured 2 on the vehicle recording; 0 = the tag's orientation.
        // BUILD 267: the recompute hook. mvp_observe counts and compares (default 1), mvp_drive composes in the hook while a vehicle is
        // detected (default 1; 0 = the 262-266 selector path), veh_build_depth is the Present offset of the label's build (default 2 since build 271; it was 1 in builds 267 to 270).
        if (sscanf_s(line, " mvp_observe = %f", &v) == 1) camera::set_mvp_observe(v > 0.0f ? 1 : 0);
        if (sscanf_s(line, " mvp_drive = %f", &v) == 1) camera::set_mvp_drive(v > 0.0f ? 1 : 0);
        if (sscanf_s(line, " veh_build_depth = %f", &v) == 1 && std::isfinite(v))
            g_cfg_veh_depth.store(v < 0.0f ? 0 : (v > 8.0f ? 8 : (int)v), std::memory_order_relaxed);
        if (sscanf_s(line, " veh_layer_lag = %f", &v) == 1 && std::isfinite(v))
            g_cfg_veh_lag.store(v < 0.0f ? 0 : (v > 14.0f ? 14 : (int)v), std::memory_order_relaxed);
        // BUILD 270: gtrec, the GROUND-TRUTH STEREO RECORDER (GtRec.h). A diagnostic, LOG ONLY: it reads and records, it
        // writes nothing of the engine's and changes no pose, label or picture. Code default OFF. Hot reload.
        //   gtrec          0 = off (nothing allocated, no file, every hook pays one atomic load); 1 = record. It goes live
        //                  once true stereo is live and (with gtrec_cb) the D3D11 hook set exists, then records every
        //                  recompute entry of camera 0 and 2 (every call site), every mod write of a camera pose, every
        //                  graph-builder entry, every write of the 1008-byte per-view constant buffer and every Present to
        //                  GRWVR\gtrec-<pid>.bin (about 1 MB/s; tools\gtrec_analyze.py reads it). Setting it back to 0
        //                  stops the recording and closes the file. One recording per run.
        //   gtrec_cb       1 (default) = also tap the per-view constant buffer (needed to resolve which recompute drew each
        //                  eye; it installs the Anatomy hook set by itself, 20 s after the first stereo layer); 0 = not.
        //   gtrec_max_mb   the file cap in MB, default 1024, clamped to [16, 20000]; at the cap the recorder stops.
        //   gtrec_seconds  0 (default) = no time limit; else stop this many seconds after going live, clamped to [0, 86400].
        if (sscanf_s(line, " gtrec = %f", &v) == 1)
            gtrec::set_enabled(v > 0.0f);
        if (sscanf_s(line, " gtrec_cb = %f", &v) == 1) {
            gtrec::set_cb(v > 0.0f);
            g_gt_cfg[15] = v > 0.0f ? 1.0f : 0.0f;
        }
        if (sscanf_s(line, " gtrec_max_mb = %f", &v) == 1 && std::isfinite(v))
            gtrec::set_max_mb(v < 16.0f ? 16 : (v > 20000.0f ? 20000 : (int)v));
        if (sscanf_s(line, " gtrec_seconds = %f", &v) == 1 && std::isfinite(v))
            gtrec::set_seconds(v < 0.0f ? 0 : (v > 86400.0f ? 86400 : (int)v));
        // BUILD 270: the keys with no getter, read again here for gtrec's file header only (log only, no setter called).
        if (sscanf_s(line, " mvp_drive = %f", &v) == 1)   g_gt_cfg[7] = v > 0.0f ? 1.0f : 0.0f;
        if (sscanf_s(line, " mvp_observe = %f", &v) == 1) g_gt_cfg[8] = v > 0.0f ? 1.0f : 0.0f;
        if (sscanf_s(line, " cam2_ipd = %f", &v) == 1 && std::isfinite(v)) g_gt_cfg[12] = v;
        if (sscanf_s(line, " veh_camera = %f", &v) == 1)  g_gt_cfg[14] = v > 0.0f ? 1.0f : 0.0f;
        if (sscanf_s(line, " eyerec = %f", &v) == 1)
            eyerec::set_enabled(v > 0.0f);
        if (sscanf_s(line, " eyerec_width = %f", &v) == 1 && std::isfinite(v))
            eyerec::set_width((int)v);
        if (sscanf_s(line, " eyerec_every = %f", &v) == 1 && std::isfinite(v))
            eyerec::set_every((int)v);
        if (sscanf_s(line, " eyerec_max_mb = %f", &v) == 1 && std::isfinite(v))
            eyerec::set_max_mb((int)v);
        // Build 45: weapon-candidate markers. 0 hides them (hot-reload).
        if (sscanf_s(line, " wp_markers = %f", &v) == 1)
            g_wpm_enable = v > 0.0f;
        // Build 153: the evidence reticle. 0 off, 1 barrel only, 2 barrel and
        // gaze. Hot-reloads like the markers, so it can be turned on between
        // takes without leaving the headset.
        if (sscanf_s(line, " barrel_reticle = %f", &v) == 1)
            g_ret_mode = (v >= 1.5f) ? 2 : (v >= 0.5f ? 1 : 0);
        // Metres. Clamped rather than trusted: a zero or a negative would put
        // the mark at or behind the camera and it would simply vanish, which
        // reads as "the build is broken" rather than "the value is silly".
        if (sscanf_s(line, " barrel_reticle_dist = %f", &v) == 1)
            g_ret_dist = (v >= 1.0f && v <= 500.0f) ? v : 20.0f;
        // Half-tangent. 0.010 is about 1.1 degrees across, which is a rifle
        // red dot rather than a dinner plate.
        if (sscanf_s(line, " barrel_reticle_size = %f", &v) == 1)
            g_ret_size = (v >= 0.002f && v <= 0.20f) ? v : 0.010f;
        // Build 64: weapon-skeleton identifier. Arms the wskel census in
        // CameraProbe and rides the WHITE marker slot on the picked
        // instance. Read-only; defaults off.
        if (sscanf_s(line, " wskel = %f", &v) == 1) {
            g_wskel_enable = v > 0.0f;
            camera::set_wskel(v > 0.0f);
        }
        // Build 65: the weapon-skeleton write test. 1 arms per-frame
        // +0.30 m height writes on the DRAWN gun's skeleton origin
        // (auto-capped; re-toggle to reset). Needs wskel=1 for the pick.
        if (sscanf_s(line, " wskel_write = %f", &v) == 1)
            camera::set_wskel_write((int)v);
        if (sscanf_s(line, " wgun = %f", &v) == 1)
            g_cfg_wgun = v;
        if (sscanf_s(line, " adapter_check = %f", &v) == 1)
            g_cfg_adapter_check = v > 0.5f;
        if (sscanf_s(line, " xr_async_wait = %f", &v) == 1)
            g_cfg_async_wait.store(v > 0.5f ? 1 : 0, std::memory_order_relaxed);
        if (sscanf_s(line, " xr_wait_timeout_ms = %f", &v) == 1) {
            int ms = (int)v;
            if (ms < 5)    ms = 5;      // below a frame is pointless
            if (ms > 5000) ms = 5000;   // above this the game is frozen anyway
            g_cfg_wait_timeout.store(ms, std::memory_order_relaxed);
        }
        if (sscanf_s(line, " xr_img_timeout_ms = %f", &v) == 1) {
            int ms = (int)v;
            if (ms < 0)    ms = 0;      // 0 = wait forever, the pre-228 behaviour
            if (ms > 5000) ms = 5000;   // above this the game is frozen anyway
            g_cfg_img_timeout.store(ms, std::memory_order_relaxed);
        }
        if (sscanf_s(line, " canvas_clamp = %f", &v) == 1)
            g_cfg_canvas_clamp.store(v > 0.5f ? 1 : 0, std::memory_order_relaxed);
        if (sscanf_s(line, " xr_layer_suppress = %f", &v) == 1)
            g_cfg_layer_suppress.store(v > 0.5f ? 1 : 0, std::memory_order_relaxed);
        if (sscanf_s(line, " xr_layer_bail = %f", &v) == 1)
            g_cfg_layer_bail.store(v > 0.5f ? 1 : 0, std::memory_order_relaxed);
        if (sscanf_s(line, " xr_firstframe_ms = %f", &v) == 1) {
            int ms = (int)v;
            if (ms < 0)     ms = 0;       // 0 = the old unbounded wait
            if (ms && ms < 500)  ms = 500;    // below this a slow load would trip it
            if (ms > 30000) ms = 30000;
            g_cfg_firstframe_ms.store(ms, std::memory_order_relaxed);
        }
        if (sscanf_s(line, " xr_init_frames = %f", &v) == 1) {
            int n = (int)v;
            if (n < 0)    n = 0;          // 0 = come up on the first Present, pre-239
            if (n > 3000) n = 3000;       // a minute of frames is the whole point gone
            g_cfg_init_frames.store(n, std::memory_order_relaxed);
        }
        // BUILD 233: the one string key in this file. It is written here by
        // whichever thread reloads the cfg and read exactly once, by
        // suppress_api_layers() on the init thread, immediately after the
        // load_config() call at the top of init(). Nothing reads it again.
        {
            char buf[512] = {0};
            if (sscanf_s(line, " xr_layer_allow = %511[^\r\n]", buf,
                         (unsigned)sizeof(buf)) == 1) {
                // trim the trailing run of spaces sscanf leaves on the tail
                size_t e = strlen(buf);
                while (e && (buf[e - 1] == ' ' || buf[e - 1] == '\t')) buf[--e] = 0;
                strncpy_s(g_cfg_layer_allow, buf, _TRUNCATE);
            }
        }
        if (sscanf_s(line, " wbaxis = %f", &v) == 1)
            g_cfg_wbaxis = v;
        if (sscanf_s(line, " wgun_smooth = %f", &v) == 1)
            g_cfg_wgun_smooth = v;
        if (sscanf_s(line, " wgun_maxstep_deg = %f", &v) == 1)
            g_cfg_wgun_maxstep = v;
        if (sscanf_s(line, " wgun_twohand = %f", &v) == 1)
            g_cfg_wgun_twohand = v;
        if (sscanf_s(line, " wgun_two_agree = %f", &v) == 1)
            g_cfg_wgun_two_agree = v;
        if (sscanf_s(line, " wgun_two_attach = %f", &v) == 1)
            g_cfg_wgun_two_attach = v;
        if (sscanf_s(line, " wgun_two_auto = %f", &v) == 1)
            g_cfg_wgun_two_auto = v;
        if (sscanf_s(line, " wgun_hands = %f", &v) == 1)
            g_cfg_wgun_hands = v;
        if (sscanf_s(line, " wgun_hands_grab = %f", &v) == 1)
            g_cfg_wgun_hands_grab = v;
        if (sscanf_s(line, " wgun_hand_free = %f", &v) == 1)
            g_cfg_wgun_hand_free = v;
        if (sscanf_s(line, " wgun_arms = %f", &v) == 1)
            g_cfg_wgun_arms = v;
        if (sscanf_s(line, " wgun_wrist = %f", &v) == 1)
            g_cfg_wgun_wrist = v;
        if (sscanf_s(line, " wgun_wrist_abs = %f", &v) == 1)
            g_cfg_wgun_wrist_abs = v;
        if (sscanf_s(line, " wgun_twist = %f", &v) == 1)
            g_cfg_wgun_twist = v;
        if (sscanf_s(line, " wgun_twist_share = %f", &v) == 1)
            g_cfg_wgun_twist_share = v;
        if (sscanf_s(line, " wgun_twist_arm = %f", &v) == 1)
            g_cfg_wgun_twist_arm = v;
        if (sscanf_s(line, " wgun_wristfirst = %f", &v) == 1)
            g_cfg_wgun_wristfirst = v;
        if (sscanf_s(line, " wgun_wrist_limit = %f", &v) == 1)
            g_cfg_wgun_wrist_limit = v;
        if (sscanf_s(line, " wgun_shoulder_max = %f", &v) == 1)
            g_cfg_wgun_shoulder_max = v;
        if (sscanf_s(line, " wgun_clav_max = %f", &v) == 1)
            g_cfg_wgun_clav_max = v;
        if (sscanf_s(line, " wgun_mirror = %f", &v) == 1)
            g_cfg_wgun_mirror = v;
        if (sscanf_s(line, " wroll_probe = %f", &v) == 1)
            g_cfg_wroll_probe = v;
        if (sscanf_s(line, " wgun_roll_unflip = %f", &v) == 1)
            g_cfg_wgun_roll_unflip = v;
        if (sscanf_s(line, " wgun_own = %f", &v) == 1)
            g_cfg_wgun_own = v;
        if (sscanf_s(line, " fov_root = %f", &v) == 1)
            g_cfg_fov_root = v;
        if (sscanf_s(line, " fov_root_mode = %f", &v) == 1)
            g_cfg_fov_root_mode = v;
        if (sscanf_s(line, " fov_root_aimk = %f", &v) == 1)
            g_cfg_fov_root_aimk = v;
        if (sscanf_s(line, " fov_src = %f", &v) == 1)
            g_cfg_fov_src = v;
        if (sscanf_s(line, " fov_scene = %f", &v) == 1)
            g_cfg_fov_scene = v;
        if (sscanf_s(line, " fov_scene_ads = %f", &v) == 1)
            g_cfg_fov_scene_ads = v;
        if (sscanf_s(line, " ffr = %f", &v) == 1)                // BUILD 216
            g_cfg_ffr = v;
        if (sscanf_s(line, " ffr_inner = %f", &v) == 1)
            g_cfg_ffr_inner = v;
        if (sscanf_s(line, " ffr_mid = %f", &v) == 1)
            g_cfg_ffr_mid = v;
        if (sscanf_s(line, " ffr_outer = %f", &v) == 1)
            g_cfg_ffr_outer = v;
        if (sscanf_s(line, " ffr_horizontal = %f", &v) == 1)
            g_cfg_ffr_horizontal = v;
        if (sscanf_s(line, " ffr_cull = %f", &v) == 1)
            g_cfg_ffr_cull = v;
        if (sscanf_s(line, " ffr_margin = %f", &v) == 1)
            g_cfg_ffr_margin = v;
        if (sscanf_s(line, " sprint_probe = %f", &v) == 1)
            g_cfg_sprint_probe = v;
        if (sscanf_s(line, " wgun_lead = %f", &v) == 1)
            g_cfg_wgun_lead = v;
        if (sscanf_s(line, " gpair = %f", &v) == 1)
            g_cfg_gpair = v;
        if (sscanf_s(line, " wgun_drawcam = %f", &v) == 1)
            g_cfg_wgun_drawcam = v;
        if (sscanf_s(line, " wgun_pull = %f", &v) == 1)
            g_cfg_wgun_pull = v;
        if (sscanf_s(line, " wgun_pull_k = %f", &v) == 1)
            g_cfg_wgun_pull_k = v;
        if (sscanf_s(line, " wgun_pull_max = %f", &v) == 1)
            g_cfg_wgun_pull_max = v;
        if (sscanf_s(line, " head_roll_fix = %f", &v) == 1)
            g_cfg_head_roll_fix = v;
        if (sscanf_s(line, " head_pos = %f", &v) == 1)
            g_cfg_head_pos = v;
        if (sscanf_s(line, " head_pos_max = %f", &v) == 1)
            g_cfg_head_pos_max = v;
        if (sscanf_s(line, " stance = %f", &v) == 1)
            g_cfg_stance = v;
        if (sscanf_s(line, " stance_height = %f", &v) == 1)
            g_cfg_stance_height = v;
        if (sscanf_s(line, " stance_crouch = %f", &v) == 1)
            g_cfg_stance_crouch = v;
        if (sscanf_s(line, " stance_prone = %f", &v) == 1)
            g_cfg_stance_prone = v;
        if (sscanf_s(line, " stance_calib = %f", &v) == 1)
            g_cfg_stance_calib = v;
        if (sscanf_s(line, " crouch_drop = %f", &v) == 1)   // BUILD 301
            g_cfg_crouch_drop = v;
        if (sscanf_s(line, " crouch_rise = %f", &v) == 1)   // BUILD 301
            g_cfg_crouch_rise = v;
        if (sscanf_s(line, " follow = %f", &v) == 1)
            g_cfg_follow = v;
        if (sscanf_s(line, " follow_dead = %f", &v) == 1)
            g_cfg_follow_dead = v;
        if (sscanf_s(line, " arm_anchor = %f", &v) == 1)
            g_cfg_arm_anchor = v;
        if (sscanf_s(line, " arm_pole = %f", &v) == 1)
            g_cfg_arm_pole = v;
        if (sscanf_s(line, " arm_pole_min = %f", &v) == 1)
            g_cfg_arm_pole_min = v;
        if (sscanf_s(line, " aim_pitch_abs = %f", &v) == 1)
            g_cfg_aim_pitch_abs = v;
        if (sscanf_s(line, " torso_turn = %f", &v) == 1)
            g_cfg_torso_turn = v;
        if (sscanf_s(line, " torso_dead = %f", &v) == 1)
            g_cfg_torso_dead = v;
        if (sscanf_s(line, " torso_rate = %f", &v) == 1)
            g_cfg_torso_rate = v;
        if (sscanf_s(line, " torso_hand_w = %f", &v) == 1)
            g_cfg_torso_hand_w = v;
        if (sscanf_s(line, " torso_max = %f", &v) == 1)
            g_cfg_torso_max = v;
        if (sscanf_s(line, " lean = %f", &v) == 1)
            g_cfg_lean = v;
        if (sscanf_s(line, " lean_max = %f", &v) == 1)
            g_cfg_lean_max = v;
        if (sscanf_s(line, " lean_soft = %f", &v) == 1)
            g_cfg_lean_soft = v;
        if (sscanf_s(line, " lean_down = %f", &v) == 1)
            g_cfg_lean_down = v;
        if (sscanf_s(line, " lean_up = %f", &v) == 1)
            g_cfg_lean_up = v;
        if (sscanf_s(line, " lean_hold_ms = %f", &v) == 1)
            g_cfg_lean_hold_ms = v;
        if (sscanf_s(line, " lean_hinge = %f", &v) == 1)
            g_cfg_lean_hinge = v;
        if (sscanf_s(line, " lean_theta = %f", &v) == 1)
            g_cfg_lean_theta = v;
        if (sscanf_s(line, " lean_slide = %f", &v) == 1)
            g_cfg_lean_slide = v;
        if (sscanf_s(line, " arm_pole_down_l = %f", &v) == 1)
            g_cfg_arm_pole_down_l = v;
        if (sscanf_s(line, " arm_pole_down_r = %f", &v) == 1)
            g_cfg_arm_pole_down_r = v;
        if (sscanf_s(line, " arm_pole_smooth = %f", &v) == 1)
            g_cfg_arm_pole_smooth = v;
        if (sscanf_s(line, " arm_pole_tau_ms = %f", &v) == 1)
            g_cfg_arm_pole_tau_ms = v;
        if (sscanf_s(line, " arm_pole_rate = %f", &v) == 1)
            g_cfg_arm_pole_rate = v;
        if (sscanf_s(line, " arm_anchor_max = %f", &v) == 1)
            g_cfg_arm_anchor_max = v;
        if (sscanf_s(line, " follow_push = %f", &v) == 1)
            g_cfg_follow_push = v;
        if (sscanf_s(line, " follow_stop = %f", &v) == 1)
            g_cfg_follow_stop = v;
        if (sscanf_s(line, " follow_vmax = %f", &v) == 1)
            g_cfg_follow_vmax = v;
        if (sscanf_s(line, " follow_bleed = %f", &v) == 1)
            g_cfg_follow_bleed = v;
        if (sscanf_s(line, " wgun_pitch_deg = %f", &v) == 1)
            g_cfg_wgun_pitch_deg = v;
        if (sscanf_s(line, " ray_probe = %f", &v) == 1)
            g_cfg_ray_probe = v;
        if (sscanf_s(line, " wgun_two_grip = %f", &v) == 1)
            g_cfg_wgun_two_grip = v;
        if (sscanf_s(line, " hand_calib = %f", &v) == 1)
            g_cfg_hand_calib = v;
        if (sscanf_s(line, " wgun_hide_arms = %f", &v) == 1)
            g_cfg_wgun_hide_arms = v;
        if (sscanf_s(line, " wgun_two_anchor = %f", &v) == 1)
            g_cfg_wgun_two_anchor = v;
        // The two keys cannot alias: the literal " = " in each format string
        // refuses "hide_child2" to the shorter pattern at the '2'.
        if (sscanf_s(line, " hide_child = %f", &v) == 1)
            g_cfg_hide_child = v;
        if (sscanf_s(line, " hide_child2 = %f", &v) == 1)
            g_cfg_hide_child2 = v;
        if (sscanf_s(line, " head_prelatch = %f", &v) == 1)
            g_cfg_head_prelatch = v;
        if (sscanf_s(line, " head_reassert = %f", &v) == 1)
            g_cfg_head_reassert = v;
        if (sscanf_s(line, " head_always = %f", &v) == 1)
            g_cfg_head_always = v;
        if (sscanf_s(line, " head_populate = %f", &v) == 1)
            g_cfg_head_populate = v;
        if (sscanf_s(line, " fp_auto = %f", &v) == 1)
            g_cfg_fp_auto = v;
        if (sscanf_s(line, " head_pulse = %f", &v) == 1)
            g_cfg_head_pulse = v;
        if (sscanf_s(line, " wgun_grip_bypass = %f", &v) == 1) {
            g_cfg_wgun_grip_bypass = v;
            camera::set_wgun_grip_bypass(v >= 0.5f ? 1 : 0);
        }
        if (sscanf_s(line, " ipd_swap = %f", &v) == 1) {
            g_cfg_ipd_swap = v;
            camera::set_ipd_sign(v >= 0.5f ? -1 : 1);
        }
        if (sscanf_s(line, " main_hand = %f", &v) == 1) {
            g_cfg_main_hand = v;
            g_main_hand.store(v >= 0.5f ? 1 : 0, std::memory_order_relaxed);
        }
        if (sscanf_s(line, " wgun_roll = %f", &v) == 1)
            g_cfg_wgun_roll = v;
        if (sscanf_s(line, " wgun_roll_deg = %f", &v) == 1)
            g_cfg_wgun_roll_deg = v;
        if (sscanf_s(line, " wgun_pos = %f", &v) == 1)
            g_cfg_wgun_pos = v;
        if (sscanf_s(line, " wgun_pos_scale = %f", &v) == 1)
            g_cfg_wgun_pos_scale = v;
        if (sscanf_s(line, " wgun_pos_clamp = %f", &v) == 1)
            g_cfg_wgun_pos_clamp = v;
        if (sscanf_s(line, " wgun_pos_smooth = %f", &v) == 1)
            g_cfg_wgun_pos_smooth = v;
        if (sscanf_s(line, " wgun_dz = %f", &v) == 1)
            g_cfg_wgun_dz = v;
        // Build 99: the grip. All default 0, which is the build 98 behaviour.
        if (sscanf_s(line, " wgun_grip_fwd = %f", &v) == 1)
            g_cfg_wgun_grip_fwd = v;
        if (sscanf_s(line, " wgun_grip_lat = %f", &v) == 1)
            g_cfg_wgun_grip_lat = v;
        if (sscanf_s(line, " wgun_grip_up = %f", &v) == 1)
            g_cfg_wgun_grip_up = v;
        if (sscanf_s(line, " wgun_grip_two = %f", &v) == 1)
            g_cfg_wgun_grip_two = v;
        if (sscanf_s(line, " wgun_grip_bone = %f", &v) == 1)   // BUILD 219
            g_cfg_wgun_grip_bone = v;
        // BUILD 221: the PiP scope.
        if (sscanf_s(line, " pip = %f", &v) == 1)            g_cfg_pip = v;
        if (sscanf_s(line, " pip_zoom = %f", &v) == 1)       g_cfg_pip_zoom = v;
        if (sscanf_s(line, " pip_radius = %f", &v) == 1)     g_cfg_pip_radius = v;
        if (sscanf_s(line, " pip_lat = %f", &v) == 1)        g_cfg_pip_lat = v;
        if (sscanf_s(line, " pip_fwd = %f", &v) == 1)        g_cfg_pip_fwd = v;
        if (sscanf_s(line, " pip_up = %f", &v) == 1)         g_cfg_pip_up = v;
        if (sscanf_s(line, " pip_block_ads = %f", &v) == 1)  g_cfg_pip_block_ads = v;
        if (sscanf_s(line, " pip_toggle = %f", &v) == 1)     g_cfg_pip_toggle = v;   // BUILD 237
        // BUILD 238: native VR ADS.
        if (sscanf_s(line, " scope_native = %f", &v) == 1)      g_cfg_scope_native = v;
        if (sscanf_s(line, " scope_hold_frames = %f", &v) == 1) g_cfg_scope_hold = v;
        if (sscanf_s(line, " wnode = %f", &v) == 1)
            g_cfg_wnode = v;
        if (sscanf_s(line, " wnode_dz = %f", &v) == 1)
            g_cfg_wnode_dz = v;
        // Build 70: the SetWorldTransform census. Log only, and independent of
        // wnode, so it is run with the substitution OFF.
        if (sscanf_s(line, " wnode_census = %f", &v) == 1)
            camera::set_wnode_census(v != 0.0f ? 1 : 0);
        if (sscanf_s(line, " wnode_radius = %f", &v) == 1)
            camera::set_wnode_radius(v);
        if (sscanf_s(line, " wnode_axis = %f", &v) == 1)
            camera::set_wnode_axis((int)v);
        // Build 47: the placement write test. Defaults keep it OFF.
        if (sscanf_s(line, " wp_write_slot = %f", &v) == 1) {
            int s = (int)v;
            g_wpw_slot = (s >= 0 && s < kWpm) ? s : -1;
        }
        if (sscanf_s(line, " wp_write_mode = %f", &v) == 1)
            g_wpw_mode = (v >= 1.5f) ? 2 : 1;
        // Build 48: hex handle, read off the wpm:/wpw: log lines. 0 = off.
        {
            unsigned h = 0;
            if (sscanf_s(line, " wp_write_handle = %x", &h) == 1)
                g_wpw_handle = h & 0xFFFFFF;
        }
        if (sscanf_s(line, " wp_write_up = %f", &v) == 1) {
            if (v < -1.0f) v = -1.0f;
            if (v >  1.0f) v =  1.0f;
            g_wpw_up = v;
        }
        if (sscanf_s(line, " aim_ctrl_smooth = %f", &v) == 1) {
            if (v < 0.05f) v = 0.05f;
            if (v > 1.0f)  v = 1.0f;
            g_ctl_smooth = v;
        }
        // Build 24: the controller-aim reticle dot. 0 hides it.
        //
        // NOTE, 2026-09-01: this key is `aim_reticle` and belongs to the
        // RETIRED controller-aim experiment (aim_source = 1), so it draws
        // nothing while aim_source is 0. Build 153's evidence rings are a
        // DIFFERENT feature on `barrel_reticle`. The names were one letter
        // apart from colliding and two features on one key is exactly the
        // config sprawl the owner has complained about. Do not merge them.
        if (sscanf_s(line, " aim_reticle = %f", &v) == 1)
            g_dot_enable = v > 0.0f;
        // Build 50: print the aim-reader census (AimTrace.h). Log-only either
        // way; this key controls the log line, not the recording, so arming it
        // mid-session shows the whole run rather than starting from zero.
        if (sscanf_s(line, " aim_trace = %f", &v) == 1)
            aimtrace::set_logging(v > 0.0f);
        // Build 51: the per-shot aim override, in DEGREES, applied only to
        // what the shot reader sees. 0 disarms. Clamped hard: this is a test
        // lever, and a large angle would just send rounds into the scenery.
        if (sscanf_s(line, " aim_shot_yaw_deg = %f", &v) == 1) {
            if (v < -45.0f) v = -45.0f;
            if (v >  45.0f) v =  45.0f;
            g_shot_yaw_deg = v;
            aimtrace::set_shot_offset(g_shot_yaw_deg * 0.01745329f,
                                      g_shot_pitch_deg * 0.01745329f);
        }
        // Alternate the sign every round: the readable test with no crosshair.
        if (sscanf_s(line, " aim_shot_alternate = %f", &v) == 1)
            aimtrace::set_shot_alternate(v > 0.0f);
        // Build 56: rotate the BULLET's own direction at spawn. Degrees.
        if (sscanf_s(line, " bullet_yaw_deg = %f", &v) == 1) {
            if (v < -90.0f) v = -90.0f;
            if (v >  90.0f) v =  90.0f;
            aimtrace::set_bullet_yaw(v);
        }
        // Build 62: bullets follow the controller ray. 0 off, 1 on.
        // Build 107: spoken test instructions. 0 silences them.
        if (sscanf_s(line, " voice = %f", &v) == 1)
            voice::set_enabled(v > 0.0f);
        // 2026-09-01: whether the guided anchor test announces itself. Default
        // 0: build 137 answered the question it was built for, and the prompt
        // was still inviting the tester into it 17 seconds into every session.
        if (sscanf_s(line, " voice_guide = %f", &v) == 1)
            aimtrace::set_voice_guide(v >= 0.5f ? 1 : 0);
        // 2026-09-01: the arm scale. F3's capture sets this live; the line
        // here is how a captured value is made permanent without recalibrating
        // every session. 1.00 is identity.
        if (sscanf_s(line, " hand_scale = %f", &v) == 1)
            camera::set_hand_scale(v);
        // Build 105: the aiming-point A/B. See AimTrace.h.
        if (sscanf_s(line, " aimpoint_ab_m = %f", &v) == 1)
            g_cfg_aimpoint_m = v;
        if (sscanf_s(line, " aimpoint_ab = %f", &v) == 1)
            aimtrace::set_aimpoint_ab(v > 0.0f, g_cfg_aimpoint_m);
        if (sscanf_s(line, " bullet_ctrl = %f", &v) == 1)
            aimtrace::set_bullet_ctrl(v > 0.0f);
        // BUILD 137: the shot direction follows the barrel. See AimTrace.h.
        if (sscanf_s(line, " shot_dir = %f", &v) == 1)
            aimtrace::set_shot_dir(v > 0.0f);
        // BUILD 152: the origin write, the second half of the hard baseline.
        if (sscanf_s(line, " shot_org = %f", &v) == 1)
            aimtrace::set_shot_org(v > 0.0f);
        // BUILD 154: the vertical. SetShotLine never reads it, so the arc is
        // rebuilt on the barrel angle instead. See AimTrace.h.
        if (sscanf_s(line, " shot_pitch = %f", &v) == 1)
            aimtrace::set_shot_pitch(v > 0.0f);
        // Build 55: rotate the engine's own aim quaternion. Degrees, 0 off.
        if (sscanf_s(line, " aim_quat_deg = %f", &v) == 1) {
            if (v < -90.0f) v = -90.0f;
            if (v >  90.0f) v =  90.0f;
            g_aq_deg = v;
            aimtrace::set_aim_quat(g_aq_deg, g_aq_axis);
        }
        if (sscanf_s(line, " aim_quat_axis = %f", &v) == 1) {
            g_aq_axis = (int)v;
            aimtrace::set_aim_quat(g_aq_deg, g_aq_axis);
        }
        // Build 52: retarget the override (hex RVA, 0 = the per-shot sites).
        // The control test points it at a read the engine demonstrably acts
        // on, which is what tells "no effect" apart from "no write".
        {
            unsigned r = 0;
            if (sscanf_s(line, " aim_shot_site_yaw = %x", &r) == 1)
                g_shot_site_yaw = r;
            if (sscanf_s(line, " aim_shot_site_pitch = %x", &r) == 1)
                g_shot_site_pitch = r;
        }
        if (sscanf_s(line, " aim_shot_pitch_deg = %f", &v) == 1) {
            if (v < -45.0f) v = -45.0f;
            if (v >  45.0f) v =  45.0f;
            g_shot_pitch_deg = v;
            aimtrace::set_shot_offset(g_shot_yaw_deg * 0.01745329f,
                                      g_shot_pitch_deg * 0.01745329f);
        }
    }
    fclose(f);
    // Applied after the whole file is parsed, so the site keys and the angle
    // keys take effect together regardless of the order they appear in.
    aimtrace::set_shot_sites(g_shot_site_yaw, g_shot_site_pitch);
    aimtrace::set_shot_offset(g_shot_yaw_deg * 0.01745329f,
                              g_shot_pitch_deg * 0.01745329f);
    // Filter before mode: set_wgun restarts the filter generation, so the
    // constants must already be in place when the next armed call runs.
    camera::set_wgun_filter(g_cfg_wgun_smooth, g_cfg_wgun_maxstep);
    camera::set_wgun_pos((int)g_cfg_wgun_pos, g_cfg_wgun_pos_scale,
                         g_cfg_wgun_pos_clamp, g_cfg_wgun_pos_smooth);
    // Build 99: the grip, applied with the rest of the weapon block so a cfg
    // save is the way back from any amount of tuner wandering.
    camera::set_wgun_grip(g_cfg_wgun_grip_fwd, g_cfg_wgun_grip_lat,
                          g_cfg_wgun_grip_up, g_cfg_wgun_grip_two);
    camera::set_wgun_grip_bone(g_cfg_wgun_grip_bone >= 0.5f ? 1 : 0);  // BUILD 219
    // BUILD 221: the PiP scope. The present-thread values are plain statics
    // written here on the init thread, read once a frame; a torn read costs one
    // frame of a wrong zoom, as with the dot and marker switches above.
    g_pip_enable = g_cfg_pip >= 0.5f;
    // BUILD 237: the zoom is a user control now, 1.2x to 4x, on both the F1
    // panel and the desktop GUI. The range is his: 1.2 is "barely magnified,
    // clearest" and 4 is "as far as this source can be pushed". It is a real
    // clamp, not a suggestion, because the disc cannot hold more angular
    // detail than the eye frame it samples: at 4x the same pixels are spread
    // over four times the area and it gets blurrier, not sharper. The slider
    // gives control over that trade, not an escape from it.
    g_pip_zoom = (isfinite(g_cfg_pip_zoom) && g_cfg_pip_zoom >= 1.2f &&
                  g_cfg_pip_zoom <= 4.0f) ? g_cfg_pip_zoom : 2.7f;
    g_pip_radius = (isfinite(g_cfg_pip_radius) && g_cfg_pip_radius > 0.002f && g_cfg_pip_radius < 0.20f) ? g_cfg_pip_radius : 0.016f;
    camera::set_pip(g_pip_enable ? 1 : 0);
    camera::set_pip_fallback(g_cfg_pip_lat, g_cfg_pip_fwd, g_cfg_pip_up);
    camera::set_pip_block_ads(g_cfg_pip_block_ads >= 0.5f ? 1 : 0);
    camera::set_pip_toggle(g_cfg_pip_toggle >= 0.5f ? 1 : 0);   // BUILD 237
    camera::set_scope_native(g_cfg_scope_native >= 0.5f ? 1 : 0);          // BUILD 238
    camera::set_scope_hold(isfinite(g_cfg_scope_hold) ? (int)g_cfg_scope_hold : 6);
    camera::set_wgun_twohand((int)g_cfg_wgun_twohand);
    camera::set_wgun_two_agree(g_cfg_wgun_two_agree);
    camera::set_wgun_two_attach(g_cfg_wgun_two_attach);
    camera::set_wgun_two_auto((int)g_cfg_wgun_two_auto);
    camera::set_wgun_hands((int)g_cfg_wgun_hands);
    camera::set_wgun_hands_grab((int)g_cfg_wgun_hands_grab);
    camera::set_wgun_hand_free((int)g_cfg_wgun_hand_free);
    camera::set_wgun_hide_arms((int)g_cfg_wgun_hide_arms);
    camera::set_wgun_arms((int)g_cfg_wgun_arms);
    camera::set_wgun_wrist((int)g_cfg_wgun_wrist);
    camera::set_wgun_wrist_abs((int)g_cfg_wgun_wrist_abs);
    camera::set_wgun_twist((int)g_cfg_wgun_twist);
    camera::set_wgun_twist_share(g_cfg_wgun_twist_share);
    camera::set_wgun_twist_arm(g_cfg_wgun_twist_arm);
    camera::set_wgun_wristfirst((int)g_cfg_wgun_wristfirst);
    camera::set_wgun_wrist_limit(g_cfg_wgun_wrist_limit);
    camera::set_wgun_shoulder_max(g_cfg_wgun_shoulder_max);
    camera::set_wgun_clav_max(g_cfg_wgun_clav_max);
    camera::set_wgun_mirror((int)g_cfg_wgun_mirror);
    camera::set_wroll_probe((int)g_cfg_wroll_probe);
    camera::set_wgun_roll_unflip((int)g_cfg_wgun_roll_unflip);
    camera::set_wgun_own((int)g_cfg_wgun_own);
    camera::set_fov_scene_ads((int)g_cfg_fov_scene_ads);   // BUILD 198
    camera::set_sprint_probe((int)g_cfg_sprint_probe);     // BUILD 200
    camera::set_wgun_lead(g_cfg_wgun_lead);                // BUILD 201
    camera::set_gpair(g_cfg_gpair != 0.0f ? 1 : 0);        // BUILD 277
    camera::set_wgun_drawcam(g_cfg_wgun_drawcam != 0.0f ? 1 : 0); // BUILD 282
    camera::set_wgun_pull(g_cfg_wgun_pull != 0.0f ? 1 : 0, g_cfg_wgun_pull_k, g_cfg_wgun_pull_max); // BUILD 283
    set_head_roll_fix((int)g_cfg_head_roll_fix);           // BUILD 202
    {   // BUILD 204
        // BUILD 300: the lean owns the head offset (soft limit, no hard clamp); head_pos alone is the old build-204 path
        const int lean = g_cfg_lean != 0.0f ? 1 : 0;
        {
            auto okf = [](float x, float lo, float hi, float dflt) { return isfinite(x) && x >= lo && x <= hi ? x : dflt; };
            g_lean_max.store(okf(g_cfg_lean_max, 0.05f, 1.5f, 0.40f), std::memory_order_relaxed);
            g_lean_soft.store(okf(g_cfg_lean_soft, 0.0f, 0.95f, 0.75f), std::memory_order_relaxed);
            g_lean_down.store(okf(g_cfg_lean_down, 0.0f, 1.0f, 0.25f), std::memory_order_relaxed);
            g_lean_up.store(okf(g_cfg_lean_up, 0.0f, 1.0f, 0.10f), std::memory_order_relaxed);
            g_lean_hold_ms.store((int)okf(g_cfg_lean_hold_ms, 0.0f, 5000.0f, 800.0f), std::memory_order_relaxed);
            if (g_lean_mode.exchange(lean, std::memory_order_relaxed) != lean)
                LOG_INFO("lean: the head's offset moves the view %s (cfg lean; build 300: soft limit %.2f m horizontal with the easing from "
                         "%.0f%% of it, %.2f m down, %.2f m up; hold the left menu button %d ms to recentre, a shorter press is Start on release).",
                         lean ? "ON" : "off", g_lean_max.load(std::memory_order_relaxed), g_lean_soft.load(std::memory_order_relaxed) * 100.0f,
                         g_lean_down.load(std::memory_order_relaxed), g_lean_up.load(std::memory_order_relaxed),
                         g_lean_hold_ms.load(std::memory_order_relaxed));
            camera::set_lean(g_cfg_lean_hinge != 0.0f ? 1 : 0, g_cfg_lean_theta, g_cfg_lean_slide);
        }
        const int want = (g_cfg_head_pos != 0.0f || lean) ? 1 : 0;
        float mx = g_cfg_head_pos_max;
        if (!(mx >= 0.0f) || !isfinite(mx)) mx = 0.0f;
        if (mx > 3.0f) mx = 3.0f;
        g_head_pos_max.store(mx, std::memory_order_relaxed);
        if (g_head_pos_on.exchange(want, std::memory_order_relaxed) != want)
            LOG_INFO("hpos: head translation %s (cfg head_pos; build 204: the head's movement since the "
                     "reference moves the viewpoint, clamped to %.2f m; the hands follow the eyes).",
                     want ? "ON" : "off", mx);
    }
    {   // BUILD 205
        const int want = g_cfg_stance != 0.0f ? 1 : 0;
        g_stance_height.store(isfinite(g_cfg_stance_height) && g_cfg_stance_height > 1.0f && g_cfg_stance_height < 2.5f ? g_cfg_stance_height : 0.0f, std::memory_order_relaxed);
        g_stance_crouch.store(isfinite(g_cfg_stance_crouch) && g_cfg_stance_crouch > 0.3f && g_cfg_stance_crouch < 0.95f ? g_cfg_stance_crouch : 0.75f, std::memory_order_relaxed);
        g_stance_prone.store(isfinite(g_cfg_stance_prone) && g_cfg_stance_prone > 0.1f && g_cfg_stance_prone < 0.7f ? g_cfg_stance_prone : 0.0f, std::memory_order_relaxed);   // BUILD 208: anything else = off
        g_crouch_drop.store(isfinite(g_cfg_crouch_drop) && g_cfg_crouch_drop >= 0.0f && g_cfg_crouch_drop <= 1.0f ? g_cfg_crouch_drop : 0.25f, std::memory_order_relaxed);   // BUILD 301
        g_crouch_rise.store(isfinite(g_cfg_crouch_rise) && g_cfg_crouch_rise >= 0.0f && g_cfg_crouch_rise <= 0.5f ? g_cfg_crouch_rise : 0.10f, std::memory_order_relaxed);   // BUILD 301
        if (g_stance_on.exchange(want, std::memory_order_relaxed) != want)
            LOG_INFO("stance: loop %s (cfg stance; build 205: the physical head height picks the stance, the pad's B "
                     "makes the character match it; build 301: crouch when the head is %.2f m below the standing height taken at the "
                     "recentre, stand again %.2f m above that (crouch_drop 0 = the old ratio %.2f); prone %s; the eyes "
                     "stay at the physical height through the change).", want ? "ON" : "off",
                     g_crouch_drop.load(std::memory_order_relaxed), g_crouch_rise.load(std::memory_order_relaxed),
                     g_stance_crouch.load(std::memory_order_relaxed),
                     g_stance_prone.load(std::memory_order_relaxed) > 0.05f ? "from the head too" : "stays the pad's (build 208)");
    }
    camera::set_fov_scene((int)g_cfg_fov_scene);   // BUILD 198: before the source and the root, so it owns the field while on
    camera::set_fov_src((int)g_cfg_fov_src);       // BUILD 197: before the root, so a reload that turns it off can turn the root on
    camera::set_fov_root((int)g_cfg_fov_root);
    camera::set_fov_root_mode((int)g_cfg_fov_root_mode);
    camera::set_fov_root_aimk(g_cfg_fov_root_aimk);
    camera::set_wgun_pitch(g_cfg_wgun_pitch_deg);
    camera::set_ray_probe((int)g_cfg_ray_probe);
    camera::set_wgun_two_grip((int)g_cfg_wgun_two_grip);
    // hand_calib is a momentary: a 1 in the file arms one capture per reload,
    // which is harmless (log only) and the reliable file-edit route survives.
    if (g_cfg_stance_calib >= 0.5f) { stance_calib_start(); g_cfg_stance_calib = 0.0f; }   // BUILD 207: its own momentary
    camera::set_arm_anchor(g_cfg_arm_anchor, g_cfg_arm_anchor_max);   // BUILD 213
    camera::set_arm_pole(g_cfg_arm_pole, g_cfg_arm_pole_min);         // BUILD 214
    camera::set_pole_smooth(g_cfg_arm_pole_smooth, g_cfg_arm_pole_tau_ms, g_cfg_arm_pole_rate);   // BUILD 285
    camera::set_pole_down(g_cfg_arm_pole_down_l, g_cfg_arm_pole_down_r);                          // BUILD 293
    camera::set_torso(g_cfg_torso_turn, g_cfg_torso_dead, g_cfg_torso_rate, g_cfg_torso_hand_w, g_cfg_torso_max);   // BUILD 294
    {   // BUILD 216: fixed foveated rendering. The first apply decides whether the hooks go in at all.
        ffr::Params fp;
        fp.on         = g_cfg_ffr != 0.0f ? 1 : 0;
        fp.inner      = g_cfg_ffr_inner;
        fp.mid        = g_cfg_ffr_mid;
        fp.outer      = g_cfg_ffr_outer;
        fp.horizontal = g_cfg_ffr_horizontal != 0.0f ? 1 : 0;
        fp.cull       = g_cfg_ffr_cull != 0.0f ? 1 : 0;
        fp.margin     = g_cfg_ffr_margin;
        ffr::set_params(fp);
    }
    {   // BUILD 209
        const int want = g_cfg_follow != 0.0f ? 1 : 0;
        g_follow_dead.store(isfinite(g_cfg_follow_dead) && g_cfg_follow_dead >= 0.05f && g_cfg_follow_dead <= 1.0f ? g_cfg_follow_dead : 0.20f, std::memory_order_relaxed);
        g_follow_push.store(isfinite(g_cfg_follow_push) && g_cfg_follow_push >= 0.30f && g_cfg_follow_push <= 1.0f ? g_cfg_follow_push : 0.75f, std::memory_order_relaxed);
        g_follow_stop.store(isfinite(g_cfg_follow_stop) && g_cfg_follow_stop >= 0.0f && g_cfg_follow_stop <= 0.5f ? g_cfg_follow_stop : 0.10f, std::memory_order_relaxed);
        g_follow_vmax.store(isfinite(g_cfg_follow_vmax) && g_cfg_follow_vmax >= 0.3f && g_cfg_follow_vmax <= 8.0f ? g_cfg_follow_vmax : 2.0f, std::memory_order_relaxed);
        g_follow_bleed.store(isfinite(g_cfg_follow_bleed) && g_cfg_follow_bleed >= 0.0f && g_cfg_follow_bleed <= 2.0f ? g_cfg_follow_bleed : 0.30f, std::memory_order_relaxed);
        if (g_follow_on.exchange(want, std::memory_order_relaxed) != want)
            LOG_INFO("follow: body follow %s (cfg follow; build 211: past %.2f m the character walks under the head on "
                     "the left stick at %.2f of it, lets go one stopping distance out (%.2f s of its own speed, capped "
                     "at %.1f m/s), and the settle takes the last centimetres out of the view at %.2f m/s; only while "
                     "his own stick is idle and he is not rushing).", want ? "ON" : "off",
                     g_follow_dead.load(std::memory_order_relaxed), g_follow_push.load(std::memory_order_relaxed),
                     g_follow_stop.load(std::memory_order_relaxed), g_follow_vmax.load(std::memory_order_relaxed),
                     g_follow_bleed.load(std::memory_order_relaxed));
    }
    if (g_cfg_hand_calib >= 0.5f) {
        camera::set_hand_calib(1);
        g_cfg_hand_calib = 0.0f;
    }
    camera::set_wgun_two_anchor((int)g_cfg_wgun_two_anchor);
    camera::set_hide_child(0, (int)g_cfg_hide_child);
    camera::set_hide_child(1, (int)g_cfg_hide_child2);
    camera::set_head_prelatch(g_cfg_head_prelatch >= 0.5f ? 1 : 0);
    camera::set_head_reassert(g_cfg_head_reassert >= 0.5f ? 1 : 0);
    camera::set_head_always(g_cfg_head_always >= 0.5f ? 1 : 0);
    camera::set_head_populate(g_cfg_head_populate >= 0.5f ? 1 : 0);
    camera::set_fp_auto(g_cfg_fp_auto >= 0.5f ? 1 : 0);
    camera::set_head_pulse(isfinite(g_cfg_head_pulse) ? (int)g_cfg_head_pulse : 30);
    g_main_hand.store(g_cfg_main_hand >= 0.5f ? 1 : 0, std::memory_order_relaxed);
    camera::set_ipd_sign(g_cfg_ipd_swap >= 0.5f ? -1 : 1);
    camera::set_wgun_grip_bypass(g_cfg_wgun_grip_bypass >= 0.5f ? 1 : 0);
    camera::set_wgun_roll((int)g_cfg_wgun_roll, g_cfg_wgun_roll_deg);
    camera::set_wgun((int)g_cfg_wgun, g_cfg_wgun_dz);
    camera::set_wbaxis((int)g_cfg_wbaxis);
    camera::set_wnode((int)g_cfg_wnode, g_cfg_wnode_dz);
    g_ipd_default = headpose::ipd_scale();
    {   // BUILD 270: gtrec's header snapshot (log only), in FileHdr::cfg order
        g_gt_cfg[0]  = headpose::ipd_scale();
        g_gt_cfg[1]  = (float)camera::get_ipd_sign();
        g_gt_cfg[2]  = (float)g_cfg_layer_lag.load(std::memory_order_relaxed);
        g_gt_cfg[3]  = (float)g_cfg_veh_lag.load(std::memory_order_relaxed);
        g_gt_cfg[4]  = (float)g_cfg_veh_depth.load(std::memory_order_relaxed);
        g_gt_cfg[5]  = headpose::cam_pose_rot() ? 1.0f : 0.0f;
        g_gt_cfg[6]  = g_cfg_head_roll_fix != 0.0f ? 1.0f : 0.0f;
        g_gt_cfg[9]  = cam2::stereo_swap_eyes() ? 1.0f : 0.0f;
        g_gt_cfg[10] = g_cfg_fs_fov_cfg;
        g_gt_cfg[11] = cam2::stereo_target_fovy();
        g_gt_cfg[13] = camera::veh_smooth();
        gtrec::set_cfg_snapshot(g_gt_cfg);
    }
    LOG_INFO("VR: grwxr.cfg read, ipd_scale = %.2f, mono_scope_fov = %.2f rad",
             headpose::ipd_scale(), headpose::mono_scope_fov());
}

const char* state_name(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE:         return "IDLE";
        case XR_SESSION_STATE_READY:        return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
        case XR_SESSION_STATE_STOPPING:     return "STOPPING";
        case XR_SESSION_STATE_EXITING:      return "EXITING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        default:                            return "?";
    }
}

bool fail(const char* what, XrResult r) {
    LOG_ERROR("VR: %s failed (XrResult %d). VR disabled, game unaffected.", what, (int)r);
    g_failed.store(true);
    return false;
}

// --- BUILD 8: head rotation -> camera (docs/HANDOFF.md "BUILD 8") -----------
//
// Each presented frame we locate the headset (VIEW space in LOCAL space at the
// predicted display time), take its orientation relative to a yaw-only
// reference captured on the first tracked pose, convert that rotation to the
// game's basis, and publish it through headpose:: for the camera hook to
// compose onto Camera+0x000. All of the XR-side maths lives here so the camera
// side stays engine-pure.

struct Quat { float x, y, z, w; };

// Hamilton product, the convention where R(a*b) = R(a) * R(b) in column form.
Quat qmul(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// Row-vector rotation matrix (the transpose of the usual column form): rows
// are the images of the basis vectors, composing as v * M, matching the
// engine's convention (docs/RE-notes.md, build 5).
void quat_to_rows(const Quat& q, float R[9]) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    R[0] = 1.0f - 2.0f * (yy + zz); R[1] = 2.0f * (xy + wz);        R[2] = 2.0f * (xz - wy);
    R[3] = 2.0f * (xy - wz);        R[4] = 1.0f - 2.0f * (xx + zz); R[5] = 2.0f * (yz + wx);
    R[6] = 2.0f * (xz + wy);        R[7] = 2.0f * (yz - wx);        R[8] = 1.0f - 2.0f * (xx + yy);
}

// BUILD 42: express a reference-space vector in the head's own frame,
// out = conj(q) * v * q. Used only by the hand markers.
void quat_rotate_inv(const Quat& q, const float v[3], float out[3]) {
    const float x = -q.x, y = -q.y, z = -q.z, w = q.w;   // conjugate
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

bool g_have_ref = false;
Quat g_ref_inv{0.0f, 0.0f, 0.0f, 1.0f};   // conjugate of the yaw-only reference

// --- Build 19: VR HEAD AIM ------------------------------------------------
//
// Feeds the head's yaw/pitch into the engine's ABSOLUTE aim pair through the
// consume-once deltas of camera::aim_arm (the mechanism build 17 proved:
// absolute, persistent, steers view AND bullets). The camera hook subtracts
// what the engine has absorbed (headpose::set_aim_cum) before composing the
// full head rotation, so the view is always the true head pose and never
// depends on how fast, or whether, the engine consumes: menus, cutscenes and
// a clamped pitch all degrade to exactly the pre-injection behaviour.
//
// The one unknown the offline work could not settle is whether the engine's
// yaw/pitch units run the same direction as our geometric convention, so
// both signs are cfg keys (state lives with the other config globals at the
// top of the file). A wrong sign shows up as the view turning the WRONG WAY
// on that axis (headset-confirmed, session 20): the compose overcorrects by
// twice the injection, net reversed. Flip the key, relaunch, done.

float wrap_pi(float a) {
    while (a >  3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

// Once per present, right after the head rotation is computed. axis 0 = yaw,
// 1 = pitch, both geometric radians in the game basis.
// Build 49 fix 3: one alternating-sign 1-count mouse move, at most one per
// 80 ms, only while the game owns the foreground. Its sole purpose is to
// wake the engine's aim path so a pending injection gets consumed.
void kick_engine_aim() {
    static ULONGLONG s_last = 0;
    static int       s_sign = 1;
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 80) return;
    HWND  fg  = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dx      = s_sign;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
    s_sign  = -s_sign;
    s_last  = now;
    g_aim_kicks.fetch_add(1, std::memory_order_relaxed);
}

void aim_pump(float head_yaw, float head_pitch) {
    const float head[2] = {head_yaw, head_pitch};
    // Injection pauses below the world fov band (ADS and optics, hazard 25
    // territory) and while toggled off. While paused the baseline re-zeroes
    // every frame, so resuming never snaps: aim continues from wherever the
    // engine has it, tracking head MOVEMENT from that moment on.
    // Build 84: the 0.65 floor is hazard-25 territory (ADS and optics), and it
    // is why head-driven injection pauses when you aim. Barrel aim cannot live
    // with that, because the right trigger engages ADS at 0.55 and only fires
    // at 0.90, so the fov is ALWAYS down by the time a round leaves. While the
    // barrel loop is actively driving, the floor drops to the real-optics
    // threshold instead. A magnified scope is still excluded, which is the case
    // hazard 25 was actually written for: there the engine owns the sight
    // picture and the compose already stands down.
    // Build 88: BARREL AIM MUST NOT BE GATED ON THE HEAD-AIM TOGGLE.
    //
    // g_vraim_on is Numpad Decimal, "VR head aim", and it defaults to OFF.
    // Build 84 left it in front of everything, so on the 2026-08-14 headset run
    // the barrel loop computed correct errors off the real barrel (frames=646,
    // src=barrel, err a few degrees) and handed every one of them to a pump
    // that was switched off: "injected 0 presents". The aim therefore stayed
    // exactly where the engine had it, which in this engine means the camera,
    // which means the head. The feature looked completely dead while in fact
    // only its last step was disabled.
    //
    // The two are also logically opposed. Head aim means "my head steers the
    // gun"; barrel aim means "my hands steer the gun". Requiring the first to
    // be on before the second can act is backwards, and it is why the tester
    // reported "aim still follows head" on a build where the barrel direction
    // was being computed correctly all along.
    const float aim_fov = headpose::read_fov(0.7853982f);
    const bool bar_want = g_bar_want.load(std::memory_order_relaxed);
    // BUILD 261: in a vehicle the head is composed onto the body heading (CameraProbe.cpp veh_step), so the head must
    // not ALSO be injected into the game's aim: injection stands down, the baseline re-zeroes every frame, and the
    // falling edge (getting out) re-syncs so the on foot view resumes from where the engine has it.
    const bool veh_now = camera::veh_on();
    static bool s_veh_prev = false;
    if (s_veh_prev && !veh_now) g_aim_resync.store(true, std::memory_order_relaxed);
    s_veh_prev = veh_now;
    const bool live = !veh_now && (g_vraim_on || bar_want) &&
                      (aim_fov >= 0.65f ||
                       (bar_want && aim_fov >= headpose::mono_scope_fov()));

    if (g_rc_on.load(std::memory_order_relaxed) && camera::aim_available()) {   // BUILD 289/292: log only, plain stores, no I/O
        float bp = 0.0f;
        if (!camera::base_pitch_geo(&bp)) bp = 0.0f;
        const unsigned bf = g_bar_frames.load(std::memory_order_relaxed);
        const bool ran = bf != g_rcx_frames_seen;           // the barrel block drove the aim since the last row
        g_rcx_frames_seen = bf;
        RcRow& r = g_rc[g_rc_w.load(std::memory_order_relaxed) & (kRcN - 1)];
        r.t_ms  = (uint32_t)GetTickCount64();
        r.shots = aimtrace::shots_total();
        r.trig  = g_in_trig[1].load(std::memory_order_relaxed);
        r.eng   = camera::aim_lastval(1) * 57.29578f;
        r.base  = bp * 57.29578f;
        r.head  = head_pitch * 57.29578f;
        r.calls = (uint32_t)camera::aim_count(1);
        r.ep     = ran ? g_bar_diag[1].load(std::memory_order_relaxed) * 57.29578f : 0.0f;
        r.barrel = ran ? g_rcx_barrel * 57.29578f : 0.0f;
        r.view   = ran ? g_rcx_view * 57.29578f : 0.0f;
        r.cum    = g_aim_cum[1] * 57.29578f;
        r.fov    = aim_fov;
        r.obj    = (uint32_t)camera::aim_lastobj();
        r.flags  = (uint8_t)((ran ? 1 : 0) | (live ? 2 : 0) | (aimtrace::firing() ? 4 : 0) |
                             ((camera::aim_pending(1) || g_aim_outf[1]) ? 8 : 0));
        g_rc_w.fetch_add(1, std::memory_order_release);
    }

    // Build 49 fix 2: complete a pending recenter resync. New injections
    // pause until it lands; the kicker unblocks a parasitic stall so this
    // cannot hang on an idle engine.
    // BUILD 290: THE PITCH BASELINE IS ABSOLUTE. His word 2026-10-11: he loaded in and the view stared up; earlier, after the
    // swoop he looked down, looked up, and the tilt reversed. The recoil CSV of build 289 (recoil-30132) shows why: the pump
    // took its baseline when it went live, with his head 44.6 deg DOWN and the engine at 0.03 deg, then drove the engine by
    // the head's change since that moment, so a level head showed as +44 deg (engine 32 deg at 1.7 s, 52 deg at the end).
    // A headset's pitch is measured against gravity, so the engine pitch must follow the head's own pitch, not its change.
    // At the first live frame, and after every recenter or vehicle exit, the pitch accounting is set to the engine's own
    // measured pitch and the baseline to zero, so the pump drives the engine to the head pitch (per-injection clamp
    // 17 deg, so a large first correction takes a few frames). Yaw keeps its relative baseline. ADS and fov gating keep the
    // old resume-without-snap behaviour. cfg aim_pitch_abs, code default ON.
    static bool s_abs = true;
    // BUILD 291: and after EVERY stretch where the pump was not live (ADS, fov gating, a vehicle, the pre-fp swoop): the 290 run
    // (recoil-23660.csv, 27 s in) had the engine's pitch stick at -7.6 deg for 4 s while the head moved to +7 and the pump kept
    // booking pushes the engine did not take, leaving the view 15 to 25 deg too low afterwards. The character loads crouched
    // and the stance change takes the camera for a few seconds.
    if (!live) s_abs = true;
    if (g_aim_resync.load(std::memory_order_relaxed)) {
        s_abs = true;
        bool busy = false;
        for (int a = 0; a < 2; ++a) {
            if (camera::aim_pending(a)) { busy = true; continue; }
            g_aim_outf[a] = false;   // its accounting dies with the reset
        }
        if (busy) {
            if (live) kick_engine_aim();
            return;
        }
        g_aim_cum[0] = g_aim_cum[1] = 0.0f;
        headpose::set_aim_cum(0.0f, 0.0f);
        g_aim_zero[0] = head[0];
        g_aim_zero[1] = head[1];
        g_aim_resync.store(false, std::memory_order_relaxed);
        note("aim: RESYNC complete (recenter): accounting zeroed, view and "
             "body re-agree from here.");
        return;
    }

    // Build 49 fix 3: a delta pending longer than 300 ms means the engine is
    // idle and not consuming (hazard 45, "aim only worked while shooting").
    // Kick it awake.
    if (live) {
        static ULONGLONG s_pend_since = 0;
        const bool pending =
            camera::aim_pending(0) || camera::aim_pending(1);
        const ULONGLONG now = GetTickCount64();
        if (!pending)             s_pend_since = 0;
        else if (!s_pend_since)   s_pend_since = now;
        else if (now - s_pend_since > 300) kick_engine_aim();
    }
    if (s_abs && live && g_cfg_aim_pitch_abs >= 0.5f && camera::aim_count(1) > 0 &&
        !camera::aim_pending(1) && !g_aim_outf[1]) {
        s_abs = false;
        const float eg = camera::aim_lastval(1) * g_aim_sign_pitch;     // the engine's pitch now, geometric radians
        const float before = g_aim_zero[1];
        g_aim_zero[1] = 0.0f;
        g_aim_cum[1]  = eg;
        headpose::set_aim_cum(g_aim_cum[0], g_aim_cum[1]);
        note("aim: pitch baseline ABSOLUTE (build 290): engine pitch %+.1f deg, head pitch %+.1f deg, old baseline %+.1f deg; the pump now "
             "drives the engine to the head's own pitch.",
             eg * 57.29578f, head[1] * 57.29578f, before * 57.29578f);
    } else if (s_abs && g_cfg_aim_pitch_abs < 0.5f) {
        s_abs = false;
    }
    // BUILD 291: THE DRIFT NET. The pump books what it armed (open loop) and that stays the law: no measurement enters the
    // per-frame head tracking (288 broke exactly that). But if the engine's measured pitch has been more than 5 deg away from
    // the booked pitch for half a second with nothing in flight, the booking is wrong (a stance or cover animation took the
    // camera, or the engine clamped, or a bump was taken by another caller of the setter): re-take the absolute baseline, so the
    // accounting is set to the measured pitch and the pump walks the engine back to the head at its normal clamped rate. A
    // normal frame never gets near 5 deg for half a second (recoil-23660: engine within 0.3 deg of the head at rest).
    if (live && !s_abs && g_cfg_aim_pitch_abs >= 0.5f && camera::aim_count(1) > 0 &&
        !camera::aim_pending(1) && !g_aim_outf[1]) {
        static ULONGLONG s_dev_since = 0;
        const ULONGLONG nowd = GetTickCount64();
        const float dev = camera::aim_lastval(1) * g_aim_sign_pitch - g_aim_cum[1];
        if (fabsf(dev) > 0.0873f) {
            if (!s_dev_since) s_dev_since = nowd;
            else if (nowd - s_dev_since > 500) {
                s_dev_since = 0;
                s_abs = true;
                note("aim: pitch drift net (build 291): the engine pitch has been %+.1f deg from the booked pitch for 0.5 s; re-taking the "
                     "absolute baseline.", dev * 57.29578f);
            }
        } else {
            s_dev_since = 0;
        }
    }
    for (int a = 0; a < 2; ++a) {
        if (camera::aim_pending(a)) continue;   // still queued for the engine
        if (g_aim_outf[a]) {
            // The delta armed earlier was consumed: account it. The engine
            // absorbed sign*d in its units, which is d in geometric radians
            // when the cfg sign is right.
            g_aim_outf[a] = false;
            g_aim_cum[a]  = wrap_pi(g_aim_cum[a] + g_aim_out[a]);
            headpose::set_aim_cum(g_aim_cum[0], g_aim_cum[1]);
        }
        if (!live) {
            g_aim_zero[a] = wrap_pi(head[a] - g_aim_cum[a]);
            continue;
        }
        float d = wrap_pi(head[a] - g_aim_zero[a] - g_aim_cum[a]);
        if (fabsf(d) < 0.0005f) continue;       // ~0.03 deg: not worth a write
        if (d >  0.30f) d =  0.30f;             // per-injection clamp: a lost
        if (d < -0.30f) d = -0.30f;             // frame cannot become a whip
        if (a == 1) {
            // Build 20, hazard 31: the engine CLAMPS pitch against external
            // writes, so chasing the head past that clamp credits g_aim_cum
            // with absorption that never happened and the rebuilt base pins
            // near vertical (the session-20 run-2 cascade). Bound the
            // accounted total instead; past the bound the compose still
            // carries the residual, so the view stays the true head pose.
            const float kCumPitchMax = 1.4f;
            if (g_aim_cum[1] + d >  kCumPitchMax) d =  kCumPitchMax - g_aim_cum[1];
            if (g_aim_cum[1] + d < -kCumPitchMax) d = -kCumPitchMax - g_aim_cum[1];
            if (fabsf(d) < 0.0005f) continue;
        }
        const float sign = a == 0 ? g_aim_sign_yaw : g_aim_sign_pitch;
        // BUILD 194: fov_root_aimk scales what the engine is handed while the
        // fov root is armed; the accounting below stays in geometric units.
        if (camera::aim_arm(a, d * sign * camera::fovroot_aim_gain()) == 1) {
            g_aim_out[a]  = d;
            g_aim_outf[a] = true;
            if (gtrec::PRow* gp = gtrec::p_stage()) gp->aim_armed[a] = d;   // BUILD 270: the gtrec P row, log only
        }
    }
}

// BUILD 14d: see the state block at the top of the file. Called once per
// present on the render thread. Injects proportional relative mouse motion
// while the right trigger is held. Fractional counts carry between presents
// so slow rates are not truncated to zero.
// BUILD 14e: SQUEEZE-TO-GRAB reference. 14d measured the offset against the
// HEAD's forward, so the user's natural wrist posture (~25 deg low in the
// 14d logs) fought the deadzone constantly and head motion steered the aim.
// Now the controller orientation AT TRIGGER PRESS becomes the zero; steering
// follows the controller's own tilt since the squeeze, head-independent.
// The deadzone edge is also ramped over 2 degrees so motion starts smooth.
void steer_aim() {
    if (!g_steer_on.load(std::memory_order_relaxed)) return;

    static bool was_held = false;
    static int  settle   = 0;
    static Quat ref_conj{0.0f, 0.0f, 0.0f, 1.0f};

    // 14e.2: gated on the GRIP, not the trigger (user direction). The grip is
    // the natural "hold to steer" and leaves the trigger free for a future
    // fire binding.
    const bool held =
        (g_in_seen.load(std::memory_order_relaxed) & 2u) &&
        g_in_grip[1].load(std::memory_order_relaxed) >= 0.6f;
    if (!held) {
        was_held = false;
        return;
    }

    // Never inject unless the game owns the foreground window.
    HWND  fg  = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;

    const Quat c{g_in_ori[1][0].load(std::memory_order_relaxed),
                 g_in_ori[1][1].load(std::memory_order_relaxed),
                 g_in_ori[1][2].load(std::memory_order_relaxed),
                 g_in_ori[1][3].load(std::memory_order_relaxed)};
    if (!was_held) {
        was_held = true;
        settle   = 10;   // ~140 ms at 72 Hz
    }
    if (settle > 0) {
        // 14e.1: the physical act of squeezing tilts the controller a few
        // degrees, and a zero captured mid-squeeze leaves the settled hand
        // offset from it: constant drift ("pulling the trigger makes the
        // camera move"). Keep re-capturing the zero until the pull settles;
        // steering begins from the settled pose.
        ref_conj = Quat{-c.x, -c.y, -c.z, c.w};
        --settle;
        return;
    }
    const Quat d = qmul(ref_conj, c);   // controller tilt since the squeeze

    // The aim ray is the controller's -Z; d rotates the reference ray onto
    // the current one. Same forward formula update_head uses.
    const float fx = -2.0f * (d.x * d.z + d.w * d.y);
    const float fy = -2.0f * (d.y * d.z - d.w * d.x);
    const float fz = -(1.0f - 2.0f * (d.x * d.x + d.y * d.y));
    const float yaw   = atan2f(fx, -fz) * 57.29578f;   // + = tilted right
    float sy = fy;
    if (sy < -1.0f) sy = -1.0f;
    if (sy >  1.0f) sy =  1.0f;
    const float pitch = asinf(sy) * 57.29578f;         // + = tilted up
    g_steer_yaw_last.store(yaw, std::memory_order_relaxed);
    g_steer_pitch_last.store(pitch, std::memory_order_relaxed);

    auto rate = [&](float deg) {
        const float excess = fabsf(deg) - g_steer_deadzone;
        if (excess <= 0.0f) return 0.0f;
        // Soft start: gain ramps in over the first 2 degrees past the
        // deadzone, so crossing it cannot jerk.
        const float ramp = excess < 2.0f ? excess * 0.5f : 1.0f;
        float r = g_steer_gain * excess * ramp;
        if (r > g_steer_maxrate) r = g_steer_maxrate;
        return deg < 0.0f ? -r : r;
    };
    const float dt = 1.0f / 72.0f;       // present cadence; exact pacing not
                                         // needed for a rate this heavily
                                         // clamped and user-tuned
    static float ax = 0.0f, ay = 0.0f;   // fractional carry, render thread only
    ax += rate(yaw)   * dt * g_steer_mpd;
    ay -= rate(pitch) * dt * g_steer_mpd;   // mouse +y is down; aim up = -y
    const int dx = (int)ax, dy = (int)ay;
    ax -= (float)dx;
    ay -= (float)dy;
    if (!dx && !dy) return;

    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dx      = dx;
    in.mi.dy      = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
    g_steer_ticks.fetch_add(1, std::memory_order_relaxed);
    g_steer_dx.fetch_add(dx, std::memory_order_relaxed);
    g_steer_dy.fetch_add(dy, std::memory_order_relaxed);
}

// BUILD 209: the body follow's tick, after Quat and quat_rotate_inv exist.
// BUILD 210 (the read-only review of 209): the offset and the advance in
// yaw-only frames (the XR horizontal displacement against the head's
// horizontal forward and right; the world horizontal step against the camera
// rows' horizontal projections, back into XR through the same head frame),
// exact inverses at every yaw, pitch and roll under a rotation or a reflection
// between XR and world (followtest211.py).
//
// BUILD 211: THE STICK THE GAME TAKES, THE STOP IT MEASURES, AND THE SETTLE.
// His words, 2026-09-06 21:31, on the 210 run: "it doesnt recenter it perfectly
// you can actually see the where the head is supposed to be and it doesnt move
// back to the camera. Needs to be tightened." The log said why. In
// grwxr-8320.log the follow pushed 0.30 to 0.45 of the left stick for six
// seconds while the 10 Hz sprint probe measured the character's own head bone
// at 0.01 to 0.06 m/s: the body did not move, and the camera-to-bone offset
// held 0.29 to 0.38 m and never closed. The merge answered every poll of that
// run (xin: calls=7820 merged=6987) and the same merge's B injection is
// confirmed (build 205), so the poll is ours and it is the MAGNITUDE the game
// discards: on a diagonal our components were 0.21 of full scale (6880 of
// 32767), under XInput's own 7849 dead zone and under the 0.25 of the game's
// own Filter_Remap_LS. A stick that small cannot move this character at all.
// So the follow stops metering speed with the stick. It pushes one magnitude
// the game cannot discard (follow_push 0.75: 0.53 on each axis of a diagonal,
// 17400 of 32767) and closes the loop on the BODY'S MEASURED SPEED instead: it
// lets go one stopping distance out (the game's v_fStopBlendTime is 0.2 s, so
// follow_stop seconds of the body's own speed), coasts while the advance keeps
// the view still, and then bleeds the last few centimetres out of the VIEW
// (follow_bleed), which is the part no stick can do: the game's smallest step
// is one dead-zone crossing, about 0.1 m of coast. A close that moves the body
// less than 5 cm in 1.5 s is abandoned as blocked (a wall) and settles, so the
// stick is never held down against something that will not move.
enum { FL_IDLE = 0, FL_CLOSING = 1, FL_COAST = 2, FL_SETTLE = 3 };
static void follow_tick(const float dxr[3], const Quat& qh, bool built, bool seated) {
    const float dead  = g_follow_dead.load(std::memory_order_relaxed);
    const float push  = g_follow_push.load(std::memory_order_relaxed);
    const float tstop = g_follow_stop.load(std::memory_order_relaxed);
    const float vmax  = g_follow_vmax.load(std::memory_order_relaxed);
    const float bleed = g_follow_bleed.load(std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();

    // the frame time, for the two speed estimates and the bleed's rate limit.
    // GetTickCount64 is 15 ms coarse, which is one frame here, so the
    // performance counter: read-only, no allocation, no lock (rule 8).
    static LARGE_INTEGER s_freq = {};
    static LARGE_INTEGER s_last = {};
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER nowq;
    QueryPerformanceCounter(&nowq);
    float dt = 0.0f;
    if (s_last.QuadPart && s_freq.QuadPart)
        dt = (float)((double)(nowq.QuadPart - s_last.QuadPart) / (double)s_freq.QuadPart);
    s_last = nowq;
    if (!(dt > 0.0005f && dt < 0.2f)) dt = 0.013f;   // the first frame, or a hitch

    // the head's horizontal frame in XR (+x right, +y up, -z forward): the
    // forward projected onto the floor, held from the last frame within about
    // three degrees of straight up or down, where the projection has no direction
    static float s_fx = 0.0f, s_fz = -1.0f;
    static bool s_fok = false;
    {
        const float back[3] = {0.0f, 0.0f, -1.0f};
        const Quat qc{-qh.x, -qh.y, -qh.z, qh.w};
        float f[3];
        quat_rotate_inv(qc, back, f);   // the inverse of the conjugate: rotate by qh, the head's forward in XR
        const float mh = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (isfinite(mh) && mh > 0.05f) { s_fx = f[0] / mh; s_fz = f[2] / mh; s_fok = true; }
    }
    const float rx = -s_fz, rz = s_fx;                // right = forward x up
    const float lx = dxr[0] * rx + dxr[2] * rz;       // the head's horizontal offset, right
    const float lf = dxr[0] * s_fx + dxr[2] * s_fz;   // and forward
    const float dist = sqrtf(lx * lx + lf * lf);      // = the XR horizontal magnitude
    g_fl_dist.store(dist, std::memory_order_relaxed);

    // the camera's horizontal rows: the advance needs them, and the follow
    // does not push without them (a push with no advance is the slide)
    float R[9], P[3];
    float crx = 0.0f, cry = 0.0f, cfx = 0.0f, cfy = 0.0f;
    bool cam = false;
    if (camera::base_frame(R, P)) {
        const float mr = sqrtf(R[0] * R[0] + R[1] * R[1]);
        const float mf = sqrtf(R[3] * R[3] + R[4] * R[4]);
        if (isfinite(mr) && isfinite(mf) && mr > 0.05f && mf > 0.05f) {
            crx = R[0] / mr; cry = R[1] / mr;
            cfx = R[3] / mf; cfy = R[4] / mf;
            cam = true;
        }
    }

    // the character's own horizontal speed, from its Head bone, and his own
    // head's speed in XR. Both smoothed over about seven frames: the stop
    // estimate and the settle's hold-still gate are the only consumers.
    static float s_vbody = 0.0f, s_vhead = 0.0f;
    static float s_prev[3] = {0.0f, 0.0f, 0.0f};
    static bool s_have = false;
    float hb[3];
    const bool bone = camera::body_head_world(hb);
    float step0 = 0.0f, step1 = 0.0f, stepm = 0.0f;
    if (bone) {
        if (s_have) {
            step0 = hb[0] - s_prev[0]; step1 = hb[1] - s_prev[1];
            stepm = sqrtf(step0 * step0 + step1 * step1);
            const float v = stepm < 0.35f ? stepm / dt : s_vbody;   // a teleport is not a speed
            if (isfinite(v)) s_vbody += (v - s_vbody) * 0.15f;
        }
    } else {
        s_vbody = 0.0f;
    }
    {
        static float s_hprev[3] = {0.0f, 0.0f, 0.0f};
        static bool s_hhave = false;
        if (s_hhave && g_head_pos_ok) {
            const float h0 = g_head_pos[0] - s_hprev[0], h2 = g_head_pos[2] - s_hprev[2];
            const float v = sqrtf(h0 * h0 + h2 * h2) / dt;
            if (isfinite(v) && v < 20.0f) s_vhead += (v - s_vhead) * 0.15f;
        }
        if (g_head_pos_ok) {
            s_hprev[0] = g_head_pos[0]; s_hprev[1] = g_head_pos[1]; s_hprev[2] = g_head_pos[2];
            s_hhave = true;
        }
    }
    g_fl_vbody.store(s_vbody, std::memory_order_relaxed);
    g_fl_vhead.store(s_vhead, std::memory_order_relaxed);

    float ulx = 0.0f, uly = 0.0f; unsigned int ubtn = 0;
    camera::move_input(&ulx, &uly, &ubtn);
    const bool user_moving = ulx * ulx + uly * uly > 0.01f;
    const bool rush = (ubtn & 0x0040u) != 0;   // the LS click, Base_Rush
    static uint64_t s_user_ms = 0;
    if (user_moving) s_user_ms = now;
    const bool user_recent = s_user_ms != 0 && (now - s_user_ms) < 300;
    const bool sane = s_fok && isfinite(dist) && dist < 3.0f;
    if (isfinite(dist) && dist >= 3.0f) g_fl_far.fetch_add(1, std::memory_order_relaxed);
    const bool gate = g_follow_on.load(std::memory_order_relaxed) != 0 && headpose::fp_enabled() &&
                      built && !seated && sane && !user_moving && !user_recent && !rush;

    // the state machine
    static int s_state = FL_IDLE;
    static uint64_t s_state_ms = 0;
    static float s_from[2] = {0.0f, 0.0f};
    static bool s_from_ok = false;
    if (!gate && s_state != FL_IDLE) { s_state = FL_IDLE; s_state_ms = now; }
    switch (s_state) {
    case FL_IDLE:
        if (gate && dist > dead) {
            s_state = FL_CLOSING; s_state_ms = now;
            s_from[0] = bone ? hb[0] : 0.0f; s_from[1] = bone ? hb[1] : 0.0f; s_from_ok = bone;
            g_fl_closes.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    case FL_CLOSING: {
        const float stopd = s_vbody * tstop + 0.03f;
        if (now - s_state_ms > 1500) {
            const float m0 = bone && s_from_ok ? hb[0] - s_from[0] : 1.0f;
            const float m1 = bone && s_from_ok ? hb[1] - s_from[1] : 0.0f;
            if (sqrtf(m0 * m0 + m1 * m1) < 0.05f) {
                // blocked: a wall, a slope, an input context that will not walk.
                // The stick is not held down against it; the view comes back instead.
                g_fl_blocked.fetch_add(1, std::memory_order_relaxed);
                s_state = FL_SETTLE; s_state_ms = now;
                g_fl_settles.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            s_state_ms = now;
            s_from[0] = bone ? hb[0] : 0.0f; s_from[1] = bone ? hb[1] : 0.0f; s_from_ok = bone;
        }
        if (dist <= stopd || dist <= dead * 0.35f || s_vbody > vmax) { s_state = FL_COAST; s_state_ms = now; }
        break; }
    case FL_COAST:
        if ((s_vbody < 0.12f && now - s_state_ms > 150) || now - s_state_ms > 900) {
            if (dist > dead) {
                s_state = FL_CLOSING; s_state_ms = now;
                s_from[0] = bone ? hb[0] : 0.0f; s_from[1] = bone ? hb[1] : 0.0f; s_from_ok = bone;
                g_fl_closes.fetch_add(1, std::memory_order_relaxed);
            } else {
                s_state = FL_SETTLE; s_state_ms = now;
                g_fl_settles.fetch_add(1, std::memory_order_relaxed);
            }
        }
        break;
    case FL_SETTLE:
        // his own head moving is a lean he means: the settle is only ever the
        // follow's own leftover, never a physical offset he is holding
        if (dist < 0.02f || now - s_state_ms > 2500 || s_vhead > 0.30f) { s_state = FL_IDLE; s_state_ms = now; }
        break;
    }
    g_fl_state.store(s_state, std::memory_order_relaxed);

    // the push
    bool active = false;
    float sx = 0.0f, sy = 0.0f;
    if (s_state == FL_CLOSING && dist > 1e-4f) {
        if (!cam) {
            g_fl_nocam.fetch_add(1, std::memory_order_relaxed);
        } else {
            sx = lx / dist * push;
            sy = lf / dist * push;
            active = true;
            g_fl_frames.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_fl_stick.store(active ? sqrtf(sx * sx + sy * sy) : 0.0f, std::memory_order_relaxed);
    headpose::set_follow_stick(sx, sy, active);
    if (!gate) {
        if (user_moving || user_recent) g_fl_user.fetch_add(1, std::memory_order_relaxed);
        else if (rush) g_fl_rush.fetch_add(1, std::memory_order_relaxed);
    }

    // the reference advance: while the follow is closing and while the body
    // coasts to its stop, the body's motion is the follow's, so the reference
    // moves with it and the view stays where it is. World horizontal step,
    // onto the camera's horizontal right and forward, into XR through the
    // head's horizontal frame: the exact inverse of the offset above.
    if (bone) {
        if (s_have && (s_state == FL_CLOSING || s_state == FL_COAST)) {
            if (stepm < 0.35f) {
                if (stepm > 1e-5f && cam) {
                    const float hx = step0 * crx + step1 * cry;
                    const float hf = step0 * cfx + step1 * cfy;
                    const float vx0 = hx * rx + hf * s_fx;
                    const float vx2 = hx * rz + hf * s_fz;
                    if (isfinite(vx0) && isfinite(vx2)) {
                        g_head_ref_pos[0] += vx0;
                        g_head_ref_pos[2] += vx2;
                        g_fl_advances.fetch_add(1, std::memory_order_relaxed);
                        g_fl_adv.store(stepm, std::memory_order_relaxed);
                    }
                }
            } else {
                g_fl_teleports.fetch_add(1, std::memory_order_relaxed);
            }
        }
        s_prev[0] = hb[0]; s_prev[1] = hb[1]; s_prev[2] = hb[2];
        s_have = true;
    } else {
        s_have = false;
    }

    // the settle: the stick cannot place the body inside its own stopping
    // distance, so the last few centimetres come out of the view. The
    // reference moves toward the head along the horizontal XR direction of the
    // offset, which reduces the offset by exactly the step and moves the view
    // onto the character's head by exactly the step (followtest211.py).
    if (s_state == FL_SETTLE && dist > 0.005f && s_vhead < 0.30f && dt > 0.0f) {
        float st = dist * (1.0f - expf(-dt / 0.25f));   // a 0.25 s time constant
        const float lim = bleed * dt;                   // and a rate limit
        if (st > lim) st = lim;
        if (st > dist) st = dist;
        const float ux = dxr[0] / dist, uz = dxr[2] / dist;
        if (isfinite(ux) && isfinite(uz) && st > 0.0f) {
            g_head_ref_pos[0] += ux * st;
            g_head_ref_pos[2] += uz * st;
            g_fl_bleeds.fetch_add(1, std::memory_order_relaxed);
            g_fl_bled.store(g_fl_bled.load(std::memory_order_relaxed) + st, std::memory_order_relaxed);
        }
    }
}

void update_head(XrSpaceLocationFlags flags, const XrQuaternionf& o) {
    if (!(flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    const Quat q{o.x, o.y, o.z, o.w};

    if (!g_have_ref) {
        // Yaw-only reference: a full-pose reference would bake the headset's
        // tilt at capture time into the world. Require TRACKED, not just
        // VALID, so a runtime filling in a guess cannot become the reference.
        if (!(flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) return;
        // Head forward in LOCAL space is q rotating (0,0,-1).
        const float fx = -2.0f * (q.x * q.z + q.w * q.y);
        const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        // Nearly straight up or down: the horizontal projection is too small
        // to define a yaw. Wait for a usable pose.
        if (fx * fx + fz * fz < 0.04f) return;
        const float psi = atan2f(-fx, -fz);   // yaw about XR +Y
        g_ref_inv = Quat{0.0f, -sinf(psi * 0.5f), 0.0f, cosf(psi * 0.5f)};
        g_have_ref = true;
        headpose::note_recenter();   // BUILD 261
        note("VR: head reference captured, yaw %.1f deg. Head tracking live.",
             psi * 57.29578f);
        // BUILD 207: the 6DOF position reference is captured HERE, with the
        // yaw reference, so every recenter (Home, Space, F2, the runtime's
        // own) re-zeroes the origin and restarts the standing average and
        // the bone maximum. The 204 block's own capture never saw a recenter
        // because this function had re-armed g_have_ref first (the internal
        // dig, 5.3). g_head_pos is stored before this call in the same
        // present; if the position is not valid yet, the 204 block captures
        // it on the first present that has one.
        if (g_head_pos_ok) {
            g_head_ref_pos[0] = g_head_pos[0];
            g_head_ref_pos[1] = g_head_pos[1];
            g_head_ref_pos[2] = g_head_pos[2];
            g_head_ref_ok = true;
            g_hp_refs.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_head_ref_ok = false;
        }
        g_st_ref_ms = GetTickCount64();
        g_st_dzmax = 0.0f; g_st_auto_sum = 0.0; g_st_auto_n = 0; g_st_auto_done = false;
    }

    // Head rotation relative to the reference, expressed in the REFERENCE
    // frame: H = ref^-1 * current. The camera side applies it in the camera's
    // own frame (rows = H * base), so the reference frame must be identified
    // with the camera frame. The other order, current * ref^-1, is the same
    // rotation expressed in LOCAL space; composed camera-locally it pitches
    // and rolls about axes yawed away from the camera's whenever the head is
    // turned away from the reference (the "world tilts sideways" trap in
    // docs/HANDOFF.md).
    const Quat rel = qmul(g_ref_inv, q);

    float Rr[9];
    quat_to_rows(rel, Rr);

    // Change of basis into the game's axes. XR view axes: x right, y up,
    // z backward. Game (RE-notes.md, Ansel): x right, y forward, z up.
    // H_game = A * Rr * A^T with A rows (1,0,0),(0,0,-1),(0,1,0); as a signed
    // permutation that is H[i][j] = S[i] * S[j] * Rr[P[i]][P[j]].
    static const int   P[3] = {0, 2, 1};
    static const float S[3] = {1.0f, -1.0f, 1.0f};
    float Hg[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            Hg[i * 3 + j] = S[i] * S[j] * Rr[P[i] * 3 + P[j]];

    // Build 19: head yaw/pitch in the SAME geometric convention the camera
    // hook extracts from the engine base (forward row: yaw = atan2(fx, fy),
    // pitch = asin(fz)), feeding the aim injection pump.
    {
        float sp = Hg[5];
        if (sp >  0.9999f) sp =  0.9999f;
        if (sp < -0.9999f) sp = -0.9999f;
        const float head_ay = atan2f(Hg[3], Hg[4]);
        const float head_ap = asinf(sp);
        float ay = head_ay;
        float ap = head_ap;
        // Build 25: while the LEFT TRIGGER holds ADS, aim reverts to the
        // HEAD. The game draws its sight picture at view center (head), so
        // controller-driven aim during ADS splits picture from impacts
        // (user: "the picture moves with the head but not the gun"). Split
        // model: hip = controller ray with the dot, ADS = look-to-aim, both
        // internally coherent. Gun-anchored optics belong to the arms phase.
        const bool lt_ads =
            g_in_trig[0].load(std::memory_order_relaxed) >= 0.5f;
        // BUILD 237: is the scope UP? On the L3 latch that is the latch state;
        // with pip_toggle = 0 it is the held trigger, exactly as before 237.
        // Computed once here because both the aim source below and the disc
        // itself further down have to agree on it.
        const bool pip_up = g_pip_enable &&
            (camera::pip_toggle_enabled() ? (camera::pip_user_on() != 0) : lt_ads);
        // Build 23: controller-driven hip-fire. Same reference (g_ref_inv),
        // same basis change, same yaw/pitch convention as the head above, so
        // the two sources are interchangeable mid-run (cfg aim_source, hot-
        // reloadable). An untracked controller HOLDS its last filtered ray
        // (no snap); one that never tracked leaves the head in charge.
        // BUILD 221: with the PiP scope on, the pull opens the disc and the
        // controller ray stays the aim; look-to-aim during ADS is off.
        // BUILD 237: with the scope on a latch the left trigger reaches the
        // game again, so a real ADS is possible for the first time since 221.
        // Look-to-aim belongs to that real ADS (the game draws its sight
        // picture at view centre); the controller ray keeps the aim only while
        // the SCOPE is up, which is what 221 actually meant.
        // BUILD 238: NATIVE VR ADS adds the third case. Build 25's revert to the
        // head exists because our camera write made the scene follow the head
        // while the game drew its sight picture at its own aim; inside the
        // flat-scope path that write is skipped, so there is one camera and no
        // split, and the rifle can pan the game's own magnified picture. The
        // condition is the LATCHED state, so it is false everywhere the mode is
        // off and everywhere the flat-scope path is not actually engaged.
        if (g_aim_source.load(std::memory_order_relaxed) == 1 &&
            (!lt_ads || pip_up || camera::scope_native_aim())) {
            const Quat c{g_in_ori[1][0].load(std::memory_order_relaxed),
                         g_in_ori[1][1].load(std::memory_order_relaxed),
                         g_in_ori[1][2].load(std::memory_order_relaxed),
                         g_in_ori[1][3].load(std::memory_order_relaxed)};
            if ((g_in_seen.load(std::memory_order_relaxed) & 2u) &&
                (c.x != 0.0f || c.y != 0.0f || c.z != 0.0f || c.w != 0.0f)) {
                const Quat rc = qmul(g_ref_inv, c);
                float Rc[9];
                quat_to_rows(rc, Rc);
                float Cg[9];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        Cg[i * 3 + j] = S[i] * S[j] * Rc[P[i] * 3 + P[j]];
                float cs = Cg[5];
                if (cs >  0.9999f) cs =  0.9999f;
                if (cs < -0.9999f) cs = -0.9999f;
                const float cp = asinf(cs);
                // Build 23.1: a near-vertical ray has no usable yaw (the
                // horizontal projection vanishes and atan2 returns noise:
                // the same gimbal guard the head reference capture has).
                // A dangling or holstered controller points steeply down,
                // so without this gate the filter chased garbage yaw and
                // dragged the aim hard off. Yaw only updates while the ray
                // is at least ~18 deg off vertical; pitch is always sound.
                const float h2 = Cg[3] * Cg[3] + Cg[4] * Cg[4];
                const bool  yaw_ok = h2 > 0.10f;
                const float cy = yaw_ok ? atan2f(Cg[3], Cg[4]) : g_ctl_yaw;
                if (!g_ctl_have) {
                    if (yaw_ok) {
                        g_ctl_yaw   = cy;
                        g_ctl_pitch = cp;
                        g_ctl_have  = true;
                    }
                } else {
                    const float a = g_ctl_smooth;
                    if (yaw_ok)
                        g_ctl_yaw = wrap_pi(g_ctl_yaw + a * wrap_pi(cy - g_ctl_yaw));
                    g_ctl_pitch = g_ctl_pitch + a * (cp - g_ctl_pitch);
                }
                // Diagnostic rider (drain thread logs it): filtered
                // controller aim vs head, and the yaw-usability verdict.
                g_ctl_diag[0].store(g_ctl_yaw,  std::memory_order_relaxed);
                g_ctl_diag[1].store(g_ctl_pitch, std::memory_order_relaxed);
                g_ctl_diag[2].store(ay, std::memory_order_relaxed);
                g_ctl_diag[3].store(ap, std::memory_order_relaxed);
                g_ctl_diag_ok.store(yaw_ok, std::memory_order_relaxed);
            }
            if (g_ctl_have) { ay = g_ctl_yaw; ap = g_ctl_pitch; }
        }

        // -------------------------------------------------------------------
        // BUILD 84: aim_barrel. See the state block at the top of the file for
        // why this is allowed where aim_source = 1 was not.
        //
        // While the RIGHT TRIGGER is held, the pump's target stops being "where
        // the head points" and becomes "where the head points, plus however far
        // the barrel is off the engine's current aim". Once the engine absorbs
        // that error its aim IS the barrel, the error reads zero, and the term
        // vanishes. Release the trigger and the target is the plain head angle
        // again, so nothing is left driving.
        //
        // Both vectors are ENGINE WORLD SPACE and the difference of two angles
        // in one frame is frame-independent, which is why this can be added to
        // ay/ap even though those live in the recentered reference frame. Doing
        // it as an absolute would have required mapping the barrel into that
        // frame, which is the kind of basis surgery that has cost this project
        // whole sessions.
        // -------------------------------------------------------------------
        // MODE 1 fires the loop on the trigger. MODE 2 runs it always.
        //
        // Mode 2 is what "hip fire follows the barrel like a real gun" actually
        // requires, and mode 1 cannot be tuned into it. The loop needs a few
        // frames to converge, and the pump can only inject while the engine is
        // touching its own aim code (hazard 45, parasitic by design). So on
        // mode 1 the FIRST round of a pull leaves before the aim has become the
        // barrel, and for a semi-automatic weapon the first round is the whole
        // shot. A real weapon is already pointing where it points before the
        // trigger breaks; it does not start aiming when you pull. Mode 2 keeps
        // the engine's aim standing on the barrel continuously, so every round
        // leaves correct with no convergence latency at all.
        //
        // THE STANDING CONSTRAINT (2026-08-05) FORBIDS CONTINUOUS AIM CHASING,
        // and this has to be argued rather than waved past. What it forbids is
        // "the camera drifts toward my hand while I am doing nothing", which is
        // a statement about the VIEW. Build 19's compose removes the injected
        // aim before composing the head, so the view is meant to stay put. If
        // it does, mode 2 breaks no constraint: the body turns, the view does
        // not. If it does not, then mode 1 is equally broken and merely hides
        // it behind the trigger. Mode 2 is therefore the honest test of the
        // architecture and mode 1 is the fallback, which is why both ship.
        //
        // Mode 2 requires the REAL barrel and will not run off the ctrl_ray
        // fallback. Continuously chasing a raw controller ray with no weapon
        // under it is build 43, which was rejected outright.
        const int bmode = g_aim_barrel.load(std::memory_order_relaxed);
        const bool bar_fire =
            aimtrace::firing() ||
            g_fire_lmb.load(std::memory_order_relaxed) ||
            g_in_trig[1].load(std::memory_order_relaxed) >= 0.90f;
        g_bar_want.store(bmode == 2 || (bmode == 1 && bar_fire),
                         std::memory_order_relaxed);
        if (bmode != 0 && (bmode == 2 || bar_fire)) {
            float bd[3], vf[3];
            // barrel_dir is fresh-or-nothing (it returns false unless the
            // weapon writer has run since the last read), so with wgun = 0 this
            // falls through to the raw controller ray rather than aiming at a
            // barrel direction that stopped updating minutes ago. Holstering
            // stops the publisher too, which is what stands mode 2 down.
            int src = 0;
            if (aimtrace::barrel_dir(bd))                  src = 1;
            else if (bmode != 2 && aimtrace::ctrl_ray(bd)) src = 2;
            if (!src) {
                g_bar_nodir.fetch_add(1, std::memory_order_relaxed);
            } else if (!aimtrace::view_fwd(vf)) {
                g_bar_noview.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Same convention both sides: x right, y forward, z up, so
                // yaw = atan2(fx, fy) and pitch = asin(fz). Identical to the
                // extraction the camera hook does on the engine base, which is
                // what makes the two comparable at all.
                float bz = bd[2], vz = vf[2];
                if (bz >  0.9999f) bz =  0.9999f;
                if (bz < -0.9999f) bz = -0.9999f;
                if (vz >  0.9999f) vz =  0.9999f;
                if (vz < -0.9999f) vz = -0.9999f;
                float ey = wrap_pi(atan2f(bd[0], bd[1]) - atan2f(vf[0], vf[1]));
                float ep = asinf(bz) - asinf(vz);

                // Build 88: CLAMP THE ERROR, DO NOT REFUSE THE FRAME.
                //
                // Build 84 dropped any frame whose error exceeded the cap, on
                // the reasoning that a tracking pop must not spin the
                // character. The 2026-08-14 run showed why that is wrong in
                // continuous mode: overcap=1047 against frames=646, so MORE
                // frames were thrown away than used. Pointing the gun a long
                // way off where you are looking is the whole point of the
                // feature, not a fault, and it produces exactly the large
                // errors the cap was rejecting. Worse, refusing is
                // self-defeating: the error stays large, so the next frame is
                // refused too, and it never converges.
                //
                // Clamping keeps the safety property that matters (no single
                // frame can swing the aim further than the cap) while still
                // walking a genuine large divergence down over a few frames.
                // The pump's own 0.30 rad per-injection limit still applies on
                // top. Nonsense input is still refused outright, but by a NaN
                // check rather than by magnitude, because magnitude cannot
                // distinguish a tracking pop from a deliberate wide hold.
                const float cap = g_aim_barrel_max.load(std::memory_order_relaxed);
                if (!(ey == ey) || !(ep == ep)) {
                    g_bar_nodir.fetch_add(1, std::memory_order_relaxed);
                } else {
                    if (ey >  cap) { ey =  cap; g_bar_clamp.fetch_add(1, std::memory_order_relaxed); }
                    if (ey < -cap) { ey = -cap; g_bar_clamp.fetch_add(1, std::memory_order_relaxed); }
                    if (ep >  cap) { ep =  cap; g_bar_clamp.fetch_add(1, std::memory_order_relaxed); }
                    if (ep < -cap) { ep = -cap; g_bar_clamp.fetch_add(1, std::memory_order_relaxed); }
                    ay = wrap_pi(ay + ey);
                    ap = ap + ep;
                    g_rcx_barrel = asinf(bz);   // BUILD 292 (log only)
                    g_rcx_view   = asinf(vz);
                    g_bar_frames.fetch_add(1, std::memory_order_relaxed);
                    g_bar_diag[0].store(ey, std::memory_order_relaxed);
                    g_bar_diag[1].store(ep, std::memory_order_relaxed);
                    g_bar_src.store(src, std::memory_order_relaxed);
                }
            }
        }

        g_aimref_head.store(ay, std::memory_order_relaxed);
        aim_pump(ay, ap);

        // Build 24: place the aim reticle at the controller ray's offset
        // from the view center (both angles live in the same recentered
        // frame). Hidden when aim is off, the source is the head (the dot
        // would sit dead center), the ray is behind the view cone, or the
        // fov gate says optics own the screen.
        g_dot_on = false;
        if (g_dot_enable && g_vraim_on && g_ctl_have && !lt_ads &&
            g_aim_source.load(std::memory_order_relaxed) == 1 &&
            headpose::read_fov(0.7853982f) >= 0.65f) {
            const float dy = wrap_pi(g_ctl_yaw - head_ay);
            const float dp = g_ctl_pitch - head_ap;
            if (fabsf(dy) < 1.2f && fabsf(dp) < 1.2f) {
                g_dot_tx = tanf(dy);
                g_dot_ty = tanf(dp);
                g_dot_on = true;
            }
        }

        // BUILD 42: hand markers. Deliberately independent of the aim
        // reference and of g_ctl_yaw: both the head and the controllers come
        // from the SAME xrLocateSpace pass in reference space, so a stale or
        // wrong recenter cannot move these. That is what makes them a valid
        // instrument rather than another thing to calibrate.
        g_hand_on[0] = g_hand_on[1] = false;
        if (g_hand_enable && g_head_pos_ok &&
            headpose::read_fov(0.7853982f) >= 0.65f) {   // hazard 25: not in optics
            const float half_ipd =
                0.5f * headpose::read_ipd(0.063f);
            for (int h = 0; h < 2; ++h) {
                if (!g_in_track[h].load(std::memory_order_relaxed)) continue;
                const float rel[3] = {
                    g_in_pos[h][0].load(std::memory_order_relaxed) - g_head_pos[0],
                    g_in_pos[h][1].load(std::memory_order_relaxed) - g_head_pos[1],
                    g_in_pos[h][2].load(std::memory_order_relaxed) - g_head_pos[2]};
                float lv[3];
                quat_rotate_inv(q, rel, lv);      // head-local: +x right, +y up, -z fwd
                bool any = false;
                for (int e = 0; e < 2; ++e) {
                    // Eye offset is a pure x shift in head-local space, so the
                    // markers carry real stereo disparity. Without this they
                    // would sit at optical infinity and fuse at the wrong
                    // depth, which reads as "floating markers", not hands.
                    const float ex = lv[0] - (e == 0 ? -half_ipd : half_ipd);
                    const float fwd = -lv[2];
                    if (fwd < 0.08f) continue;    // at or behind the eye
                    const float tx = ex / fwd, ty = lv[1] / fwd;
                    if (tx < -3.0f || tx > 3.0f || ty < -3.0f || ty > 3.0f) continue;
                    float tr = 0.045f / fwd;      // ~9 cm blob, shrinks with range
                    if (tr > 0.6f) tr = 0.6f;     // hand against the lens
                    if (tr < 0.004f) tr = 0.004f;
                    g_hand_tx[h][e] = tx;
                    g_hand_ty[h][e] = ty;
                    g_hand_tr[h][e] = tr;
                    any = true;
                }
                g_hand_on[h] = any;
            }
        }

        // BUILD 221: the PiP scope. Shown while the left trigger is pulled and
        // the anchor has an eyepiece; placed like the weapon markers below
        // (engine world through base_frame, per-eye IPD shift). The source
        // centre is where the BARREL points, from the barrel publish the
        // green ring consumes (camera axes: x right, y forward, z up), so the
        // disc is honest about the shot under every aim_source; the
        // controller-ray angles are never computed under aim_source 0
        // (review 2 of build 221). Same optics gate as the rings.
        g_pip_show = false;
        // BUILD 237: the disc follows the L3 latch (pip_up, computed above).
        if (pip_up &&
            headpose::read_fov(0.7853982f) >= 0.65f) {
            float lens[3], R[9], C[3], bl[3];
            uint32_t bgen = 0;
            if (camera::pip_lens(lens) && camera::base_frame(R, C) &&
                aimtrace::barrel_local_peek(bl, &bgen) && bgen != 0 &&
                bl[1] > 0.05f) {
                // BUILD 222: C is the CENTRE viewpoint (the latch leaves the eye
                // offset out on purpose); each eye's frame was rendered from
                // C + eye_shift(e) along the camera's right axis, so the disc
                // for eye e is projected from exactly there. The 221 run had it
                // one disc-width left of the glass with the marker convention.
                const float rw[3] = {lens[0] - C[0], lens[1] - C[1], lens[2] - C[2]};
                const float lx = rw[0] * R[0] + rw[1] * R[1] + rw[2] * R[2];
                const float lf = rw[0] * R[3] + rw[1] * R[4] + rw[2] * R[5];
                const float lu = rw[0] * R[6] + rw[1] * R[7] + rw[2] * R[8];
                // The barrel's tangent in the frame: right over forward, up over
                // forward, the same convention as lx/lf/lu just above.
                const float atx = bl[0] / bl[1], aty = bl[2] / bl[1];
                if (lf > 0.05f && fabsf(atx) < 2.5f && fabsf(aty) < 2.5f) {
                    bool any = false;
                    for (int e = 0; e < 2; ++e) {
                        const float ex = lx - camera::eye_shift(e);
                        const float tx = ex / lf, ty = lu / lf;
                        if (tx < -3.0f || tx > 3.0f || ty < -3.0f || ty > 3.0f) continue;
                        float tr = g_pip_radius / lf;
                        if (tr > 0.60f)  tr = 0.60f;
                        if (tr < 0.010f) tr = 0.010f;
                        g_pip_tx[e] = tx; g_pip_ty[e] = ty; g_pip_tr[e] = tr;
                        any = true;
                    }
                    if (any) {
                        g_pip_atx = atx;
                        g_pip_aty = aty;
                        g_pip_show = true;
                    }
                }
            }
        }

        // BUILD 45: weapon-candidate markers. Unlike the hand markers these
        // are placed from ENGINE world positions, so the conversion uses the
        // GAME CAMERA's own basis (base_frame: row 0 right, row 1 forward,
        // row 2 up, world z up, metres), not the XR head pose. That means a
        // marker lands where the object appears IN THE GAME FRAME, which is
        // exactly right for identification; during fast head motion it lags
        // the content by the same reprojection offset the content itself
        // carries, which is acceptable for a "which colour is on the gun"
        // read. Same optics gate as the hands (hazard 25).
        for (int i = 0; i < kWpm; ++i) g_wpm_on[i] = false;
        if ((g_wpm_enable || g_wskel_enable) &&
            headpose::read_fov(0.7853982f) >= 0.65f) {
            float R[9], C[3];
            if (camera::base_frame(R, C)) {
                const float half_ipd = 0.5f * headpose::read_ipd(0.063f);
                for (int i = 0; i < kWpm; ++i) {
                    float wpos[3];
                    // Build 64: while wskel is armed the WHITE slot carries
                    // the weapon-skeleton pick; the wp handles keep the
                    // other five slots (and all six when it is off).
                    if (i == kWpm - 1 && g_wskel_enable && g_wskel_marker) {
                        if (!camera::wskel_marker(wpos)) continue;
                    } else if (!g_wpm_enable || !wp::marker(i, wpos)) {
                        continue;
                    }
                    const float rw[3] = {wpos[0] - C[0], wpos[1] - C[1],
                                         wpos[2] - C[2]};
                    const float lx = rw[0] * R[0] + rw[1] * R[1] + rw[2] * R[2];
                    const float lf = rw[0] * R[3] + rw[1] * R[4] + rw[2] * R[5];
                    const float lu = rw[0] * R[6] + rw[1] * R[7] + rw[2] * R[8];
                    if (lf < 0.15f) continue;   // at or behind the camera
                    bool any = false;
                    for (int e = 0; e < 2; ++e) {
                        const float ex = lx - (e == 0 ? -half_ipd : half_ipd);
                        const float tx = ex / lf, ty = lu / lf;
                        if (tx < -3.0f || tx > 3.0f || ty < -3.0f || ty > 3.0f)
                            continue;
                        float tr = 0.030f / lf;   // ~6 cm blob at the object
                        if (tr > 0.5f)   tr = 0.5f;
                        if (tr < 0.004f) tr = 0.004f;
                        g_wpm_tx[i][e] = tx;
                        g_wpm_ty[i][e] = ty;
                        g_wpm_tr[i][e] = tr;
                        any = true;
                    }
                    g_wpm_on[i] = any;
                }
            }
        }

        // BUILD 153: the evidence reticle. Same conversion as the wp markers
        // above (game camera basis, per-eye tangents), so it inherits their
        // headset-proven placement rather than inventing a second one.
        //
        // barrel_dir_PEEK, never barrel_dir. The plain accessor keeps its
        // freshness memory in one function-static and is SINGLE CONSUMER: a
        // second caller is answered "stale" whenever the frame-rate consumer
        // arrives first, which is a build that quietly does nothing for a
        // reason no log line explains. That trap is already recorded in
        // FIREARM section 3 and peek exists precisely for this.
        g_ret_on[0] = g_ret_on[1] = false;
        if (g_ret_mode > 0 && headpose::read_fov(0.7853982f) >= 0.65f) {
            float R[9], C[3];
            if (camera::base_frame(R, C)) {
                const float half_ipd = 0.5f * headpose::read_ipd(0.063f);
                float wpos[kRet][3];
                bool  have[kRet] = {false, false};

                // BOTH rings originate at the CAMERA, and that is a fix, not
                // a shortcut. 2026-09-02, owner: "the gun following one gets
                // turned around if the character moves, it will end up behind
                // the player." He was right and the cause is freshness:
                // aimtrace::player_muzzle reads g_sd_muz, which is published
                // from the MUZZLE GETTER HOOK, and the engine only calls that
                // when it wants a muzzle point, which in practice means while
                // shooting. Between shots the value is a STALE WORLD POSITION,
                // so a ring drawn from it stays pinned to a dead point and the
                // player walks away from it. The direction was live the whole
                // time; the origin was not.
                //
                // Drawing both from base_frame's camera position makes them
                // live by construction, and it is also the better instrument:
                // same origin and same distance means the on-screen gap is
                // EXACTLY the angle between the barrel and the view, which is
                // the rayprobe's raw-vs-cam number and the thing being
                // measured. The muzzle is about 0.6 m off the camera, so at a
                // sane range this costs a couple of degrees of parallax on a
                // mark that was never an impact predictor anyway.
                // 153b: consume the CAMERA-AXES publish, so the barrel ring
                // needs no conversion here at all and cannot pick up a basis
                // from a different instant than the one it was built with.
                // x right, y forward, z up. Placed as a world point below
                // purely so both rings share one code path.
                float bl[3];
                uint32_t bgen = 0;
                if (aimtrace::barrel_local_peek(bl, &bgen) && bgen != 0 &&
                    bl[1] > 0.05f) {          // in front of the camera
                    for (int k = 0; k < 3; ++k)
                        wpos[0][k] = C[k] + (bl[0] * R[k]
                                           + bl[1] * R[3 + k]
                                           + bl[2] * R[6 + k]) * g_ret_dist;
                    have[0] = true;
                }
                if (g_ret_mode >= 2) {
                    // Camera forward is base_frame row 1 (row 0 right,
                    // row 1 forward, row 2 up), the same convention the
                    // markers above swizzle through.
                    for (int k = 0; k < 3; ++k)
                        wpos[1][k] = C[k] + R[3 + k] * g_ret_dist;
                    have[1] = true;
                }

                for (int i = 0; i < kRet; ++i) {
                    if (!have[i]) continue;
                    const float rw[3] = {wpos[i][0] - C[0], wpos[i][1] - C[1],
                                         wpos[i][2] - C[2]};
                    const float lx = rw[0] * R[0] + rw[1] * R[1] + rw[2] * R[2];
                    const float lf = rw[0] * R[3] + rw[1] * R[4] + rw[2] * R[5];
                    const float lu = rw[0] * R[6] + rw[1] * R[7] + rw[2] * R[8];
                    if (lf < 0.15f) continue;   // at or behind the camera
                    bool any = false;
                    for (int e = 0; e < 2; ++e) {
                        const float ex = lx - (e == 0 ? -half_ipd : half_ipd);
                        const float tx = ex / lf, ty = lu / lf;
                        if (tx < -3.0f || tx > 3.0f || ty < -3.0f || ty > 3.0f)
                            continue;
                        g_ret_tx[i][e] = tx;
                        g_ret_ty[i][e] = ty;
                        any = true;
                    }
                    g_ret_on[i] = any;
                }
            }
        }

        // BUILD 47: push the write target and the right controller's
        // ENGINE-WORLD position to wp:: once per frame. World position =
        // camera base + the same head-local offset the hand markers verified
        // in the headset (build 42), mapped onto the game basis (base_frame:
        // row 0 right, row 1 forward, row 2 up). The offset is clamped to
        // 2 m from the camera (skeleton rule 5: every controller-driven
        // transform gets a clamp before it reaches the game). When the
        // target slot is -1 the push still happens so wp:: sees the OFF
        // immediately, and a dead controller pushes ctrl_ok=false, which
        // mode 2 treats as "no write this call".
        {
            float cw[3] = {};
            bool  cok   = false;
            float Rw[9], Cw[3];
            if ((g_wpw_handle || g_wpw_slot >= 0) && g_head_pos_ok &&
                g_in_track[1].load(std::memory_order_relaxed) &&
                camera::base_frame(Rw, Cw)) {
                const float rel[3] = {
                    g_in_pos[1][0].load(std::memory_order_relaxed) - g_head_pos[0],
                    g_in_pos[1][1].load(std::memory_order_relaxed) - g_head_pos[1],
                    g_in_pos[1][2].load(std::memory_order_relaxed) - g_head_pos[2]};
                float lv[3];
                quat_rotate_inv(q, rel, lv);   // +x right, +y up, -z forward
                float lx = lv[0], lf = -lv[2], lu = lv[1];
                const float m2 = lx * lx + lf * lf + lu * lu;
                if (m2 > 4.0f) {
                    const float s = 2.0f / sqrtf(m2);
                    lx *= s; lf *= s; lu *= s;
                }
                cw[0] = Cw[0] + lx * Rw[0] + lf * Rw[3] + lu * Rw[6];
                cw[1] = Cw[1] + lx * Rw[1] + lf * Rw[4] + lu * Rw[7];
                cw[2] = Cw[2] + lx * Rw[2] + lf * Rw[5] + lu * Rw[8];
                cok = true;
            }
            wp::set_write(g_wpw_handle, g_wpw_slot, g_wpw_mode, g_wpw_up,
                          cw, cok);
        }

        // BUILD 62: publish the right controller's aim DIRECTION in engine
        // world space for the bullet relocation. Same route as the build 47
        // position write just above and the same reason it is trustworthy:
        // head and controller poses come from one xrLocateSpace pass, the
        // head-local step is the hand-marker math verified in the headset,
        // and the world step is the camera's own basis. The aim pose's
        // forward is -Z; rotate-by-q is rotate-inverse-by-conjugate, so no
        // new quaternion helper is needed.
        {
            float dw[3] = {};
            bool  dok   = false;
            float Rw[9], Cw[3];
            if (g_head_pos_ok &&
                g_in_track[1].load(std::memory_order_relaxed) &&
                camera::base_frame(Rw, Cw)) {
                const Quat c{g_in_ori[1][0].load(std::memory_order_relaxed),
                             g_in_ori[1][1].load(std::memory_order_relaxed),
                             g_in_ori[1][2].load(std::memory_order_relaxed),
                             g_in_ori[1][3].load(std::memory_order_relaxed)};
                if (c.x != 0.0f || c.y != 0.0f || c.z != 0.0f || c.w != 0.0f) {
                    const float fwd_ref_in[3] = {0.0f, 0.0f, -1.0f};
                    const Quat cconj{-c.x, -c.y, -c.z, c.w};
                    float fr[3];
                    quat_rotate_inv(cconj, fwd_ref_in, fr);  // ctrl fwd, ref space
                    float lv[3];
                    quat_rotate_inv(q, fr, lv);   // head-local: +x r, +y u, -z fwd
                    const float lx = lv[0], lf = -lv[2], lu = lv[1];
                    dw[0] = lx * Rw[0] + lf * Rw[3] + lu * Rw[6];
                    dw[1] = lx * Rw[1] + lf * Rw[4] + lu * Rw[7];
                    dw[2] = lx * Rw[2] + lf * Rw[5] + lu * Rw[8];
                    const float m = sqrtf(dw[0] * dw[0] + dw[1] * dw[1] +
                                          dw[2] * dw[2]);
                    if (m > 0.5f) {   // basis rows are unit, so ~1 expected
                        dw[0] /= m; dw[1] /= m; dw[2] /= m;
                        dok = true;
                    }
                }
            }
            aimtrace::set_ctrl_ray(dw, dok);

            // BUILD 81: the controller's POSITION, same pass, same basis, so
            // the gun's origin and its barrel can never come from different
            // instants. The head-local offset is the hand-marker route
            // verified in the headset (build 42, "tracks incredibly well");
            // the world step adds the game camera's own origin, which in
            // first person sits on the head bone, so hand-relative-to-head
            // becomes hand-in-the-world with no extra calibration.
            {
                float pw[3] = {};
                bool  pok   = false;
                float Rp[9], Cp[3];
                if (g_head_pos_ok &&
                    g_in_track[1].load(std::memory_order_relaxed) &&
                    camera::base_frame(Rp, Cp)) {
                    camera::apply_ctrl_lead(Cp);   // BUILD 201: the frame that draws this hand
                    const float crel[3] = {
                        g_in_pos[1][0].load(std::memory_order_relaxed) -
                            g_head_pos[0],
                        g_in_pos[1][1].load(std::memory_order_relaxed) -
                            g_head_pos[1],
                        g_in_pos[1][2].load(std::memory_order_relaxed) -
                            g_head_pos[2]};
                    float lv[3];
                    quat_rotate_inv(q, crel, lv);  // +x right, +y up, -z fwd
                    const float lx = lv[0], lf = -lv[2], lu = lv[1];
                    pw[0] = Cp[0] + lx * Rp[0] + lf * Rp[3] + lu * Rp[6];
                    pw[1] = Cp[1] + lx * Rp[1] + lf * Rp[4] + lu * Rp[7];
                    pw[2] = Cp[2] + lx * Rp[2] + lf * Rp[5] + lu * Rp[8];
                    pok = isfinite(pw[0]) && isfinite(pw[1]) && isfinite(pw[2]);
                    if (pok) { const float off[3] = {lx, lf, lu}; camera::gpair_note(1, pw, off, Cp); camera::note_pub_cam(0, Cp, pw); }   // BUILD 277 log only; BUILD 278/282 the camera this hand was placed from
                }
                aimtrace::set_ctrl_pos(pw, pok);
            }

            // 2026-08-13: the LEFT controller's position, for the two-handed
            // weapon. Deliberately a copy of the block above with the hand
            // index changed, rather than a shared helper: that block is the
            // headset-verified route (build 42's hand markers, build 81's
            // world step) and the cheapest way to be sure the left hand lands
            // in the same space as the right is to walk the identical path.
            {
                float pl[3] = {};
                bool  plok  = false;
                float Rl[9], Cl[3];
                if (g_head_pos_ok &&
                    g_in_track[0].load(std::memory_order_relaxed) &&
                    camera::base_frame(Rl, Cl)) {
                    camera::apply_ctrl_lead(Cl);   // BUILD 201: same lead, same instant
                    const float lrel[3] = {
                        g_in_pos[0][0].load(std::memory_order_relaxed) -
                            g_head_pos[0],
                        g_in_pos[0][1].load(std::memory_order_relaxed) -
                            g_head_pos[1],
                        g_in_pos[0][2].load(std::memory_order_relaxed) -
                            g_head_pos[2]};
                    float lv[3];
                    quat_rotate_inv(q, lrel, lv);  // +x right, +y up, -z fwd
                    const float lx = lv[0], lf = -lv[2], lu = lv[1];
                    pl[0] = Cl[0] + lx * Rl[0] + lf * Rl[3] + lu * Rl[6];
                    pl[1] = Cl[1] + lx * Rl[1] + lf * Rl[4] + lu * Rl[7];
                    pl[2] = Cl[2] + lx * Rl[2] + lf * Rl[5] + lu * Rl[8];
                    plok = isfinite(pl[0]) && isfinite(pl[1]) && isfinite(pl[2]);
                    if (plok) camera::note_pub_cam(1, Cl, pl);   // BUILD 278: the camera the left hand was placed from
                }
                aimtrace::set_ctrl_pos_l(pl, plok);
            }

            // BUILD 158, CX-017: the LEFT controller's forward and up, so the
            // free hand's wrist can follow the real wrist instead of freezing
            // in the animation's brace pose. Identical route to the right
            // controller's up vector below: local reference vector through the
            // controller quaternion, into head space, signed permutation, then
            // the engine base. Vectors only, never a quaternion, for the same
            // reason that block states.
            {
                float fw[3] = {}, uw2[3] = {};
                bool  vok = false;
                float Rv2[9], Cv2[3];
                if (g_head_pos_ok &&
                    g_in_track[0].load(std::memory_order_relaxed) &&
                    camera::base_frame(Rv2, Cv2)) {
                    const Quat c{g_in_ori[0][0].load(std::memory_order_relaxed),
                                 g_in_ori[0][1].load(std::memory_order_relaxed),
                                 g_in_ori[0][2].load(std::memory_order_relaxed),
                                 g_in_ori[0][3].load(std::memory_order_relaxed)};
                    if (c.x != 0.0f || c.y != 0.0f || c.z != 0.0f ||
                        c.w != 0.0f) {
                        const Quat cconj{-c.x, -c.y, -c.z, c.w};
                        const float refs[2][3] = {{0.0f, 0.0f, -1.0f},
                                                  {0.0f, 1.0f,  0.0f}};
                        float* outs[2] = {fw, uw2};
                        int good = 0;
                        for (int vi = 0; vi < 2; ++vi) {
                            float vr[3], lv[3];
                            quat_rotate_inv(cconj, refs[vi], vr);
                            quat_rotate_inv(q, vr, lv);
                            const float lx = lv[0], lf = -lv[2], lu = lv[1];
                            float* o = outs[vi];
                            o[0] = lx * Rv2[0] + lf * Rv2[3] + lu * Rv2[6];
                            o[1] = lx * Rv2[1] + lf * Rv2[4] + lu * Rv2[7];
                            o[2] = lx * Rv2[2] + lf * Rv2[5] + lu * Rv2[8];
                            const float m = sqrtf(o[0]*o[0] + o[1]*o[1] +
                                                  o[2]*o[2]);
                            if (m > 0.5f) {
                                o[0] /= m; o[1] /= m; o[2] /= m;
                                ++good;
                            }
                        }
                        vok = (good == 2);
                    }
                }
                aimtrace::set_ctrl_vecs_l(fw, uw2, vok);
            }

            // BUILD 162: the LEFT controller's GRIP-pose +Y and +Z, the
            // anatomical frame, through exactly the route the aim vectors
            // above take: controller quaternion, head space, the signed
            // permutation, the engine base. Two vectors, never the
            // quaternion, for the reason the ctrl_up block states.
            {
                float gy[3] = {}, gz[3] = {};
                bool  gok = false;
                float Rv3[9], Cv3[3];
                if (g_head_pos_ok &&
                    g_in_gtrack[0].load(std::memory_order_relaxed) &&
                    camera::base_frame(Rv3, Cv3)) {
                    const Quat c{g_in_gori[0][0].load(std::memory_order_relaxed),
                                 g_in_gori[0][1].load(std::memory_order_relaxed),
                                 g_in_gori[0][2].load(std::memory_order_relaxed),
                                 g_in_gori[0][3].load(std::memory_order_relaxed)};
                    if (c.x != 0.0f || c.y != 0.0f || c.z != 0.0f ||
                        c.w != 0.0f) {
                        const Quat cconj{-c.x, -c.y, -c.z, c.w};
                        const float refs[2][3] = {{0.0f, 1.0f, 0.0f},
                                                  {0.0f, 0.0f, 1.0f}};
                        float* outs[2] = {gy, gz};
                        int good = 0;
                        for (int vi = 0; vi < 2; ++vi) {
                            float vr[3], lv[3];
                            quat_rotate_inv(cconj, refs[vi], vr);
                            quat_rotate_inv(q, vr, lv);
                            const float lx = lv[0], lf = -lv[2], lu = lv[1];
                            float* o = outs[vi];
                            o[0] = lx * Rv3[0] + lf * Rv3[3] + lu * Rv3[6];
                            o[1] = lx * Rv3[1] + lf * Rv3[4] + lu * Rv3[7];
                            o[2] = lx * Rv3[2] + lf * Rv3[5] + lu * Rv3[8];
                            const float m = sqrtf(o[0]*o[0] + o[1]*o[1] +
                                                  o[2]*o[2]);
                            if (m > 0.5f) {
                                o[0] /= m; o[1] /= m; o[2] /= m;
                                ++good;
                            }
                        }
                        gok = (good == 2);
                    }
                }
                aimtrace::set_ctrl_gripf_l(gy, gz, gok);
            }

            // 2026-08-13: the RIGHT controller's UP vector, so the weapon can
            // carry wrist roll. Identical to the aim-ray block above except
            // that the reference vector is the controller's local +Y instead
            // of its local -Z. Publishing a second VECTOR rather than the
            // whole quaternion is deliberate: the basis change below is a
            // signed permutation that has been verified for vectors, and
            // re-expressing a quaternion under it would need its own
            // derivation and its own headset test to trust.
            {
                float uw[3] = {};
                bool  uok   = false;
                float Ru[9], Cu[3];
                if (g_head_pos_ok &&
                    g_in_track[1].load(std::memory_order_relaxed) &&
                    camera::base_frame(Ru, Cu)) {
                    const Quat c{g_in_ori[1][0].load(std::memory_order_relaxed),
                                 g_in_ori[1][1].load(std::memory_order_relaxed),
                                 g_in_ori[1][2].load(std::memory_order_relaxed),
                                 g_in_ori[1][3].load(std::memory_order_relaxed)};
                    if (c.x != 0.0f || c.y != 0.0f || c.z != 0.0f || c.w != 0.0f) {
                        const float up_ref_in[3] = {0.0f, 1.0f, 0.0f};
                        const Quat cconj{-c.x, -c.y, -c.z, c.w};
                        float ur[3];
                        quat_rotate_inv(cconj, up_ref_in, ur);
                        float lv[3];
                        quat_rotate_inv(q, ur, lv);
                        const float lx = lv[0], lf = -lv[2], lu = lv[1];
                        uw[0] = lx * Ru[0] + lf * Ru[3] + lu * Ru[6];
                        uw[1] = lx * Ru[1] + lf * Ru[4] + lu * Ru[7];
                        uw[2] = lx * Ru[2] + lf * Ru[5] + lu * Ru[8];
                        const float m = sqrtf(uw[0]*uw[0] + uw[1]*uw[1] +
                                              uw[2]*uw[2]);
                        if (m > 0.5f) {
                            uw[0] /= m; uw[1] /= m; uw[2] /= m;
                            uok = true;
                        }
                    }
                }
                aimtrace::set_ctrl_up(uw, uok);
            }
            // Build 72: the game's own aim direction, from the same basis and
            // the same instant. Rows of Rw are right/forward/up, so row 1 is
            // forward. Published even when the controller ray is unavailable,
            // because the two are consumed independently.
            {
                float Rv[9], Cv[3];
                if (camera::base_frame(Rv, Cv)) {
                    const float f[3] = {Rv[3], Rv[4], Rv[5]};
                    aimtrace::set_view_fwd(f, true);
                } else {
                    aimtrace::set_view_fwd(nullptr, false);
                }
            }
        }
    }

    // Build 13a: the absolute XR-space orientation travels with the rotation
    // so the camera hook can echo back exactly which head orientation each
    // built frame was composed with (render-pose submit, see HeadPose.h).
    const float q_xr[4] = {q.x, q.y, q.z, q.w};
    headpose::publish(Hg, q_xr);
}

// Acquire one image of the given eye's canvas swapchain, clear it black,
// place the content at the eye's angle-correct offset, release it. Render
// thread, no logging (rule 8). Returns false if the copy did not happen; the
// image is released either way so the swapchain cannot wedge on a leaked
// acquire.
bool copy_into(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    ph(kPhAcquireCopy, eye);
    if (XR_FAILED(xrAcquireSwapchainImage(g_swapchains[eye], &ai, &idx))) {
        ph(kPhDone);
        return false;
    }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = img_wait_timeout();
    bool copied = false;
    ph(kPhWaitCopy, eye);
    const XrResult wr = xrWaitSwapchainImage(g_swapchains[eye], &wi);
    ph(kPhDone);
    if (wr == XR_TIMEOUT_EXPIRED)
        g_img_to_copy.fetch_add(1, std::memory_order_relaxed);
    if (XR_SUCCEEDED(wr) && wr != XR_TIMEOUT_EXPIRED) {
        if (idx < g_rtv[eye].size() && g_rtv[eye][idx]) {
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            ctx->ClearRenderTargetView(g_rtv[eye][idx], black);
        }
        D3D11_BOX box{0, 0, 0, g_content_w, g_content_h, 1};
        ctx->CopySubresourceRegion(g_images[eye][idx].texture, 0,
                                   (UINT)g_dst_x[eye], (UINT)g_dst_y[eye], 0,
                                   src, 0, &box);
        copied = true;
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_swapchains[eye], &ri);
    return copied;
}

// --- BUILD 11a: scaling blit machinery -------------------------------------

// D3DCompile, resolved at init from the System32 copy explicitly, so a stray
// compiler DLL in the game directory can never be picked up. Prototype from
// d3dcompiler.h; the two pointer params we always pass null are untyped so
// the header is not needed.
typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*,
                                        void*, LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob**, ID3DBlob**);

// Everything the blit touches on the game's immediate context, saved before
// the draws and restored after (the standard injected-overlay pattern). Every
// Get* AddRefs what it returns, so restore Releases each saved pointer.
// Refcount traffic is atomic increments, not COM creation; rule 8 holds.
struct CtxState {
    ID3D11InputLayout*        il;
    D3D11_PRIMITIVE_TOPOLOGY  topo;
    ID3D11VertexShader*       vs; ID3D11ClassInstance* vsi[16]; UINT vsn;
    ID3D11PixelShader*        ps; ID3D11ClassInstance* psi[16]; UINT psn;
    ID3D11GeometryShader*     gs; ID3D11ClassInstance* gsi[16]; UINT gsn;
    ID3D11HullShader*         hs; ID3D11ClassInstance* hsi[16]; UINT hsn;
    ID3D11DomainShader*       ds; ID3D11ClassInstance* dsi[16]; UINT dsn;
    ID3D11ShaderResourceView* srv0;
    ID3D11SamplerState*       smp0;
    ID3D11Buffer*             cb0;    // BUILD 221: the PiP disc binds PS b0
    ID3D11RasterizerState*    rs;
    UINT                      nvp;
    D3D11_VIEWPORT            vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11BlendState*         bs; FLOAT bf[4]; UINT bmask;
    ID3D11DepthStencilState*  dss; UINT sref;
    ID3D11RenderTargetView*   rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView*   dsv;
    // 11a.1: the predicate. The engine leaves one set at Present time (GPU
    // occlusion culling), and a set predicate silently skips Draw calls while
    // leaving clears and copies alone: headset went black the moment the 11a
    // copy became a draw, desktop and tracking fine. Save it, clear it for
    // our draws, restore it.
    ID3D11Predicate*          pred; BOOL pred_val;
};

void ctx_save(ID3D11DeviceContext* c, CtxState& s) {
    s.vsn = s.psn = s.gsn = s.hsn = s.dsn = 16;
    c->IAGetInputLayout(&s.il);
    c->IAGetPrimitiveTopology(&s.topo);
    c->VSGetShader(&s.vs, s.vsi, &s.vsn);
    c->PSGetShader(&s.ps, s.psi, &s.psn);
    c->GSGetShader(&s.gs, s.gsi, &s.gsn);
    c->HSGetShader(&s.hs, s.hsi, &s.hsn);
    c->DSGetShader(&s.ds, s.dsi, &s.dsn);
    c->PSGetShaderResources(0, 1, &s.srv0);
    c->PSGetConstantBuffers(0, 1, &s.cb0);   // BUILD 221
    c->PSGetSamplers(0, 1, &s.smp0);
    c->RSGetState(&s.rs);
    s.nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&s.nvp, s.vp);
    c->OMGetBlendState(&s.bs, s.bf, &s.bmask);
    c->OMGetDepthStencilState(&s.dss, &s.sref);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
    c->GetPredication(&s.pred, &s.pred_val);
}

void ctx_restore(ID3D11DeviceContext* c, CtxState& s) {
    c->IASetInputLayout(s.il);
    if (s.il) s.il->Release();
    c->IASetPrimitiveTopology(s.topo);
    c->VSSetShader(s.vs, s.vsi, s.vsn);
    if (s.vs) s.vs->Release();
    for (UINT i = 0; i < s.vsn; ++i) if (s.vsi[i]) s.vsi[i]->Release();
    c->PSSetShader(s.ps, s.psi, s.psn);
    if (s.ps) s.ps->Release();
    for (UINT i = 0; i < s.psn; ++i) if (s.psi[i]) s.psi[i]->Release();
    c->GSSetShader(s.gs, s.gsi, s.gsn);
    if (s.gs) s.gs->Release();
    for (UINT i = 0; i < s.gsn; ++i) if (s.gsi[i]) s.gsi[i]->Release();
    c->HSSetShader(s.hs, s.hsi, s.hsn);
    if (s.hs) s.hs->Release();
    for (UINT i = 0; i < s.hsn; ++i) if (s.hsi[i]) s.hsi[i]->Release();
    c->DSSetShader(s.ds, s.dsi, s.dsn);
    if (s.ds) s.ds->Release();
    for (UINT i = 0; i < s.dsn; ++i) if (s.dsi[i]) s.dsi[i]->Release();
    c->PSSetShaderResources(0, 1, &s.srv0);
    if (s.srv0) s.srv0->Release();
    c->PSSetConstantBuffers(0, 1, &s.cb0);   // BUILD 221: the PiP disc's b0 undone
    if (s.cb0) s.cb0->Release();
    c->PSSetSamplers(0, 1, &s.smp0);
    if (s.smp0) s.smp0->Release();
    c->RSSetState(s.rs);
    if (s.rs) s.rs->Release();
    if (s.nvp) c->RSSetViewports(s.nvp, s.vp);
    c->OMSetBlendState(s.bs, s.bf, s.bmask);
    if (s.bs) s.bs->Release();
    c->OMSetDepthStencilState(s.dss, s.sref);
    if (s.dss) s.dss->Release();
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (s.rtv[i]) s.rtv[i]->Release();
    if (s.dsv) s.dsv->Release();
    c->SetPredication(s.pred, s.pred_val);
    if (s.pred) s.pred->Release();
}

// The pipeline state common to every blit this present: set once after
// ctx_save. GS/HS/DS are nulled defensively; a game-bound geometry or
// tessellation stage would mangle the triangle.
void blit_bind(ID3D11DeviceContext* c) {
    c->SetPredication(nullptr, FALSE);   // 11a.1: see CtxState.pred
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g_blit_vs, nullptr, 0);
    c->PSSetShader(g_blit_ps, nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->PSSetSamplers(0, 1, &g_blit_samp);
    c->RSSetState(g_blit_rs);
    const float bf[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    c->OMSetBlendState(nullptr, bf, 0xFFFFFFFFu);   // default: opaque
    c->OMSetDepthStencilState(g_blit_dss, 0);
}

// One-time device work at init (rule 8). Returns false on any failure, with
// the reason logged; the caller then leaves g_blit_ok false and the 10L
// fixed-copy path runs.
bool create_blit_resources(const d3d11::State& st) {
    wchar_t sys[MAX_PATH];
    const UINT sn = GetSystemDirectoryW(sys, MAX_PATH);
    if (!sn || sn >= MAX_PATH) return false;
    const std::wstring dll = std::wstring(sys) + L"\\d3dcompiler_47.dll";
    HMODULE h = LoadLibraryW(dll.c_str());
    if (!h) {
        LOG_ERROR("VR: 11a d3dcompiler_47.dll not found in System32; blit disabled.");
        return false;
    }
    auto compile = (PFN_D3DCompile)GetProcAddress(h, "D3DCompile");
    if (!compile) {
        LOG_ERROR("VR: 11a D3DCompile export missing; blit disabled.");
        return false;
    }

    // Fullscreen triangle: SV_VertexID 0/1/2 -> a triangle covering the
    // viewport with uv (0,0) at its top-left and (1,1) at its bottom-right.
    // The viewport IS the destination rectangle, so the shaders need no
    // parameters at all.
    static const char kSrc[] =
        "Texture2D    tex0 : register(t0);\n"
        "SamplerState smp0 : register(s0);\n"
        // BUILD 221: the PiP disc's source window: xy = the uv of the barrel's
        // aim point in the frame, zw = the half extent in uv at this zoom.
        "cbuffer PipCB : register(b0) { float4 pip_uv; };\n"
        "void vs_main(uint id : SV_VertexID, out float4 pos : SV_Position,\n"
        "             out float2 uv : TEXCOORD0) {\n"
        "    float2 t = float2((id << 1) & 2, id & 2);\n"
        "    pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);\n"
        "    uv  = t;\n"
        "}\n"
        "float4 ps_main(float4 pos : SV_Position,\n"
        "               float2 uv : TEXCOORD0) : SV_Target {\n"
        "    return tex0.Sample(smp0, uv);\n"
        "}\n"
        // Build 24: the aim dot. The viewport is the dot's bounding square;
        // discard outside the circle, darker rim for contrast on sky.
        "float4 ps_dot(float4 pos : SV_Position,\n"
        "              float2 uv : TEXCOORD0) : SV_Target {\n"
        "    float r = length(uv - 0.5) * 2.0;\n"
        "    if (r > 1.0) discard;\n"
        "    float3 col = float3(1.0, 0.30, 0.15);\n"
        "    if (r > 0.72) col *= 0.25;\n"
        "    return float4(col, 1.0);\n"
        "}\n"
        // Build 42: hand markers, left blue / right orange, bright rim
        // so they read against sky and against dark interiors.
        "float4 ps_hand_l(float4 pos : SV_Position,\n"
        "                 float2 uv : TEXCOORD0) : SV_Target {\n"
        "    float r = length(uv - 0.5) * 2.0;\n"
        "    if (r > 1.0) discard;\n"
        "    float3 col = float3(0.20, 0.55, 1.0);\n"
        "    if (r > 0.75) col = float3(0.85, 0.95, 1.0);\n"
        "    return float4(col, 1.0);\n"
        "}\n"
        "float4 ps_hand_r(float4 pos : SV_Position,\n"
        "                 float2 uv : TEXCOORD0) : SV_Target {\n"
        "    float r = length(uv - 0.5) * 2.0;\n"
        "    if (r > 1.0) discard;\n"
        "    float3 col = float3(1.0, 0.45, 0.10);\n"
        "    if (r > 0.75) col = float3(1.0, 0.90, 0.70);\n"
        "    return float4(col, 1.0);\n"
        "}\n"
        // Build 45: weapon-candidate markers. Shared body; each entry point
        // only sets its colour. Dark rim so every colour reads on both sky
        // and shadow, and so they cannot be mistaken for the rimless-bright
        // hand markers.
        "float4 wp_body(float2 uv, float3 col) {\n"
        "    float r = length(uv - 0.5) * 2.0;\n"
        "    if (r > 1.0) discard;\n"
        "    if (r > 0.70) col *= 0.20;\n"
        "    return float4(col, 1.0);\n"
        "}\n"
        "float4 ps_wp0(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "    { return wp_body(uv, float3(1.00, 0.12, 0.10)); }\n"   // RED
        "float4 ps_wp1(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "    { return wp_body(uv, float3(0.12, 0.95, 0.18)); }\n"   // GREEN
        "float4 ps_wp2(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "    { return wp_body(uv, float3(1.00, 0.92, 0.10)); }\n"   // YELLOW
        "float4 ps_wp3(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "    { return wp_body(uv, float3(1.00, 0.15, 0.95)); }\n"   // MAGENTA
        "float4 ps_wp4(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "    { return wp_body(uv, float3(0.10, 0.95, 1.00)); }\n"   // CYAN
        "float4 ps_wp5(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "    { return wp_body(uv, float3(1.00, 1.00, 1.00)); }\n"   // WHITE
        // BUILD 153: the evidence reticle. It was a centre dot plus a thin
        // ring with a hollow gap, so that it read as an aiming mark on a
        // recording and left the target visible through the middle.
        // BUILD 192, owner 2026-09-05: "make it a single green dot not a
        // circle dot". The ring is gone; the dot is half the quad across,
        // which at barrel_reticle_size 0.010 is about half a degree, a rifle
        // red dot. The same body serves the orange camera-forward mark when
        // barrel_reticle = 2 (off in his cfg).
        // BUILD 221: the PiP scope. The viewport is the eyepiece's bounding
        // square; inside the circle the frame is sampled about the aim point
        // at 1/zoom of the disc's own angular size, which is the magnification;
        // a dark rim reads as the tube, a small mark sits on the aim point.
        "float4 ps_pip(float4 pos : SV_Position,\n"
        "              float2 uv : TEXCOORD0) : SV_Target {\n"
        "    float2 d = uv - 0.5;\n"
        "    float  r = length(d) * 2.0;\n"
        "    if (r > 1.0) discard;\n"
        "    float2 suv = pip_uv.xy + d * 2.0 * pip_uv.zw;\n"
        "    float3 col = tex0.Sample(smp0, suv).rgb;\n"
        "    if (r > 0.94) col *= 0.15;\n"
        "    if (r < 0.030) col = float3(1.0, 0.25, 0.10);\n"
        "    return float4(col, 1.0);\n"
        "}\n"
        "float4 ret_body(float2 uv, float3 col) {\n"
        "    float r = length(uv - 0.5) * 2.0;\n"
        "    if (r < 0.50) return float4(col, 1.0);\n"
        "    discard;\n"
        "    return float4(0, 0, 0, 0);\n"
        "}\n"
        "float4 ps_ret0(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "    { return ret_body(uv, float3(0.10, 1.00, 0.25)); }\n"  // BARREL, green
        "float4 ps_ret1(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "    { return ret_body(uv, float3(1.00, 0.30, 0.10)); }\n"; // GAZE, orange

    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "grwxr_blit", nullptr, nullptr,
                         "vs_main", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) {
        LOG_ERROR("VR: 11a vertex shader compile failed (0x%08lX): %s",
                  (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }
    hr = compile(kSrc, sizeof(kSrc) - 1, "grwxr_blit", nullptr, nullptr,
                 "ps_main", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        LOG_ERROR("VR: 11a pixel shader compile failed (0x%08lX): %s",
                  (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        vsb->Release();
        return false;
    }
    if (err) err->Release();

    bool ok =
        SUCCEEDED(st.device->CreateVertexShader(vsb->GetBufferPointer(),
                                                vsb->GetBufferSize(), nullptr, &g_blit_vs)) &&
        SUCCEEDED(st.device->CreatePixelShader(psb->GetBufferPointer(),
                                               psb->GetBufferSize(), nullptr, &g_blit_ps));
    vsb->Release();
    psb->Release();

    if (ok) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(st.device->CreateSamplerState(&sd, &g_blit_samp));
    }
    if (ok) {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode        = D3D11_FILL_SOLID;
        rd.CullMode        = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ok = SUCCEEDED(st.device->CreateRasterizerState(&rd, &g_blit_rs));
    }
    if (ok) {
        D3D11_DEPTH_STENCIL_DESC dd{};   // depth and stencil both disabled
        ok = SUCCEEDED(st.device->CreateDepthStencilState(&dd, &g_blit_dss));
    }
    // BUILD 221: the PiP scope shader and its constant buffer, OPTIONAL like
    // the dot: a failure costs the disc, never the blit.
    if (ok) {
        ID3DBlob* pb = nullptr;
        ID3DBlob* pe = nullptr;
        if (SUCCEEDED(compile(kSrc, sizeof(kSrc) - 1, "grwxr_blit", nullptr,
                              nullptr, "ps_pip", "ps_4_0", 0, 0, &pb, &pe))) {
            if (FAILED(st.device->CreatePixelShader(pb->GetBufferPointer(),
                                                    pb->GetBufferSize(),
                                                    nullptr, &g_pip_ps)))
                LOG_WARN("VR: 221 pip shader creation failed, no PiP scope");
            pb->Release();
        } else {
            LOG_WARN("VR: 221 pip shader compile failed, no PiP scope: %s",
                     pe ? (const char*)pe->GetBufferPointer() : "?");
        }
        if (pe) pe->Release();
        if (g_pip_ps) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = 16;
            bd.Usage          = D3D11_USAGE_DEFAULT;
            bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(st.device->CreateBuffer(&bd, nullptr, &g_pip_cb))) {
                LOG_WARN("VR: 221 pip constant buffer failed, no PiP scope");
                g_pip_ps->Release(); g_pip_ps = nullptr;
            }
        }
    }
    if (ok) {
        // 2026-08-12: straight alpha blending, for the settings panel only.
        // Menu::render() clears its target to alpha 0.92, so the panel is
        // meant to sit slightly translucent over the game rather than punch a
        // hole in it. Every other draw in this file stays opaque.
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable           = TRUE;
        bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        // Optional, like the dot shader: losing it costs the panel, never the
        // blit, so it does not join the `ok` chain that gates VR itself.
        if (FAILED(st.device->CreateBlendState(&bd, &g_menu_blend))) {
            g_menu_blend = nullptr;
            LOG_WARN("VR: menu blend state creation failed; panel will be opaque.");
        }
    }
    if (!ok) {
        LOG_ERROR("VR: 11a blit state creation failed; blit disabled.");
        if (g_blit_dss)  { g_blit_dss->Release();  g_blit_dss  = nullptr; }
        if (g_blit_rs)   { g_blit_rs->Release();   g_blit_rs   = nullptr; }
        if (g_blit_samp) { g_blit_samp->Release(); g_blit_samp = nullptr; }
        if (g_blit_ps)   { g_blit_ps->Release();   g_blit_ps   = nullptr; }
        if (g_pip_ps)    { g_pip_ps->Release();    g_pip_ps    = nullptr; }   // BUILD 221
        if (g_pip_cb)    { g_pip_cb->Release();    g_pip_cb    = nullptr; }
        if (g_blit_vs)   { g_blit_vs->Release();   g_blit_vs   = nullptr; }
    }
    // Build 24: the dot shader is OPTIONAL: a failure only costs the
    // reticle, never the blit.
    if (ok) {
        ID3DBlob* db = nullptr;
        err = nullptr;
        if (SUCCEEDED(compile(kSrc, sizeof(kSrc) - 1, "grwxr_blit", nullptr,
                              nullptr, "ps_dot", "ps_4_0", 0, 0, &db, &err))) {
            if (FAILED(st.device->CreatePixelShader(db->GetBufferPointer(),
                                                    db->GetBufferSize(),
                                                    nullptr, &g_dot_ps)))
                LOG_WARN("VR: 24 dot shader creation failed, no aim reticle");
            db->Release();
        } else {
            LOG_WARN("VR: 24 dot shader compile failed, no aim reticle: %s",
                     err ? (const char*)err->GetBufferPointer() : "?");
        }
        if (err) err->Release();

        // Build 42: hand markers. Each shader is independent; a failure
        // costs that marker only, never the blit (rule: a probe must degrade,
        // not break the thing that works).
        {
            const char* hnames[2] = {"ps_hand_l", "ps_hand_r"};
            for (int h = 0; h < 2; ++h) {
                ID3DBlob* hb = nullptr;
                ID3DBlob* he = nullptr;
                if (SUCCEEDED(compile(kSrc, sizeof(kSrc) - 1, "grwxr_blit",
                                      nullptr, nullptr, hnames[h], "ps_4_0",
                                      0, 0, &hb, &he))) {
                    if (FAILED(st.device->CreatePixelShader(hb->GetBufferPointer(),
                                                            hb->GetBufferSize(),
                                                            nullptr, &g_hand_ps[h])))
                        LOG_WARN("VR: 42 hand shader %d creation failed", h);
                    hb->Release();
                } else {
                    LOG_WARN("VR: 42 hand shader %s compile failed: %s", hnames[h],
                             he ? (const char*)he->GetBufferPointer() : "?");
                }
                if (he) he->Release();
            }
            LOG_INFO("VR: 42 hand markers %s (left BLUE, right ORANGE). "
                     "They are drawn where the controllers actually are, "
                     "independent of the aim reference, so they stay correct "
                     "even if recentering is off. hand_markers=0 in "
                     "grwxr.cfg hides them.",
                     (g_hand_ps[0] && g_hand_ps[1]) ? "armed" : "PARTIALLY armed");
        }

        // Build 45: weapon-candidate marker shaders, same independence rule
        // as the hand markers: any single failure costs that colour only.
        {
            const char* wnames[kWpm] = {"ps_wp0", "ps_wp1", "ps_wp2",
                                        "ps_wp3", "ps_wp4", "ps_wp5"};
            int made = 0;
            for (int i = 0; i < kWpm; ++i) {
                ID3DBlob* wb = nullptr;
                ID3DBlob* we = nullptr;
                if (SUCCEEDED(compile(kSrc, sizeof(kSrc) - 1, "grwxr_blit",
                                      nullptr, nullptr, wnames[i], "ps_4_0",
                                      0, 0, &wb, &we))) {
                    if (SUCCEEDED(st.device->CreatePixelShader(
                            wb->GetBufferPointer(), wb->GetBufferSize(),
                            nullptr, &g_wpm_ps[i])))
                        ++made;
                    else
                        LOG_WARN("VR: 45 wp marker shader %d creation failed", i);
                    wb->Release();
                } else {
                    LOG_WARN("VR: 45 wp marker shader %s compile failed: %s",
                             wnames[i],
                             we ? (const char*)we->GetBufferPointer() : "?");
                }
                if (we) we->Release();
            }
            LOG_INFO("VR: 45 weapon-candidate markers armed (%d/%d shaders). "
                     "Colour order RED GREEN YELLOW MAGENTA CYAN WHITE; the "
                     "wpm: log line maps colours to placement handles each "
                     "second. Look at the gun and name the colour sitting on "
                     "it. wp_markers=0 in grwxr.cfg hides them.",
                     made, kWpm);
        }

        // BUILD 153: the evidence reticle shaders. Same independence rule
        // again: if one fails to compile that mark is simply absent, and the
        // build is still worth running with the other one.
        {
            const char* rnames[kRet] = {"ps_ret0", "ps_ret1"};
            int made = 0;
            for (int i = 0; i < kRet; ++i) {
                ID3DBlob* rb = nullptr;
                ID3DBlob* re = nullptr;
                if (SUCCEEDED(compile(kSrc, sizeof(kSrc) - 1, "grwxr_blit",
                                      nullptr, nullptr, rnames[i], "ps_4_0",
                                      0, 0, &rb, &re))) {
                    if (SUCCEEDED(st.device->CreatePixelShader(
                            rb->GetBufferPointer(), rb->GetBufferSize(),
                            nullptr, &g_ret_ps[i])))
                        ++made;
                    else
                        LOG_WARN("VR: 153 reticle shader %d creation failed", i);
                    rb->Release();
                } else {
                    LOG_WARN("VR: 153 reticle shader %s compile failed: %s",
                             rnames[i],
                             re ? (const char*)re->GetBufferPointer() : "?");
                }
                if (re) re->Release();
            }
            LOG_INFO("VR: 153 evidence reticle shaders armed (%d/%d). "
                     "GREEN ring = the published barrel direction from your "
                     "muzzle, ORANGE ring = camera forward. Both at "
                     "barrel_reticle_dist metres, so their separation is the "
                     "head-versus-barrel divergence. barrel_reticle=0 off, "
                     "1 barrel only, 2 both.",
                     made, kRet);
        }
    }
    return ok;
}


// BUILD 11a: acquire one image of the eye's canvas, clear it black, DRAW the
// eye's captured frame (g_last mips) at the angular size of `fovy`, release.
// Assumes blit_bind() state is current. Render thread, no logging (rule 8).
// BUILD 247: `src` and `aspect` are the true-stereo hooks. Left at their defaults (null, 0) the eye's own
// last-frame texture and the backbuffer's aspect are used and the function is the build 246 function. With
// them set, `src` is the SRV of the eye's half of the backbuffer and `aspect` is that half's width over its
// height, which is the content's angular extent: tan_h = tan_v * aspect.
bool blit_into(int eye, ID3D11DeviceContext* ctx, float fovy,
               ID3D11ShaderResourceView* src = nullptr, float aspect = 0.0f,
               float* out_tan_h = nullptr, float* out_tan_v = nullptr) {   // BUILD 248: the extents actually used
    ID3D11ShaderResourceView* src_srv = src ? src : g_last_srv[eye];
    if (fovy < 0.05f) fovy = 0.05f;      // keep the viewport inside D3D11's
    if (fovy > 2.60f) fovy = 2.60f;      // +-32k coordinate bounds
    // BUILD 11e: scoped frames display across the fixed window, not their
    // true (tiny) angle. Same threshold as the flat-scope camera gate, so
    // the enlarged frames are exactly the ones rendered without head
    // compose.
    if (fovy < headpose::mono_scope_fov() && g_scope_disp_fov > 0.0f)
        fovy = g_scope_disp_fov;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    ph(kPhAcquireBlit, eye);
    if (XR_FAILED(xrAcquireSwapchainImage(g_swapchains[eye], &ai, &idx))) {
        ph(kPhDone);
        return false;
    }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = img_wait_timeout();
    bool drawn = false;
    ph(kPhWaitBlit, eye);
    const XrResult wr = xrWaitSwapchainImage(g_swapchains[eye], &wi);
    ph(kPhDone);
    if (wr == XR_TIMEOUT_EXPIRED)
        g_img_to_blit.fetch_add(1, std::memory_order_relaxed);
    if (XR_SUCCEEDED(wr) && wr != XR_TIMEOUT_EXPIRED &&
        idx < g_rtv[eye].size() && g_rtv[eye][idx]) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ctx->ClearRenderTargetView(g_rtv[eye][idx], black);
        // Content spans [-tan_h..+tan_h] x [-tan_v..+tan_v] about the optical
        // center; the viewport is that span in canvas pixels. Fractional
        // placement is intended (viewports are float); the sampler resolves it.
        const float tan_v = tanf(0.5f * fovy);
        const float tan_h = tan_v * (aspect > 0.0f ? aspect : ((float)g_content_w / (float)g_content_h));
        if (out_tan_h) *out_tan_h = tan_h;
        if (out_tan_v) *out_tan_v = tan_v;
        D3D11_VIEWPORT vp;
        vp.TopLeftX = g_cx[eye] - tan_h * g_sx[eye];
        vp.TopLeftY = g_cy[eye] - tan_v * g_sy[eye];
        vp.Width    = 2.0f * tan_h * g_sx[eye];
        vp.Height   = 2.0f * tan_v * g_sy[eye];
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShaderResources(0, 1, &src_srv);
        ctx->OMSetRenderTargets(1, &g_rtv[eye][idx], nullptr);
        ctx->Draw(3, 0);
        drawn = true;

        // BUILD 221: the PiP scope, drawn over the content with the content
        // itself as the source (still bound at t0). The disc's viewport is the
        // eyepiece's bounding square in this eye; the constant buffer carries
        // the aim point's uv in the frame and the half extent at this zoom.
        if (g_pip_show && g_pip_ps && g_pip_cb) {
            const float tr = g_pip_tr[eye];
            const float rx = tr * g_sx[eye], ry = tr * g_sy[eye];
            float zoom = g_pip_zoom;
            if (!(zoom >= 1.2f)) zoom = 1.2f;   // BUILD 237: the user range
            if (zoom > 4.0f) zoom = 4.0f;
            // BUILD 237: this eye's own latched aim tangent, not the shared
            // pair. See the note at g_pip_atx_eye.
            const float cb[4] = {
                0.5f + g_pip_atx_eye[eye] / (2.0f * tan_h),
                0.5f - g_pip_aty_eye[eye] / (2.0f * tan_v),
                (tr / zoom) / (2.0f * tan_h),
                (tr / zoom) / (2.0f * tan_v)};
            ctx->UpdateSubresource(g_pip_cb, 0, nullptr, cb, 0, 0);
            ctx->PSSetConstantBuffers(0, 1, &g_pip_cb);
            D3D11_VIEWPORT pv;
            pv.TopLeftX = g_cx[eye] + g_pip_tx[eye] * g_sx[eye] - rx;
            pv.TopLeftY = g_cy[eye] - g_pip_ty[eye] * g_sy[eye] - ry;
            pv.Width    = 2.0f * rx;
            pv.Height   = 2.0f * ry;
            pv.MinDepth = 0.0f;
            pv.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &pv);
            ctx->PSSetShader(g_pip_ps, nullptr, 0);
            ctx->Draw(3, 0);
            ctx->PSSetShader(g_blit_ps, nullptr, 0);
        }

        // Build 24: the aim reticle rides the same acquire. Zero-disparity
        // placement (same angular offset in both eyes = optical infinity),
        // ~0.5 deg across, drawn with the CURRENT controller offset even on
        // the stale eye's redraw, which keeps the dot at display latency
        // rather than content latency.
        if (g_dot_on && g_dot_ps) {
            const float r = 0.0045f * g_sx[eye];
            D3D11_VIEWPORT dv;
            dv.TopLeftX = g_cx[eye] + g_dot_tx * g_sx[eye] - r;
            dv.TopLeftY = g_cy[eye] - g_dot_ty * g_sy[eye] - r;
            dv.Width    = 2.0f * r;
            dv.Height   = 2.0f * r;
            dv.MinDepth = 0.0f;
            dv.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &dv);
            ctx->PSSetShader(g_dot_ps, nullptr, 0);
            ctx->Draw(3, 0);
            ctx->PSSetShader(g_blit_ps, nullptr, 0);
        }

        // Build 42: the hand markers, same viewport-as-bounding-box trick.
        for (int h = 0; h < 2; ++h) {
            if (!g_hand_on[h] || !g_hand_ps[h]) continue;
            const float rx = g_hand_tr[h][eye] * g_sx[eye];
            const float ry = g_hand_tr[h][eye] * g_sy[eye];
            D3D11_VIEWPORT hv;
            hv.TopLeftX = g_cx[eye] + g_hand_tx[h][eye] * g_sx[eye] - rx;
            hv.TopLeftY = g_cy[eye] - g_hand_ty[h][eye] * g_sy[eye] - ry;
            hv.Width    = 2.0f * rx;
            hv.Height   = 2.0f * ry;
            hv.MinDepth = 0.0f;
            hv.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &hv);
            ctx->PSSetShader(g_hand_ps[h], nullptr, 0);
            ctx->Draw(3, 0);
            ctx->PSSetShader(g_blit_ps, nullptr, 0);
        }

        // Build 45: the weapon-candidate markers, identical mechanism.
        for (int i = 0; i < kWpm; ++i) {
            if (!g_wpm_on[i] || !g_wpm_ps[i]) continue;
            const float rx = g_wpm_tr[i][eye] * g_sx[eye];
            const float ry = g_wpm_tr[i][eye] * g_sy[eye];
            D3D11_VIEWPORT wv;
            wv.TopLeftX = g_cx[eye] + g_wpm_tx[i][eye] * g_sx[eye] - rx;
            wv.TopLeftY = g_cy[eye] - g_wpm_ty[i][eye] * g_sy[eye] - ry;
            wv.Width    = 2.0f * rx;
            wv.Height   = 2.0f * ry;
            wv.MinDepth = 0.0f;
            wv.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &wv);
            ctx->PSSetShader(g_wpm_ps[i], nullptr, 0);
            ctx->Draw(3, 0);
            ctx->PSSetShader(g_blit_ps, nullptr, 0);
        }

        // Build 153: the evidence reticle, same mechanism, drawn after the
        // markers so it is never hidden behind one. Fixed angular size, so
        // unlike the markers the radius does not come from the distance.
        for (int i = 0; i < kRet; ++i) {
            if (!g_ret_on[i] || !g_ret_ps[i]) continue;
            const float rx = g_ret_size * g_sx[eye];
            const float ry = g_ret_size * g_sy[eye];
            D3D11_VIEWPORT rv;
            rv.TopLeftX = g_cx[eye] + g_ret_tx[i][eye] * g_sx[eye] - rx;
            rv.TopLeftY = g_cy[eye] - g_ret_ty[i][eye] * g_sy[eye] - ry;
            rv.Width    = 2.0f * rx;
            rv.Height   = 2.0f * ry;
            rv.MinDepth = 0.0f;
            rv.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &rv);
            ctx->PSSetShader(g_ret_ps[i], nullptr, 0);
            ctx->Draw(3, 0);
            ctx->PSSetShader(g_blit_ps, nullptr, 0);
        }

        // 2026-08-12: the settings panel, drawn last so it sits over
        // everything else. Same viewport-as-bounding-box trick as the markers,
        // which is all it needs: the blit vertex shader already spreads uv 0..1
        // across whatever viewport is set, so pointing it at the menu texture
        // and sizing the viewport to the panel IS the draw.
        //
        // Placed HEAD LOCKED at zero disparity, exactly like the aim reticle:
        // identical angular offset in both eyes, so the panel reads as a
        // comfortable heads-up surface at optical infinity and text does not
        // fight the eyes. A world-locked panel would need a real quad with a
        // per-eye matrix, because turning your head makes it a general
        // quadrilateral that a viewport rectangle cannot express.
        //
        // g_menu_srv is produced once per frame by on_present. If the panel is
        // shut this is null and the whole block costs one branch.
        if (g_menu_srv) {
            const float half_w = kMenuHalfTanX;
            const float half_h = kMenuHalfTanX * ((float)menu::kMenuH / (float)menu::kMenuW);
            const float rx = half_w * g_sx[eye];
            const float ry = half_h * g_sy[eye];
            D3D11_VIEWPORT mv;
            mv.TopLeftX = g_cx[eye] - rx;
            mv.TopLeftY = g_cy[eye] - ry;
            mv.Width    = 2.0f * rx;
            mv.Height   = 2.0f * ry;
            mv.MinDepth = 0.0f;
            mv.MaxDepth = 1.0f;

            const float bf[4] = {0, 0, 0, 0};
            if (g_menu_blend) ctx->OMSetBlendState(g_menu_blend, bf, 0xFFFFFFFFu);
            ctx->RSSetViewports(1, &mv);
            ctx->PSSetShaderResources(0, 1, &g_menu_srv);
            ctx->Draw(3, 0);
            if (g_menu_blend) ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFFu);
            // Put the eye texture back for anything drawn after this.
            ctx->PSSetShaderResources(0, 1, &src_srv);
        }

        // 11a.2 one-shot probe: read one pixel of the blit source and one of
        // the canvas after the draw. Runs once per run, a few seconds in (so
        // its note cannot clobber the canvas note), Map stall accepted as a
        // one-time diagnostic cost. Values are raw R8G8B8A8 memory (RR GG BB
        // AA reading the hex right to left).
        if (!g_probe_done && g_probe_tex && d3d11::frame_count() > 400) {
            g_probe_done = true;
            uint32_t src_px = 0, dst_px = 0;
            D3D11_MAPPED_SUBRESOURCE ms{};
            D3D11_BOX bs_{g_content_w / 2, g_content_h / 2, 0,
                          g_content_w / 2 + 1, g_content_h / 2 + 1, 1};
            ctx->CopySubresourceRegion(g_probe_tex, 0, 0, 0, 0, g_last[eye], 0, &bs_);
            if (SUCCEEDED(ctx->Map(g_probe_tex, 0, D3D11_MAP_READ, 0, &ms))) {
                src_px = *(const uint32_t*)ms.pData;
                ctx->Unmap(g_probe_tex, 0);
            }
            UINT px = (UINT)(vp.TopLeftX + vp.Width * 0.5f);
            UINT py = (UINT)(vp.TopLeftY + vp.Height * 0.5f);
            if (px >= g_sc_w) px = g_sc_w / 2;
            if (py >= g_sc_h) py = g_sc_h / 2;
            D3D11_BOX bd_{px, py, 0, px + 1, py + 1, 1};
            ctx->CopySubresourceRegion(g_probe_tex, 0, 0, 0, 0,
                                       g_images[eye][idx].texture, 0, &bd_);
            if (SUCCEEDED(ctx->Map(g_probe_tex, 0, D3D11_MAP_READ, 0, &ms))) {
                dst_px = *(const uint32_t*)ms.pData;
                ctx->Unmap(g_probe_tex, 0);
            }
            note("VR: 11a.2 probe eye %d: source px %08X, canvas px after draw %08X "
                 "(equal-ish = draw works; canvas 000000FF/black = draw lost; "
                 "source black = capture lost)",
                 eye, src_px, dst_px);
        }
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_swapchains[eye], &ri);
    return drawn;
}

// BUILD 10L: create the per-eye canvas swapchains, sized so canvas pixels
// per tangent unit equal the content's pixels per tangent unit at the
// reference fov (fovy pi/4, the game's stationary value). The content then
// copies in 1:1 with no scaling and spans its true angular size. Called once
// from the present path (one-time creation; rule 8 tolerates it as init).
bool create_canvases(const d3d11::State& st, const XrView views[2]) {
    const float half_v = 0.3926991f;  // pi/8
    const float aspect = (float)g_content_w / (float)g_content_h;
    const float ppt    = (float)g_content_w / (2.0f * tanf(half_v) * aspect);

    float tw = 0.0f, th = 0.0f;
    for (int e = 0; e < 2; ++e) {
        const float w = tanf(views[e].fov.angleRight) - tanf(views[e].fov.angleLeft);
        const float h = tanf(views[e].fov.angleUp) - tanf(views[e].fov.angleDown);
        if (w > tw) tw = w;
        if (h > th) th = h;
    }
    g_sc_w = ((uint32_t)(tw * ppt + 0.5f) + 1u) & ~1u;
    g_sc_h = ((uint32_t)(th * ppt + 0.5f) + 1u) & ~1u;

    // BUILD 229: cap at what the runtime asked for, with the game's own
    // content size as a hard floor. The floor matters: copy_into copies a
    // content-sized box into this canvas and the dst-offset arithmetic below
    // computes g_sc_w - g_content_w unsigned, so a canvas narrower than the
    // content would underflow it. Each axis is capped independently, which is
    // fine because g_sx and g_sy are derived per axis further down.
    const uint32_t was_w = g_sc_w, was_h = g_sc_h;
    if (g_cfg_canvas_clamp.load(std::memory_order_relaxed) && g_rec_w && g_rec_h) {
        const uint32_t cap_w = g_rec_w > g_content_w ? g_rec_w : g_content_w;
        const uint32_t cap_h = g_rec_h > g_content_h ? g_rec_h : g_content_h;
        if (g_sc_w > cap_w) g_sc_w = (cap_w + 1u) & ~1u;
        if (g_sc_h > cap_h) g_sc_h = (cap_h + 1u) & ~1u;
    }
    if (g_max_w && g_sc_w > g_max_w) g_sc_w = g_max_w & ~1u;
    if (g_max_h && g_sc_h > g_max_h) g_sc_h = g_max_h & ~1u;
    g_clamp_from_w = was_w; g_clamp_from_h = was_h;

    for (int eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo scci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        scci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        scci.format      = g_sc_format;
        scci.sampleCount = 1;
        scci.width       = g_sc_w;
        scci.height      = g_sc_h;
        scci.faceCount   = 1;
        scci.arraySize   = 1;
        scci.mipCount    = 1;
        ph(kPhCreateSwapchain, eye);
        if (XR_FAILED(xrCreateSwapchain(g_session, &scci, &g_swapchains[eye]))) {
            ph(kPhDone);
            return false;
        }

        uint32_t ic = 0;
        ph(kPhEnumImages, eye);
        xrEnumerateSwapchainImages(g_swapchains[eye], 0, &ic, nullptr);
        ph(kPhDone);
        g_images[eye].assign(ic, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(xrEnumerateSwapchainImages(g_swapchains[eye], ic, &ic,
                                                 (XrSwapchainImageBaseHeader*)g_images[eye].data())))
            return false;
        g_rtv[eye].assign(ic, nullptr);
        // 11a.2: the runtime may create the swapchain textures TYPELESS, and
        // a null-desc RTV on a typeless resource FAILS. That failure has been
        // possible since 10L but was invisible: the RTV only served the
        // cosmetic clear, and the never-drawn XR images are zero (black)
        // anyway. The blit is the first consumer that NEEDS the RTV. Ask for
        // a typed view of the format we requested, and record failures.
        D3D11_RENDER_TARGET_VIEW_DESC rvd{};
        rvd.Format        = (DXGI_FORMAT)g_sc_format;
        rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        for (uint32_t i = 0; i < ic; ++i) {
            const HRESULT hr = st.device->CreateRenderTargetView(
                g_images[eye][i].texture, &rvd, &g_rtv[eye][i]);
            if (FAILED(hr)) {
                g_rtv[eye][i] = nullptr;
                ++g_rtv_fail;
                if (!g_rtv_hr) g_rtv_hr = (long)hr;
            }
        }

        g_eyefov[eye] = views[eye].fov;
        const float tanL = tanf(views[eye].fov.angleLeft);
        const float tanR = tanf(views[eye].fov.angleRight);
        const float tanU = tanf(views[eye].fov.angleUp);
        const float tanD = tanf(views[eye].fov.angleDown);
        // Canvas column/row where the content's optical center (angle 0)
        // belongs; content is centered on it. Row 0 is angleUp.
        const float cx = (float)g_sc_w * (0.0f - tanL) / (tanR - tanL);
        const float cy = (float)g_sc_h * tanU / (tanU - tanD);
        // BUILD 11a: the exact tangent-to-pixel mapping of the canvas as
        // submitted (the texture spans tanL..tanR, tanU..tanD edge to edge),
        // used by blit_into to size the destination rect from the live fov.
        // Slightly more exact than ppt: it absorbs the even-rounding above.
        g_sx[eye] = (float)g_sc_w / (tanR - tanL);
        g_sy[eye] = (float)g_sc_h / (tanU - tanD);
        g_cx[eye] = -tanL * g_sx[eye];
        g_cy[eye] =  tanU * g_sy[eye];
        int dx = (int)(cx + 0.5f) - (int)g_content_w / 2;
        int dy = (int)(cy + 0.5f) - (int)g_content_h / 2;
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;
        if (dx > (int)(g_sc_w - g_content_w)) dx = (int)(g_sc_w - g_content_w);
        if (dy > (int)(g_sc_h - g_content_h)) dy = (int)(g_sc_h - g_content_h);
        g_dst_x[eye] = dx;
        g_dst_y[eye] = dy;
    }
    g_canvas_ready = true;
    // 11a.2: a blit without working RTVs submits nothing (black headset).
    // Fall back to the fixed copy, which needs no RTV, and say so.
    if (g_rtv_fail && g_blit_ok) g_blit_ok = false;
    note("VR: 10L canvas %ux%u, content %ux%u at L(%d,%d) R(%d,%d), rtv fail %d hr 0x%08lX%s",
         g_sc_w, g_sc_h, g_content_w, g_content_h,
         g_dst_x[0], g_dst_y[0], g_dst_x[1], g_dst_y[1],
         g_rtv_fail, (unsigned long)g_rtv_hr,
         g_rtv_fail ? " BLIT OFF, fixed-copy fallback" : "");
    return true;
}

// BUILD 118: DEPTH CANDIDATE PROBE. Log only, latched once, then never again.
//
// The stale-eye reprojection CLAUDE.md calls "the single most important delta
// from the reference" needs a depth buffer per eye. The mod has never captured
// one. It does, however, ALREADY hold the game's depth-stencil view: ctx_save
// calls OMGetRenderTargets and stores s.dsv, purely so ctx_restore can put it
// back. It has been sitting there, AddRef'd, unread, since build 11a.
//
// This reads its description exactly ONCE and reports it. It answers, in a
// single run and with no new hook, no new signature and no new address:
//   - does the game even leave a depth-stencil bound at Present (route "a")
//   - the real DXGI format, expected 19 R32G8X24_TYPELESS
//   - the real BindFlags, expected 0x48 DEPTH_STENCIL | SHADER_RESOURCE, which
//     is what makes an SRV possible at all and is the make-or-break fact
//   - SampleDesc.Count, THE MSAA QUESTION. GRW.ini carries AntiAliasingMode=4
//     and the engine's own AA option table has MSAA 4x at index 4. If the
//     scene depth is multisampled the blit needs Texture2DMS and a resolve,
//     which is a different build. This is the only thing standing between the
//     project and a working depth blit and it cannot be settled offline.
//   - the true render resolution. GRW.ini has Supersampling=0.90000 and
//     0.9 * 3840 = 3456 exactly, so an exact match against st.width would
//     silently find nothing. Do not predicate on it; measure it.
//   - whether the engine's debug names survive into retail. If GetPrivateData
//     returns a name, targets can be identified by string at runtime, which is
//     exact and immune to description collisions.
//
// RULE 8: this is COM work on the render thread, which the rule bans. It is
// permitted here on the same grounds as create_canvases, which allocates
// swapchains in this same path: it runs ONCE, is init-shaped, and is gated by
// a flag that is set before the work rather than after, so a fault inside
// cannot make it retry every frame. It logs through note(), the deferred
// buffer, never LOG_INFO directly.
void probe_depth_once(const CtxState& s) {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;   // set FIRST: one attempt, ever, whatever happens below

    if (!s.dsv) {
        note_once("VR: 118 depth probe: NO depth-stencil bound at Present. "
             "Route (a) is dead; the capture must move to OMSetRenderTargets "
             "(d3d11 vtable slots 33 AND 34, or the UAV-bound passes are missed).");
        return;
    }

    ID3D11Resource* res = nullptr;
    s.dsv->GetResource(&res);
    if (!res) {
        note_once("VR: 118 depth probe: DSV bound but GetResource returned null.");
        return;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    s.dsv->GetDesc(&vd);

    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);

        // The engine names most render targets but, per the static pass,
        // deliberately not this one. Ask anyway: a name here would be a
        // stronger identifier than any description predicate.
        char name[128] = {};
        UINT n = sizeof(name) - 1;
        const GUID kDebugObjectName =
            {0x429b8c22, 0x9188, 0x4b0c, {0x87, 0x46, 0xb1, 0xe3, 0xa0, 0xc3, 0xfd, 0xcd}};
        if (FAILED(tex->GetPrivateData(kDebugObjectName, &n, name)) || !n)
            strcpy_s(name, "<unnamed>");

        note_once("VR: 118 depth probe: %ux%u fmt=%u samples=%u/%u bind=0x%X "
             "usage=%u arr=%u mips=%u misc=0x%X dsvfmt=%u dsvdim=%u dsvflags=0x%X "
             "name=\"%s\" | content=%ux%u %s%s",
             td.Width, td.Height, (unsigned)td.Format,
             td.SampleDesc.Count, td.SampleDesc.Quality,
             td.BindFlags, (unsigned)td.Usage, td.ArraySize, td.MipLevels,
             td.MiscFlags,
             (unsigned)vd.Format, (unsigned)vd.ViewDimension, vd.Flags,
             name, g_content_w, g_content_h,
             (td.BindFlags & D3D11_BIND_SHADER_RESOURCE)
                 ? "SRV-BINDABLE" : "NOT SRV-bindable, cannot sample it",
             td.SampleDesc.Count > 1
                 ? ", MSAA: needs Texture2DMS and a resolve" : ", single-sample");
        tex->Release();
    } else {
        note_once("VR: 118 depth probe: depth resource is not a Texture2D.");
    }
    res->Release();
}

// ---------------------------------------------------------------------------
// BUILD 233: UNIVERSAL OPENXR API LAYER SUPPRESSION. See the cfg block above
// for the evidence. Everything here runs once, on the init thread, before
// xrCreateInstance, and it logs whatever it finds whether it acts or not.
// ---------------------------------------------------------------------------

// The manifest scanner itself lives in src\XrLayers.h so tools\layertest can
// run it against real manifests without the game. See that header for why it
// is a scan and not a JSON parse.

bool read_file_text(const char* path, std::string& out) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (1 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(h, &out[0], (DWORD)sz.QuadPart, &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

// Case-insensitive membership in the comma-separated xr_layer_allow list.
bool layer_is_allowed(const char* name) {
    if (!g_cfg_layer_allow[0]) return false;
    std::string list = g_cfg_layer_allow, item;
    for (size_t i = 0; i <= list.size(); ++i) {
        if (i == list.size() || list[i] == ',') {
            size_t s = item.find_first_not_of(" \t");
            size_t e = item.find_last_not_of(" \t");
            if (s != std::string::npos && _stricmp(item.substr(s, e - s + 1).c_str(),
                                                   name) == 0)
                return true;
            item.clear();
        } else {
            item += list[i];
        }
    }
    return false;
}

// Walk one hive's Implicit key. Returns how many enabled layers it suppressed.
int suppress_hive(HKEY root, const char* hive_name) {
    HKEY key = nullptr;
    LONG r = RegOpenKeyExA(root, "SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit",
                           0, KEY_READ, &key);
    if (r != ERROR_SUCCESS) {
        // ERROR_FILE_NOT_FOUND is the ordinary case: most machines have no
        // HKCU key at all. Anything else is worth a line.
        if (r != ERROR_FILE_NOT_FOUND)
            LOG_INFO("VR: layers: %s implicit key unreadable (error %ld)", hive_name, r);
        return 0;
    }

    int suppressed = 0;
    for (DWORD i = 0;; ++i) {
        char  path[1024];
        DWORD path_len = (DWORD)sizeof(path);
        DWORD type = 0, data = 0, data_len = (DWORD)sizeof(data);
        LONG e = RegEnumValueA(key, i, path, &path_len, nullptr, &type,
                               (LPBYTE)&data, &data_len);
        if (e == ERROR_NO_MORE_ITEMS) break;
        if (e != ERROR_SUCCESS) {
            LOG_INFO("VR: layers: %s enum stopped at index %lu (error %ld)",
                     hive_name, i, e);
            break;
        }
        if (type != REG_DWORD) data = 0;   // treat a non-DWORD as enabled

        std::string text, name, disable_env;
        bool parsed = read_file_text(path, text);
        if (parsed) {
            xrlayers::layer_name(text, name);
            xrlayers::disable_env(text, disable_env);
        }
        const char* shown = name.empty() ? "(name unreadable)" : name.c_str();

        if (data != 0) {
            LOG_INFO("VR: layers: %s %s ALREADY OFF at the registry (%s)",
                     hive_name, shown, path);
            continue;
        }
        if (!parsed) {
            LOG_WARN("VR: layers: %s manifest could NOT BE READ, so this layer "
                     "cannot be switched off and stays in the path: %s "
                     "(error %lu)", hive_name, path, GetLastError());
            continue;
        }
        if (layer_is_allowed(shown)) {
            LOG_INFO("VR: layers: %s %s KEPT by xr_layer_allow", hive_name, shown);
            continue;
        }
        if (disable_env.empty()) {
            LOG_WARN("VR: layers: %s %s is ENABLED and its manifest carries NO "
                     "disable_environment, so it CANNOT be switched off and "
                     "stays in the path: %s", hive_name, shown, path);
            continue;
        }
        SetEnvironmentVariableA(disable_env.c_str(), "1");
        ++suppressed;
        // BUILD 236: the manifest path belongs on this line too. Every other
        // outcome above prints it, and without it the SUPPRESSED line is the
        // one case where a tester's log cannot be traced back to a file on
        // their disk.
        LOG_INFO("VR: layers: %s %s SUPPRESSED for this process (%s = 1) [%s]",
                 hive_name, shown, disable_env.c_str(), path);
    }
    RegCloseKey(key);
    return suppressed;
}

// BUILD 236: the layers that survived, remembered by name so the first-frame
// watchdog can print them again at the moment of the stall. A tester's log is
// thousands of lines long and the suppression block is at the very top of it;
// repeating the list beside the failure means the two facts that matter -
// "this hung" and "this was in the chain while it hung" - sit together.
//
// Written once on the init thread before any VR frame exists, read afterwards
// on the render thread. There is no window where both happen at once.
std::vector<std::string> g_layer_survivor_names;

// Reprint the survivor list. Silent by design when nothing survived: on a
// machine with a clean chain this must not add a line that reads like a fault.
void log_layer_survivors(const char* when) {
    if (g_layer_survivor_names.empty()) {
        LOG_ERROR("xrlayers: no foreign OpenXR layer was in the chain %s, so a "
                  "layer is NOT the explanation for this machine.", when);
        return;
    }
    LOG_ERROR("xrlayers: %zu foreign OpenXR layer(s) were in the chain %s:",
              g_layer_survivor_names.size(), when);
    for (const std::string& n : g_layer_survivor_names)
        LOG_ERROR("xrlayers:   %s", n.c_str());
}

// Returns how many foreign layers are STILL in our chain after everything
// above has been tried. Zero is the healthy answer and the answer on every
// machine that has ever completed a VR frame.
uint32_t suppress_api_layers() {
    // The two hardcoded names stay as a floor. If the registry walk is
    // refused outright these are still the two layers we know by name, and
    // when the walk works it finds the same two and sets them again.
    SetEnvironmentVariableA("DISABLE_XR_APILAYER_NOVENDOR_OBSMirror", "1");
    SetEnvironmentVariableA("DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY", "1");

    if (!g_cfg_layer_suppress.load(std::memory_order_relaxed)) {
        LOG_INFO("VR: layers: xr_layer_suppress = 0, only the two hardcoded "
                 "names are set. This is the pre-233 behaviour.");
    } else {
        if (g_cfg_layer_allow[0])
            LOG_INFO("VR: layers: xr_layer_allow = %s", g_cfg_layer_allow);
        int n = suppress_hive(HKEY_LOCAL_MACHINE, "HKLM")
              + suppress_hive(HKEY_CURRENT_USER,  "HKCU");
        LOG_INFO("VR: layers: %d implicit layer(s) switched off for this "
                 "process by the registry walk.", n);
    }

    // The verdict line. The loader honours the variables above at this call,
    // so whatever it still reports is genuinely in our chain.
    uint32_t unwanted = 0;   // survivors the user did NOT ask to keep
    g_layer_survivor_names.clear();   // BUILD 236
    uint32_t lc = 0;
    xrEnumerateApiLayerProperties(0, &lc, nullptr);
    std::vector<XrApiLayerProperties> lps(lc, {XR_TYPE_API_LAYER_PROPERTIES});
    if (lc) xrEnumerateApiLayerProperties(lc, &lc, lps.data());
    if (lc == 0) {
        LOG_INFO("VR: 0 API layer(s) visible after suppression. Nothing "
                 "foreign is in the OpenXR chain.");
    } else {
        // Loud, because every wedged machine we can currently reach has
        // exactly one of these and every machine that runs has none.
        LOG_WARN("VR: %u API layer(s) STILL VISIBLE after suppression. A "
                 "foreign layer sits inside every OpenXR call this mod makes:",
                 lc);
        for (uint32_t i = 0; i < lc; ++i) {
            bool chosen = layer_is_allowed(lps[i].layerName);
            LOG_WARN("VR:   %s (v%u) %s%s", lps[i].layerName,
                     lps[i].layerVersion, lps[i].description,
                     chosen ? "  [KEPT ON PURPOSE by xr_layer_allow]" : "");
            // BUILD 236: kept for the watchdog. Allow-listed ones go in too:
            // the question the watchdog answers is "what was in the chain",
            // and a layer the user asked to keep is still in the chain.
            {
                std::string n = lps[i].layerName;
                if (chosen) n += "  [kept on purpose by xr_layer_allow]";
                g_layer_survivor_names.push_back(n);
            }
            // A layer the user named in xr_layer_allow is their decision and
            // does not count toward the stand-down: they asked for it, they
            // were told what it risks, and overriding that would be the mod
            // deciding for them.
            if (!chosen) ++unwanted;
        }
        LOG_WARN("VR: if the first frame never completes, this is the first "
                 "thing to switch off. Set its registry DWORD to 1 under "
                 "SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit and relaunch.");
    }
    return unwanted;
}

}  // namespace

bool begin_and_arm();   // build 38, defined below

bool init(const d3d11::State& st) {
    if (g_failed.load()) return false;
    // Build 38: a session that exists but is not begun yet is poll_start()'s
    // business, not a reason to claim success here.
    if (g_session != XR_NULL_HANDLE) return g_active.load();
    if (!st.device || !st.context) return false;

    LOG_INFO("VR: initialising OpenXR on the game's D3D11 device");
    load_config();

    // BUILD 10d, REPLACED BY BUILD 233: foreign implicit API layers sit inside
    // every OpenXR call this mod makes. 10d killed the two this machine had by
    // hardcoded name; 233 reads the machine's own registry and manifests and
    // switches off whatever is actually there. load_config() ran directly
    // above, so xr_layer_suppress and xr_layer_allow are current.
    uint32_t survivors = suppress_api_layers();

    // BUILD 234: THE GAME NEVER DIES FOR THIS AGAIN.
    //
    // If a foreign layer is STILL in the chain here, we have tried everything
    // we can and failed: its manifest carries no disable_environment, or the
    // manifest could not be read, or the user allow-listed it. That is the
    // exact configuration that kills the game today. The mod cannot rescue it
    // later either: the wedge parks the game's own render thread inside the
    // first Present, and no watchdog thread can unblock another thread that
    // is already stuck in a runtime call.
    //
    // So decide it HERE, before a session exists, where bailing out is free.
    // The game then runs flat, on its own monitor, exactly as if the mod were
    // not installed, and the log says which layer and what to do about it.
    // "No VR, and here is the one line telling you why" beats "black screen,
    // back to your launcher, no idea".
    //
    // This cannot cost anyone their VR: every machine that has ever completed
    // a frame reports zero layers here, and every machine that reports one is
    // already failing to start. xr_layer_bail = 0 for the user who wants to
    // try anyway.
    if (survivors && g_cfg_layer_bail.load(std::memory_order_relaxed)) {
        LOG_ERROR("VR: STANDING DOWN BEFORE STARTING. %u foreign OpenXR layer(s) "
                  "are in the chain and could not be switched off, and that is "
                  "the configuration that hangs this game on the first frame.",
                  survivors);
        LOG_ERROR("VR: THE GAME WILL NOW RUN NORMALLY ON YOUR MONITOR, WITHOUT "
                  "VR. Nothing is broken and nothing needs reinstalling.");
        LOG_ERROR("VR: TO GET VR BACK: switch the layer(s) named above off. In "
                  "regedit, under HKEY_LOCAL_MACHINE\\SOFTWARE\\Khronos\\OpenXR"
                  "\\1\\ApiLayers\\Implicit, set that layer's value to 1, then "
                  "relaunch. Most are a checkbox in the program that installed "
                  "them (ReShade's setup, OBS, OpenXR Toolkit).");
        LOG_ERROR("VR: to skip this check and start VR anyway, set "
                  "xr_layer_bail = 0 in grwxr.cfg. The game may then hang on "
                  "the first frame, which is what this check exists to prevent.");
        g_failed.store(true);
        return false;
    }

    // --- extensions ---
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    if (n) xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    bool has_d3d11 = false;
    // BUILD 118: the list was enumerated and thrown away since build 1, and
    // "does this runtime expose the depth extension" has been an open
    // [UNKNOWN] blocking the reprojection work that whole time. It is init
    // time, so rule 8 does not apply and the lines are free. Log the lot.
    LOG_INFO("VR: %u instance extension(s):", n);
    for (auto& e : exts) {
        LOG_INFO("VR:   ext %s v%u", e.extensionName, e.extensionVersion);
        if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) has_d3d11 = true;
        if (!strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME))
            g_depth_ext = true;
    }
    if (!has_d3d11) {
        LOG_ERROR("VR: runtime does not expose XR_KHR_D3D11_enable. Cannot continue.");
        g_failed.store(true);
        return false;
    }
    LOG_INFO("VR: XR_KHR_composition_layer_depth %s",
             g_depth_ext ? "PRESENT, enabling it"
                         : "ABSENT: compositor stays orientation-only");

    // Enabling the depth extension costs nothing on its own: it only matters
    // once a depth layer is actually chained onto a projection view. Asking
    // for an extension the runtime advertises cannot fail instance creation,
    // and if it somehow did we would rather find out here, at init, than at
    // submit time on the render thread.
    const char* enabled[2] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME, nullptr };
    uint32_t    enabled_n  = 1;
    if (g_depth_ext) enabled[enabled_n++] = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    ici.enabledExtensionCount = enabled_n;
    ici.enabledExtensionNames = enabled;
    strcpy_s(ici.applicationInfo.applicationName, "GRW-XR");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "AnvilNext");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XrResult r = xrCreateInstance(&ici, &g_instance);
    // BUILD 118 safety net. This build is the first to ask for a second
    // extension, and it was deployed without a headset test. If the extra
    // request is what killed instance creation, fall straight back to the
    // exact build 117 request rather than losing VR altogether. A mod that
    // silently drops one optimisation beats a mod that does not start.
    if (XR_FAILED(r) && enabled_n > 1) {
        LOG_WARN("VR: xrCreateInstance failed with the depth extension (XrResult %d); "
                 "retrying with XR_KHR_D3D11_enable alone", (int)r);
        g_depth_ext = false;
        ici.enabledExtensionCount = 1;
        r = xrCreateInstance(&ici, &g_instance);
    }
    if (XR_FAILED(r)) return fail("xrCreateInstance", r);

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &ip);
    LOG_INFO("VR: runtime = %s", ip.runtimeName);

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(g_instance, &sgi, &g_system);
    if (XR_FAILED(r)) {
        LOG_ERROR("VR: no headset available (XrResult %d). Is Quest Link connected?", (int)r);
        g_failed.store(true);
        return false;
    }

    // 2026-08-12: name the HEADSET, not just the runtime. Every tester report
    // so far has needed a round trip to ask which headset it was, and the
    // runtime name does not answer it: VirtualDesktopXR and SteamVR both front
    // several different devices. systemName is the runtime's own string for
    // the physical hardware, so it lands in the log with no user effort.
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(g_instance, g_system, &sp))) {
        LOG_INFO("VR: headset = %s (vendor 0x%04X), max swapchain %ux%u, %u layer(s)",
                 sp.systemName, sp.vendorId,
                 sp.graphicsProperties.maxSwapchainImageWidth,
                 sp.graphicsProperties.maxSwapchainImageHeight,
                 sp.graphicsProperties.maxLayerCount);
    } else {
        LOG_WARN("VR: headset name unavailable (xrGetSystemProperties failed)");
    }

    {   // BUILD 229: what size does the runtime actually want? Never asked
        // before this build. Log it whatever the clamp is set to, so the
        // numbers are on record even when the clamp is off.
        uint32_t vc = 0;
        XrResult vr_r = xrEnumerateViewConfigurationViews(
            g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &vc, nullptr);
        if (XR_SUCCEEDED(vr_r) && vc >= 1) {
            std::vector<XrViewConfigurationView> vcv(
                vc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
            vr_r = xrEnumerateViewConfigurationViews(
                g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                vc, &vc, vcv.data());
            if (XR_SUCCEEDED(vr_r) && vc >= 1) {
                g_rec_w = vcv[0].recommendedImageRectWidth;
                g_rec_h = vcv[0].recommendedImageRectHeight;
                g_max_w = vcv[0].maxImageRectWidth;
                g_max_h = vcv[0].maxImageRectHeight;
                LOG_INFO("VR: 229 the runtime recommends %ux%u per eye "
                         "(max %ux%u, %u view(s), %u sample(s)). canvas_clamp "
                         "= %d: the canvas is capped at this, and never below "
                         "the game's own content size.",
                         g_rec_w, g_rec_h, g_max_w, g_max_h, vc,
                         vcv[0].recommendedSwapchainSampleCount,
                         g_cfg_canvas_clamp.load(std::memory_order_relaxed));
            }
        }
        if (!g_rec_w)
            LOG_WARN("VR: 229 xrEnumerateViewConfigurationViews gave nothing "
                     "(result %d). The canvas clamp is INERT this run and the "
                     "canvas is sized exactly as it was before build 229.",
                     (int)vr_r);
    }

    // The runtime dictates which adapter. If the game's device is on a different
    // one, sharing textures across adapters silently fails and the compositor
    // hangs after the first frame. That is issue #2: a desktop whose CPU has
    // integrated graphics (Ryzen 7000/9000, Intel iGPU) plus a discrete card,
    // where Windows bound the game to the iGPU while the runtime used the dGPU.
    // We read both LUIDs, and on a mismatch STAND DOWN to flat rendering with a
    // loud, actionable log instead of freezing the game.
    LUID want_luid = {};
    bool have_want = false;
    PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&pfn);
    if (pfn) {
        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (XR_SUCCEEDED(pfn(g_instance, g_system, &req))) {
            want_luid = req.adapterLuid;
            have_want = (want_luid.HighPart != 0 || want_luid.LowPart != 0);
            LOG_INFO("VR: runtime wants adapter LUID %08lX:%08lX",
                     (unsigned long)req.adapterLuid.HighPart,
                     (unsigned long)req.adapterLuid.LowPart);
        }
    }

    // Read the GAME device's own adapter LUID via DXGI and compare.
    if (have_want && g_cfg_adapter_check) {
        LUID game_luid = {};
        bool have_game = false;
        IDXGIDevice* dxdev = nullptr;
        if (SUCCEEDED(st.device->QueryInterface(__uuidof(IDXGIDevice),
                                                (void**)&dxdev)) && dxdev) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dxdev->GetAdapter(&ad)) && ad) {
                DXGI_ADAPTER_DESC d{};
                if (SUCCEEDED(ad->GetDesc(&d))) {
                    game_luid = d.AdapterLuid;
                    have_game = true;
                    char name[160] = {};
                    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name,
                                        (int)sizeof(name), nullptr, nullptr);
                    LOG_INFO("VR: game device is on adapter LUID %08lX:%08lX (%s)",
                             (unsigned long)game_luid.HighPart,
                             (unsigned long)game_luid.LowPart, name);
                }
                ad->Release();
            }
            dxdev->Release();
        }
        if (have_game &&
            (game_luid.HighPart != want_luid.HighPart ||
             game_luid.LowPart  != want_luid.LowPart)) {
            LOG_ERROR("");
            LOG_ERROR("############ FATAL: GPU ADAPTER MISMATCH ############");
            LOG_ERROR("  The headset runtime is using one GPU, but the GAME is");
            LOG_ERROR("  rendering on a DIFFERENT GPU. Frames cannot be shared");
            LOG_ERROR("  across GPUs, so VR would freeze after the first frame.");
            LOG_ERROR("  VR is STOOD DOWN; the game keeps running on the flat");
            LOG_ERROR("  monitor so it does not hang. This is common on desktops");
            LOG_ERROR("  whose CPU has integrated graphics (Ryzen 7000/9000,");
            LOG_ERROR("  Intel iGPU) next to a discrete card.");
            LOG_ERROR("  FIX (do ONE, then relaunch):");
            LOG_ERROR("   1. Plug your MONITOR into the discrete GPU's ports,");
            LOG_ERROR("      not the motherboard's video output.");
            LOG_ERROR("   2. Windows Settings > System > Display > Graphics: add");
            LOG_ERROR("      GRW.exe, set it to High performance (your dGPU).");
            LOG_ERROR("   3. NVIDIA Control Panel / AMD Software: set GRW.exe's");
            LOG_ERROR("      preferred GPU to the discrete card.");
            LOG_ERROR("   4. Or disable the integrated GPU in Device Manager/BIOS.");
            LOG_ERROR("  Override (NOT recommended): adapter_check=0 in grwxr.cfg.");
            LOG_ERROR("####################################################");
            LOG_ERROR("");
            g_failed.store(true);
            return false;
        }
    }

    // --- session on the GAME's device ---
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = st.device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    r = xrCreateSession(g_instance, &sci, &g_session);
    if (XR_FAILED(r)) return fail("xrCreateSession", r);
    LOG_INFO("VR: session created on the game's own D3D11 device");

    // LOCAL, not STAGE: for a fixed screen in front of the viewer we want a
    // seated origin, not a room-scale floor origin.
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    r = xrCreateReferenceSpace(g_session, &rsci, &g_space);
    if (XR_FAILED(r)) return fail("xrCreateReferenceSpace", r);

    // Build 8: a VIEW space handle so xrLocateSpace(VIEW, LOCAL, t) yields the
    // head pose directly, without pulling per-eye view state we do not need yet.
    XrReferenceSpaceCreateInfo vsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vsci.poseInReferenceSpace.orientation.w = 1.0f;
    r = xrCreateReferenceSpace(g_session, &vsci, &g_view_space);
    if (XR_FAILED(r)) return fail("xrCreateReferenceSpace(VIEW)", r);
    // BUILD 205: a STAGE space alongside, for the head's height above the
    // physical floor (immune to recenter). Optional: a runtime without a
    // floor leaves it null and the stance loop falls back to the LOCAL
    // height plus an assumed standing eye height.
    {
        XrReferenceSpaceCreateInfo ssci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        ssci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        ssci.poseInReferenceSpace.orientation.w = 1.0f;
        const XrResult sr = xrCreateReferenceSpace(g_session, &ssci, &g_stage_space);
        if (XR_FAILED(sr)) {
            g_stage_space = XR_NULL_HANDLE;
            LOG_INFO("stance: no STAGE space on this runtime (%d); the head's height comes from LOCAL plus 1.65 m.", (int)sr);
        } else {
            LOG_INFO("stance: STAGE space created; the head's height above the floor is read every frame.");
        }
    }

    // BUILD 13b: controller input plumbing. Every step is checked; a failure
    // logs which call died and disables input for the run WITHOUT failing the
    // mirror (motion controls are additive, the screen must keep working).
    g_input_ok = [&]() -> bool {
        XrResult ir;
        if (XR_FAILED(xrStringToPath(g_instance, "/user/hand/left",  &g_hand[0])) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right", &g_hand[1]))) {
            LOG_INFO("VR: 13b hand paths FAILED, input disabled");
            return false;
        }
        XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(asci.actionSetName, "grwxr");
        strcpy_s(asci.localizedActionSetName, "GRW-XR");
        ir = xrCreateActionSet(g_instance, &asci, &g_actionset);
        if (XR_FAILED(ir)) { LOG_INFO("VR: 13b xrCreateActionSet FAILED %d, input disabled", (int)ir); return false; }

        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy_s(aci.actionName, "aim_pose");
        strcpy_s(aci.localizedActionName, "Aim pose");
        aci.countSubactionPaths = 2;
        aci.subactionPaths = g_hand;
        ir = xrCreateAction(g_actionset, &aci, &g_act_aim);
        if (XR_FAILED(ir)) { LOG_INFO("VR: 13b create aim_pose FAILED %d, input disabled", (int)ir); return false; }

        // BUILD 162: the GRIP pose, the anatomical frame OpenXR defines for
        // the hand itself (+X palm normal, -Z little finger to thumb, +Y by
        // the right-hand rule). Non-fatal, unlike the rest of this block:
        // the aim pose stays the aim, and the whole input surface must not
        // be lost to a runtime that cannot supply this one.
        aci = {XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy_s(aci.actionName, "grip_pose");
        strcpy_s(aci.localizedActionName, "Grip pose");
        aci.countSubactionPaths = 2;
        aci.subactionPaths = g_hand;
        ir = xrCreateAction(g_actionset, &aci, &g_act_gpose);
        if (XR_FAILED(ir)) { LOG_INFO("VR: 162 create grip_pose FAILED %d, absolute wrist unavailable", (int)ir); g_act_gpose = XR_NULL_HANDLE; }

        aci = {XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy_s(aci.actionName, "trigger");
        strcpy_s(aci.localizedActionName, "Trigger");
        aci.countSubactionPaths = 2;
        aci.subactionPaths = g_hand;
        ir = xrCreateAction(g_actionset, &aci, &g_act_trig);
        if (XR_FAILED(ir)) { LOG_INFO("VR: 13b create trigger FAILED %d, input disabled", (int)ir); return false; }

        // Build 14e.2: the grip (squeeze) value, the aim-steer gate. The
        // trigger stays plumbed for a future fire binding.
        aci = {XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy_s(aci.actionName, "grip");
        strcpy_s(aci.localizedActionName, "Grip");
        aci.countSubactionPaths = 2;
        aci.subactionPaths = g_hand;
        ir = xrCreateAction(g_actionset, &aci, &g_act_grip);
        if (XR_FAILED(ir)) { LOG_INFO("VR: 14e create grip FAILED %d, input disabled", (int)ir); return false; }

        // Build 22: sticks and buttons for the XInput merge. One creation
        // failure disables input wholesale, same policy as above.
        struct BDef { XrAction* act; XrActionType type; const char* name;
                      const char* loc; bool both; };
        const BDef defs[] = {
            {&g_act_stick,    XR_ACTION_TYPE_VECTOR2F_INPUT, "stick",      "Thumbstick",       true},
            {&g_act_stickclk, XR_ACTION_TYPE_BOOLEAN_INPUT,  "stickclick", "Thumbstick click", true},
            {&g_act_btn_a,    XR_ACTION_TYPE_BOOLEAN_INPUT,  "btn_a",      "A",                false},
            {&g_act_btn_b,    XR_ACTION_TYPE_BOOLEAN_INPUT,  "btn_b",      "B",                false},
            {&g_act_btn_x,    XR_ACTION_TYPE_BOOLEAN_INPUT,  "btn_x",      "X",                false},
            {&g_act_btn_y,    XR_ACTION_TYPE_BOOLEAN_INPUT,  "btn_y",      "Y",                false},
            {&g_act_menu,     XR_ACTION_TYPE_BOOLEAN_INPUT,  "menu",       "Menu",             false},
        };
        for (const BDef& d : defs) {
            aci = {XR_TYPE_ACTION_CREATE_INFO};
            aci.actionType = d.type;
            strcpy_s(aci.actionName, d.name);
            strcpy_s(aci.localizedActionName, d.loc);
            if (d.both) { aci.countSubactionPaths = 2; aci.subactionPaths = g_hand; }
            ir = xrCreateAction(g_actionset, &aci, d.act);
            if (XR_FAILED(ir)) { LOG_INFO("VR: 22 create %s FAILED %d, input disabled", d.name, (int)ir); return false; }
        }

        // Oculus Touch bindings: the aim pose (runtime-defined forward ray of
        // the controller, exactly what gun direction wants), trigger value,
        // and grip (squeeze) value.
        XrPath prof, p_aim[2], p_trig[2], p_grip[2];
        XrPath p_gpose[2] = {XR_NULL_PATH, XR_NULL_PATH};   // BUILD 162
        XrPath p_stick[2], p_stickclk[2], p_a, p_b, p_x, p_y, p_menu;
        if (XR_FAILED(xrStringToPath(g_instance, "/interaction_profiles/oculus/touch_controller", &prof)) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/aim/pose",       &p_aim[0]))  ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right/input/aim/pose",      &p_aim[1]))  ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/trigger/value",  &p_trig[0])) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right/input/trigger/value", &p_trig[1])) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/squeeze/value",  &p_grip[0])) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right/input/squeeze/value", &p_grip[1])) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/thumbstick",        &p_stick[0]))    ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right/input/thumbstick",       &p_stick[1]))    ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/thumbstick/click",  &p_stickclk[0])) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right/input/thumbstick/click", &p_stickclk[1])) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right/input/a/click",          &p_a))           ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right/input/b/click",          &p_b))           ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/x/click",           &p_x))           ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/y/click",           &p_y))           ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/menu/click",        &p_menu))) {
            LOG_INFO("VR: 13b binding paths FAILED, input disabled");
            return false;
        }
        // BUILD 162: the grip pose paths, non-fatal. The two grip-pose rows
        // sit LAST in the table so a count of 15 leaves them out.
        const bool gpose_ok = g_act_gpose != XR_NULL_HANDLE &&
            XR_SUCCEEDED(xrStringToPath(g_instance, "/user/hand/left/input/grip/pose",  &p_gpose[0])) &&
            XR_SUCCEEDED(xrStringToPath(g_instance, "/user/hand/right/input/grip/pose", &p_gpose[1]));
        if (!gpose_ok) LOG_INFO("VR: 162 grip pose paths unavailable, absolute wrist unavailable");
        XrActionSuggestedBinding sb[17] = {
            {g_act_aim,      p_aim[0]},      {g_act_aim,      p_aim[1]},
            {g_act_trig,     p_trig[0]},     {g_act_trig,     p_trig[1]},
            {g_act_grip,     p_grip[0]},     {g_act_grip,     p_grip[1]},
            {g_act_stick,    p_stick[0]},    {g_act_stick,    p_stick[1]},
            {g_act_stickclk, p_stickclk[0]}, {g_act_stickclk, p_stickclk[1]},
            {g_act_btn_a,    p_a},           {g_act_btn_b,    p_b},
            {g_act_btn_x,    p_x},           {g_act_btn_y,    p_y},
            {g_act_menu,     p_menu},
            {g_act_gpose,    p_gpose[0]},    {g_act_gpose,    p_gpose[1]},
        };
        XrInteractionProfileSuggestedBinding ipsb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        ipsb.interactionProfile = prof;
        ipsb.countSuggestedBindings = gpose_ok ? 17 : 15;
        ipsb.suggestedBindings = sb;
        ir = xrSuggestInteractionProfileBindings(g_instance, &ipsb);
        if (XR_FAILED(ir)) { LOG_INFO("VR: 13b suggest bindings FAILED %d, input disabled", (int)ir); return false; }

        XrSessionActionSetsAttachInfo sai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        sai.countActionSets = 1;
        sai.actionSets = &g_actionset;
        ir = xrAttachSessionActionSets(g_session, &sai);
        if (XR_FAILED(ir)) { LOG_INFO("VR: 13b attach action sets FAILED %d, input disabled", (int)ir); return false; }

        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo aspci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            aspci.action = g_act_aim;
            aspci.subactionPath = g_hand[h];
            aspci.poseInActionSpace.orientation.w = 1.0f;
            ir = xrCreateActionSpace(g_session, &aspci, &g_aim_space[h]);
            if (XR_FAILED(ir)) { LOG_INFO("VR: 13b action space %d FAILED %d, input disabled", h, (int)ir); return false; }
        }
        // BUILD 162: a space per hand for the grip pose, non-fatal.
        if (gpose_ok) {
            for (int h = 0; h < 2; ++h) {
                XrActionSpaceCreateInfo gsci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                gsci.action = g_act_gpose;
                gsci.subactionPath = g_hand[h];
                gsci.poseInActionSpace.orientation.w = 1.0f;
                if (XR_FAILED(xrCreateActionSpace(g_session, &gsci, &g_grip_space[h]))) {
                    LOG_INFO("VR: 162 grip pose space %d FAILED, absolute wrist unavailable", h);
                    g_grip_space[h] = XR_NULL_HANDLE;
                }
            }
        }
        LOG_INFO("VR: 13b input plumbing up (aim pose + trigger + grip, both hands, Touch profile; grip pose %s)",
                 (gpose_ok && g_grip_space[0] && g_grip_space[1]) ? "bound" : "UNBOUND");
        return true;
    }();

    // --- swapchain sized to the game's backbuffer ---
    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &fc, nullptr);
    std::vector<int64_t> formats(fc);
    xrEnumerateSwapchainFormats(g_session, fc, &fc, formats.data());
    int64_t chosen = formats.empty() ? (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM : formats[0];
    for (auto f : formats) {
        if ((DXGI_FORMAT)f == st.format) { chosen = f; break; }
    }

    // BUILD 10L: swapchain creation is DEFERRED to the first present with
    // valid views. The canvas dimensions and per-eye placement offsets need
    // the frustum tangents, and xrLocateViews only yields those once frame
    // timing exists. Store what deferred creation needs.
    g_sc_format    = chosen;
    g_content_w    = st.width;
    g_content_h    = st.height;
    g_canvas_ready = false;
    LOG_INFO("VR: canvas swapchains deferred to the first frame (10L), format %lld",
             (long long)chosen);

    // BUILD 11a: shaders and pipeline states for the scaling blit, created
    // once here (rule 8, hazard 20). Failure is loud and leaves the 10L
    // fixed-copy path in charge.
    g_blit_ok = create_blit_resources(st);

    // BUILD 10i: the per-eye last-frame copies, same size/format as the
    // swapchain images. BUILD 11a: when the blit is armed these are also its
    // SOURCE, so they get a full mip chain (GenerateMips at capture) and a
    // shader resource view; scoped fovs minify the content 4-5x and trilinear
    // needs the mips to not shimmer. Without the blit they stay the plain
    // 10i re-copy textures.
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width  = st.width;
        td.Height = st.height;
        td.MipLevels = g_blit_ok ? 0 : 1;   // 0 = full chain
        td.ArraySize = 1;
        td.Format = st.format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        if (g_blit_ok) {
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        }
        for (int eye = 0; eye < 2; ++eye) {
            if (FAILED(st.device->CreateTexture2D(&td, nullptr, &g_last[eye]))) {
                g_last[eye] = nullptr;
            }
            if (g_blit_ok && g_last[eye] &&
                FAILED(st.device->CreateShaderResourceView(g_last[eye], nullptr,
                                                           &g_last_srv[eye]))) {
                g_last_srv[eye] = nullptr;
            }
        }
        if (g_blit_ok &&
            (!g_last[0] || !g_last[1] || !g_last_srv[0] || !g_last_srv[1])) {
            // The blit needs both sources; fall back wholesale to the 10L
            // fixed copy with plain 10i textures.
            LOG_ERROR("VR: 11a blit source textures failed; falling back to fixed placement.");
            for (int eye = 0; eye < 2; ++eye) {
                if (g_last_srv[eye]) { g_last_srv[eye]->Release(); g_last_srv[eye] = nullptr; }
                if (g_last[eye])     { g_last[eye]->Release();     g_last[eye]     = nullptr; }
            }
            g_blit_ok = false;
            td.MipLevels = 1;
            td.BindFlags = 0;
            td.MiscFlags = 0;
            for (int eye = 0; eye < 2; ++eye) {
                if (FAILED(st.device->CreateTexture2D(&td, nullptr, &g_last[eye]))) {
                    g_last[eye] = nullptr;
                }
            }
        }
        // 11a.2: the probe staging texture (1x1, CPU-readable), created here
        // once; a null just skips the probe.
        {
            D3D11_TEXTURE2D_DESC pd{};
            pd.Width = 1;
            pd.Height = 1;
            pd.MipLevels = 1;
            pd.ArraySize = 1;
            pd.Format = st.format;
            pd.SampleDesc.Count = 1;
            pd.Usage = D3D11_USAGE_STAGING;
            pd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(st.device->CreateTexture2D(&pd, nullptr, &g_probe_tex)))
                g_probe_tex = nullptr;
        }

        if (g_blit_ok) {
            LOG_INFO("VR: 11a scaling blit armed: live-fov placement, mip-filtered source.");
        } else if (g_last[0] && g_last[1]) {
            LOG_INFO("VR: 11a blit UNAVAILABLE; 10L fixed pi/4 placement, stale eye re-copied.");
        } else {
            LOG_INFO("VR: 10i last-frame texture creation FAILED; stale eye falls back to re-reference.");
        }
    }

    // --- begin session ---
    // Session must reach READY before begin; poll briefly for the event.
    for (int i = 0; i < 400 && g_state_xr != XR_SESSION_STATE_READY; ++i) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                g_state_xr = ((XrEventDataSessionStateChanged*)&ev)->state;
                LOG_INFO("VR: session state -> %s", state_name(g_state_xr));
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (g_state_xr != XR_SESSION_STATE_READY) Sleep(25);
    }
    // BUILD 38: not reaching READY in ten seconds is NOT a failure, it is a
    // headset that is asleep, idle in the Oculus home, or still coming up over
    // Link. Until now this marked VR dead for the whole run (g_failed latched,
    // and dllmain only ever called init once), so launching the game before
    // the headset was awake meant restarting the game. Now the session is left
    // alive and poll_start(), called once a second from the init thread, begins
    // it the moment READY arrives.
    if (g_state_xr != XR_SESSION_STATE_READY) {
        LOG_WARN("VR: session is %s, not READY yet (headset asleep, idle in "
                 "the Oculus home, or Link still coming up). NOT giving up: "
                 "put the headset on and the mirror arms automatically.",
                 state_name(g_state_xr));
        g_pending.store(true);
        return false;
    }
    return begin_and_arm();
}

// BUILD 38: the tail of init, split out so a deferred start can reuse it.
bool begin_and_arm() {
    XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    const XrResult r = xrBeginSession(g_session, &sbi);
    if (XR_FAILED(r)) return fail("xrBeginSession", r);

    LOG_INFO("VR: session begun. Submitting the game frame to the headset.");
    LOG_INFO("VR: build 11f, big flat scope (%.2f rad window) + FP demo", g_scope_disp_fov);
    // Build 96: the full live block, not three of it. This line had listed only
    // Home, 8 and Decimal since build 21 while 1, 3, 4 and 7 were added around
    // it, so the one place a tester could check what a key does was wrong about
    // more keys than it was right about.
    // Build 104: Numpad 4 removed from this list because the key is gone. It
    // armed the spawn-direction write with no cfg key, and that write lands in
    // a cosmetic trail object, so it was a hotkey for a retracted lever.
    // BUILD 129b: this banner is now generated from what the keys ACTUALLY do.
    // Build 129 moved six digit keys to the preset loader and left this text
    // advertising the old map, so the log told the tester to press Numpad 8
    // for first person when Numpad 8 had become an empty preset slot. A stale
    // key banner is worse than no banner: it is the mod lying about itself.
    LOG_INFO("VR: keys: F1 panel, F2 first person, F3 ARM CALIBRATION (press "
             "it, let go, then stand with both controllers straight out to "
             "your sides at shoulder height and hold; it speaks when it "
             "fires, applies the arm scale and puts you in first person), "
             "Home recenter (Space too, but Space also vaults), Numpad 1..9 "
             "then 0 LOAD A WHOLE PRESET from GRWVR\\presets (each one says "
             "its name), Numpad Decimal head aim, Numpad Minus palette "
             "capture.");
    LOG_INFO("VR: what moved in build 129: barrel aim, ADS, eye swap and the "
             "camera pose write are Settings rows in the F1 panel; the "
             "weapon-draw recorder and first person are Captures rows. The "
             "numpad digits are presets now.");
    LOG_INFO("VR: tuning keys: Numpad / world BIGGER, Numpad * world SMALLER "
             "(ipd_scale, 1.00 = LIFE SIZE, 0.05 a step, clamped 0.00..3.00), "
             "Numpad + head roll "
             "into the camera (cam_pose_rot, DIAGNOSTIC: it also makes the "
             "camera fight yaw and pitch). Saving grwxr.cfg restores its "
             "values.");
    LOG_INFO("VR: END starts a GUIDED SPOKEN test run: it clears the "
             "capture table, talks you through drawing each weapon, and "
             "tells you when to quit. voice = 0 in grwxr.cfg silences it.");
    LOG_INFO("VR: THE TUNER (build 99): INSERT cycles which setting is live, "
             "PAGE UP and PAGE DOWN step it, DELETE resets it. Every press logs "
             "the name, the value and what it changes. Start with "
             "wgun_grip_fwd to seat the gun in your hand, then wgun_grip_two "
             "for the front-hand hold.");
    LOG_INFO("VR: all tuning lives in grwxr.cfg, hot-reloaded ~1 s after any save");
    LOG_INFO("VR: (edit by hand or with cfg_gui.exe). Build 21.");
    LOG_INFO("VR: fullscreen %s, fov %.2f rad; desktop view %s, crop %.2f rad",
             headpose::fs_enabled() ? "ON" : "OFF", headpose::fs_fov(),
             g_desk_on ? "ON" : "OFF", g_desk_fov);
    LOG_INFO("VR: Judge in the headset; the desktop mirror vibrates by design.");
    g_active.store(true);
    g_pending.store(false);
    return true;
}

// Build 38: called once a second from the init thread while a created session
// has not begun. Drains session events and begins the moment the runtime says
// READY, so waking the headset after launch arms the mirror with no relaunch.
// Returns true exactly once, on the tick the session actually begins.
bool poll_start() {
    if (!g_pending.load() || g_failed.load() || g_session == XR_NULL_HANDLE)
        return false;
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            g_state_xr = ((XrEventDataSessionStateChanged*)&ev)->state;
            LOG_INFO("VR: session state -> %s", state_name(g_state_xr));
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (g_state_xr == XR_SESSION_STATE_EXITING ||
        g_state_xr == XR_SESSION_STATE_LOSS_PENDING) {
        LOG_ERROR("VR: runtime ended the session before it began (%s). "
                  "Restart the game with the headset awake.",
                  state_name(g_state_xr));
        g_pending.store(false);
        g_failed.store(true);
        return false;
    }
    if (g_state_xr != XR_SESSION_STATE_READY) return false;
    LOG_INFO("VR: headset is awake, session reached READY. Arming now.");
    return begin_and_arm();
}

// ---------------------------------------------------------------------------
// The frame-wait watchdog, 2026-08-12.
//
// xrWaitFrame BLOCKS by design: it is the runtime's frame throttle. Until this
// build it was called directly on the GAME'S RENDER THREAD, from inside our
// Present hook. So a runtime that never returns from it never lets the game
// return from Present either, and the game freezes hard after exactly one
// frame while our own heartbeat thread carries on logging.
//
// That is the exact signature in every failing Virtual Desktop log:
//
//     hb 12  frames=1  (+1 in the last second)
//     hb 13  frames=1  (+0 in the last second)      <- and forever after
//     head compose: idle (no head pose published)
//
// It localises to this call and nowhere else, because D3D11Hook.cpp does
// frames++ BEFORE it calls us, and the head pose is published eight lines
// AFTER the wait returns. frames stuck at 1 with no head pose ever published
// means the thread went in and did not come out. It is not the exclusive
// fullscreen upsize (that run had upsize disabled and windowed confirmed),
// not the adapter (LUIDs matched), and not the canvas swapchains (created
// further down, after xrBeginFrame, never reached).
//
// A thread already blocked inside the runtime cannot be rescued, so the only
// fix that keeps the game alive is to never make the call on the render
// thread. With xr_async_wait=1 the wait runs on a worker and the render
// thread waits on it with a timeout. On timeout we skip VR for that frame and
// return, and the game keeps presenting normally: a flat game beats a frozen
// one.
//
// The healthy path is unchanged in ORDER: one xrWaitFrame, then xrBeginFrame
// and xrEndFrame on the render thread, exactly as before. Only the thread the
// wait runs on differs, which is the pipelined pattern the OpenXR frame
// timing chapter describes. A new wait is only ever requested when none is in
// flight, so the one-to-one ratio with xrBeginFrame is preserved as well.
//
// Default OFF, so every setup that works today is bit-for-bit unaffected
// until this is confirmed in a headset.
HANDLE                g_wait_thread  = nullptr;
HANDLE                g_wait_request = nullptr;   // render -> worker
HANDLE                g_wait_done    = nullptr;   // worker -> render
std::atomic<bool>     g_wait_exit{false};
std::atomic<bool>     g_wait_inflight{false};
std::atomic<bool>     g_wait_ok{false};
std::atomic<unsigned> g_wait_stalled{0};
XrFrameState          g_wait_result{XR_TYPE_FRAME_STATE};

DWORD WINAPI frame_wait_thread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_wait_request, INFINITE);
        if (g_wait_exit.load(std::memory_order_acquire)) break;

        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState    fs{XR_TYPE_FRAME_STATE};
        // This is the call that may never return. It is now the only thing
        // this thread does, and this thread owns nothing the game needs.
        const XrResult r = xrWaitFrame(g_session, &fwi, &fs);

        g_wait_result = fs;
        g_wait_ok.store(XR_SUCCEEDED(r), std::memory_order_release);
        SetEvent(g_wait_done);
    }
    return 0;
}

// Idempotent, called from the render thread. Failure here is not fatal: the
// caller falls back to waiting inline, which is the pre-existing behaviour.
bool start_wait_thread() {
    if (g_wait_thread) return true;

    if (!g_wait_request) g_wait_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wait_done)    g_wait_done    = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
    if (!g_wait_request || !g_wait_done) return false;

    g_wait_thread = CreateThread(nullptr, 0, frame_wait_thread, nullptr, 0, nullptr);
    return g_wait_thread != nullptr;
}

// True when fs holds a frame state and the caller may proceed into
// xrBeginFrame. False means "skip VR this frame and let the game present".
bool acquire_frame_state(XrFrameState& fs) {
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};

    if (g_cfg_async_wait.load(std::memory_order_relaxed) == 0 ||
        !start_wait_thread()) {
        fs = XrFrameState{XR_TYPE_FRAME_STATE};
        return XR_SUCCEEDED(xrWaitFrame(g_session, &fwi, &fs));
    }

    // Only ask for a new wait when the previous one actually came back.
    if (!g_wait_inflight.exchange(true)) {
        ResetEvent(g_wait_done);
        SetEvent(g_wait_request);
    }

    const DWORD ms = (DWORD)g_cfg_wait_timeout.load(std::memory_order_relaxed);
    if (WaitForSingleObject(g_wait_done, ms) != WAIT_OBJECT_0) {
        // Still inside the runtime. Leave it in flight and give the frame back
        // to the game. note() defers the write off this thread (rule 8).
        const unsigned n = g_wait_stalled.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 60) {
            note("VR: xrWaitFrame has not returned for %u frames (%lu ms each). "
                 "The runtime is not releasing the frame. VR is standing by and "
                 "the GAME KEEPS RUNNING; before this build the game froze here.",
                 n, (unsigned long)ms);
        }
        return false;
    }

    g_wait_inflight.store(false, std::memory_order_release);
    if (g_wait_stalled.exchange(0, std::memory_order_relaxed) >= 60) {
        note("VR: xrWaitFrame returned again. Resuming submission to the headset.");
    }

    fs = g_wait_result;
    return g_wait_ok.load(std::memory_order_acquire);
}

// BUILD 203: the level pose. Build 202 used a swing-twist decomposition about
// the view's own Z axis, which is NOT the roll against gravity: a level head
// looking left and down has a large "twist" in that decomposition (45 degrees
// at yaw 90 and pitch 45, rolltest203.py), so the image was rotated by it and
// the corners went black: "why is it on a screen again? its like the fov has
// be tightened and the head stracking is worse" (2026-09-06 19:13, build 202
// REFUTED, rolled back by cfg). The pose the content deserves is the head's
// yaw and pitch with no roll: the frame whose forward is the head's forward
// and whose right axis is horizontal in the reference space (LOCAL space, +Y
// up). right = f x up, up' = right x f, R = [right | up' | -f], then the
// quaternion. Exact for every yaw and pitch (residual 0.0000 degrees over
// 20000 random heads in the test); the roll it removes is reported on the 1 Hz
// line. Looking within about three degrees of straight up or down the roll is
// undefined and the pose passes through untouched.
static XrQuaternionf strip_roll_xr(XrQuaternionf q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    // the head's forward in the reference space: q rotates the local (0,0,-1)
    const float v[3] = {0.0f, 0.0f, -1.0f};
    const float cx = y * v[2] - z * v[1], cy = z * v[0] - x * v[2], cz = x * v[1] - y * v[0];
    const float f[3] = {v[0] + 2.0f * (w * cx + y * cz - z * cy),
                        v[1] + 2.0f * (w * cy + z * cx - x * cz),
                        v[2] + 2.0f * (w * cz + x * cy - y * cx)};
    // right = f x up with up = +Y: (-f.z, 0, f.x)
    float r[3] = {-f[2], 0.0f, f[0]};
    const float rm = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (!(rm > 0.05f) || !isfinite(rm)) return q;    // straight up or down: no defined roll
    r[0] /= rm; r[2] /= rm;
    // up' = right x f
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    // R columns: local X -> r, local Y -> u, local Z -> -f; M[row][col]
    const float M00 = r[0], M01 = u[0], M02 = -f[0];
    const float M10 = r[1], M11 = u[1], M12 = -f[1];
    const float M20 = r[2], M21 = u[2], M22 = -f[2];
    XrQuaternionf s;
    const float tr = M00 + M11 + M22;
    if (tr > 0.0f) {
        const float sq = sqrtf(tr + 1.0f) * 2.0f;
        s.w = 0.25f * sq; s.x = (M21 - M12) / sq; s.y = (M02 - M20) / sq; s.z = (M10 - M01) / sq;
    } else if (M00 > M11 && M00 > M22) {
        const float sq = sqrtf(1.0f + M00 - M11 - M22) * 2.0f;
        s.w = (M21 - M12) / sq; s.x = 0.25f * sq; s.y = (M01 + M10) / sq; s.z = (M02 + M20) / sq;
    } else if (M11 > M22) {
        const float sq = sqrtf(1.0f + M11 - M00 - M22) * 2.0f;
        s.w = (M02 - M20) / sq; s.x = (M01 + M10) / sq; s.y = 0.25f * sq; s.z = (M12 + M21) / sq;
    } else {
        const float sq = sqrtf(1.0f + M22 - M00 - M11) * 2.0f;
        s.w = (M10 - M01) / sq; s.x = (M02 + M20) / sq; s.y = (M12 + M21) / sq; s.z = 0.25f * sq;
    }
    const float m = sqrtf(s.x * s.x + s.y * s.y + s.z * s.z + s.w * s.w);
    if (!(m > 1e-6f) || !isfinite(m)) return q;
    s.x /= m; s.y /= m; s.z /= m; s.w /= m;
    // the roll removed: q = s * d with d about the local forward (0,0,-1);
    // d = conj(s) * q, its signed angle about that axis is 2 * atan2(-d.z, d.w)
    const float dw = s.w * w + s.x * x + s.y * y + s.z * z;
    const float dz = s.w * z - s.x * y + s.y * x - s.z * w;
    g_roll_last_deg.store(2.0f * atan2f(-dz, dw) * 57.29578f, std::memory_order_relaxed);
    g_roll_strips.fetch_add(1, std::memory_order_relaxed);
    return s;
}

static void roll_log() {
    LOG_INFO("roll: fix=%d strips=%llu last=%+.1f deg (the roll removed from the submitted "
             "pose at the last submit; with the fix off nothing is removed and strips stays)",
             g_head_roll_fix.load(std::memory_order_relaxed),
             (unsigned long long)g_roll_strips.load(std::memory_order_relaxed),
             g_roll_last_deg.load(std::memory_order_relaxed));
}

static void on_present_body(const d3d11::State& st);

// ===========================================================================================
// BUILD 247: TRUE STEREO SUBMISSION (cfg cam2_stereo; Cam2.h has the contract).
//
// One game frame holds BOTH eyes: camera 0 rendered the left half of the backbuffer, camera 2 the right
// half. Each half is copied into its own half-size texture (mip chain, like g_last) and drawn into its eye's
// canvas by the same scaling blit the alternate-eye path uses, at the content's own angular size: the
// vertical fov is the one the projection hook published (fs_fov in the world band, the engine's own in a
// zoom), the horizontal extent follows the half's aspect, so the world scale is what it was. Both layer views
// are fresh, carry the pose each image was RENDERED with (the tag's head orientation, the per-eye position of
// the one xrLocateViews of this frame), and share the frame's predicted display time because there is one
// xrEndFrame. Nothing here logs, locks or allocates per frame except the one-time half-texture creation
// (init-shaped, like create_canvases, attempted at most three times).
//
// WHY THE CONTENT-FOV ROUTE AND NOT THE ENGINE'S PER-CAMERA FRUSTUM WINDOW (camera+0x324). The window would
// give each eye the asymmetric OpenXR frustum straight from the engine, but the world's fov is carried by the
// hooked projection argument and the sky, the cascades and the culling frustum read other derivations of the
// camera: an off-centre window on them is untested and would have to be right in every consumer. The
// symmetric wide content already covers a headset eye's frustum (tan 1.27 each way at fs_fov 1.92 and a
// 3840x2160 backbuffer, against about 1.2 of the widest eye of a Quest 3), the placement machinery is the
// one that has been in the headset since build 11a, and the world scale cannot change because the vertical fov
// is the same number the alternate-eye path publishes and places by.
// ===========================================================================================
static ID3D11Texture2D*          g_half[2]     = {nullptr, nullptr};
static ID3D11ShaderResourceView* g_half_srv[2] = {nullptr, nullptr};
static uint32_t                  g_half_w = 0, g_half_h = 0;
static int                       g_half_tries = 0;
static bool                      g_stereo_prev_halves = false;

// Creates (or re-creates after a size change) the two half-size sources. Render thread, init-shaped work.
static bool ensure_half(const d3d11::State& st, uint32_t full_w, uint32_t full_h) {
    const uint32_t hw = full_w / 2;
    if (g_half[0] && g_half[1] && g_half_srv[0] && g_half_srv[1] && g_half_w == hw && g_half_h == full_h) return true;
    if (g_half_tries >= 3) return false;
    ++g_half_tries;
    for (int i = 0; i < 2; ++i) {
        if (g_half_srv[i]) { g_half_srv[i]->Release(); g_half_srv[i] = nullptr; }
        if (g_half[i])     { g_half[i]->Release();     g_half[i]     = nullptr; }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width  = hw;
    td.Height = full_h;
    td.MipLevels = 0;                                   // full chain, GenerateMips at capture (as g_last)
    td.ArraySize = 1;
    td.Format = st.format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) {
        if (FAILED(st.device->CreateTexture2D(&td, nullptr, &g_half[i])) ||
            FAILED(st.device->CreateShaderResourceView(g_half[i], nullptr, &g_half_srv[i])))
            ok = false;
    }
    if (!ok) {
        for (int i = 0; i < 2; ++i) {
            if (g_half_srv[i]) { g_half_srv[i]->Release(); g_half_srv[i] = nullptr; }
            if (g_half[i])     { g_half[i]->Release();     g_half[i]     = nullptr; }
        }
        g_half_w = g_half_h = 0;
        note("stereo2: creating the two %ux%u half-size eye textures FAILED (attempt %d of 3)", hw, full_h, g_half_tries);
        return false;
    }
    g_half_w = hw;
    g_half_h = full_h;
    note("stereo2: eye textures created, %ux%u each (half of the %ux%u backbuffer), mip chain, format %u", hw, full_h, full_w, full_h, (unsigned)st.format);
    return true;
}

// BUILD 250: the runtime's predicted display time for this present and the previous one (ns), for the recorder's rows.
static int64_t g_dbg_pdt = 0, g_dbg_pdt_prev = 0;

// BUILD 251: THE LAYER POSE LAG. Measured on build 250's recording (6709 frames, both eyes): the picture a present shows was drawn
// with the head orientation located at the present THREE earlier, at every head speed and in every window (corr 0.99-1.00 when
// turning, the left and right eyes agree with each other at 0.99). The tag carries the orientation published to the engine when the
// frame was built, which is the present ONE earlier (19 per cent of frames: two earlier), so the layer was labelled 1-2 presents
// newer than its picture and by an amount that varied: the compositor's reprojection of that error is the jitter on head turns.
// With cfg layer_pose_lag = N the stereo layer carries the orientation located at the present N earlier instead. 0 = the tag's, as
// before. A ring of the last 16 located head orientations, stamped with the present counter they were located at.
static XrQuaternionf     g_qh_ring[16];
static uint64_t          g_qh_ring_frame[16] = {};
static std::atomic<uint32_t> g_lag_used{0}, g_lag_miss{0};

static void stereo_pack_views(const XrView views[2], float pos[2][3], float ori[2][4], float fov[2][4]) {
    for (int e = 0; e < 2; ++e) {
        pos[e][0] = views[e].pose.position.x;    pos[e][1] = views[e].pose.position.y;    pos[e][2] = views[e].pose.position.z;
        ori[e][0] = views[e].pose.orientation.x; ori[e][1] = views[e].pose.orientation.y;
        ori[e][2] = views[e].pose.orientation.z; ori[e][3] = views[e].pose.orientation.w;
        fov[e][0] = views[e].fov.angleLeft; fov[e][1] = views[e].fov.angleRight;
        fov[e][2] = views[e].fov.angleUp;   fov[e][3] = views[e].fov.angleDown;
    }
}

// 0 = not a usable halves frame (the caller runs the alternate-eye path), 1 = both eyes drawn and the layer
// filled, 2 = attempted and failed after drawing began (the caller submits nothing).
//   true_stereo: the right half is camera 2's (a trusted stereo frame). False: the left half goes to both eyes.
static int submit_halves(const d3d11::State& st, ID3D11Texture2D* back, const XrView views[2], bool true_stereo,
                         bool head_ori_ok, const XrQuaternionf& head_ori, bool built_q_ok, const float built_q[4],
                         float cfov, int tag,
                         XrCompositionLayerProjectionView pviews[2], XrCompositionLayerProjection& layer) {
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    if (bd.Width != g_content_w || bd.Height != g_content_h || bd.SampleDesc.Count != 1 || (bd.Width & 1u) != 0u || bd.Width < 64u) {
        cam2::stereo_note_reason("the backbuffer is not the size the mod initialised on, or is odd-width or multisampled: no half-split is possible");
        return 0;
    }
    if (!g_canvas_ready || !g_blit_ok || !st.context) {
        cam2::stereo_note_reason("the canvases or the scaling blit are not ready");
        return 0;
    }
    if (!ensure_half(st, bd.Width, bd.Height)) {
        cam2::stereo_note_reason("the half-size eye textures could not be created");
        return 0;
    }
    const uint32_t hw = g_half_w, hh = g_half_h;
    float aspect = cam2::stereo_aspect_override();
    if (!(aspect > 0.0f)) aspect = (float)hw / (float)hh;

    // The two halves. Source rects: the left half (0,0,W/2,H) and the right half (W/2,0,W/2,H) of the backbuffer.
    const int nsrc = true_stereo ? 2 : 1;
    for (int i = 0; i < nsrc; ++i) {
        const D3D11_BOX box{(UINT)(i * hw), 0u, 0u, (UINT)((i + 1) * hw), (UINT)hh, 1u};
        st.context->CopySubresourceRegion(g_half[i], 0, 0, 0, 0, back, 0, &box);
        st.context->GenerateMips(g_half_srv[i]);
    }

    // The poses each image was rendered with: the tag's head orientation (else the located one), and each
    // eye's located position. The roll strip is the alternate path's, for both views.
    XrPosef pose[2] = {views[0].pose, views[1].pose};
    gtrec::PRow* const gp = gtrec::p_stage();   // BUILD 270: the gtrec P row (log only), nullptr unless recording
    for (int e = 0; e < 2; ++e) {
        // BUILD 270: which override below set this eye's label, and the Present count it was taken at (gtrec only)
        uint32_t lsrc  = gtrec::kLsLocatedView;
        uint32_t lfrom = gp ? (uint32_t)d3d11::frame_count() : 0u;
        if (head_ori_ok) { pose[e].orientation = head_ori; lsrc = gtrec::kLsHead; }
        if (built_q_ok)  { pose[e].orientation = {built_q[0], built_q[1], built_q[2], built_q[3]}; lsrc = gtrec::kLsTag; lfrom = 0xFFFFFFFFu; }
        // BUILD 251: true stereo only: the orientation the picture was actually drawn with (see g_cfg_layer_lag).
        // BUILD 266: in a vehicle the picture is drawn with the located head from exactly TWO presents earlier: measured on the build 265
        // recording (eyerec-6472, 22 windows of 45 frames of head motion, best lag 2 in 21 and 1 in the other; on foot the same method gives 3,
        // which is build 251's number). The 265 label, the tag, matched the picture worse (it is the located head from 1 or 2 presents earlier,
        // varying): yaw correlation 0.880 and residual p95 1.76 degrees, against 0.937 and 1.48 for lag 2 and 0.829 and 2.17 for lag 3.
        // So while the vehicle compose is on, the layer carries the ring's orientation from cfg veh_layer_lag presents earlier (default 2).
        // BUILD 267: while the recompute hook drives the vehicle frame the label is the exact head quaternion of the build that
        // was composed at Present count (now - veh_build_depth); a miss falls back to the 266 lag estimate, never shifts a queue.
        bool have_vq = false;
        if (true_stereo && camera::veh_hook_drives()) {
            float vq[4];
            const uint64_t fc = d3d11::frame_count();
            const uint64_t dep = (uint64_t)g_cfg_veh_depth.load(std::memory_order_relaxed);
            if (fc >= dep && camera::veh_build_label(fc - dep, vq)) {
                pose[e].orientation = {vq[0], vq[1], vq[2], vq[3]};
                have_vq = true;
                if (e == 0) g_lag_used.fetch_add(1, std::memory_order_relaxed);
                lsrc  = gtrec::kLsBuildLabel;   // BUILD 270
                lfrom = (uint32_t)(fc - dep);
            }
        }
        const int lag = camera::veh_on() ? g_cfg_veh_lag.load(std::memory_order_relaxed) : g_cfg_layer_lag.load(std::memory_order_relaxed);
        if (!have_vq && true_stereo && lag > 0 && lag < 15) {
            const uint64_t want = d3d11::frame_count() - (uint64_t)lag;
            if (g_qh_ring_frame[want & 15u] == want) {
                pose[e].orientation = g_qh_ring[want & 15u];
                if (e == 0) g_lag_used.fetch_add(1, std::memory_order_relaxed);
                lsrc  = gtrec::kLsLagRing;   // BUILD 270
                lfrom = (uint32_t)want;
            } else if (e == 0) {
                g_lag_miss.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (g_head_roll_fix.load(std::memory_order_relaxed)) {
            pose[e].orientation = strip_roll_xr(pose[e].orientation);
            lsrc |= gtrec::kLsStripped;   // BUILD 270
            if (gp && e == 0) gp->roll_removed = g_roll_last_deg.load(std::memory_order_relaxed);
        }
        if (gp) {   // BUILD 270
            gp->label_src[e]  = lsrc;
            gp->label_from[e] = lfrom;
        }
    }

    const bool swap = cam2::stereo_swap_eyes();
    bool all_ok = true;
    float used_th[2] = {0.0f, 0.0f}, used_tv[2] = {0.0f, 0.0f};
    bool  own_cam[2] = {false, false};
    ID3D11ShaderResourceView* rec_src[2] = {nullptr, nullptr};
    const bool aspect_forced = cam2::stereo_aspect_override() > 0.0f;
    for (int e = 0; e < 2; ++e) {
        const int srci = true_stereo ? (swap ? (e ^ 1) : e) : 0;
        g_pip_atx_eye[e] = g_pip_atx;     // BUILD 237: the disc's source centre travels with its picture
        g_pip_aty_eye[e] = g_pip_aty;
        // BUILD 248: each eye is placed at the angular size ITS OWN camera rendered with (the camera's projection,
        // Camera+0x420), not the one shared last-writer fov the two cameras used to fight over. A frame whose camera
        // cannot be read falls back to the shared fov and aspect, as before.
        float eye_fovy = cfov, eye_aspect = aspect;
        if (true_stereo && !aspect_forced) {
            float th = 0.0f, tv = 0.0f;
            const uint64_t cam = (srci == 0) ? cam2::first_camera() : cam2::second_camera();
            if (cam2::camera_proj_tans(cam, &th, &tv)) {
                eye_fovy = 2.0f * atanf(tv);
                eye_aspect = th / tv;
                own_cam[e] = true;
            }
        }
        if (gp) {   // BUILD 270
            gp->eye_fovy[e] = eye_fovy;
            if (own_cam[e]) gp->fl |= (e == 0) ? (1u << 12) : (1u << 13);
        }
        rec_src[e] = g_half_srv[srci];
        if (blit_into(e, st.context, eye_fovy, g_half_srv[srci], eye_aspect, &used_th[e], &used_tv[e])) {
            g_eye[e] = {pose[e], views[e].fov, true, eye_fovy};
        } else {
            g_eye[e].valid = false;
            all_ok = false;
        }
    }
    cam2::stereo_note_place(used_th, used_tv, own_cam);
    if (gp) {   // BUILD 270: the placement each eye used
        for (int e = 0; e < 2; ++e) {
            gp->th[e] = used_th[e];
            gp->tv[e] = used_tv[e];
        }
        if (eyerec::enabled()) gp->fl |= 1u << 11;
    }
    cam2::stereo_note_lag(g_cfg_layer_lag.load(std::memory_order_relaxed), g_lag_used.load(std::memory_order_relaxed), g_lag_miss.load(std::memory_order_relaxed));
    float pp[2][3], oo[2][4], ff[2][4];
    stereo_pack_views(views, pp, oo, ff);
    cam2::stereo_note_frame(all_ok ? (true_stereo ? cam2::kStereoBoth : cam2::kStereoMonoLeft) : cam2::kStereoFailed, tag,
                            d3d11::frame_count(), pp, oo, ff, cfov, hw, hh, aspect, true);
    if (!all_ok) return 2;
    // BUILD 248: the eye recorder, trusted stereo frames only (never a menu or a loading frame: those are not
    // true_stereo). One relaxed load when off. The blit state is current, the eye placement is final.
    if (true_stereo && eyerec::enabled()) {
        char meta[480];
        // BUILD 250: qb = the orientation this frame was BUILT with (the one the layer carries), qh = the head orientation located
        // at this present, dtms = the step of the predicted display time since the previous present. The recorder analysis compares
        // the picture's own motion between rows with the rotation between their qb values.
        const int n0 = snprintf(meta, sizeof(meta), "ql=%.5f;%.5f;%.5f;%.5f lag=%d qb=%.5f;%.5f;%.5f;%.5f qh=%.5f;%.5f;%.5f;%.5f dtms=%.2f bq=%d ho=%d ",
                 (double)pose[0].orientation.x, (double)pose[0].orientation.y, (double)pose[0].orientation.z, (double)pose[0].orientation.w,
                 g_cfg_layer_lag.load(std::memory_order_relaxed),
                 built_q_ok ? (double)built_q[0] : 0.0, built_q_ok ? (double)built_q[1] : 0.0, built_q_ok ? (double)built_q[2] : 0.0, built_q_ok ? (double)built_q[3] : 1.0,
                 (double)head_ori.x, (double)head_ori.y, (double)head_ori.z, (double)head_ori.w,
                 (double)(g_dbg_pdt - g_dbg_pdt_prev) / 1.0e6, built_q_ok ? 1 : 0, head_ori_ok ? 1 : 0);
        snprintf(meta + (n0 > 0 ? n0 : 0), sizeof(meta) - (size_t)(n0 > 0 ? n0 : 0), "tag=%d th0=%.3f tv0=%.3f th1=%.3f tv1=%.3f own=%d%d gap=%.4f cfov=%.3f",
                 tag, (double)used_th[0], (double)used_tv[0], (double)used_th[1], (double)used_tv[1],
                 own_cam[0] ? 1 : 0, own_cam[1] ? 1 : 0,
                 (double)std::sqrt((views[1].pose.position.x - views[0].pose.position.x) * (views[1].pose.position.x - views[0].pose.position.x) +
                                   (views[1].pose.position.y - views[0].pose.position.y) * (views[1].pose.position.y - views[0].pose.position.y) +
                                   (views[1].pose.position.z - views[0].pose.position.z) * (views[1].pose.position.z - views[0].pose.position.z)),
                 (double)cfov);
        eyerec::record(st.context, rec_src, used_th, used_tv, g_cx, g_cy, g_sx, g_sy, g_sc_w, g_sc_h, d3d11::frame_count(), meta);
        st.context->PSSetShader(g_blit_ps, nullptr, 0);
    }
    for (int i = 0; i < 2; ++i) {
        pviews[i].pose = g_eye[i].pose;
        pviews[i].fov  = g_eye[i].fov;
        pviews[i].subImage.swapchain = g_swapchains[i];
        pviews[i].subImage.imageRect.offset = {0, 0};
        pviews[i].subImage.imageRect.extent = {(int32_t)g_sc_w, (int32_t)g_sc_h};
        pviews[i].subImage.imageArrayIndex  = 0;
    }
    layer.layerFlags = 0;
    layer.space      = g_space;
    layer.viewCount  = 2;
    layer.views      = pviews;
    return 1;
}

// BUILD 215: the present cadence and the whole hook are timed (CX-029). The
// body below is unchanged; the wrapper reads the performance counter at entry
// (the interval since the last Present is the frame time, and the presents per
// second are the exact frame rate) and around the body.
// ---------------------------------------------------------------------------
// BUILD 235: THE FIRST FRAME CANNOT KILL THE GAME ANY MORE.
//
// Every failing machine on record dies the same way: the game's render thread
// enters Present once and never comes back. D3D11Hook.cpp:106 counts ENTRIES,
// so frames=1 is not one frame drawn, it is one frame entered. And because our
// callback never returns, the hook never reaches g_orig_present, so the game
// itself never presents again. The window stops answering, and eight to nine
// seconds later something outside the process kills it. The user sees a black
// screen and their launcher, with nothing to report.
//
// A watchdog thread cannot rescue that: the stuck thread is the game's own and
// there is no safe way to unstick a thread parked inside a runtime call. So
// bound it instead. The FIRST frame, and only the first, runs on a worker
// thread while the render thread waits with a deadline. Nothing is concurrent:
// the render thread does nothing but wait, so exactly one thread touches the
// device, which is the same guarantee the inline path gives.
//
//   worker finishes in time  -> disarm, and every later frame runs inline with
//                               no thread, no wait and no cost. One frame's
//                               hand-off, once per launch.
//   worker does not          -> take the thread snapshot (StallWatch.h) so the
//                               log NAMES what is holding it, stand VR down,
//                               and return so the game presents flat and keeps
//                               running.
//
// The honest cost of the second branch: the worker is abandoned where it sits.
// It is stuck inside a runtime call, and if that call is touching the device
// when the game resumes, the two could collide. That is a risk. Today's
// behaviour is not a risk, it is a certainty: the process dies every time. A
// logged, switchable fallback that usually saves the session beats that, and
// xr_firstframe_ms = 0 restores the old unbounded behaviour exactly.
//
// The deadline defaults to 5000 ms deliberately. The measured gap between
// "session begun" and the process being killed is 8.1 to 9.1 s across every
// failing run, and the best numeric fit for that is Windows TDR (TdrDelay 2 s
// plus TdrDdiDelay 5 s). Giving up at 5 s means we act BEFORE the thing that
// kills the process, not after it. The slowest healthy first frame we have
// ever recorded is about 2 s, on the RTX 5090 run that works.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_ff_settled{false};   // first frame decided, guard off
static d3d11::State      g_ff_state;            // the worker's copy of the state
static HANDLE            g_ff_done = nullptr;

static DWORD WINAPI ff_worker(LPVOID) {
    // The guard is constructed HERE, on the thread that actually draws, so the
    // foveation hook still recognises our own render-target binds as ours.
    ffr::PresentGuard pg;
    on_present_body(g_ff_state);
    SetEvent(g_ff_done);
    return 0;
}

static void first_frame_guarded(const d3d11::State& st) {
    const int ms = g_cfg_firstframe_ms.load(std::memory_order_relaxed);

    // Nothing to guard until there is a session to wedge on, and no reason to
    // pay for a thread hand-off on the frames before VR arms.
    if (ms <= 0 || g_session == XR_NULL_HANDLE || g_dead) {
        ffr::PresentGuard pg;
        on_present_body(st);
        if (g_session != XR_NULL_HANDLE) g_ff_settled.store(true, std::memory_order_relaxed);
        return;
    }

    g_ff_state = st;
    if (!g_ff_done) g_ff_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE th = g_ff_done ? CreateThread(nullptr, 0, ff_worker, nullptr, 0, nullptr)
                          : nullptr;
    if (!th) {
        // Could not get a thread. Run it inline rather than skip the frame:
        // that is exactly today's behaviour and no worse than it.
        LOG_WARN("xrff: could not start the first-frame worker (error %lu); "
                 "running it inline, unguarded, as every build before 235 did.",
                 GetLastError());
        if (th) CloseHandle(th);
        ffr::PresentGuard pg;
        on_present_body(st);
        g_ff_settled.store(true, std::memory_order_relaxed);
        return;
    }

    const DWORD w = WaitForSingleObject(g_ff_done, (DWORD)ms);
    if (w == WAIT_OBJECT_0) {
        WaitForSingleObject(th, 1000);   // it has signalled; this just reaps it
        CloseHandle(th);
        CloseHandle(g_ff_done);
        g_ff_done = nullptr;
        g_ff_settled.store(true, std::memory_order_relaxed);
        LOG_INFO("xrff: the first VR frame completed and the guard is off. "
                 "Every frame from here runs inline, as before.");
        return;
    }

    // It did not come back. Take the reading FIRST, while it is still parked
    // exactly where it stuck, then stand down.
    LOG_ERROR("xrff: THE FIRST VR FRAME HAS NOT RETURNED IN %d ms. This is the "
              "failure that kills the game on startup. Reading every thread "
              "before standing down.", ms);

    // BUILD 236: the three facts a reported log must carry side by side. The
    // phase is read FIRST and from the atomic alone, because it is the one
    // that is still moving: it names the last OpenXR step the frame worker
    // entered, which is as close to "the call it is parked in" as this process
    // can get without the stack walk below.
    LOG_ERROR("xrff: the frame worker was last inside: %s", frame_phase());
    log_layer_survivors("when the first frame stalled");

    stallwatch::report("the first VR frame did not return");

    g_failed.store(true);
    g_dead = true;
    g_ff_settled.store(true, std::memory_order_relaxed);
    CloseHandle(th);   // the worker is abandoned where it sits; see the block above

    LOG_ERROR("xrff: VR IS OFF FOR THIS RUN. THE GAME WILL NOW CARRY ON "
              "NORMALLY ON YOUR MONITOR. Nothing is broken and nothing needs "
              "reinstalling.");
    LOG_ERROR("xrff: the thread snapshot above names what was holding it. If a "
              "foreign DLL appears there (ReShade64.dll, XR_APILAYER_*.dll), "
              "switch that layer off and VR will start: its registry value is "
              "under HKEY_LOCAL_MACHINE\\SOFTWARE\\Khronos\\OpenXR\\1\\"
              "ApiLayers\\Implicit, set to 1.");
    LOG_ERROR("xrff: xr_firstframe_ms = 0 in grwxr.cfg restores the old "
              "behaviour, where this hung the game instead.");
}

// BUILD 239: the header. Called from the heartbeat thread by the stall
// watchdog, which is the one thread still ticking through every wedge on
// record. It cannot unstick the render thread and does not try: it makes sure
// that IF that thread returns, it finds VR already off and presents flat,
// instead of walking into a session nobody is driving.
void stand_down(const char* why) {
    if (g_dead) return;
    g_failed.store(true);
    g_dead = true;
    g_ff_settled.store(true, std::memory_order_relaxed);   // the guard has nothing left to guard
    LOG_ERROR("VR: STOOD DOWN (%s). If the game comes back it will run flat on "
              "your monitor from here. Nothing is broken and nothing needs "
              "reinstalling.", why ? why : "no reason given");
}

int init_frames() { return g_cfg_init_frames.load(std::memory_order_relaxed); }

void on_present(const d3d11::State& st) {
    perf::mark_present();
    perf::Scope sc(perf::kPresent);
    ph(kPhBody);

    // BUILD 235: the first frame is bounded; after that this is the old path
    // exactly, with one relaxed atomic load in front of it.
    if (!g_ff_settled.load(std::memory_order_relaxed)) {
        first_frame_guarded(st);
        return;
    }

    ffr::PresentGuard pg;   // BUILD 216: render-target binds inside our own Present are ours
    on_present_body(st);
}

static void on_present_body(const d3d11::State& st) {
    // Build 8.1: the session survives a doff. Taking the headset off sends
    // STOPPING; build 8 stopped polling events there, never called
    // xrEndSession, and so left the runtime showing a frozen frame with no way
    // back. Now STOPPING parks the session (xrEndSession, stop submitting) but
    // events keep draining every frame, and the READY that arrives when the
    // headset is put back on re-begins the session with a FRESH head
    // reference, since the headset has physically moved.
    if (g_session == XR_NULL_HANDLE || g_dead) return;
    if (!st.device || !st.context || !st.swapchain) return;

    {   // BUILD 216: publish what the foveation mask is built from (the content
        // size, the wide field, both eyes' frustum tangents) and clear the rate
        // on the immediate context before anything of ours draws. Eight tanf
        // per frame; the mask itself is built on the init thread.
        float et[2][4];
        for (int e = 0; e < 2; ++e) {
            et[e][0] = tanf(g_eyefov[e].angleLeft);
            et[e][1] = tanf(g_eyefov[e].angleRight);
            et[e][2] = tanf(g_eyefov[e].angleUp);
            et[e][3] = tanf(g_eyefov[e].angleDown);
        }
        ph(kPhFfrTick);
        ffr::present_tick(st.context, g_content_w, g_content_h, et, g_canvas_ready, headpose::fs_fov());
    }

    ph(kPhSamples);
    camera::survive_sample();   // BUILD 172: log only, 1 Hz, reads only
    camera::gun_survive_sample();   // BUILD 184: the gun root's records, same rules
    camera::sprint_sample();        // BUILD 200: the sprint probe, 10 Hz, reads only

    // Drain session events cheaply, active or parked.
    ph(kPhEnter);
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    ph(kPhPollEvent);
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            g_state_xr = ((XrEventDataSessionStateChanged*)&ev)->state;
            note("VR: session state -> %s", state_name(g_state_xr));
            if (g_state_xr == XR_SESSION_STATE_STOPPING) {
                headpose::disable();
                g_active.store(false);
                ph(kPhEndSession);
                xrEndSession(g_session);
            } else if (g_state_xr == XR_SESSION_STATE_READY &&
                       !g_active.load(std::memory_order_relaxed)) {
                XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
                sbi.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                ph(kPhBeginSession);
                if (XR_SUCCEEDED(xrBeginSession(g_session, &sbi))) {
                    g_have_ref = false;
                    g_rc_begin.fetch_add(1, std::memory_order_relaxed);   // BUILD 212
                    g_aim_resync.store(true, std::memory_order_relaxed);
                    // Build 10a: the pre-doff eye images and poses are stale
                    // in every sense; bootstrap both eyes afresh.
                    g_eye[0].valid = false;
                    g_eye[1].valid = false;
                    // Build 93: and so is the TAG RING, which build 10a missed.
                    // The pop sits after the !g_active early return, so the
                    // present that handled STOPPING returned without popping
                    // while pushes had already stopped. Depth is 1 at almost
                    // any instant, so a doff leaves exactly one stale tag in
                    // flight and every pop after this point is off by one,
                    // which SWAPS THE EYES for the rest of the session with no
                    // counter moving. See headpose::reset_eye_tags().
                    headpose::reset_eye_tags();
                    g_active.store(true);
                } else {
                    g_dead = true;
                }
            } else if (g_state_xr == XR_SESSION_STATE_EXITING ||
                       g_state_xr == XR_SESSION_STATE_LOSS_PENDING) {
                headpose::disable();
                g_active.store(false);
                g_dead = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // Build 10a.1 took any reference space change as a user recenter and
            // recaptured everything. BUILD 212: which space changed decides, and
            // a LOCAL change is ABSORBED rather than thrown away.
            //
            // The 211 run (grwxr-18104.log) took six references in 110 seconds
            // off ONE F2 press: four re-zeroes nobody asked for, each of them
            // re-taking the yaw reference, which turns the world to wherever the
            // head points, and each of them also resetting the standing height
            // (the block in update_head), which moves the view vertically off the
            // character's head and stretches the arm past its reach. We hold three
            // spaces (LOCAL to compose in, VIEW, and STAGE for the stance loop
            // since build 205), and the old code answered a change of ANY of them
            // by re-zeroing the compose reference.
            //
            // The event carries poseInPreviousSpace: the new space's origin
            // expressed in the old one. Transforming our two references by its
            // inverse leaves the head-relative offset and the head-to-reference
            // rotation bit-identical, so the view does not move at all
            // (followtest212.py: 2e-15 m and 6e-16 over 20000 poses; the yaw is
            // all that is taken from it, which is exact while the runtime keeps
            // gravity, and a 4 degree tilt would leave a few degrees).
            const XrEventDataReferenceSpaceChangePending* rc =
                (const XrEventDataReferenceSpaceChangePending*)&ev;
            if (rc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                if (rc->poseValid && g_have_ref) {
                    const XrPosef& pp = rc->poseInPreviousSpace;
                    const Quat pq{pp.orientation.x, pp.orientation.y, pp.orientation.z, pp.orientation.w};
                    // the yaw of the change, the same formula the reference uses
                    const float pfx = -2.0f * (pq.x * pq.z + pq.w * pq.y);
                    const float pfz = -(1.0f - 2.0f * (pq.x * pq.x + pq.y * pq.y));
                    const float psi_p = atan2f(-pfx, -pfz);
                    // g_ref_inv is the conjugate of a yaw-only quaternion
                    const float psi_r = -2.0f * atan2f(g_ref_inv.y, g_ref_inv.w);
                    const float psi_n = psi_r - psi_p;
                    g_ref_inv = Quat{0.0f, -sinf(psi_n * 0.5f), 0.0f, cosf(psi_n * 0.5f)};
                    if (g_head_ref_ok) {
                        const float v[3] = {g_head_ref_pos[0] - pp.position.x,
                                            g_head_ref_pos[1] - pp.position.y,
                                            g_head_ref_pos[2] - pp.position.z};
                        float outp[3];
                        quat_rotate_inv(pq, v, outp);   // conj(pq) applied to v
                        if (isfinite(outp[0]) && isfinite(outp[1]) && isfinite(outp[2])) {
                            g_head_ref_pos[0] = outp[0];
                            g_head_ref_pos[1] = outp[1];
                            g_head_ref_pos[2] = outp[2];
                        }
                    }
                    g_eye[0].valid = false;
                    g_eye[1].valid = false;
                    g_rc_absorbed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_have_ref = false;
                    g_aim_resync.store(true, std::memory_order_relaxed);
                    g_eye[0].valid = false;
                    g_eye[1].valid = false;
                    g_rc_local.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (rc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE) {
                // the floor moved under the stance loop. The height model re-takes
                // its standing reference; the yaw and the 6DOF origin are not the
                // runtime's business and are left exactly as they are.
                g_st_dzmax = 0.0f; g_st_auto_sum = 0.0; g_st_auto_n = 0; g_st_auto_done = false;
                g_st_ref_ms = GetTickCount64();
                g_rc_stage.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_rc_other.fetch_add(1, std::memory_order_relaxed);
            }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    // Build 10a.1: manual recenter on the Home key, edge-triggered. The head
    // reference was captured from wherever the headset pointed at launch
    // (often the desk), which bakes that angle into the camera as a constant
    // offset. Home re-captures it at the current head pose: look where you
    // want "forward" to be and press it. Only the camera compose reference
    // resets; the layer poses are raw LOCAL space and unaffected.
    {
        static bool s_was_down = false;
        // BUILD 129 (owner request, 2026-08-22): "recenter spacebar or
        // something easy when the game is focused". Space is added BESIDE
        // Home, not instead of it.
        //
        // KNOWN AND ACCEPTED SIDE EFFECT: the mod polls the keyboard, it does
        // not intercept it. There is no WndProc hook in this tree (the window
        // in D3D11Hook.cpp is a throwaway for device creation), so the game
        // sees the same keypress and Space is bound in game. Every recenter on
        // Space also vaults or jumps. Home does not. Both are live; use
        // whichever wins by feel.
        const bool down = (GetAsyncKeyState(VK_HOME)  & 0x8000) != 0 ||
                          (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        const bool edge = (down && !s_was_down) ||
                          menu::fire_pending(menu::kProbeRecenter);
        if (edge) {
            g_have_ref = false;
            g_rc_key.fetch_add(1, std::memory_order_relaxed);   // BUILD 212
            g_aim_resync.store(true, std::memory_order_relaxed);
            note("VR: recenter requested (Home, Space or the panel). Head "
                 "reference will re-capture.");
        }
        s_was_down = down;
    }

    // Build 21: every numpad TUNING key is removed. Tuning now lives in
    // grwxr.cfg, hot-reloaded about a second after any save (poll_config on
    // the init thread), edited by hand or with tools/cfg_gui. Only the three
    // PLAY toggles remain as keys: Home (recenter, above), Numpad 8 (first
    // person, below) and Numpad Decimal (head aim, below). Removed: 9 - *
    // (ipd), 7 4 6 5 3 0 (first-person offsets), 1 2 + (fullscreen fov),
    // / (desktop view).

    // Build 11c/15e/16a: first-person toggle, on Numpad 8.
    //
    // BUILD 129: THE KEY IS GONE, the toggle is not. Numpad 8 is a preset slot
    // now (dllmain.cpp explains the whole digit row), and first person is the
    // "First person toggle" row on the panel's Captures page, which has been
    // there since the probe table was written and is reachable by controller
    // as of build 128.
    //
    // Build 44's reason for folding a recenter into the toggle still stands (a
    // stale head reference cost a run on 2026-08-05), and it is not lost: Home
    // has always recentered on its own, and build 129 adds Space beside it.
    //
    // What must NOT move out of this poll is the head-hide republish below.
    {
        // BUILD 129b: F2 TOGGLES FIRST PERSON, because 129 took Numpad 8 for a
        // preset slot and left the tester with no key at all.
        //
        // `[VERIFIED, log grwxr-8232]` he pressed Numpad 8 twice and got
        // "preset 8 is empty" both times, and `[VERIFIED, grwxr-25296]` the
        // next whole run reports `hide: idle (fp off)` on every status line:
        // first person was never on, so the camera sat on the third-person
        // chase rig for the entire session and read as the camera jumping.
        // Build 129's startup banner still advertised Numpad 8 for it, so the
        // mod told him to press a key it had just repurposed.
        //
        // F2 and not a numpad digit, so all ten preset slots survive: he
        // already reaches for F1 for the panel, and F2 is the next key along.
        // The panel's Captures row still fires it and is untested.
        static bool s_f2 = false;
        const bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        const bool f2_edge = f2 && !s_f2;
        s_f2 = f2;
        // 2026-09-01: F3 ARMS THE WINGSPAN CAPTURE, the owner's design. F1 is
        // the panel, F2 is first person, F3 is the third key along and can be
        // found by feel with the headset on. It exists because the capture
        // wants both arms straight out to the sides while the old trigger was
        // a row in a menu, which cannot both be true, so every sample was
        // taken mid-reach. The 3.5 s timer is what buys the time to let go and
        // strike the pose, and a good capture then applies the scale and asks
        // for first person on its own.
        static bool s_f3 = false;
        const bool f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3 && !s_f3) camera::set_hand_calib(1);
        {   // BUILD 207: F4 calibrates the standing height on its own (stand normally; four seconds from one second after the press)
            static bool s_f4 = false;
            const bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
            if (f4 && !s_f4) stance_calib_start();
            s_f4 = f4;
        }
        {   // BUILD 216: F7 toggles foveated rendering at run time, for an A/B in the headset (the ffr: line counts it)
            static bool s_f7 = false;
            const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            if (f7 && !s_f7) ffr::toggle();
            s_f7 = f7;
        }
        s_f3 = f3;

        // A successful capture asks for first person. Folded into the SAME
        // branch F2 uses rather than duplicated, so the recenter that has been
        // part of entering first person since build 44 comes with it, and
        // there is only ever one first-person path to reason about. Forces ON
        // rather than toggling: the capture means "put me there", not "flip".
        const bool fp_req = camera::take_fp_request() && !headpose::fp_enabled();
        if (menu::fire_pending(menu::kProbeFirstPerson) || f2_edge || fp_req) {
            const bool on = fp_req ? true : !headpose::fp_enabled();
            headpose::set_fp_enabled(on);
            // BUILD 225: a deliberate off holds fp_auto until the pin changes.
            if (!fp_req) camera::set_fp_user_off(on ? 0 : 1);
            g_rc_fp.fetch_add(1, std::memory_order_relaxed);   // BUILD 212
            // BUILD 44 (user request): the first-person toggle ALSO recenters.
            // A stale head reference is invisible until it has already ruined
            // a test (2026-08-05: the reference was ~110 deg off, which read
            // as "the character faces hard to the left" and cost a run before
            // the log identified it). Folding the recenter into the toggle
            // means the reference is always freshly captured at the moment you
            // enter first person, looking where you are actually looking.
            g_have_ref = false;
            g_aim_resync.store(true, std::memory_order_relaxed);
            note("VR: first person %s, and RECENTERED (the head reference "
                 "re-captures from where you are looking now).",
                 on ? "ON" : "OFF");
        }
        // Build 18: the head-hide override simply follows the FP state.
        // Re-published every poll (not only on the edge) so the detour is
        // correct even if FP was toggled before the hook finished installing.
        camera::set_head_hide(headpose::fp_enabled());
        // BUILD 224: hold the hidden state against the engine's show-all
        // path (the helmet after a roll, a climb, cover, a vehicle exit).
        camera::head_hide_frame();
    }
    // BUILD 125: Numpad 9 flips the EYE SIGN, live.
    //
    // This is the A/B build 93 asked for and that no log line can settle. It
    // is a hotkey rather than a cfg key because a fusion judgement cannot
    // survive a relaunch: the eyes adapt over seconds, so the two states have
    // to be compared back to back on the same view, standing still.
    //
    // It SPEAKS, because the tester cannot read a log or a screen mid-test.
    //
    // BUILD 129: THE KEY IS GONE, the A/B is not. Numpad 9 is a preset slot
    // now. The swap is the "Swap the eyes  <<< TRY THIS FIRST" row that build
    // 125b put on the panel, and the cfg key ipd_swap means a preset can carry
    // either sign, which is a better A/B than the key was: both signs can now
    // be a whole config each, loaded back to back on one key press.
    //
    // The panel row already speaks through the same voice path.
    // Build 19: Numpad Decimal toggles VR HEAD AIM (repurposed from the
    // build-17 one-shot experiment, which it supersedes). Default off.
    {
        static bool s_yb = false;
        // BUILD 41: arm motion aim once, automatically, as soon as the setter
        // hooks exist. Numpad Decimal still toggles it off and on afterwards;
        // cfg aim_on_start=0 restores the old start-disabled behaviour.
        if (!g_vraim_armed_once.load(std::memory_order_relaxed) &&
            g_aim_on_start.load(std::memory_order_relaxed) == 1 &&
            camera::aim_available()) {
            g_vraim_armed_once.store(true, std::memory_order_relaxed);
            g_vraim_on = true;
            note("aim: VR AIM ARMED AUTOMATICALLY at startup (source: %s). "
                 "Numpad Decimal toggles it; aim_on_start=0 in grwxr.cfg "
                 "starts it off instead.",
                 g_aim_source.load(std::memory_order_relaxed) == 1
                     ? "right controller" : "head");
        }
        const bool yb = (GetAsyncKeyState(VK_DECIMAL) & 0x8000) != 0;
        if (yb && !s_yb) {
            if (!camera::aim_available()) {
                note("aim: VR head aim unavailable (setter hooks not installed)");
            } else {
                g_vraim_on = !g_vraim_on;
                const bool ctl = g_aim_source.load(std::memory_order_relaxed) == 1;
                note("aim: VR AIM %s (source: %s; aim_source/aim_ctrl_smooth "
                     "in grwxr.cfg)",
                     g_vraim_on
                         ? (ctl ? "ON: bullets follow the right controller"
                                : "ON: bullets follow your gaze")
                         : "OFF",
                     ctl ? "controller" : "head");
            }
        }
        s_yb = yb;
    }
    if (g_dead || !g_active.load(std::memory_order_relaxed)) {
        // BUILD 14f: a parked or dead session must not hold ADS (doff safety).
        // BUILD 14h: nor fire.
        if (g_ads_rmb.load(std::memory_order_relaxed)) ads_send(false);
        if (g_fire_lmb.load(std::memory_order_relaxed)) fire_send(false);
        // Build 22: nor keep feeding the pad merge a stale snapshot.
        headpose::set_touch_pad(0, nullptr, false);
        return;
    }

    // Build 10b.1: exactly one tag pop per present, mirroring the one push
    // per built frame on the camera side. -1 means no camera write built this
    // frame (menus, loading, VR-inactive stretches): the image is mono.
    // Build 13a: the tag also delivers the XR-space orientation the frame was
    // composed with; the submit path below prefers it over a present-time
    // locate (render-pose submit, see HeadPose.h).
    float built_q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool  built_q_ok = false;
    // BUILD 247: the tag's flags come back too (bit 0x08 = the second camera was not trusted yet when this
    // frame was built). The eye field is 0/1 for an alternate-eye frame, 2 for a true-stereo frame, -1 for none.
    unsigned char built_flags = 0;
    const int built_eye = headpose::pop_eye_tag(built_q, &built_q_ok, nullptr, &built_flags);
    // BUILD 270: the gtrec P row (log only). p_begin opens the static staging row only while recording (one atomic
    // load otherwise); the fields below are filled in place where they become known, and the guard commits the row on
    // every return from here on. gp is nullptr unless recording.
    gtrec::p_begin(d3d11::frame_count(), built_eye, built_q, built_q_ok, built_flags);
    gtrec::PScope gtrec_commit;
    gtrec::PRow* const gp = gtrec::p_stage();
    // BUILD 247: foveation is suspended while the two cameras share one target (Foveate.h, set_suspended).
    ffr::set_suspended(cam2::stereo_live());

    // 2026-08-12: was a bare xrWaitFrame on this, the game's render thread.
    // See the watchdog block above on_present(). Everything below is reached
    // only with a frame state in hand, exactly as before.
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    bool have_fs = false;
    {   // BUILD 215: the runtime's wait, timed apart from our own work
        perf::Scope sc(perf::kWait);
        ph(kPhWaitFrame);
        have_fs = acquire_frame_state(fs);
        if (have_fs) { g_dbg_pdt_prev = g_dbg_pdt; g_dbg_pdt = (int64_t)fs.predictedDisplayTime; }   // BUILD 250
    }
    if (gp) {   // BUILD 270
        gp->qpc_wait = gtrec::qpc_now();
        if (have_fs) {
            gp->pdt = (int64_t)fs.predictedDisplayTime;
            gp->pdp = (int64_t)fs.predictedDisplayPeriod;
            if (fs.shouldRender) gp->fl |= 1u << 3;
        }
    }
    if (!have_fs) return;

    // Build 8: head pose for this frame's predicted display time, published to
    // the camera hook. No logging here (rule 8); note() defers.
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    bool head_ori_ok = false;
    XrQuaternionf head_ori{0.0f, 0.0f, 0.0f, 1.0f};
    ph(kPhLocateView);
    if (XR_SUCCEEDED(xrLocateSpace(g_view_space, g_space,
                                   fs.predictedDisplayTime, &loc))) {
        // Build 42: the head POSITION, needed to place the hand markers
        // relative to the eyes. Orientation-only tracking is common while a
        // headset settles, so this is flagged separately and the markers
        // simply do not draw until it is valid.
        if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
            g_head_pos[0] = loc.pose.position.x;
            g_head_pos[1] = loc.pose.position.y;
            g_head_pos[2] = loc.pose.position.z;
            g_head_pos_ok = true;
        } else {
            g_head_pos_ok = false;
        }
        update_head(loc.locationFlags, loc.pose.orientation);
        // BUILD 204: TRUE 6DOF. The head's displacement since the reference,
        // head-local (the conversion the hands ride: rotate by the inverse of
        // the current head orientation; +x right, +y up, -z forward), clamped
        // to head_pos_max, published for the camera hook. The reference
        // position is captured the frame the yaw reference is, so Home (a
        // recenter) re-zeroes both. Published false whenever the key is off,
        // nothing is tracked, or no reference exists: the hook then adds 0.
        {
            // BUILD 207: the capture lives in update_head with the yaw reference;
            // this is the late capture for a present whose position arrived
            // after the orientation, and the reset while no reference exists.
            if (!g_have_ref) g_head_ref_ok = false;
            if (g_have_ref && !g_head_ref_ok && g_head_pos_ok) {
                g_head_ref_pos[0] = g_head_pos[0];
                g_head_ref_pos[1] = g_head_pos[1];
                g_head_ref_pos[2] = g_head_pos[2];
                g_head_ref_ok = true;
                g_hp_refs.fetch_add(1, std::memory_order_relaxed);
                g_st_ref_ms = GetTickCount64();
            }
            float lx = 0.0f, lf = 0.0f, lu = 0.0f, uh = 0.0f;
            float dxr[3] = {0.0f, 0.0f, 0.0f};   // BUILD 210: the XR displacement, for the follow's own horizontal frames
            bool hok = false, use_h = false;
            if (g_head_pos_on.load(std::memory_order_relaxed) && g_head_ref_ok && g_head_pos_ok &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                const Quat qh{loc.pose.orientation.x, loc.pose.orientation.y,
                              loc.pose.orientation.z, loc.pose.orientation.w};
                float d[3] = {g_head_pos[0] - g_head_ref_pos[0],
                              g_head_pos[1] - g_head_ref_pos[1],
                              g_head_pos[2] - g_head_ref_pos[2]};
                const bool lean_mode = g_lean_mode.load(std::memory_order_relaxed) != 0;
                if (lean_mode) {
                    // BUILD 300: the soft limit, in the tracking frame (y up) before the head-local conversion, so a pitched head cannot
                    // change what counts as horizontal. Slope 1 up to where the easing begins, then tanh to the limit: no wall.
                    auto soft = [](float m, float lim, float frac) {
                        const float s0 = lim * frac, wd = lim - s0;
                        if (m <= s0) return m;
                        if (!(wd > 1e-4f)) return lim;
                        return s0 + wd * tanhf((m - s0) / wd);
                    };
                    const float mxh = g_lean_max.load(std::memory_order_relaxed);
                    const float fr  = g_lean_soft.load(std::memory_order_relaxed);
                    const float mh  = sqrtf(d[0] * d[0] + d[2] * d[2]);
                    if (mh > mxh * fr) g_lean_soft_n.fetch_add(1, std::memory_order_relaxed);
                    if (mh > 1e-6f) {
                        const float kh = soft(mh, mxh, fr) / mh;
                        d[0] *= kh; d[2] *= kh;
                    }
                    const float lim = d[1] < 0.0f ? g_lean_down.load(std::memory_order_relaxed) : g_lean_up.load(std::memory_order_relaxed);
                    d[1] = d[1] < 0.0f ? -soft(-d[1], lim, fr) : soft(d[1], lim, fr);
                }
                float lv[3];
                quat_rotate_inv(qh, d, lv);   // head-local: +x right, +y up, -z forward
                lx = lv[0]; lf = -lv[2]; lu = lv[1];
                dxr[0] = d[0]; dxr[1] = d[1]; dxr[2] = d[2];   // BUILD 210
                // BUILD 205 and 207: with the stance loop on, the eyes sit at the
                // PHYSICAL height: (height minus standing) minus (bone minus
                // standing bone), so a crouch does not drop the view twice. It
                // is a world-vertical quantity and travels to the hook as its
                // own term (uh, use_h) that rides world up; lu keeps riding the
                // camera basis with x and f. Not in a seat (no bone to trust).
                {
                    float dz = 0.0f;
                    if (g_stance_on.load(std::memory_order_relaxed) && headpose::head_dz(&dz) && !g_st_seated &&
                        g_st_hstand > 1.0f && g_st_dzmax > 0.5f && isfinite(g_st_height)) {
                        uh = (g_st_height - g_st_hstand) - (dz - g_st_dzmax);
                        use_h = true;
                    }
                }
                // BUILD 205: the clamp is horizontal; the height has its own.
                const float mag = sqrtf(lx * lx + lf * lf);
                const float mx = g_head_pos_max.load(std::memory_order_relaxed);
                if (!lean_mode && mag > mx && mag > 1e-6f) {   // BUILD 300: the lean has its own soft limit above
                    const float k = mx / mag;
                    lx *= k; lf *= k;
                    g_hp_clamps.fetch_add(1, std::memory_order_relaxed);
                }
                if (lu > 1.5f) lu = 1.5f;
                if (lu < -1.5f) lu = -1.5f;
                if (uh > 1.5f) uh = 1.5f;
                if (uh < -1.5f) uh = -1.5f;
                hok = isfinite(lx) && isfinite(lf) && isfinite(lu);
                if (hok) {
                    g_hp_frames.fetch_add(1, std::memory_order_relaxed);
                    g_hp_last[0].store(lx, std::memory_order_relaxed);
                    g_hp_last[1].store(lf, std::memory_order_relaxed);
                    g_hp_last[2].store(lu, std::memory_order_relaxed);
                }
            }
            headpose::set_head_offset(lx, lf, lu, uh, use_h, hok);
            // BUILD 209: the body follow, from the same offset and the same head
            if (hok) {
                const Quat qf{loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
                follow_tick(dxr, qf, built_q_ok, g_st_seated);
            } else {
                headpose::set_follow_stick(0.0f, 0.0f, false);
            }
        }
        // BUILD 205: THE STANCE LOOP. The physical head height above the floor
        // (STAGE, or LOCAL plus 1.65 m), the standing height as the running
        // maximum since the recenter (or cfg stance_height), the desired stance
        // from the ratio with hysteresis; the actual stance from the character's
        // head bone (1.6 standing, 1.1 crouched, 0.3 prone); while they disagree
        // and the loop is on, in first person, anchored, the pad's B: a tap for
        // one step (down to crouch, or up), a hold for prone. A settle after
        // every act; four acts without a change back the loop off for three
        // seconds. The head's own height must come first: the loop reads what
        // this frame published, the merge sees the button on its next poll.
        {
            // the physical height
            float height = 0.0f;
            bool hok2 = false;
            if (g_stage_space != XR_NULL_HANDLE) {
                XrSpaceLocation sl{XR_TYPE_SPACE_LOCATION};
                ph(kPhLocateStage);
                if (XR_SUCCEEDED(xrLocateSpace(g_view_space, g_stage_space, fs.predictedDisplayTime, &sl)) &&
                    (sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                    height = sl.pose.position.y;
                    hok2 = isfinite(height) && height > 0.05f && height < 2.6f;
                    g_st_stage_ok = hok2;
                }
            }
            if (!hok2 && g_head_pos_ok) {
                height = g_head_pos[1] + 1.65f;
                hok2 = isfinite(height) && height > 0.05f && height < 2.6f;
                g_st_stage_ok = false;
            }
            if (hok2) {
                g_st_height = height;
                const uint64_t tnow = GetTickCount64();
                // BUILD 206: the F3 window, four seconds from one second after the press
                if (g_st_cal_until) {
                    if (tnow >= g_st_cal_from && tnow < g_st_cal_until) {
                        g_st_cal_sum += height; ++g_st_cal_n;
                    } else if (tnow >= g_st_cal_until) {
                        if (g_st_cal_n >= 60 && g_st_cal_sum / g_st_cal_n > 0.5 && g_st_cal_sum / g_st_cal_n < 2.4) {   // BUILD 207: the plausibility band
                            g_st_hf3 = (float)(g_st_cal_sum / g_st_cal_n);
                            g_st_hf3_ok = true;
                            note("stance: standing height calibrated at %.2f m over %u samples (F4). "
                                 "Crouch below %.2f m, prone below %.2f m.", g_st_hf3, g_st_cal_n,
                                 g_st_hf3 * g_stance_crouch.load(std::memory_order_relaxed),
                                 g_st_hf3 * g_stance_prone.load(std::memory_order_relaxed));
                        } else {
                            note("stance: F4 height capture got only %u samples; not used.", g_st_cal_n);
                        }
                        g_st_cal_until = 0; g_st_cal_sum = 0.0; g_st_cal_n = 0;
                    }
                }
                const float over = g_stance_height.load(std::memory_order_relaxed);
                if (over > 1.0f) {
                    g_st_hstand = over; g_st_src = 2;
                } else if (g_st_hf3_ok) {
                    g_st_hstand = g_st_hf3; g_st_src = 1;
                } else {
                    // automatic: the average over the three seconds starting one
                    // second after the recenter; until then, the current height
                    g_st_src = 0;
                    const uint64_t age = g_st_ref_ms ? tnow - g_st_ref_ms : 0;
                    // BUILD 302 (his choice 2026-10-12: "the recentre takes it only if I am standing"): the average is
                    // accepted only within 0.10 m of the tallest average accepted this session (the 301 run took 0.70 to
                    // 1.44 m from recentres held while crouched); a rejected one leaves the standing height as it was.
                    // Until the first average lands, the current height stands in.
                    if (!g_st_auto_done && age >= 1000 && age < 4000) {
                        g_st_auto_sum += height; ++g_st_auto_n;
                        if (g_st_tallest <= 0.0f) g_st_hstand = (float)(g_st_auto_sum / g_st_auto_n);
                    } else if (!g_st_auto_done && age >= 4000 && g_st_auto_n > 0) {
                        g_st_auto_done = true;
                        const float avg = (float)(g_st_auto_sum / g_st_auto_n);
                        if (g_st_tallest <= 0.0f || avg >= g_st_tallest - 0.10f) {
                            g_st_hstand = avg;
                            if (avg > g_st_tallest && avg < 2.3f) g_st_tallest = avg;
                            note("stance: standing height taken as %.2f m, the average of the three seconds after "
                                 "the recenter (Home re-takes it; F4 calibrates it properly).", g_st_hstand);
                        } else {
                            note("stance: recentre average %.2f m REJECTED, you were not standing (tallest this session %.2f m); "
                                 "standing height stays %.2f m.", avg, g_st_tallest, g_st_hstand);
                        }
                    } else if (g_st_auto_n == 0 && g_st_tallest <= 0.0f) {
                        g_st_hstand = height;
                    }
                }
            }
            // the actual stance, and the bone's standing height
            float dz = 0.0f;
            const bool anch = headpose::head_dz(&dz);
            int actual = -1;
            if (anch && isfinite(dz)) {
                if (dz > g_st_dzmax && dz < 1.75f) g_st_dzmax = dz;   // BUILD 207: capped
                // BUILD 207: the seat band. A vehicle seat reads 0.69 to 0.75
                // (grwxr-26920), between prone and crouch: no stance, no B,
                // no compensation until the vehicle gate (4.3) exists.
                g_st_seated = dz > 0.55f && dz < 0.85f;
                // BUILD 209: the lines at the clip midpoints from the game data (stand 1.58 to 1.65,
                // sprint 1.39 to 1.42, crouch 0.98, prone 0.27 to 0.36): stand above 1.31, prone below 0.66
                actual = g_st_seated ? -1 : (dz > 1.31f ? 0 : (dz > 0.66f ? 1 : 2));
            } else {
                g_st_seated = false;
            }
            if (actual != g_st_actual) {
                if (g_st_actual >= 0 && actual >= 0) g_st_changes.fetch_add(1, std::memory_order_relaxed);
                g_st_actual = actual;
            }
            // the desired stance, with hysteresis
            static int s_desired = 0;
            if (hok2 && g_st_hstand > 1.0f) {
                const float ratio = height / g_st_hstand;
                const float cr = g_stance_crouch.load(std::memory_order_relaxed);
                const float pr = g_stance_prone.load(std::memory_order_relaxed);
                // BUILD 208: prone from the head only with stance_prone set; by
                // default the head picks stand or crouch and prone is his own.
                const bool prone_on = pr > 0.05f;
                // BUILD 301: the crouch threshold in metres of real drop (his to tune), hysteresis crouch_rise on the way up.
                // crouch_drop 0 keeps the 205 ratio test. Prone is untouched (his, unless stance_prone is set).
                const float cdrop = g_crouch_drop.load(std::memory_order_relaxed);
                const float crise = g_crouch_rise.load(std::memory_order_relaxed);
                const float drop  = g_st_hstand - height;
                g_st_drop = drop;
                const bool  metres = cdrop > 0.01f;
                const bool  want_crouch = metres ? (drop > cdrop) : (ratio < cr);
                const bool  want_stand  = metres ? (drop < cdrop - crise) : (ratio > cr + 0.07f);
                if (s_desired == 0) {
                    if (prone_on && ratio < pr) s_desired = 2; else if (want_crouch) s_desired = 1;
                } else if (s_desired == 1) {
                    if (prone_on && ratio < pr) s_desired = 2; else if (want_stand) s_desired = 0;
                } else {
                    if (!prone_on || ratio > pr + 0.08f) s_desired = want_stand ? 0 : 1;
                }
            }
            g_st_desired = s_desired;
            // the act
            static uint64_t s_b_until = 0, s_settle_until = 0, s_window_until = 0;
            static int s_attempts = 0, s_last_actual = -1, s_prev_desired = -1;
            static bool s_user_b_prev = false;
            const uint64_t now = GetTickCount64();
            if (actual != s_last_actual) { s_attempts = 0; s_last_actual = actual; }
            bool b = now < s_b_until;
            const bool on = g_stance_on.load(std::memory_order_relaxed) != 0 && headpose::fp_enabled();
            uint32_t pad_btn = 0;
            float pad_ax[6] = {};
            const bool pad_live = headpose::touch_pad(&pad_btn, pad_ax);
            const bool user_b = pad_live && (pad_btn & headpose::PAD_B);
            const bool aiming = pad_live && pad_ax[4] > 0.12f;   // BUILD 207: never pulse while aiming (the survey's gate)
            // BUILD 207: the loop acts only on a CHANGE of the desired stance,
            // inside a three-second window (four acts at most); his own B
            // closes the window, so a manual crouch or stand is respected
            // until his body moves again (the internal dig, 1.4).
            if (s_desired != s_prev_desired) {
                if (s_prev_desired >= 0) { s_window_until = now + 3000; s_attempts = 0; }
                s_prev_desired = s_desired;
            }
            if (user_b && !s_user_b_prev && !b) {
                if (now < s_window_until) g_st_manual.fetch_add(1, std::memory_order_relaxed);
                s_window_until = 0;
            }
            s_user_b_prev = user_b;
            // BUILD 207: only with a stereo frame built this present (a menu or a
            // loading screen has none, and B there is Back), and never while aiming.
            // BUILD 208: a prone character is his own doing while prone is not the head's: leave it.
            const bool prone_his = g_stance_prone.load(std::memory_order_relaxed) <= 0.05f && actual == 2;
            if (on && !b && built_q_ok && !aiming && !prone_his && now < s_window_until && now >= s_settle_until && actual >= 0 && s_desired != actual && !user_b) {
                if (s_attempts >= 4) {
                    s_window_until = 0;
                    g_st_fights.fetch_add(1, std::memory_order_relaxed);
                } else if (s_desired == 2 && actual < 2) {
                    s_b_until = now + 700;            // the hold: prone
                    s_settle_until = s_b_until + 1100;
                    g_st_holds.fetch_add(1, std::memory_order_relaxed);
                    ++s_attempts; b = true;
                } else {
                    s_b_until = now + 120;            // the tap: one step down, or up
                    s_settle_until = s_b_until + 900;
                    g_st_taps.fetch_add(1, std::memory_order_relaxed);
                    ++s_attempts; b = true;
                }
            }
            if (!on) b = false;
            headpose::set_stance_b(b);
        }
        // BUILD 10j: this is the orientation the frame's content is composed
        // with (both eyes share it); the submit path attaches it to both views.
        if (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
            head_ori = loc.pose.orientation;
            head_ori_ok = true;
            {   // BUILD 251: the layer-pose-lag ring (same present counter the recorder rows use)
                const uint64_t fc = d3d11::frame_count();
                g_qh_ring[fc & 15u] = head_ori;
                g_qh_ring_frame[fc & 15u] = fc;
            }
            if (gp) {   // BUILD 270: the located head, which is the quaternion published to the camera side this Present
                gp->head_q[0] = head_ori.x; gp->head_q[1] = head_ori.y; gp->head_q[2] = head_ori.z; gp->head_q[3] = head_ori.w;
                gp->fl |= 1u << 1;
            }
        }
    }
    if (gp) {   // BUILD 270: the reference and the aim accounting as this Present left them (update_head ran above)
        gp->ref_inv[0] = g_ref_inv.x; gp->ref_inv[1] = g_ref_inv.y; gp->ref_inv[2] = g_ref_inv.z; gp->ref_inv[3] = g_ref_inv.w;
        gp->ref_epoch  = headpose::ref_epoch();
        gp->aim_cum[0] = g_aim_cum[0];
        gp->aim_cum[1] = g_aim_cum[1];
    }

    // BUILD 13b: sync the action set and locate both aim poses. Read-only,
    // stores into atomics; the heartbeat thread logs them (drain_input).
    // xrSyncActions returns SESSION_NOT_FOCUSED harmlessly until focus.
    if (g_input_ok) {
        XrActiveActionSet aas{g_actionset, XR_NULL_PATH};
        XrActionsSyncInfo syn{XR_TYPE_ACTIONS_SYNC_INFO};
        syn.countActiveActionSets = 1;
        syn.activeActionSets = &aas;
        ph(kPhSyncActions);
        if (XR_SUCCEEDED(xrSyncActions(g_session, &syn))) {
            uint32_t seen = 0;
            // BUILD 124: physical hand h is stored in LOGICAL slot s, where
            // slot 1 is the main (trigger and pivot) hand. With main_hand = 1
            // this is the identity and the build is byte-identical to 123.
            const int mh = g_main_hand.load(std::memory_order_relaxed) ? 1 : 0;
            for (int h = 0; h < 2; ++h) {
                const int s = (mh == 1) ? h : (1 - h);
                XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
                ph(kPhLocateAim, (int)h);
                if (XR_SUCCEEDED(xrLocateSpace(g_aim_space[h], g_space,
                                               fs.predictedDisplayTime, &hl)) &&
                    (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                    (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                    seen |= 1u << s;
                    g_in_track[s].store(true, std::memory_order_relaxed);
                    g_in_pos[s][0].store(hl.pose.position.x, std::memory_order_relaxed);
                    g_in_pos[s][1].store(hl.pose.position.y, std::memory_order_relaxed);
                    g_in_pos[s][2].store(hl.pose.position.z, std::memory_order_relaxed);
                    g_in_ori[s][0].store(hl.pose.orientation.x, std::memory_order_relaxed);
                    g_in_ori[s][1].store(hl.pose.orientation.y, std::memory_order_relaxed);
                    g_in_ori[s][2].store(hl.pose.orientation.z, std::memory_order_relaxed);
                    g_in_ori[s][3].store(hl.pose.orientation.w, std::memory_order_relaxed);
                } else {
                    g_in_track[s].store(false, std::memory_order_relaxed);
                }
                // BUILD 162: the grip pose, orientation only, same pass and
                // same predicted time, so the two frames describe one
                // instant of the same controller.
                bool gtr = false;
                if (g_grip_space[h]) {
                    XrSpaceLocation gl{XR_TYPE_SPACE_LOCATION};
                    ph(kPhLocateGrip, (int)h);
                    if (XR_SUCCEEDED(xrLocateSpace(g_grip_space[h], g_space,
                                                   fs.predictedDisplayTime, &gl)) &&
                        (gl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                        g_in_gori[s][0].store(gl.pose.orientation.x, std::memory_order_relaxed);
                        g_in_gori[s][1].store(gl.pose.orientation.y, std::memory_order_relaxed);
                        g_in_gori[s][2].store(gl.pose.orientation.z, std::memory_order_relaxed);
                        g_in_gori[s][3].store(gl.pose.orientation.w, std::memory_order_relaxed);
                        gtr = true;
                    }
                }
                g_in_gtrack[s].store(gtr, std::memory_order_relaxed);
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = g_act_trig;
                gi.subactionPath = g_hand[h];
                XrActionStateFloat tf{XR_TYPE_ACTION_STATE_FLOAT};
                ph(kPhActionState, (int)s);
                if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &tf)) && tf.isActive)
                    g_in_trig[s].store(tf.currentState, std::memory_order_relaxed);
                gi.action = g_act_grip;
                tf = {XR_TYPE_ACTION_STATE_FLOAT};
                if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &tf)) && tf.isActive)
                    g_in_grip[s].store(tf.currentState, std::memory_order_relaxed);
            }
            g_in_seen.store(seen, std::memory_order_relaxed);

            // Build 22: assemble the Touch-as-gamepad snapshot. Left stick =
            // move, right stick = turn, triggers map straight across, grips
            // are the bumpers, stick clicks and A/B/X/Y direct, left menu =
            // Start. XInputMerge consumes it on the game's input thread.
            {
                uint32_t btn = 0;
                float    ax[6] = {};
                XrActionStateGetInfo  gi2{XR_TYPE_ACTION_STATE_GET_INFO};
                XrActionStateVector2f v2{XR_TYPE_ACTION_STATE_VECTOR2F};
                XrActionStateBoolean  ab{XR_TYPE_ACTION_STATE_BOOLEAN};
                for (int h = 0; h < 2; ++h) {
                    gi2.action = g_act_stick;
                    gi2.subactionPath = g_hand[h];
                    v2 = {XR_TYPE_ACTION_STATE_VECTOR2F};
                    const XrResult vr2 = xrGetActionStateVector2f(g_session, &gi2, &v2);
                    // Build 22.1 diagnostic: why is a stick zero? (rc, active)
                    g_in_stick_rc[h].store(((int)vr2 << 1) |
                                               (v2.isActive ? 1 : 0),
                                           std::memory_order_relaxed);
                    if (XR_SUCCEEDED(vr2) && v2.isActive) {
                        ax[h * 2 + 0] = v2.currentState.x;
                        ax[h * 2 + 1] = v2.currentState.y;
                    }
                    gi2.action = g_act_stickclk;
                    ab = {XR_TYPE_ACTION_STATE_BOOLEAN};
                    if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi2, &ab)) &&
                        ab.isActive && ab.currentState)
                        btn |= h == 0 ? headpose::PAD_LTHUMB : headpose::PAD_RTHUMB;
                    // Build 145: with the grip-gated hold on, the OFF hand's
                    // squeeze belongs to the weapon, not the pad, so it no
                    // longer presses a bumper in game. The gun-hand grip
                    // keeps its bumper.
                    const bool offhand =
                        (h == 0) == (g_main_hand.load(
                                         std::memory_order_relaxed) != 0);
                    if (g_in_grip[h].load(std::memory_order_relaxed) >= 0.6f &&
                        !(offhand && g_cfg_wgun_two_grip >= 0.5f))
                        btn |= h == 0 ? headpose::PAD_LB : headpose::PAD_RB;
                }
                // Build 145: publish the off-hand squeeze for the grip gate.
                camera::set_grip_off(
                    g_in_grip[g_main_hand.load(std::memory_order_relaxed)
                                  ? 0 : 1]
                        .load(std::memory_order_relaxed));
                const struct { XrAction a; uint32_t bit; } bmap[] = {
                    {g_act_btn_a, headpose::PAD_A}, {g_act_btn_b, headpose::PAD_B},
                    {g_act_btn_x, headpose::PAD_X}, {g_act_btn_y, headpose::PAD_Y},
                    {g_act_menu,  headpose::PAD_START},
                };
                gi2.subactionPath = XR_NULL_PATH;
                // BUILD 300: with a hold time set, the left menu button is not Start while it is down: a hold of lean_hold_ms recentres
                // (the Home recenter), and a shorter press is sent as a 120 ms Start when it is released.
                const int hold_ms = g_lean_hold_ms.load(std::memory_order_relaxed);
                bool menu_down = false;
                for (const auto& m : bmap) {
                    gi2.action = m.a;
                    ab = {XR_TYPE_ACTION_STATE_BOOLEAN};
                    const bool on = XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi2, &ab)) && ab.isActive && ab.currentState;
                    if (m.bit == headpose::PAD_START && hold_ms > 0) { menu_down = on; continue; }
                    if (on) btn |= m.bit;
                }
                if (hold_ms > 0) {
                    static bool s_md = false, s_mfired = false;
                    static uint64_t s_mt0 = 0, s_mpulse = 0;
                    const uint64_t tn = GetTickCount64();
                    if (menu_down && !s_md) { s_md = true; s_mfired = false; s_mt0 = tn; }
                    if (menu_down && s_md && !s_mfired && tn - s_mt0 >= (uint64_t)hold_ms) {
                        s_mfired = true;
                        g_have_ref = false;
                        g_aim_resync.store(true, std::memory_order_relaxed);
                        g_lean_rc_menu.fetch_add(1, std::memory_order_relaxed);
                        note("VR: recenter requested (left menu button held). Head reference and lean origin will re-capture.");
                    }
                    if (!menu_down && s_md) {
                        s_md = false;
                        if (!s_mfired) { s_mpulse = tn + 120; g_lean_start_taps.fetch_add(1, std::memory_order_relaxed); }
                    }
                    if (tn < s_mpulse) btn |= headpose::PAD_START;
                }
                ax[4] = g_in_trig[0].load(std::memory_order_relaxed);
                ax[5] = g_in_trig[1].load(std::memory_order_relaxed);
                // Build 22.2: live means the ACTIONS are answering, not that
                // the poses track. A controller resting out of camera view
                // loses pose while its stick and buttons still report; gating
                // the pad on pose killed the sticks whenever tracking
                // flickered. Pose validity (seen) still gates the aim rays,
                // never the pad.
                const bool acts_live =
                    (g_in_stick_rc[0].load(std::memory_order_relaxed) & 1) ||
                    (g_in_stick_rc[1].load(std::memory_order_relaxed) & 1);
                headpose::set_touch_pad(btn, ax, acts_live || seen != 0);
            }
        }
    }

    // Build 22: while the Touch-as-gamepad merge is live, the mouse-synthesis
    // paths stand down wholesale. The game flips its control scheme to
    // whichever device spoke last; feeding it synthetic mouse AND pad input
    // in the same frames would flap the scheme (and double-fire: RT is the
    // pad's own fire). Any held synthetic button is released on the way out.
    if (xin::merging_live()) {
        if (g_ads_rmb.load(std::memory_order_relaxed)) ads_send(false);
        if (g_fire_lmb.load(std::memory_order_relaxed)) fire_send(false);
    } else {
        // BUILD 14d/14e: controller aim steer, gated on the right grip. Uses
        // the input state stored just above.
        steer_aim();

        // BUILD 14f: right trigger drives the game's own ADS.
        ads_input();
        // BUILD 14h: full pull fires. After ads_input so a single frame that
        // crosses both thresholds aims before it shoots.
        fire_input();
    }

    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult begin_r = XR_ERROR_RUNTIME_FAILURE;
    {   // BUILD 215
        perf::Scope sc(perf::kBegin);
        ph(kPhBeginFrame);
        begin_r = xrBeginFrame(g_session, &fbi);
        ph(kPhDone);
    }
    if (XR_FAILED(begin_r)) return;

    // BUILD 10a (docs/HANDOFF.md "BUILD 10" step A): AER submission machinery.
    // Each presented frame the backbuffer is copied into ONE eye's swapchain,
    // alternating; that eye's view gets this frame's located pose and fov. The
    // other eye re-submits its previous image with the pose and fov stored
    // when that image was fresh, and the compositor reprojects both views to
    // display time. The camera is NOT offset yet: both eyes show the same
    // viewpoint, so the headset should look identical to build 9. That
    // sameness is this step's acceptance test.
    bool submitted = false;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pviews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    };

    if (fs.shouldRender) {
        // Per-eye poses for the layer. The xrLocateSpace(VIEW) head-pose path
        // above stays for the camera compose; this is the layer's anchor.
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime           = fs.predictedDisplayTime;
        vli.space                 = g_space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t vc = 0;
        const bool views_ok =
            XR_SUCCEEDED(xrLocateViews(g_session, &vli, &vs, 2, &vc, views)) &&
            vc == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
        if (gp && views_ok) {   // BUILD 270: the located views
            for (int e = 0; e < 2; ++e) {
                gp->view_q[e][0] = views[e].pose.orientation.x; gp->view_q[e][1] = views[e].pose.orientation.y;
                gp->view_q[e][2] = views[e].pose.orientation.z; gp->view_q[e][3] = views[e].pose.orientation.w;
                gp->view_p[e][0] = views[e].pose.position.x; gp->view_p[e][1] = views[e].pose.position.y;
                gp->view_p[e][2] = views[e].pose.position.z;
            }
            gp->fl |= 1u << 2;
        }

        // BUILD 10b: the measured IPD feeds the camera-side eye offset.
        if (views_ok) {
            const float dx = views[1].pose.position.x - views[0].pose.position.x;
            const float dy = views[1].pose.position.y - views[0].pose.position.y;
            const float dz = views[1].pose.position.z - views[0].pose.position.z;
            headpose::publish_ipd(sqrtf(dx * dx + dy * dy + dz * dz));

            // BUILD 10j one-shot diagnostic: the runtime's per-eye view
            // orientations and the angle between them. Nonzero cant here is
            // the measured cause of the constant angular doubling.
            static bool s_cant_logged = false;
            if (!s_cant_logged) {
                s_cant_logged = true;
                const XrQuaternionf& a = views[0].pose.orientation;
                const XrQuaternionf& b = views[1].pose.orientation;
                float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
                if (dot < 0.0f) dot = -dot;
                if (dot > 1.0f) dot = 1.0f;
                const float cant_deg = 2.0f * acosf(dot) * 57.29578f;
                note("VR: 10j view ori L(%.4f,%.4f,%.4f,%.4f) R(%.4f,%.4f,%.4f,%.4f) cant %.3f deg",
                     a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, cant_deg);
            }
        }

        ID3D11Texture2D* back = nullptr;
        if (views_ok &&
            SUCCEEDED(st.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
            // BUILD 10b.1: the eye identity comes from the FIFO tag pushed by
            // the camera hook when this frame was BUILT, never from frame
            // parity: the build-to-present pipeline depth is unknown and
            // parity guessing swapped the eyes. A mono frame (no tag)
            // invalidates both eyes so the bootstrap below feeds the same
            // image to both with their current located poses.
            int fresh = built_eye;
            if (fresh < 0 || fresh > 1) {   // BUILD 247: 2 is a true-stereo tag, not an eye index; the existing path treats it like none
                g_eye[0].valid = false;
                g_eye[1].valid = false;
                fresh = 0;
            }
            const int stale = fresh ^ 1;

            // BUILD 10L: one-time canvas creation, needs the live view fovs.
            if (!g_canvas_ready && !create_canvases(st, views)) {
                g_dead = true;
                note("VR: 10L canvas creation FAILED, VR submission disabled");
            }

            // BUILD 11a: the vertical fov this frame's content was rendered
            // with, published live by the proj[2] hook. Fallback pi/4 until
            // the first publish (menus, loading) reproduces the 10L placement
            // exactly. read_fov is at most one pipeline step stale, the same
            // bound as the submitted-layer fov always had.
            const float cfov = headpose::read_fov(kRefFov);

            // Save the game's context state once around all of this present's
            // draws, and bind the shared blit state.
            CtxState saved{};
            const bool blitting = g_canvas_ready && g_blit_ok;
            if (blitting) {
                ctx_save(st.context, saved);

                // BUILD 118: read the game's depth-stencil description once.
                // ctx_save has just AddRef'd it for the restore, so it is
                // guaranteed live here and this borrows it without a second
                // reference. Latched: one attempt per process, ever.
                probe_depth_once(saved);

                // 2026-08-12: draw the settings panel's ImGui frame ONCE here,
                // before either eye is composited, and hand the result to both.
                // render() is a no-op returning null unless F1 has opened the
                // panel, so a closed panel costs one call and nothing else.
                // It must run BEFORE blit_bind: the ImGui DX11 backend sets up
                // its own pipeline state, and doing that after we bind ours
                // would leave the eye blit drawing with ImGui's state.
                {   // BUILD 215
                    perf::Scope sc(perf::kMenu);
                    g_menu_srv = menu::ready() ? menu::render() : nullptr;
                }

                blit_bind(st.context);
            } else {
                g_menu_srv = nullptr;
            }

            // BUILD 247: TRUE STEREO. A frame built with both cameras holds the LEFT eye in the left half of the
            // backbuffer and the RIGHT eye in the right half; submit_halves() draws each half into its eye's
            // canvas and fills the layer with two fresh views. rc 0 = not a halves frame, or not possible: the
            // alternate-eye code below runs exactly as before. rc 1 = done. rc 2 = tried and failed: submit nothing.
            //   halves frame = a stereo tag (eye 2), or no tag at all while the stereo route is live (a menu or a
            //   loading frame in the half-viewport main view: the left half is shown, mono, rather than two images).
            int halves_rc = 0;
            const bool st_req = cam2::stereo_requested();
            if (st_req) {
                // BUILD 248: the one vertical fov (in the half viewport) both cameras are forced to render with: enough to
                // cover each eye's whole view, horizontally and vertically, with a 3 per cent margin. The persp2 hook reads it.
                float need_h = 0.0f, need_v = 0.0f;
                for (int e = 0; e < 2; ++e) {
                    need_h = std::max(need_h, std::max(-tanf(views[e].fov.angleLeft), tanf(views[e].fov.angleRight)));
                    need_v = std::max(need_v, std::max(tanf(views[e].fov.angleUp), -tanf(views[e].fov.angleDown)));
                }
                float half_aspect = cam2::stereo_aspect_override();
                if (!(half_aspect > 0.0f)) half_aspect = ((float)(g_content_w / 2u)) / (float)g_content_h;
                if (need_h > 0.1f && need_v > 0.1f && half_aspect > 0.1f)
                    cam2::set_stereo_target_fovy(2.0f * atanf(1.03f * std::max(need_v, need_h / half_aspect)));
                // BUILD 249: the sky and the clouds are told the world fov by the scene override (fov_scene, which writes
                // fs_fov), so while stereo is live fs_fov carries the FULL-WIDTH-equivalent of the stereo target: the engine
                // then derives the target itself in a half viewport, and the world, the sky and the clouds share one frustum.
                // (Build 248 forced the world to the target while the scene still said 1.92 and the clouds followed the head.)
                if (cam2::stereo_live() && half_aspect > 0.1f) {
                    float fe = 2.0f * atanf(tanf(0.5f * cam2::stereo_target_fovy()) * half_aspect / (16.0f / 9.0f));
                    fe = std::min(2.0f, std::max(0.7f, fe));
                    headpose::set_fs_fov(fe);
                    g_fs_stereo_applied = true;
                }
            }
            if (g_fs_stereo_applied && !cam2::stereo_live()) {   // stereo left: the cfg value is the fullscreen fov again
                headpose::set_fs_fov(g_cfg_fs_fov_cfg);
                g_fs_stereo_applied = false;
            }
            if (st_req) {
                const bool stereo_tag = (built_eye == 2);
                const bool halves = stereo_tag || (built_eye < 0 && cam2::stereo_live());
                if (halves) {
                    if (!blitting) {
                        cam2::stereo_note_reason("the scaling blit is not available (create_blit_resources failed or the canvases are not ready): stereo needs it");
                    } else {
                        halves_rc = submit_halves(st, back, views, stereo_tag && !(built_flags & 0x08u),
                                                  head_ori_ok, head_ori, built_q_ok, built_q, cfov, built_eye,
                                                  pviews, layer);
                        if (halves_rc == 1) submitted = true;
                    }
                }
                if (gp) {   // BUILD 270: what this Present did with the frame
                    gp->halves_rc = halves_rc;
                    gp->cfov = cfov;
                    if (halves) gp->fl |= 1u << 5;
                    if (stereo_tag && !(built_flags & 0x08u)) gp->fl |= 1u << 4;
                    if (g_head_roll_fix.load(std::memory_order_relaxed)) gp->fl |= 1u << 7;
                    if (cam2::stereo_swap_eyes()) gp->fl |= 1u << 8;
                    if (camera::veh_on()) gp->fl |= 1u << 9;
                    if (camera::veh_hook_drives()) gp->fl |= 1u << 10;
                }
                const bool in_halves = halves_rc != 0;
                if (!in_halves && g_stereo_prev_halves) {
                    g_eye[0].valid = false;   // leaving stereo: the stored eyes are stereo-mode images, re-bootstrap both
                    g_eye[1].valid = false;
                }
                g_stereo_prev_halves = in_halves;
                if (!in_halves) {
                    float pp[2][3], oo[2][4], ff[2][4];
                    stereo_pack_views(views, pp, oo, ff);
                    cam2::stereo_note_frame(stereo_tag && halves ? cam2::kStereoNotReady : cam2::kStereoAlt, built_eye,
                                            d3d11::frame_count(), pp, oo, ff, cfov, g_half_w, g_half_h, 0.0f, true);
                }
            }

            if (halves_rc == 0) {   // BUILD 247: the whole existing alternate-eye section, down to the desktop redraw

            // Capture the fresh frame into its eye's private texture first:
            // it is the blit source now, and the 10i stale re-copy source
            // as before.
            if (g_canvas_ready && g_last[fresh]) {
                st.context->CopySubresourceRegion(g_last[fresh], 0, 0, 0, 0,
                                                  back, 0, nullptr);
                if (blitting) st.context->GenerateMips(g_last_srv[fresh]);
            }
            // BUILD 237: latch the disc's source centre WITH the picture it
            // belongs to. This eye's content is the frame just captured, so
            // this frame's aim tangent is the right one for it; the other eye
            // keeps the pair it was captured with and stops being cropped
            // around a direction its picture never saw.
            g_pip_atx_eye[fresh] = g_pip_atx;
            g_pip_aty_eye[fresh] = g_pip_aty;

            const bool fresh_ok =
                g_canvas_ready &&
                (blitting ? blit_into(fresh, st.context, cfov)
                          : copy_into(fresh, st.context, back));
            if (fresh_ok) {
                // BUILD 10j: both eyes' content is rendered with ONE shared
                // camera orientation (the game camera composed with the head
                // pose). Submitting each view with its own xrLocateViews
                // orientation asserts a per-eye cant the content does not
                // have; the compositor then shows the eyes diverged by the
                // cant difference: a constant angular doubling that ignores
                // the ipd knob (measured ~15 deg total, builds 10g/10h).
                // Attach the shared render orientation to both views;
                // positions stay per-eye. Zero runtime cant makes this a
                // no-op by construction.
                XrPosef fresh_pose = views[fresh].pose;
                XrPosef stale_boot = views[stale].pose;
                if (head_ori_ok) {
                    fresh_pose.orientation = head_ori;
                    stale_boot.orientation = head_ori;
                }
                // BUILD 13a: RENDER-POSE SUBMIT. The content of this image was
                // composed with the head orientation the camera hook consumed
                // when the frame was BUILT, not the orientation located at
                // present time. Submitting the present-time locate makes the
                // compositor timewarp each submission from a pose the content
                // does not have; with the build-to-present depth flapping 0..1
                // the angular error changes per refresh: monocular 36 Hz
                // stutter during head rotation, invisible when still (session
                // 13 headset report). The tag FIFO delivers the true render
                // orientation with the frame; submit that instead, for the
                // fresh eye and (below, via g_eye) stored for its stale
                // re-submits. The bootstrap fill re-uses this same image, so
                // it gets the same orientation. Positions stay per-eye located
                // (translation latency is a separate, smaller error; one
                // change per build). Mono frames (no tag) keep the located
                // orientation, exactly the old behaviour.
                if (built_q_ok) {
                    fresh_pose.orientation = {built_q[0], built_q[1],
                                              built_q[2], built_q[3]};
                    stale_boot.orientation = fresh_pose.orientation;
                }
                // BUILD 202: the head roll (CX-022). The content has no roll;
                // submit a pose with none, and the compositor levels the
                // horizon by the head's roll. Both orientations, so the
                // bootstrap fill and the stored re-submits agree.
                if (g_head_roll_fix.load(std::memory_order_relaxed)) {
                    fresh_pose.orientation = strip_roll_xr(fresh_pose.orientation);
                    stale_boot.orientation = strip_roll_xr(stale_boot.orientation);
                }
                // BUILD 10L: the recommended per-eye fov is now the TRUTH,
                // because the canvas is shaped to it and the content sits
                // angle-correct inside. (10k submitted this fov as a lie and
                // proved the runtime ignores the field either way.)
                g_eye[fresh] = {fresh_pose, views[fresh].fov, true, cfov};

                // First frame after start or re-begin: the other eye has no
                // image yet, so feed it this same frame with its own located
                // pose. From the next frame on the eyes alternate.
                if (!g_eye[stale].valid) {
                    if (g_last[stale]) {
                        st.context->CopySubresourceRegion(g_last[stale], 0, 0, 0, 0,
                                                          back, 0, nullptr);
                        if (blitting) st.context->GenerateMips(g_last_srv[stale]);
                    }
                    if (blitting ? blit_into(stale, st.context, cfov)
                                 : copy_into(stale, st.context, back)) {
                        g_eye[stale] = {stale_boot, views[stale].fov, true, cfov};
                    }
                } else if (g_last[stale]) {
                    // BUILD 10i: re-copy the stale eye's own last frame into a
                    // freshly acquired image so this submission is a normal
                    // release, not a re-reference. Pose and fov stay the stored
                    // ones (the frame's render pose); BUILD 11a likewise draws
                    // at the content fov STORED when that image was captured,
                    // so a fov change mid zoom cannot shear the eye pair.
                    if (blitting) blit_into(stale, st.context, g_eye[stale].cfov);
                    else          copy_into(stale, st.context, g_last[stale]);
                }

                if (g_eye[stale].valid) {
                    for (int i = 0; i < 2; ++i) {
                        pviews[i].pose = g_eye[i].pose;
                        pviews[i].fov  = g_eye[i].fov;
                        pviews[i].subImage.swapchain = g_swapchains[i];
                        pviews[i].subImage.imageRect.offset = {0, 0};
                        pviews[i].subImage.imageRect.extent = {(int32_t)g_sc_w, (int32_t)g_sc_h};
                        pviews[i].subImage.imageArrayIndex  = 0;
                    }
                    layer.layerFlags = 0;
                    layer.space      = g_space;
                    layer.viewCount  = 2;
                    layer.views      = pviews;
                    submitted = true;
                }
            }

            // BUILD 12c: desktop recording view. While the blit state is
            // still bound, redraw the backbuffer from the LEFT eye's capture
            // (g_eye[0] holds the fov it was rendered with), cropped so
            // desktop_fov fills the window. Oversized viewport, clipped by
            // the target bounds, exactly the canvas blit's own pattern.
            // Content refreshes on left-eye frames (~36 Hz); steady beats
            // the +-IPD/2 alternation shiver for recordings.
            if (blitting && g_desk_on && g_desk_fov > 0.0f &&
                g_eye[0].valid && g_last_srv[0] &&
                g_eye[0].cfov > g_desk_fov) {
                if (g_desk_rtv_key != back && g_desk_rtv) {
                    g_desk_rtv->Release();
                    g_desk_rtv = nullptr;
                    g_desk_rtv_key = nullptr;
                }
                if (!g_desk_rtv) {   // once per backbuffer (rule 8: creation
                    D3D11_RENDER_TARGET_VIEW_DESC rd{};   // is init-like)
                    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // hazard 22
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    if (SUCCEEDED(st.device->CreateRenderTargetView(back, &rd,
                                                                    &g_desk_rtv)))
                        g_desk_rtv_key = back;
                    else
                        g_desk_rtv = nullptr;
                }
                if (g_desk_rtv) {
                    float scale = tanf(0.5f * g_eye[0].cfov) /
                                  tanf(0.5f * g_desk_fov);
                    if (scale > 16.0f) scale = 16.0f;   // keep the viewport
                    // inside D3D11's +-32k bounds at any cfg value
                    D3D11_VIEWPORT vp;
                    vp.Width    = (float)g_content_w * scale;
                    vp.Height   = (float)g_content_h * scale;
                    vp.TopLeftX = ((float)g_content_w - vp.Width)  * 0.5f;
                    vp.TopLeftY = ((float)g_content_h - vp.Height) * 0.5f;
                    vp.MinDepth = 0.0f;
                    vp.MaxDepth = 1.0f;
                    st.context->RSSetViewports(1, &vp);
                    st.context->PSSetShaderResources(0, 1, &g_last_srv[0]);
                    st.context->OMSetRenderTargets(1, &g_desk_rtv, nullptr);
                    st.context->Draw(3, 0);
                }
            }

            }   // BUILD 247: end of the alternate-eye section (halves_rc == 0)

            // BUILD 11a: hand the context back to the game exactly as found.
            if (blitting) ctx_restore(st.context, saved);
        }
        if (back) back->Release();
    }

    const XrCompositionLayerBaseHeader* layers[] = { (XrCompositionLayerBaseHeader*)&layer };
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime          = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount           = submitted ? 1 : 0;
    fei.layers               = submitted ? layers : nullptr;
    {   // BUILD 215: the submit, timed apart from our own work
        perf::Scope sc(perf::kEnd);
        ph(kPhEndFrame);
        xrEndFrame(g_session, &fei);
        ph(kPhDone);
    }
    if (gp) {   // BUILD 270: what was submitted (the guard commits the row when this function returns)
        gp->qpc_end = gtrec::qpc_now();
        if (submitted) {
            gp->fl |= 1u << 6;
            for (int e = 0; e < 2; ++e) {
                const XrPosef& lp = pviews[e].pose;
                gp->label_q[e][0] = lp.orientation.x; gp->label_q[e][1] = lp.orientation.y;
                gp->label_q[e][2] = lp.orientation.z; gp->label_q[e][3] = lp.orientation.w;
                gp->label_p[e][0] = lp.position.x; gp->label_p[e][1] = lp.position.y; gp->label_p[e][2] = lp.position.z;
                gp->label_fov[e][0] = pviews[e].fov.angleLeft; gp->label_fov[e][1] = pviews[e].fov.angleRight;
                gp->label_fov[e][2] = pviews[e].fov.angleUp;   gp->label_fov[e][3] = pviews[e].fov.angleDown;
            }
        }
    }
    if (submitted) anatomy::on_stereo_submit();   // BUILD 241: starts the anatomy clock, first time only
    if (submitted) census::on_stereo_submit();    // BUILD 242: starts the census clock, first time only
}

bool active() { return g_active.load(); }

// BUILD 228: the phase the render thread last entered, for the heartbeat.
const char* frame_phase() {
    static char buf[64];
    static const char* const kName[] = {
        "idle", "xrWaitFrame", "xrBeginFrame", "xrCreateSwapchain",
        "xrEnumerateSwapchainImages", "xrAcquireSwapchainImage(copy)",
        "xrWaitSwapchainImage(copy)", "xrAcquireSwapchainImage(blit)",
        "xrWaitSwapchainImage(blit)", "xrEndFrame", "returned",
        "Present entry (before any OpenXR call)", "xrPollEvent",
        "xrBeginSession", "xrEndSession", "xrLocateSpace(view)",
        "xrLocateSpace(stage)", "xrSyncActions", "xrLocateSpace(aim)",
        "xrLocateSpace(grip)", "xrGetActionStateFloat",
        "on_present_body (the D3D side of the present)",
        "ffr::present_tick", "the camera 1 Hz samples"
    };
    int p = g_phase.load(std::memory_order_relaxed);
    if (p < 0 || p >= kPhCount) p = kPhIdle;
    const int e = g_phase_eye.load(std::memory_order_relaxed);
    if (e < 0) return kName[p];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s eye %d", kName[p], e);
    return buf;
}

void drain_log() {
    // BUILD 119d: drain the one-shot slot FIRST and independently, so a
    // latched probe can never be lost to a per-frame diagnostic sharing the
    // other buffer. Exchange, so it prints exactly once.
    if (g_once_want.exchange(false, std::memory_order_acq_rel))
        LOG_INFO("%s", g_once_note);
    if (!g_want_log.exchange(false, std::memory_order_acq_rel)) return;
    LOG_INFO("%s", g_deferred);
}

// BUILD 13b: 1 Hz controller state line, heartbeat thread. Position in LOCAL
// meters, orientation quaternion, trigger 0..1. Silent until a hand tracks,
// so headset-only runs stay uncluttered.
// BUILD 215: THE MOD'S OWN TIME, once a second (CX-029, item 4.16).
//
// The frame rate decays after about ninety good seconds on the same binary
// and cfg, with every 6DOF feature and the clouds fix ruled out by controlled
// runs; that shape is an accumulation, and nothing the mod does per frame had
// ever been timed. This line answers "is it ours": if ours, rig or cam grow
// over the run while fps falls, the accumulation is in the mod and the phase
// names it; if they stay flat and nothing of the process climbs, it is the
// game's or the runtime's, and the answer is headroom (foveated rendering,
// then upscaling). Init thread only: the process queries and the COM below are
// banned in Present (rule 8). The line is assembled from short pieces, each
// with its arguments beside its format, so no field can lose its argument.
namespace {
struct PerfLine {
    char s[1400] = {};
    int  p = 0;
    void add(const char* fmt, ...) {
        if (p >= (int)sizeof(s) - 1) return;
        va_list ap;
        va_start(ap, fmt);
        const int w = vsnprintf(s + p, sizeof(s) - (size_t)p, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        p += w;
        if (p > (int)sizeof(s) - 1) p = (int)sizeof(s) - 1;
    }
};
}  // namespace

static void perf_log() {
    static int64_t       s_last        = 0;
    static uint64_t      s_last_faults = 0;
    static IDXGIAdapter3* s_adapter3   = nullptr;
    static bool          s_adapter_tried = false;

    const int64_t t  = perf::now();
    const double  dt = s_last ? (double)(t - s_last) / (double)perf::freq() : 0.0;
    s_last = t;

    const perf::Take pr = perf::take(perf::kPresent);
    const perf::Take wt = perf::take(perf::kWait);
    const perf::Take bg = perf::take(perf::kBegin);
    const perf::Take en = perf::take(perf::kEnd);
    const perf::Take mn = perf::take(perf::kMenu);
    const perf::Take rg = perf::take(perf::kRig);
    const perf::Take ff = perf::take(perf::kFfr);   // BUILD 216
    perf::Take cam[perf::kNumCam];
    uint64_t cam_n = 0;
    double   cam_sum = 0.0, cam_max = 0.0;
    for (int i = 0; i < perf::kNumCam; ++i) {
        cam[i] = perf::take(perf::kCam0 + i);
        cam_n   += cam[i].n;
        cam_sum += cam[i].sum_ms;
        if (cam[i].max_ms > cam_max) cam_max = cam[i].max_ms;
    }
    const double   k      = 1000.0 / (double)perf::freq();
    const double   ft_max = (double)perf::g_ft_max.exchange(0, std::memory_order_relaxed) * k;
    const uint64_t over20 = perf::g_ft_over20.exchange(0, std::memory_order_relaxed);
    const uint64_t over28 = perf::g_ft_over28.exchange(0, std::memory_order_relaxed);
    const uint64_t lines  = perf::g_log_lines.exchange(0, std::memory_order_relaxed);
    const uint64_t bytes  = perf::g_log_bytes.exchange(0, std::memory_order_relaxed);

    // the first tick has no interval: the accumulators are reset and nothing is rated
    if (dt <= 0.0) return;
    // nothing presented (the game loading, VR not armed): keep the log quiet
    if (pr.n == 0 && rg.n == 0 && cam_n == 0) return;

    const double ours = pr.sum_ms - wt.sum_ms - bg.sum_ms - en.sum_ms;
    const double fps  = (double)pr.n / dt;

    // the process, cheap, this thread only
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    double   ws_mb = 0.0, priv_mb = 0.0;
    uint64_t faults = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        ws_mb   = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
        priv_mb = (double)pmc.PrivateUsage   / (1024.0 * 1024.0);
        faults  = pmc.PageFaultCount;
    }
    const uint64_t dfaults = (s_last_faults && faults >= s_last_faults) ? faults - s_last_faults : 0;
    s_last_faults = faults;
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD usr = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);

    // the process's video memory, from the adapter the game's device sits on;
    // resolved once the device exists, and that one attempt is never retried
    ID3D11Device* dev = d3d11::state().device;
    if (!s_adapter_tried && dev) {
        s_adapter_tried = true;
        IDXGIDevice*  dd  = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && dd) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dd->GetAdapter(&ad)) && ad) {
                if (FAILED(ad->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&s_adapter3)))
                    s_adapter3 = nullptr;
                ad->Release();
            }
            dd->Release();
        }
    }
    double vloc = -1.0, vnon = -1.0;
    if (s_adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
        if (SUCCEEDED(s_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)))
            vloc = (double)mi.CurrentUsage / (1024.0 * 1024.0);
        if (SUCCEEDED(s_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &mi)))
            vnon = (double)mi.CurrentUsage / (1024.0 * 1024.0);
    }

    PerfLine L;
    L.add("perf: dt=%.2fs fps=%.1f ftmax=%.1fms over20=%llu over28=%llu",
          dt, fps, ft_max, (unsigned long long)over20, (unsigned long long)over28);
    L.add(" | present n=%llu sum=%.2f max=%.2f",
          (unsigned long long)pr.n, pr.sum_ms, pr.max_ms);
    L.add(" | wait sum=%.2f max=%.2f | begin sum=%.2f max=%.2f | end sum=%.2f max=%.2f",
          wt.sum_ms, wt.max_ms, bg.sum_ms, bg.max_ms, en.sum_ms, en.max_ms);
    L.add(" | menu sum=%.2f max=%.2f | ours=%.2f",
          mn.sum_ms, mn.max_ms, ours);
    L.add(" | rig n=%llu sum=%.2f max=%.3f",
          (unsigned long long)rg.n, rg.sum_ms, rg.max_ms);
    L.add(" | cam n=%llu sum=%.2f max=%.3f",
          (unsigned long long)cam_n, cam_sum, cam_max);
    for (int i = 0; i < perf::kNumCam; ++i) {
        if (!cam[i].n) continue;
        L.add(" c%d=%llu/%.2f", i, (unsigned long long)cam[i].n, cam[i].sum_ms);
    }
    L.add(" | ffr n=%llu sum=%.2f max=%.3f",
          (unsigned long long)ff.n, ff.sum_ms, ff.max_ms);
    {   // BUILD 216: the GPU's own busy share in the second, NVAPI, this thread only (-1 = unavailable)
        int fb = -1;
        const int gu = ffr::gpu_util(&fb);
        L.add(" | gpu util=%d fb=%d", gu, fb);
    }
    L.add(" | log lines=%llu kb=%.1f",
          (unsigned long long)lines, (double)bytes / 1024.0);
    L.add(" | mem ws=%.0fMB priv=%.0fMB faults=%llu handles=%lu gdi=%lu user=%lu",
          ws_mb, priv_mb, (unsigned long long)dfaults, (unsigned long)handles, (unsigned long)gdi, (unsigned long)usr);
    L.add(" | vram local=%.0fMB nonlocal=%.0fMB",
          vloc, vnon);
    LOG_INFO("%s (build 215/216: the mod's own CPU in the second, ms; ffr the render-target bind hook per bind, "
             "gpu util the GPU's busy share from NVAPI (-1 unavailable); present is the whole Present hook, wait/begin/end "
             "the OpenXR runtime calls inside it, ours = present minus those; rig is the gun-root and arm write per "
             "call; cam the camera and skeleton hooks by target, cN=calls/ms; max the worst single call; mem and "
             "vram are the whole process)", L.s);
}

void drain_input() {
    // BUILD 191: the fov root line, 1 Hz, before the input gate, so its off
    // state and its counters are on record even when OpenXR input is not.
    camera::fovroot_log();
    camera::fovsrc_log();   // BUILD 197
    camera::fovscene_log(); // BUILD 198
    camera::sprint_log();   // BUILD 200: the sprint probe's drain
    camera::lead_log();     // BUILD 201
    camera::gpair_log();    // BUILD 277: the pairing recorder's drain
    camera::late_log();     // BUILD 278
    roll_log();             // BUILD 202
    perf_log();             // BUILD 215: the mod's own time, before any gate
    ffr::tick_1hz();        // BUILD 216: NVAPI init on the first tick with a device, the mask, the ffr: line
    {   // BUILD 229: the canvas that was actually built, once, as a WARN so
        // it survives the quiet log at log_diag = 0.
        static bool s_said = false;
        if (!s_said && g_canvas_ready) {
            s_said = true;
            const double mp_before = (double)g_clamp_from_w * g_clamp_from_h / 1e6;
            const double mp_after  = (double)g_sc_w * g_sc_h / 1e6;
            LOG_WARN("xrcanvas: eye canvas %ux%u (%.1f MP). Unclamped it would "
                     "have been %ux%u (%.1f MP); the runtime recommends %ux%u, "
                     "content is %ux%u, canvas_clamp = %d. Restore the old "
                     "canvas with canvas_clamp = 0 and a relaunch.",
                     g_sc_w, g_sc_h, mp_after,
                     g_clamp_from_w, g_clamp_from_h, mp_before,
                     g_rec_w, g_rec_h, g_content_w, g_content_h,
                     g_cfg_canvas_clamp.load(std::memory_order_relaxed));
        }
    }
    {   // BUILD 228: the swapchain-image wait bound. WARN so it survives the
        // quiet log: log_diag = 0 drops INFO instrument lines and every
        // tester runs the shipped default.
        static unsigned s_c = 0, s_b = 0;
        const unsigned c = g_img_to_copy.load(std::memory_order_relaxed);
        const unsigned b = g_img_to_blit.load(std::memory_order_relaxed);
        if (c != s_c || b != s_b) {
            LOG_WARN("xrimg: xrWaitSwapchainImage TIMED OUT, bound %d ms "
                     "(copy=%u blit=%u total, +%u +%u this second). That eye "
                     "was skipped and the frame was still submitted with no "
                     "layer, so Present returns and the game keeps running. "
                     "xr_img_timeout_ms = 0 restores the unbounded wait.",
                     g_cfg_img_timeout.load(std::memory_order_relaxed),
                     c, b, c - s_c, b - s_b);
            s_c = c; s_b = b;
        }
    }
    LOG_INFO("hpos: on=%d ref=%d frames=%llu clamps=%llu refs=%llu last=(x %+.3f f %+.3f u %+.3f) m "
             "(build 204: the head's translation since the reference, head-local, as published to the "
             "camera hook; x right, f forward, u up; a lean forward shows as f growing; Home re-zeroes)",
             g_head_pos_on.load(std::memory_order_relaxed), g_head_ref_ok ? 1 : 0,
             (unsigned long long)g_hp_frames.load(std::memory_order_relaxed),
             (unsigned long long)g_hp_clamps.load(std::memory_order_relaxed),
             (unsigned long long)g_hp_refs.load(std::memory_order_relaxed),
             g_hp_last[0].load(std::memory_order_relaxed), g_hp_last[1].load(std::memory_order_relaxed),
             g_hp_last[2].load(std::memory_order_relaxed));   // BUILD 204
    LOG_INFO("stance: on=%d stage=%d height=%.2f stand=%.2f src=%d ratio=%.2f drop=%.2f thr=%.2f/%.2f desired=%d actual=%d dz=%.2f dzmax=%.2f | "
             "taps=%llu holds=%llu fights=%llu changes=%llu manual=%llu seated=%d b=%d (build 205: 0 stand 1 crouch 2 prone; desired from the "
             "physical height over the standing maximum, actual from the character's head bone; taps and holds "
             "are the pad's B the loop pressed; fights are back-offs after four acts without effect; build 301: drop = metres below "
             "the standing height taken at the recentre, thr = crouch_drop / crouch_rise)",
             g_stance_on.load(std::memory_order_relaxed), g_st_stage_ok ? 1 : 0, g_st_height, g_st_hstand, g_st_src,
             g_st_hstand > 1.0f ? g_st_height / g_st_hstand : 0.0f, g_st_drop,
             g_crouch_drop.load(std::memory_order_relaxed), g_crouch_rise.load(std::memory_order_relaxed), g_st_desired, g_st_actual,
             [](){ float d = 0.0f; headpose::head_dz(&d); return d; }(), g_st_dzmax,
             (unsigned long long)g_st_taps.load(std::memory_order_relaxed),
             (unsigned long long)g_st_holds.load(std::memory_order_relaxed),
             (unsigned long long)g_st_fights.load(std::memory_order_relaxed),
             (unsigned long long)g_st_changes.load(std::memory_order_relaxed),
             (unsigned long long)g_st_manual.load(std::memory_order_relaxed), g_st_seated ? 1 : 0,
             headpose::stance_b() ? 1 : 0);   // BUILD 205, 207
    LOG_INFO("leanin: soft=%llu menu_hold_recentres=%llu start_taps=%llu (build 300: soft = present frames the horizontal offset was past where "
             "the easing begins; menu_hold_recentres = left menu button held to recentre; start_taps = short presses sent to the game as Start on release)",
             (unsigned long long)g_lean_soft_n.load(std::memory_order_relaxed),
             (unsigned long long)g_lean_rc_menu.load(std::memory_order_relaxed),
             (unsigned long long)g_lean_start_taps.load(std::memory_order_relaxed));   // BUILD 300
    LOG_INFO("recenter: refs=%llu | runtime local=%llu absorbed=%llu stage=%llu other=%llu | home_space=%llu fp_f2=%llu session=%llu "
             "(build 212: every re-zero of the view by source. A re-zero re-takes the YAW reference and turns the world to where the head "
             "points, and resets the standing height: absorbed ones do neither, the view does not move at all)",
             (unsigned long long)g_hp_refs.load(std::memory_order_relaxed),
             (unsigned long long)g_rc_local.load(std::memory_order_relaxed),
             (unsigned long long)g_rc_absorbed.load(std::memory_order_relaxed),
             (unsigned long long)g_rc_stage.load(std::memory_order_relaxed),
             (unsigned long long)g_rc_other.load(std::memory_order_relaxed),
             (unsigned long long)g_rc_key.load(std::memory_order_relaxed),
             (unsigned long long)g_rc_fp.load(std::memory_order_relaxed),
             (unsigned long long)g_rc_begin.load(std::memory_order_relaxed));   // BUILD 212
    {   // BUILD 209, 210, 211
        unsigned long long inj = 0, injblk = 0; int injmax = 0;
        headpose::follow_inj_counts(&inj, &injblk, &injmax);
        LOG_INFO("follow: on=%d st=%d dist=%.2f stick=%.2f vbody=%.2f vhead=%.2f | frames=%llu advances=%llu adv=%.3f | "
                 "closes=%llu settles=%llu blocked=%llu bleeds=%llu bled=%.2f | teleports=%llu user=%llu rush=%llu nocam=%llu far=%llu | "
                 "inj=%llu injblk=%llu injmax=%d "
                 "(build 211: st 0 idle 1 closing 2 coast 3 settle; dist the horizontal head offset in the head's yaw frame; vbody the "
                 "character's own speed, which decides the stop; bled the metres the settle took out of the view; inj the polls the game "
                 "was handed the follow's stick and injmax the largest axis of it, of 32767)",
                 g_follow_on.load(std::memory_order_relaxed), g_fl_state.load(std::memory_order_relaxed),
                 g_fl_dist.load(std::memory_order_relaxed), g_fl_stick.load(std::memory_order_relaxed),
                 g_fl_vbody.load(std::memory_order_relaxed), g_fl_vhead.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_frames.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_advances.load(std::memory_order_relaxed), g_fl_adv.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_closes.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_settles.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_blocked.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_bleeds.load(std::memory_order_relaxed), g_fl_bled.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_teleports.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_user.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_rush.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_nocam.load(std::memory_order_relaxed),
                 (unsigned long long)g_fl_far.load(std::memory_order_relaxed), inj, injblk, injmax);
    }
    if (!g_input_ok) return;
    // Build 23.1: controller-aim diagnostic, logged whenever VR aim is on
    // with the controller source, even if pose flags look off.
    if (g_vraim_on && g_aim_source.load(std::memory_order_relaxed) == 1) {
        LOG_INFO("ctlaim: ctl yaw %+.1f pitch %+.1f | head yaw %+.1f "
                 "pitch %+.1f | yaw_ok %d",
                 g_ctl_diag[0].load(std::memory_order_relaxed) * 57.29578f,
                 g_ctl_diag[1].load(std::memory_order_relaxed) * 57.29578f,
                 g_ctl_diag[2].load(std::memory_order_relaxed) * 57.29578f,
                 g_ctl_diag[3].load(std::memory_order_relaxed) * 57.29578f,
                 g_ctl_diag_ok.load(std::memory_order_relaxed) ? 1 : 0);
    }
    // 2026-09-01: the look-reversal probe. Three yaws side by side, 1 Hz:
    // head is what the pump was fed, view is the engine's own view forward
    // (aim getter, world basis, yaw = atan2(x, y)), eng is the engine's
    // absolute setter yaw. Protocol: stand still, turn the head slowly left,
    // then right. head against view answers "is look reversed" end to end;
    // eng says which side of the pump any flip sits on. Log-only.
    // BUILD 159, log-only rider on 158 (CX-015): the owner localized the
    // reversal to VEHICLES ("the first person in vehicles is reveresed");
    // the 2026-09-01 on-foot runs could not reproduce it (54 of 56 turn
    // windows agreed). fov is the discriminator this line was missing:
    // on-foot world sits 0.78..0.87, sprint and vehicle ride near 1.22. The
    // protocol gains one leg: while DRIVING, sweep the head slowly left,
    // then right. The fov column marks the vehicle windows; head against
    // view inside them names the leg that reverses, and only then does any
    // sign change anywhere.
    if (camera::aim_available()) {
        float vf[3];
        const bool vok = aimtrace::view_fwd(vf);
        LOG_INFO("aimref: head=%+7.1f view=%+7.1f eng=%+7.1f deg fov=%.3f "
                 "v_ok=%d",
                 g_aimref_head.load(std::memory_order_relaxed) * 57.29578f,
                 (vok ? atan2f(vf[0], vf[1]) : 0.0f) * 57.29578f,
                 camera::aim_lastval(0) * 57.29578f,
                 headpose::read_fov(0.0f), vok ? 1 : 0);
    }
    {   // BUILD 289: flush the recoil ring to GRWVR\recoil-<pid>.csv (drain thread only; never the Present thread)
        const uint32_t w = g_rc_w.load(std::memory_order_acquire);
        if (g_rc_on.load(std::memory_order_relaxed) && w != g_rc_r) {
            uint32_t from = g_rc_r;
            if (w - from > kRcN) from = w - kRcN;     // the ring wrapped: keep the newest
            static FILE* s_f = nullptr;
            if (!s_f) {
                const std::wstring p = log::data_dir() + L"\\recoil-" + std::to_wstring(GetCurrentProcessId()) + L".csv";
                if (_wfopen_s(&s_f, p.c_str(), L"w") != 0) s_f = nullptr;
                if (s_f) {
                    fprintf(s_f, "t_ms,shots,trig,eng_deg,base_deg,head_deg,calls,ep_deg,barrel_deg,view_deg,cum_deg,flags,fov,obj\n");
                    LOG_INFO("recoil: recorder ON, writing recoil-%lu.csv (build 289/292, log only: one row per Present; eng = the pitch the engine "
                             "passes to SetPitch, base = the camera base pitch, head = what the pump chases, cum = the pump's booked pitch, "
                             "flags bit0 barrel block ran, bit1 pump live, bit2 firing, bit3 delta in flight; fov in radians, low = ADS)",
                             (unsigned long)GetCurrentProcessId());
                }
            }
            if (s_f) {
                for (uint32_t i = from; i != w; ++i) {
                    const RcRow& r = g_rc[i & (kRcN - 1)];
                    fprintf(s_f, "%u,%u,%.2f,%.2f,%.2f,%.2f,%u,%.2f,%.2f,%.2f,%.2f,%u,%.3f,%x\n", r.t_ms, r.shots, r.trig, r.eng, r.base, r.head,
                            r.calls, r.ep, r.barrel, r.view, r.cum, (unsigned)r.flags, r.fov, r.obj);
                }
                fflush(s_f);
            }
            g_rc_r = w;
        }
    }
    // BUILD 260: THE VEHICLE INSTRUMENT. Log only, no behaviour. The 2026-10-07 drive on the 251 baseline showed the
    // game's vehicle camera owns the view (the picture follows the car whatever the head does; it swings to the rear
    // view when reversing). Before any fix is built the project needs a VEHICLE STATE it can detect and a VEHICLE
    // HEADING that stays steady while the engine camera swings. This line prints both candidates side by side:
    //   gate   = the 2026-09-08 composite (VEHICLE-2026-09-08 section 6.2): A the camera is more than 3 m from the
    //            character, B the head bone sits 0.55..0.85 m above the origin (seated), C the first person anchor is live.
    //            The raw flags are printed so the thresholds can be judged against a real drive; edges = transitions.
    //   cam    = the engine camera's forward, as yaw (atan2(x, y), degrees, the same convention as aimref view) and the
    //            z component of that forward (up). This is the one that swings to the rear when reversing.
    //   root / head = the three model axes X Y Z of the pose root (the body) and of the head bone, each as
    //            yaw/up: the forward axis is the one whose up is near 0 and whose yaw follows the vehicle. If a
    //            seated character's body turns with the vehicle and does NOT swing when reversing, it is the base the
    //            head should be composed onto.
    //   vel    = the character origin's speed over the last second, its yaw heading, and its dot product with each
    //            root axis; the sign on the forward axis flips when the vehicle reverses.
    // Drive one loop that includes reversing. Judge the lines against that drive.
    if (g_veh_probe.load(std::memory_order_relaxed)) {
        camera::VehSample vs;
        camera::veh_sample(&vs);
        camera::MvpStats ms;
        camera::mvp_stats(&ms);
        {   // BUILD 262: dump the per-frame record on the way out of a vehicle and every ten seconds inside one
            static bool s_on_prev = false;
            static int s_dump_tick = 0;
            if ((s_on_prev && !vs.veh_on) || (vs.veh_on && ++s_dump_tick >= 10)) {
                camera::veh_dump_csv();
                s_dump_tick = 0;
            }
            s_on_prev = vs.veh_on;
        }
        float crot[9] = {}, cpos[3] = {};
        const bool cam_ok = camera::base_frame(crot, cpos);
        static ULONGLONG s_prev_ms = 0;
        static float     s_prev_o[3] = {};
        static bool      s_prev_ok = false;
        static bool      s_gate_prev = false;
        static unsigned  s_edges = 0;
        const ULONGLONG nowms = GetTickCount64();
        float vel[3] = {}, speed = 0.0f, vyaw = 0.0f;
        bool vel_ok = false;
        if (vs.origin_ok && s_prev_ok && nowms > s_prev_ms) {
            const float dt = (float)(nowms - s_prev_ms) / 1000.0f;
            if (dt > 0.2f && dt < 3.0f) {
                for (int i = 0; i < 3; ++i) vel[i] = (vs.origin[i] - s_prev_o[i]) / dt;
                speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
                vyaw = atan2f(vel[0], vel[1]) * 57.29578f;
                vel_ok = true;
            }
        }
        s_prev_ok = vs.origin_ok;
        s_prev_ms = nowms;
        if (vs.origin_ok) memcpy(s_prev_o, vs.origin, sizeof(s_prev_o));
        const bool ga = vs.cam_dist > 3.0f;
        const bool gb = vs.dz >= 0.55f && vs.dz <= 0.85f;
        const bool gc = vs.anchored;
        const bool gate = ga && gb && gc;
        if (gate != s_gate_prev) { ++s_edges; s_gate_prev = gate; }
        auto yawd = [](const float* v) { return atan2f(v[0], v[1]) * 57.29578f; };
        const float* ra = vs.root_ax;
        const float* ha = vs.head_ax;
        LOG_INFO("veh: compose=%d edges=%u rootfail=%u smooth=%.2f early(w/kept/lost)=%u/%u/%u begin(used/missed)=%u/%u mvp(%s calls c0/c2/other=%u/%u/%u builds 1cam/2cam=%u/%u cam2first=%u tid same/diff=%u/%u rows eq/ne=%u/%u label hit/miss=%u/%u sites A/B/other=%u/%u/%u) | gate(raw)=%d | A dist=%.2f m (%d) B dz=%+.2f (%d) C anchored (%d) | cam yaw %+.1f up %+.2f ok=%d | "
                 "root X %+.0f/%+.2f Y %+.0f/%+.2f Z %+.0f/%+.2f ok=%d | head X %+.0f/%+.2f Y %+.0f/%+.2f Z %+.0f/%+.2f ok=%d | "
                 "vel %.1f m/s yaw %+.0f dotX %+.1f dotY %+.1f dotZ %+.1f ok=%d "
                 "(build 260, log only: gate = A and B and C; axes are yaw/up of the model X, Y, Z in the world; dot = velocity "
                 "on each root axis, its sign flips on the forward axis when reversing)",
                 vs.veh_on ? 1 : 0, vs.veh_edges, vs.veh_rootfail, vs.veh_smooth, vs.pre_writes, vs.pre_kept, vs.pre_lost, vs.vb_used, vs.vb_missed, ms.live ? "LIVE" : "off", ms.calls[0], ms.calls[1], ms.calls[2], ms.builds_one, ms.builds_two, ms.cam2_first, ms.same_tid, ms.diff_tid, ms.rows_eq, ms.rows_ne, ms.label_hit, ms.label_miss, ms.site_a, ms.site_b, ms.site_other, gate ? 1 : 0, vs.cam_dist, ga ? 1 : 0, vs.dz, gb ? 1 : 0, gc ? 1 : 0,
                 cam_ok ? yawd(crot + 3) : 0.0f, cam_ok ? crot[5] : 0.0f, cam_ok ? 1 : 0,
                 vs.root_ok ? yawd(ra + 0) : 0.0f, vs.root_ok ? ra[2] : 0.0f,
                 vs.root_ok ? yawd(ra + 3) : 0.0f, vs.root_ok ? ra[5] : 0.0f,
                 vs.root_ok ? yawd(ra + 6) : 0.0f, vs.root_ok ? ra[8] : 0.0f, vs.root_ok ? 1 : 0,
                 vs.head_ok ? yawd(ha + 0) : 0.0f, vs.head_ok ? ha[2] : 0.0f,
                 vs.head_ok ? yawd(ha + 3) : 0.0f, vs.head_ok ? ha[5] : 0.0f,
                 vs.head_ok ? yawd(ha + 6) : 0.0f, vs.head_ok ? ha[8] : 0.0f, vs.head_ok ? 1 : 0,
                 speed, vyaw,
                 (vel_ok && vs.root_ok) ? vel[0] * ra[0] + vel[1] * ra[1] + vel[2] * ra[2] : 0.0f,
                 (vel_ok && vs.root_ok) ? vel[0] * ra[3] + vel[1] * ra[4] + vel[2] * ra[5] : 0.0f,
                 (vel_ok && vs.root_ok) ? vel[0] * ra[6] + vel[1] * ra[7] + vel[2] * ra[8] : 0.0f,
                 vel_ok ? 1 : 0);
    }
    // Build 84: barrel-aim liveness. Printed whenever the feature is armed,
    // including when it has never fired, because "armed and did nothing" and
    // "not armed" look identical in the headset and have different causes.
    // err is the LAST angular error the loop applied: it should be large on the
    // first frame of a trigger hold and fall toward zero while held. If it does
    // not fall, the engine is not absorbing and the pump is the problem, not
    // this. src distinguishes the real barrel from the ctrl_ray fallback, which
    // is the difference between testing this feature and testing build 23.
    if (g_aim_barrel.load(std::memory_order_relaxed) != 0) {
        LOG_INFO("barrel: mode=%d frames=%u src=%s | last err yaw %+.2f "
                 "pitch %+.2f deg | skip nodir=%u noview=%u overcap=%u",
                 g_aim_barrel.load(std::memory_order_relaxed),
                 g_bar_frames.load(std::memory_order_relaxed),
                 g_bar_src.load(std::memory_order_relaxed) == 1 ? "barrel" :
                 g_bar_src.load(std::memory_order_relaxed) == 2 ? "ctrlray" : "-",
                 g_bar_diag[0].load(std::memory_order_relaxed) * 57.29578f,
                 g_bar_diag[1].load(std::memory_order_relaxed) * 57.29578f,
                 g_bar_nodir.load(std::memory_order_relaxed),
                 g_bar_noview.load(std::memory_order_relaxed),
                 g_bar_clamp.load(std::memory_order_relaxed));
    }

    const uint32_t seen = g_in_seen.load(std::memory_order_relaxed);
    if (!seen) return;
    float p[2][3], q[2][4], t[2];
    for (int h = 0; h < 2; ++h) {
        for (int i = 0; i < 3; ++i) p[h][i] = g_in_pos[h][i].load(std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) q[h][i] = g_in_ori[h][i].load(std::memory_order_relaxed);
        t[h] = g_in_trig[h].load(std::memory_order_relaxed);
    }
    // BUILD 124: L and R here are LOGICAL slots, off hand and main hand. With
    // main_hand = 0 they are the physical right and left controller in that
    // order. The tag says which, so no future analysis of this line can be
    // done against the wrong hand.
    LOG_INFO("input: [main=%s] L%c(%.3f %.3f %.3f) q(%.3f %.3f %.3f %.3f) trig %.2f | "
             "R%c(%.3f %.3f %.3f) q(%.3f %.3f %.3f %.3f) trig %.2f | "
             "stick rc/act L=%d/%d R=%d/%d",
             g_main_hand.load(std::memory_order_relaxed) ? "right" : "LEFT",
             (seen & 1u) ? '+' : '-', p[0][0], p[0][1], p[0][2],
             q[0][0], q[0][1], q[0][2], q[0][3], t[0],
             (seen & 2u) ? '+' : '-', p[1][0], p[1][1], p[1][2],
             q[1][0], q[1][1], q[1][2], q[1][3], t[1],
             g_in_stick_rc[0].load(std::memory_order_relaxed) >> 1,
             g_in_stick_rc[0].load(std::memory_order_relaxed) & 1,
             g_in_stick_rc[1].load(std::memory_order_relaxed) >> 1,
             g_in_stick_rc[1].load(std::memory_order_relaxed) & 1);

    // BUILD 14d: aim steer liveness. yaw/pitch are the last computed
    // controller-vs-head offsets (only updated while the trigger is held);
    // dx/dy are total injected mouse counts.
    if (g_steer_on.load(std::memory_order_relaxed)) {
        LOG_INFO("aim: steer armed, last yaw %+.1f pitch %+.1f deg, injected %llu presents dx=%lld dy=%lld | ads %s, %u holds | fire %s, %u pulls | kicks=%ld",
                 g_steer_yaw_last.load(std::memory_order_relaxed),
                 g_steer_pitch_last.load(std::memory_order_relaxed),
                 (unsigned long long)g_steer_ticks.load(std::memory_order_relaxed),
                 (long long)g_steer_dx.load(std::memory_order_relaxed),
                 (long long)g_steer_dy.load(std::memory_order_relaxed),
                 g_ads_rmb.load(std::memory_order_relaxed) ? "HELD" : "idle",
                 g_ads_holds.load(std::memory_order_relaxed),
                 g_fire_lmb.load(std::memory_order_relaxed) ? "HELD" : "idle",
                 g_fire_pulls.load(std::memory_order_relaxed),
                 g_aim_kicks.load(std::memory_order_relaxed));
    }
}

void shutdown() {
    // BUILD 14f: never leave a synthetic right-mouse held past our lifetime.
    // BUILD 14h: nor a left-mouse.
    if (g_ads_rmb.load(std::memory_order_relaxed)) ads_send(false);
    if (g_fire_lmb.load(std::memory_order_relaxed)) fire_send(false);
    headpose::disable();
    g_active.store(false);
    for (int h = 0; h < 2; ++h) {
        if (g_aim_space[h]) { xrDestroySpace(g_aim_space[h]); g_aim_space[h] = XR_NULL_HANDLE; }
        if (g_grip_space[h]) { xrDestroySpace(g_grip_space[h]); g_grip_space[h] = XR_NULL_HANDLE; }
    }
    if (g_actionset) { xrDestroyActionSet(g_actionset); g_actionset = XR_NULL_HANDLE; }
    g_input_ok = false;
    if (g_staging)    { g_staging->Release(); g_staging = nullptr; }
    if (g_probe_tex)  { g_probe_tex->Release(); g_probe_tex = nullptr; }
    if (g_desk_rtv)   { g_desk_rtv->Release(); g_desk_rtv = nullptr; g_desk_rtv_key = nullptr; }
    for (int eye = 0; eye < 2; ++eye) {
        if (g_last_srv[eye]) { g_last_srv[eye]->Release(); g_last_srv[eye] = nullptr; }
        if (g_last[eye])     { g_last[eye]->Release();     g_last[eye]     = nullptr; }
        if (g_half_srv[eye]) { g_half_srv[eye]->Release(); g_half_srv[eye] = nullptr; }   // BUILD 247
        if (g_half[eye])     { g_half[eye]->Release();     g_half[eye]     = nullptr; }
    }
    g_blit_ok = false;
    // The SRV belongs to Menu.cpp, which frees it in its own shutdown; we only
    // ever borrowed the pointer, so drop it without releasing it.
    g_menu_srv = nullptr;
    if (g_menu_blend) { g_menu_blend->Release(); g_menu_blend = nullptr; }
    if (g_blit_dss)  { g_blit_dss->Release();  g_blit_dss  = nullptr; }
    if (g_blit_rs)   { g_blit_rs->Release();   g_blit_rs   = nullptr; }
    if (g_blit_samp) { g_blit_samp->Release(); g_blit_samp = nullptr; }
    if (g_blit_ps)   { g_blit_ps->Release();   g_blit_ps   = nullptr; }
    if (g_pip_ps)    { g_pip_ps->Release();    g_pip_ps    = nullptr; }   // BUILD 221
    if (g_pip_cb)    { g_pip_cb->Release();    g_pip_cb    = nullptr; }
    if (g_blit_vs)   { g_blit_vs->Release();   g_blit_vs   = nullptr; }
    for (int eye = 0; eye < 2; ++eye) {
        for (auto* r : g_rtv[eye]) {
            if (r) r->Release();
        }
        g_rtv[eye].clear();
        if (g_swapchains[eye]) {
            xrDestroySwapchain(g_swapchains[eye]);
            g_swapchains[eye] = XR_NULL_HANDLE;
        }
        g_canvas_ready = false;
    }
    if (g_view_space) { xrDestroySpace(g_view_space);    g_view_space = XR_NULL_HANDLE; }
    if (g_space)      { xrDestroySpace(g_space);         g_space = XR_NULL_HANDLE; }
    // 2026-08-12: retire the frame-wait worker. It is only ever idle-waiting on
    // g_wait_request or blocked inside xrWaitFrame, and the blocked case is the
    // whole reason this thread exists, so it is never joined: a join would
    // reintroduce exactly the hang this fix removes, just at shutdown.
    g_wait_exit.store(true, std::memory_order_release);
    if (g_wait_request) SetEvent(g_wait_request);

    if (g_wait_inflight.load(std::memory_order_acquire)) {
        // Tearing the session down underneath a thread that is still inside
        // xrWaitFrame on it is undefined. This runs at process exit, where the
        // OS reclaims both handles anyway, so leaking them beats racing.
        LOG_WARN("VR: the frame-wait worker is still inside xrWaitFrame. Leaving "
                 "the session and instance for the OS to reclaim rather than "
                 "destroying them under a blocked thread.");
    } else {
        if (g_session)   { xrDestroySession(g_session);     g_session = XR_NULL_HANDLE; }
        if (g_instance)  { xrDestroyInstance(g_instance);   g_instance = XR_NULL_HANDLE; }
    }
}

// Build 21: cfg hot-reload. load_config() is idempotent (every key clamps
// and lands in an atomic or seqlock publish), so re-running it on a file
// change IS the live-tuning mechanism: save grwxr.cfg (by hand or from
// tools/cfg_gui) and the values apply within about a second. Called at 1 Hz
// from the init thread's drain loop, never from Present (rule 8: file I/O).
// BUILD 227: read by dllmain before the palette probe's draw detours install.
bool palette_probe() { return g_palette_probe; }

void poll_config() {
    static FILETIME s_last = {};
    static bool     s_have = false;
    const std::wstring path = log::data_dir() + L"\\grwxr.cfg";
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return;                       // missing or mid-replace: retry next tick
    if (!s_have) {                    // baseline is the init-time load
        s_last = fad.ftLastWriteTime;
        s_have = true;
        return;
    }
    if (CompareFileTime(&fad.ftLastWriteTime, &s_last) == 0) return;
    s_last = fad.ftLastWriteTime;
    LOG_INFO("VR: grwxr.cfg changed on disk, reloading");
    load_config();
}

// Build 86: the same facts the 1 Hz "barrel:" log line carries, handed to the
// in-headset panel so a headset test never needs a log file. See VRMirror.h
// for why that matters more than it sounds.
BarrelStatus barrel_status() {
    BarrelStatus s;
    s.mode          = g_aim_barrel.load(std::memory_order_relaxed);
    s.src           = g_bar_src.load(std::memory_order_relaxed);
    s.frames        = g_bar_frames.load(std::memory_order_relaxed);
    s.nodir         = g_bar_nodir.load(std::memory_order_relaxed);
    s.noview        = g_bar_noview.load(std::memory_order_relaxed);
    s.overcap       = g_bar_clamp.load(std::memory_order_relaxed);
    s.err_yaw_deg   = g_bar_diag[0].load(std::memory_order_relaxed) * 57.29578f;
    s.err_pitch_deg = g_bar_diag[1].load(std::memory_order_relaxed) * 57.29578f;
    return s;
}

// Build 86: Numpad 1. Cycles the barrel aim so the A/B test can be run from
// inside the headset instead of from a text editor. Counters are reset on
// every change, because the question the panel answers ("did it drive the aim
// this time") is about the mode you just selected, not about a total that
// carries over from the previous one and reads as a false success.
void cycle_aim_barrel() {
    const int next = (g_aim_barrel.load(std::memory_order_relaxed) + 1) % 3;
    g_aim_barrel.store(next, std::memory_order_relaxed);
    g_bar_frames.store(0, std::memory_order_relaxed);
    g_bar_nodir.store(0, std::memory_order_relaxed);
    g_bar_noview.store(0, std::memory_order_relaxed);
    g_bar_clamp.store(0, std::memory_order_relaxed);
    g_bar_src.store(0, std::memory_order_relaxed);
    LOG_INFO("hotkey: aim_barrel = %d (%s)", next,
             next == 2 ? "always on" :
             next == 1 ? "only while firing" : "off");
}

// Build 86: Numpad 3. OFF is real hip fire, with ADS on the left trigger where
// the game itself puts it; ON restores the build 14f behaviour where a half
// pull raises the sights and every shot therefore passes through ADS.
void toggle_aim_ads() {
    const bool next = !g_ads_on.load(std::memory_order_relaxed);
    g_ads_on.store(next, std::memory_order_relaxed);
    LOG_INFO("hotkey: aim_ads = %d (%s)", next ? 1 : 0,
             next ? "trigger also aims down sights"
                  : "hip fire, ADS on the left trigger");
}

// Build 96: Numpad 0. The gun-jitter control. See VRMirror.h for why it is a
// key and for the two warnings that come with it.
//
// The log line names the CONSEQUENCE, not the variable, because the tester is
// reading this afterwards to work out which half of an A/B a stretch of the run
// belongs to, and "cam_selector_pose = 0" tells him nothing on its own.
void toggle_cam_pose() {
    const bool next = !headpose::cam_selector_pose();
    headpose::set_cam_selector_pose(next);
    camera::reset_head_telemetry();
    LOG_INFO("hotkey: cam_selector_pose = %d (%s)", next ? 1 : 0,
             next ? "camera pose write ON: first person and stereo live, and "
                    "the gun's origin rides the written camera row"
                  : "camera pose write OFF: first person is a no-op and both "
                    "eyes get the same image, but the gun's origin is the "
                    "engine's own camera again (the build 88 configuration). "
                    "This is a DIAGNOSTIC state, not a setting to keep.");
}

// Build 97: Numpad / and Numpad *. See VRMirror.h.
//
// The log line names which way the WORLD moved, not just the number, because
// the mapping is the opposite of the intuitive one and the tester is converging
// this by eye with no reference to check against. A narrower baseline is less
// parallax, which the brain reads as a more distant and therefore larger
// object: perceived size scales by (true IPD / rendered baseline).
void step_ipd_scale(int dir) {
    const float step = 0.05f;
    float v = headpose::ipd_scale() + (dir < 0 ? -step : step);
    // BUILD 117 (owner directive, 2026-08-20): the old band was 0.30..1.50 and
    // the comment here claimed "there is no legitimate tuning target outside
    // that". That was wrong, and it cost a test session. The owner reached the
    // rail while still reporting the near-field gun as crosseyed, which is the
    // one symptom this control exists to chase. A guess about what a tester
    // could possibly need is not a safety argument.
    //
    // Band is now 0.00..3.00.
    //
    // 0.00 IS DELIBERATE AND IT IS THE MOST USEFUL VALUE ON THE DIAL. It means
    // both eyes receive an identical view: no stereo separation at all. If the
    // gun STILL doubles at 0.00 then eye separation cannot be the cause, and
    // the fault is temporal (one eye per frame under AFR, with no stale-eye
    // reprojection) rather than geometric. That single press decides between
    // the two remaining candidates without a build, so it is worth more than
    // any value the old band could reach.
    //
    // The runaway-key argument the old comment made still holds, which is why
    // there is still a clamp at all: 3.00 bounds it, and the step is unchanged.
    if (v < 0.00f) v = 0.00f;
    if (v > 3.00f) v = 3.00f;
    // Snap to the step grid so repeated presses cannot accumulate float drift
    // into values like 0.7999998 that read as noise in the log.
    v = floorf(v * 20.0f + 0.5f) / 20.0f;
    headpose::set_ipd_scale(v);
    // BUILD 118: SAY WHAT SATURATION COSTS. The old line said only "lower =
    // bigger world" and never named a nominal value, so there was nothing on
    // screen to stop at. [VERIFIED, 479-log sweep 2026-08-20] ipd_scale has
    // only ever been PERSISTED at 0.50 and 0.90; 1.00 has never been run. On
    // 2026-08-20, minutes after the band was widened, it was driven 0.85 to
    // 0.00 in 4.1 seconds and then back one step to 0.05, and 0.05 became the
    // baseline. That is a saturated control, not a chosen value.
    //
    // Perceived depth and size both scale as 1/ipd_scale, because the brain
    // reads a smaller rendered baseline as a more distant, larger world. At
    // 0.05 the world reads 20x oversize and effectively flat. Printing that
    // multiplier is the whole fix: a tester cannot knowingly saturate a dial
    // that tells him what he is giving up. The BAND IS DELIBERATELY UNCHANGED
    // at 0.00..3.00 (re-clamping is the mistake CLAUDE.md already records).
    if (v >= 0.005f) {
        LOG_INFO("hotkey: ipd_scale = %.2f (world reads %.1fx life size. "
                 "1.00 = life size. Depth cues scale as 1/ipd_scale, so far "
                 "below 1.00 the world goes big and FLAT. Save grwxr.cfg to "
                 "keep it)", v, 1.0f / v);
    } else {
        LOG_INFO("hotkey: ipd_scale = %.2f (MONO: both eyes get the SAME "
                 "view, no stereo at all. If the gun still doubles here the "
                 "cause is NOT eye separation. 1.00 = life size)", v);
    }
}

// ---------------------------------------------------------------------------
// Build 99: THE TUNER. See VRMirror.h for why it exists and what the keys are.
//
// Each row is a name, a getter, a setter, a step, bounds, and one line of help
// that says what the tester will SEE change. The help matters more than it
// looks: he is reading these from a log after the fact to work out which half
// of an A/B a stretch of play belongs to, and a bare number does not tell him.
//
// The cfg default is captured the first time a row is touched, not at load, so
// Delete restores what the file actually said even after a hot-reload changed
// it under us.
// ---------------------------------------------------------------------------
namespace {

float tune_get_grip_fwd() { float v; camera::get_wgun_grip(&v, 0, 0, 0); return v; }
float tune_get_grip_lat() { float v; camera::get_wgun_grip(0, &v, 0, 0); return v; }
float tune_get_grip_up()  { float v; camera::get_wgun_grip(0, 0, &v, 0); return v; }
float tune_get_grip_two() { float v; camera::get_wgun_grip(0, 0, 0, &v); return v; }

void tune_set_grip(int which, float v) {
    float f, l, u, t;
    camera::get_wgun_grip(&f, &l, &u, &t);
    if (which == 0) f = v;
    if (which == 1) l = v;
    if (which == 2) u = v;
    if (which == 3) t = v;
    camera::set_wgun_grip(f, l, u, t);
}
void tune_set_grip_fwd(float v) { tune_set_grip(0, v); }
void tune_set_grip_lat(float v) { tune_set_grip(1, v); }
void tune_set_grip_up (float v) { tune_set_grip(2, v); }
void tune_set_grip_two(float v) { tune_set_grip(3, v); }

float tune_get_ipd() { return headpose::ipd_scale(); }
void  tune_set_ipd(float v) { headpose::set_ipd_scale(v); }

// Build 103: the ADS presentation, converged by eye. See HeadPose.h.
// Build 105: the aiming-point A/B, armable from inside the headset.
float tune_get_apab() { return aimtrace::aimpoint_ab() ? 1.0f : 0.0f; }
void  tune_set_apab(float v) {
    aimtrace::set_aimpoint_ab(v >= 0.5f, g_cfg_aimpoint_m);
}
float tune_get_apab_m() { return g_cfg_aimpoint_m; }
void  tune_set_apab_m(float v) {
    g_cfg_aimpoint_m = v;
    aimtrace::set_aimpoint_ab(aimtrace::aimpoint_ab(), g_cfg_aimpoint_m);
}

// BUILD 137: the direction write, armable and disarmable from inside the
// headset. This one belongs in the tuner more than most: the question it
// answers is "does the round go where the barrel points", and that is far
// easier to answer by turning it off and on between shots than by taking the
// headset off to edit a cfg and relaunching.
float tune_get_shot_dir() { return aimtrace::shot_dir() ? 1.0f : 0.0f; }
void  tune_set_shot_dir(float v) { aimtrace::set_shot_dir(v >= 0.5f); }
float tune_get_shot_org() { return aimtrace::shot_org() ? 1.0f : 0.0f; }
void  tune_set_shot_org(float v) { aimtrace::set_shot_org(v >= 0.5f); }

float tune_get_fs_lo() { return headpose::fs_fov_lo(); }
void  tune_set_fs_lo(float v) { headpose::set_fs_fov_lo(v); }

float tune_get_roll_deg() { return g_cfg_wgun_roll_deg; }
void  tune_set_roll_deg(float v) {
    g_cfg_wgun_roll_deg = v;
    camera::set_wgun_roll((int)g_cfg_wgun_roll, v);
}

float tune_get_pos_clamp() { return g_cfg_wgun_pos_clamp; }
void  tune_set_pos_clamp(float v) {
    g_cfg_wgun_pos_clamp = v;
    camera::set_wgun_pos((int)g_cfg_wgun_pos, g_cfg_wgun_pos_scale,
                         g_cfg_wgun_pos_clamp, g_cfg_wgun_pos_smooth);
}

// Build 101. Three knobs that a test has already asked about and that were not
// reachable from inside the headset, so the tester could not answer either
// question without a cfg edit he has told us he will not make.
float tune_get_pos_smooth() { return g_cfg_wgun_pos_smooth; }
void  tune_set_pos_smooth(float v) {
    g_cfg_wgun_pos_smooth = v;
    camera::set_wgun_pos((int)g_cfg_wgun_pos, g_cfg_wgun_pos_scale,
                         g_cfg_wgun_pos_clamp, g_cfg_wgun_pos_smooth);
}

float tune_get_maxstep() { return g_cfg_wgun_maxstep; }
void  tune_set_maxstep(float v) {
    g_cfg_wgun_maxstep = v;
    camera::set_wgun_filter(g_cfg_wgun_smooth, g_cfg_wgun_maxstep);
}

// A/B for the shoulder-left rotation reported 2026-08-15. With two-handed aim
// on, the barrel direction is the line from the rear hand to the front hand;
// with it off it is the rear controller's own forward. One keypress says which
// of the two the tester is actually fighting, which is a measurement instead of
// a theory about where his left hand sits when he shoulders.
float tune_get_twohand() { return g_cfg_wgun_twohand; }
float tune_get_two_agree() { return camera::get_wgun_two_agree(); }
void  tune_set_two_agree(float v) { g_cfg_wgun_two_agree = v; camera::set_wgun_two_agree(v); }
// 2026-08-31b: the attach band, live, because "how far can I drop my off hand
// before the gun lets go" is judged by feel in the headset and nowhere else.
float tune_get_two_attach() { return camera::get_wgun_two_attach(); }
void  tune_set_two_attach(float v) {
    g_cfg_wgun_two_attach = v;
    camera::set_wgun_two_attach(v);
}
// BUILD 125: the eye sign, live.
// BUILD 126: the blob test, live.
float tune_get_grip_bypass() { return (float)camera::get_wgun_grip_bypass(); }
void  tune_set_grip_bypass(float v) {
    const bool on = (v >= 0.5f);
    g_cfg_wgun_grip_bypass = on ? 1.0f : 0.0f;
    camera::set_wgun_grip_bypass(on ? 1 : 0);
}

float tune_get_ipd_swap() { return camera::get_ipd_sign() < 0 ? 1.0f : 0.0f; }
void  tune_set_ipd_swap(float v) {
    const bool sw = (v >= 0.5f);
    g_cfg_ipd_swap = sw ? 1.0f : 0.0f;
    camera::set_ipd_sign(sw ? -1 : 1);
}

// BUILD 124: handedness, live. Changing it swaps which physical controller the
// weapon pivots on, so the weapon filters are reseeded through the same
// generation counter the other direction-changing settings use.
float tune_get_main_hand() { return (float)g_main_hand.load(std::memory_order_relaxed); }
void  tune_set_main_hand(float v) {
    const int want = (v >= 0.5f) ? 1 : 0;
    g_cfg_main_hand = (float)want;
    if (g_main_hand.exchange(want, std::memory_order_relaxed) != want)
        camera::reseed_wgun();
}

// BUILD 123: the front-hand detection, so a bad call can be undone in the
// headset without a cfg edit or a rebuild.
float tune_get_two_auto() { return (float)camera::get_wgun_two_auto(); }
void  tune_set_two_auto(float v) {
    g_cfg_wgun_two_auto = (v >= 0.5f) ? 1.0f : 0.0f;
    camera::set_wgun_two_auto((int)g_cfg_wgun_two_auto);
}
void  tune_set_twohand(float v) {
    g_cfg_wgun_twohand = (v >= 0.5f) ? 1.0f : 0.0f;
    camera::set_wgun_twohand((int)g_cfg_wgun_twohand);
}

// 2026-08-31: the hands, the anchor, and the hide slots, live, because the
// hide slots in particular ARE a headset measurement (step until an arm
// vanishes, then look at the hand) and taking the headset off between steps
// is how measurements stall for weeks.
float tune_get_hands() { return g_cfg_wgun_hands; }
void  tune_set_hands(float v) {
    g_cfg_wgun_hands = (v >= 0.5f) ? 1.0f : 0.0f;
    camera::set_wgun_hands((int)g_cfg_wgun_hands);
}
float tune_get_hands_grab() { return g_cfg_wgun_hands_grab; }
void  tune_set_hands_grab(float v) {
    g_cfg_wgun_hands_grab = (v >= 0.5f) ? 1.0f : 0.0f;
    camera::set_wgun_hands_grab((int)g_cfg_wgun_hands_grab);
}
float tune_get_hide_arms() { return g_cfg_wgun_hide_arms; }
void  tune_set_hide_arms(float v) {
    g_cfg_wgun_hide_arms = (v >= 0.5f) ? 1.0f : 0.0f;
    camera::set_wgun_hide_arms((int)g_cfg_wgun_hide_arms);
}
// Build 153: the evidence reticle, on the panel so it can be armed between
// takes without leaving the headset. These write the drawing state directly;
// there is no camera:: call because nothing outside this file consumes them.
float tune_get_reticle() { return (float)g_ret_mode; }
void  tune_set_reticle(float v) {
    g_ret_mode = (v >= 1.5f) ? 2 : (v >= 0.5f ? 1 : 0);
}
float tune_get_reticle_dist() { return g_ret_dist; }
void  tune_set_reticle_dist(float v) {
    if (v >= 1.0f && v <= 500.0f) g_ret_dist = v;
}

float tune_get_hand_free() { return g_cfg_wgun_hand_free; }
void  tune_set_hand_free(float v) {
    g_cfg_wgun_hand_free = (v >= 0.5f) ? 1.0f : 0.0f;
    camera::set_wgun_hand_free((int)g_cfg_wgun_hand_free);
}
float tune_get_arms() { return g_cfg_wgun_arms; }
void  tune_set_arms(float v) {
    g_cfg_wgun_arms = (v >= 0.5f) ? 1.0f : 0.0f;
    camera::set_wgun_arms((int)g_cfg_wgun_arms);
}
float tune_get_wrist() { return g_cfg_wgun_wrist; }
void  tune_set_wrist(float v) {
    g_cfg_wgun_wrist = (v >= 0.5f) ? 1.0f : 0.0f;
    camera::set_wgun_wrist((int)g_cfg_wgun_wrist);
}
float tune_get_pitch() { return g_cfg_wgun_pitch_deg; }
void  tune_set_pitch(float v) {
    g_cfg_wgun_pitch_deg = v;
    camera::set_wgun_pitch(v);
}
float tune_get_two_grip() { return g_cfg_wgun_two_grip; }
void  tune_set_two_grip(float v) {
    g_cfg_wgun_two_grip = (v >= 0.5f) ? 1.0f : 0.0f;
    camera::set_wgun_two_grip((int)g_cfg_wgun_two_grip);
}
// The capture is momentary: stepping the row to 1 arms exactly one capture
// and the row snaps back to 0, so it reads as a button.
float tune_get_calib() { return 0.0f; }
void  tune_set_calib(float v) {
    if (v >= 0.5f) camera::set_hand_calib(1);
}
float tune_get_two_anchor() { return g_cfg_wgun_two_anchor; }
void  tune_set_two_anchor(float v) {
    g_cfg_wgun_two_anchor = (v >= 0.5f) ? 1.0f : 0.0f;
    camera::set_wgun_two_anchor((int)g_cfg_wgun_two_anchor);
}
float tune_get_hide_child() { return g_cfg_hide_child; }
void  tune_set_hide_child(float v) {
    g_cfg_hide_child = (float)(int)v;
    camera::set_hide_child(0, (int)g_cfg_hide_child);
}
float tune_get_hide_child2() { return g_cfg_hide_child2; }
void  tune_set_hide_child2(float v) {
    g_cfg_hide_child2 = (float)(int)v;
    camera::set_hide_child(1, (int)g_cfg_hide_child2);
}

struct Tunable {
    const char* name;
    float (*get)();
    void  (*set)(float);
    float step, lo, hi;
    const char* help;
};

// BUILD 101: EVERY RAIL WIDENED. `[USER DIRECTIVE, 2026-08-15]` "you absolutely
// can not cap things like this you always are too conservative with your cap."
// He is right and the log proves it: he ran wgun_grip_up into its +0.40 rail and
// pressed Page Up thirty-one more times against a value that had stopped moving.
//
// A rail on a tuning row is not a safety device. The safety device is
// wgun_pos_clamp, which bounds the FINAL placement no matter what these say. So
// these exist only to stop a stuck key from running a number to infinity, and
// they belong far outside any value a person would choose, not just outside the
// value I guessed at.
const Tunable kTunables[] = {
    {"wgun_grip_fwd", tune_get_grip_fwd, tune_set_grip_fwd, 0.02f, -2.00f, 2.00f,
     "slides the gun ALONG ITS BARREL through your right hand: + puts your hand "
     "further back on the weapon"},
    {"wgun_grip_up", tune_get_grip_up, tune_set_grip_up, 0.02f, -2.00f, 2.00f,
     "raises or lowers the gun in your right hand"},
    {"wgun_grip_lat", tune_get_grip_lat, tune_set_grip_lat, 0.02f, -2.00f, 2.00f,
     "shifts the gun left or right in your right hand"},
    {"wgun_grip_two", tune_get_grip_two, tune_set_grip_two, 0.10f, 0.00f, 1.00f,
     "the FRONT HAND HOLD: 0 the left hand only points, 1 the handguard lands "
     "in it, 0.5 splits the difference. Cannot move point of aim"},
    {"wgun_twohand", tune_get_twohand, tune_set_twohand, 1.00f, 0.00f, 1.00f,
     "1 aims the gun along the line between your two hands, 0 aims it along the "
     "right controller alone. Press this if the gun rotates when you shoulder"},
    {"wgun_hands", tune_get_hands, tune_set_hands, 1.00f, 0.00f, 1.00f,
     "THE HANDS HOLD THE GUN. 1 carries the character's hands and fingers with "
     "the weapon so the grip you see is the animation's own, wherever you move "
     "it. 0 leaves the hands to the animation and only the gun moves"},
    {"wgun_hands_grab", tune_get_hands_grab, tune_set_hands_grab, 1.00f, 0.00f, 1.00f,
     "THE GRAB. 1: the character's LEFT hand rides the weapon only while your "
     "real off hand holds it, and stays with the animation otherwise, so idle "
     "cannot force both hands on. 0: both hands carried always (build 139)"},
    {"wgun_hand_free", tune_get_hand_free, tune_set_hand_free, 1.00f, 0.00f, 1.00f,
     "HAND PRESENCE. 1: off the weapon, your character's left hand follows "
     "your left controller, so you can reach over your shoulder or down to "
     "your thigh and see it. The grab then slides it from your hand onto the "
     "foregrip. 0: a free hand stays with the animation"},
    {"wgun_arms", tune_get_arms, tune_set_arms, 1.00f, 0.00f, 1.00f,
     "THE ARMS FOLLOW. 1: the elbows bend so each arm reaches its hand, using "
     "the animation's own elbow direction, which is the cure for the "
     "stretching that keeps the arms visible. 0: arms stay with the "
     "animation. wgun_hide_arms overrides this when on"},
    {"wgun_pitch_deg", tune_get_pitch, tune_set_pitch, 0.50f, -45.00f, 45.00f,
     "THE BARREL-TO-BULLET TRIM. Shots print HIGH of where the barrel looks: "
     "raise this until holes and barrel agree at a wall. Rotates only what "
     "the gun model shows; the bullet's own line never moves, so this cannot "
     "break the muzzle rule"},
    {"wgun_two_grip", tune_get_two_grip, tune_set_two_grip, 1.00f, 0.00f, 1.00f,
     "GRIP TO HOLD. 1: the two-handed aim engages only while your off hand "
     "SQUEEZES its grip button, like native VR games, and releases when you "
     "let go. This is the fix for the aim riding a hand that is not really "
     "gripping. 0: the hold engages on hand positions alone"},
    {"hand_calib", tune_get_calib, tune_set_calib, 1.00f, 0.00f, 1.00f,
     "WINGSPAN CAPTURE, a one-shot button on a 3.5 s timer. F3 does the same "
     "thing without opening this panel, which is the easier route. Have the "
     "weapon OUT and move it around first (the avatar's arm lengths are only "
     "measured while a weapon is drawn), then fire this, let go, and stand "
     "with both controllers straight out to your sides at shoulder height. A "
     "good capture APPLIES the arm scale and switches to first person, and "
     "says so"},
    {"barrel_reticle", tune_get_reticle, tune_set_reticle, 1.00f, 0.00f, 2.00f,
     "EVIDENCE RETICLE. 1 draws a GREEN ring where the published barrel "
     "direction points, from your own muzzle. 2 adds an ORANGE ring along "
     "camera forward. Both sit at barrel_reticle_dist metres, so the gap between "
     "them IS how far the gun and your head disagree, on video. The round "
     "should land on the GREEN one. It does not predict an impact at a "
     "different range: the camera and the muzzle are not the same point"},
    {"barrel_reticle_dist", tune_get_reticle_dist, tune_set_reticle_dist, 5.00f,
     1.00f, 500.00f,
     "Metres out to place both rings. Set it to roughly your target range or "
     "the two rings answer a question you are not asking"},
    {"wgun_hide_arms", tune_get_hide_arms, tune_set_hide_arms, 1.00f, 0.00f, 1.00f,
     "HIDE THE ARMS AT BONE LEVEL. Collapses both arms onto the shoulders to "
     "kill the stretch between the animated arm and the carried hand. Try "
     "hide_child pieces FIRST; this is the fallback if a piece takes the hand "
     "with the arm. Off restores stock arms instantly"},
    {"wgun_two_anchor", tune_get_two_anchor, tune_set_two_anchor, 1.00f, 0.00f, 1.00f,
     "1 rests the weapon on your REAR hand when your right hand measures as the "
     "front one, which is how you actually hold it. 0 is the old behaviour that "
     "anchored to the right controller regardless. Turn off if the gun sits in "
     "the wrong hand"},
    {"hide_child", tune_get_hide_child, tune_set_hide_child, 1.00f, -1.00f, 63.00f,
     "HIDE ONE BODY PIECE. Step until the piece you want gone disappears (the "
     "log names each step). -1 is off. THE QUESTION THAT MATTERS: when an arm "
     "vanishes, does the hand vanish with it? Say what you see"},
    {"hide_child2", tune_get_hide_child2, tune_set_hide_child2, 1.00f, -1.00f, 63.00f,
     "the second hide slot, same as hide_child, because you have two arms"},
    {"wgun_grip_bypass", tune_get_grip_bypass, tune_set_grip_bypass, 1.00f, 0.00f, 1.00f,
     "THE BLOB TEST. Parks the gun ON your tracked hand by ignoring the grip "
     "offset, so you can see whether the gun lands on the orange marker. Your "
     "tuned grip values are kept and come back when you turn this off"},
    {"ipd_swap", tune_get_ipd_swap, tune_set_ipd_swap, 1.00f, 0.00f, 1.00f,
     "SWAP THE EYES. 1 swapped, 0 as shipped. The F1 panel row is the better "
     "way. If swapped is the right one, ipd_scale goes back to 1.00 and the "
     "world stops reading 20x too large"},
    {"main_hand", tune_get_main_hand, tune_set_main_hand, 1.00f, 0.00f, 1.00f,
     "WHICH HAND HOLDS THE GRIP AND PULLS THE TRIGGER. 1 right, 0 left. Your "
     "last run pulled the LEFT trigger 19 times and the right one once, so ADS "
     "and FIRE were both watching the hand you do not shoot with"},
    {"wgun_two_auto", tune_get_two_auto, tune_set_two_auto, 1.00f, 0.00f, 1.00f,
     "1 works out which hand is in FRONT, 0 always assumes the left. Your last "
     "run measured your RIGHT hand in front, which pointed the gun backwards and "
     "made the gate refuse every frame. Leave this at 1"},
    {"wgun_two_agree", tune_get_two_agree, tune_set_two_agree, 0.05f, -1.00f, 1.00f,
     "THE TWO-HAND GATE. Refuses the front hand when the line between your hands "
     "disagrees with the right controller. Leave this at 0.35: the refusals were "
     "the backwards barrel, not a tight gate, and wgun_two_auto is the fix"},
    {"wgun_two_attach", tune_get_two_attach, tune_set_two_attach, 0.05f, 0.25f, 3.00f,
     "HOW FAR YOUR OFF HAND CAN LEAVE THE WEAPON before it lets go of the hold "
     "(metres between your hands; release completes 0.15 further out). Real "
     "holds measure 0.11 to 0.41. Raise it if the hold releases while you are "
     "still gripping, lower it if a hanging hand still drags the gun. 3.00 "
     "disables the release"},
    {"wgun_roll_deg", tune_get_roll_deg, tune_set_roll_deg, 5.0f, -180.0f, 180.0f,
     "rotates the gun about its own barrel. Cannot move point of aim"},
    {"wgun_pos_clamp", tune_get_pos_clamp, tune_set_pos_clamp, 0.05f, 0.05f, 8.00f,
     "how far the gun may sit from where the engine put it. Raise it if the gun "
     "will not reach your hand. Watch clamped= in the wgun log line"},
    {"wgun_pos_smooth", tune_get_pos_smooth, tune_set_pos_smooth, 0.05f, 0.02f, 1.00f,
     "how hard the gun's POSITION is filtered: 1.00 is raw and instant, lower is "
     "smoother and laggier. Raise it if the gun feels like it is swimming"},
    {"wgun_maxstep_deg", tune_get_maxstep, tune_set_maxstep, 2.50f, 0.50f, 90.00f,
     "the most the gun may ROTATE in one call. Raise it if fast hand turns feel "
     "rubbery, lower it if tracking pops throw the muzzle"},
    {"ipd_scale", tune_get_ipd, tune_set_ipd, 0.05f, 0.00f, 3.00f,
     "eye separation. 1.00 IS LIFE SIZE and has never actually been run. The "
     "world reads 1/ipd_scale oversize and equally flatter, so 0.05 is a 20x "
     "giant flat world. 0.00 = both eyes get the SAME view, so if the gun "
     "still doubles there it is NOT the IPD"},
    {"aimpoint_ab", tune_get_apab, tune_set_apab, 1.00f, 0.00f, 1.00f,
     "THE SHOT TEST. Alternates the weapon's aiming point left then right on "
     "each trigger pull. Fire ten single shots at a wall: ONE group of holes "
     "or TWO?"},
    {"aimpoint_ab_m", tune_get_apab_m, tune_set_apab_m, 1.00f, 0.50f, 25.00f,
     "how far the shot test throws the aiming point each way, in metres"},
    {"shot_dir", tune_get_shot_dir, tune_set_shot_dir, 1.00f, 0.00f, 1.00f,
     "THE ROUND FOLLOWS THE BARREL. 1 on, 0 the game's own aim. Only touches "
     "shots that start at your own muzzle, so it cannot reach the enemy AI. "
     "Turn it off and on between shots to compare"},
    {"shot_org", tune_get_shot_org, tune_set_shot_org, 1.00f, 0.00f, 1.00f,
     "THE ROUND LEAVES THE MUZZLE. When the game builds your shot at the "
     "CAMERA (aiming, obstruction, its hybrid shoot system) the origin is "
     "moved back to your muzzle instead of the shot being refused. 0 restores "
     "the build 137 refusal"},
    {"fullscreen_fov_lo", tune_get_fs_lo, tune_set_fs_lo, 0.02f, 0.05f, 1.30f,
     "AIMING DOWN SIGHTS. Step DOWN past 0.33 to stop ADS drawing as a small "
     "flat window and make it fill the view like the rest of the game"},
};
const int kTunableCount = (int)(sizeof(kTunables) / sizeof(kTunables[0]));

int   g_tune_sel = 0;
float g_tune_default[kTunableCount] = {};
bool  g_tune_have_default[kTunableCount] = {};

// BUILD 120: THE TUNER SPEAKS. This was the whole point of the tuner and it
// was missing.
//
// Every tuner press wrote one LOG_INFO line and nothing else. The tester is
// inside a headset, where the log, the cfg and the desktop screen are all
// unreachable, which is the exact problem Voice.h was written for ("the mod
// talks, because the tester cannot read"). So the instruction "press INSERT
// until it says wgun_grip_two" was unfollowable: nothing ever said anything.
//
// Rule 8: say() only copies a string into a queue under a lock and returns,
// and all COM lives on Voice's own thread. The tuner runs on the hotkey poll,
// not on Present, which is one of the two callers Voice.h names as intended.
//
// The name is shortened for speech: the "wgun_" prefix is dropped and
// underscores become spaces, so "wgun_grip_two" is spoken "grip two".
void tune_speak(const Tunable& t, const char* suffix) {
    const char* n = t.name;
    if (!strncmp(n, "wgun_", 5)) n += 5;
    char nice[80];
    size_t i = 0;
    for (; n[i] && i + 1 < sizeof(nice); ++i) nice[i] = (n[i] == '_') ? ' ' : n[i];
    nice[i] = 0;
    char sp[200];
    snprintf(sp, sizeof(sp), "%s, %.2f%s", nice, t.get(), suffix ? suffix : "");
    voice::say(sp);
}

void tune_capture_default(int i) {
    if (!g_tune_have_default[i]) {
        g_tune_default[i] = kTunables[i].get();
        g_tune_have_default[i] = true;
    }
}

}  // namespace

void tuner_cycle() {
    g_tune_sel = (g_tune_sel + 1) % kTunableCount;
    const Tunable& t = kTunables[g_tune_sel];
    tune_capture_default(g_tune_sel);
    LOG_INFO("tuner: [%d/%d] %s = %.2f  (Page Up / Page Down step by %.2f, "
             "Delete resets). %s",
             g_tune_sel + 1, kTunableCount, t.name, t.get(), t.step, t.help);
    tune_speak(t, nullptr);
}

// Build 101: `mult` is 10 while Shift is held, so a knob that needs 30 cm of
// travel costs three presses instead of fifteen. Same reason the rails moved:
// the tester should never be grinding a key to reach a value.
//
// AND IT SAYS WHEN IT IS RAILED. Build 99 printed the same number over and over
// while the tester kept pressing, with nothing in the line to distinguish "this
// moved" from "this cannot move". That is a probe reporting its own capacity
// limit, which is measurement rule 2, and it was missing here.
void tuner_step(int dir, int mult) {
    const Tunable& t = kTunables[g_tune_sel];
    tune_capture_default(g_tune_sel);
    if (mult < 1) mult = 1;
    const float before = t.get();
    float v = before + (dir < 0 ? -t.step : t.step) * (float)mult;
    bool railed = false;
    if (v < t.lo) { v = t.lo; railed = true; }
    if (v > t.hi) { v = t.hi; railed = true; }
    // Snap to the step grid so repeated presses cannot accumulate float drift
    // into values that read as noise in the log.
    const float inv = 1.0f / t.step;
    v = floorf(v * inv + 0.5f) / inv;
    t.set(v);
    if (railed && v == before) {
        LOG_INFO("tuner: %s = %.2f AT ITS %s RAIL (%.2f .. %.2f), pressing again "
                 "will not move it. Say so and the rail moves",
                 t.name, v, dir < 0 ? "LOWER" : "UPPER", t.lo, t.hi);
        tune_speak(t, ", at its limit");
    } else {
        LOG_INFO("tuner: %s = %.2f%s  (%s)", t.name, v,
                 mult > 1 ? "  [coarse x10]" : "", t.help);
        tune_speak(t, nullptr);
    }
}

void tuner_reset() {
    const Tunable& t = kTunables[g_tune_sel];
    tune_capture_default(g_tune_sel);
    t.set(g_tune_default[g_tune_sel]);
    LOG_INFO("tuner: %s RESET to %.2f (the value it had when you first touched "
             "it this run; saving grwxr.cfg also restores the file's value)",
             t.name, g_tune_default[g_tune_sel]);
    tune_speak(t, ", reset");
}

// Build 97: Numpad +. See VRMirror.h. This is the control for the reversed
// head roll reported 2026-08-15, not the fix for it.
void toggle_cam_pose_rot() {
    const bool next = !headpose::cam_pose_rot();
    headpose::set_cam_pose_rot(next);
    LOG_INFO("hotkey: cam_pose_rot = %d (%s)", next ? 1 : 0,
             next ? "head rotation composed into the camera basis: ROLL now "
                    "reaches the content, so it should agree with the pose we "
                    "advertise. The camera will also fight you on yaw and "
                    "pitch, which is expected and is why this ships off. "
                    "DIAGNOSTIC only"
                  : "position-only camera write: the engine keeps its own "
                    "basis and nothing argues with where you are looking");
}

}  // namespace vr
}  // namespace grwxr
