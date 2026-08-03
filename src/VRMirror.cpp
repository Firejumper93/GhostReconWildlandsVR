#include "VRMirror.h"
#include "CameraProbe.h"
#include "HeadPose.h"
#include "Log.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
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
XrPath      g_hand[2]      = {XR_NULL_PATH, XR_NULL_PATH};
XrSpace     g_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
bool        g_input_ok = false;
// Hot path -> heartbeat handoff (rule 8: the present hook only stores plain
// atomics; the heartbeat thread formats and logs).
std::atomic<uint32_t> g_in_seen{0};   // bit0 = left tracked, bit1 = right
std::atomic<float>    g_in_pos[2][3] = {};
std::atomic<float>    g_in_ori[2][4] = {};
std::atomic<float>    g_in_trig[2]   = {};
std::atomic<float>    g_in_grip[2]   = {};   // build 14e.2

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
float g_aim_sign_yaw   = 1.0f;          // cfg aim_yaw_sign  (+1 or -1)
float g_aim_sign_pitch = 1.0f;          // cfg aim_pitch_sign
float g_aim_cum[2]     = {0.0f, 0.0f};  // absorbed by the engine, geometric rad
float g_aim_zero[2]    = {0.0f, 0.0f};  // head-angle baseline (re-zeroed while
                                        // off or fov-gated: no snap on resume)
float g_aim_out[2]     = {0.0f, 0.0f};  // armed, not yet observed consumed
bool  g_aim_outf[2]    = {false, false};

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
ID3D11SamplerState*       g_blit_samp = nullptr;
ID3D11RasterizerState*    g_blit_rs   = nullptr;
ID3D11DepthStencilState*  g_blit_dss  = nullptr;
ID3D11ShaderResourceView* g_last_srv[2] = {nullptr, nullptr};
bool g_blit_ok = false;

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

