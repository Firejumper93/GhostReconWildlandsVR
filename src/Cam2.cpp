// Cam2.cpp - BUILD 243, review fixes in BUILD 244, gameplay gate / discriminator / engine viewports in
// BUILD 245, see Cam2.h. An experiment; the code default is OFF.
// BUILD 247: TRUE STEREO (cfg cam2_stereo, default OFF). The two cameras become the two eyes; the pose
// writes, the tag, the half-split submission and the stereo2: diagnostics are at the end of this file
// (set_stereo ... stereo_note_frame), in CameraProbe.cpp (write_pose_head, the selector branch) and in
// VRMirror.cpp (submit_halves). With cam2_stereo = 0 none of it runs.
//
// Layout: the pins (the only TU25-specific numbers), guarded memory helpers, the gameplay gate,
// the read-only discriminator ring and its dump, the creation path (SEH-wrapped, once), the
// per-frame path (SEH-wrapped), the builder callback, then the init-thread tick that verifies,
// gates, installs and logs.
//
// SOURCES (all in C:\Claude\scratch-tu25-render, TU25 RVAs, never carried to another build):
//   REPORT.md sections 5 and 9 and NOTES-second-camera.txt: the recipe, the field map and the build 244 diagnosis.
//   The decompile of the engine's own camera clone (FUN_14d7effc0): the exact list of
//   fields it copies, which is the list replicated in copy_camera_fields() below.
//   The decompile of the main-view creation (FUN_1467625c0): the same factory, the same
//   pose setter and the same AddCamera call this code makes for camera 2.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "Cam2.h"
#include "CameraProbe.h"
#include "Census.h"
#include "D3D11Hook.h"
#include "GameBuild.h"
#include "GtRec.h"   // BUILD 270: the ground-truth stereo recorder (log only, cfg gtrec, off by default)
#include "Log.h"

// BUILD 245: the mod's own gameplay latch lives in CameraProbe.cpp (the `wgun: ARMED` state). Both are
// plain globals with C linkage there; the experiment only READS them, from the init thread.
extern "C" {
extern uint64_t          grwxr_wgun_skel;    // non-zero only while `wgun: ARMED` (the player's rig and its gun-root node resolved)
extern volatile uint64_t grwxr_wgun_calls;   // SkeletonPostUpdate entries, every skeleton in the world
}

