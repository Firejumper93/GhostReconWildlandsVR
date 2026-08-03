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
void* grwxr_probe_originals[11] = {};
void  grwxr_probe_entry_0();
void  grwxr_probe_entry_1();
void  grwxr_probe_entry_2();
void  grwxr_probe_entry_3();
void  grwxr_probe_entry_4();
void  grwxr_probe_entry_5();
void  grwxr_probe_entry_6();
void  grwxr_probe_entry_7();
void  grwxr_probe_entry_8();
void  grwxr_probe_entry_9();
void  grwxr_probe_entry_10();

// Build 17, the aim-architecture experiment. Written and read by the asm entry
// grwxr_setyaw_entry (ProbeStub.asm); the drain thread reads the counters and
// the key thread sets `pending` through camera::arm_yaw_bump(). Aligned plain
// loads and stores are atomic on x64; `pending` is consumed with xchg in the
// stub so a queued bump fires exactly once.
uint32_t grwxr_setyaw_pending = 0;
uint32_t grwxr_setyaw_bump    = 0;   // float bits, set before install
uint32_t grwxr_setyaw_disp    = 0;   // vtable byte offset, from the slot bytes
volatile uint64_t grwxr_setyaw_count   = 0;
volatile uint64_t grwxr_setyaw_lastobj = 0;
volatile uint32_t grwxr_setyaw_lastval = 0;
volatile uint32_t grwxr_setyaw_shipped = 0;
volatile uint32_t grwxr_setyaw_fired   = 0;
void  grwxr_setyaw_entry();

// Build 19: the pitch half, same consume-once mechanism.
uint32_t grwxr_setpitch_pending = 0;
uint32_t grwxr_setpitch_bump    = 0;
uint32_t grwxr_setpitch_disp    = 0;
volatile uint64_t grwxr_setpitch_count   = 0;
volatile uint64_t grwxr_setpitch_lastobj = 0;
volatile uint32_t grwxr_setpitch_lastval = 0;
void  grwxr_setpitch_entry();

// Build 18, head hide. Written here and in VRMirror's key poll, read by the
// asm entry grwxr_headhide_entry every engine SetHidden call.
uint64_t grwxr_headhide_table = 0;   // 0 until verified at install
uint64_t grwxr_headhide_impl  = 0;
uint32_t grwxr_headhide_on    = 0;
volatile uint64_t grwxr_headhide_calls  = 0;
volatile uint64_t grwxr_headhide_forced = 0;
volatile uint64_t grwxr_headhide_obj    = 0;
void  grwxr_headhide_entry();
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
    // Build 14a, skeleton/HIK runtime probe. Both thunks were located OFFLINE
    // against the pinned binary (docs/RE-notes.md "Task function pointers
    // extracted" and "The HIK datablock reader") and both are the same
    // 5-byte-jmp-in-16-byte-slot shape as the camera thunks. Read-only: the
    // recorder captures registers and counts, nothing engine-visible changes.
    {0x01865A10, 0x0DA1A990, false, "SkeletonPostUpdate 0x0DA1A990"},
    {0x018BE500, 0x0DC4F9B0, false, "HIK datablock reader 0x0DC4F9B0"},
    // Build 15L (session 18): THE PLAYER PREDICATE. Offline decompilation
    // found cPlayerComponent::OnInit (RVA 0x11412C60, named by its own log
    // format strings) and, inside it, this thunk called with rcx = the
    // component. [component+0x10] is the OWNING ENTITY, i.e. the local
    // player's entity: the exact identity every probe since 15e failed to
    // find by scanning. Read-only capture; the predicate is then
    // [skeleton+0x10] == that entity.
    {0x02713160, 0x114A6DE0, false, "cPlayerComponent::OnInit callee"},
};
constexpr int kNumTargets = (int)(sizeof(kTargets) / sizeof(kTargets[0]));

hook::ThunkHook g_hooks[kNumTargets];
uintptr_t       g_module      = 0;
size_t          g_module_size = 0;   // build 14b: for in-image vtable checks
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

// --- build 14a: skeleton/HIK runtime probe ----------------------------------
//
// Motion-control and first-person groundwork. Two questions only a live run
// can answer: does SkeletonPostUpdate actually fire per frame with a usable
// object in rcx, and does the HIK datablock reader run at character load with
// the rig instance in rcx (the offline caller disasm says rcx = owning object,
// rdx = data source, docs/RE-notes.md). The recorder stores the first few
// UNIQUE rcx values per probe plus the latest rdx; snap_drain prints them once
// per 10 s. No writes, no reads through the pointers.
constexpr uint64_t kSkelPostProbe  = 8;
constexpr uint64_t kPlayerCompProbe = 10;
constexpr uint64_t kHikReaderProbe = 9;
constexpr int      kSkelPtrs       = 8;

std::atomic<uint64_t> g_skel_rcx[kSkelPtrs] = {};
std::atomic<uint64_t> g_skel_rdx_last{0};
std::atomic<uint64_t> g_hik_rcx[kSkelPtrs] = {};
std::atomic<uint64_t> g_hik_rdx_last{0};

// --- build 14b: pointer classification -------------------------------------
//
// 14a's first-8 tables filled within seconds and could not distinguish "a
// fixed set of engine anim contexts" from "the first 8 of many churning
// per-character components". 14b answers that plus WHAT the objects are:
//
//   1. The recorder round-robins every rcx into a small ring (relaxed store,
//      nothing else added to the hot path).
//   2. The 1 Hz drain thread harvests the rings into drain-side distinct
//      sets (capped, overflow counted): set growth over time IS the churn
//      measurement.
//   3. For every NEW distinct pointer the drain reads its first qword under
//      SEH. A value inside the image is a vtable: its RVA identifies the
//      class offline via docs/RAW/rtti-all.txt. (Pointers can be stale by
//      the time the drain reads them; SEH turns that into "unreadable", and
//      a garbage read cannot land in-image by accident often enough to
//      matter for a survey.)
//   4. The HIK reader's rdx (the data source) gets its first 16 bytes
//      dumped: HIK datablocks begin with a literal magic ("HIKCHARACTER000"
//      per the parser's own compares), so the bytes themselves say whether
//      rdx IS the datablock.
// Build 15d.1: 16 was too small a window. ~82 characters cycle through
// SkeletonPostUpdate, so the last-16 snapshot missed the player's object on
// most drain ticks (the 01:34-01:36 pin flapping in the sha-7d1ac069 log).
// 128 covers more than one full pass over every character.
constexpr uint32_t kRingSize = 128;   // power of two
std::atomic<uint64_t> g_skel_ring[kRingSize] = {};
std::atomic<uint32_t> g_skel_ring_w{0};
std::atomic<uint64_t> g_hik_ring[kRingSize] = {};
std::atomic<uint32_t> g_hik_ring_w{0};
std::atomic<uint64_t> g_hik_rdx_ring[kRingSize] = {};
std::atomic<uint32_t> g_hik_rdx_ring_w{0};

// Lock-free first-N-unique insert. Full table drops the value; the drain line
// shows the table so a full table is visible rather than silent.
void capture_unique(std::atomic<uint64_t>* table, uint64_t v) {
    if (!v) return;
    for (int i = 0; i < kSkelPtrs; ++i) {
        uint64_t cur = table[i].load(std::memory_order_relaxed);
        if (cur == v) return;
        if (cur == 0) {
            uint64_t expected = 0;
            if (table[i].compare_exchange_strong(expected, v,
                                                 std::memory_order_relaxed))
                return;
            if (expected == v) return;
        }
    }
}

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
// Build 15L: rcx captured at the cPlayerComponent::OnInit callee, and the
// player entity resolved from it on the drain thread.
std::atomic<uint64_t> g_playercomp{0};
std::atomic<uint64_t> g_player_entity{0};

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
// Build 15e: is the anchored path actually live? Reported at 1 Hz.
std::atomic<bool> g_fp_anchored{false};

// Build 15e: THE PLAYER PIN, look-at discriminator.
//
// "Nearest character to the camera" is not good enough: the three squadmates
// stand within 2 m, so nearest flip-flops between four characters tick to
// tick (logs 02:41 and 03:08). But the chase camera LOOKS AT the player, so
// the unit vector from camera to player is nearly parallel to the camera's
// own forward axis, while a teammate beside you is far off it. Score by that
// alignment and the pin holds the right character even in a huddle.
//
// Runs on the drain thread, read-only, SEH-guarded. Publishes the winner to
// HeadPose for the camera write to re-read every frame.
struct PinResult {
    uint64_t obj    = 0;
    float    align  = 0;    // horizontal dot(dir to character, camera fwd)
    float    dist   = 0;    // horizontal distance from the camera
    int      cands  = 0;
    float    pos[3] = {0, 0, 0};   // winner's origin, for continuity
    char     mode   = '-';  // C incumbent kept, N nearest re-pin, A acquired
};

// 15e.3: POSITION CONTINUITY owns the pin; the camera-alignment test is
// demoted to acquisition only. Alignment identification is inherently
// ambiguous: whenever the chase camera sweeps across a bystander, that
// bystander scores a perfect alignment while the player sits momentarily
// off-axis, and no threshold closes the gap (15e changes=12, 15e.2
// changes=4, user: "still jumps to teammates"). Position cannot be faked:
// between 1 Hz pin ticks a character moves a few meters at most, so the
// candidate nearest the LAST PINNED POSITION is the same character, whether
// or not the engine recycled the object pointer (the 15d finding that
// killed pointer-identity gating). The strict 15e.1 horizontal-alignment
// test remains for acquisition, when there is no last position or a
// teleport/fast travel broke continuity.
constexpr float kPinAcquire = 0.985f;   // 10 deg: needed to ACQUIRE the pin
constexpr float kPinGate    = 12.0f;    // continuity radius per tick, m
// 15k.2: acquisition candidacy. Framed (20 deg) and within a plausible
// chase arm; the nearest such humanoid wins. Every good pin all session
// measured 1.6..2.0 m, so 4 m is generous and still excludes the 6.15 m
// bystander that 15k grabbed.
constexpr float kPinFrame   = 0.940f;
constexpr float kPinMaxAcq  = 4.0f;

// 15h.2: HUMANOID GATE. The ring carries every SkeletonPostUpdate object,
// including props and weapons (runtime rig groups measured 1..19 bones with
// no Head bone; the 15h run showed the pin sitting on such a rig, which is
// the "randomly jumps to a different position" defect). A pin candidate
// must link a rig (obj+0x220) whose name map contains the Head bone hash.
// Rig layout per the session-18 disasm: sorted {u32 CRC32 hash, u16 index}
// pairs at [rig+0x50], count word at +0x5A.
bool rig_has_hash(uint64_t rig, uint32_t hash) {
    uint16_t cnt = 0;
    uint64_t map = 0;
    if (!read_block(rig + 0x5A, &cnt, sizeof(cnt))) return false;
    if (cnt == 0 || cnt >= 2048) return false;
    if (!read_block(rig + 0x50, &map, sizeof(map))) return false;
    if (map < 0x10000 || (map & 3)) return false;
    int a = 0, b = (int)cnt - 1;
    while (a <= b) {
        const int mid = (a + b) / 2;
        uint32_t h = 0;
        if (!read_block(map + (uint64_t)mid * 8, &h, sizeof(h))) return false;
        if (h == hash) return true;
        if (h < hash) a = mid + 1; else b = mid - 1;
    }
    return false;
}