// BUILD 10c: read the one user knob from GRWVR\grwxr.cfg at init. Missing
// file or key leaves the default. Lines are `key=value`; `#` starts a comment.
void load_config() {
    const std::wstring path = log::data_dir() + L"\\grwxr.cfg";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || !f) {
        LOG_INFO("VR: no grwxr.cfg, ipd_scale = 1.00 (Numpad 9/- adjusts, * resets)");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        float v = 0.0f;
        if (sscanf_s(line, " ipd_scale = %f", &v) == 1) {
            if (v < -2.0f) v = -2.0f;
            if (v >  2.0f) v =  2.0f;
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
            headpose::set_fs_fov(v);
            headpose::set_fs_enabled(v > 0.0f);
        }
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
    }
    fclose(f);
    g_ipd_default = headpose::ipd_scale();
    LOG_INFO("VR: grwxr.cfg read, ipd_scale = %.2f (Numpad 9/- adjusts, * resets to it),",
             headpose::ipd_scale());
    LOG_INFO("VR: mono_scope_fov = %.2f rad (eye offset collapses below this rendered fov)",
             headpose::mono_scope_fov());
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
void aim_pump(float head_yaw, float head_pitch) {
    const float head[2] = {head_yaw, head_pitch};
    // Injection pauses below the world fov band (ADS and optics, hazard 25
    // territory) and while toggled off. While paused the baseline re-zeroes
    // every frame, so resuming never snaps: aim continues from wherever the
    // engine has it, tracking head MOVEMENT from that moment on.
    const bool live = g_vraim_on &&
                      headpose::read_fov(0.7853982f) >= 0.65f;
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
        const float sign = a == 0 ? g_aim_sign_yaw : g_aim_sign_pitch;
        if (camera::aim_arm(a, d * sign) == 1) {
            g_aim_out[a]  = d;
            g_aim_outf[a] = true;
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
        note("VR: head reference captured, yaw %.1f deg. Head tracking live.",
             psi * 57.29578f);
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
        aim_pump(atan2f(Hg[3], Hg[4]), asinf(sp));
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
    if (XR_FAILED(xrAcquireSwapchainImage(g_swapchains[eye], &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    bool copied = false;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(g_swapchains[eye], &wi))) {
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
        "void vs_main(uint id : SV_VertexID, out float4 pos : SV_Position,\n"
        "             out float2 uv : TEXCOORD0) {\n"
        "    float2 t = float2((id << 1) & 2, id & 2);\n"
        "    pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);\n"
        "    uv  = t;\n"
        "}\n"
        "float4 ps_main(float4 pos : SV_Position,\n"
        "               float2 uv : TEXCOORD0) : SV_Target {\n"
        "    return tex0.Sample(smp0, uv);\n"
        "}\n";

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
    if (!ok) {
        LOG_ERROR("VR: 11a blit state creation failed; blit disabled.");
        if (g_blit_dss)  { g_blit_dss->Release();  g_blit_dss  = nullptr; }
        if (g_blit_rs)   { g_blit_rs->Release();   g_blit_rs   = nullptr; }
        if (g_blit_samp) { g_blit_samp->Release(); g_blit_samp = nullptr; }
        if (g_blit_ps)   { g_blit_ps->Release();   g_blit_ps   = nullptr; }
        if (g_blit_vs)   { g_blit_vs->Release();   g_blit_vs   = nullptr; }
    }
    return ok;
}

// BUILD 11a: acquire one image of the eye's canvas, clear it black, DRAW the
// eye's captured frame (g_last mips) at the angular size of `fovy`, release.
// Assumes blit_bind() state is current. Render thread, no logging (rule 8).
bool blit_into(int eye, ID3D11DeviceContext* ctx, float fovy) {
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
    if (XR_FAILED(xrAcquireSwapchainImage(g_swapchains[eye], &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    bool drawn = false;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(g_swapchains[eye], &wi)) &&
        idx < g_rtv[eye].size() && g_rtv[eye][idx]) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ctx->ClearRenderTargetView(g_rtv[eye][idx], black);
        // Content spans [-tan_h..+tan_h] x [-tan_v..+tan_v] about the optical
        // center; the viewport is that span in canvas pixels. Fractional
        // placement is intended (viewports are float); the sampler resolves it.
        const float tan_v = tanf(0.5f * fovy);
        const float tan_h = tan_v * ((float)g_content_w / (float)g_content_h);
        D3D11_VIEWPORT vp;
        vp.TopLeftX = g_cx[eye] - tan_h * g_sx[eye];
        vp.TopLeftY = g_cy[eye] - tan_v * g_sy[eye];
        vp.Width    = 2.0f * tan_h * g_sx[eye];
        vp.Height   = 2.0f * tan_v * g_sy[eye];
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShaderResources(0, 1, &g_last_srv[eye]);
        ctx->OMSetRenderTargets(1, &g_rtv[eye][idx], nullptr);
        ctx->Draw(3, 0);
        drawn = true;

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
        if (XR_FAILED(xrCreateSwapchain(g_session, &scci, &g_swapchains[eye])))
            return false;

        uint32_t ic = 0;
        xrEnumerateSwapchainImages(g_swapchains[eye], 0, &ic, nullptr);
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

}  // namespace

bool init(const d3d11::State& st) {
    if (g_session != XR_NULL_HANDLE || g_failed.load()) return g_session != XR_NULL_HANDLE;
    if (!st.device || !st.context) return false;

    LOG_INFO("VR: initialising OpenXR on the game's D3D11 device");
    load_config();

    // BUILD 10d: two foreign implicit API layers sit inside every call we
    // make (flagged by the session 1 probe, confirmed enabled in the
    // registry): OBS Mirror and Virtual Desktop's Oculus compatibility shim.
    // The mod needs neither, and both are suspects for the offset-independent
    // monocular doubling. Each honors a disable_environment variable that the
    // loader reads at instance creation, so setting them here suppresses the
    // layers for this process only.
    SetEnvironmentVariableA("DISABLE_XR_APILAYER_NOVENDOR_OBSMirror", "1");
    SetEnvironmentVariableA("DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY", "1");
    uint32_t lc = 0;
    xrEnumerateApiLayerProperties(0, &lc, nullptr);
    std::vector<XrApiLayerProperties> lps(lc, {XR_TYPE_API_LAYER_PROPERTIES});
    if (lc) xrEnumerateApiLayerProperties(lc, &lc, lps.data());
    LOG_INFO("VR: %u API layer(s) visible after suppression%s", lc,
             lc ? ":" : ". OBS Mirror and VD compatibility are out of the path.");
    for (uint32_t i = 0; i < lc; ++i) LOG_INFO("VR:   %s", lps[i].layerName);

    // --- extensions ---
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    if (n) xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    bool has_d3d11 = false;
    for (auto& e : exts) {
        if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) has_d3d11 = true;
    }
    if (!has_d3d11) {
        LOG_ERROR("VR: runtime does not expose XR_KHR_D3D11_enable. Cannot continue.");
        g_failed.store(true);
        return false;
    }

    const char* enabled[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    strcpy_s(ici.applicationInfo.applicationName, "GRW-XR");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "AnvilNext");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XrResult r = xrCreateInstance(&ici, &g_instance);
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

    // The runtime dictates which adapter. If the game's device is on a different
    // one we must know, because sharing textures across adapters will not work.
    PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&pfn);
    if (pfn) {
        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (XR_SUCCEEDED(pfn(g_instance, g_system, &req))) {
            LOG_INFO("VR: runtime wants adapter LUID %08lX:%08lX",
                     (unsigned long)req.adapterLuid.HighPart,
                     (unsigned long)req.adapterLuid.LowPart);
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

        // Oculus Touch bindings: the aim pose (runtime-defined forward ray of
        // the controller, exactly what gun direction wants), trigger value,
        // and grip (squeeze) value.
        XrPath prof, p_aim[2], p_trig[2], p_grip[2];
        if (XR_FAILED(xrStringToPath(g_instance, "/interaction_profiles/oculus/touch_controller", &prof)) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/aim/pose",       &p_aim[0]))  ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right/input/aim/pose",      &p_aim[1]))  ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/trigger/value",  &p_trig[0])) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right/input/trigger/value", &p_trig[1])) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left/input/squeeze/value",  &p_grip[0])) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/right/input/squeeze/value", &p_grip[1]))) {
            LOG_INFO("VR: 13b binding paths FAILED, input disabled");
            return false;
        }
        XrActionSuggestedBinding sb[6] = {
            {g_act_aim,  p_aim[0]},  {g_act_aim,  p_aim[1]},
            {g_act_trig, p_trig[0]}, {g_act_trig, p_trig[1]},
            {g_act_grip, p_grip[0]}, {g_act_grip, p_grip[1]},
        };
        XrInteractionProfileSuggestedBinding ipsb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        ipsb.interactionProfile = prof;
        ipsb.countSuggestedBindings = 6;
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
        LOG_INFO("VR: 13b input plumbing up (aim pose + trigger + grip, both hands, Touch profile)");
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
    XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
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
    if (g_state_xr != XR_SESSION_STATE_READY) {
        LOG_ERROR("VR: session never reached READY. Is the headset awake and on?");
        g_failed.store(true);
        return false;
    }
    r = xrBeginSession(g_session, &sbi);
    if (XR_FAILED(r)) return fail("xrBeginSession", r);

    LOG_INFO("VR: session begun. Submitting the game frame to the headset.");
    LOG_INFO("VR: build 11f, big flat scope (%.2f rad window) + FP demo", g_scope_disp_fov);
    LOG_INFO("VR: FP keys: Numpad 8 toggle, 7/4 fwd, 6/5 side, 3/0 up/down (0.10 m steps)");
    LOG_INFO("VR: fullscreen %s, fov %.2f rad (Numpad 1 toggle, 2/+ step 0.10)",
             headpose::fs_enabled() ? "ON" : "OFF", headpose::fs_fov());
    LOG_INFO("VR: desktop view %s, crop fov %.2f rad (Numpad / toggle; steady "
             "left eye for recording)", g_desk_on ? "ON" : "OFF", g_desk_fov);
    LOG_INFO("VR: Home recenters. Numpad + and - step the eye separation by");
    LOG_INFO("VR: 0.05, Numpad * resets it to 1.00; each change is logged.");
    LOG_INFO("VR: Judge in the headset; the desktop mirror vibrates by design.");
    g_active.store(true);
    return true;
}