namespace grwxr {
namespace cam2 {

namespace {

// ---------------------------------------------------------------------------
// THE PINS. Every byte here was read from the TU25 GRW.exe ON DISK
// (sha256 1284e4d5aad5f1ca5d2832b6c46707b1d043205e929b5810ffe1caf61cbdf751),
// read-only, 2026-10-05, by RVA -> file offset through its section table
// (scripts C:\Claude\scratch-tu25-render\pin_cam2.py, pin_cam2v2.py, pin2.py, dis_ps2.py). At run
// time the same bytes are compared with the loaded image and ANY mismatch creates nothing and
// installs nothing.
// ---------------------------------------------------------------------------
enum : int {
    kFactory = 0, kDesc, kPose, kAddBody, kAddThunk, kSetSurf,
    // BUILD 245
    kSetVp, kSetVpThunk, kSetVpTail, kSceneModeSet, kSetVpCallThunk,
    kPassGate, kPassCnt, kAtlas, kBindCull, kBindSlot,
    kNumFn
};

struct FnPin {
    const char* name;
    uint32_t    rva;
    uint32_t    len;         // bytes compared (the descriptor getter is 8 bytes long, the rest of its 16 is the next function's lead-in)
    uint8_t     bytes[16];
};

struct Cam2Build {
    uint32_t pe_timestamp;
    uint32_t pe_size_of_image;
    FnPin    fn[kNumFn];
    uint32_t view_global;    // [image+this] = the main view (its only writer is 0x067625C0, REPORT 2/9)
    uint32_t view_vtable;    // the main view's vtable; slot +8 = the AddCamera thunk
    uint32_t renderer_holder;// [[image+this]+0x10] = the Renderer singleton (the census pin, Census.cpp kCensusTU25)
};

constexpr Cam2Build kTU25 = {
    0x6A99768A, 0x185BA000,   // 2026-09-15 Title Update 25, the kTU25 row of GameBuild.cpp
    {
        {"camera factory", 0x0E524F20, 16,
         {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48}},
        {"camera descriptor getter", 0x0D802050, 8,
         {0x48, 0x8B, 0x05, 0x59, 0x71, 0xD1, 0xF6, 0xC3, 0x83, 0xE0, 0x00, 0x66, 0x29, 0xC0, 0x66, 0x2B}},
        {"camera pose setter", 0x0FC2BA70, 16,
         {0x0F, 0x28, 0x02, 0x0F, 0x29, 0x01, 0x0F, 0x28, 0x4A, 0x10, 0x0F, 0x29, 0x49, 0x10, 0x0F, 0x28}},
        {"AddCamera body", 0x0D672670, 16,
         {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48}},
        {"AddCamera thunk (view vtable +8)", 0x01373590, 16,
         {0xE9, 0xDB, 0xF0, 0x2F, 0x0C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
        {"SetViewSurface", 0x0D92B890, 16,
         {0x40, 0x53, 0x55, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xD3, 0x48, 0x89, 0xCE, 0x48, 0x8B}},
        // ---- BUILD 245 ----
        // FUN_14d92b050 SetViewport(cam, float rect[4]): stores the rect at cam+0x314..0x320; when the rect is not
        // (0,0,1,1) sets cam+0x290 = 4 and (*(view+0x30))+0x150 = 4; then tail-calls RecomputeViewport.
        {"SetViewport body (14d92b050)", 0x0D92B050, 16,
         {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0x10, 0x02, 0x48, 0x89, 0xCB, 0x0F, 0x57, 0xC9, 0x0F}},
        {"SetViewport thunk", 0x0138B660, 16,
         {0xE9, 0xEB, 0xF9, 0x59, 0x0C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
        // body+0x35: mov dword [rcx+0x290], 4 ; mov rcx,[rcx+0x748] ; test rcx,rcx  (the mode store, in the very bytes read)
        {"SetViewport mode store (+0x35)", 0x0D92B085, 16,
         {0xC7, 0x81, 0x90, 0x02, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x89, 0x48, 0x07, 0x00}},
        // FUN_14d64b060: mov [rcx+0x150], edx ; ret
        {"view-settings mode setter (14d64b060)", 0x0D64B060, 7,
         {0x89, 0x91, 0x50, 0x01, 0x00, 0x00, 0xC3}},
        // the thunk SetViewport's E8 (body+0x54) calls; it must land on the setter above
        {"SetViewport -> mode-setter thunk", 0x013669E0, 16,
         {0xE9, 0x7B, 0x46, 0x2E, 0x0C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
        // FUN_14e1272c0 (the per-camera pass-list builder): test byte [rbx+0x8C0],4 ; je ; mov rdi,[rbx+0xD90]
        {"pass list: test [rend+0x8C0],4 ; [+0xD90]", 0x0E12772B, 16,
         {0xF6, 0x83, 0xC0, 0x08, 0x00, 0x00, 0x04, 0x74, 0x55, 0x48, 0x8B, 0xBB, 0x90, 0x0D, 0x00, 0x00}},
        // same function: cmp [rdi+0x8570],esi ; jbe   (the number of ProjShadow passes appended)
        {"pass list: cmp [cull+0x8570]", 0x0E12773B, 8,
         {0x39, 0xB7, 0x70, 0x85, 0x00, 0x00, 0x76, 0x46}},
        // FUN_14dfba720 (a ProjShadow pass): mov rcx,[r14+0xB60]  (the projector-shadow atlas, the null that faulted)
        {"ProjShadow pass: mov rcx,[rend+0xB60]", 0x0DFBA82E, 7,
         {0x49, 0x8B, 0x8E, 0x60, 0x0B, 0x00, 0x00}},
        // FUN_14dfb8060 (the camera bind): mov [rdi+0xD90],rbp ; ... mov [rdi+0xD98],ecx
        {"camera bind: mov [rend+0xD90],cull", 0x0DFB80B8, 7,
         {0x48, 0x89, 0xAF, 0x90, 0x0D, 0x00, 0x00}},
        {"camera bind: mov [rend+0xD98],slot", 0x0DFB80CC, 6,
         {0x89, 0x8F, 0x98, 0x0D, 0x00, 0x00}},
    },
    0x04D5B0B0, 0x03A080B0, 0x04D75898,
};

// Struct offsets (REPORT.md section 9, NOTES-second-camera.txt, and the decompiles named above).
constexpr uint32_t kViewCamArr = 0x18, kViewCamCount = 0x22, kViewScene = 0x30;   // view: array ptr, ushort count, settings object
constexpr uint32_t kSceneMode = 0x150;                                            // [[view+0x30]+0x150]: the viewport mode the engine syncs cameras to
constexpr uint32_t kCamMode = 0x290, kCamWmo = 0x2A0, kCamHandle = 0x2A8, kCamSlot = 0x2F4, kCamPrimary = 0x304;
constexpr uint32_t kCamViewport = 0x314, kCamBackPtr = 0x748, kCamSize = 0x7C0;
constexpr uint32_t kDescSize = 0x28;
constexpr uint32_t kSetVpCallOff = 0x54;                                          // the E8 inside SetViewport's body that reaches the mode setter's thunk
// BUILD 245, the renderer (struct offsets, each pinned above).
constexpr uint32_t kRendMask = 0x8C0, kRendAtlas = 0xB60, kRendCull = 0xD90, kRendSlot = 0xD98;
constexpr uint32_t kCamCull = 0x758, kCullNProj = 0x8570;
constexpr uint8_t  kProjBit = 4;

constexpr unsigned long long kMinFrames = 600;   // Present entries before anything is verified (the pins); camera 2 waits for the gate on top of this
constexpr int      kMaxLeaks = 3;                // a creation that is abandoned before AddCamera leaks one camera object; stop after this many
constexpr uint32_t kViewportMode = 4;            // what SetViewport leaves in cam+0x290 and view-settings+0x150 for a non-full rect

// The two rectangles, normalized (x, y, w, h). Camera 0 the left half, camera 2 the right half.
const float kVp0[4] = {0.0f, 0.0f, 0.5f, 1.0f};
const float kVp2[4] = {0.5f, 0.0f, 0.5f, 1.0f};

constexpr uint64_t kWatchMs = 2000;              // recording-only time before camera 2 may be created
constexpr uint32_t kWatchMinRows = 20;           // builder entries recorded in that time, or it waits longer
constexpr uint64_t kGapMs = 3000;                // two gate readings further apart than this are not "continuous"
constexpr uint64_t kRollingDumpMs = 5000;        // the diag file is rewritten this often while recording

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
//   gate : pins verified, nothing installed, waiting for live gameplay to hold for cam2_arm_seconds
//   watch: gate passed, hook installed, RECORDING ONLY for kWatchMs, camera 2 not created
//   armed: creation happens at the next builder entry
enum : int { kIdle = 0, kArmed, kRunning, kFaulted, kRefused, kLost, kGate, kWatch };
const char* const kStateName[] = {"idle", "armed", "RUNNING", "FAULTED", "REFUSED", "LOST", "gate", "watch"};

std::atomic<bool>     g_requested{false};
std::atomic<uint32_t> g_ipd_bits{0x3D8B4396u};      // 0.064f
std::atomic<uint32_t> g_arm_ms{45000u};             // cam2_arm_seconds * 1000
std::atomic<bool>     g_noproj{false};              // cam2_noprojshadow
std::atomic<int>      g_state{kIdle};
std::atomic<uint64_t> g_cam2_ptr{0};                // the keying value CameraProbe reads
std::atomic<uint32_t> g_busy{0};
std::atomic<uint64_t> g_last_surf{0};               // camera 0's surface OBJECT ([handle]) as of the last time camera 2 was synced to it

// ---------------------------------------------------------------------------
// BUILD 247: TRUE STEREO state (cfg cam2_stereo). Everything here is inert while g_stereo_req is false.
// Writers: the cfg thread (req / swap / aspect), the builder callback (run_ok, cam0, warm), the engine's
// selector hooks (the pose latch and the order counters), the present thread (the frame counters and
// rows). Readers: the same threads, and the init thread's 1 Hz log. All relaxed atomics or a seqlock.
// ---------------------------------------------------------------------------
constexpr uint32_t kWarmFrames = 8;          // per-frame updates that must complete, viewports steady, before camera 2 is trusted
constexpr uint32_t kWarmAfterDrift = 4;      // and after a viewport drift (the frame that drifted is not trusted either)
std::atomic<bool>     g_stereo_req{false};   // cfg cam2_stereo
std::atomic<bool>     g_stereo_swap{false};  // cfg cam2_stereo_swap_eyes
std::atomic<uint32_t> g_stereo_aspect_bits{0};   // cfg cam2_stereo_aspect, float bits, 0 = none
std::atomic<bool>     g_run_ok{false};       // RUNNING and both viewports read back as the setter left them at the last builder entry
std::atomic<uint64_t> g_cam0_ptr{0};         // camera 0 while RUNNING
std::atomic<uint32_t> g_warm_left{kWarmFrames};

// The latched pose of camera 2 for the frame the camera selector last composed, a seqlock (single writer
// at a time: g_lat_busy is a try-lock that skips the latch rather than wait, rule 8).
struct PoseLatch {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint64_t> frame{~0ull};
    float                 m[16] = {};
};
PoseLatch             g_lat;
std::atomic<uint32_t> g_lat_busy{0};

// Counters. The engine threads and the present thread add, the init thread reads.
std::atomic<uint32_t> g_sk[5] = {};          // present frames by StereoKind
std::atomic<uint32_t> g_st_tag2{0};          // present frames whose tag said stereo (kind 1 + 2 with a tag + 3 + 4)
std::atomic<uint32_t> g_pose_pub{0};         // stereo_publish_pose calls that wrote camera 2
std::atomic<uint32_t> g_pose_bad{0};         // calls refused because a float was not finite
std::atomic<uint32_t> g_pose_werr{0};        // camera 2 pose writes that faulted
std::atomic<uint32_t> g_mirror_n{0};         // stereo_mirror writes
std::atomic<uint32_t> g_ord_cam0_first{0};   // camera 2's selector call came AFTER camera 0 composed this frame
std::atomic<uint32_t> g_ord_cam2_first{0};   // camera 2's selector call came BEFORE (its pose is one frame old when consumed)
std::atomic<uint32_t> g_c2_reasserts{0};
std::atomic<bool>     g_mirror_fov{true};    // cfg cam2_stereo_mirror_fov
std::atomic<uint32_t> g_fov_mirrored{0}, g_fov_differed{0}, g_fov_skipped{0};
std::atomic<uint32_t> g_proj_half{0}, g_proj_full{0}, g_proj_other{0};   // persp2 calls by aspect: ~0.89 (half), ~1.78 (full), other
std::atomic<uint32_t> g_proj_last_bits{0};
// BUILD 248: the forced vertical fov of a half-viewport world projection, the persp2 override counters, and the
// last placement (tangents per eye) for the 1 Hz line. Floats travel as bits in relaxed atomics.
std::atomic<uint32_t> g_target_fovy_bits{0x40000000u};   // 2.0f until the headset's fovs are known
std::atomic<uint32_t> g_ovr_forced{0}, g_ovr_same{0};
std::atomic<uint32_t> g_ovr_last_in_bits{0};
std::atomic<uint32_t> g_place_bits[4] = {};              // th0 th1 tv0 tv1
std::atomic<uint32_t> g_place_cam{0};                    // bit 0 / bit 1: eye 0 / eye 1 was placed from its camera's own projection
std::atomic<uint32_t> g_place_fallback{0};
std::atomic<int>      g_lag_cfg{0};                      // BUILD 251: the layer pose lag in use, and its counters, for the 1 Hz line
std::atomic<uint32_t> g_lag_used_n{0}, g_lag_miss_n{0};               // frames where an eye fell back to the shared fov
// BUILD 249: which NATIVE fovy values the forced calls carried (key = round(fovy * 1000) + 1, 0 = free), so the log
// names the distinct projections being flattened to the target. Eight slots, relaxed atomics, no lock.
struct NativeSlot { std::atomic<uint32_t> key{0}; std::atomic<uint32_t> n{0}; };
NativeSlot g_native[8];
inline void note_native(float fovy) {
    const uint32_t k = (uint32_t)(fovy * 1000.0f + 0.5f) + 1u;
    for (int i = 0; i < 8; ++i) {
        uint32_t cur = g_native[i].key.load(std::memory_order_relaxed);
        if (cur == 0u) {
            uint32_t exp = 0u;
            if (g_native[i].key.compare_exchange_strong(exp, k, std::memory_order_relaxed)) cur = k; else cur = exp;
        }
        if (cur == k) { g_native[i].n.fetch_add(1, std::memory_order_relaxed); return; }
    }
}
inline uint32_t fbits(float f) { uint32_t b; memcpy(&b, &f, sizeof(b)); return b; }
inline float bitsf(uint32_t b) { float f; memcpy(&f, &b, sizeof(f)); return f; }
std::atomic<const char*> g_first_reason{nullptr};

// The present thread's rows, a single-writer ring read by the init thread under a per-slot seqlock.
struct SRow {
    uint64_t qpc = 0, frame = 0;
    float    pos[2][3] = {}, ori[2][4] = {}, fov[2][4] = {};
    float    cfov = 0, aspect = 0;
    uint32_t eye_w = 0, eye_h = 0, entry = 0;
    int8_t   kind = 0, tag = 0, poses_ok = 0, pad = 0;
};
struct SSlot {
    std::atomic<uint32_t> seq{0};
    SRow                  row;
};
constexpr uint32_t kSCap = 256;              // power of two
constexpr uint32_t kSDump = 120;
SSlot                 g_srows[kSCap];
std::atomic<uint32_t> g_srows_head{0};
const char* const     kKindName[] = {"alt", "BOTH", "mono-left", "not-ready", "failed"};
uint32_t srow_snapshot(SRow* out, uint32_t want);   // defined with the logging below

const Cam2Build* g_b = nullptr;
uint64_t         g_base = 0;
const char*      g_why = "";                        // string literals only; written before the state it explains

// Facts the creation records, plain fields read by the tick thread only after it sees state >= kRunning.
struct Facts {
    uint64_t view = 0, vt = 0, arr = 0, cam0 = 0, cam2 = 0, desc = 0, settings = 0, handle0 = 0;
    uint32_t mode = 0;
    uint32_t count_before = 0, count_after = 0;
    uint32_t step = 0;                              // the last step the creation started (where a fault happened)
    uint32_t surf_called = 0;                       // 1 when step 7 had to call SetViewSurface, 0 when AddCamera's own call already shared the surface
    uint64_t surf0 = 0, surf2_before = 0;           // camera 0's surface object, and camera 2's before step 7 (0 = none)
    // BUILD 245: the viewports, by the engine's setter
    float    orig_vp0[4] = {0, 0, 1, 1};            // camera 0's rect before the experiment touched it
    uint32_t orig_mode0 = 0, orig_scene = 0;        // cam0+0x290 and view-settings+0x150 before
    float    rb_vp0[4] = {}, rb_vp2[4] = {};        // read back right after the two SetViewport calls
    uint32_t rb_m0 = 0xFFFFFFFFu, rb_m2 = 0xFFFFFFFFu;
    uint32_t vp_changed = 0;                        // 1 once camera 0's rect / modes were handed to the engine setter
    uint64_t created_frame = 0;                     // Present entries at creation
    uint32_t created_entry = 0;                     // builder entry number at creation
};
Facts g_f;

std::atomic<uint32_t> g_entries{0};                 // builder entries seen by the callback
std::atomic<uint32_t> g_waits{0};                   // entries spent waiting for the view / camera 0 / handle
std::atomic<uint32_t> g_frames_ok{0};               // per-frame updates completed
std::atomic<uint32_t> g_resyncs{0};                 // surface handle re-syncs
std::atomic<uint32_t> g_badpose{0};                 // frames skipped because camera 0's pose was not finite
std::atomic<uint32_t> g_handle_bad{0};              // frames where camera 0's handle was not valid for a re-sync
std::atomic<uint32_t> g_leaks{0};
std::atomic<uint32_t> g_mismatch_run{0};
const char*           g_wait_why = "";              // the last reason the callback is waiting (literals only; diagnostic)

// BUILD 245: viewport drift. The callback counts and stores; the tick thread prints.
std::atomic<uint32_t> g_drift_n{0};                 // frames on which a viewport or mode was not what the setter left
std::atomic<uint32_t> g_reapply_n{0};               // engine-setter calls made because of drift
std::atomic<uint32_t> g_drift_last[10] = {};        // cam0 rect bits x4, cam0 mode, cam2 rect bits x4, cam2 mode (as read at the last drift)
std::atomic<uint32_t> g_drift_who{0};               // bit 0 cam0, bit 1 cam2 (last drift)
std::atomic<uint32_t> g_restored{0};                // 0 not attempted, 1 camera 0 restored, 2 skipped (camera 0 not provably live)

// BUILD 245: cam2_noprojshadow
std::atomic<uint32_t> g_np_count{0}, g_np_fail{0};
std::atomic<uint32_t> g_np_first{0xFFFFFFFFu}, g_np_last{0xFFFFFFFFu};   // the byte at renderer+0x8C0 BEFORE the clear

// ---------------------------------------------------------------------------
// guarded memory
// ---------------------------------------------------------------------------
bool query(uint64_t a, size_t n, bool write) {
    if (a < 0x10000 || a + n < a || a + n > 0x00007FFFFFFE0000ull) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD rd_ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD wr_ok = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & (write ? wr_ok : rd_ok))) return false;
    const uint64_t hi = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (a + n > hi) {
        // The range may straddle two regions of the same heap block; check the tail separately.
        return query(hi, (size_t)(a + n - hi), write);
    }
    return true;
}
bool readable(uint64_t a, size_t n) { return query(a, n, false); }
bool writable(uint64_t a, size_t n) { return query(a, n, true); }

bool rd(uint64_t a, void* out, size_t n) {
    if (!readable(a, n)) return false;
    __try {
        memcpy(out, (const void*)(uintptr_t)a, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> bool rdv(uint64_t a, T* out) { return rd(a, out, sizeof(T)); }

bool wr(uint64_t a, const void* src, size_t n) {
    if (!writable(a, n)) return false;
    __try {
        memcpy((void*)(uintptr_t)a, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The separation is clamped to [-1, 1] metres wherever it is parsed and wherever it is used. NaN and
// infinity are not a value: they fall back to the 0.064 default.
float clamp_ipd(float f) {
    if (!std::isfinite(f)) return 0.064f;
    if (f < -1.0f) return -1.0f;
    if (f > 1.0f) return 1.0f;
    return f;
}

float ipd() {
    const uint32_t b = g_ipd_bits.load(std::memory_order_relaxed);
    float f;
    memcpy(&f, &b, sizeof(f));
    return clamp_ipd(f);
}

// ---------------------------------------------------------------------------
// engine entry points. Built only from VERIFIED pins, only after verify_pins() passed.
// ---------------------------------------------------------------------------
typedef uint64_t (*FactoryFn)(uint64_t, uint64_t, uint64_t);
typedef uint64_t (*DescFn)();
typedef void     (*PoseFn)(void* cam, const void* mat);
typedef void     (*AddFn)(uint64_t view, uint64_t cam);
typedef void     (*SetSurfFn)(uint64_t cam, uint64_t* slot);
typedef void     (*SetVpFn)(uint64_t cam, const float* rect);   // FUN_14d92b050

inline uint64_t fn_addr(int i) { return g_base + g_b->fn[i].rva; }

// The engine's own viewport setter. READ FROM THE DECOMPILE AND THE PINNED BYTES, not assumed:
//   stores rect[0..3] at cam+0x314..0x320 (an unaligned 16-byte load and store);
//   if rect is not exactly (0,0,1,1): cam+0x290 = 4, and when cam+0x748 (the view) is set,
//     FUN_14d64b060(*(view+0x30), 4), i.e. view-settings+0x150 = 4 (the mode every camera of the view syncs to);
//   then tail-calls RecomputeViewport (FUN_14d936640), which leaves +0x314 alone in mode 4 and rewrites it
//   to the full (0,0,1,1) in mode 0. Caller must be inside an SEH-guarded region.
inline void setvp(uint64_t cam, const float* rect) { ((SetVpFn)fn_addr(kSetVp))(cam, rect); }

// ---------------------------------------------------------------------------
// readiness: pure reads, no engine calls. The callback runs this every entry until it passes.
// ---------------------------------------------------------------------------
enum : int { kWait = 0, kReady = 1, kRefuse = 2 };

struct Probe {
    uint64_t view = 0, vt = 0, arr = 0, cam0 = 0, handle = 0, settings = 0;
    uint16_t count = 0;
    uint32_t mode = 0;
    const char* why = "";
};

bool handle_valid(uint64_t P) {
    uint64_t raw = 0;
    int32_t m = 0;
    if (!readable(P, 0x10)) return false;
    if (!rdv(P, &raw) || !rdv(P + 0xC, &m)) return false;
    return raw != 0 && m < 0;   // the engine's own validity mask: the int at +0xC is negative when the handle is live
}

// The surface OBJECT a handle block refers to ([handle], what SetViewSurface reads as S), or 0 when the
// handle is not valid.
uint64_t surface_of(uint64_t H) {
    uint64_t s = 0;
    if (!H || !handle_valid(H) || !rdv(H, &s)) return 0;
    return s;
}

int probe_ready(Probe& p) {
    p = Probe();
    if (!rdv(g_base + g_b->view_global, &p.view) || !p.view) { p.why = "no main view yet"; return kWait; }
    if (!rdv(p.view, &p.vt)) { p.why = "main view not readable"; return kWait; }
    if (p.vt != g_base + g_b->view_vtable) {
        // A view under construction can briefly carry another vtable; only a mismatch that persists is a refusal.
        p.why = "main view vtable is not the pinned one";
        return (g_mismatch_run.fetch_add(1, std::memory_order_relaxed) >= 180) ? kRefuse : kWait;
    }
    g_mismatch_run.store(0, std::memory_order_relaxed);
    if (!rdv(p.view + kViewCamCount, &p.count)) { p.why = "view camera count not readable"; return kWait; }
    if (p.count == 0) { p.why = "view has no camera yet"; return kWait; }
    if (p.count >= 2) { p.why = "view already holds 2 or more cameras (refusing to add a third)"; return kRefuse; }
    if (!rdv(p.view + kViewCamArr, &p.arr) || !p.arr) { p.why = "view camera array not readable"; return kWait; }
    if (!rdv(p.arr, &p.cam0) || !p.cam0) { p.why = "camera 0 slot is empty"; return kWait; }
    if (!readable(p.cam0, kCamSize) || !writable(p.cam0, kCamSize)) { p.why = "camera 0 is not a readable, writable object"; return kWait; }
    uint64_t back = 0;
    if (!rdv(p.cam0 + kCamBackPtr, &back) || back != p.view) { p.why = "camera 0 does not point back at the view yet"; return kWait; }
    if (!rdv(p.cam0 + kCamHandle, &p.handle) || !p.handle || !handle_valid(p.handle)) { p.why = "camera 0 surface handle is not valid yet"; return kWait; }
    if (!rdv(p.view + kViewScene, &p.settings) || !p.settings) { p.why = "view settings object missing"; return kWait; }
    if (!rdv(p.settings + kSceneMode, &p.mode)) { p.why = "view settings mode not readable"; return kWait; }
    return kReady;
}

// ---------------------------------------------------------------------------
// BUILD 245: THE GAMEPLAY GATE. Init thread only (tick), reads only.
//
// WHY. Build 244 armed at Present #600 with the process 27 s old: the title or a loading screen
// (984 fps, no `wgun: ARMED` in the log). Three workers then took an access violation in the game's
// ProjShadow pass. Whether the title was the cause is unresolved, so camera 2 is never created
// there. THE SIGNAL CHOSEN is the mod's own `wgun: ARMED` state, grwxr_wgun_skel != 0, which is
// published only while the skeleton census has picked a DRAWN gun on the player's rig and resolved
// its gun-root node (CameraProbe.cpp, the wgun block of drain()). It is set false again at once
// when the pick is lost: the title (`wskel: no pick`), a loading screen, the player without a gun.
// [VERIFIED] in logs grwxr-16560 and grwxr-12972 (TU25, 2026-10-05): `wskel: no pick` at the title,
// `wgun: ARMED node=10` from the first second of gameplay. It is the same signal the project's
// own rule "a run is valid only while wgun: ARMED prints" uses.
// ON TOP OF IT, every tick, all of these must hold, continuously, for cam2_arm_seconds:
//   - the frame counter advanced at 5 or more per second (the game is presenting);
//   - grwxr_wgun_calls advanced (the engine is running skeleton updates: the world is ticking, not paused or loading);
//   - the main view is the pinned one with EXACTLY ONE camera, camera 0 is a mode-0 camera with a valid surface,
//     and the view settings mode reads 0 (nothing of ours has touched them);
//   - g_player_cam (captured by the selector hook) is non-zero, equals camera 0, and has not changed.
// NOT USED: a bare "g_player_cam is valid" (it is captured at the title too: the same camera object
// lives from the title through gameplay, census-16560.txt camera 0x207832FBE60 = fovscene cam), and
// frame rate (984 fps at the title on a desktop, but a headset run vsyncs there too).
// A requirement the owner's cfg switches off (wgun = 0 or wskel = 0) keeps this gate shut forever;
// that is the fail-closed direction and the tick log says why.
// ---------------------------------------------------------------------------
struct Gate {
    bool     primed = false, prev_ok = false;
    uint64_t last_ms = 0, last_frames = 0, last_calls = 0, last_pcam = 0;
    uint64_t accum_ms = 0;               // how long the latch has held continuously
    uint64_t start_frames = 0, start_calls = 0;   // counters when the current continuous hold began
    uint64_t skel = 0, pcam = 0, cam0 = 0, view = 0;   // the last good reading, for the arming-decision log
    const char* why = "";
    const char* last_logged = nullptr;
    uint32_t last_log_tick = 0;
};
Gate g_gate;

// 0 = holds, 1 = not (g_gate.why says why), 2 = refuse the experiment (g_why says why)
int gate_eval(uint64_t now_ms) {
    Gate& g = g_gate;
    const uint64_t frames = d3d11::frame_count();
    const uint64_t calls = grwxr_wgun_calls;
    const uint64_t skel = grwxr_wgun_skel;
    const uint64_t pcam = ::grwxr::camera::player_camera();
    const uint64_t dt = g.primed ? now_ms - g.last_ms : 0;
    const uint64_t df = g.primed ? frames - g.last_frames : 0;
    const bool prime_now = !g.primed;
    const bool calls_moved = calls != g.last_calls;
    const bool pcam_moved = g.last_pcam != 0 && pcam != g.last_pcam;
    g.primed = true;
    g.last_ms = now_ms; g.last_frames = frames; g.last_calls = calls; g.last_pcam = pcam;

    bool ok = false;
    int  ret = 1;
    Probe p;
    if (prime_now) g.why = "first reading";
    else if (dt > kGapMs) g.why = "two readings more than 3 s apart (the process stalled)";
    else if (df * 1000ull < 5ull * dt) g.why = "the game is not presenting (under 5 frames per second)";
    else if (skel == 0) g.why = "wgun is not ARMED (no drawn gun on the player's rig: title, loading screen, or no weapon)";
    else if (!calls_moved) g.why = "skeleton post-update calls are not advancing (the world is not ticking)";
    else if (pcam == 0) g.why = "no player camera captured by the selector hook yet";
    else if (pcam_moved) g.why = "the player camera pointer changed";
    else {
        const int pr = probe_ready(p);
        if (pr == kRefuse) { g_why = p.why; ret = 2; }
        else if (pr != kReady) g.why = p.why;
        else if (p.count != 1) g.why = "the main view does not hold exactly one camera";
        else if (p.mode != 0) g.why = "the view settings mode is not 0";
        else {
            uint32_t m0 = 0xFFFFFFFFu;
            if (!rdv(p.cam0 + kCamMode, &m0) || m0 != 0) g.why = "camera 0 is not a mode-0 camera";
            else if (pcam != p.cam0) g.why = "the player camera is not the main view's camera 0";
            else { ok = true; g.skel = skel; g.pcam = pcam; g.cam0 = p.cam0; g.view = p.view; }
        }
    }
    if (ret == 2) { g.accum_ms = 0; g.prev_ok = false; return 2; }
    if (ok) {
        if (g.prev_ok) g.accum_ms += dt;
        else { g.accum_ms = 0; g.start_frames = frames; g.start_calls = calls; }
        g.why = "holds";
    } else {
        g.accum_ms = 0;
    }
    g.prev_ok = ok;
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// BUILD 245: THE READ-ONLY DISCRIMINATOR. One row per builder entry, recorded by the callback into
// this preallocated ring (no logging, no lock, no allocation: rule 8). The tick thread (never a game
// worker) turns the last kDumpRows into GRWVR\cam2diag-<pid>.txt.
//
// ROW LAYOUT (DiagRow, 96 bytes). All values are what the callback read at the ENTRY of the graph-frame
// builder, i.e. BEFORE the builder body of that frame, so the renderer fields still hold the PREVIOUS
// frame's last-bound camera (the bind, FUN_14dfb8060, rewrites +0xD90/+0xD98 for each camera).
//   qpc, frame (Present entries), entry (builder entry number), tid (the thread that ran the builder)
//   state       the experiment's state at that entry (watch / armed / RUNNING)
//   renderer    [[image+0x04D75898]+0x10]
//   b8c0        BYTE at renderer+0x8C0 as read BEFORE any cam2_noprojshadow clear (bit 4 = ProjShadow passes allowed)
//   b60         QWORD at renderer+0xB60, the projector-shadow atlas (NULL is the build 244 fault)
//   d90, d98    QWORD renderer+0xD90 (the bound camera's culling object), INT renderer+0xD98 (its slot)
//   cull0/cull2 camera+0x758 of camera 0 / camera 2 (0 = no such camera yet)
//   n0 / n2     INT at cull+0x8570 for camera 0 / camera 2: how many ProjShadow passes that camera's culling asks for
//   mode0/mode2 camera+0x290 of camera 0 / camera 2
//   camcnt      cameras in the main view
//   ps / pslive bit i = ProjShadow<i> went through BuildCmdList since the previous row (census hook 2); pslive says
//               whether hook 2 was recording at all (census window open). pslive 0 means NOT OBSERVED.
//   flags       1 renderer, 2 b8c0, 4 b60, 8 d90, 16 d98, 32 n0, 64 n2, 128 mode0, 256 mode2 read OK;
//               512 the noprojshadow clear was applied to this row's byte
// An unread INT field is stored as 0xFFFFFFFF and printed as -1.
// ---------------------------------------------------------------------------
struct DiagRow {
    uint64_t qpc, frame, renderer, b60, d90, cull0, cull2;
    uint32_t entry, tid, d98, n0, n2, mode0, mode2, row;
    uint16_t flags;
    uint8_t  state, b8c0, ps, pslive, camcnt, pad;
};
struct DiagSlot {
    std::atomic<uint32_t> seq;   // row number + 1 once published, 0 while it is being written
    DiagRow               row;
};
constexpr uint32_t kDiagCap = 512;     // power of two
constexpr uint32_t kDumpRows = 300;
DiagSlot              g_diag[kDiagCap];
std::atomic<uint32_t> g_diag_head{0};  // rows ever written
uint64_t              g_qpc_first = 0; // qpc of row 0
std::atomic<uint32_t> g_dumping{0};
std::atomic<uint32_t> g_dump_n{0};
// running counts over EVERY row, not just the last 300 (callback writes, tick reads)
std::atomic<uint32_t> g_c_gate{0}, g_c_b60null{0}, g_c_gate_b60null{0};
std::atomic<uint32_t> g_first_null_row{0xFFFFFFFFu}, g_first_null_state{0xFFFFFFFFu};
std::atomic<uint64_t> g_first_null_frame{0};

void cams_now(uint64_t* c0, uint64_t* c2, uint16_t* cnt) {
    *c0 = *c2 = 0;
    *cnt = 0;
    uint64_t view = 0, arr = 0;
    if (!rdv(g_base + g_b->view_global, &view) || !view) return;
    if (!rdv(view + kViewCamCount, cnt)) { *cnt = 0; return; }
    if (*cnt == 0 || !rdv(view + kViewCamArr, &arr) || !arr) return;
    rdv(arr, c0);
    if (*cnt >= 2) rdv(arr + 8, c2);
}

// Records one row. allow_clear is true only in kRunning, and only then may cam2_noprojshadow clear bit 4 of
// the byte at renderer+0x8C0 (pinned: the pass-list builder tests exactly that bit, kPassGate). The row keeps the
// value BEFORE the clear.
void diag_record(int st, bool allow_clear) {
    DiagRow r;
    memset(&r, 0, sizeof(r));
    r.d98 = r.n0 = r.n2 = r.mode0 = r.mode2 = 0xFFFFFFFFu;
    const uint32_t n = g_diag_head.load(std::memory_order_relaxed);
    r.row = n;
    r.entry = g_entries.load(std::memory_order_relaxed);
    r.state = (uint8_t)st;
    r.frame = d3d11::frame_count();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    r.qpc = (uint64_t)q.QuadPart;
    r.tid = GetCurrentThreadId();
    if (n == 0) g_qpc_first = r.qpc;

    uint64_t holder = 0, rend = 0;
    if (rdv(g_base + g_b->renderer_holder, &holder) && holder && rdv(holder + 0x10, &rend) && rend) {
        r.renderer = rend;
        r.flags |= 1;
        uint8_t b = 0;
        if (rdv(rend + kRendMask, &b)) {
            r.flags |= 2;
            r.b8c0 = b;
            if (allow_clear && g_noproj.load(std::memory_order_relaxed) && (b & kProjBit)) {
                const uint8_t nb = (uint8_t)(b & ~kProjBit);
                if (wr(rend + kRendMask, &nb, 1)) {
                    r.flags |= 512;
                    if (g_np_count.fetch_add(1, std::memory_order_relaxed) == 0) g_np_first.store(b, std::memory_order_relaxed);
                    g_np_last.store(b, std::memory_order_relaxed);
                } else {
                    g_np_fail.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (rdv(rend + kRendAtlas, &r.b60)) r.flags |= 4;
        if (rdv(rend + kRendCull, &r.d90)) r.flags |= 8;
        if (rdv(rend + kRendSlot, &r.d98)) r.flags |= 16;
    }
    uint64_t c0 = 0, c2 = 0;
    uint16_t cnt = 0;
    cams_now(&c0, &c2, &cnt);
    r.camcnt = (uint8_t)std::min<uint16_t>(cnt, 255);
    if (c0) {
        if (rdv(c0 + kCamCull, &r.cull0) && r.cull0 && rdv(r.cull0 + kCullNProj, &r.n0)) r.flags |= 32;
        if (rdv(c0 + kCamMode, &r.mode0)) r.flags |= 128;
    }
    if (c2) {
        if (rdv(c2 + kCamCull, &r.cull2) && r.cull2 && rdv(r.cull2 + kCullNProj, &r.n2)) r.flags |= 64;
        if (rdv(c2 + kCamMode, &r.mode2)) r.flags |= 256;
    }
    bool live = false;
    r.ps = (uint8_t)census::projshadow_take(&live);
    r.pslive = live ? 1 : 0;

    // running counts over every row
    const bool gate_set = (r.flags & 2) && (r.b8c0 & kProjBit);
    const bool atlas_null = (r.flags & 4) && r.b60 == 0;
    if (gate_set) g_c_gate.fetch_add(1, std::memory_order_relaxed);
    if (atlas_null) g_c_b60null.fetch_add(1, std::memory_order_relaxed);
    if (gate_set && atlas_null && g_c_gate_b60null.fetch_add(1, std::memory_order_relaxed) == 0) {
        g_first_null_frame.store(r.frame, std::memory_order_relaxed);
        g_first_null_state.store((uint32_t)st, std::memory_order_relaxed);
        g_first_null_row.store(n, std::memory_order_relaxed);
    }

    DiagSlot& s = g_diag[n & (kDiagCap - 1)];
    s.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.row = r;
    s.seq.store(n + 1, std::memory_order_release);
    g_diag_head.store(n + 1, std::memory_order_release);
}

// Copies the last `want` published rows (oldest first) into out. Rows being rewritten are skipped.
uint32_t diag_snapshot(DiagRow* out, uint32_t want, uint32_t* first_row) {
    const uint32_t head = g_diag_head.load(std::memory_order_acquire);
    const uint32_t cnt = std::min<uint32_t>(std::min<uint32_t>(head, kDiagCap - 8), want);
    uint32_t k = 0;
    *first_row = head - cnt;
    for (uint32_t i = head - cnt; i < head; ++i) {
        DiagSlot& s = g_diag[i & (kDiagCap - 1)];
        if (s.seq.load(std::memory_order_acquire) != i + 1) continue;
        DiagRow tmp = s.row;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.seq.load(std::memory_order_relaxed) != i + 1) continue;
        out[k++] = tmp;
    }
    return k;
}

// The dump. Init thread (or DLL detach) only: it opens a file. Reads the ring; writes nothing the engine owns.
void dump_diag(const char* reason) {
    if (g_diag_head.load(std::memory_order_acquire) == 0) return;
    if (g_dumping.exchange(1, std::memory_order_acquire) != 0) return;
    static DiagRow rows[kDumpRows];
    uint32_t first = 0;
    const uint32_t nrows = diag_snapshot(rows, kDumpRows, &first);

    const std::wstring path = log::data_dir() + L"\\cam2diag-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        LOG_ERROR("cam2: could not open the diag file (err %lu).", GetLastError());
        g_dumping.store(0, std::memory_order_release);
        return;
    }
    static char fbuf[1 << 16];
    setvbuf(f, fbuf, _IOFBF, sizeof(fbuf));
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    const double qpf = (double)qf.QuadPart > 0 ? (double)qf.QuadPart : 1.0;
    const int st = g_state.load(std::memory_order_acquire);

    uint32_t n_gate = 0, n_null = 0, n_gate_null = 0, n_ps = 0, n_live = 0, n_c0 = 0, n_c2 = 0, n_cleared = 0, by_state[8] = {};
    for (uint32_t i = 0; i < nrows; ++i) {
        const DiagRow& r = rows[i];
        const bool gs = (r.flags & 2) && (r.b8c0 & kProjBit);
        const bool an = (r.flags & 4) && r.b60 == 0;
        if (gs) ++n_gate;
        if (an) ++n_null;
        if (gs && an) ++n_gate_null;
        if (r.ps) ++n_ps;
        if (r.pslive) ++n_live;
        if ((r.flags & 32) && r.n0 > 0 && r.n0 != 0xFFFFFFFFu) ++n_c0;
        if ((r.flags & 64) && r.n2 > 0 && r.n2 != 0xFFFFFFFFu) ++n_c2;
        if (r.flags & 512) ++n_cleared;
        if (r.state < 8) ++by_state[r.state];
    }

    fprintf(f, "GRW-VR cam2 DIAGNOSTICS, build 245, observation only. pid %lu. Written by the init thread, never by a game worker.\n",
            GetCurrentProcessId());
    fprintf(f, "reason: %s\n", reason ? reason : "");
    fprintf(f, "state now: %s (%s); builder entries %u, per-frame updates %u; rows ever recorded %u, in this file %u (the last %u are kept)\n",
            kStateName[st >= 0 && st < 8 ? st : 0], g_why, g_entries.load(), g_frames_ok.load(), g_diag_head.load(), nrows, kDumpRows);
    fprintf(f, "frames now: %llu; dump #%u\n", (unsigned long long)d3d11::frame_count(), g_dump_n.fetch_add(1) + 1);
    fprintf(f, "created: frame %llu, builder entry %u; view 0x%llX cam0 0x%llX cam2 0x%llX\n",
            (unsigned long long)g_f.created_frame, g_f.created_entry, (unsigned long long)g_f.view, (unsigned long long)g_f.cam0,
            (unsigned long long)g_f.cam2);
    fprintf(f, "viewports: originals cam0 rect (%.3f %.3f %.3f %.3f) mode %u, view-settings mode %u; after SetViewport read back cam0 (%.3f %.3f %.3f %.3f) mode %u, cam2 (%.3f %.3f %.3f %.3f) mode %u; "
               "drift frames %u, setter re-applies %u\n",
            (double)g_f.orig_vp0[0], (double)g_f.orig_vp0[1], (double)g_f.orig_vp0[2], (double)g_f.orig_vp0[3], g_f.orig_mode0, g_f.orig_scene,
            (double)g_f.rb_vp0[0], (double)g_f.rb_vp0[1], (double)g_f.rb_vp0[2], (double)g_f.rb_vp0[3], g_f.rb_m0,
            (double)g_f.rb_vp2[0], (double)g_f.rb_vp2[1], (double)g_f.rb_vp2[2], (double)g_f.rb_vp2[3], g_f.rb_m2,
            g_drift_n.load(), g_reapply_n.load());
    fprintf(f, "noprojshadow: %s; clears %u, failed writes %u, byte before the first clear 0x%02X, before the last 0x%02X\n",
            g_noproj.load() ? "ON" : "off", g_np_count.load(), g_np_fail.load(), g_np_first.load() & 0xFFu, g_np_last.load() & 0xFFu);
    fprintf(f, "ALL rows ever (not just this file): ProjShadow gate set (8c0 bit 4) %u, atlas null %u, BOTH %u; first both-row: #%d at frame %llu in state %s\n",
            g_c_gate.load(), g_c_b60null.load(), g_c_gate_b60null.load(),
            g_first_null_row.load() == 0xFFFFFFFFu ? -1 : (int)g_first_null_row.load(), (unsigned long long)g_first_null_frame.load(),
            g_first_null_state.load() < 8 ? kStateName[g_first_null_state.load()] : "none");
    fprintf(f, "THIS FILE (%u rows): gate set %u, atlas null %u, both %u; rows with a ProjShadow descriptor seen in BuildCmdList %u (hook 2 recording on %u rows); "
               "cam0 asks for ProjShadow passes on %u rows, cam2 on %u; noprojshadow clears %u; rows by state: watch %u armed %u RUNNING %u\n",
            nrows, n_gate, n_null, n_gate_null, n_ps, n_live, n_c0, n_c2, n_cleared, by_state[kWatch], by_state[kArmed], by_state[kRunning]);
    fprintf(f, "READING IT: build 244 faulted when a ProjShadow pass ran with the atlas (b60) NULL. 'both' counts the rows where the pass-list gate was open and the atlas was\n"
               "null. If it is non-zero BEFORE camera 2 exists (state watch/armed), the title/loading theory holds. If it first appears in state RUNNING, camera 2 is implicated.\n"
               "Row fields are read at builder ENTRY (before the body), so d90/d98 still hold the previous frame's last-bound camera.\n");
    fprintf(f, "LEGEND: row=index in ring, ent=builder entry, frame=Present entries, t_ms=since row 0, tid=builder thread, st=state, fl=flags(hex: 1 rend 2 8c0 4 b60 8 d90 10 d98 20 n0 40 n2 80 mode0 100 mode2 200 noproj-cleared),\n"
               "  8c0=byte renderer+0x8C0 BEFORE any clear (bit 4 = ProjShadow allowed), b60=atlas qword, d90=bound camera culling obj, d98=its slot, n0/n2=ProjShadow count at cull+0x8570 for cam0/cam2 (-1 unread),\n"
               "  ps=mask of ProjShadow0..7 seen in BuildCmdList since the previous row, 'L' = census hook 2 was recording (else '-' = NOT OBSERVED), m0/m2=camera+0x290, cc=cameras in the view\n");
    fprintf(f, "%5s %5s %10s %9s %6s %-7s %3s %3s %-14s %-14s %-14s %4s %5s %5s %4s %3s %3s %2s\n",
            "row", "ent", "frame", "t_ms", "tid", "st", "fl", "8c0", "renderer", "b60", "d90", "d98", "n0", "n2", "ps", "m0", "m2", "cc");
    for (uint32_t i = 0; i < nrows; ++i) {
        const DiagRow& r = rows[i];
        const double t = (double)(int64_t)(r.qpc - g_qpc_first) * 1000.0 / qpf;
        fprintf(f, "%5u %5u %10llu %9.1f %6u %-7s %03X %02X%c 0x%012llX 0x%012llX 0x%012llX %4d %5d %5d %02X%c %3d %3d %2u\n",
                r.row, r.entry, (unsigned long long)r.frame, t, r.tid, kStateName[r.state < 8 ? r.state : 0], (unsigned)r.flags,
                r.b8c0, (r.flags & 2) ? ' ' : '?', (unsigned long long)r.renderer, (unsigned long long)r.b60, (unsigned long long)r.d90,
                (int)r.d98, (int)r.n0, (int)r.n2, r.ps, r.pslive ? 'L' : '-', (int)r.mode0, (int)r.mode2, (unsigned)r.camcnt);
    }
    // BUILD 247: the present thread's stereo rows ("stereo2: frame"), the last kSDump, oldest first.
    if (g_stereo_req.load(std::memory_order_relaxed) || g_srows_head.load(std::memory_order_acquire) != 0) {
        static SRow srows[kSDump];
        const uint32_t ns = srow_snapshot(srows, kSDump);
        fprintf(f, "\nstereo2: frame rows (build 247), the last %u presented frames while cam2_stereo was requested. "
                   "kind: alt = alternate-eye path, BOTH = both eyes fresh from the two halves, mono-left = left half to both eyes, not-ready, failed. "
                   "tag: -1 none, 0/1 alternate eye, 2 stereo. L pos / R pos are the two eyes' xrLocateViews positions for that frame (reference space, metres); "
                   "cfov is the content vertical fov, eye WxH the half the picture was cut from, aspect the content aspect used to place it.\n", ns);
        fprintf(f, "%10s %9s %-10s %3s %8s %9s %6s %7s %-33s %-33s\n", "frame", "t_ms", "kind", "tag", "cfov", "eye WxH", "aspect", "entry", "L pos", "R pos");
        for (uint32_t i = 0; i < ns; ++i) {
            const SRow& r = srows[i];
            const double t = (double)(int64_t)(r.qpc - g_qpc_first) * 1000.0 / qpf;
            char w[24];
            _snprintf_s(w, sizeof(w), _TRUNCATE, "%ux%u", r.eye_w, r.eye_h);
            fprintf(f, "%10llu %9.1f %-10s %3d %8.4f %9s %6.3f %7u (%8.4f %8.4f %8.4f)   (%8.4f %8.4f %8.4f)%s\n",
                    (unsigned long long)r.frame, t, kKindName[r.kind >= 0 && r.kind < 5 ? r.kind : 0], (int)r.tag, (double)r.cfov, w,
                    (double)r.aspect, r.entry, (double)r.pos[0][0], (double)r.pos[0][1], (double)r.pos[0][2],
                    (double)r.pos[1][0], (double)r.pos[1][1], (double)r.pos[1][2], r.poses_ok ? "" : "  POSES-INVALID");
        }
    }
    fclose(f);
    LOG_INFO("cam2: diagnostics written to %ls (%u rows; reason: %s). Atlas-null with the pass gate open: %u rows ever, %u in this file.",
             path.c_str(), nrows, reason ? reason : "", g_c_gate_b60null.load(), n_gate_null);
    g_dumping.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// the field list of the engine's own camera clone (FUN_14d7effc0), camera 0 -> camera 2.
// Plain value copies; nothing here is a pointer the engine owns. Not copied, on purpose:
//   +0x000..0x03F  the pose, written with the engine's pose setter instead
//   +0x290         set from the view's settings object (recipe step 3)
//   +0x2A0         the weak matrix pointer (camera 2's pose must stay authoritative at +0)
//   +0x2A8         the refcounted surface handle, shared with SetViewSurface (recipe step 5)
//   +0x2F4         the renderer slot, assigned by the graph builder every frame
//   +0x304         the primary flag, forced to 0 (recipe step 3)
//   +0x306..0x308  per-frame TAA / viewport-changed state
//   +0x7xx         every owner pointer: ViewTasks, frustum, culling, lock, view
// ---------------------------------------------------------------------------
struct Range { uint32_t off, len; };
constexpr Range kCopy[] = {
    {0x040, 0x220},                 // +0x040..0x25F (the clone copies qwords 0..0x4B)
    {0x260, 0x020},                 // +0x260..0x27F
    {0x280, 0x010},                 // +0x280..0x28F
    {0x294, 0x008},                 // +0x294, +0x298
    {0x2B0, 0x038},                 // +0x2B0..0x2E7: fov, near, far, aspect, render scale, skew, +0x2CC..0x2E7
    {0x2E8, 0x004},
    {0x2FC, 0x008},                 // +0x2FC, +0x300
    {0x305, 0x001},
    {0x309, 0x004},                 // +0x309..0x30C
    {0x30E, 0x006},                 // +0x30E..0x313
    {0x314, 0x010},                 // the normalized viewport (replaced by the engine setter below, copied for completeness)
    {0x324, 0x010},                 // the normalized frustum window (default 0,0,1,1)
};

// ---------------------------------------------------------------------------
// creation. ONE attempt, SEH-wrapped by create_guarded().
// ---------------------------------------------------------------------------
enum : int { kOk = 0, kRetry = 1, kRefusedCreate = 2, kFailedCreate = 3, kFault = -1 };

int create_inner(Facts* c, const Probe& p) {
    c->view = p.view; c->vt = p.vt; c->arr = p.arr; c->cam0 = p.cam0;
    c->settings = p.settings; c->mode = p.mode; c->handle0 = p.handle;
    c->count_before = p.count;

    c->step = 1;   // the descriptor
    const uint64_t desc = ((DescFn)fn_addr(kDesc))();
    c->desc = desc;
    uint32_t dsz = 0;
    if (!rdv(desc + kDescSize, &dsz) || dsz != kCamSize) { g_why = "camera descriptor size is not 0x7C0"; return kRefusedCreate; }

    c->step = 2;   // the factory: one new Camera object, constructed by the engine (allocates ViewTasks, culling, frustum, lock)
    const uint64_t cam2 = ((FactoryFn)fn_addr(kFactory))(0, 0, desc);
    c->cam2 = cam2;
    if (!cam2 || !readable(cam2, kCamSize) || !writable(cam2, kCamSize) || (cam2 & 15) != 0) {
        g_why = "the factory returned an unusable camera pointer";
        return kFailedCreate;
    }
    // KEY FIRST. AddCamera recomputes the new camera, and the mod's player-camera hooks must not mistake it
    // for the player: its +0x290 reads 0 once it is synced to the view. On TU25 the on_calc_mvp hook is NOT
    // installed (CameraProbe.cpp install table); the selector stand-in (kSelectorProbe) is the hook that
    // runs on this build, and it skips this pointer. Either way the key has to be published before AddCamera.
    g_cam2_ptr.store(cam2, std::memory_order_release);

    c->step = 3;   // the pose: camera 0's effective pose through the engine's pose setter
    alignas(16) float mat[16];
    uint64_t wmo = 0;
    const uint64_t srcp = (rdv(p.cam0 + kCamWmo, &wmo) && wmo && readable(wmo, 0x40)) ? wmo : p.cam0;
    if (!rd(srcp, mat, sizeof(mat))) { g_why = "camera 0 pose not readable"; return kFailedCreate; }
    ((PoseFn)fn_addr(kPose))((void*)(uintptr_t)cam2, mat);

    c->step = 4;   // the configuration, field for field as the engine's clone does it
    for (const Range& r : kCopy) {
        uint8_t tmp[0x240];
        if (r.len > sizeof(tmp) || !rd(p.cam0 + r.off, tmp, r.len) || !wr(cam2 + r.off, tmp, r.len)) {
            g_why = "a camera 0 field range could not be copied";
            return kFailedCreate;
        }
    }
    const uint8_t zero = 0;
    wr(cam2 + kCamPrimary, &zero, 1);                         // never the primary: the ViewSurface pre-render picks the first +0x304 == 1 camera
    wr(cam2 + kCamMode, &p.mode, sizeof(p.mode));             // equal to the view's own, so Recompute never rewrites +0x314

    c->step = 5;   // last look before the one irreversible call
    Probe q;
    if (probe_ready(q) != kReady || q.view != p.view || q.cam0 != p.cam0 || q.count != 1) {
        g_cam2_ptr.store(0, std::memory_order_release);
        g_why = "the view changed during creation; camera 2 abandoned before AddCamera";
        return kRetry;
    }

    c->step = 6;   // AddCamera, once. The engine's own entry: appends, recomputes, recomputes the viewport.
    ((AddFn)fn_addr(kAddBody))(p.view, cam2);

    // STEP 7: camera 2 shares camera 0's surface.
    //
    // WHAT SetViewSurface (FUN_14d92b890, RVA 0x0D92B890) DOES, from the TU25 decompile (E:\GRW-decomp\grw_new,
    // shard_14d92.c / shard_14dc6.c / shard_14dc7.c; offsets are struct offsets, no address carries over):
    //   - swaps the camera's refcounted handle at cam+0x2A8 for *slot (atomic refcount +1 on the new block,
    //     -1 on the old; idempotent when called again with the same block);
    //   - S = [handle] when the int at handle+0xC is negative, else 0. With S != 0: the closing virtual call is
    //     (*(E+0xD0)->vtbl[3])(E+0xD0, *(view+0x40), 0) with E = [S+0xD0] (the surface's environment object, the
    //     one carrying the near/far planes at E+0x16D0/E+0x16D4, default 0.1 and 1200), wrapped in the
    //     thread-local scope FUN_14e51dfd0 push / FUN_14e5378d0 pop (a balanced per-thread frame stack, index at
    //     TLS+0xB40; the pop flushes and resets the frame, nothing accumulates across calls);
    //   - then near/far from E, ViewTasks+0x10 = S+0xD18, cam+0x7B1 |= 1.
    // WHAT THE VIRTUAL IS: E+0xD0 is the settings sub-object built by FUN_14dc682d0 (vtable VA 0x143A89C70 in
    // this image, written once at the head of that constructor and never replaced). Its slot 3 is FUN_14dc7b130,
    // and the shared helper FUN_1416e5af0(dest, src) is literally src->vtbl[3](src, dest): slot 3 is COPY-TO.
    // FUN_14dc7b130 passes its second argument through FUN_14e536f10 (returns it unchanged when non-null, else
    // allocates) and FUN_14e5f5760, so it copies INTO the existing object *(view+0x40), field group by field
    // group (each group is again a vtbl[3] copy-to), and allocates and registers nothing. The stock run does
    // this once per SetViewSurface call, camera 0's.
    // VERDICT: the data copy is IDEMPOTENT (an in-place overwrite of the same destination from the same
    // source: camera 2 shares the view and the surface, so dest and src are the same objects camera 0's call
    // used). ONE residue, NOT idempotent: the copy ends with src+0x1091 = 0 after copying src+0x1090/+0x1091 to
    // the destination, so a second call hands the destination a 0 for +0x1091 where the first handed it the
    // source's value. Those two bytes are serialized booleans of the settings object (read by FUN_154ad1470
    // as an "any flag set" test); what they mean to the renderer is UNKNOWN. The vtable is read statically
    // from the constructor, not from a live object: UNVERIFIED at run time.
    // CONSEQUENCE, the conservative variant: call it for camera 2 only when AddCamera's own internal
    // SetViewSurface(cam, &cam+0x2A8) did not already leave camera 2 on camera 0's surface object. That
    // internal call runs with camera 2's DEFAULT handle (no live surface), so S is 0 there and the virtual is
    // skipped; sharing the surface therefore takes this one call. It runs once at creation and then ONLY when
    // camera 0's surface object [handle] changes (run_inner), never per frame and never because the handle
    // pointer was merely re-read.
    c->step = 7;
    {
        // Re-validate everything step 7 depends on: AddCamera has run and the engine may have moved on.
        uint16_t n7 = 0;
        uint64_t arr7 = 0, a07 = 0, a17 = 0, H0 = 0;
        const bool have_count = rdv(p.view + kViewCamCount, &n7);
        c->count_after = n7;
        if (!have_count || n7 != 2 || !rdv(p.view + kViewCamArr, &arr7) || !arr7 ||
            !rdv(arr7, &a07) || !rdv(arr7 + 8, &a17) || a07 != p.cam0 || a17 != cam2) {
            g_why = "after AddCamera the view no longer holds camera 0 and camera 2 in slots 0 and 1; camera 2 left as added, surface not shared, nothing further done";
            return kRefusedCreate;
        }
        if (!rdv(p.cam0 + kCamHandle, &H0) || !H0 || !handle_valid(H0)) {
            g_why = "after AddCamera camera 0's surface handle is not valid (+0xC not negative); camera 2 left as added, surface not shared, nothing further done";
            return kRefusedCreate;
        }
        c->handle0 = H0;
        uint64_t H2 = 0;
        rdv(cam2 + kCamHandle, &H2);
        c->surf0 = surface_of(H0);
        c->surf2_before = surface_of(H2);
        if (!c->surf0) {
            g_why = "after AddCamera camera 0's surface object could not be read; camera 2 left as added, surface not shared, nothing further done";
            return kRefusedCreate;
        }
        if (c->surf2_before != c->surf0) {
            ((SetSurfFn)fn_addr(kSetSurf))(cam2, (uint64_t*)(uintptr_t)(p.cam0 + kCamHandle));
            c->surf_called = 1;
        }
        g_last_surf.store(c->surf0, std::memory_order_relaxed);
    }

    // STEP 8 (BUILD 245): the viewports, by the ENGINE'S OWN SETTER, once. Build 244 wrote +0x314 raw and both
    // cameras read (0,0,1,1) a second later: with the camera mode 0, RecomputeViewport rewrites +0x314 to the full
    // rect on every call. FUN_14d92b050 stores the rect AND moves the camera to mode 4 and the view settings to
    // mode 4, the one mode RecomputeViewport leaves alone. What is remembered here is what the restore puts back.
    c->step = 8;
    rd(p.cam0 + kCamViewport, c->orig_vp0, sizeof(c->orig_vp0));
    rdv(p.cam0 + kCamMode, &c->orig_mode0);
    rdv(p.settings + kSceneMode, &c->orig_scene);
    c->vp_changed = 1;                                         // from here on a fault restores camera 0
    setvp(p.cam0, kVp0);                                       // first: it also moves the view settings to mode 4
    setvp(cam2, kVp2);
    rd(p.cam0 + kCamViewport, c->rb_vp0, sizeof(c->rb_vp0));
    rd(cam2 + kCamViewport, c->rb_vp2, sizeof(c->rb_vp2));
    rdv(p.cam0 + kCamMode, &c->rb_m0);
    rdv(cam2 + kCamMode, &c->rb_m2);

    c->step = 9;
    uint16_t n = 0;
    rdv(p.view + kViewCamCount, &n);
    c->count_after = n;
    return kOk;
}

int create_guarded(Facts* c, const Probe& p) {
    __try {
        return create_inner(c, p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kFault;
    }
}

// ---------------------------------------------------------------------------
// the per-frame update, at every builder entry once camera 2 exists.
// ---------------------------------------------------------------------------
enum : int { kRunOk = 0, kRunLostView = 1, kRunLostCount = 2, kRunLostArray = 3, kRunFault = -1 };

inline void bits_of(const float* v, uint32_t* out) { memcpy(out, v, 16); }

int run_inner() {
    uint64_t view = 0;
    if (!rdv(g_base + g_b->view_global, &view) || view != g_f.view) { g_why = "the main view pointer changed (level change?)"; return kRunLostView; }
    uint16_t cnt = 0;
    uint64_t arr = 0, a0 = 0, a1 = 0;
    if (!rdv(view + kViewCamCount, &cnt) || cnt != 2) { g_why = "the view no longer holds exactly 2 cameras"; return kRunLostCount; }
    if (!rdv(view + kViewCamArr, &arr) || !rdv(arr, &a0) || !rdv(arr + 8, &a1) || a0 != g_f.cam0 || a1 != g_f.cam2) {
        g_why = "the view's camera array no longer holds camera 0 and camera 2 in slots 0 and 1";
        return kRunLostArray;
    }
    const uint64_t cam0 = g_f.cam0, cam2 = g_f.cam2;

    // The surface: camera 0's can be replaced (resize, quality change). SetViewSurface is not free of side
    // effects (see step 7 in create_inner), so camera 2 is re-synced ONLY when camera 0's surface OBJECT
    // ([handle]) differs from the one camera 2 was last synced to; the handle block pointer alone being
    // re-read, or replaced by a block that refers to the same surface, does nothing. Per frame this is reads only.
    uint64_t P0 = 0;
    if (rdv(cam0 + kCamHandle, &P0)) {
        if (P0 && handle_valid(P0)) {
            uint64_t s0 = 0;
            if (rdv(P0, &s0) && s0 != 0 && s0 != g_last_surf.load(std::memory_order_relaxed)) {
                ((SetSurfFn)fn_addr(kSetSurf))(cam2, (uint64_t*)(uintptr_t)(cam0 + kCamHandle));
                g_last_surf.store(s0, std::memory_order_relaxed);
                g_resyncs.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            g_handle_bad.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // The pose: camera 0's, moved along camera 0's right axis. Row 0 of the 4x4 is the right basis vector,
    // row 3 is the position (CameraProbe.cpp write_pose_head: row-vector convention, 1 unit = 1 m).
    // BUILD 247: NOT in true-stereo mode. With cam2_stereo on, camera 2's pose is written by
    // write_pose_head (CameraProbe.cpp), in the same call as camera 0's, from the same composed head pose.
    // This builder entry runs BEFORE the selector composes the frame, so a write here would be one frame
    // old and from a different function (the exact thing the stereo design forbids).
    float m[16];
    uint64_t wmo = 0;
    const bool stereo_poses = g_stereo_req.load(std::memory_order_relaxed);
    const uint64_t srcp = (!stereo_poses && rdv(cam0 + kCamWmo, &wmo) && wmo && readable(wmo, 0x40)) ? wmo : cam0;
    if (!stereo_poses && rd(srcp, m, sizeof(m))) {
        bool finite = true;
        for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) finite = false;
        const float d = ipd();
        if (finite && std::isfinite(d)) {
            m[12] += d * m[0];
            m[13] += d * m[1];
            m[14] += d * m[2];
            wr(cam2, m, sizeof(m));
        } else {
            g_badpose.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // BUILD 245: the viewports are READ every frame and the engine setter is called again ONLY for a camera
    // that drifted (rect not bit-equal to what the setter was given, or mode not 4). The resize path, a
    // settings-mode reset or anything else that makes the engine recompute a mode-0 viewport lands here.
    float v0[4] = {}, v2[4] = {};
    uint32_t m0 = 0xFFFFFFFFu, m2 = 0xFFFFFFFFu;
    const bool r0 = rd(cam0 + kCamViewport, v0, sizeof(v0)) && rdv(cam0 + kCamMode, &m0);
    const bool r2 = rd(cam2 + kCamViewport, v2, sizeof(v2)) && rdv(cam2 + kCamMode, &m2);
    const bool d0 = r0 && (memcmp(v0, kVp0, sizeof(v0)) != 0 || m0 != kViewportMode);
    const bool d2 = r2 && (memcmp(v2, kVp2, sizeof(v2)) != 0 || m2 != kViewportMode);
    if (d0 || d2) {
        uint32_t b[4];
        bits_of(v0, b);
        for (int i = 0; i < 4; ++i) g_drift_last[i].store(b[i], std::memory_order_relaxed);
        g_drift_last[4].store(m0, std::memory_order_relaxed);
        bits_of(v2, b);
        for (int i = 0; i < 4; ++i) g_drift_last[5 + i].store(b[i], std::memory_order_relaxed);
        g_drift_last[9].store(m2, std::memory_order_relaxed);
        g_drift_who.store((d0 ? 1u : 0u) | (d2 ? 2u : 0u), std::memory_order_relaxed);
        g_drift_n.fetch_add(1, std::memory_order_relaxed);
        if (d0) { setvp(cam0, kVp0); g_reapply_n.fetch_add(1, std::memory_order_relaxed); }
        if (d2) { setvp(cam2, kVp2); g_reapply_n.fetch_add(1, std::memory_order_relaxed); }
    }
    // BUILD 247: the stereo gate. Both viewports were read back and were what the setter left, or a
    // re-apply just happened and the next kWarmAfterDrift frames are not trusted. A viewport that could
    // not be read at all is not "as set".
    if (d0 || d2 || !r0 || !r2) {
        g_warm_left.store(kWarmAfterDrift, std::memory_order_relaxed);
    } else {
        const uint32_t w = g_warm_left.load(std::memory_order_relaxed);
        if (w > 0) g_warm_left.store(w - 1, std::memory_order_relaxed);
    }
    // A drifted viewport was just put back by the setter above, so this frame is built with the right
    // rects; it is simply not trusted yet (the warm-up count). An unreadable viewport is not live.
    g_run_ok.store(r0 && r2, std::memory_order_release);
    return kRunOk;
}

int run_guarded() {
    __try {
        return run_inner();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kRunFault;
    }
}

// ---------------------------------------------------------------------------
// the builder callback (the thread that calls the graph-frame builder, before its body)
// ---------------------------------------------------------------------------
bool setvp_guarded(uint64_t cam, const float* rect) {
    __try {
        setvp(cam, rect);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Puts camera 0 back as the experiment found it (its rect, its mode, the view settings mode) when the experiment
// stops, but ONLY if camera 0 is provably still the live camera 0 of the same view: the main view global still
// holds the view recorded at creation, that view's slot 0 still holds the recorded camera 0, that camera still
// points back at the view, and the view's settings object is the recorded one. Every read and write goes through
// the guarded helpers (VirtualQuery plus SEH), and the one engine call is SEH-guarded too, so this cannot fault.
// When the view was replaced (level change) the recorded camera 0 may be freed memory: nothing is written.
// ORDER: the two modes first, so that the setter's own recompute runs in the original mode (mode 0 rewrites the
// rect to the full (0,0,1,1)); then the setter with the original rect. Camera 2 cannot be removed (there is no
// RemoveCamera), and what it does once the view settings mode is back at 0 is UNVERIFIED (REPORT.md section 9 says a camera
// whose +0x290 differs from the view's gets its +0x314 rewritten by Recompute).
void restore_cam0_viewport() {
    g_restored.store(2, std::memory_order_relaxed);
    uint64_t view = 0, arr = 0, a0 = 0, back = 0, settings = 0;
    uint16_t cnt = 0;
    if (!rdv(g_base + g_b->view_global, &view) || view != g_f.view) return;
    if (!rdv(view + kViewCamCount, &cnt) || cnt == 0) return;
    if (!rdv(view + kViewCamArr, &arr) || !arr || !rdv(arr, &a0) || a0 != g_f.cam0) return;
    if (!rdv(a0 + kCamBackPtr, &back) || back != view) return;
    if (!g_f.vp_changed) return;
    uint32_t m = 0;
    if (rdv(a0 + kCamMode, &m) && m == kViewportMode) wr(a0 + kCamMode, &g_f.orig_mode0, sizeof(g_f.orig_mode0));
    if (rdv(view + kViewScene, &settings) && settings == g_f.settings && rdv(settings + kSceneMode, &m) && m == kViewportMode)
        wr(settings + kSceneMode, &g_f.orig_scene, sizeof(g_f.orig_scene));
    setvp_guarded(a0, g_f.orig_vp0);
    g_restored.store(1, std::memory_order_relaxed);
}

void arm_step() {
    Probe p;
    const int r = probe_ready(p);
    if (r == kWait) {
        g_wait_why = p.why;
        g_waits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (r == kRefuse) {
        g_f.view = p.view; g_f.vt = p.vt; g_f.count_before = p.count;
        g_why = p.why;
        g_state.store(kRefused, std::memory_order_release);
        census::release_graph_hook();
        return;
    }
    Facts c;
    g_why = "";
    const int rc = create_guarded(&c, p);
    c.created_frame = d3d11::frame_count();
    c.created_entry = g_entries.load(std::memory_order_relaxed);
    g_f = c;
    if (rc == kOk) {
        // BUILD 247: the stereo keys are published before the state, camera 0's pointer first. The viewports
        // were just set by the engine setter (step 8) and read back; the warm-up counts frames from here.
        g_warm_left.store(kWarmFrames, std::memory_order_relaxed);
        g_cam0_ptr.store(g_f.cam0, std::memory_order_release);
        g_run_ok.store(true, std::memory_order_release);
        g_state.store(kRunning, std::memory_order_release);
        return;
    }
    if (rc == kRetry) {
        if (g_leaks.fetch_add(1, std::memory_order_relaxed) + 1 >= (uint32_t)kMaxLeaks) {
            g_why = "creation was abandoned three times (the view kept changing); stopping";
            g_state.store(kRefused, std::memory_order_release);
            census::release_graph_hook();
        }
        return;
    }
    if (rc == kFault) {
        g_why = "access violation inside the creation (see step); disabled for the run, never retried";
        g_state.store(kFaulted, std::memory_order_release);
    } else {
        g_state.store(rc == kRefusedCreate ? kRefused : kFaulted, std::memory_order_release);
    }
    if (g_f.vp_changed) restore_cam0_viewport();   // the creation got as far as the engine setter
    census::release_graph_hook();
}

void run_step() {
    const int r = run_guarded();
    if (r == kRunOk) {
        g_frames_ok.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // BUILD 247: any failure ends the stereo route at once, before the restore below touches camera 0.
    g_run_ok.store(false, std::memory_order_release);
    g_cam0_ptr.store(0, std::memory_order_release);
    if (r == kRunFault) {
        g_why = "access violation inside the per-frame update; disabled for the run";
        g_state.store(kFaulted, std::memory_order_release);
        // Camera 2 is still in the view (there is no RemoveCamera), so its key stays published and the hooks
        // keep skipping it. Camera 0 goes back as it was if it can be proven live.
        restore_cam0_viewport();
    } else {
        g_state.store(kLost, std::memory_order_release);   // g_why set by run_inner
        restore_cam0_viewport();
        // The recorded camera 2 address can be reused by a different object once the view is gone: unkey it.
        g_cam2_ptr.store(0, std::memory_order_release);
    }
    census::release_graph_hook();
}

// BUILD 263: the early write for the vehicle compose (CameraProbe.h veh_composed). At the graph-frame builder entry,
// which runs before the engine's visibility work, both cameras' ROTATION rows (rows 0..2, three floats each; the position
// and the w column are not touched) are set to the rotation the last frame composed, so the engine culls for the direction
// the picture really points. Guarded writes, no logging, only while the vehicle compose is on.
void veh_prewrite() {
    float out[9];
    if (!camera::veh_begin_frame(out)) return;   // BUILD 264: composed NOW, once for the frame
    bool any = false;
    const uint64_t cams[2] = {g_f.cam0, g_f.cam2};
    for (int k = 0; k < 2; ++k) {
        const uint64_t cam = cams[k];
        if (!cam) continue;
        bool ok = true;
        for (int r = 0; r < 3 && ok; ++r) ok = wr(cam + (uint64_t)r * 16, out + r * 3, 12);
        any = any || ok;
    }
    if (any) camera::veh_note_prewrite(out);
    if (any && gtrec::on()) gtrec::w_mark(gtrec::kWPrewrite, g_f.cam0, g_f.cam2);   // BUILD 270: log only
}

void on_graph_entry() {
    if (g_busy.exchange(1, std::memory_order_acquire) != 0) return;
    g_entries.fetch_add(1, std::memory_order_relaxed);
    const int st = g_state.load(std::memory_order_acquire);
    if (st == kWatch) {
        diag_record(st, false);                 // recording only: nothing of the engine's is touched
    } else if (st == kArmed) {
        diag_record(st, false);
        arm_step();
    } else if (st == kRunning) {
        diag_record(st, true);                  // the only place cam2_noprojshadow may act
        // BUILD 270: the gtrec G row (log only), before run_step and any prewrite, so it shows both cameras as the
        // builder found them.
        if (gtrec::on()) gtrec::g_emit(g_f.cam0, g_f.cam2, camera::build_seq_now(), st);
        run_step();
        // BUILD 267: when the recompute hook drives the vehicle frame it composes first, once per build; the graph-entry
        // compose of 263 to 266 stays as the fallback for when the hook is not live.
        if (g_run_ok.load(std::memory_order_relaxed) && !camera::veh_hook_drives()) veh_prewrite();
    }
    g_busy.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// init thread: verification, the gate, and the log
// ---------------------------------------------------------------------------
void hex16(const uint8_t* b, uint32_t n, char* out, size_t cap) {
    out[0] = 0;
    size_t k = 0;
    for (uint32_t i = 0; i < n && k + 4 < cap; ++i) k += (size_t)_snprintf_s(out + k, cap - k, _TRUNCATE, "%02X ", b[i]);
}

// An E9 rel32 thunk at `rva`: the RVA it lands on, or 0 when the bytes are not an E9.
uint32_t jmp_target_rva(uint32_t rva) {
    uint8_t b[5] = {};
    int32_t rel = 0;
    if (!rd(g_base + rva, b, 5) || b[0] != 0xE9) return 0;
    memcpy(&rel, b + 1, 4);
    return (uint32_t)((int64_t)rva + 5 + rel);
}

// Returns nullptr when every pin matches, else a literal naming the first mismatch. Logs the whole table.
const char* verify_pins() {
    const char* bad = nullptr;
    for (int i = 0; i < kNumFn; ++i) {
        const FnPin& f = g_b->fn[i];
        uint8_t found[16] = {};
        char e[64], g[64];
        const bool got = rd(g_base + f.rva, found, f.len);
        hex16(f.bytes, f.len, e, sizeof(e));
        hex16(found, f.len, g, sizeof(g));
        const bool ok = got && memcmp(found, f.bytes, f.len) == 0;
        LOG_INFO("cam2: pin %-38s RVA 0x%08X  expected %s| found %s| %s", f.name, f.rva, e, got ? g : "(unreadable) ", ok ? "OK" : "MISMATCH");
        if (!ok && !bad) bad = f.name;
    }
    if (bad) return "a pinned function's first bytes differ from the TU25 exe";
    // The thunk must land on the AddCamera body, and the view vtable slot +8 must hold the thunk.
    int32_t rel = 0;
    uint64_t slot = 0;
    rdv(g_base + g_b->fn[kAddThunk].rva + 1, &rel);
    const uint64_t target = g_base + g_b->fn[kAddThunk].rva + 5 + (int64_t)rel;
    const bool thunk_ok = target == g_base + g_b->fn[kAddBody].rva;
    const bool slot_ok = rdv(g_base + g_b->view_vtable + 8, &slot) && slot == g_base + g_b->fn[kAddThunk].rva;
    LOG_INFO("cam2: AddCamera thunk resolves to RVA 0x%08X (want 0x%08X): %s;  view vtable RVA 0x%08X slot +8 = 0x%llX (want thunk 0x%llX): %s",
             (uint32_t)(target - g_base), g_b->fn[kAddBody].rva, thunk_ok ? "OK" : "MISMATCH", g_b->view_vtable,
             (unsigned long long)slot, (unsigned long long)(g_base + g_b->fn[kAddThunk].rva), slot_ok ? "OK" : "MISMATCH");
    if (!thunk_ok) return "the AddCamera thunk does not resolve to the pinned body";
    if (!slot_ok) return "the view vtable slot +8 does not hold the pinned AddCamera thunk";

    // BUILD 245: the SetViewport chain. The thunk lands on the body; the E8 at body+0x54 reaches the pinned thunk,
    // which lands on the pinned view-settings mode setter. (All four byte patterns were just compared above.)
    const uint32_t vt_to = jmp_target_rva(g_b->fn[kSetVpThunk].rva);
    uint8_t call_op = 0;
    int32_t call_rel = 0;
    const uint32_t call_at = g_b->fn[kSetVp].rva + kSetVpCallOff;
    const bool have_call = rd(g_base + call_at, &call_op, 1) && rd(g_base + call_at + 1, &call_rel, 4) && call_op == 0xE8;
    const uint32_t call_to = have_call ? (uint32_t)((int64_t)call_at + 5 + call_rel) : 0;
    const uint32_t call_thunk_to = jmp_target_rva(g_b->fn[kSetVpCallThunk].rva);
    const bool vp_ok = vt_to == g_b->fn[kSetVp].rva;
    const bool ms_ok = have_call && call_to == g_b->fn[kSetVpCallThunk].rva && call_thunk_to == g_b->fn[kSceneModeSet].rva;
    LOG_INFO("cam2: SetViewport thunk resolves to RVA 0x%08X (want 0x%08X): %s;  its E8 at body+0x%X calls RVA 0x%08X (want thunk 0x%08X): %s, "
             "which lands on RVA 0x%08X (want the mode setter 0x%08X): %s",
             vt_to, g_b->fn[kSetVp].rva, vp_ok ? "OK" : "MISMATCH", kSetVpCallOff, call_to, g_b->fn[kSetVpCallThunk].rva,
             have_call && call_to == g_b->fn[kSetVpCallThunk].rva ? "OK" : "MISMATCH", call_thunk_to, g_b->fn[kSceneModeSet].rva,
             call_thunk_to == g_b->fn[kSceneModeSet].rva ? "OK" : "MISMATCH");
    if (!vp_ok) return "the SetViewport thunk does not resolve to the pinned body";
    if (!ms_ok) return "SetViewport's mode-setter call does not reach the pinned setter";
    return nullptr;
}

void start() {
    const gamebuild::Build* gb = gamebuild::get();
    if (!gb || gb->pe_timestamp != kTU25.pe_timestamp || gb->pe_size_of_image != kTU25.pe_size_of_image) {
        g_why = "this GRW.exe is not the TU25 build the cam2 table is keyed to";
        g_state.store(kRefused, std::memory_order_release);
        LOG_ERROR("cam2: REFUSED: %s (%s). Nothing created, nothing installed.", g_why, gb ? gb->name : "unknown");
        return;
    }
    const HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) {
        g_why = "no main module";
        g_state.store(kRefused, std::memory_order_release);
        LOG_ERROR("cam2: REFUSED: %s.", g_why);
        return;
    }
    g_b = &kTU25;
    g_base = (uint64_t)(uintptr_t)mod;
    LOG_INFO("cam2: experiment requested; TU25 row matched; image base 0x%llX. Verifying the pinned functions.", (unsigned long long)g_base);
    if (const char* bad = verify_pins()) {
        g_why = bad;
        g_state.store(kRefused, std::memory_order_release);
        LOG_ERROR("cam2: REFUSED: %s. Nothing created, nothing installed.", bad);
        return;
    }
    memset(&g_gate, 0, sizeof(g_gate));
    LOG_INFO("cam2: all %d pins OK. GATE: nothing is installed and camera 2 is not created until live gameplay has held continuously for %.0f s "
             "(wgun ARMED + skeleton updates advancing + frames advancing + one mode-0 camera in the main view that is the player camera). "
             "cam2_noprojshadow=%d.",
             (int)kNumFn, (double)g_arm_ms.load() / 1000.0, g_noproj.load() ? 1 : 0);
    g_state.store(kGate, std::memory_order_release);
}

// One tick of the gate (states gate and watch). Init thread.
void gate_step(int st) {
    const uint64_t now = GetTickCount64();
    static uint64_t s_watch_ms = 0;
    static uint32_t s_watch_row0 = 0;
    static bool     s_said_silent = false;

    if (!g_requested.load(std::memory_order_relaxed)) {
        // cam2_experiment was turned off in the cfg before anything was created: honour it.
        census::release_graph_hook();
        census::projshadow_watch(false);
        g_state.store(kIdle, std::memory_order_release);
        LOG_INFO("cam2: cam2_experiment is now 0; the gate is closed again and nothing was created.");
        return;
    }
    const int ge = gate_eval(now);
    if (ge == 2) {
        census::release_graph_hook();
        census::projshadow_watch(false);
        g_state.store(kRefused, std::memory_order_release);
        return;
    }
    if (st == kGate) {
        if (ge == 0 && g_gate.accum_ms >= (uint64_t)g_arm_ms.load(std::memory_order_relaxed)) {
            census::projshadow_watch(true);
            g_state.store(kWatch, std::memory_order_release);   // BEFORE the callback can exist
            const char* why = "";
            if (!census::acquire_graph_hook(&on_graph_entry, &why)) {
                g_why = why;
                census::projshadow_watch(false);
                g_state.store(kRefused, std::memory_order_release);
                LOG_ERROR("cam2: REFUSED: the graph-builder hook was not installed: %s. Nothing created.", why);
                return;
            }
            s_watch_ms = now;
            s_watch_row0 = g_diag_head.load(std::memory_order_acquire);
            s_said_silent = false;
            LOG_INFO("cam2: GATE PASSED (live gameplay held %.1f s). Graph-builder hook installed (%s). RECORDING ONLY for at least %llu ms before "
                     "camera 2 may be created; rows go to the ring, the file is GRWVR\\cam2diag-<pid>.txt.",
                     (double)g_gate.accum_ms / 1000.0, why, (unsigned long long)kWatchMs);
        }
        return;
    }
    // st == kWatch
    if (ge != 0) {
        census::release_graph_hook();
        census::projshadow_watch(false);
        g_state.store(kGate, std::memory_order_release);
        LOG_WARN("cam2: the gameplay latch was LOST during the recording pre-roll (%s). Hook released, nothing created; waiting again.", g_gate.why);
        return;
    }
    const uint32_t rows = g_diag_head.load(std::memory_order_acquire) - s_watch_row0;
    if (now - s_watch_ms >= kWatchMs && rows >= kWatchMinRows) {
        int exp = kWatch;
        bool live = false;
        const uint32_t ps = census::projshadow_take(&live);
        (void)ps;
        const double held = (double)g_gate.accum_ms / 1000.0;
        const double fps = held > 0.0 ? (double)(d3d11::frame_count() - g_gate.start_frames) / held : 0.0;
        LOG_INFO("cam2: ARMING DECISION: camera 2 will be created at the next builder entry. Gameplay latch held %.1f s (%.0f presented frames, %.1f fps, "
                 "%llu skeleton updates); signals: wgun ARMED skeleton 0x%llX, player camera 0x%llX == main view camera 0 (view 0x%llX, 1 camera, mode 0). "
                 "Pre-roll recorded %u builder rows in %llu ms; census hook 2 %s.",
                 held, (double)(d3d11::frame_count() - g_gate.start_frames), fps, (unsigned long long)(grwxr_wgun_calls - g_gate.start_calls),
                 (unsigned long long)g_gate.skel, (unsigned long long)g_gate.pcam, (unsigned long long)g_gate.view, rows,
                 (unsigned long long)(now - s_watch_ms), live ? "is recording (ProjShadow descriptors are observed)" : "is NOT recording (ProjShadow descriptors are NOT observed; read the 8c0/b60/n0 columns)");
        g_state.compare_exchange_strong(exp, kArmed, std::memory_order_acq_rel);
    } else if (!s_said_silent && now - s_watch_ms >= 15000 && rows < kWatchMinRows) {
        s_said_silent = true;
        LOG_WARN("cam2: only %u builder rows were recorded in %llu ms; the graph-builder callback is not firing as expected. Still waiting, nothing created.",
                 rows, (unsigned long long)(now - s_watch_ms));
    }
}

void log_gate(int st, uint32_t ticks) {
    Gate& g = g_gate;
    if (st == kGate) {
        if (g.why != g.last_logged || ticks - g.last_log_tick >= 10) {
            LOG_INFO("cam2: state=gate: %s; latch held %.1f of %.0f s (pins verified, hook NOT installed, camera 2 NOT created). wgun skeleton 0x%llX, player camera 0x%llX.",
                     g.why, (double)g.accum_ms / 1000.0, (double)g_arm_ms.load() / 1000.0, (unsigned long long)grwxr_wgun_skel,
                     (unsigned long long)::grwxr::camera::player_camera());
            g.last_logged = g.why;
            g.last_log_tick = ticks;
        }
        return;
    }
    LOG_INFO("cam2: state=watch: recording only (builder entries %u, rows %u, latch %.1f s). Camera 2 not created yet.",
             g_entries.load(std::memory_order_relaxed), g_diag_head.load(std::memory_order_relaxed), (double)g.accum_ms / 1000.0);
}

void log_diag_summary() {
    const uint32_t head = g_diag_head.load(std::memory_order_acquire);
    if (head == 0) return;
    DiagRow last[1];
    uint32_t first = 0;
    const uint32_t k = diag_snapshot(last, 1, &first);
    if (k == 0) return;
    const DiagRow& r = last[0];
    LOG_INFO("cam2: diag: rows %u; last row frame %llu state %s: 8c0=0x%02X%s b60=0x%llX d90=0x%llX d98=%d n0=%d n2=%d ps=0x%02X(%s) | ever: gate-open rows %u, atlas-null rows %u, BOTH %u",
             head, (unsigned long long)r.frame, kStateName[r.state < 8 ? r.state : 0], r.b8c0, (r.flags & 512) ? " (bit 4 cleared after)" : "",
             (unsigned long long)r.b60, (unsigned long long)r.d90, (int)r.d98, (int)r.n0, (int)r.n2, r.ps, r.pslive ? "observed" : "not observed",
             g_c_gate.load(), g_c_b60null.load(), g_c_gate_b60null.load());
    static bool s_said_both = false;
    if (!s_said_both && g_c_gate_b60null.load() > 0) {
        s_said_both = true;
        const uint32_t fs = g_first_null_state.load();
        LOG_WARN("cam2: DISCRIMINATOR: the projector-shadow atlas (renderer+0xB60) read NULL while the pass gate (renderer+0x8C0 bit 4) was open; "
                 "first at row %u, frame %llu, experiment state %s. %s",
                 g_first_null_row.load(), (unsigned long long)g_first_null_frame.load(), fs < 8 ? kStateName[fs] : "?",
                 fs == (uint32_t)kRunning ? "That is AFTER camera 2 exists." : "That is BEFORE camera 2 exists (the build 244 fault does not need it).");
    }
}

// Copies the last `want` published stereo rows (oldest first). Rows being rewritten are skipped.
uint32_t srow_snapshot(SRow* out, uint32_t want) {
    const uint32_t head = g_srows_head.load(std::memory_order_acquire);
    const uint32_t cnt = std::min<uint32_t>(std::min<uint32_t>(head, kSCap - 8), want);
    uint32_t k = 0;
    for (uint32_t i = head - cnt; i < head; ++i) {
        SSlot& s = g_srows[i & (kSCap - 1)];
        if (s.seq.load(std::memory_order_acquire) != i + 1) continue;
        SRow tmp = s.row;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.seq.load(std::memory_order_relaxed) != i + 1) continue;
        out[k++] = tmp;
    }
    return k;
}

// BUILD 247: the 1 Hz stereo2: line. Init thread only. Counters are cumulative; the rate is over the last tick.
void log_stereo(int st) {
    if (!g_stereo_req.load(std::memory_order_relaxed)) return;
    static uint32_t s_prev_both = 0;
    static uint64_t s_prev_ms = 0;
    static bool s_said_reason = false;
    const uint64_t now = GetTickCount64();
    const uint32_t both = g_sk[kStereoBoth].load(std::memory_order_relaxed);
    const double dt = s_prev_ms ? (double)(now - s_prev_ms) / 1000.0 : 0.0;
    const double rate = dt > 0.2 ? (double)(both - s_prev_both) / dt : 0.0;
    s_prev_ms = now;
    s_prev_both = both;
    if (st != kRunning) {
        LOG_INFO("stereo2: requested, waiting: camera 2 state=%s (the stereo route engages only while camera 2 is RUNNING; until then the alternate-eye path runs). "
                 "present frames alt=%u", kStateName[st >= 0 && st < 8 ? st : 0], g_sk[kStereoAlt].load(std::memory_order_relaxed));
        return;
    }
    SRow last[1];
    const uint32_t have = srow_snapshot(last, 1);
    const SRow& r = last[0];
    uint32_t proj_bits = g_proj_last_bits.load(std::memory_order_relaxed);
    float proj_asp;
    memcpy(&proj_asp, &proj_bits, sizeof(proj_asp));
    LOG_INFO("stereo2: live=%d warm=%d swap=%d | frames: BOTH(both eyes fresh)=%u (%.1f/s) mono-left=%u alt=%u not-ready=%u failed=%u tag2=%u | "
             "poses: written=%u bad=%u write-faults=%u mirror=%u | order: cam0-first=%u cam2-first=%u reasserts=%u | "
             "persp2 aspect last=%.3f half=%u full=%u other=%u",
             stereo_live() ? 1 : 0, stereo_warm() ? 1 : 0, g_stereo_swap.load() ? 1 : 0, both, rate,
             g_sk[kStereoMonoLeft].load(), g_sk[kStereoAlt].load(), g_sk[kStereoNotReady].load(), g_sk[kStereoFailed].load(), g_st_tag2.load(),
             g_pose_pub.load(), g_pose_bad.load(), g_pose_werr.load(), g_mirror_n.load(),
             g_ord_cam0_first.load(), g_ord_cam2_first.load(), g_c2_reasserts.load(),
             (double)proj_asp, g_proj_half.load(), g_proj_full.load(), g_proj_other.load());
    if (have) {
        const double rad = 57.29577951308232;
        LOG_INFO("stereo2: last frame #%llu kind=%s tag=%d | eye content %ux%u aspect %.3f (override %.3f) content fovy %.3f rad | "
                 "L pos(%.3f %.3f %.3f) q(%.3f %.3f %.3f %.3f) fov l%.1f r%.1f u%.1f d%.1f deg | "
                 "R pos(%.3f %.3f %.3f) q(%.3f %.3f %.3f %.3f) fov l%.1f r%.1f u%.1f d%.1f deg | measured eye gap %.4f m | poses %s",
                 (unsigned long long)r.frame, kKindName[r.kind >= 0 && r.kind < 5 ? r.kind : 0], (int)r.tag, r.eye_w, r.eye_h,
                 (double)r.aspect, (double)stereo_aspect_override(), (double)r.cfov,
                 (double)r.pos[0][0], (double)r.pos[0][1], (double)r.pos[0][2], (double)r.ori[0][0], (double)r.ori[0][1], (double)r.ori[0][2], (double)r.ori[0][3],
                 (double)r.fov[0][0] * rad, (double)r.fov[0][1] * rad, (double)r.fov[0][2] * rad, (double)r.fov[0][3] * rad,
                 (double)r.pos[1][0], (double)r.pos[1][1], (double)r.pos[1][2], (double)r.ori[1][0], (double)r.ori[1][1], (double)r.ori[1][2], (double)r.ori[1][3],
                 (double)r.fov[1][0] * rad, (double)r.fov[1][1] * rad, (double)r.fov[1][2] * rad, (double)r.fov[1][3] * rad,
                 std::sqrt((double)((r.pos[1][0] - r.pos[0][0]) * (r.pos[1][0] - r.pos[0][0]) + (r.pos[1][1] - r.pos[0][1]) * (r.pos[1][1] - r.pos[0][1]) +
                                    (r.pos[1][2] - r.pos[0][2]) * (r.pos[1][2] - r.pos[0][2]))),
                 r.poses_ok ? "from xrLocateViews" : "NOT valid");
    }
    LOG_INFO("stereo2: layer pose lag=%d presents (0 = the tag's orientation) | frames with the lagged orientation=%u, none in the ring=%u",
             g_lag_cfg.load(std::memory_order_relaxed), g_lag_used_n.load(std::memory_order_relaxed), g_lag_miss_n.load(std::memory_order_relaxed));
    {   // BUILD 249: the native fovy values of the calls the hook sees in a half viewport (value x calls), one line, own format.
        char mix[200];
        int off = 0;
        mix[0] = 0;
        for (int i = 0; i < 8; ++i) {
            const uint32_t k = g_native[i].key.load(std::memory_order_relaxed);
            if (!k || off > 170) continue;
            off += snprintf(mix + off, sizeof(mix) - (size_t)off, " %.3f:%u", (double)(k - 1u) / 1000.0, g_native[i].n.load(std::memory_order_relaxed));
        }
        LOG_INFO("stereo2: native fovy values forced (rad:calls):%s", mix);
    }
    {   // BUILD 248: the placement each eye used and the persp2 override, in their own line (own format, own arguments).
        const uint32_t pc = g_place_cam.load(std::memory_order_relaxed);
        const float t0h = bitsf(g_place_bits[0].load()), t1h = bitsf(g_place_bits[1].load());
        const float t0v = bitsf(g_place_bits[2].load()), t1v = bitsf(g_place_bits[3].load());
        LOG_INFO("stereo2: place eye0 tan h=%.3f v=%.3f (v fov %.3f rad, %s) | eye1 tan h=%.3f v=%.3f (v fov %.3f rad, %s) | "
                 "target v fov %.3f rad | persp2 forced=%u already-target=%u last-native=%.3f | placement fell back to the shared fov on %u frames",
                 (double)t0h, (double)t0v, 2.0 * std::atan((double)t0v), (pc & 1u) ? "own camera" : "SHARED",
                 (double)t1h, (double)t1v, 2.0 * std::atan((double)t1v), (pc & 2u) ? "own camera" : "SHARED",
                 (double)stereo_target_fovy(), g_ovr_forced.load(), g_ovr_same.load(),
                 (double)bitsf(g_ovr_last_in_bits.load()), g_place_fallback.load());
    }
    // The cameras' own pixel rectangles (floats x, y, w, h: scaled at +0x334, unscaled at +0x344; layout read from the
    // viewport recompute, FUN_14d936640 / FUN_14d7ff020; UNVERIFIED against a live object until this line is read).
    {
        float s0[4] = {}, u0[4] = {}, s2[4] = {}, u2[4] = {};
        rd(g_f.cam0 + 0x334, s0, 16); rd(g_f.cam0 + 0x344, u0, 16);
        rd(g_f.cam2 + 0x334, s2, 16); rd(g_f.cam2 + 0x344, u2, 16);
        float fv0 = 0.0f, fv2 = 0.0f;
        rdv(g_f.cam0 + 0x2B0, &fv0);
        rdv(g_f.cam2 + 0x2B0, &fv2);
        LOG_INFO("stereo2: camera fov fields (+0x2B0) cam0 %.4f cam2 %.4f rad | mirror %s: writes-checked %u, camera 2 differed %u, skipped %u "
                 "(differed near 0 = the engine already keeps camera 2's field in step; large = it leaves it alone and the mirror is what keeps the eyes equal in ADS)",
                 (double)fv0, (double)fv2, g_mirror_fov.load() ? "ON" : "off", g_fov_mirrored.load(), g_fov_differed.load(), g_fov_skipped.load());
        LOG_INFO("stereo2: engine pixel rects (x y w h) cam0 scaled (%.0f %.0f %.0f %.0f) unscaled (%.0f %.0f %.0f %.0f) | cam2 scaled (%.0f %.0f %.0f %.0f) unscaled (%.0f %.0f %.0f %.0f)",
                 (double)s0[0], (double)s0[1], (double)s0[2], (double)s0[3], (double)u0[0], (double)u0[1], (double)u0[2], (double)u0[3],
                 (double)s2[0], (double)s2[1], (double)s2[2], (double)s2[3], (double)u2[0], (double)u2[1], (double)u2[2], (double)u2[3]);
    }
    const char* why = g_first_reason.load(std::memory_order_relaxed);
    if (why && !s_said_reason) {
        s_said_reason = true;
        LOG_WARN("stereo2: FIRST FALLBACK REASON (logged once): %s", why);
    }
}

void log_state(int st) {
    const uint32_t entries = g_entries.load(std::memory_order_relaxed);
    if (st == kArmed) {
        uint64_t view = 0, vt = 0;
        uint16_t cnt = 0;
        if (rdv(g_base + g_b->view_global, &view) && view) { rdv(view, &vt); rdv(view + kViewCamCount, &cnt); }
        LOG_INFO("cam2: state=armed, waiting: '%s'. builder entries %u, waiting entries %u. main view 0x%llX vtable 0x%llX (pinned 0x%llX) cameras %u.",
                 g_wait_why, entries, g_waits.load(std::memory_order_relaxed), (unsigned long long)view, (unsigned long long)vt,
                 (unsigned long long)(g_base + g_b->view_vtable), (unsigned)cnt);
        log_diag_summary();
        log_stereo(st);   // BUILD 247
        return;
    }
    if (st == kRunning) {
        static bool said_created = false, said_slot = false;
        if (!said_created) {
            said_created = true;
            LOG_INFO("cam2: CREATED. view 0x%llX (vtable 0x%llX) cam0 0x%llX cam2 0x%llX descriptor 0x%llX; cameras in the view before add %u, after add %u; "
                     "cam0 handle 0x%llX surface 0x%llX; cam2 surface before step 7 0x%llX, step 7 SetViewSurface %s; "
                     "view settings 0x%llX mode %u (written to cam2+0x290); creation reached step %u of 9; at Present #%llu, builder entry %u.",
                     (unsigned long long)g_f.view, (unsigned long long)g_f.vt, (unsigned long long)g_f.cam0, (unsigned long long)g_f.cam2,
                     (unsigned long long)g_f.desc, g_f.count_before, g_f.count_after, (unsigned long long)g_f.handle0,
                     (unsigned long long)g_f.surf0, (unsigned long long)g_f.surf2_before, g_f.surf_called ? "CALLED" : "skipped (already shared)",
                     (unsigned long long)g_f.settings, g_f.mode, g_f.step, (unsigned long long)g_f.created_frame, g_f.created_entry);
            LOG_INFO("cam2: viewports by the ENGINE SETTER (FUN_14d92b050): before: cam0 rect (%.3f %.3f %.3f %.3f) mode %u, view-settings mode %u. "
                     "Read back right after: cam0 (%.3f %.3f %.3f %.3f) mode %u [want (0 0 0.5 1) mode 4], cam2 (%.3f %.3f %.3f %.3f) mode %u [want (0.5 0 0.5 1) mode 4].",
                     (double)g_f.orig_vp0[0], (double)g_f.orig_vp0[1], (double)g_f.orig_vp0[2], (double)g_f.orig_vp0[3], g_f.orig_mode0, g_f.orig_scene,
                     (double)g_f.rb_vp0[0], (double)g_f.rb_vp0[1], (double)g_f.rb_vp0[2], (double)g_f.rb_vp0[3], g_f.rb_m0,
                     (double)g_f.rb_vp2[0], (double)g_f.rb_vp2[1], (double)g_f.rb_vp2[2], (double)g_f.rb_vp2[3], g_f.rb_m2);
            LOG_WARN("cam2: NOTE: camera 0 is now mode %u, not 0. CameraProbe.cpp's selector and on_calc_mvp branches act only on mode-0 cameras, so while the "
                     "experiment runs they no longer re-capture or head-pose-write camera 0 (alternate-eye code untouched, it just stops matching).", kViewportMode);
            if (g_f.count_after != 2) LOG_WARN("cam2: the view reports %u cameras after AddCamera, expected 2.", g_f.count_after);
        }
        uint16_t cnt = 0;
        uint32_t s0 = 0xFFFFFFFFu, s2 = 0xFFFFFFFFu, m0 = 0xFFFFFFFFu, m2 = 0xFFFFFFFFu;
        float v0[4] = {}, v2[4] = {};
        uint64_t P0 = 0, P2 = 0;
        const char* shared = "DIFFERS";
        rdv(g_f.view + kViewCamCount, &cnt);
        rdv(g_f.cam0 + kCamSlot, &s0);
        rdv(g_f.cam2 + kCamSlot, &s2);
        rd(g_f.cam0 + kCamViewport, v0, sizeof(v0));
        rd(g_f.cam2 + kCamViewport, v2, sizeof(v2));
        rdv(g_f.cam0 + kCamMode, &m0);
        rdv(g_f.cam2 + kCamMode, &m2);
        rdv(g_f.cam0 + kCamHandle, &P0);
        rdv(g_f.cam2 + kCamHandle, &P2);
        if (P0 == P2) shared = "shared";
        else if (surface_of(P0) != 0 && surface_of(P0) == surface_of(P2)) shared = "same-surface";
        const uint32_t ok = g_frames_ok.load(std::memory_order_relaxed);
        if (!said_slot && ok >= 3) {
            said_slot = true;
            LOG_INFO("cam2: after %u frames: the graph builder assigned renderer slot index cam0=%d cam2=%d (cam2+0x2F4).", ok, (int)s0, (int)s2);
        }
        LOG_INFO("cam2: state=RUNNING cameras=%u slot0=%d slot2=%d vp0=(%.2f %.2f %.2f %.2f) mode0=%u vp2=(%.2f %.2f %.2f %.2f) mode2=%u handle %s "
                 "ipd=%.4f frames=%u entries=%u resyncs=%u badpose=%u handle-bad=%u",
                 (unsigned)cnt, (int)s0, (int)s2, (double)v0[0], (double)v0[1], (double)v0[2], (double)v0[3], m0, (double)v2[0], (double)v2[1],
                 (double)v2[2], (double)v2[3], m2, shared, (double)ipd(), ok, entries,
                 g_resyncs.load(std::memory_order_relaxed), g_badpose.load(std::memory_order_relaxed), g_handle_bad.load(std::memory_order_relaxed));
        static uint32_t s_drift_seen = 0;
        const uint32_t dn = g_drift_n.load(std::memory_order_relaxed);
        if (dn != s_drift_seen) {
            s_drift_seen = dn;
            float d0[4], d2[4];
            uint32_t b[4];
            for (int i = 0; i < 4; ++i) b[i] = g_drift_last[i].load(std::memory_order_relaxed);
            memcpy(d0, b, 16);
            for (int i = 0; i < 4; ++i) b[i] = g_drift_last[5 + i].load(std::memory_order_relaxed);
            memcpy(d2, b, 16);
            LOG_WARN("cam2: VIEWPORT DRIFT on %u frame(s) so far (setter re-applied %u times). Last: cam%s%s drifted; cam0 rect (%.3f %.3f %.3f %.3f) mode %u, "
                     "cam2 rect (%.3f %.3f %.3f %.3f) mode %u (want mode 4).",
                     dn, g_reapply_n.load(std::memory_order_relaxed), (g_drift_who.load() & 1) ? " 0" : "", (g_drift_who.load() & 2) ? " 2" : "",
                     (double)d0[0], (double)d0[1], (double)d0[2], (double)d0[3], g_drift_last[4].load(), (double)d2[0], (double)d2[1], (double)d2[2],
                     (double)d2[3], g_drift_last[9].load());
        }
        if (g_noproj.load()) {
            LOG_INFO("cam2: noprojshadow: ON. renderer+0x8C0 bit 4 cleared %u times (failed writes %u); the byte BEFORE the first clear 0x%02X, before the latest 0x%02X.",
                     g_np_count.load(), g_np_fail.load(), g_np_first.load() & 0xFFu, g_np_last.load() & 0xFFu);
        }
        // The gate dropping while camera 2 exists changes nothing (there is no RemoveCamera), but it is worth a line.
        // Only the `wgun: ARMED` half is re-read here: gate_eval() itself is not run while camera 2 exists (its view
        // probe refuses a view that holds 2 cameras, and it would overwrite g_why).
        static bool s_said_gate_drop = false;
        if (!s_said_gate_drop && grwxr_wgun_skel == 0) {
            s_said_gate_drop = true;
            LOG_WARN("cam2: wgun is no longer ARMED while camera 2 exists (loading screen, weapon holstered or the mission ended?). Nothing is done about it.");
        }
        log_diag_summary();
        log_stereo(st);   // BUILD 247: nothing when cam2_stereo is 0
        return;
    }
    // faulted / refused / lost
    LOG_INFO("cam2: state=%s: %s (creation step %u; cam2 0x%llX; cameras before add %u, after add %u; entries %u, frames %u; camera 0 restore: %s)",
             kStateName[st], g_why, g_f.step, (unsigned long long)g_f.cam2, g_f.count_before, g_f.count_after, entries,
             g_frames_ok.load(std::memory_order_relaxed),
             g_restored.load() == 1 ? "done (rect, mode and view-settings mode put back)" : (g_restored.load() == 2 ? "skipped (camera 0 not provably live)" : "not needed"));
}

}  // namespace

void set_enabled(bool on) { g_requested.store(on, std::memory_order_relaxed); }
bool requested() { return g_requested.load(std::memory_order_relaxed); }

void set_ipd(float metres) {
    metres = clamp_ipd(metres);
    uint32_t b;
    memcpy(&b, &metres, sizeof(b));
    g_ipd_bits.store(b, std::memory_order_relaxed);
}

void set_arm_seconds(float seconds) {
    if (!std::isfinite(seconds)) seconds = 45.0f;
    if (seconds < 1.0f) seconds = 1.0f;
    if (seconds > 3600.0f) seconds = 3600.0f;
    g_arm_ms.store((uint32_t)(seconds * 1000.0f), std::memory_order_relaxed);
}

void set_noprojshadow(bool on) { g_noproj.store(on, std::memory_order_relaxed); }

uint64_t second_camera() { return g_cam2_ptr.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// BUILD 247: TRUE STEREO. See Cam2.h for the contract.
// ---------------------------------------------------------------------------
void set_stereo(bool on) { g_stereo_req.store(on, std::memory_order_relaxed); }
void set_stereo_swap_eyes(bool on) { g_stereo_swap.store(on, std::memory_order_relaxed); }
void set_stereo_mirror_fov(bool on) { g_mirror_fov.store(on, std::memory_order_relaxed); }
void set_stereo_aspect(float a) {
    if (!std::isfinite(a) || a < 0.2f || a > 5.0f) a = 0.0f;   // 0 = none; a silly value is no value
    uint32_t b;
    memcpy(&b, &a, sizeof(b));
    g_stereo_aspect_bits.store(b, std::memory_order_relaxed);
}
bool stereo_requested() { return g_stereo_req.load(std::memory_order_relaxed); }
bool stereo_live() {
    return g_stereo_req.load(std::memory_order_relaxed) && g_run_ok.load(std::memory_order_acquire);
}
bool stereo_swap_eyes() { return g_stereo_swap.load(std::memory_order_relaxed); }
float stereo_aspect_override() {
    const uint32_t b = g_stereo_aspect_bits.load(std::memory_order_relaxed);
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}
uint64_t first_camera() { return g_cam0_ptr.load(std::memory_order_acquire); }
bool stereo_warm() { return g_warm_left.load(std::memory_order_relaxed) == 0; }

namespace {

// 64 bytes into camera 2's pose block. SEH only: this runs on the engine's selector threads, three times a
// frame, so no VirtualQuery. Camera 2 is never freed while its pointer is published (there is no
// RemoveCamera; the pointer is cleared when the experiment is LOST).
bool put_cam2_pose(const float* m) {
    const uint64_t c = g_cam2_ptr.load(std::memory_order_relaxed);
    if (!c || (c & 15) != 0) return false;
    __try {
        memcpy((void*)(uintptr_t)c, m, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pose_werr.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

bool all_finite16(const float* m) {
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false;
    return true;
}

// Camera 0's vertical fov (+0x2B0) into camera 2's. WHY: the director that zooms the player camera for ADS and
// sprint writes camera 0's field and nothing owns camera 2's, so without this one eye could keep the walking fov
// while the other zooms. The value is one the engine itself authored for camera 0 (judged finite and in the
// projection hook's own accepted range) and what it did not change is counted: `differed` in the stereo2: line
// says whether the engine was already keeping camera 2's field in step (0) or leaves it alone (many).
void mirror_fov() {
    if (!g_mirror_fov.load(std::memory_order_relaxed)) return;
    const uint64_t c0 = g_cam0_ptr.load(std::memory_order_relaxed);
    const uint64_t c2 = g_cam2_ptr.load(std::memory_order_relaxed);
    if (!c0 || !c2 || (c0 & 15) != 0 || (c2 & 15) != 0) return;
    __try {
        const float f0 = *(volatile const float*)(uintptr_t)(c0 + 0x2B0);
        const float f2 = *(volatile const float*)(uintptr_t)(c2 + 0x2B0);
        if (!(f0 > 0.01f && f0 < 3.1f)) { g_fov_skipped.fetch_add(1, std::memory_order_relaxed); return; }
        if (memcmp(&f0, &f2, sizeof(f0)) != 0) {
            g_fov_differed.fetch_add(1, std::memory_order_relaxed);
            *(volatile float*)(uintptr_t)(c2 + 0x2B0) = f0;
        }
        g_fov_mirrored.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pose_werr.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

void stereo_publish_pose(uint64_t frame, const float m2[16]) {
    if (!all_finite16(m2)) {
        g_pose_bad.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The latch first, so camera 2's own selector call (which may run on another thread) can re-assert it.
    if (g_lat_busy.exchange(1, std::memory_order_acquire) == 0) {
        const uint32_t s = g_lat.seq.load(std::memory_order_relaxed);
        g_lat.seq.store(s | 1u, std::memory_order_relaxed);          // odd: being written
        std::atomic_thread_fence(std::memory_order_release);
        memcpy(g_lat.m, m2, sizeof(g_lat.m));
        g_lat.frame.store(frame, std::memory_order_relaxed);
        g_lat.seq.store((s | 1u) + 1u, std::memory_order_release);   // even: published
        g_lat_busy.store(0, std::memory_order_release);
    }
    if (put_cam2_pose(m2)) g_pose_pub.fetch_add(1, std::memory_order_relaxed);
    mirror_fov();
}

// BUILD 267: camera 2's own recompute entry, called from the hook, puts the pose latched by THIS build's compose into camera 2's
// rows. Same body as stereo_cam2_call but not keyed on the Present count (a build is not a Present).
void stereo_reassert_current() {
    float m[16];
    bool have = false;
    for (int tries = 0; tries < 2 && !have; ++tries) {
        const uint32_t s1 = g_lat.seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        memcpy(m, g_lat.m, sizeof(m));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_lat.seq.load(std::memory_order_relaxed) == s1) have = true;
    }
    if (have && put_cam2_pose(m)) {
        g_c2_reasserts.fetch_add(1, std::memory_order_relaxed);
        if (gtrec::on()) gtrec::w_mark(gtrec::kWReassert, 0, g_cam2_ptr.load(std::memory_order_relaxed));   // BUILD 270
    }
}

void stereo_cam2_call(uint64_t frame) {
    float m[16];
    bool have = false;
    for (int tries = 0; tries < 2 && !have; ++tries) {
        const uint32_t s1 = g_lat.seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        const uint64_t f = g_lat.frame.load(std::memory_order_relaxed);
        if (f != frame) { g_ord_cam2_first.fetch_add(1, std::memory_order_relaxed); return; }
        memcpy(m, g_lat.m, sizeof(m));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_lat.seq.load(std::memory_order_relaxed) == s1) have = true;
    }
    if (!have) return;
    g_ord_cam0_first.fetch_add(1, std::memory_order_relaxed);
    if (put_cam2_pose(m)) {
        g_c2_reasserts.fetch_add(1, std::memory_order_relaxed);
        if (gtrec::on()) gtrec::w_mark(gtrec::kWCam2Sel, 0, g_cam2_ptr.load(std::memory_order_relaxed));   // BUILD 270
    }
}

void stereo_mirror(uint64_t cam0) {
    if (!cam0) return;
    // A real pose was composed for this frame already (an earlier call of the same frame): never overwrite it
    // with a mirror of camera 0, which would put camera 2 a full separation beyond camera 0's own offset.
    if (g_lat.frame.load(std::memory_order_relaxed) == d3d11::frame_count()) return;
    float m[16];
    __try {
        memcpy(m, (const void*)(uintptr_t)cam0, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!all_finite16(m)) return;
    const float d = ipd();
    m[12] += d * m[0];
    m[13] += d * m[1];
    m[14] += d * m[2];
    if (put_cam2_pose(m)) {
        g_mirror_n.fetch_add(1, std::memory_order_relaxed);
        if (gtrec::on()) gtrec::w_mark(gtrec::kWMirror, 0, g_cam2_ptr.load(std::memory_order_relaxed));   // BUILD 270
    }
}

void stereo_note_proj(float aspect) {
    if (!g_stereo_req.load(std::memory_order_relaxed)) return;
    uint32_t b;
    memcpy(&b, &aspect, sizeof(b));
    g_proj_last_bits.store(b, std::memory_order_relaxed);
    if (aspect > 0.80f && aspect < 1.00f) g_proj_half.fetch_add(1, std::memory_order_relaxed);
    else if (aspect > 1.65f && aspect < 1.90f) g_proj_full.fetch_add(1, std::memory_order_relaxed);
    else g_proj_other.fetch_add(1, std::memory_order_relaxed);
}

void set_stereo_target_fovy(float fovy) {
    if (!std::isfinite(fovy) || fovy < 1.0f || fovy > 2.5f) return;
    g_target_fovy_bits.store(fbits(fovy), std::memory_order_relaxed);
}
float stereo_target_fovy() { return bitsf(g_target_fovy_bits.load(std::memory_order_relaxed)); }

bool stereo_override_fovy(float fovy, float aspect, float lo, float* out) {
    if (!g_stereo_req.load(std::memory_order_relaxed)) return false;
    if (!(aspect > 0.80f && aspect < 1.00f)) return false;          // the half viewport; full width and cubemap faces are left alone
    // What this projection would be at the full 16:9 viewport: native world 0.78..0.87, sprint/vehicle ~1.22,
    // the mod's own override 1.92. Below `lo` is ADS or a scope; above 1.95 is something else.
    const float eq = 2.0f * atanf(tanf(0.5f * fovy) * aspect / (16.0f / 9.0f));
    if (!(eq >= lo && eq <= 1.95f)) return false;
    const float tgt = stereo_target_fovy();
    note_native(fovy);
    g_ovr_last_in_bits.store(fbits(fovy), std::memory_order_relaxed);
    if (fabsf(fovy - tgt) < 1e-4f) { g_ovr_same.fetch_add(1, std::memory_order_relaxed); return false; }
    g_ovr_forced.fetch_add(1, std::memory_order_relaxed);
    *out = tgt;
    return true;
}

bool camera_proj_tans(uint64_t cam, float* tan_h, float* tan_v) {
    if (!cam || (cam & 15) != 0) return false;
    float m[16];
    __try {
        memcpy(m, (const void*)(uintptr_t)(cam + 0x420), sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!std::isfinite(m[0]) || !std::isfinite(m[5])) return false;
    if (!(m[0] > 0.1f && m[0] < 20.0f && m[5] > 0.1f && m[5] < 20.0f)) return false;
    *tan_h = 1.0f / m[0];
    *tan_v = 1.0f / m[5];
    return true;
}

void stereo_note_lag(int lag, uint32_t used, uint32_t miss) {
    g_lag_cfg.store(lag, std::memory_order_relaxed);
    g_lag_used_n.store(used, std::memory_order_relaxed);
    g_lag_miss_n.store(miss, std::memory_order_relaxed);
}

void stereo_note_place(const float th[2], const float tv[2], const bool from_cam[2]) {
    g_place_bits[0].store(fbits(th[0]), std::memory_order_relaxed);
    g_place_bits[1].store(fbits(th[1]), std::memory_order_relaxed);
    g_place_bits[2].store(fbits(tv[0]), std::memory_order_relaxed);
    g_place_bits[3].store(fbits(tv[1]), std::memory_order_relaxed);
    g_place_cam.store((from_cam[0] ? 1u : 0u) | (from_cam[1] ? 2u : 0u), std::memory_order_relaxed);
    if (!from_cam[0] || !from_cam[1]) g_place_fallback.fetch_add(1, std::memory_order_relaxed);
}

void stereo_note_reason(const char* literal) {
    const char* none = nullptr;
    g_first_reason.compare_exchange_strong(none, literal, std::memory_order_relaxed);
}

void stereo_note_frame(int kind, int tag, uint64_t frame, const float pos[2][3], const float ori[2][4],
                       const float fov[2][4], float cfov, uint32_t eye_w, uint32_t eye_h, float aspect, bool poses_ok) {
    if (kind >= 0 && kind < 5) g_sk[kind].fetch_add(1, std::memory_order_relaxed);
    if (tag == 2) g_st_tag2.fetch_add(1, std::memory_order_relaxed);
    // Single writer (the present thread): fill the slot under its own seqlock. No allocation, no lock.
    const uint32_t n = g_srows_head.load(std::memory_order_relaxed);
    SSlot& s = g_srows[n & (kSCap - 1)];
    s.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    SRow& r = s.row;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    r.qpc = (uint64_t)q.QuadPart;
    r.frame = frame;
    memcpy(r.pos, pos, sizeof(r.pos));
    memcpy(r.ori, ori, sizeof(r.ori));
    memcpy(r.fov, fov, sizeof(r.fov));
    r.cfov = cfov; r.aspect = aspect; r.eye_w = eye_w; r.eye_h = eye_h;
    r.entry = g_entries.load(std::memory_order_relaxed);
    r.kind = (int8_t)kind; r.tag = (int8_t)tag; r.poses_ok = poses_ok ? 1 : 0;
    s.seq.store(n + 1, std::memory_order_release);
    g_srows_head.store(n + 1, std::memory_order_release);
}

// The "game stopped presenting" signal of the heartbeat (dllmain.cpp), the same one that takes the thread
// snapshot. Runs on the init thread, which is never one of the stalled workers.
void on_stall(const char* why) {
    const int st = g_state.load(std::memory_order_acquire);
    if (st == kIdle || st == kGate || st == kRefused) return;
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "the game stopped presenting (%s)", why ? why : "");
    dump_diag(buf);
}

// DLL detach: best effort only. A hard kill never gets here; the rolling dump and on_stall cover that.
void on_exit() {
    const int st = g_state.load(std::memory_order_acquire);
    if (st != kWatch && st != kArmed && st != kRunning) return;
    dump_diag("process detach (exit of the window)");
}

void tick() {
    static unsigned s_ticks = 0;
    int st = g_state.load(std::memory_order_acquire);
    // Off means off: not a line in the log, and never a read of the engine. (A run that already
    // started keeps going if the cfg is turned off mid-run: there is no RemoveCamera.)
    if (st == kIdle && !g_requested.load(std::memory_order_relaxed)) {
        // BUILD 247: the one line a half-set pair of keys gets (cam2_stereo needs cam2_experiment). Once.
        static bool s_said_pair = false;
        if (!s_said_pair && g_stereo_req.load(std::memory_order_relaxed)) {
            s_said_pair = true;
            LOG_WARN("stereo2: cam2_stereo = 1 but cam2_experiment = 0: camera 2 is never created, so the stereo route stays off "
                     "(the alternate-eye path runs). Set cam2_experiment = 1 as well.");
        }
        return;
    }
    ++s_ticks;
    if (st == kIdle) {
        const unsigned long long fc = d3d11::frame_count();
        if (fc < kMinFrames) {
            if ((s_ticks % 5) == 1)
                LOG_INFO("cam2: state=idle, requested; waiting for Present #%llu before verifying (now %llu).", kMinFrames, fc);
            return;
        }
        start();
        st = g_state.load(std::memory_order_acquire);
    }
    if (st == kGate || st == kWatch) {
        gate_step(st);
        st = g_state.load(std::memory_order_acquire);
        if (st == kGate || st == kWatch) {
            log_gate(st, s_ticks);
            if (st == kWatch) log_diag_summary();
            if ((s_ticks % 5) == 0) log_stereo(st);   // BUILD 247: one waiting line every 5 s, nothing when cam2_stereo is 0
        }
    }
    // A terminal state (FAULTED / REFUSED / LOST) is logged once when it is entered, then at most once
    // every 30 s: the 1 Hz repeat of an unchanging terminal line told nothing and filled the log.
    if (st == kFaulted || st == kRefused || st == kLost) {
        static int s_term_state = -1;
        static unsigned long long s_term_ms = 0;
        const unsigned long long now = GetTickCount64();
        if (st == s_term_state && now - s_term_ms < 30000ull) return;
        s_term_state = st;
        s_term_ms = now;
        log_state(st);
        if (st != kRefused || g_diag_head.load(std::memory_order_acquire) != 0) {
            static int s_dumped_for = -1;
            if (s_dumped_for != st) {
                s_dumped_for = st;
                char buf[160];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "state became %s: %s", kStateName[st], g_why);
                dump_diag(buf);
            }
        }
        return;
    }
    if (st == kArmed || st == kRunning) log_state(st);

    // The rolling dump: while recording, the last 300 rows are rewritten every kRollingDumpMs, so a hard kill
    // of the process (which no handler can write from) still leaves the last few seconds on disk.
    if (st == kWatch || st == kArmed || st == kRunning) {
        static unsigned long long s_last_dump = 0;
        const unsigned long long now = GetTickCount64();
        if (now - s_last_dump >= kRollingDumpMs) {
            s_last_dump = now;
            dump_diag("rolling (every 5 s while recording)");
        }
    }
}

}  // namespace cam2
}  // namespace grwxr
