// HeadPose.cpp - see HeadPose.h.

#include "HeadPose.h"

#include <atomic>
#include <cstring>

namespace grwxr {
namespace headpose {
namespace {

// Seqlock. The writer bumps the counter to odd, stores the data, bumps it back
// to even; a reader accepts a copy only if it saw the same even value on both
// sides of it. There is exactly one writer (the Present hook), so the writer
// side needs no CAS. The data elements are relaxed atomics rather than plain
// floats so the concurrent access is defined behaviour; on x64 they compile to
// ordinary moves. The odd store is seq_cst so the data stores cannot become
// visible before the "write in progress" mark does.
std::atomic<uint32_t> g_seq{0};
std::atomic<bool>     g_live{false};
std::atomic<float>    g_R[9] = {};
// Build 13a: the same orientation as an absolute XR-space quaternion, inside
// the same seqlock so R and q can never be read from different publishes.
std::atomic<float>    g_Q[4] = {};

// Build 9. Stored as float bits so 0 can mean "never published": no rendered
// fovy is 0.0f, and the reader falls back until the first real publish.
std::atomic<uint32_t> g_fov_bits{0};

// Build 10b. Same encoding for the measured IPD.
std::atomic<uint32_t> g_ipd_bits{0};

// Build 10c. Plain atomic float: 0.0 is a legitimate value here (mono), so
// the bits-with-zero-meaning-unset encoding above would eat it.
std::atomic<float> g_ipd_scale{1.0f};

// Build 11b. See HeadPose.h.
std::atomic<float> g_mono_scope_fov{0.30f};

// Build 11c. See HeadPose.h.
std::atomic<bool>  g_fp_enabled{false};
std::atomic<float> g_fp_forward{2.20f};

// Build 11f. See HeadPose.h.
std::atomic<float> g_fp_side{-0.40f};
std::atomic<float> g_fp_up{0.0f};
// Build 15e: anchored first person. Eye height above the CHARACTER ORIGIN
// (obj+0x120, [VERIFIED]), meters, world up. 0.85 is the user's tuned value
// (2026-08-02; the origin is not at the feet).
std::atomic<float> g_fp_eye{0.85f};
// Build 15e.3: anchored lateral offset, meters along the base camera's right
// axis, for centering the viewpoint on the head (the body blades in
// weapon-ready stances, so the head is not above the origin).
std::atomic<float> g_fp_anchor_side{0.0f};
// The pinned player character object, published by the probe's 1 Hz pin and
// read per frame by the camera write. 0 = no pin, fall back to the push.
std::atomic<unsigned long long> g_player_obj{0};

// Build 16a. See HeadPose.h. 0.10 m is the head joint to eye trim, not
// fp_eye's 0.85 m origin-to-eye rise.
std::atomic<bool>         g_fp_head_anchor{true};
std::atomic<float>        g_fp_head_eye{0.10f};
std::atomic<unsigned int> g_head_node{0xFFFFu};

// Build 12a. See HeadPose.h. Enabled by default: fullscreen is the intended
// mode; Numpad 1 drops back to the windowed view for A/B.
std::atomic<bool>  g_fs_enabled{true};
std::atomic<float> g_fs_fov{1.92f};

// Build 10b.1. Eye-tag ring, power-of-two size. 16 slots is far deeper than
// any real render-ahead queue; a full ring drops the push, and the resulting
// -1 pops downgrade frames to mono rather than desyncing the eyes.
// Build 13a: each slot also carries the XR-space orientation the frame was
// composed with. Plain (non-atomic) fields are defined behaviour here: the
// single producer fills the slot BEFORE the release store of g_tag_w, and the
// single consumer reads it AFTER the acquire load, so the indices order every
// slot access.
struct Tag { uint8_t eye; float q[4]; };
constexpr uint64_t    kTagRing = 16;
std::atomic<uint64_t> g_tag_w{0}, g_tag_r{0};
Tag                   g_tags[kTagRing] = {};

}  // namespace

void publish(const float R[9], const float q_xr[4]) {
    const uint32_t s = g_seq.load(std::memory_order_relaxed);
    g_seq.store(s + 1, std::memory_order_seq_cst);
    for (int i = 0; i < 9; ++i) g_R[i].store(R[i], std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) g_Q[i].store(q_xr[i], std::memory_order_relaxed);
    g_seq.store(s + 2, std::memory_order_release);
    g_live.store(true, std::memory_order_release);
}

void disable() {
    g_live.store(false, std::memory_order_release);
}

bool read(float R[9], float q_xr[4]) {
    if (!g_live.load(std::memory_order_acquire)) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s1 = g_seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        float tmp[9], tq[4];
        for (int i = 0; i < 9; ++i) tmp[i] = g_R[i].load(std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) tq[i]  = g_Q[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_seq.load(std::memory_order_relaxed) != s1) continue;
        for (int i = 0; i < 9; ++i) R[i] = tmp[i];
        for (int i = 0; i < 4; ++i) q_xr[i] = tq[i];
        return true;
    }
    return false;
}

void publish_fov(float fovy_radians) {
    uint32_t bits;
    memcpy(&bits, &fovy_radians, sizeof(bits));
    g_fov_bits.store(bits, std::memory_order_relaxed);
}

float read_fov(float fallback) {
    const uint32_t bits = g_fov_bits.load(std::memory_order_relaxed);
    if (!bits) return fallback;
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

void publish_ipd(float ipd_meters) {
    uint32_t bits;
    memcpy(&bits, &ipd_meters, sizeof(bits));
    g_ipd_bits.store(bits, std::memory_order_relaxed);
}

float read_ipd(float fallback) {
    const uint32_t bits = g_ipd_bits.load(std::memory_order_relaxed);
    if (!bits) return fallback;
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

void set_ipd_scale(float s) {
    g_ipd_scale.store(s, std::memory_order_relaxed);
}

float ipd_scale() {
    return g_ipd_scale.load(std::memory_order_relaxed);
}

void set_mono_scope_fov(float radians) {
    g_mono_scope_fov.store(radians, std::memory_order_relaxed);
}

float mono_scope_fov() {
    return g_mono_scope_fov.load(std::memory_order_relaxed);
}

void set_fp_enabled(bool on) {
    g_fp_enabled.store(on, std::memory_order_relaxed);
}

bool fp_enabled() {
    return g_fp_enabled.load(std::memory_order_relaxed);
}

void set_fp_forward(float meters) {
    g_fp_forward.store(meters, std::memory_order_relaxed);
}

float fp_forward() {
    return g_fp_forward.load(std::memory_order_relaxed);
}

void set_fp_side(float meters) {
    g_fp_side.store(meters, std::memory_order_relaxed);
}

float fp_side() {
    return g_fp_side.load(std::memory_order_relaxed);
}

void set_fp_up(float meters) {
    g_fp_up.store(meters, std::memory_order_relaxed);
}

float fp_up() {
    return g_fp_up.load(std::memory_order_relaxed);
}

void set_fp_anchor_side(float meters) {
    g_fp_anchor_side.store(meters, std::memory_order_relaxed);
}

float fp_anchor_side() {
    return g_fp_anchor_side.load(std::memory_order_relaxed);
}

void set_fp_eye(float meters) {
    g_fp_eye.store(meters, std::memory_order_relaxed);
}

float fp_eye() {
    return g_fp_eye.load(std::memory_order_relaxed);
}

void set_fp_head_anchor(bool on) {
    g_fp_head_anchor.store(on, std::memory_order_relaxed);
}

bool fp_head_anchor() {
    return g_fp_head_anchor.load(std::memory_order_relaxed);
}

void set_fp_head_eye(float meters) {
    g_fp_head_eye.store(meters, std::memory_order_relaxed);
}

float fp_head_eye() {
    return g_fp_head_eye.load(std::memory_order_relaxed);
}

void set_head_node(unsigned int idx) {
    g_head_node.store(idx, std::memory_order_relaxed);
}

unsigned int head_node() {
    return g_head_node.load(std::memory_order_relaxed);
}

void set_player_obj(unsigned long long p) {
    g_player_obj.store(p, std::memory_order_relaxed);
}

unsigned long long player_obj() {
    return g_player_obj.load(std::memory_order_relaxed);
}

void set_fs_enabled(bool on) {
    g_fs_enabled.store(on, std::memory_order_relaxed);
}

bool fs_enabled() {
    return g_fs_enabled.load(std::memory_order_relaxed);
}

void set_fs_fov(float radians) {
    g_fs_fov.store(radians, std::memory_order_relaxed);
}

float fs_fov() {
    return g_fs_fov.load(std::memory_order_relaxed);
}

void push_eye_tag(int eye, const float q_xr[4]) {
    const uint64_t w = g_tag_w.load(std::memory_order_relaxed);
    if (w - g_tag_r.load(std::memory_order_acquire) >= kTagRing) return;
    Tag& t = g_tags[w & (kTagRing - 1)];
    t.eye = (uint8_t)eye;
    for (int i = 0; i < 4; ++i) t.q[i] = q_xr[i];
    g_tag_w.store(w + 1, std::memory_order_release);
}

std::atomic<uint64_t> g_pops_tagged{0}, g_pops_mono{0};

int pop_eye_tag(float q_xr[4], bool* q_ok) {
    const uint64_t r = g_tag_r.load(std::memory_order_relaxed);
    if (g_tag_w.load(std::memory_order_acquire) == r) {
        g_pops_mono.fetch_add(1, std::memory_order_relaxed);
        if (q_ok) *q_ok = false;
        return -1;
    }
    const Tag& t = g_tags[r & (kTagRing - 1)];
    const int e = t.eye;
    for (int i = 0; i < 4; ++i) q_xr[i] = t.q[i];
    if (q_ok) *q_ok = true;
    g_tag_r.store(r + 1, std::memory_order_release);
    g_pops_tagged.fetch_add(1, std::memory_order_relaxed);
    return e;
}

unsigned long long pops_tagged() { return g_pops_tagged.load(std::memory_order_relaxed); }
unsigned long long pops_mono()   { return g_pops_mono.load(std::memory_order_relaxed); }

// Build 19: absorbed aim-injection totals, geometric radians (HeadPose.h).
static std::atomic<float> g_aim_cum_yaw{0.0f};
static std::atomic<float> g_aim_cum_pitch{0.0f};

void set_aim_cum(float yaw_geo, float pitch_geo) {
    g_aim_cum_yaw.store(yaw_geo, std::memory_order_relaxed);
    g_aim_cum_pitch.store(pitch_geo, std::memory_order_relaxed);
}

// Build 22: the Touch-as-gamepad snapshot (see the header).
static std::atomic<uint32_t> g_pad_btn{0};
static std::atomic<bool>     g_pad_live{false};
static std::atomic<float>    g_pad_ax0{0.0f}, g_pad_ax1{0.0f}, g_pad_ax2{0.0f},
                             g_pad_ax3{0.0f}, g_pad_ax4{0.0f}, g_pad_ax5{0.0f};

void set_touch_pad(uint32_t buttons, const float axes[6], bool live) {
    g_pad_btn.store(buttons, std::memory_order_relaxed);
    if (axes) {
        g_pad_ax0.store(axes[0], std::memory_order_relaxed);
        g_pad_ax1.store(axes[1], std::memory_order_relaxed);
        g_pad_ax2.store(axes[2], std::memory_order_relaxed);
        g_pad_ax3.store(axes[3], std::memory_order_relaxed);
        g_pad_ax4.store(axes[4], std::memory_order_relaxed);
        g_pad_ax5.store(axes[5], std::memory_order_relaxed);
    }
    g_pad_live.store(live, std::memory_order_relaxed);
}

bool touch_pad(uint32_t* buttons, float axes[6]) {
    if (!g_pad_live.load(std::memory_order_relaxed)) return false;
    if (buttons) *buttons = g_pad_btn.load(std::memory_order_relaxed);
    if (axes) {
        axes[0] = g_pad_ax0.load(std::memory_order_relaxed);
        axes[1] = g_pad_ax1.load(std::memory_order_relaxed);
        axes[2] = g_pad_ax2.load(std::memory_order_relaxed);
        axes[3] = g_pad_ax3.load(std::memory_order_relaxed);
        axes[4] = g_pad_ax4.load(std::memory_order_relaxed);
        axes[5] = g_pad_ax5.load(std::memory_order_relaxed);
    }
    return true;
}

bool aim_cum(float* yaw_geo, float* pitch_geo) {
    const float y = g_aim_cum_yaw.load(std::memory_order_relaxed);
    const float p = g_aim_cum_pitch.load(std::memory_order_relaxed);
    *yaw_geo = y;
    *pitch_geo = p;
    return y != 0.0f || p != 0.0f;
}

int eye_tag_depth() {
    return (int)(g_tag_w.load(std::memory_order_relaxed) -
                 g_tag_r.load(std::memory_order_relaxed));
}

}  // namespace headpose
}  // namespace grwxr
