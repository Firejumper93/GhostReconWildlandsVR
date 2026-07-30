// CameraProbe.cpp - see CameraProbe.h. Read-only survey of the camera and
// projection path, to answer docs/QUESTIONS.md Q9.
//
// BUILD 3 (2026-07-28). What the previous two builds established:
//
//   Build 1 hooked only CalculatePerspectiveProjectionMatrix and saw 2 calls in
//   33 minutes, both at startup. Wrong function.
//
//   Build 2 hooked all six projection variants and found the gameplay path:
//   0x0C50C420 took 599,110 of 600,097 calls. It also found the flaw in this
//   file's table design. Rows were keyed on the exact bit pattern of every
//   float, and one of those floats is a field of view that varies continuously
//   with player speed, so every distinct FOV value consumed a row. The table
//   filled after 64 rows and dropped 599,853 calls.
//
// Build 3 fixes both. Rows are keyed on the call site (and, for camera-side
// probes, the camera pointer), while the floats are accumulated as min/max
// ranges instead of exact values. A continuously varying argument now widens a
// range rather than consuming a row.
//
// It also adds the two hooks that can actually answer Q9. The projection
// functions receive output matrices, not the camera, so they cannot tell us
// WHICH camera is being drawn. UpdateCameraMatricesAndFrustum and the
// projection-mode selector both take the camera in rcx, so they can.
//
// BUILD 5 (session 3). Q9 is answered: the player's camera is the one reaching
// on_calc_mvp with Camera+0x290 == 0. Before phase 5 may write anything, two
// facts must be confirmed at runtime (docs/HANDOFF.md):
//
//   1. The 21-argument mapping of on_calc_mvp in RE-notes.md is [INFERRED]
//      from a static read of one call site. This build captures the live
//      argument vector for the mode-0 camera and compares every argument
//      against the predicted camera-relative offset.
//
//   2. Which of the nine matrices at Camera+0x420..0x620 is which is
//      [UNKNOWN]. This build dumps all nine periodically so each slot can be
//      identified from its contents (view vs world vs projection vs inverse),
//      offline, against known player positions and camera states.
//
// Still strictly read-only. Nothing the engine sees is changed.
//
// BUILD 6 (session 3). Both build 5 questions answered: the mapping is exact
// and Camera+0x4A0 (arg 9) holds a pose matrix. First camera WRITE: a 15
// degree yaw composed onto +0x4A0 pre-call. Result: writes landed, no visual
// change; +0x4A0 is a derived output the function rewrites.
//
// BUILD 7 (session 3). Same write, retargeted at Camera+0x000, which the
// entry disassembly shows is the authoritative transform the view builder
// consumes (and the slot worldMatrixOverride copies into). Also dumps
// Camera+0x000 alongside the nine matrices. Result: write authority PROVEN,
// a rock-stable 15 degree view offset with the engine refreshing the slot
// every frame.
//
// BUILD 8 (session 4). The fixed yaw is replaced by the live OpenXR head
// rotation, published by VRMirror through HeadPose.h and composed onto
// Camera+0x000 frame-idempotently (see write_pose_head). With no headset the
// channel never goes live and the camera is left untouched.
//
// BUILD 9 (session 5). One addition on this side: the proj[2] hook publishes
// the fovy the engine renders with (its 5th argument) through HeadPose, so
// VRMirror can stamp the projection layer with the fov of the submitted image.

#include "CameraProbe.h"

#include "D3D11Hook.h"
#include "HeadPose.h"
#include "Log.h"
#include "Sig.h"
#include "ThunkHook.h"

#include <atomic>
#include <cstdint>
#include <cstring>

// Provided by ProbeStub.asm. Keep SavedArgs in sync with the layout there.
extern "C" {
void* grwxr_probe_originals[8] = {};
void  grwxr_probe_entry_0();
void  grwxr_probe_entry_1();
void  grwxr_probe_entry_2();
void  grwxr_probe_entry_3();
void  grwxr_probe_entry_4();
void  grwxr_probe_entry_5();
void  grwxr_probe_entry_6();
void  grwxr_probe_entry_7();
}