void on_present(const d3d11::State& st) {
    // Build 8.1: the session survives a doff. Taking the headset off sends
    // STOPPING; build 8 stopped polling events there, never called
    // xrEndSession, and so left the runtime showing a frozen frame with no way
    // back. Now STOPPING parks the session (xrEndSession, stop submitting) but
    // events keep draining every frame, and the READY that arrives when the
    // headset is put back on re-begins the session with a FRESH head
    // reference, since the headset has physically moved.
    if (g_session == XR_NULL_HANDLE || g_dead) return;
    if (!st.device || !st.context || !st.swapchain) return;

    // Drain session events cheaply, active or parked.
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            g_state_xr = ((XrEventDataSessionStateChanged*)&ev)->state;
            note("VR: session state -> %s", state_name(g_state_xr));
            if (g_state_xr == XR_SESSION_STATE_STOPPING) {
                headpose::disable();
                g_active.store(false);
                xrEndSession(g_session);
            } else if (g_state_xr == XR_SESSION_STATE_READY &&
                       !g_active.load(std::memory_order_relaxed)) {
                XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
                sbi.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_session, &sbi))) {
                    g_have_ref = false;
                    // Build 10a: the pre-doff eye images and poses are stale
                    // in every sense; bootstrap both eyes afresh.
                    g_eye[0].valid = false;
                    g_eye[1].valid = false;
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
            // Build 10a.1: the runtime recentred (user long-press recenter).
            // Both the yaw reference and the stored per-eye poses are in the
            // old space; recapture everything.
            g_have_ref = false;
            g_eye[0].valid = false;
            g_eye[1].valid = false;
            note("VR: runtime recenter. Head reference will re-capture.");
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
        const bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (down && !s_was_down) {
            g_have_ref = false;
            note("VR: recenter requested (Home). Head reference will re-capture.");
        }
        s_was_down = down;
    }

    // Build 10c: live eye-separation tuning, same edge-triggered pattern.
    // Numpad 9 steps ipd_scale +0.05, Numpad - steps -0.05, Numpad * resets
    // to the startup value (cfg default).
    // Build 10m: the 10h coarse merge-hunt keys are REMOVED. The merge
    // measurement is done, and VK_RETURN is global, so any Enter press
    // (menus, chat) silently bumped depth by +0.5 (hazard 15; it fired in
    // the 20:44 run, kicking a tuned -0.50 back to 0.00 right before quit).
    // Increase moved from Numpad + to Numpad 9 (user request), and the
    // sweep range [-20, +20] tightened back to [-2, +2].
    {
        static bool s_add = false, s_sub = false, s_mul = false;
        const bool add = (GetAsyncKeyState(VK_NUMPAD9)  & 0x8000) != 0;
        const bool sub = (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
        const bool mul = (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) != 0;
        float s = headpose::ipd_scale();
        bool changed = false;
        if (add && !s_add) { s += 0.05f; changed = true; }
        if (sub && !s_sub) { s -= 0.05f; changed = true; }
        if (mul && !s_mul) { s = g_ipd_default; changed = true; }
        if (changed) {
            if (s < -2.0f) s = -2.0f;
            if (s >  2.0f) s =  2.0f;
            headpose::set_ipd_scale(s);
            note("VR: ipd scale = %.2f (persist it as ipd_scale=%.2f in grwxr.cfg)", s, s);
        }
        s_add = add; s_sub = sub; s_mul = mul;
    }

    // Build 11c: first-person demo controls, same edge-triggered pattern.
    // Numpad 8 toggles the forward push, Numpad 7 / Numpad 4 step the
    // distance by +-0.10 m for in-headset tuning of "inside the head".
    {
        static bool s_tog = false, s_up = false, s_dn = false;
        const bool tog = (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) != 0;
        const bool up  = (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) != 0;
        const bool dn  = (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0;
        if (tog && !s_tog) {
            const bool on = !headpose::fp_enabled();
            headpose::set_fp_enabled(on);
            note("VR: first-person demo %s (fp_forward %.2f m, Numpad 7/4 steps)",
                 on ? "ON" : "OFF", headpose::fp_forward());
        }
        // Build 18: the head-hide override simply follows the FP state.
        // Re-published every poll (not only on the edge) so the detour is
        // correct even if FP was toggled before the hook finished installing.
        camera::set_head_hide(headpose::fp_enabled());
        // Build 15e: Numpad 7/4 now step whichever distance is actually in
        // play, so no new keys are needed (hazard 24). Anchored: eye height
        // above the character origin. Not anchored: the 11c forward push.
        // Build 16a: when the HEAD BONE anchor is the one in play, those same
        // keys tune fp_head_eye (the joint-to-eye trim, 0.02 m steps because
        // the whole range is a few centimeters) instead of fp_eye. Still no
        // new keys: the pair always steps whatever the viewpoint is actually
        // riding on.
        bool moved = false;
        const bool head_live = headpose::fp_head_anchor() &&
                               headpose::head_node() != 0xFFFFu;
        if (headpose::player_obj() && head_live) {
            float e = headpose::fp_head_eye();
            if (up && !s_up) { e += 0.02f; moved = true; }
            if (dn && !s_dn) { e -= 0.02f; moved = true; }
            if (moved) {
                if (e < -0.5f) e = -0.5f;
                if (e >  1.0f) e =  1.0f;
                headpose::set_fp_head_eye(e);
                note("VR: fp_head_eye = %.2f m above the HEAD BONE "
                     "(persist as fp_head_eye=%.2f in grwxr.cfg)", e, e);
            }
        } else if (headpose::player_obj()) {
            float e = headpose::fp_eye();
            if (up && !s_up) { e += 0.05f; moved = true; }
            if (dn && !s_dn) { e -= 0.05f; moved = true; }
            if (moved) {
                if (e < 0.0f) e = 0.0f;
                if (e > 2.5f) e = 2.5f;
                headpose::set_fp_eye(e);
                note("VR: fp_eye = %.2f m above the character "
                     "(persist as fp_eye=%.2f in grwxr.cfg)", e, e);
            }
        } else {
            float d = headpose::fp_forward();
            if (up && !s_up) { d += 0.10f; moved = true; }
            if (dn && !s_dn) { d -= 0.10f; moved = true; }
            if (moved) {
                if (d < 0.0f) d = 0.0f;
                if (d > 4.0f) d = 4.0f;
                headpose::set_fp_forward(d);
                note("VR: fp_forward = %.2f m (persist as fp_forward=%.2f in grwxr.cfg)", d, d);
            }
        }
        // Build 11f: side (Numpad 6 right, Numpad 5 left) and up (Numpad 3
        // up, Numpad 0 down), 0.10 m steps, for centering the view on the
        // head (the right-shoulder camera makes a pure forward push land
        // right of it). Build 15e.3: like Numpad 7/4, 6/5 step whichever
        // side offset is in play: fp_anchor_side (0.05 m) while pinned,
        // the 11f fp_side otherwise.
        static bool s_r = false, s_l = false, s_u2 = false, s_d2 = false;
        const bool kr = (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;
        const bool kl = (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;
        const bool ku = (GetAsyncKeyState(VK_NUMPAD3) & 0x8000) != 0;
        const bool kd = (GetAsyncKeyState(VK_NUMPAD0) & 0x8000) != 0;
        if (headpose::player_obj()) {
            bool moved2 = false;
            float as = headpose::fp_anchor_side();
            if (kr && !s_r) { as += 0.05f; moved2 = true; }
            if (kl && !s_l) { as -= 0.05f; moved2 = true; }
            if (moved2) {
                if (as < -1.0f) as = -1.0f;
                if (as >  1.0f) as =  1.0f;
                headpose::set_fp_anchor_side(as);
                note("VR: fp_anchor_side = %.2f m (persist as "
                     "fp_anchor_side=%.2f in grwxr.cfg)", as, as);
            }
        } else {
            bool moved2 = false;
            float sd = headpose::fp_side();
            if (kr && !s_r) { sd += 0.10f; moved2 = true; }
            if (kl && !s_l) { sd -= 0.10f; moved2 = true; }
            if (moved2) {
                if (sd < -2.0f) sd = -2.0f;
                if (sd >  2.0f) sd =  2.0f;
                headpose::set_fp_side(sd);
                note("VR: fp_side = %.2f (persist as fp_side in grwxr.cfg)",
                     sd);
            }
        }
        {
            bool moved3 = false;
            float ud = headpose::fp_up();
            if (ku && !s_u2) { ud += 0.10f; moved3 = true; }
            if (kd && !s_d2) { ud -= 0.10f; moved3 = true; }
            if (moved3) {
                if (ud < -2.0f) ud = -2.0f;
                if (ud >  2.0f) ud =  2.0f;
                headpose::set_fp_up(ud);
                note("VR: fp_up = %.2f (persist as fp_up in grwxr.cfg)", ud);
            }
        }
        s_r = kr; s_l = kl; s_u2 = ku; s_d2 = kd;
        s_tog = tog; s_up = up; s_dn = dn;
    }

    // Build 12a: fullscreen controls, same edge-triggered pattern. Numpad 1
    // toggles the world-band fov override, Numpad 2 / Numpad + step the
    // render fov by -/+0.10 rad for in-headset tuning (more fov = more
    // coverage but softer image at a fixed render resolution).
    {
        static bool s_tog = false, s_dn = false, s_up = false;
        const bool tog = (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0;
        const bool dn  = (GetAsyncKeyState(VK_NUMPAD2) & 0x8000) != 0;
        const bool up  = (GetAsyncKeyState(VK_ADD)     & 0x8000) != 0;
        if (tog && !s_tog) {
            const bool on = !headpose::fs_enabled();
            headpose::set_fs_enabled(on);
            note("VR: fullscreen %s (fov %.2f rad, Numpad 2/+ steps)",
                 on ? "ON" : "OFF", headpose::fs_fov());
        }
        bool moved = false;
        float f = headpose::fs_fov();
        if (up && !s_up) { f += 0.10f; moved = true; }
        if (dn && !s_dn) { f -= 0.10f; moved = true; }
        if (moved) {
            if (f < 0.8f) f = 0.8f;
            if (f > 2.5f) f = 2.5f;
            headpose::set_fs_fov(f);
            note("VR: fullscreen_fov = %.2f rad (persist as fullscreen_fov=%.2f in grwxr.cfg)",
                 f, f);
        }
        s_tog = tog; s_dn = dn; s_up = up;
    }

    // Build 12c: desktop recording view toggle, same edge-triggered pattern.
    {
        static bool s_tog = false;
        const bool tog = (GetAsyncKeyState(VK_DIVIDE) & 0x8000) != 0;
        if (tog && !s_tog) {
            g_desk_on = !g_desk_on;
            note("VR: desktop view %s (crop fov %.2f rad, desktop_fov in grwxr.cfg)",
                 g_desk_on ? "ON (steady left eye, normal fov)" : "OFF (raw wide render)",
                 g_desk_fov);
        }
        s_tog = tog;
    }
    // Build 19: Numpad Decimal toggles VR HEAD AIM (repurposed from the
    // build-17 one-shot experiment, which it supersedes). Default off.
    {
        static bool s_yb = false;
        const bool yb = (GetAsyncKeyState(VK_DECIMAL) & 0x8000) != 0;
        if (yb && !s_yb) {
            if (!camera::aim_available()) {
                note("aim: VR head aim unavailable (setter hooks not installed)");
            } else {
                g_vraim_on = !g_vraim_on;
                note("aim: VR HEAD AIM %s (signs yaw%+.0f pitch%+.0f, "
                     "aim_yaw_sign/aim_pitch_sign in grwxr.cfg)",
                     g_vraim_on ? "ON: bullets follow your gaze" : "OFF",
                     g_aim_sign_yaw, g_aim_sign_pitch);
            }
        }
        s_yb = yb;
    }
    if (g_dead || !g_active.load(std::memory_order_relaxed)) {
        // BUILD 14f: a parked or dead session must not hold ADS (doff safety).
        // BUILD 14h: nor fire.
        if (g_ads_rmb.load(std::memory_order_relaxed)) ads_send(false);
        if (g_fire_lmb.load(std::memory_order_relaxed)) fire_send(false);
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
    const int built_eye = headpose::pop_eye_tag(built_q, &built_q_ok);

    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g_session, &fwi, &fs))) return;

    // Build 8: head pose for this frame's predicted display time, published to
    // the camera hook. No logging here (rule 8); note() defers.
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    bool head_ori_ok = false;
    XrQuaternionf head_ori{0.0f, 0.0f, 0.0f, 1.0f};
    if (XR_SUCCEEDED(xrLocateSpace(g_view_space, g_space,
                                   fs.predictedDisplayTime, &loc))) {
        update_head(loc.locationFlags, loc.pose.orientation);
        // BUILD 10j: this is the orientation the frame's content is composed
        // with (both eyes share it); the submit path attaches it to both views.
        if (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
            head_ori = loc.pose.orientation;
            head_ori_ok = true;
        }
    }

    // BUILD 13b: sync the action set and locate both aim poses. Read-only,
    // stores into atomics; the heartbeat thread logs them (drain_input).
    // xrSyncActions returns SESSION_NOT_FOCUSED harmlessly until focus.
    if (g_input_ok) {
        XrActiveActionSet aas{g_actionset, XR_NULL_PATH};
        XrActionsSyncInfo syn{XR_TYPE_ACTIONS_SYNC_INFO};
        syn.countActiveActionSets = 1;
        syn.activeActionSets = &aas;
        if (XR_SUCCEEDED(xrSyncActions(g_session, &syn))) {
            uint32_t seen = 0;
            for (int h = 0; h < 2; ++h) {
                XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
                if (XR_SUCCEEDED(xrLocateSpace(g_aim_space[h], g_space,
                                               fs.predictedDisplayTime, &hl)) &&
                    (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                    (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                    seen |= 1u << h;
                    g_in_pos[h][0].store(hl.pose.position.x, std::memory_order_relaxed);
                    g_in_pos[h][1].store(hl.pose.position.y, std::memory_order_relaxed);
                    g_in_pos[h][2].store(hl.pose.position.z, std::memory_order_relaxed);
                    g_in_ori[h][0].store(hl.pose.orientation.x, std::memory_order_relaxed);
                    g_in_ori[h][1].store(hl.pose.orientation.y, std::memory_order_relaxed);
                    g_in_ori[h][2].store(hl.pose.orientation.z, std::memory_order_relaxed);
                    g_in_ori[h][3].store(hl.pose.orientation.w, std::memory_order_relaxed);
                }
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = g_act_trig;
                gi.subactionPath = g_hand[h];
                XrActionStateFloat tf{XR_TYPE_ACTION_STATE_FLOAT};
                if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &tf)) && tf.isActive)
                    g_in_trig[h].store(tf.currentState, std::memory_order_relaxed);
                gi.action = g_act_grip;
                tf = {XR_TYPE_ACTION_STATE_FLOAT};
                if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &tf)) && tf.isActive)
                    g_in_grip[h].store(tf.currentState, std::memory_order_relaxed);
            }
            g_in_seen.store(seen, std::memory_order_relaxed);
        }
    }

    // BUILD 14d/14e: controller aim steer, gated on the right grip. Uses
    // the input state stored just above.
    steer_aim();

    // BUILD 14f: right trigger drives the game's own ADS.
    ads_input();
    // BUILD 14h: full pull fires. After ads_input so a single frame that
    // crosses both thresholds aims before it shoots.
    fire_input();

    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g_session, &fbi))) return;

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
            if (fresh < 0) {
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
                blit_bind(st.context);
            }

            // Capture the fresh frame into its eye's private texture first:
            // it is the blit source now, and the 10i stale re-copy source
            // as before.
            if (g_canvas_ready && g_last[fresh]) {
                st.context->CopySubresourceRegion(g_last[fresh], 0, 0, 0, 0,
                                                  back, 0, nullptr);
                if (blitting) st.context->GenerateMips(g_last_srv[fresh]);
            }

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
    xrEndFrame(g_session, &fei);
}