// Build 16a: the same binary search, but returning the NODE INDEX rather than
// a yes/no. The map records are 8 bytes, {u32 CRC32 name hash, u16 node
// index}, sorted by hash. Returns -1 on miss or on any unreadable field. We
// walk the map ourselves rather than calling the engine's own lookup
// (0x00CF90F0) so no engine code runs on our thread and no address beyond the
// already-verified rig layout is trusted.
int rig_find_node(uint64_t rig, uint32_t hash) {
    uint16_t cnt = 0, bones = 0;
    uint64_t map = 0;
    if (!read_block(rig + 0x5A, &cnt, sizeof(cnt))) return -1;
    if (cnt == 0 || cnt >= 2048) return -1;
    if (!read_block(rig + 0x50, &map, sizeof(map))) return -1;
    if (map < 0x10000 || (map & 3)) return -1;
    read_block(rig + 0x8A, &bones, sizeof(bones));
    int a = 0, b = (int)cnt - 1;
    while (a <= b) {
        const int mid = (a + b) / 2;
        uint32_t h = 0;
        if (!read_block(map + (uint64_t)mid * 8, &h, sizeof(h))) return -1;
        if (h == hash) {
            uint16_t idx = 0xFFFF;
            if (!read_block(map + (uint64_t)mid * 8 + 4, &idx, sizeof(idx)))
                return -1;
            // The index must address a real node. bones is the rig's node
            // count; a hit outside it means the record layout is not what we
            // think and the caller must not use it.
            if (bones && idx >= bones) return -1;
            return (int)idx;
        }
        if (h < hash) a = mid + 1; else b = mid - 1;
    }
    return -1;
}

// Build 16a: THE HEAD BONE READ. Engine thread, inside the camera hook's SEH
// (the caller guards; this function itself only touches memory it has range
// checked). Layout is [VERIFIED] from the engine's own leaf accessors:
//
//   pose   = [skel + 0x238]        the per-character FINAL pose
//   buf    = [pose + 0x178]        bone transform buffer, stride 0x20
//   rec    = buf + idx * 0x20      { float4 translation, float4 quaternion }
//   flags  = [pose + 0x8C]         bit 26 set = buffer already in world space
//   rootT  = [pose + 0x00]         float4, the pose root translation
//   rootQ  = [pose + 0x10]         float4 xyzw, the pose root rotation
//
// The pose at +0x238 is created with bit 26 CLEARED, so its buffer is MODEL
// space and world = rootQ * boneT + rootT. Bit 26 is honoured anyway, because
// a future engine path could hand us a world-space buffer and silently
// double-transforming would be a subtle, hard-to-see error.
bool read_bone_world(uint64_t skel, unsigned int idx, float out[3]) {
    if (!skel || idx == 0xFFFFu) return false;
    uint64_t pose = 0, buf = 0;
    if (!read_block(skel + 0x238, &pose, sizeof(pose))) return false;
    if (pose < 0x10000 || (pose & 7)) return false;
    if (!read_block(pose + 0x178, &buf, sizeof(buf))) return false;
    if (buf < 0x10000 || (buf & 7)) return false;

    float t[4];
    if (!read_block(buf + (uint64_t)idx * 0x20, t, sizeof(t))) return false;
    if (!isfinite(t[0]) || !isfinite(t[1]) || !isfinite(t[2])) return false;

    uint32_t flags = 0;
    read_block(pose + 0x8C, &flags, sizeof(flags));
    if (flags & 0x04000000u) {           // already world space
        out[0] = t[0]; out[1] = t[1]; out[2] = t[2];
        return true;
    }

    float rt[4], rq[4];
    if (!read_block(pose + 0x00, rt, sizeof(rt))) return false;
    if (!read_block(pose + 0x10, rq, sizeof(rq))) return false;
    for (int i = 0; i < 4; ++i)
        if (!isfinite(rt[i]) || !isfinite(rq[i])) return false;

    // The root quaternion's w carries a uniform scale as well as the rotation
    // (offline note), so normalise before rotating: an unnormalised quaternion
    // would scale the bone offset as a side effect of the rotation.
    const float n = sqrtf(rq[0] * rq[0] + rq[1] * rq[1] +
                          rq[2] * rq[2] + rq[3] * rq[3]);
    if (!(n > 1e-6f)) return false;
    const float qx = rq[0] / n, qy = rq[1] / n, qz = rq[2] / n, qw = rq[3] / n;

    // v' = v + 2*qw*(q x v) + 2*(q x (q x v))
    const float cx = qy * t[2] - qz * t[1];
    const float cy = qz * t[0] - qx * t[2];
    const float cz = qx * t[1] - qy * t[0];
    const float dx = qy * cz - qz * cy;
    const float dy = qz * cx - qx * cz;
    const float dz = qx * cy - qy * cx;
    out[0] = t[0] + 2.0f * (qw * cx + dx) + rt[0];
    out[1] = t[1] + 2.0f * (qw * cy + dy) + rt[1];
    out[2] = t[2] + 2.0f * (qw * cz + dz) + rt[2];
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

// Build 17: THE AIM ARCHITECTURE EXPERIMENT.
//
// (The 16b effector-array sweep that lived here was removed per rule 6: two
// headset runs found nothing on the skeleton or the owner entity, and the
// negative plus its caveats are recorded in CURRENT-STATE.md. The next
// effector probe, when wanted, is the root-global route described there.)
//
// docs/RE-notes.md "THE ABSOLUTE AIM ANGLE EXISTS": an absolute yaw/pitch pair
// with virtual accessors is integrated from the look-input deltas every frame,
// and RECOIL writes the same pair. Whether it is the authoritative aim
// (bullets) or a camera-only value is the open architectural question, and it
// decides whether 1:1 VR aiming is possible. This experiment answers it in one
// test: queue a ONE-SHOT +20 degree bump onto the next SetYaw call (Numpad
// Decimal). The engine's integrate loop is get-modify-write on the same
// object, so the bump persists on its own.
//   - view and reticle snap ~20 deg and BULLETS FOLLOW: the pair is the
//     authoritative aim; absolute pose-driven aiming is on.
//   - view snaps but bullets keep the old line: camera-only; aim needs the
//     look-input delta route instead.
//   - view snaps then eases back: the "wanted" fields (+0xA0/+0xA4) drive a
//     controller that fights external writes; next probe writes those too.
//
// SetYaw is not an E9 thunk: RVA 0x006777C0 is a virtual-dispatch stub,
// `mov rax,[rcx]; jmp qword ptr [rax+0x570]`, alone in an int3-padded 16-byte
// .edata slot. install_raw verifies every byte below before patching, and the
// asm replacement re-implements the dispatch with the offset taken from these
// verified bytes. Register proof of the prototype (this=rcx, yaw radians in
// xmm1): the integrate site does `movaps xmm1,xmm0` right before its call
// (bytes read from the pinned exe at 0x124D34CB, session 20).
constexpr uintptr_t kSetYawSlotRva   = 0x006777C0;
constexpr uint8_t   kSetYawExpect[10] = {0x48, 0x8B, 0x01,              // mov rax,[rcx]
                                         0x48, 0xFF, 0xA0,              // jmp qword ptr [rax+
                                         0x70, 0x05, 0x00, 0x00};       //   0x570]

// Build 19: the pitch setter, same slot shape, dispatch offset 0x5D0
// (RE-notes "THE ABSOLUTE AIM ANGLE EXISTS", printer-proved accessor set).
constexpr uintptr_t kSetPitchSlotRva   = 0x005FA190;
constexpr uint8_t   kSetPitchExpect[10] = {0x48, 0x8B, 0x01,
                                           0x48, 0xFF, 0xA0,
                                           0xD0, 0x05, 0x00, 0x00};

hook::ThunkHook g_setyaw_hook;
hook::ThunkHook g_setpitch_hook;

// Build 18: HEAD HIDE (docs/RE-notes.md "The visibility setter, ported from
// the community mod" and "THE PROXIMITY HIDE, fully traced").
//
// Session 20 confirmed in the headset that the engine's own proximity cull
// short-circuits at first-person camera distance and never hides the head, so
// the mod must win the argument itself. The engine re-asserts visibility every
// camera update (hazard 29), which is why this is a detour that overrides the
// `hide` argument on EVERY call for the one matching object, not a one-shot
// call of our own (and why no engine code ever runs on our threads for this).
//
// The identity test is the community mod's, ported: the head-visibility
// component's [obj+0x08] is a name-hashed method table unique to its class.
// Rule 7 forbids trusting the table RVA bare, so install verifies the chain:
// the table's slot +0x1F0 must resolve (through its 5-byte jmp thunk if
// present) to the function found by THIS unique signature, which is the
// class's own slot-0x0F member (RVA 0x124E15A0 in the pinned binary).
constexpr uintptr_t kHeadTableRva     = 0x04A66410;
constexpr uintptr_t kHeadSetterThunk  = 0x029DC7D0;
constexpr uintptr_t kHeadSetterImpl   = 0x12582AC0;
constexpr const char* kHeadSlotFnSig  =
    "48 89 5C 24 08 57 48 83 EC 20 48 83 7A 20 00 48 89 D3 48 89 CF 74 ? "
    "49 89 D0 31 D2 E8";
// The setter's own body, unique, no rel32/rip operands (RE-notes).
constexpr const char* kHeadSetterSig  =
    "48 83 EC 08 44 0F B6 DA 49 89 C9 38 51 68 74 ? 44 0F B7 51 4A";

hook::ThunkHook g_headhide_hook;

// Build 16a diagnostics, written by the camera hook, read by the 1 Hz drain.
// Plain floats: a torn diagnostic read is harmless.
float g_head_pos[3]    = {};
float g_head_dz        = 0.0f;   // head height above the character origin
bool  g_head_valid     = false;
std::atomic<uint64_t> g_head_reads{0}, g_head_rejects{0};

bool skel_is_humanoid(uint64_t p) {
    constexpr uint32_t kHeadHash = 0x07C159A2;   // CRC32("Head")
    // 15h.4: the cache expires every ~64 calls. Rig heap addresses are
    // reused (the 15h.3 diag showed the same character reporting different
    // rig pointers across ticks), so a verdict cached at a stale address,
    // or one taken during a failed read, must not stick forever.
    static struct { uint64_t rig; bool human; } cache[64];
    static int n_cache = 0, calls = 0;
    if (++calls >= 64) {
        calls = 0;
        n_cache = 0;
    }
    uint64_t rig = 0;
    if (!read_block(p + 0x220, &rig, sizeof(rig))) return false;
    if (rig < 0x10000 || (rig & 7)) return false;
    for (int i = 0; i < n_cache; ++i)
        if (cache[i].rig == rig) return cache[i].human;
    const bool human = rig_has_hash(rig, kHeadHash);
    if (n_cache < 64) {
        cache[n_cache].rig = rig;
        cache[n_cache].human = human;
        ++n_cache;
    }
    return human;
}

PinResult pin_player(const std::atomic<uint64_t>* ring, uint32_t n,
                     uint64_t incumbent, const float* last_pos,
                     bool allow_acquire) {
    PinResult best;
    best.dist = 1e9f;   // 15k.2: acquisition minimises distance
    float cam[3], fwd[3];
    memcpy(cam, g_base_pos, sizeof(cam));
    memcpy(fwd, g_base + 3, sizeof(fwd));   // row 1 of the camera basis
    // Horizontal forward (15e.1: the vertical chase-camera tilt would
    // otherwise force a cone wide enough to contain bystanders).
    const float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
    if (!(fl > 0.1f)) return best;          // straight up/down, or no basis
    fwd[0] /= fl;
    fwd[1] /= fl;

    uint64_t inc_obj = 0, near_obj = 0;
    float    inc_pos[3], near_pos[3];
    float    inc_align = -2.0f, inc_dist = 0;
    float    near_d = 1e9f, near_align = 0, near_dist = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t p = ring[i].load(std::memory_order_relaxed);
        if (!p) continue;
        float o[3];
        if (!read_block(p + 0x120, o, sizeof(o))) continue;
        if (!isfinite(o[0]) || !isfinite(o[1]) || !isfinite(o[2])) continue;
        const float vx = o[0] - cam[0];
        const float vy = o[1] - cam[1];
        const float d  = sqrtf(vx * vx + vy * vy);
        // The chase arm, horizontally: never on top of the camera, never far.
        if (!(d > 0.3f && d < 8.0f)) continue;
        // 15h.2: props and weapons can never hold the pin.
        if (!skel_is_humanoid(p)) continue;
        ++best.cands;
        const float a = (vx / d) * fwd[0] + (vy / d) * fwd[1];
        if (p == incumbent) {
            inc_obj = p;
            inc_align = a;
            inc_dist = d;
            memcpy(inc_pos, o, sizeof(inc_pos));
        }
        if (last_pos) {
            const float cd = dist3(o, last_pos);
            if (cd < near_d) {
                near_d = cd;
                near_obj = p;
                near_align = a;
                near_dist = d;
                memcpy(near_pos, o, sizeof(near_pos));
            }
        }
        // 15k.2: ACQUISITION SCORES BY DISTANCE, NOT ALIGNMENT. The 15k
        // acquire took a character 6.15 m away at align 0.999: an NPC
        // standing behind the player, dead centre in frame. The chase arm
        // is short (measured 1.6..2.0 m on every good pin all session), and
        // anything the camera frames at that range IS the player. So among
        // framed humanoids, take the NEAREST, not the best aligned.
        if (a >= kPinFrame && d < best.dist) {
            best.align = a;
            best.obj   = p;
            best.dist  = d;
            memcpy(best.pos, o, sizeof(best.pos));
        }
    }

    // 15k.3: THE INCUMBENT IS NEVER REPLACED FROM THE RING. Root cause of
    // every jump since 15e: the 128-slot ring is fed by SkeletonPostUpdate
    // and hundreds of objects churn through it, so the PLAYER's own entry
    // is regularly evicted. The old "nearest to the last position" recovery
    // then handed the pin to whichever bystander stood closest to where the
    // player had been (log 21:32:22: perfect 1.84 m acquire, replaced one
    // second later by an NPC at 4.76 m). An acquired pointer is now held and
    // validated DIRECTLY, exactly as the camera write reads it, with the
    // ring consulted only for a fresh acquisition. The 'N' recovery path is
    // removed per rule 6.
    if (incumbent) {
        float o[3];
        if (read_block(incumbent + 0x120, o, sizeof(o)) &&
            isfinite(o[0]) && isfinite(o[1]) && isfinite(o[2])) {
            const float dx = o[0] - cam[0], dy = o[1] - cam[1];
            const float d  = sqrtf(dx * dx + dy * dy);
            if (d < 25.0f) {          // still plausibly the chased body
                best.obj   = incumbent;
                best.dist  = d;
                best.align = d > 0.01f
                    ? (dx / d) * fwd[0] + (dy / d) * fwd[1] : 0.0f;
                best.mode  = 'C';
                memcpy(best.pos, o, sizeof(best.pos));
                return best;
            }
        }
        // Unreadable or implausible: report the loss, hold nothing, and let
        // the caller decide (it keeps the old pin until a re-toggle).
        best.obj  = 0;
        best.mode = 'X';
        best.dist = 0;
        return best;
    }
    (void)inc_obj; (void)inc_align; (void)inc_dist; (void)inc_pos;
    (void)near_obj; (void)near_align; (void)near_dist; (void)near_pos;
    (void)near_d; (void)last_pos;
    // Acquisition: only a strictly framed candidate may take a fresh pin,
    // and 15k: only at the moment the USER asks (the Numpad 8 rising edge).
    // Every heuristic re-acquisition tonight eventually grabbed a bystander
    // (15e..15j ledger); the one moment the camera is GUARANTEED to be
    // framing the player is when the player himself toggles first person
    // from the third-person view.
    if (!allow_acquire || !best.obj || best.dist > kPinMaxAcq) {
        best.obj = 0;
        if (best.dist > 1e8f) best.dist = 0;
        return best;
    }
    best.mode = 'A';
    return best;
}