namespace grwxr {
namespace camera {
namespace {

// docs/RE-notes.md: the Odyssey calc_projection signature matches Wildlands byte
// for byte, one match in the 369 MB image. This is the ANCHOR: the one address
// derived from a signature rather than from the documented thunk table, and
// what proves the loaded image is the binary we analysed.
constexpr const char* kProjSig =
    "48 89 E0 53 48 81 EC 90 00 00 00 0F 29 70 E8 48 89 CB F3";

struct Target {
    uintptr_t   thunk_rva;
    uintptr_t   expected_fn_rva;
    bool        rcx_is_camera;   // key rows on rcx, and read camera fields
    const char* name;
};

// docs/RE-notes.md. Read from this exact binary, pinned by SHA256, with
// tools/find_callers.py. Entry 0 must resolve to the signature-scanned address
// or nothing is installed at all; that check validates the whole table.
constexpr Target kTargets[] = {
    {0x01347280, 0x0C50C0E0, false, "proj[0] 0x0C50C0E0 (anchor)"},
    {0x01347460, 0x0C50C2E0, false, "proj[1] 0x0C50C2E0"},
    {0x01347530, 0x0C50C420, false, "proj[2] 0x0C50C420 (gameplay)"},
    {0x01347840, 0x0C50C7E0, false, "proj[3] 0x0C50C7E0 (skew path)"},
    {0x01345720, 0x0C5094D0, false, "proj[4] 0x0C5094D0"},
    {0x01345800, 0x0C509720, false, "proj[5] 0x0C509720"},
    {0x0135F720, 0x0C5E47E0, true,  "on_calc_mvp 0x0C5E47E0"},
    {0x01349DF0, 0x0C510B20, true,  "selector 0x0C510B20"},
};
constexpr int kNumTargets = (int)(sizeof(kTargets) / sizeof(kTargets[0]));

hook::ThunkHook g_hooks[kNumTargets];
uintptr_t       g_module = 0;
bool            g_any    = false;

// Mirrors the block ProbeStub.asm writes at rsp+40h.
struct SavedArgs {
    uint64_t rcx, rdx, r8, r9;
    float    xmm0[4], xmm1[4], xmm2[4], xmm3[4];
};

// Camera struct offsets, docs/RE-notes.md. All are read-only.
constexpr size_t kOffMode = 0x290;   // int, camera mode/type enum
constexpr size_t kOffWmo  = 0x2A0;   // Matrix4x4*, worldMatrixOverride
constexpr size_t kOffFov  = 0x2BC;   // float, base field of view
constexpr size_t kOffSkX  = 0x2C4;   // float, projection skew x
constexpr size_t kOffSkY  = 0x2C8;   // float, projection skew y

struct CamFields {
    uint32_t mode = 0;
    uint64_t wmo  = 0;
    float    fov = 0, skx = 0, sky = 0;
    bool     valid = false;
};

// Separate function so the SEH block contains no C++ object unwinding. If the
// pointer is not what we think it is, we get a false rather than a crash.
CamFields read_camera(uint64_t p) {
    CamFields c;
    if (!p) return c;
    __try {
        const auto* b = (const uint8_t*)p;
        memcpy(&c.mode, b + kOffMode, sizeof(c.mode));
        memcpy(&c.wmo,  b + kOffWmo,  sizeof(c.wmo));
        memcpy(&c.fov,  b + kOffFov,  sizeof(c.fov));
        memcpy(&c.skx,  b + kOffSkX,  sizeof(c.skx));
        memcpy(&c.sky,  b + kOffSkY,  sizeof(c.sky));
        c.valid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        c.valid = false;
    }
    return c;
}

// --- the recording table ----------------------------------------------------
//
// One row per (probe, call site, camera). Floats are accumulated as ranges.
// Fixed size, no allocation, no locks (project rule 8). The min/max updates
// are deliberately not atomic: a torn range in a diagnostic is harmless, and a
// lock in a function called six times per frame is not acceptable.

// 256, not 64. The user's camera inventory (docs/CAMERA-INVENTORY.md) lists far
// more distinct player camera states than the original table could hold: five
// vehicle families with passenger variants, a two-seat helicopter with separate
// pilot and gunner views, skydiving, shoulder swap, multi-zoom and hybrid
// scopes, three drone modes, cutscenes and four menu screens. Each can present
// a different camera object, and a row is only ~120 bytes.
constexpr int kMaxRows = 256;
constexpr int kNumFloats = 4;   // xmm1, xmm2, xmm3, first stack slot

struct Row {
    std::atomic<uint32_t> used{0};
    uint32_t              probe  = 0;
    uintptr_t             caller = 0;
    uint64_t              ctx    = 0;   // camera pointer, or 0
    float                 lo[kNumFloats]{}, hi[kNumFloats]{};
    CamFields             cam{};
    std::atomic<uint64_t> count{0};
};

Row                   g_rows[kMaxRows];
std::atomic<uint64_t> g_calls[kNumTargets];
std::atomic<uint64_t> g_overflow{0};
std::atomic<uint32_t> g_rows_used{0};

// --- build 5: on_calc_mvp argument snapshot and camera matrix dump ----------

constexpr uint64_t kOnCalcMvpProbe = 6;   // index into kTargets
constexpr int      kNumArgs        = 21;  // 4 register + 17 stack

// Build 9: the gameplay projection function, whose 5th argument is the
// vertical field of view in radians (docs/RE-notes.md: 0.78 to 0.83 in play,
// 0.17 scoped; the two call sites swap near/far but agree on the angle).
constexpr uint64_t kProjGameplayProbe = 2;

// The statically derived argument mapping, docs/RE-notes.md, [INFERRED] from
// the single call site 0x0C5E4EBA. Offsets are camera-relative; -1 marks an
// argument documented as a float value rather than a pointer into the camera.
constexpr int64_t kExpectedOff[kNumArgs] = {
    0x000, 0x290, 0x314, 0x334, 0x324, 0x000, 0x360, 0x420, 0x4A0, 0x4E0,
    0x520,    -1, 0x79C,    -1,    -1,    -1, 0x5A0, 0x560, 0x460, 0x5E0,
    0x620,
};

struct ArgSnap {
    uint64_t cam = 0;
    uint64_t arg[kNumArgs] = {};   // arg 1..21 in call order
    float    xmm[4] = {};          // low float of xmm0..xmm3 at entry
};

// Single-writer handshake: drain() arms it (0 -> 1), the recorder claims it
// (1 -> 3), fills the buffer, publishes (3 -> 2), drain() prints and resets
// (2 -> 0). The recorder never blocks and never writes unless armed.
std::atomic<uint32_t> g_snap_state{0};
ArgSnap               g_snap;
std::atomic<uint64_t> g_player_cam{0};

// The nine 4x4 matrices, docs/RE-notes.md, contiguous at Camera+0x420,
// 0x40 apart, last one at +0x620.
constexpr size_t kMatBase = 0x420;
constexpr int    kNumMats = 9;

// SEH-guarded block read, same rationale as read_camera above.
bool read_block(uint64_t src, void* dst, size_t n) {
    __try {
        memcpy(dst, (const void*)src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- build 8: PHASE 5 STEP 2, head rotation drives the camera ---------------
//
// Builds 6 and 7 proved where the write must land: Camera+0x000 is the
// authoritative transform the view builder consumes (Camera+0x4A0 is a
// derived output), and composing a rotation there pre-call steers the whole
// render pipeline with the engine refreshing the slot every frame.
//
// Build 7's compose was per call (rotate whatever is in the slot), which only
// worked because the engine refreshes the slot. Whether it refreshes before
// EACH of the two per-frame calls is still [UNKNOWN] (RE-notes.md), so build 8
// does not inherit that pattern. Instead it is frame-idempotent:
//
//   on the first on_calc_mvp call of each rendered frame, capture the
//   engine's own rotation rows (the base) from Camera+0x000;
//   on EVERY call, write the absolute product H * base back.
//
// The second per-frame call then rewrites the same value whether or not the
// engine refreshed the slot in between: fresh slot, same base is applied
// again; stale slot, the identical product is simply written twice. Nothing
// accumulates either way.
//
// H comes from VRMirror through the HeadPose seqlock, already in the game
// basis, relative to a yaw-only reference. Row-vector convention throughout:
// rows are basis vectors, so rows' = H * base applies the head rotation in
// the camera's own frame and then the engine's base takes it to world. Row 3
// (position) and column 3 are left untouched.

constexpr size_t kOffPose = 0x000;

// Engine-thread state. on_calc_mvp reaches this ~2 times per rendered frame
// for the single mode-0 camera on the frame-build thread; these are not
// shared with any other thread.
uint64_t g_last_frame  = ~0ull;
float    g_base[9]     = {};
float    g_base_pos[3] = {};
int      g_eye_toggle  = 0;   // flips once per built frame; 0 left, 1 right

// BUILD 10b.2 diagnostics: is the engine refreshing the POSITION row of
// Camera+0x000 every frame, the way build 7 verified for rotation? If it is
// NOT, each frame's base capture reads back our previous eye-offset write and
// the offsets compound. Discriminator: the distance between this frame's
// captured base position and the position we WROTE last frame.
//   vs-write ~ 0.0000  -> slot still holds our write: NOT refreshed, bug.
//   vs-write ~ 0.03    -> engine rebuilt it: refreshed, offsets are clean.
// Plain floats, engine thread writes, drain reads racily (diagnostic only).
float g_diag_written[3]   = {};
bool  g_diag_have_written = false;
float g_diag_step_last = 0, g_diag_step_max = 0;    // capture vs prev capture
float g_diag_vsw_last  = 0, g_diag_vsw_max  = 0;    // capture vs last write
float g_diag_prev_cap[3] = {};
bool  g_diag_have_prev   = false;

float dist3(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

std::atomic<uint64_t> g_head_writes{0};
std::atomic<uint64_t> g_head_write_fails{0};
std::atomic<uint64_t> g_head_frames{0};   // frames on which base was captured

// BUILD 10b.1: the eye identity of every built frame travels WITH the frame.
// Build 10b derived it from frame parity at present, assuming the frame built
// between two Presents is the next one presented; the engine pipelines builds
// ahead of Presents by an unknown depth, so that assumption swaps the eyes
// whenever the phase is odd (user report: stereo "totally unusable"). Now the
// camera hook alternates its own eye toggle per built frame, offsets the
// camera toward that eye, and pushes the tag into the HeadPose FIFO; VRMirror
// pops one tag per present. Eye 0 is the left eye (XR view order), offset
// -IPD/2 along the camera's right axis; eye 1 is +IPD/2.
bool write_pose_head(uint64_t cam, const float* H) {
    float* m = (float*)(cam + kOffPose);
    // frame_count() is bumped in the Present hook, so it is stable across the
    // frame build; the calls/frame ratio in the drain log verifies that.
    const uint64_t f = d3d11::frame_count();
    __try {
        if (f != g_last_frame) {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    g_base[r * 3 + c] = m[r * 4 + c];
            for (int c = 0; c < 3; ++c)
                g_base_pos[c] = m[12 + c];
            if (g_diag_have_prev) {
                g_diag_step_last = dist3(g_base_pos, g_diag_prev_cap);
                if (g_diag_step_last > g_diag_step_max) g_diag_step_max = g_diag_step_last;
            }
            if (g_diag_have_written) {
                g_diag_vsw_last = dist3(g_base_pos, g_diag_written);
                if (g_diag_vsw_last > g_diag_vsw_max) g_diag_vsw_max = g_diag_vsw_last;
            }
            for (int c = 0; c < 3; ++c) g_diag_prev_cap[c] = g_base_pos[c];
            g_diag_have_prev = true;
            g_last_frame = f;
            g_eye_toggle ^= 1;
            headpose::push_eye_tag(g_eye_toggle);
            g_head_frames.fetch_add(1, std::memory_order_relaxed);
        }

        // Build 11d (combined with 11c per user, "one off"): FLAT SCOPE.
        // Below the mono_scope_fov threshold the camera write is SKIPPED
        // ENTIRELY (rotation AND position), superseding 11b's zero-offset
        // gate. Reason (session 11, 6x screenshot): the game anchors the
        // scope mask and reticle to ITS aim direction while the scene
        // follows OUR head-composed camera, so any head-vs-aim angle
        // displaces overlay from image, magnified by the zoom (doubled mask
        // circles, "wildly moves", recenter-proof). With no write, the game
        // renders its own scope view exactly like the flat game: mask
        // centered, reticle true, ballistics match the crosshair. Head
        // compose resumes the instant the fov rises. The per-frame block
        // above still ran, so the eye-tag stream stays intact. read_fov is
        // at most one frame stale, costing one frame at the transition.
        if (headpose::read_fov(0.7853982f) < headpose::mono_scope_fov())
            return true;

        float out[9];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out[r * 3 + c] = H[r * 3 + 0] * g_base[0 + c]
                               + H[r * 3 + 1] * g_base[3 + c]
                               + H[r * 3 + 2] * g_base[6 + c];
            }
        }
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                m[r * 4 + c] = out[r * 3 + c];

        // The eye offset. Row 0 of the composed rotation is the camera's
        // right basis vector in world space (row-vector convention, unit
        // length); 1 world unit = 1 m, so the measured IPD applies directly.
        // Frame-idempotent like the rotation: base position captured once per
        // frame, absolute value written on every call.
        // Build 10c: ipd_scale tunes perceived separation against the game's
        // actual world scale (grwxr.cfg, Numpad +/- live).
        // Build 10m: mapping inverted. Once 10L made the eyes fuse, the
        // user's depth verdict landed at ipd_scale -0.50 (run of 2026-07-29
        // 20:44), so the 10f "conventional" sign was backwards for the actual
        // eye routing. The sign is folded in here: positive scale is now the
        // correct-depth direction. (10e's eye-swap null predates fusion and
        // said nothing about this.)
        const float half = 0.5f * headpose::read_ipd(0.063f) * headpose::ipd_scale();
        const float s    = g_eye_toggle ? -half : +half;
        // Build 11c: first-person DEMO. Push the viewpoint forward along the
        // BASE (game camera) forward row, so the offset lands at the
        // character wherever the head looks. Base position and rotation are
        // captured once per frame, so this stays frame-idempotent like
        // everything else here. The base forward is unit length (rotation
        // row) and fp_forward is meters (1 world unit = 1 m).
        float pos[3] = {g_base_pos[0], g_base_pos[1], g_base_pos[2]};
        if (headpose::fp_enabled()) {
            // Build 11f: full placement in the base camera's axes. Row 0 is
            // right, row 1 forward, row 2 up; the side default is negative
            // because the third-person camera hangs off the right shoulder.
            const float df = headpose::fp_forward();
            const float ds = headpose::fp_side();
            const float du = headpose::fp_up();
            for (int c = 0; c < 3; ++c)
                pos[c] += df * g_base[3 + c] + ds * g_base[0 + c] + du * g_base[6 + c];
        }
        for (int c = 0; c < 3; ++c) {
            m[12 + c] = pos[c] + s * out[c];
            g_diag_written[c] = m[12 + c];
        }
        g_diag_have_written = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// THE RECORDER. Called from the assembly stub on an engine thread, several
// times per frame. Must not log, allocate, lock, or call anything that might.
// `stack_args` points at the caller's shadow space, so argument 5 of the hooked
// function is stack_args[4].
extern "C" void grwxr_probe_record(uint64_t index, const void* saved_raw,
                                   void* return_address, uint64_t* stack_args) {
    if (index >= (uint64_t)kNumTargets) return;
    const auto* a = (const SavedArgs*)saved_raw;

    g_calls[index].fetch_add(1, std::memory_order_relaxed);

    // Build 9: publish the rendered fovy to the VR side, so the projection
    // layer VRMirror submits carries the fov the image was actually rendered
    // with. An out-of-range value (a torn stack read, or a call this table has
    // not seen before) is dropped rather than published.
    if (index == kProjGameplayProbe) {
        float fovy;
        memcpy(&fovy, &stack_args[4], sizeof(fovy));
        if (fovy > 0.01f && fovy < 3.1f) {
            // Build 12a: FULLSCREEN. Argument 5 is passed on the caller's
            // stack, the stub tail-jumps to the real function with that
            // stack intact, and this recorder runs first, so overwriting
            // the slot here changes the fov the engine actually renders
            // with. Only the world band is overridden: plain ADS
            // (0.49..0.52) keeps its zoom and magnified optics keep the
            // flat-scope path. The blit needs no change; it already places
            // content by its published fov, which is the overridden value.
            // Build 12a.1: the band gets an UPPER limit. proj[2] is also
            // called at exactly pi/2 (1.5708), the fov of cubemap faces
            // (sky/reflection captures); 12a widened those too and the
            // misregistered sky layer slid against the world ("the clouds
            // followed the headset"). 1.35 keeps everything real (world
            // 0.78..0.87, sprint/vehicle ~1.22) and releases the captures.
            const float fs = headpose::fs_fov();
            if (headpose::fs_enabled() && fs > 0.0f &&
                fovy >= 0.60f && fovy <= 1.35f) {
                fovy = fs;
                memcpy(&stack_args[4], &fovy, sizeof(fovy));
            }
            headpose::publish_fov(fovy);
        }
    }

    // Build 5: when armed, capture one full argument vector of on_calc_mvp for
    // the player's camera (mode 0). The CAS makes the recorder the single
    // writer; drain() prints the buffer on the init thread. No logging here.
    if (index == kOnCalcMvpProbe) {
        CamFields c = read_camera(a->rcx);
        if (c.valid && c.mode == 0) {
            g_player_cam.store(a->rcx, std::memory_order_relaxed);

            // Build 8: the one behavioural change of this build. See the
            // comment block at write_pose_head. A failed read (nothing
            // published, VR inactive, or a torn seqlock) leaves the camera
            // entirely alone for this call.
            float H[9];
            if (headpose::read(H)) {
                if (write_pose_head(a->rcx, H))
                    g_head_writes.fetch_add(1, std::memory_order_relaxed);
                else
                    g_head_write_fails.fetch_add(1, std::memory_order_relaxed);
            }
            uint32_t armed = 1;
            if (g_snap_state.compare_exchange_strong(armed, 3,
                                                     std::memory_order_acq_rel)) {
                g_snap.cam    = a->rcx;
                g_snap.arg[0] = a->rcx;
                g_snap.arg[1] = a->rdx;
                g_snap.arg[2] = a->r8;
                g_snap.arg[3] = a->r9;
                // Arg N (N >= 5) lives at stack_args[N-1]; stack_args[0..3] are
                // the shadow-space homes of the register arguments.
                for (int i = 4; i < kNumArgs; ++i) g_snap.arg[i] = stack_args[i];
                g_snap.xmm[0] = a->xmm0[0];
                g_snap.xmm[1] = a->xmm1[0];
                g_snap.xmm[2] = a->xmm2[0];
                g_snap.xmm[3] = a->xmm3[0];
                g_snap_state.store(2, std::memory_order_release);
            }
        }
    }

    const uintptr_t caller = (uintptr_t)return_address - g_module;
    const uint64_t  ctx    = kTargets[index].rcx_is_camera ? a->rcx : 0;

    float f[kNumFloats];
    f[0] = a->xmm1[0];
    f[1] = a->xmm2[0];
    f[2] = a->xmm3[0];
    memcpy(&f[3], &stack_args[4], sizeof(float));

    const uint32_t used = g_rows_used.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < used; ++i) {
        Row& r = g_rows[i];
        if (r.probe != index || r.caller != caller || r.ctx != ctx) continue;
        for (int k = 0; k < kNumFloats; ++k) {
            if (f[k] < r.lo[k]) r.lo[k] = f[k];
            if (f[k] > r.hi[k]) r.hi[k] = f[k];
        }
        // Refresh the camera snapshot so the log shows current values, not the
        // ones from the first frame this camera was ever seen.
        if (kTargets[index].rcx_is_camera) r.cam = read_camera(ctx);
        r.count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint32_t slot = g_rows_used.load(std::memory_order_relaxed);
    if (slot >= kMaxRows) {
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t expected = 0;
    if (g_rows[slot].used.compare_exchange_strong(expected, 1,
                                                  std::memory_order_acq_rel)) {
        Row& r = g_rows[slot];
        r.probe  = (uint32_t)index;
        r.caller = caller;
        r.ctx    = ctx;
        for (int k = 0; k < kNumFloats; ++k) { r.lo[k] = f[k]; r.hi[k] = f[k]; }
        if (kTargets[index].rcx_is_camera) r.cam = read_camera(ctx);
        r.count.store(1, std::memory_order_relaxed);
        // Publish only after the fields are written.
        g_rows_used.store(slot + 1, std::memory_order_release);
    }
}

bool install() {
    auto img = sig::main_image();
    if (!img) {
        LOG_ERROR("camera: cannot read the GRW.exe image headers. Not installing.");
        return false;
    }
    g_module = (uintptr_t)img->base;
    LOG_INFO("camera: image base 0x%p, size 0x%zX", (void*)img->base, img->size);

    size_t matches = 0;
    auto found = sig::find_unique(*img, kProjSig, &matches);
    if (!found) {
        LOG_ERROR("camera: anchor signature matched %zu times, expected exactly 1.",
                  matches);
        LOG_ERROR("camera: probe NOT installed. The game runs unmodified.");
        if (matches == 0) {
            LOG_ERROR("camera: 0 matches usually means the game was updated. "
                      "Re-run tools/sig_scan.py against the new GRW.exe.");
        }
        return false;
    }
    const uintptr_t anchor_rva = (uintptr_t)*found - g_module;
    LOG_INFO("camera: anchor found at RVA 0x%08zX, %zu match", (size_t)anchor_rva, matches);
    if (anchor_rva != kTargets[0].expected_fn_rva) {
        LOG_ERROR("camera: anchor is at RVA 0x%08zX but the thunk table was built "
                  "for 0x%08zX. This is NOT the binary we analysed.",
                  (size_t)anchor_rva, (size_t)kTargets[0].expected_fn_rva);
        return false;
    }

    void* entries[8] = {
        (void*)&grwxr_probe_entry_0, (void*)&grwxr_probe_entry_1,
        (void*)&grwxr_probe_entry_2, (void*)&grwxr_probe_entry_3,
        (void*)&grwxr_probe_entry_4, (void*)&grwxr_probe_entry_5,
        (void*)&grwxr_probe_entry_6, (void*)&grwxr_probe_entry_7,
    };

    int ok = 0;
    for (int i = 0; i < kNumTargets; ++i) {
        uint8_t* fn = img->base + kTargets[i].expected_fn_rva;
        grwxr_probe_originals[i] = fn;
        if (g_hooks[i].install(img->base + kTargets[i].thunk_rva, fn, entries[i],
                               kTargets[i].name)) {
            ++ok;
        }
    }

    g_any = ok > 0;
    LOG_INFO("camera: %d of %d targets hooked.", ok, kNumTargets);
    if (!g_any) {
        LOG_ERROR("camera: nothing installed. The game runs unmodified.");
        return false;
    }
    LOG_INFO("camera: survey armed. BUILD 8: the live OpenXR head rotation is");
    LOG_INFO("camera: composed frame-idempotently onto the root transform at");
    LOG_INFO("camera: Camera+0x000 before on_calc_mvp. With no headset the");
    LOG_INFO("camera: channel stays idle and the camera is untouched.");
    return true;
}

namespace {

// Build 5. Prints one captured argument vector with the camera-relative offset
// of every argument next to the statically predicted one, so the [INFERRED]
// mapping is either confirmed or corrected by a single glance at the log.
void print_snapshot() {
    const ArgSnap& s = g_snap;
    LOG_INFO("");
    LOG_INFO("=== on_calc_mvp argument snapshot, cam=0x%016llX (mode 0) ===",
             (unsigned long long)s.cam);
    LOG_INFO("  xmm0..3 at entry: %g  %g  %g  %g",
             s.xmm[0], s.xmm[1], s.xmm[2], s.xmm[3]);
    LOG_INFO("  arg  raw                 vs camera    predicted    verdict  as float");
    for (int i = 0; i < kNumArgs; ++i) {
        const uint64_t v = s.arg[i];

        char rel[24];
        if (s.cam && v >= s.cam && v < s.cam + 0x2000)
            snprintf(rel, sizeof(rel), "cam+0x%03llX",
                     (unsigned long long)(v - s.cam));
        else
            snprintf(rel, sizeof(rel), "-");

        char exp[24];
        const char* verdict;
        if (kExpectedOff[i] >= 0) {
            snprintf(exp, sizeof(exp), "cam+0x%03llX",
                     (unsigned long long)kExpectedOff[i]);
            verdict = (v == s.cam + (uint64_t)kExpectedOff[i]) ? "MATCH" : "DIFFERS";
        } else {
            snprintf(exp, sizeof(exp), "float/calc");
            verdict = "-";
        }

        float f;
        const uint32_t lo = (uint32_t)v;
        memcpy(&f, &lo, sizeof(f));
        LOG_INFO("  %3d  0x%016llX  %-11s  %-11s  %-7s  %g",
                 i + 1, (unsigned long long)v, rel, exp, verdict, f);
    }
    LOG_INFO("=== end snapshot ===");
}

// Build 5. Dumps the nine matrices so each slot can be identified offline:
// the projection by its characteristic zeros, the view and the camera pose by
// orthonormal rows and the player's world position, the inverses by pairing.
void dump_matrices() {
    const uint64_t cam = g_player_cam.load(std::memory_order_relaxed);
    if (!cam) return;
    float m[kNumMats][16];
    if (!read_block(cam + kMatBase, m, sizeof(m))) {
        LOG_WARN("matrices at cam+0x%03zX unreadable this tick", kMatBase);
        return;
    }
    LOG_INFO("");
    LOG_INFO("=== nine camera matrices, cam=0x%016llX ===", (unsigned long long)cam);
    // Build 7: also show the root transform at Camera+0x000, the matrix the
    // view builder actually consumes.
    float root[16];
    if (read_block(cam, root, sizeof(root))) {
        LOG_INFO("  [cam+0x000] (root transform)");
        for (int r = 0; r < 4; ++r) {
            LOG_INFO("    %14.6g %14.6g %14.6g %14.6g",
                     root[r * 4 + 0], root[r * 4 + 1],
                     root[r * 4 + 2], root[r * 4 + 3]);
        }
    }
    for (int i = 0; i < kNumMats; ++i) {
        LOG_INFO("  [cam+0x%03zX]", kMatBase + (size_t)i * 0x40);
        for (int r = 0; r < 4; ++r) {
            LOG_INFO("    %14.6g %14.6g %14.6g %14.6g",
                     m[i][r * 4 + 0], m[i][r * 4 + 1],
                     m[i][r * 4 + 2], m[i][r * 4 + 3]);
        }
    }
    LOG_INFO("=== end matrices ===");
}

// Build 5 cadence, one call per second from the init thread. Prints a pending
// snapshot as soon as the recorder fills one, and every 20 seconds re-arms the
// snapshot and dumps the matrices. Independent of the survey throttle below so
// neither starves the other.
void snap_drain() {
    static int ticks = 0;
    ++ticks;

    // Build 11a diagnostic: the live published fov, once per second. This is
    // the number the scaling blit places the content with, and the dataset
    // for choosing the mono-scope threshold: have the user scope each optic
    // class and read the values off these lines.
    {
        const float f = headpose::read_fov(0.0f);
        if (f > 0.0f) LOG_INFO("fov: %.4f rad (%.1f deg)", f, f * 57.29578f);
    }

    if (g_snap_state.load(std::memory_order_acquire) == 2) {
        print_snapshot();
        g_snap_state.store(0, std::memory_order_release);
    }

    if ((ticks % 20) == 5) {
        uint32_t idle = 0;
        g_snap_state.compare_exchange_strong(idle, 1, std::memory_order_acq_rel);
        dump_matrices();
    }

    // Build 8: liveness of the head compose, once per 10 s. A nonzero fail
    // count means the SEH guard fired. The calls/frame ratio verifies the
    // frame-idempotence assumption: ~2.0 means frame_count() is stable across
    // both per-frame calls, as designed.
    if ((ticks % 10) == 0) {
        const unsigned long long w  = g_head_writes.load(std::memory_order_relaxed);
        const unsigned long long fr = g_head_frames.load(std::memory_order_relaxed);
        const unsigned long long fl = g_head_write_fails.load(std::memory_order_relaxed);
        if (fr) {
            LOG_INFO("head compose: %llu writes over %llu frames (%.2f calls/frame), %llu failed",
                     w, fr, (double)w / (double)fr, fl);
            // Build 10b.1: the ring occupancy IS the build-to-present pipeline
            // depth, the number parity-based eye matching guessed wrong.
            LOG_INFO("aer: pipeline depth %d, pops tagged=%llu mono=%llu",
                     headpose::eye_tag_depth(),
                     headpose::pops_tagged(), headpose::pops_mono());
            // Build 10b.2: vs-write ~0 means the engine is NOT refreshing the
            // position row and our eye offsets are compounding (see the
            // comment at g_diag_written). step is capture-to-capture motion.
            LOG_INFO("aer: basepos step last=%.4f max=%.4f, vs-write last=%.4f max=%.4f",
                     g_diag_step_last, g_diag_step_max,
                     g_diag_vsw_last, g_diag_vsw_max);
        } else {
            LOG_INFO("head compose: idle (no head pose published; camera untouched)");
        }
    }
}

}  // namespace

void drain() {
    if (!g_any) return;

    snap_drain();

    static uint32_t last_rows = 0;
    static int      ticks     = 0;

    const uint32_t rows = g_rows_used.load(std::memory_order_acquire);
    const bool changed = rows != last_rows;
    if (!changed && (++ticks % 30) != 0) return;
    last_rows = rows;

    uint64_t total = 0;
    for (int i = 0; i < kNumTargets; ++i)
        total += g_calls[i].load(std::memory_order_relaxed);

    LOG_INFO("");
    LOG_INFO("=== Q9 survey: %llu calls, %u rows ===", (unsigned long long)total, rows);
    for (int i = 0; i < kNumTargets; ++i) {
        LOG_INFO("  %-34s %12llu calls", kTargets[i].name,
                 (unsigned long long)g_calls[i].load(std::memory_order_relaxed));
    }
    if (rows) {
        LOG_INFO("  probe caller     calls      camera             mode  wmo    "
                 "camFOV   skewX    skewY    | xmm1 range        xmm2 range        "
                 "xmm3 range        arg5 range");
        for (uint32_t i = 0; i < rows; ++i) {
            Row& r = g_rows[i];
            char camtxt[96];
            if (r.ctx == 0) {
                snprintf(camtxt, sizeof(camtxt), "%-18s %4s %6s %8s %8s %8s",
                         "-", "-", "-", "-", "-", "-");
            } else if (!r.cam.valid) {
                snprintf(camtxt, sizeof(camtxt), "0x%016llX UNREADABLE            ",
                         (unsigned long long)r.ctx);
            } else {
                snprintf(camtxt, sizeof(camtxt),
                         "0x%016llX %4u %6s %8.5f %8.5f %8.5f",
                         (unsigned long long)r.ctx, r.cam.mode,
                         r.cam.wmo ? "SET" : "null",
                         r.cam.fov, r.cam.skx, r.cam.sky);
            }
            LOG_INFO("  %5u 0x%08zX %10llu %s | "
                     "%9.4f..%-9.4f %9.4f..%-9.4f %9.5f..%-9.5f %9.5f..%-9.5f",
                     r.probe, (size_t)r.caller,
                     (unsigned long long)r.count.load(std::memory_order_relaxed),
                     camtxt,
                     r.lo[0], r.hi[0], r.lo[1], r.hi[1],
                     r.lo[2], r.hi[2], r.lo[3], r.hi[3]);
        }
    }
    const uint64_t of = g_overflow.load(std::memory_order_relaxed);
    if (of) {
        LOG_WARN("  TABLE FULL: %llu calls did not fit in %d rows. INCOMPLETE.",
                 (unsigned long long)of, kMaxRows);
    }
    LOG_INFO("=== end ===");
}

void uninstall() {
    for (auto& h : g_hooks) h.restore();
    g_any = false;
}

}  // namespace camera
}  // namespace grwxr