bool active() { return g_active.load(); }

void drain_log() {
    if (!g_want_log.exchange(false, std::memory_order_acq_rel)) return;
    LOG_INFO("%s", g_deferred);
}

// BUILD 13b: 1 Hz controller state line, heartbeat thread. Position in LOCAL
// meters, orientation quaternion, trigger 0..1. Silent until a hand tracks,
// so headset-only runs stay uncluttered.
void drain_input() {
    if (!g_input_ok) return;
    const uint32_t seen = g_in_seen.load(std::memory_order_relaxed);
    if (!seen) return;
    float p[2][3], q[2][4], t[2];
    for (int h = 0; h < 2; ++h) {
        for (int i = 0; i < 3; ++i) p[h][i] = g_in_pos[h][i].load(std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) q[h][i] = g_in_ori[h][i].load(std::memory_order_relaxed);
        t[h] = g_in_trig[h].load(std::memory_order_relaxed);
    }
    LOG_INFO("input: L%c(%.3f %.3f %.3f) q(%.3f %.3f %.3f %.3f) trig %.2f | "
             "R%c(%.3f %.3f %.3f) q(%.3f %.3f %.3f %.3f) trig %.2f",
             (seen & 1u) ? '+' : '-', p[0][0], p[0][1], p[0][2],
             q[0][0], q[0][1], q[0][2], q[0][3], t[0],
             (seen & 2u) ? '+' : '-', p[1][0], p[1][1], p[1][2],
             q[1][0], q[1][1], q[1][2], q[1][3], t[1]);

    // BUILD 14d: aim steer liveness. yaw/pitch are the last computed
    // controller-vs-head offsets (only updated while the trigger is held);
    // dx/dy are total injected mouse counts.
    if (g_steer_on.load(std::memory_order_relaxed)) {
        LOG_INFO("aim: steer armed, last yaw %+.1f pitch %+.1f deg, injected %llu presents dx=%lld dy=%lld | ads %s, %u holds | fire %s, %u pulls",
                 g_steer_yaw_last.load(std::memory_order_relaxed),
                 g_steer_pitch_last.load(std::memory_order_relaxed),
                 (unsigned long long)g_steer_ticks.load(std::memory_order_relaxed),
                 (long long)g_steer_dx.load(std::memory_order_relaxed),
                 (long long)g_steer_dy.load(std::memory_order_relaxed),
                 g_ads_rmb.load(std::memory_order_relaxed) ? "HELD" : "idle",
                 g_ads_holds.load(std::memory_order_relaxed),
                 g_fire_lmb.load(std::memory_order_relaxed) ? "HELD" : "idle",
                 g_fire_pulls.load(std::memory_order_relaxed));
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
    }
    if (g_actionset) { xrDestroyActionSet(g_actionset); g_actionset = XR_NULL_HANDLE; }
    g_input_ok = false;
    if (g_staging)    { g_staging->Release(); g_staging = nullptr; }
    if (g_probe_tex)  { g_probe_tex->Release(); g_probe_tex = nullptr; }
    if (g_desk_rtv)   { g_desk_rtv->Release(); g_desk_rtv = nullptr; g_desk_rtv_key = nullptr; }
    for (int eye = 0; eye < 2; ++eye) {
        if (g_last_srv[eye]) { g_last_srv[eye]->Release(); g_last_srv[eye] = nullptr; }
        if (g_last[eye])     { g_last[eye]->Release();     g_last[eye]     = nullptr; }
    }
    g_blit_ok = false;
    if (g_blit_dss)  { g_blit_dss->Release();  g_blit_dss  = nullptr; }
    if (g_blit_rs)   { g_blit_rs->Release();   g_blit_rs   = nullptr; }
    if (g_blit_samp) { g_blit_samp->Release(); g_blit_samp = nullptr; }
    if (g_blit_ps)   { g_blit_ps->Release();   g_blit_ps   = nullptr; }
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
    if (g_session)   { xrDestroySession(g_session);     g_session = XR_NULL_HANDLE; }
    if (g_instance)  { xrDestroyInstance(g_instance);   g_instance = XR_NULL_HANDLE; }
}

}  // namespace vr
}  // namespace grwxr