bool write_pose_head(uint64_t cam, const float* H, const float* Q) {
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
            // Build 13a: the tag carries the XR-space orientation this frame
            // is being composed with, so the present side can submit the true
            // render pose (see HeadPose.h).
            headpose::push_eye_tag(g_eye_toggle, Q);
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

        // Build 19: once aim injection has been absorbed by the engine, the
        // raw base rotation INCLUDES the head yaw/pitch we injected, and
        // composing the full head rotation on top would double-apply it.
        // Rebuild the base from its own geometric yaw/pitch with the absorbed
        // amounts removed. Extraction and rebuild share one convention
        // (game basis: x right, y forward, z up; yaw = atan2(fx, fy),
        // pitch = asin(fz)), so with zero absorbed this path is exact for a
        // roll-less base; engine camera roll (shakes) is dropped while it is
        // active, which is accepted v1 jank. While aim_cum is (0,0), the raw
        // base is used untouched: byte-identical to build 18 behaviour.
        const float* B = g_base;
        float rebuilt[9];
        float cy = 0, cp = 0;
        if (headpose::aim_cum(&cy, &cp)) {
            float sp = g_base[5];
            if (sp >  0.9999f) sp =  0.9999f;
            if (sp < -0.9999f) sp = -0.9999f;
            const float Y = atan2f(g_base[3], g_base[4]) - cy;
            float       P = asinf(sp) - cp;
            if (P >  1.55f) P =  1.55f;
            if (P < -1.55f) P = -1.55f;
            const float cyaw = cosf(Y), syaw = sinf(Y);
            const float cpit = cosf(P), spit = sinf(P);
            rebuilt[0] =  cyaw;        rebuilt[1] = -syaw;        rebuilt[2] = 0.0f;
            rebuilt[3] =  syaw * cpit; rebuilt[4] =  cyaw * cpit; rebuilt[5] = spit;
            rebuilt[6] = -syaw * spit; rebuilt[7] = -cyaw * spit; rebuilt[8] = cpit;
            B = rebuilt;
        }

        float out[9];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out[r * 3 + c] = H[r * 3 + 0] * B[0 + c]
                               + H[r * 3 + 1] * B[3 + c]
                               + H[r * 3 + 2] * B[6 + c];
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
            // Build 15e: ANCHORED first person. Prefer the character's own
            // world origin (skeleton +0x120, [VERIFIED]) over the 11c push:
            // the push rides the third-person camera, so it slides whenever
            // that camera pitches, orbits or shortens its arm ("gets out of
            // whack when you control the character"). Anchoring to the
            // character makes the viewpoint independent of the chase rig.
            // The pin comes from the drain thread; the ORIGIN is re-read
            // here every call so the camera tracks movement at frame rate.
            // Any failure (no pin, unreadable, implausible) falls through to
            // the 11c push, which is still the accepted demo behaviour.
            bool anchored = false;
            if (const uint64_t obj = (uint64_t)headpose::player_obj()) {
                float o[3];
                memcpy(o, (const void*)(obj + 0x120), sizeof(o));
                // Sanity: finite, and within one chase-arm of the camera. A
                // freed or recycled object reads as garbage and is dropped.
                if (isfinite(o[0]) && isfinite(o[1]) && isfinite(o[2]) &&
                    dist3(o, g_base_pos) < 12.0f) {
                    // Build 16a: THE HEAD BONE takes over from the origin
                    // when it reads back sane. The origin anchor stays as the
                    // fallback on every failure path, so a bad rig, a
                    // stale pose pointer or an unresolved node index costs
                    // us today's behaviour rather than the viewpoint.
                    // Plausibility is judged against the character's own
                    // origin, not the camera: a head is within a meter
                    // horizontally of the body centre and between the knees
                    // (prone) and full standing height above it.
                    bool head_ok = false;
                    if (headpose::fp_head_anchor()) {
                        float h[3];
                        if (read_bone_world(obj, headpose::head_node(), h)) {
                            const float hdx = h[0] - o[0];
                            const float hdy = h[1] - o[1];
                            const float hdz = h[2] - o[2];
                            if (hdx * hdx + hdy * hdy < 1.0f &&
                                hdz > -0.5f && hdz < 2.2f) {
                                o[0] = h[0];
                                o[1] = h[1];
                                o[2] = h[2];
                                g_head_pos[0] = h[0];
                                g_head_pos[1] = h[1];
                                g_head_pos[2] = h[2];
                                g_head_dz  = hdz;
                                head_ok    = true;
                                g_head_reads.fetch_add(
                                    1, std::memory_order_relaxed);
                            } else {
                                g_head_rejects.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        } else {
                            g_head_rejects.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        g_head_valid = head_ok;
                    }
                    // 15e.3: fp_anchor_side slides the anchored viewpoint
                    // along the BASE camera's right axis (row 0). The origin
                    // sits at body center but the head is not above it in
                    // weapon-ready stances (the body blades), so the user
                    // needs a centering knob until the head bone lands.
                    // Base right, not composed: turning the head must not
                    // swing the anchor.
                    // 16a: the vertical rise depends on WHICH anchor won.
                    // From the character origin it is fp_eye (0.85, a whole
                    // body height); from the head JOINT it is fp_head_eye
                    // (0.10, joint to eyes). Using one knob for both would
                    // put the viewpoint a metre above the character's head
                    // the moment the head bone started resolving.
                    const float as   = headpose::fp_anchor_side();
                    const float rise = head_ok ? headpose::fp_head_eye()
                                               : headpose::fp_eye();
                    pos[0] = o[0] + as * g_base[0];
                    pos[1] = o[1] + as * g_base[1];
                    pos[2] = o[2] + as * g_base[2] + rise;  // world up is +Z
                    anchored = true;
                }
            }
            if (!anchored) {
                // Build 11c/11f push, unchanged, including the fp_side
                // shoulder correction: that offset exists ONLY to undo the
                // third-person camera hanging off the right shoulder.
                // fp_eye and fp_anchor_side are the anchored path's controls.
                const float df = headpose::fp_forward();
                const float ds = headpose::fp_side();
                const float du = headpose::fp_up();
                for (int c = 0; c < 3; ++c)
                    pos[c] += df * g_base[3 + c] + ds * g_base[0 + c]
                            + du * g_base[6 + c];
            }
            g_fp_anchored.store(anchored, std::memory_order_relaxed);
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
            float H[9], Q[4];
            if (headpose::read(H, Q)) {
                if (write_pose_head(a->rcx, H, Q))
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

    // Build 14a: pointer capture for the skeleton/HIK probes. Counters and
    // the generic row below still run for them, giving caller RVAs and rates.
    // Build 14b adds the sampling rings; the drain thread does the rest.
    if (index == kPlayerCompProbe) {
        // One-shot-ish: OnInit runs at spawn. Publish the component pointer;
        // the drain thread does the (guarded) [+0x10] read.
        g_playercomp.store(a->rcx, std::memory_order_relaxed);
    }
    if (index == kSkelPostProbe) {
        capture_unique(g_skel_rcx, a->rcx);
        g_skel_rdx_last.store(a->rdx, std::memory_order_relaxed);
        g_skel_ring[g_skel_ring_w.fetch_add(1, std::memory_order_relaxed) &
                    (kRingSize - 1)].store(a->rcx, std::memory_order_relaxed);
    } else if (index == kHikReaderProbe) {
        capture_unique(g_hik_rcx, a->rcx);
        g_hik_rdx_last.store(a->rdx, std::memory_order_relaxed);
        g_hik_ring[g_hik_ring_w.fetch_add(1, std::memory_order_relaxed) &
                   (kRingSize - 1)].store(a->rcx, std::memory_order_relaxed);
        g_hik_rdx_ring[g_hik_rdx_ring_w.fetch_add(1, std::memory_order_relaxed) &
                       (kRingSize - 1)].store(a->rdx, std::memory_order_relaxed);
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
    g_module      = (uintptr_t)img->base;
    g_module_size = img->size;
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

    void* entries[11] = {
        (void*)&grwxr_probe_entry_0, (void*)&grwxr_probe_entry_1,
        (void*)&grwxr_probe_entry_2, (void*)&grwxr_probe_entry_3,
        (void*)&grwxr_probe_entry_4, (void*)&grwxr_probe_entry_5,
        (void*)&grwxr_probe_entry_6, (void*)&grwxr_probe_entry_7,
        (void*)&grwxr_probe_entry_8, (void*)&grwxr_probe_entry_9,
        (void*)&grwxr_probe_entry_10,
    };
    static_assert(kNumTargets == 11, "kTargets and the entry table must match");

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

    // Build 17: the SetYaw virtual-dispatch stub. Not part of the E9 table
    // above; install_raw verifies the exact slot bytes instead of a jump
    // target, and a mismatch leaves the aim experiment off with everything
    // else still running. The dispatch offset the asm stub must emulate and
    // the bump value are published to the asm globals BEFORE the patch makes
    // the entry reachable.
    {
        memcpy(&grwxr_setyaw_disp, kSetYawExpect + 6, sizeof(grwxr_setyaw_disp));
        g_setyaw_hook.install_raw(img->base + kSetYawSlotRva, kSetYawExpect,
                                  sizeof(kSetYawExpect),
                                  (void*)&grwxr_setyaw_entry,
                                  "SetYaw vdispatch 0x006777C0");
        memcpy(&grwxr_setpitch_disp, kSetPitchExpect + 6,
               sizeof(grwxr_setpitch_disp));
        g_setpitch_hook.install_raw(img->base + kSetPitchSlotRva,
                                    kSetPitchExpect, sizeof(kSetPitchExpect),
                                    (void*)&grwxr_setpitch_entry,
                                    "SetPitch vdispatch 0x005FA190");
        if (g_setyaw_hook.installed() && g_setpitch_hook.installed())
            LOG_INFO("aim: both setters hooked. Numpad Decimal toggles VR "
                     "head aim (default off).");
    }

    // Build 18: head hide. Three independent checks before anything is
    // patched, each failing loudly and independently (rule 7):
    //   1. the setter signature must match uniquely AT the documented impl,
    //   2. the class method table must be verified through its slot +0x1F0
    //      resolving to the function found by the slot-fn signature,
    //   3. ThunkHook::install verifies the E9 thunk really targets the impl.
    {
        bool hh_ok = true;
        size_t m = 0;
        auto setter = sig::find_unique(*img, kHeadSetterSig, &m);
        if (!setter || *setter != img->base + kHeadSetterImpl) {
            LOG_ERROR("hide: SetHidden signature %s (matches=%zu, expected "
                      "RVA 0x%08zX). Head hide OFF, everything else runs.",
                      setter ? "matched at the WRONG address" : "missed",
                      m, (size_t)kHeadSetterImpl);
            hh_ok = false;
        }
        auto slotfn = sig::find_unique(*img, kHeadSlotFnSig, &m);
        if (hh_ok && !slotfn) {
            LOG_ERROR("hide: class slot-fn signature missed (matches=%zu). "
                      "Cannot verify the method table. Head hide OFF.", m);
            hh_ok = false;
        }
        if (hh_ok) {
            // Slot +0x1F0 of the candidate table must resolve to *slotfn,
            // directly or through one 5-byte jmp thunk.
            uint64_t slotval = 0;
            memcpy(&slotval, img->base + kHeadTableRva + 0x1F0,
                   sizeof(slotval));
            const uint8_t* p = (const uint8_t*)slotval;
            const uint8_t* resolved = p;
            if (p >= img->base && p < img->base + img->size && p[0] == 0xE9) {
                int32_t rel = 0;
                memcpy(&rel, p + 1, sizeof(rel));
                resolved = p + 5 + rel;
            }
            if (resolved != *slotfn) {
                LOG_ERROR("hide: method table slot +0x1F0 resolves to 0x%p, "
                          "signature found 0x%p. NOT the class we analysed. "
                          "Head hide OFF.",
                          (const void*)resolved, (void*)*slotfn);
                hh_ok = false;
            }
        }
        if (hh_ok) {
            grwxr_headhide_impl = (uint64_t)(img->base + kHeadSetterImpl);
            if (g_headhide_hook.install(img->base + kHeadSetterThunk,
                                        img->base + kHeadSetterImpl,
                                        (void*)&grwxr_headhide_entry,
                                        "SetHidden 0x12582AC0")) {
                // Published LAST: the asm entry ignores everything until the
                // table pointer is nonzero.
                grwxr_headhide_table = (uint64_t)(img->base + kHeadTableRva);
                LOG_INFO("hide: armed. The head object hides whenever first "
                         "person is on (class table verified via slot fn).");
            }
        }
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

    // Build 15e: refresh the player pin every second (the anchored camera
    // re-reads the ORIGIN per frame, so 1 Hz only has to keep up with which
    // OBJECT is the player, not with movement). Sticky: a tick that cannot
    // identify the player keeps the previous pin rather than dropping the
    // viewpoint back to the third-person push.
    // 15e.2: never re-pin while the rendered fov is below the world band
    // (plain ADS 0.49..0.52, magnified optics lower; the world floor is
    // 0.78). In ADS the weapon's own skeleton instance sits dead-center in
    // front of the camera and inside the distance window, so a pin tick
    // during aim steals the pin for the GUN and the viewpoint lands on the
    // barrel (2026-08-02 log: pin changes at the same timestamp as fov
    // 0.2549 and 0.5236). The incumbent simply holds until the fov returns
    // to the world band.
    if (g_calls[kSkelPostProbe].load(std::memory_order_relaxed)) {
        const float pin_fov = headpose::read_fov(1.0f);
        // 15e.3: continuity state, drain thread only. Holds the last pinned
        // character position so the next tick can follow the CHARACTER
        // instead of re-judging by what the camera happens to frame.
        static float s_last_pos[3];
        static bool  s_have_last = false;
        // 15k: acquisition is armed ONLY by the FP toggle's rising edge and
        // disarmed by the first successful acquisition. While first person
        // stays on, the pin is continuity-only: nothing during play (ADS,
        // camera sweeps, crowds) can move it to another body. If it ever
        // looks wrong, toggling FP off and on while facing the character is
        // the user's deliberate re-pin.
        // Build 15L: THE EXACT PREDICATE. If the cPlayerComponent hook has
        // fired, we know the local player's ENTITY, and a skeleton belongs
        // to the player iff [skel+0x10] == that entity. One character is a
        // cluster of skeleton instances (body + gear), so take the cluster
        // member nearest the camera. No alignment, no distance guessing, no
        // toggle needed: NPCs are structurally excluded.
        if (const uint64_t pc = g_playercomp.load(std::memory_order_relaxed)) {
            uint64_t ent = 0;
            if (read_block(pc + 0x10, &ent, sizeof(ent)) &&
                ent > 0x10000 && !(ent & 7)) {
                const uint64_t was =
                    g_player_entity.exchange(ent, std::memory_order_relaxed);
                if (was != ent)
                    LOG_INFO("fp: player ENTITY 0x%012llX (from "
                             "cPlayerComponent 0x%012llX)",
                             (unsigned long long)ent,
                             (unsigned long long)pc);
            }
        }
        const uint64_t pent = g_player_entity.load(std::memory_order_relaxed);
        if (pent) {
            uint64_t bestp = 0;
            float bestd = 1e9f, bpos[3] = {0, 0, 0};
            int members = 0;
            for (uint32_t i = 0; i < kRingSize; ++i) {
                const uint64_t p =
                    g_skel_ring[i].load(std::memory_order_relaxed);
                if (!p) continue;
                uint64_t own = 0;
                if (!read_block(p + 0x10, &own, sizeof(own))) continue;
                if (own != pent) continue;
                ++members;
                float o[3];
                if (!read_block(p + 0x120, o, sizeof(o))) continue;
                if (!isfinite(o[0]) || !isfinite(o[1]) || !isfinite(o[2]))
                    continue;
                const float dx = o[0] - g_base_pos[0];
                const float dy = o[1] - g_base_pos[1];
                const float d  = sqrtf(dx * dx + dy * dy);
                if (d < bestd) {
                    bestd = d;
                    bestp = p;
                    memcpy(bpos, o, sizeof(bpos));
                }
            }
            if (bestp) {
                headpose::set_player_obj(bestp);
                // Build 16a: resolve the Head node index for THIS skeleton's
                // rig. Cheap (a binary search over a sorted map) and re-done
                // every tick, so a respawn, an outfit change or any other
                // event that swaps the rig cannot leave a stale index behind.
                // The camera write consumes only the published index.
                {
                    uint64_t rig = 0;
                    int node = -1;
                    if (read_block(bestp + 0x220, &rig, sizeof(rig)) &&
                        rig > 0x10000 && !(rig & 7))
                        node = rig_find_node(rig, 0x07C159A2u);  // CRC32 Head
                    const unsigned int pub =
                        node >= 0 ? (unsigned int)node : 0xFFFFu;
                    static unsigned int s_last_node = 0xFFFEu;
                    headpose::set_head_node(pub);
                    if (pub != s_last_node) {
                        s_last_node = pub;
                        if (pub == 0xFFFFu)
                            LOG_INFO("fp: HEAD BONE not found in rig "
                                     "0x%012llX (anchor falls back to the "
                                     "character origin)",
                                     (unsigned long long)rig);
                        else
                            LOG_INFO("fp: HEAD BONE node %u in rig 0x%012llX",
                                     pub, (unsigned long long)rig);
                    }
                }
                if ((ticks % 10) == 3)
                    LOG_INFO("fp: pin(entity) 0x%012llX dist=%.2fm "
                             "cluster=%d (camera %s)",
                             (unsigned long long)bestp, bestd, members,
                             g_fp_anchored.load(std::memory_order_relaxed)
                                 ? "ANCHORED to the character"
                                 : "on the 11c push (not anchored)");
                // The stance test the user runs: stand, crouch, go prone.
                // A real animated head bone moves ~1.6 / ~1.1 / ~0.3 m above
                // the character origin. A static offset does not move at all,
                // which is exactly how a wrong node index would present.
                if ((ticks % 2) == 0 && g_head_valid)
                    LOG_INFO("fp: HEAD world=(%.2f %.2f %.2f) dz=%+.2fm "
                             "reads=%llu rejects=%llu",
                             g_head_pos[0], g_head_pos[1], g_head_pos[2],
                             g_head_dz,
                             (unsigned long long)g_head_reads.load(
                                 std::memory_order_relaxed),
                             (unsigned long long)g_head_rejects.load(
                                 std::memory_order_relaxed));
                else if ((ticks % 10) == 3 && headpose::fp_head_anchor() &&
                         headpose::fp_enabled())
                    LOG_INFO("fp: HEAD not readable yet (node=%u reads=%llu "
                             "rejects=%llu); anchor is on the origin",
                             headpose::head_node(),
                             (unsigned long long)g_head_reads.load(
                                 std::memory_order_relaxed),
                             (unsigned long long)g_head_rejects.load(
                                 std::memory_order_relaxed));
            } else if ((ticks % 10) == 3) {
                LOG_INFO("fp: pin(entity) no ring member for entity "
                         "0x%012llX (holding 0x%012llX)",
                         (unsigned long long)pent,
                         (unsigned long long)headpose::player_obj());
            }
            // The heuristic pin and the identity diagnostics below exist
            // only to guess what we now know exactly, so skip the rest of
            // the drain tick while the entity is live.
            return;
        }

        static bool s_prev_fp = false;
        static bool s_want_acquire = false;
        const bool fp_now = headpose::fp_enabled();
        if (fp_now && !s_prev_fp) {
            s_want_acquire = true;
            headpose::set_player_obj(0);
            s_have_last = false;
            LOG_INFO("fp: pin cleared, acquisition armed by FP toggle");
        }
        s_prev_fp = fp_now;
        if (pin_fov >= 0.65f) {
            const uint64_t inc = (uint64_t)headpose::player_obj();
            const PinResult pin = pin_player(g_skel_ring, kRingSize, inc,
                                             s_have_last ? s_last_pos
                                                         : nullptr,
                                             s_want_acquire);
            if (pin.mode == 'X') {
                // 15k.3: the pinned pointer went bad (death, respawn, level
                // change). Drop it so the camera falls back to the 11c push
                // and the next FP toggle can re-acquire.
                LOG_INFO("fp: pin LOST (object unreadable or far); "
                         "toggle FP off/on facing your character to re-pin");
                headpose::set_player_obj(0);
                s_have_last = false;
                s_want_acquire = false;
            } else if (pin.obj) {
                headpose::set_player_obj(pin.obj);
                memcpy(s_last_pos, pin.pos, sizeof(s_last_pos));
                s_have_last = true;
                if (pin.mode == 'A') {
                    s_want_acquire = false;
                    LOG_INFO("fp: pin ACQUIRED 0x%012llX align=%.3f "
                             "dist=%.2fm, locked until next FP toggle",
                             (unsigned long long)pin.obj, pin.align,
                             pin.dist);
                }
            } else if (s_want_acquire) {
                // 15k.2: say why an armed acquisition found nobody, so a
                // failed toggle is diagnosable without another build.
                LOG_INFO("fp: acquire armed but no candidate "
                         "(humanoids in window=%d)", pin.cands);
            }
            // 15e.1: count pin CHANGES. Mode N changes are pointer churn on
            // the same character (benign); mode A changes after continuity
            // was live are the real body hops to watch.
            static unsigned changes = 0;
            if (pin.obj && inc && pin.obj != inc) ++changes;
            if ((ticks % 10) == 3) {
                LOG_INFO("fp: pin 0x%012llX mode=%c align=%.3f dist=%.2fm "
                         "cands=%d changes=%u (camera %s)",
                         (unsigned long long)headpose::player_obj(), pin.mode,
                         pin.align, pin.dist, pin.cands, changes,
                         g_fp_anchored.load(std::memory_order_relaxed)
                             ? "ANCHORED to the character"
                             : "on the 11c push (not anchored)");
                // 15h.3 DIAG: the 15h.2 run showed the candidate set missing
                // the player entirely (nothing at the ~1.75 m chase-arm
                // distance, only teammates at 3-4 m behind negative
                // alignment). For every ring entry in the distance window,
                // log dist, alignment, rig pointer, and the humanoid-gate
                // verdict, so the player's own entry (or its absence) is
                // visible directly.
                float cam2[3], fwd2[3];
                memcpy(cam2, g_base_pos, sizeof(cam2));
                memcpy(fwd2, g_base + 3, sizeof(fwd2));
                const float fl2 = sqrtf(fwd2[0] * fwd2[0] +
                                        fwd2[1] * fwd2[1]);
                if (fl2 > 0.1f) {
                    fwd2[0] /= fl2;
                    fwd2[1] /= fl2;
                    // 15h.4: widened to 30 m with no near floor (is the
                    // player hiding under d<0.3 or beyond 8 m? is the BASE
                    // camera even where we think it is?), owner scanned for
                    // the ASCII player name "Nomad" (user tip: the player
                    // character has a fixed name; if the owner entity
                    // carries it, identification is a string compare).
                    LOG_INFO("fp: diag base=(%.1f %.1f %.1f) fp=%d",
                             cam2[0], cam2[1], cam2[2],
                             headpose::fp_enabled() ? 1 : 0);
                    int shown = 0, in_window = 0;
                    for (uint32_t i = 0; i < kRingSize && shown < 8; ++i) {
                        const uint64_t p =
                            g_skel_ring[i].load(std::memory_order_relaxed);
                        if (!p) continue;
                        float o[3];
                        if (!read_block(p + 0x120, o, sizeof(o))) continue;
                        if (!isfinite(o[0]) || !isfinite(o[1]) ||
                            !isfinite(o[2])) continue;
                        const float dx = o[0] - cam2[0];
                        const float dy = o[1] - cam2[1];
                        const float dd = sqrtf(dx * dx + dy * dy);
                        if (!(dd < 30.0f)) continue;
                        ++in_window;
                        const float aa = dd > 0.01f
                            ? (dx / dd) * fwd2[0] + (dy / dd) * fwd2[1]
                            : 0.0f;
                        uint64_t rig = 0;
                        const bool rig_ok =
                            read_block(p + 0x220, &rig, sizeof(rig)) &&
                            rig > 0x10000 && !(rig & 7);
                        // Owner name scan: ASCII "Nomad" in the first 0x800
                        // bytes of the owner entity.
                        int nomad = 0;
                        uint64_t own = 0;
                        if (read_block(p + 0x10, &own, sizeof(own)) &&
                            own > 0x10000 && !(own & 7)) {
                            uint8_t ob[0x800];
                            if (read_block(own, ob, sizeof(ob))) {
                                for (size_t oo = 0; oo + 5 <= sizeof(ob);
                                     ++oo) {
                                    if (ob[oo] == 'N' && ob[oo+1] == 'o' &&
                                        ob[oo+2] == 'm' && ob[oo+3] == 'a' &&
                                        ob[oo+4] == 'd') {
                                        nomad = (int)oo;
                                        break;
                                    }
                                }
                            }
                        }
                        LOG_INFO("fp: diag 0x%012llX d=%.2f a=%+.2f rig=%s"
                                 "0x%012llX human=%d dz=%+.2f nomad=%d",
                                 (unsigned long long)p, dd, aa,
                                 rig_ok ? "" : "BAD ",
                                 (unsigned long long)rig,
                                 skel_is_humanoid(p) ? 1 : 0,
                                 o[2] - cam2[2], nomad);
                        ++shown;
                    }
                    LOG_INFO("fp: diag in_window=%d (showing %d)",
                             in_window, shown);
                    // 15i DISCOVERY: the chase camera must know its orbit
                    // target, and the pointer scan (15h) proved it stores no
                    // character POINTER, so it stores a POSITION. Scan the
                    // camera object for float triples equal (5 cm) to any
                    // nearby character origin; an offset that matches ONE
                    // character every pass is the engine's own answer to
                    // "who is the player".
                    // 15j: ARG-16 STRUCT SCAN. (The 15i/15i.2 camtgt scan is
                    // removed per rule 6: no persistent orbit-target field
                    // in cam+0x3000; its single-pass hits were the camera
                    // physically passing through the player's head.) Arg 16
                    // of on_calc_mvp is a pointer OUTSIDE the camera struct,
                    // unidentified since build 5. Whatever feeds the camera
                    // its chase target must know the player: scan the first
                    // 0x400 of arg16's target for (a) qword pointers equal
                    // to any ring skeleton/owner/rig, (b) float triples
                    // XY-matching a nearby character origin.
                    const uint64_t a16 = g_snap.arg[15];
                    if (a16 > 0x10000 && !(a16 & 7)) {
                        alignas(8) uint8_t ab[0x400];
                        if (read_block(a16, ab, sizeof(ab))) {
                            int hits = 0;
                            for (uint32_t i = 0;
                                 i < kRingSize && hits < 8; ++i) {
                                const uint64_t p = g_skel_ring[i].load(
                                    std::memory_order_relaxed);
                                if (!p) continue;
                                uint64_t own = 0, rg = 0;
                                read_block(p + 0x10, &own, sizeof(own));
                                read_block(p + 0x220, &rg, sizeof(rg));
                                float o[3];
                                const bool o_ok =
                                    read_block(p + 0x120, o, sizeof(o)) &&
                                    isfinite(o[0]) && isfinite(o[1]);
                                for (size_t w = 0;
                                     w + 8 <= sizeof(ab) && hits < 8;
                                     w += 4) {
                                    const uint64_t v =
                                        *(const uint64_t*)(ab + w);
                                    if ((w & 7) == 0 &&
                                        (v == p || (own && v == own) ||
                                         (rg && v == rg))) {
                                        LOG_INFO("fp: a16 0x%012llX+0x%03zX "
                                                 "ptr-> %s 0x%012llX",
                                                 (unsigned long long)a16, w,
                                                 v == p ? "skel"
                                                 : v == own ? "owner"
                                                            : "rig",
                                                 (unsigned long long)p);
                                        ++hits;
                                    }
                                    if (o_ok && w + 12 <= sizeof(ab)) {
                                        const float* f = (const float*)(ab + w);
                                        const float ex = f[0] - o[0];
                                        const float ey = f[1] - o[1];
                                        if (isfinite(f[0]) && isfinite(f[1]) &&
                                            ex * ex + ey * ey < 0.0225f &&
                                            isfinite(f[2]) &&
                                            fabsf(f[2] - o[2]) < 3.0f) {
                                            LOG_INFO("fp: a16 0x%012llX+"
                                                     "0x%03zX xy 0x%012llX "
                                                     "dz=%+.2f",
                                                     (unsigned long long)a16,
                                                     w,
                                                     (unsigned long long)p,
                                                     f[2] - o[2]);
                                            ++hits;
                                        }
                                    }
                                }
                            }
                            if (hits == 0)
                                LOG_INFO("fp: a16 0x%012llX no matches",
                                         (unsigned long long)a16);
                        }
                    }
                }
            }
        } else if ((ticks % 10) == 3) {
            LOG_INFO("fp: pin FROZEN, fov %.4f is below the world band "
                     "(holding 0x%012llX)",
                     pin_fov, (unsigned long long)headpose::player_obj());
        }
    }

    // Build 15h (session 18): RIG IDENTITY + INSTANCE POOL, read-only. The
    // offline disasm of the rig factory chain (session 18 agent, RE-notes)
    // mapped the rig descriptor: a SHARED, cached 0xF8 object per skeleton
    // class. Bone count word at rig+0x8A; sorted {u32 CRC32 bone-name hash,
    // u16 node index} map at [rig+0x50] with count word at +0x5A; instance
    // pool helper ptr at rig+0x20 (0x60 bytes: pool base at +0x18, one
    // 0x1808 allocation holding 64 slots of 0x60, active-list sentinel at
    // pool+0x17A0, links at slot+0x48/+0x40). Player ground truth: 100
    // bones; Head=0x07C159A2, LeftHand=0xB675F36C, RightHand=0x75F94D30.
    // (a) every pass: group live skeletons by rig, log bone count and Head
    //     membership per rig: how many characters share the player CLASS;
    // (b) once: raw-dump the helper, pool header, and sentinel region of
    //     each 100-bone rig so the slot layout (expected to hold the
    //     per-character pose buffer pointer) can be derived offline.
    // The 15g.2 camera-target scan is kept (verdict pending); the module
    // scan is dropped (15g: 14 passes, zero matches).
    if ((ticks % 30) == 15 &&
        g_calls[kSkelPostProbe].load(std::memory_order_relaxed)) {
        // Candidate set: skeleton (S), owner entity (O), rig (R) per ring
        // entry, deduped, sorted for range-gated binary search.
        constexpr int kMaxCand = kRingSize * 3;
        static uint64_t vals[kMaxCand];
        static uint64_t via_skel[kMaxCand];   // which instance produced it
        static char     typ[kMaxCand];
        int n = 0;
        auto add = [&](uint64_t v, char t, uint64_t skel) {
            if (v < 0x10000 || (v & 7) || n >= kMaxCand) return;
            for (int k = 0; k < n; ++k)
                if (vals[k] == v) return;
            vals[n] = v; typ[n] = t; via_skel[n] = skel; ++n;
        };
        for (uint32_t i = 0; i < kRingSize; ++i) {
            const uint64_t p = g_skel_ring[i].load(std::memory_order_relaxed);
            if (!p) continue;
            add(p, 'S', p);
            uint64_t q = 0;
            if (read_block(p + 0x10, &q, sizeof(q))) add(q, 'O', p);
            q = 0;
            if (read_block(p + 0x220, &q, sizeof(q))) add(q, 'R', p);
        }
        if (n > 0) {
            for (int a = 1; a < n; ++a) {   // insertion sort, keep triples
                const uint64_t vv = vals[a], vs = via_skel[a];
                const char vt = typ[a];
                int b = a - 1;
                while (b >= 0 && vals[b] > vv) {
                    vals[b + 1] = vals[b];
                    via_skel[b + 1] = via_skel[b];
                    typ[b + 1] = typ[b];
                    --b;
                }
                vals[b + 1] = vv; via_skel[b + 1] = vs; typ[b + 1] = vt;
            }
            const uint64_t lo = vals[0], hi = vals[n - 1];
            auto find = [&](uint64_t v) -> int {
                if (v < lo || v > hi) return -1;
                int a2 = 0, b2 = n - 1;
                while (a2 <= b2) {
                    const int mid = (a2 + b2) / 2;
                    if (vals[mid] == v) return mid;
                    if (vals[mid] < v) a2 = mid + 1; else b2 = mid - 1;
                }
                return -1;
            };
            int logged = 0;

            // (a) The camera object, 0x2000 in 0x800 chunks.
            const uint64_t cam = g_player_cam.load(std::memory_order_relaxed);
            int cam_hits = 0, cam_fail = 0;
            if (cam) {
                alignas(8) uint8_t cbuf[0x800];
                for (uint64_t coff = 0; coff < 0x2000; coff += sizeof(cbuf)) {
                    if (!read_block(cam + coff, cbuf, sizeof(cbuf))) {
                        ++cam_fail;
                        continue;
                    }
                    for (size_t o = 0; o + 8 <= sizeof(cbuf); o += 8) {
                        const int f = find(*(const uint64_t*)(cbuf + o));
                        if (f < 0) continue;
                        ++cam_hits;
                        if (logged < 24) {
                            LOG_INFO("idglobal: cam+0x%04llX -> %c "
                                     "0x%012llX (skel 0x%012llX)",
                                     (unsigned long long)(coff + o), typ[f],
                                     (unsigned long long)vals[f],
                                     (unsigned long long)via_skel[f]);
                            ++logged;
                        }
                    }
                }
            }

            LOG_INFO("idglobal: pass done, cands=%d cam=0x%012llX "
                     "camhits=%d camfail=%d",
                     n, (unsigned long long)cam, cam_hits, cam_fail);
        }

        // (a) Rig groups: which live characters share which rig descriptor.
        struct RigGroup {
            uint64_t rig;
            int      nskel;
            uint16_t bones;
            uint16_t mapn;
            int      head_idx;   // node index of the Head hash, -1 if absent
            uint64_t skel0;      // first skeleton seen with this rig
        };
        RigGroup groups[24];
        int ngrp = 0, unreadable = 0;
        const uint64_t pinned_skel = (uint64_t)headpose::player_obj();
        uint64_t pinned_rig = 0;
        for (uint32_t i = 0; i < kRingSize; ++i) {
            const uint64_t p = g_skel_ring[i].load(std::memory_order_relaxed);
            if (!p) continue;
            uint64_t rig = 0;
            if (!read_block(p + 0x220, &rig, sizeof(rig)) ||
                rig < 0x10000 || (rig & 7)) {
                ++unreadable;
                continue;
            }
            if (p == pinned_skel) pinned_rig = rig;
            int g = -1;
            for (int k = 0; k < ngrp; ++k)
                if (groups[k].rig == rig) { g = k; break; }
            if (g >= 0) {
                ++groups[g].nskel;
                continue;
            }
            if (ngrp >= 24) continue;
            RigGroup& gr = groups[ngrp];
            gr.rig = rig;
            gr.nskel = 1;
            gr.skel0 = p;
            gr.bones = 0;
            gr.mapn = 0;
            gr.head_idx = -1;
            read_block(rig + 0x8A, &gr.bones, sizeof(gr.bones));
            read_block(rig + 0x5A, &gr.mapn, sizeof(gr.mapn));
            if (rig_has_hash(rig, 0x07C159A2u)) gr.head_idx = 1;
            ++ngrp;
        }
        for (int k = 0; k < ngrp; ++k) {
            // 15h.2: wrist-target hashes are player-BODY helper bones from
            // the offline GR_PCF rig (partial names ...LeftWristTarget /
            // ...RightWristTarget). If only one live rig carries them, that
            // rig IS the player's, and identification is exact.
            const bool wl = groups[k].head_idx >= 0 &&
                            rig_has_hash(groups[k].rig, 0xDE15CAC7u);
            const bool wr = groups[k].head_idx >= 0 &&
                            rig_has_hash(groups[k].rig, 0x4F870480u);
            LOG_INFO("rigid: rig 0x%012llX skels=%d bones=%u map=%u "
                     "Head=%d W=%d%d%s",
                     (unsigned long long)groups[k].rig, groups[k].nskel,
                     groups[k].bones, groups[k].mapn,
                     groups[k].head_idx >= 0 ? 1 : 0, wl ? 1 : 0, wr ? 1 : 0,
                     groups[k].rig == pinned_rig ? " [PINNED]" : "");
        }
        LOG_INFO("rigid: pass done, rigs=%d unreadable=%d pinned_rig=0x%012llX",
                 ngrp, unreadable, (unsigned long long)pinned_rig);

        // (b) One-shot raw dumps of the instance-pool structures of every
        // 100-bone rig, for offline layout derivation.
        static bool s_pool_dumped = false;
        if (!s_pool_dumped) {
            int dumped = 0;
            for (int k = 0; k < ngrp && dumped < 3; ++k) {
                // 15h.2: runtime rigs are MERGED (255-276 bones for
                // humanoids, never the asset's 100), so gate on the Head
                // hash instead of an exact count.
                if (groups[k].head_idx < 0) continue;
                uint64_t helper = 0;
                if (!read_block(groups[k].rig + 0x20, &helper,
                                sizeof(helper)) ||
                    helper < 0x10000 || (helper & 7)) continue;
                ++dumped;
                uint64_t q[12] = {};
                if (read_block(helper, q, sizeof(q))) {
                    LOG_INFO("pool: rig 0x%012llX helper %016llX %016llX "
                             "%016llX %016llX %016llX %016llX",
                             (unsigned long long)groups[k].rig,
                             q[0], q[1], q[2], q[3], q[4], q[5]);
                    LOG_INFO("pool:   helper+30 %016llX %016llX %016llX "
                             "%016llX %016llX %016llX",
                             q[6], q[7], q[8], q[9], q[10], q[11]);
                }
                const uint64_t pool = q[3];   // helper+0x18
                if (pool > 0x10000 && !(pool & 7)) {
                    uint64_t ph[8] = {};
                    if (read_block(pool, ph, sizeof(ph)))
                        LOG_INFO("pool:   base %016llX %016llX %016llX "
                                 "%016llX %016llX %016llX %016llX %016llX",
                                 ph[0], ph[1], ph[2], ph[3], ph[4], ph[5],
                                 ph[6], ph[7]);
                    uint64_t sn[8] = {};
                    if (read_block(pool + 0x1780, sn, sizeof(sn)))
                        LOG_INFO("pool:   +1780 %016llX %016llX %016llX "
                                 "%016llX %016llX %016llX %016llX %016llX",
                                 sn[0], sn[1], sn[2], sn[3], sn[4], sn[5],
                                 sn[6], sn[7]);
                }
            }
            if (dumped) s_pool_dumped = true;
        }
    }

    // Build 14a/14b: skeleton/HIK probe report, once per 10 s, only once
    // either probe has fired. The drain thread owns all of this state; the
    // recorder only feeds the rings and first-8 tables.
    if ((ticks % 10) == 3) {
        const unsigned long long sk =
            g_calls[kSkelPostProbe].load(std::memory_order_relaxed);
        const unsigned long long hk =
            g_calls[kHikReaderProbe].load(std::memory_order_relaxed);
        if (sk || hk) {
            // Drain-side distinct sets. 256 entries each; overflow means the
            // population is even larger than that, which is itself an answer.
            struct SeenSet {
                uint64_t           v[256];
                int                n        = 0;
                unsigned long long overflow = 0;
                bool add(uint64_t p) {
                    if (!p) return false;
                    for (int i = 0; i < n; ++i)
                        if (v[i] == p) return false;
                    if (n < 256) { v[n++] = p; return true; }
                    ++overflow;
                    return false;
                }
            };
            static SeenSet skel_seen, hik_seen, hik_rdx_seen;

            // Classify a pointer by its first qword. In-image = vtable RVA
            // (offline lookup: docs/RAW/rtti-all.txt). The pointer may be
            // stale by now; read_block's SEH turns that into "gone".
            auto classify = [](const char* which, uint64_t p) {
                uint64_t vt = 0;
                if (!read_block(p, &vt, sizeof(vt))) {
                    LOG_INFO("skel: new %s obj 0x%012llX first-qword UNREADABLE (stale?)",
                             which, (unsigned long long)p);
                } else if (vt >= g_module && vt < g_module + g_module_size) {
                    LOG_INFO("skel: new %s obj 0x%012llX vtbl RVA 0x%08llX",
                             which, (unsigned long long)p,
                             (unsigned long long)(vt - g_module));
                } else {
                    LOG_INFO("skel: new %s obj 0x%012llX first qword 0x%016llX (not a vtable)",
                             which, (unsigned long long)p,
                             (unsigned long long)vt);
                }
            };

            // Harvest a ring plus its first-8 table into a set, classifying
            // up to a bounded number of newcomers per tick (log hygiene).
            auto harvest = [&](const char* which, SeenSet& set,
                               std::atomic<uint64_t>* ring,
                               std::atomic<uint64_t>* first8, bool do_classify) {
                int fresh = 0;
                for (uint32_t i = 0; i < kRingSize + kSkelPtrs; ++i) {
                    const uint64_t p =
                        (i < kRingSize ? ring[i] : first8[i - kRingSize])
                            .load(std::memory_order_relaxed);
                    if (set.add(p)) {
                        ++fresh;
                        if (do_classify && fresh <= 6) classify(which, p);
                    }
                }
                return fresh;
            };

            const int f_post = harvest("post", skel_seen, g_skel_ring,
                                       g_skel_rcx, true);
            const int f_hik  = harvest("hik", hik_seen, g_hik_ring,
                                       g_hik_rcx, true);
            int f_rdx = 0;
            for (uint32_t i = 0; i < kRingSize; ++i)
                if (hik_rdx_seen.add(
                        g_hik_rdx_ring[i].load(std::memory_order_relaxed)))
                    ++f_rdx;

            LOG_INFO("skel: post=%llu calls, distinct rcx=%d(+%d)%s | "
                     "hik=%llu calls, rcx=%d(+%d), rdx=%d(+%d)%s",
                     sk, skel_seen.n, f_post,
                     skel_seen.overflow ? " OVERFLOW" : "",
                     hk, hik_seen.n, f_hik, hik_rdx_seen.n, f_rdx,
                     hik_rdx_seen.overflow ? " OVERFLOW" : "");

            // The HIK data source's first 16 bytes: a HIK datablock announces
            // itself with a literal magic. Dumped every report; rdx changes
            // per call, so this samples different characters over time.
            const uint64_t rdx =
                g_hik_rdx_last.load(std::memory_order_relaxed);
            uint8_t b[16];
            if (rdx && read_block(rdx, b, sizeof(b))) {
                char ascii[17];
                for (int i = 0; i < 16; ++i)
                    ascii[i] = (b[i] >= 0x20 && b[i] < 0x7F) ? (char)b[i] : '.';
                ascii[16] = 0;
                LOG_INFO("skel: hik rdx 0x%012llX bytes %02X %02X %02X %02X %02X %02X %02X %02X "
                         "%02X %02X %02X %02X %02X %02X %02X %02X \"%s\"",
                         (unsigned long long)rdx,
                         b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                         b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15],
                         ascii);
            }

            // Build 15d: PLAYER PIN + HEAD-BONE HUNT. The 14g.2 scanner is
            // SUPERSEDED, its question answered: aggregated over the
            // 2026-08-01 runs it hit the character world position at
            // obj+0x120 (copy at +0x250, owner copy at +0x050) on every
            // instance it touched, 18 hits across many objects; the one
            // near-camera sample sat 1.0 m below the camera root, the
            // character origin. This iteration climbs the ladder:
            // (a) PIN the player: of the skeletons that updated this frame,
            //     the one whose +0x120 is nearest the camera, sticky across
            //     ticks (squadmates hover nearby, but the chase camera hangs
            //     ~1 m from the player only);
            // (b) hunt the HEAD BONE inside the pinned player: sweep the
            //     object, its transform sub-objects [+0x230]/[+0x238], the
            //     owner entity, and the +0x2D0 item payloads for float
            //     triples parked within 1.5 m horizontally of the player
            //     origin at plausible body heights. USER PROTOCOL: stand
            //     still, crouch, go prone, stand: a real head bone's dz
            //     tracks ~1.6 standing, ~1.1 crouched, ~0.3 prone; static
            //     offsets do not move.
            // All reads drain-side under SEH; stale pointers read as garbage
            // and are dropped by the range checks, never dereferenced hot.
            {
                const uint64_t cam = g_player_cam.load(std::memory_order_relaxed);
                float cam_pos[3];
                // Camera root transform row 3 (position), Camera+0x000+0x30,
                // the same slot write_pose_head treats as position. Skipped
                // near the world origin (menus/loads), where zeros would
                // false-match.
                if (cam && read_block(cam + 0x30, cam_pos, sizeof(cam_pos))) {
                    const float zero3[3] = {0, 0, 0};
                    if (dist3(cam_pos, zero3) > 1000.0f) {
                        LOG_INFO("skel: scan cam=(%.1f %.1f %.1f)",
                                 cam_pos[0], cam_pos[1], cam_pos[2]);
                        // --- (a) the pin --------------------------------
                        static uint64_t player = 0;
                        static int      streak = 0;
                        // 15d.2: track the TWO nearest. The dc2c8bcf run
                        // showed two distinct objects parked at the same
                        // horizontal spot 0.2 m apart vertically (body rig
                        // plus a second rig?); hunt both.
                        uint64_t best        = 0, second = 0;
                        float    best_d      = 6.0f, sec_d = 6.0f;
                        float    best_pos[3] = {}, sec_pos[3] = {};
                        for (uint32_t i = 0; i < kRingSize; ++i) {
                            const uint64_t p =
                                g_skel_ring[i].load(std::memory_order_relaxed);
                            if (!p || p == best || p == second) continue;
                            float f[3];
                            if (!read_block(p + 0x120, f, sizeof(f))) continue;
                            const float dx = f[0] - cam_pos[0];
                            const float dy = f[1] - cam_pos[1];
                            const float dz = cam_pos[2] - f[2];
                            // The camera floats 0..2.5 m above the origin of
                            // the character it chases.
                            if (dz < -0.5f || dz > 2.5f) continue;
                            const float d = sqrtf(dx * dx + dy * dy);
                            if (d < best_d) {
                                second = best; sec_d = best_d;
                                memcpy(sec_pos, best_pos, sizeof(sec_pos));
                                best_d      = d;
                                best        = p;
                                best_pos[0] = f[0];
                                best_pos[1] = f[1];
                                best_pos[2] = f[2];
                            } else if (d < sec_d) {
                                sec_d      = d;
                                second     = p;
                                sec_pos[0] = f[0];
                                sec_pos[1] = f[1];
                                sec_pos[2] = f[2];
                            }
                        }
                        // 15d.1: the skeleton OBJECTS CHURN (session 14:
                        // per-character objects of this class are transient),
                        // so pointer identity cannot gate the hunt: the same
                        // character came back as a different address between
                        // ticks and streak never reached 2. The pointer IS
                        // live this frame (it came out of the ring), so hunt
                        // on every tick that has a nearest candidate. The
                        // streak stays logged as churn telemetry only.
                        if (best && best == player) ++streak;
                        else { player = best; streak = best ? 1 : 0; }
                        LOG_INFO("skel: player pin 0x%012llX d=%.2fm streak=%d "
                                 "pos=(%.2f %.2f %.2f) | second 0x%012llX "
                                 "d=%.2fm z=%.2f",
                                 (unsigned long long)player, best_d, streak,
                                 best_pos[0], best_pos[1], best_pos[2],
                                 (unsigned long long)second, sec_d, sec_pos[2]);

                        // --- (b) the head hunt, both candidates ---------
                        static uint8_t hbuf[0x1000];
                        // pp: the reference origin the window is measured
                        // against (the candidate character's +0x120).
                        auto hunt = [&](const char* tag, uint64_t base,
                                        size_t span, const float* pp) {
                                if (!base || base < 0x10000 ||
                                    base > 0x7FFFFFFFFFFFull || (base & 3))
                                    return;
                                size_t got =
                                    span < sizeof(hbuf) ? span : sizeof(hbuf);
                                while (got >= 0x100 &&
                                       !read_block(base, hbuf, got))
                                    got -= 0x100;   // page-end backoff
                                if (got < 0x100) return;
                                int hits = 0;
                                for (size_t off = 0; off + 12 <= got; off += 4) {
                                    float f[3];
                                    memcpy(f, hbuf + off, sizeof(f));
                                    // 15d.2: every comparison on a NaN is
                                    // false, so NaN triples sailed through
                                    // the range gates below and 441 of them
                                    // ate the whole log budget in the
                                    // dc2c8bcf run. Finite or gone.
                                    if (!isfinite(f[0]) || !isfinite(f[1]) ||
                                        !isfinite(f[2]))
                                        continue;
                                    const float dx = f[0] - pp[0];
                                    const float dy = f[1] - pp[1];
                                    const float dz = f[2] - pp[2];
                                    if (dx < -1.5f || dx > 1.5f) continue;
                                    if (dy < -1.5f || dy > 1.5f) continue;
                                    if (dz < -0.6f || dz > 2.3f) continue;
                                    // Exact copies of the origin itself are
                                    // the known +0x120/+0x250 family: skip.
                                    if (dx * dx + dy * dy < 1e-6f &&
                                        dz > -0.01f && dz < 0.01f)
                                        continue;
                                    ++hits;
                                    if (hits <= 12)
                                        LOG_INFO("skel: head? %s+0x%03zX "
                                                 "dz=%+.2f dxy=(%+.2f %+.2f)",
                                                 tag, off, dz, dx, dy);
                                }
                                if (hits > 12)
                                    LOG_INFO("skel: head? %s %d more in 0x%zX",
                                             tag, hits - 12, got);
                            };
                        const uint64_t cand[2] = {best, second};
                        const float*   cpos[2] = {best_pos, sec_pos};
                        for (int ci = 0; ci < 2; ++ci) {
                            const uint64_t pl = cand[ci];
                            if (!pl) continue;
                            const float*   pp = cpos[ci];
                            char T[4][16];
                            snprintf(T[0], sizeof(T[0]), "c%d.obj", ci);
                            snprintf(T[1], sizeof(T[1]), "c%d.t230", ci);
                            snprintf(T[2], sizeof(T[2]), "c%d.t238", ci);
                            snprintf(T[3], sizeof(T[3]), "c%d.own", ci);
                            hunt(T[0], pl, 0xD00, pp);
                            uint64_t t230 = 0, t238 = 0, owner = 0;
                            read_block(pl + 0x230, &t230, sizeof(t230));
                            read_block(pl + 0x238, &t238, sizeof(t238));
                            read_block(pl + 0x10, &owner, sizeof(owner));
                            LOG_INFO("skel: c%d subs t230=0x%012llX "
                                     "t238=0x%012llX own=0x%012llX",
                                     ci,
                                     (unsigned long long)t230,
                                     (unsigned long long)t238,
                                     (unsigned long long)owner);
                            hunt(T[1], t230, 0x800, pp);
                            hunt(T[2], t238, 0x800, pp);
                            // Field map: [[obj+0x238]+0x178] is an xmmword
                            // world-position candidate; read it directly.
                            uint64_t s178 = 0;
                            if (t238 &&
                                read_block(t238 + 0x178, &s178, sizeof(s178)) &&
                                s178 > 0x10000 && s178 < 0x7FFFFFFFFFFFull) {
                                float f[3];
                                if (read_block(s178, f, sizeof(f)))
                                    LOG_INFO("skel: c%d [t238+0x178]=0x%012llX "
                                             "-> (%.2f %.2f %.2f)",
                                             ci, (unsigned long long)s178,
                                             f[0], f[1], f[2]);
                            }
                            hunt(T[3], owner, 0x1000, pp);
                            // The +0x2D0 record table: 8 x 12 bytes of
                            // {item-array qword, word count at +0xA}; sweep
                            // the first few items of each populated record.
                            uint8_t recs[96];
                            if (read_block(pl + 0x2D0, recs, sizeof(recs))) {
                                int hunted = 0;
                                for (int r = 0; r < 8 && hunted < 8; ++r) {
                                    uint64_t arr = 0;
                                    uint16_t cnt = 0;
                                    memcpy(&arr, recs + r * 12, 8);
                                    memcpy(&cnt, recs + r * 12 + 10, 2);
                                    if (!arr || arr < 0x10000 ||
                                        arr > 0x7FFFFFFFFFFFull || (arr & 7) ||
                                        !cnt || cnt > 0x400)
                                        continue;
                                    uint64_t  items[4] = {};
                                    const int m = cnt < 4 ? cnt : 4;
                                    if (!read_block(arr, items, m * 8)) continue;
                                    for (int k = 0; k < m && hunted < 8;
                                         ++k, ++hunted) {
                                        char tag[16];
                                        snprintf(tag, sizeof(tag),
                                                 "c%d.it%d.%d", ci, r, k);
                                        hunt(tag, items[k], 0x140, pp);
                                    }
                                }
                            }
                        }

                        // (15d.4's quaternion-run detector lived here one run:
                        // NO unit-quat runs exist in the player object, its
                        // transform sub-objects, or the HIK rigs. Answered
                        // negative, removed per rule 6: the rigs hold
                        // matrices, not quats.)

                        // --- (c) 15d.6: rig link finder + rig scan ------
                        // The HIK "reader" is a per-spawn CONSTRUCTOR (189
                        // calls = 189 distinct rigs, all at load), so the
                        // capture ring holds whatever spawned recently, not
                        // the player (the 6dbddc4f run swept six stale rigs
                        // and hit nothing). The rig is built FROM the
                        // skeleton object, so find the link instead: sweep
                        // the pinned candidates' object and owner for any
                        // qword pointing at an object whose vtable is the
                        // rig class 0x03A81DC8, then world-scan that rig
                        // (+-6 m window, offsets logged, explicit negative).
                        auto rig_scan = [&](int ci, uint64_t rig,
                                            const float* pp) {
                            size_t got = sizeof(hbuf);
                            while (got >= 0x100 && !read_block(rig, hbuf, got))
                                got -= 0x100;
                            if (got < 0x100) {
                                LOG_INFO("skel: rig c%d 0x%012llX unreadable",
                                         ci, (unsigned long long)rig);
                                return;
                            }
                            int wh = 0;
                            for (size_t off = 0; off + 12 <= got; off += 4) {
                                float f[3];
                                memcpy(f, hbuf + off, sizeof(f));
                                if (!isfinite(f[0]) || !isfinite(f[1]) ||
                                    !isfinite(f[2]))
                                    continue;
                                const float dx = f[0] - pp[0];
                                const float dy = f[1] - pp[1];
                                const float dz = f[2] - pp[2];
                                if (dx < -6.f || dx > 6.f) continue;
                                if (dy < -6.f || dy > 6.f) continue;
                                if (dz < -1.f || dz > 2.6f) continue;
                                ++wh;
                                if (wh <= 24)
                                    LOG_INFO("skel: rig c%d+0x%03zX dz=%+.2f "
                                             "dxy=(%+.2f %+.2f)",
                                             ci, off, dz, dx, dy);
                            }
                            LOG_INFO("skel: rig c%d 0x%012llX: %d world hits "
                                     "in 0x%zX",
                                     ci, (unsigned long long)rig, wh, got);
                        };
                        for (int ci = 0; ci < 2; ++ci) {
                            const uint64_t pl = cand[ci];
                            if (!pl) continue;
                            uint64_t owner = 0;
                            read_block(pl + 0x10, &owner, sizeof(owner));
                            const struct { const char* tag; uint64_t base;
                                           size_t len; } spans[2] = {
                                {"obj", pl, 0xD00}, {"own", owner, 0x1000}};
                            int links = 0;
                            for (const auto& sp : spans) {
                                if (!sp.base || links >= 3) continue;
                                size_t got = sp.len < sizeof(hbuf)
                                                 ? sp.len : sizeof(hbuf);
                                while (got >= 0x100 &&
                                       !read_block(sp.base, hbuf, got))
                                    got -= 0x100;
                                if (got < 0x100) continue;
                                // hbuf is reused by rig_scan below, so pull
                                // the candidate pointers out first.
                                uint64_t rigs[3] = {};
                                size_t   roff[3] = {};
                                int      nr      = 0;
                                for (size_t off = 0;
                                     off + 8 <= got && nr < 3; off += 8) {
                                    uint64_t q;
                                    memcpy(&q, hbuf + off, sizeof(q));
                                    if (q < 0x10000 ||
                                        q > 0x7FFFFFFFFFFFull || (q & 7))
                                        continue;
                                    uint64_t vt = 0;
                                    if (!read_block(q, &vt, sizeof(vt)))
                                        continue;
                                    if (vt != g_module + 0x03A81DC8) continue;
                                    rigs[nr] = q;
                                    roff[nr] = off;
                                    ++nr;
                                }
                                for (int k = 0; k < nr && links < 3; ++k) {
                                    LOG_INFO("skel: riglink c%d.%s+0x%03zX -> "
                                             "rig 0x%012llX",
                                             ci, sp.tag, roff[k],
                                             (unsigned long long)rigs[k]);
                                    rig_scan(ci, rigs[k], cpos[ci]);
                                    ++links;
                                }
                            }
                            if (!links)
                                LOG_INFO("skel: riglink c%d: no rig pointer "
                                         "in obj/own", ci);
                        }
                    }
                }

                // (Build 14g's slot-70 check lived here; ANSWERED NEGATIVE
                // live in session 15 and removed per rule 6.)
            }
        }
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

    // Build 19 telemetry, drain thread only. The counters are written by the
    // asm stubs with plain aligned stores, so a torn read is impossible and a
    // slightly stale one is harmless.
    if (g_setyaw_hook.installed() || g_setpitch_hook.installed()) {
        static int aim_ticks = 0;
        if ((++aim_ticks % 5) == 2) {
            float ly = 0, lp = 0, cy = 0, cp = 0;
            uint32_t bits = grwxr_setyaw_lastval;
            memcpy(&ly, &bits, sizeof(ly));
            bits = grwxr_setpitch_lastval;
            memcpy(&lp, &bits, sizeof(lp));
            headpose::aim_cum(&cy, &cp);
            LOG_INFO("aim: yaw calls=%llu %.1f deg | pitch calls=%llu "
                     "%.1f deg | absorbed=(%.1f, %.1f) deg pending=(%u,%u)",
                     (unsigned long long)grwxr_setyaw_count, ly * 57.29578f,
                     (unsigned long long)grwxr_setpitch_count, lp * 57.29578f,
                     cy * 57.29578f, cp * 57.29578f,
                     grwxr_setyaw_pending, grwxr_setpitch_pending);
        }
    }

    // Build 18 telemetry. The latched object is the verified route to the
    // head-visibility component (RE-notes), so log it once when it appears.
    if (g_headhide_hook.installed()) {
        static uint64_t s_seen_obj = 0;
        static int hide_ticks = 0;
        const uint64_t obj = grwxr_headhide_obj;
        if (obj && obj != s_seen_obj) {
            s_seen_obj = obj;
            LOG_INFO("hide: head object LATCHED 0x%012llX (class identity "
                     "passed)", (unsigned long long)obj);
        }
        if ((++hide_ticks % 10) == 4) {
            LOG_INFO("hide: %s, setter calls=%llu forced=%llu obj=0x%012llX",
                     grwxr_headhide_on ? "FORCING (fp on)" : "idle (fp off)",
                     (unsigned long long)grwxr_headhide_calls,
                     (unsigned long long)grwxr_headhide_forced,
                     (unsigned long long)grwxr_headhide_obj);
        }
    }

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
    grwxr_headhide_table = 0;   // asm entry goes inert before the patch lifts
    g_headhide_hook.restore();
    g_setyaw_hook.restore();
    g_setpitch_hook.restore();
    for (auto& h : g_hooks) h.restore();
    g_any = false;
}

// Build 18: drive the head-hide override from the first-person state. Called
// from VRMirror's per-frame key poll; a plain aligned store, read by the asm
// entry on every engine SetHidden call. When this drops to 0 the engine's own
// re-assert (hazard 29, every camera update) restores visibility by itself
// within a frame, so no explicit un-hide call is needed or wanted.
void set_head_hide(bool on) {
    grwxr_headhide_on = on ? 1u : 0u;
}

// Build 19: the aim injection surface (supersedes build 17's one-shot bump,
// which this generalises). Render thread arms one delta per axis; the asm
// stub consumes it exactly once via xchg on the engine thread. The write
// order (bump first, then pending with a full barrier) means the stub can
// never read a stale bump: it only looks at bump after xchg returned 1.
bool aim_available() {
    return g_setyaw_hook.installed() && g_setpitch_hook.installed();
}

bool aim_pending(int axis) {
    return (axis == 0 ? grwxr_setyaw_pending : grwxr_setpitch_pending) != 0;
}

int aim_arm(int axis, float delta_engine_units) {
    hook::ThunkHook& h  = axis == 0 ? g_setyaw_hook : g_setpitch_hook;
    uint32_t& bump      = axis == 0 ? grwxr_setyaw_bump
                                    : grwxr_setpitch_bump;
    uint32_t& pending   = axis == 0 ? grwxr_setyaw_pending
                                    : grwxr_setpitch_pending;
    if (!h.installed()) return -1;
    if (pending) return 0;
    memcpy(&bump, &delta_engine_units, sizeof(bump));
    InterlockedExchange((volatile LONG*)&pending, 1);
    return 1;
}

}  // namespace camera
}  // namespace grwxr
