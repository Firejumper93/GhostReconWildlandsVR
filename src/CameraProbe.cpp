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
#include "AimTrace.h"
#include "Cam2.h"       // BUILD 243: the second-camera experiment's pointer, to keep the player-camera hooks off it
#include "CbDump.h"     // BUILD 246: the constant-buffer content capture reads the proj[2] call arguments

#include "GameBuild.h"

#include "D3D11Hook.h"
#include "HeadPose.h"
#include "Log.h"
#include "Sig.h"
#include "Perf.h"   // BUILD 215: the rig write and the camera hooks are timed
#include "Voice.h"
#include "ThunkHook.h"
#include "MvpHook.h"          // BUILD 267: the in-engine recompute hook
#include "GtRec.h"            // BUILD 270: the ground-truth stereo recorder (log only, cfg gtrec, off by default)

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
// Build 34: direct setter calls made on the FP toggle edge (instant hide).
volatile uint64_t grwxr_headhide_direct = 0;

// BUILD 224: THE PRE-LATCH (ported from the flat mod, 2026-08-12). Read and
// written by grwxr_headlatch_entry in ProbeStub.asm on every engine
// visibility-predicate call. The asm OFFERS a candidate in _cand; promotion
// to grwxr_headhide_obj happens on the drain thread after the ENTITY TIE
// (see head_obj_entity below). Same plain-global convention as above.
uint64_t grwxr_headlatch_impl = 0;   // 0 until VERIFIED at install
volatile uint64_t grwxr_headlatch_cand = 0;
volatile uint64_t grwxr_headlatch_hits = 0;
void  grwxr_headlatch_entry();

// BUILD 121: THE HIDER CENSUS.
//
// Owner insight: the hider can hide HANDS, not just the head and helmet, and
// the hands are where the weapon is actually held. So the question stops being
// "did the head hide" and becomes "what else can this setter reach".
//
// Today the stub latches ONE object, the first whose class matches, and the
// 1 Hz line reports that single pointer. In the 2026-08-20 run it reported the
// same address for three minutes, which tells us nothing about whether hands
// ever pass through at all.
//
// This records every DISTINCT object pointer the setter is called on, with its
// class method table beside it. Distinct OBJECTS matter more than distinct
// CLASSES here: head, helmet and hands are plausibly the same class as
// separate visuals, so a class-only census would collapse them into one row.
//
// Cost: [VERIFIED, headset log grwxr-26616] the setter was called 21 times in
// a three minute run. A 16-slot linear scan on a path that runs 21 times is
// free, and the stub still makes no call, touches no stack and allocates
// nothing, so rule 8 holds exactly as it did before.
uint64_t grwxr_headhide_ring[16]     = {};   // distinct object pointers
uint64_t grwxr_headhide_ring_cls[16] = {};   // their class method tables
void  grwxr_headhide_entry();

// Build 67: THE GUN-ROOT BONE WRITE. `[VERIFIED, headset, log grwxr-18692]`
// the held weapon's origin tracks the PLAYER rig's Fake_gunroot bone to
// 0..6 mm across 88 samples and 0.7 m of movement, so that bone is the mount.
// The engine composes the weapon's placement from it inside
// Skeleton::PublishAttachments, which is therefore the only window where a
// write is both after the animation solver and before the consumer (the whole
// chain is in docs/RE-notes.md). Offsetting one bone by a constant asks one
// binary question: does the RENDERED gun move. cfg wgun / wgun_dz.
uint64_t grwxr_wgun_skel   = 0;   // published only while armed
uint64_t grwxr_wgun_impl   = 0;
uint32_t grwxr_wgun_node   = 0;
uint32_t grwxr_wgun_dz     = 0;   // float bits
volatile uint64_t grwxr_wgun_calls  = 0;
volatile uint64_t grwxr_wgun_writes = 0;
void  grwxr_wgun_entry();

// BUILD 80: the rotation. The lift was four instructions of assembly; setting
// the barrel onto the controller ray is quaternion work, so the stub now calls
// out to C for it, exactly as build 68's grwxr_wnode_prep does. Runs on the
// engine's own thread inside PublishAttachments: no allocation, no lock, no
// logging, rule 8 intact. Every gate has its own counter (the rule this
// project wrote after one counter behind five early returns cost three runs).
extern "C" void grwxr_wgun_apply(void* skel);
uint32_t grwxr_wgun_rot = 0;             // 1 = rotate, 0 = the verified lift
volatile uint64_t grwxr_wg_nopose = 0;   // [skel+0x238] unreadable or null
volatile uint64_t grwxr_wg_nobuf  = 0;   // [pose+0x178] unreadable or null
volatile uint64_t grwxr_wg_norec  = 0;   // bone record unreadable
volatile uint64_t grwxr_wg_noray  = 0;   // no fresh controller ray
volatile uint64_t grwxr_wg_badq   = 0;   // degenerate quaternion or axis
// 2026-08-13: two-hand and roll accounting. two = frames the front hand had
// some authority, tworej = frames the agreement gate refused it, roll = frames
// a twist was applied. Read together these say whether the feature is actually
// engaging in play or quietly never firing.
volatile uint64_t grwxr_wg_two    = 0;
volatile uint64_t grwxr_wg_tworej = 0;
volatile uint64_t grwxr_wg_roll   = 0;
volatile uint64_t grwxr_wg_rot    = 0;   // rotations actually written
volatile uint64_t grwxr_wg_nopos  = 0;   // build 81: no controller position
volatile uint64_t grwxr_wg_pos    = 0;   // build 81: positions written
volatile uint64_t grwxr_wg_mirror = 0;   // build 183: mount-mirror copies written
// Build 99: frames the front-hand hold slid the weapon along its barrel. Zero
// with wgun_grip_two at 0 (the default) or with only one hand on the weapon,
// so a nonzero value is proof the feature is engaging and not merely armed.
volatile uint64_t grwxr_wg_grip   = 0;
// Build 101: frames the safety clamp actually bit. A high count next to pos
// means the gun is being held back from where the controller asked for it, and
// the tester feels that as the weapon refusing to follow his hand. This existed
// as a silent gate for eighteen builds and cost a tuning session.
volatile uint64_t grwxr_wg_clamped = 0;
// 2026-08-31: THE HANDS. hands = calls where the hand chains rode the gun,
// anchor = calls where the position anchor crossfaded to the rear hand because
// the front hand had measured as the right one. Zero with wgun_hands or
// wgun_two_anchor at 0, so each is proof its feature engaged and not merely
// shipped.
volatile uint64_t grwxr_wg_hands  = 0;
volatile uint64_t grwxr_wg_anchor = 0;
// Build 140: calls the arm collapse actually wrote. Zero with wgun_hide_arms
// at 0, so nonzero is proof the diagnostic engaged.
volatile uint64_t grwxr_wg_arms   = 0;
// Build 141: free = calls the left chain tracked the controller (any amount),
// hfar = calls the reach clamp bit. A silent clamp reads as the hand refusing
// to reach, which is the exact defect the clamp counter rule exists for.
volatile uint64_t grwxr_wg_free   = 0;
volatile uint64_t grwxr_wg_hfar   = 0;
// Build 142: ik = arm solves written, ikcl = solves where the hand target sat
// beyond the avatar's arm reach and the arm went straight instead (the wrist
// gap case, and the number that says whether the wingspan scale is needed).
volatile uint64_t grwxr_wg_ik     = 0;
volatile uint64_t grwxr_wg_ikcl   = 0;
// Build 143: solves refused by a guard (unmeasured lengths, unreadable
// records, degenerate geometry). ik=0 with ikskip climbing was build 142's
// whole arm failure, invisible because no counter watched the guards.
volatile uint64_t grwxr_wg_ikskip = 0;
// Build 145: frames the grip gate zeroed an otherwise-engaged two-hand
// authority. veto climbing while the hold feels one-handed means the off
// grip is not being squeezed (or not published), not that the hold broke.
volatile uint64_t grwxr_wg_gripveto = 0;
// 2026-08-31b: calls the ATTACH BAND released entirely, the off hand too far
// from the weapon to be holding it. Next to two= and tworej= this says whether
// a dead two-handed feel is the gate, the geometry, or a hand that simply was
// not on the gun.
volatile uint64_t grwxr_wg_far    = 0;

// Build 68: THE WEAPON PLACEMENT SUBSTITUTION. Research verdict (2026-08-09,
// docs/RESEARCH-IK-MOTION-CONTROLS-2026-08.md): every closed-engine precedent
// converges on being the LAST WRITER, and this is the only point with no
// timing hazard at all, because we are inside the engine's own commit rather
// than racing it. FRIK abandoned its renderer and animation-vfunc hooks for
// exactly this reason; UEVR calls the same idea "Permanent Change".
uint64_t grwxr_wnode_impl  = 0;
volatile uint64_t grwxr_wnode_calls = 0;
extern "C" const void* grwxr_wnode_prep(void* node, const float* m);
void  grwxr_wnode_entry();
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

// Build 46: the RVAs moved to GameBuild.cpp, one table per analysed binary,
// selected by the fail-closed PE-header pin. This array keeps only the
// build-independent metadata, in the SAME ROW ORDER as Build::cam. Entry 0
// must still resolve to the signature-scanned anchor or nothing is installed
// at all; that check validates whichever table the pin selected.
struct Target {
    bool        rcx_is_camera;   // key rows on rcx, and read camera fields
    const char* name;
};

constexpr Target kTargets[] = {
    {false, "proj[0] (anchor)"},
    {false, "proj[1]"},
    {false, "proj[2] (gameplay)"},
    {false, "proj[3] (skew path)"},
    {false, "proj[4]"},
    {false, "proj[5]"},
    {true,  "on_calc_mvp"},
    {true,  "selector"},
    // Build 14a, skeleton/HIK runtime probe. Both thunks were located OFFLINE
    // against the pinned binary (docs/RE-notes.md "Task function pointers
    // extracted" and "The HIK datablock reader") and both are the same
    // 5-byte-jmp-in-16-byte-slot shape as the camera thunks. Read-only: the
    // recorder captures registers and counts, nothing engine-visible changes.
    {false, "SkeletonPostUpdate"},
    {false, "HIK datablock reader"},
    // Build 15L (session 18): THE PLAYER PREDICATE. Offline decompilation
    // found cPlayerComponent::OnInit (named by its own log format strings)
    // and, inside it, this thunk called with rcx = the component.
    // [component+0x10] is the OWNING ENTITY, i.e. the local player's entity:
    // the exact identity every probe since 15e failed to find by scanning.
    // Read-only capture; the predicate is then [skeleton+0x10] == that
    // entity.
    {false, "cPlayerComponent::OnInit callee"},
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
// CORRECTED 2026-08-20: +0x2BC is the BASE ASPECT MULTIPLIER, not the fov.
// [VERIFIED] from projbuild (lastrites 0x0D7C0610), which computes
//   aspect = Camera[0x2BC] * (viewportW / viewportH)
// and only then derives the frustum. The real vertical fov in radians, the
// value actually fed to tan(fov/2), is Camera+0x2B0. This field is read once
// into a diagnostic and printed in one log line; nothing branches on it, so
// the mislabel never changed behaviour. Renamed rather than repointed,
// because the log line's meaning is what was wrong.
constexpr size_t kOffAspect = 0x2BC; // float, base aspect multiplier
// Neighbours, [VERIFIED] same listing, recorded so they are not re-derived:
//   +0x2B0 float  vertical fov, radians (the true fov)
//   +0x2B4 float  default near clip, initialised to 0.1f at 0x0D7A21D8
//   +0x2B8 float  far clip
//   +0x2C0 float  resolution scale
//   +0x7A4 float  near clip OVERRIDE, used when |value| > 1e-5
// NOTE: docs previously labelled +0x7A4 as fov. That is refuted; it is near.
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
        memcpy(&c.fov,  b + kOffAspect, sizeof(c.fov));
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
// Fixed size, no allocation, no locks (CLAUDE.md rule 8). The min/max updates
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
// Build 87: the OTHER target that receives the camera in rcx. on_calc_mvp is
// the only writer of g_player_cam, and on the 2026-08-13 build its body could
// not be re-derived, so that hook does not install and the camera object is
// never captured. Everything downstream then fails silently and confusingly:
// base_frame() returns false, so the controller ray never publishes, so the
// weapon writer skips every call (noray), so no barrel direction is published
// and barrel aim reports nodir forever. The 2026-08-14 headset run showed
// exactly that chain: noray=4107, rots=0, pos=0, nodir=4417, frames=0.
// The selector IS hooked and its rcx is the same camera, already validated by
// the same read_camera + mode==0 test, so it can supply the pointer until
// on_calc_mvp is re-derived.
constexpr uint64_t kSelectorProbe  = 7;   // index into kTargets
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

// Build 65: the weapon-skeleton WRITE test (cfg wskel_write, default off).
// While armed and the pick is a DRAWN gun (dhand < 0.4 m, so holstered
// gear is never written), the drain publishes the target instance + its
// rig; the kSkelPostProbe recorder adds +0.30 m of height to the
// instance origin (+0x120) and its copy (+0x250) pre-update, on the
// engine's own thread. If the update recomputes the origin from
// animation the gun floats a constant 30 cm; if it does not, the gun
// rises visibly frame over frame. Either outcome answers authority.
// Guards: rig identity re-check in the writer (pool-recycle lesson from
// the bullet work), SEH, hard write cap, cleared on any disarm.
constexpr uint64_t    kWskelWriteCap = 7200;   // ~100 s at 72 Hz
std::atomic<bool>     g_wskel_write_on{false};
std::atomic<int>      g_wskel_mode{1};
std::atomic<uint64_t> g_wskel_tgt{0};
std::atomic<uint64_t> g_wskel_tgt_rig{0};
std::atomic<uint64_t> g_wskel_writes{0};

// Build 66 (2026-08-09): MODE 2 writes the POSE ROOT instead of the instance
// origin. Mode 1 is build 65's original target and is kept only so the old
// negative can be reproduced on demand.
//
// Why the root: the 2026-08-09 wpose run proved the held weapon's bone buffer
// is MODEL space (flags bit 26 clear on every tick), so world = rootQ * boneT
// + rootT and the root is the one rigid transform carrying the whole rig. Its
// rotation is live and tracks the player's aim, so it is the real animated
// transform rather than a stale mirror.
//
// The catch this test exists to settle: rootT reads bit-identical to the
// instance +0x120 that build 65 wrote 1680 times with no visual effect. So
// either they are the same storage, or the root is regenerated from +0x120
// every frame. ACCUMULATING +0.30 m separates those outcomes by eye:
//   gun climbs away fast  = nothing overwrites us, the root IS the render
//                           input, and 6DoF is then controller pose -> root
//   gun sits ~0.30 m high = the engine rewrites the root each frame and our
//                           write lands just before it, so we need the later
//                           writer (the decomp agents are naming it)
//   gun does not move     = the root is not read by the renderer either, and
//                           the palette/draw path is the only remaining route
// Bit 26 is re-checked at write time: if the engine ever hands us a
// world-space buffer the root stops being the carrier and we must not write.
// Build 68 state. The node is republished every census tick and never cached
// across a weapon swap: FRIK and UEVR both report stale-handle bugs exactly
// there (a weapon node stays non-null while invalid, and a swap invalidates
// the object outright), so identity is re-derived, not remembered.
std::atomic<uint64_t> g_wnode_target{0};
std::atomic<int>      g_wnode_mode{0};
std::atomic<float>    g_wnode_dz{0.30f};
volatile uint64_t     g_wnode_hits = 0;
alignas(16) float     g_wnode_mat[16] = {};

// BUILD 70: THE SetWorldTransform CENSUS (cfg wnode_census).
//
// Build 68 derived the substitution target by a POINTER CHAIN GUESS,
// [[weapon instance + 0x10] + 0x18]. The 2026-08-09 headset run refuted it
// three ways at once: the tester reports that EVERY weapon he carries is
// affected, not just the held one; the derived target flaps between two values
// while the pick above it is stable and locked; and the pick's own distance to
// the gun root reads 0.080 m rather than the 0.002 m the mount census measured.
// A chain that yields a shared parent, is unstable for a stable input, and does
// not land on the gun root is simply the wrong object.
//
// So identify it the way the mount census identified Fake_gunroot: from the
// engine's own data. Record every node the engine places, with the position it
// places it at, and let the one that sits ON the gun root at 2 substitutions
// per frame name itself. Self-verifying, so a hit is real and a miss is real.
//
// Rule 8: this runs inside the hook, so it does no logging, no allocation, no
// lock and no COM. It is a direct-mapped store with a short probe. Races
// between the engine's job threads are benign here: the worst case is a lost
// sample of a node we see 144 times a second.
// BUILD 71: PROXIMITY GATE. The first census (build 70) used 512 ungated slots
// and filled 509 of them, so it stopped recording early and its ranking was
// "the nearest of whatever was placed first", not "the nearest". The engine
// places tens of thousands of nodes a second across the whole world; almost
// none of them are on the player. Gating on distance to the gun root BEFORE a
// slot is consumed spends the whole table on things attached to him, which is
// the only population the question is about. The reference is republished once
// a second by the drain tick, so it is at worst one second stale, which against
// a 1.5 m gate is irrelevant while standing still.
constexpr uint32_t kCenSlots = 2048;
struct CenEntry {
    std::atomic<uint64_t> node;
    std::atomic<uint32_t> hits;
    float p[3];
    float dmin;      // closest this node has EVER been placed to the gun root
    uint32_t prev;   // hits at the previous drain tick, to spot a LIVE node
};
CenEntry g_cen[kCenSlots];
std::atomic<int>      g_cen_on{0};

// BUILD 72: the anchor, and why the gun root cannot be it.
//
// The build 71 census showed there is no single held-weapon node. The node that
// sits ON the gun root is placed 333 times at attach and then never again,
// while the weapon's PARTS (receiver, optic, grip, barrel, magazine) are each
// their own object placed every frame at 7 to 21 cm from that bone. Moving the
// gun therefore means moving a cluster, not an object, which needs a geometric
// gate rather than an identity one.
//
// A geometric gate needs a per-frame centre. The gun root is a bone we can only
// read on the 1 Hz drain tick, which is far too stale to gate a 35 cm radius
// while the player moves. So the centre is the ANCHOR: the continuously placed
// part nearest the gun root, identified once by the census and thereafter
// tracked from the engine's own placement of it, which is fresh every frame.
// Rotating about the grip rather than the bone is also the better pivot: it is
// where the hand is.
std::atomic<uint64_t> g_wanchor{0};
std::atomic<float>    g_wanchor_pos[3];
std::atomic<int>      g_wanchor_ok{0};
std::atomic<float>    g_wnode_radius{0.35f};
// BUILD 73: the pivot must be FRESH, not merely chosen. g_wanchor_ok is cleared
// whenever the anchor changes and is set only by the hook actually seeing that
// anchor placed, so we can never rotate a cluster about a point frozen where
// the player used to be. That is what threw weapon parts to the character's
// head and feet in the build 72 screenshot.
std::atomic<uint32_t> g_wanchor_seen{0};

// BUILD 74: WHICH AXIS IS THE BARREL, MEASURED RATHER THAN GUESSED.
//
// Mode 3 rotates from "where the game is aiming" onto "where the controller
// points", and build 73 used the camera forward for the first of those. The
// tester's report pins the flaw precisely: it works in ADS, where the camera
// forward IS the barrel, and throws the receiver into his face in hip fire,
// where the character holds the rifle canted and the two are unrelated.
//
// The correct source is the weapon's own forward, which means knowing which of
// its basis rows is the barrel, and in which sign. That is exactly the kind of
// engine convention this project does not guess at, so it is measured: score
// all six signed candidates against the camera forward over a few seconds, and
// latch the winner. The gun points broadly where the player looks often enough
// that the barrel row wins decisively, and the score margin is logged so a
// marginal win can be seen rather than trusted.
std::atomic<float>    g_wanchor_basis[9];
volatile float        g_axis_score[6] = {};
std::atomic<int>      g_axis_idx{-1};       // 0..5, -1 = not yet calibrated
std::atomic<uint32_t> g_axis_samples{0};

// BUILD 73: one matrix buffer per call, round-robin. The gun is fourteen parts
// and the engine places them from several job threads, so a single shared
// buffer means one part's transform can be handed to another. Prior art for the
// exact remedy: the 6DOF Master Reference section 8D, RDR2's redirect-the-copy,
// which uses a round-robin bank for this reason. Thirty-two is far more than
// the parts in flight at once, so a buffer cannot be recycled while the engine
// is still reading it.
constexpr uint32_t kMatRing = 32;
alignas(16) float     g_wnode_ring[kMatRing][16];
std::atomic<uint32_t> g_wnode_ring_i{0};

// BUILD 184: WHERE THE ROLL GOES, two log-only probes. grwxr-21068 (183):
// after the jump and the roll the weapon's origin sat on our node-10 record
// to millimetres, the roll write and the mirror ran on every frame, and the
// roll still did not render until a death. So either (a) the gun root's
// ROTATION is rewritten after our publish (an after-camera bone pass), or
// (b) the weapon's placement takes the bone's position and composes its
// orientation elsewhere in that state. Probe A reads node 10 and the three
// mirror records back at Present, mm and deg from what we left (the arm's
// survive line, extended to the gun). Probe B latches one weapon part the
// engine places every frame within 0.15 m of our gun root (there is no
// node placed AT the root each frame; the parts sit 7 to 21 cm from it,
// build 72) and compares that part's motion since the latch with our
// written world rotation's motion: an error near 0 while we roll means the
// part follows our whole rotation; an error tracking the roll, about the
// barrel, means the roll is dropped between our record and the placement.
// Reads only, both sides. cfg wroll_probe (default 1).
std::atomic<int>      g_wroll_probe{1};
// Probe A: the last call's buffer and the four records as we left them.
float                 g_gs_ours[4][8] = {};
uint64_t              g_gs_buf = 0;
int                   g_gs_ok = 0;
std::atomic<uint64_t> g_gs_seq{0};
std::atomic<uint32_t> g_gs_out[8] = {};   // per record: mm, deg (float bits)
std::atomic<uint32_t> g_gs_n{0};
// Probe B: our written world pose, the latched part and its last matrix.
std::atomic<uint32_t> g_wr_pos[3] = {};   // our gun-root world position, float bits
std::atomic<uint32_t> g_wr_q[4]   = {};   // our written world quaternion xyzw, float bits
std::atomic<uint32_t> g_wr_roll{0};       // the filtered roll we add, radians, float bits
// BUILD 188: the INPUTS to theta, so the measurement can be judged instead of
// trusted. 187 proved theta sweeps -172..+174 deg, which no wrist produces, so
// the question is no longer how the filter behaves but what it is being fed.
// All log only. Nothing here is read by the roll, the aim or the shot chain.
std::atomic<uint32_t> g_ri_dot{0};        // gp . cp, the cosine, float bits
std::atomic<uint32_t> g_ri_sgn{0};        // (gp x cp) . barrel, the sine, float bits
std::atomic<uint32_t> g_ri_gn2{0};        // |gun up projected onto the barrel normal plane|
std::atomic<uint32_t> g_ri_cn2{0};        // |controller up, same projection|
std::atomic<uint32_t> g_ri_croll{0};      // the CONTROLLER's own roll about its own ray,
                                          // measured against the world horizon, radians.
                                          // This is what the wrist physically does. If
                                          // theta sweeps while this stays small, theta is
                                          // contaminated by something that is not the wrist,
                                          // and the shortest-arc swing's up precession is
                                          // the first suspect: the swing carries no twist
                                          // by construction, but the frame it produces has
                                          // an up that rolls as the aim direction travels.
std::atomic<uint32_t> g_ri_cu[3] = {};    // the controller up vector, world
std::atomic<uint32_t> g_ri_t[3]  = {};    // the barrel axis the roll turns about, world
std::atomic<uint64_t> g_ri_skips{0};      // times the gn2/cn2 > 0.10 degeneracy guard
                                          // rejected the sample and left s_roll frozen
std::atomic<uint64_t> g_ri_n{0};          // times the angle was computed
std::atomic<uint64_t> g_ri_flips{0};      // BUILD 189: times the gun-up flip was ENTERED
std::atomic<uint32_t> g_ri_flipped{0};    // BUILD 189: 1 while the correction is applied
std::atomic<uint32_t> g_wr_theta{0};      // BUILD 187: the MEASURED roll before the filter,
                                          // radians, float bits. theta is bounded by atan2f
                                          // plus the trim; s_roll is the accumulator that
                                          // follows it. Logging both is what separates "the
                                          // filter wound up" from "the measurement is wrong".
std::atomic<uint64_t> g_wr_node{0};       // the latched part (a TransformNode)
alignas(16) float     g_wr_mat[16] = {};  // its last placement
std::atomic<uint64_t> g_wr_seq{0};        // odd while g_wr_mat is being written
std::atomic<uint64_t> g_wr_hits{0};
// BUILD 190: the own-the-bone path, log only. Nothing here is read by the
// roll, the aim or the shot chain.
std::atomic<uint32_t> g_ro_on{0};         // 1 when the last call built the orientation itself
std::atomic<uint32_t> g_ro_roll{0};       // the up we wrote, against the horizon about the barrel, radians
std::atomic<uint32_t> g_ro_tgt{0};        // the controller's up the same way, before the filter
std::atomic<uint32_t> g_ro_croll{0};      // the controller's own roll about its ray (build 188's croll)
std::atomic<uint32_t> g_ro_up[3] = {};    // the up written, world
std::atomic<uint32_t> g_ro_t[3]  = {};    // the barrel, world
std::atomic<uint64_t> g_ro_n{0};          // calls that wrote the orientation
std::atomic<uint64_t> g_ro_holds{0};      // calls with no usable controller up, the last up held
std::atomic<uint64_t> g_ro_horizon{0};    // calls seeded upright from the world up
// BUILD 191: THE FOV ROOT (cfg fov_root). Item 4.6, the clouds. State for the
// write into Camera+0x2B0 on the mode-0 camera at the selector, its
// diagnostics, and proj[2]'s fovy BEFORE the fullscreen override, which is
// also the pre-override capture item 4.3 asked for.
std::atomic<int>      g_fov_root{0};        // cfg fov_root: 1 = write the root; 0 = off, restores
std::atomic<uint32_t> g_fr_orig{0};         // the engine-authored bits of Camera+0x2B0, last seen
std::atomic<uint32_t> g_fr_written{0};      // the bits we wrote
std::atomic<uint32_t> g_fr_armed{0};        // 1 while the field is believed to hold our bits
std::atomic<uint32_t> g_fr_eng{0};          // proj[2] fovy before the override, float bits
std::atomic<uint32_t> g_fr_ratio{0};        // proj[2] pre-override fovy / Camera+0x2B0, float bits
std::atomic<uint32_t> g_fr_splay{0};        // that ratio, latched while the field was engine-authored and in the world band
std::atomic<uint32_t> g_fr_narrow{0};       // 1 while the engine is narrowing (ADS, scope): hands off, restore
std::atomic<uint32_t> g_fr_f2B0{0};         // diagnostics, float bits, the player camera only
std::atomic<uint32_t> g_fr_f2BC{0};
std::atomic<uint32_t> g_fr_f2C0{0};
std::atomic<uint32_t> g_fr_f798{0};
std::atomic<uint32_t> g_fr_lastmode{0};     // the last non-zero Camera+0x290 the selector saw
std::atomic<uint64_t> g_fr_ticks{0};
std::atomic<uint64_t> g_fr_writes{0};
std::atomic<uint64_t> g_fr_restores{0};
std::atomic<uint64_t> g_fr_skip_band{0};
std::atomic<uint64_t> g_fr_skip_narrow{0};
std::atomic<uint64_t> g_fr_notplayer{0};
std::atomic<uint64_t> g_fr_seh{0};
// Review fixes, 2026-09-05: ownership is per camera; proj[2] samples are
// paired to the mode-0 tick that preceded them; the play ratio latches only
// when two paired samples agree; a watchdog re-latches if the armed state
// runs over-wide for about a second.
std::atomic<uint64_t> g_fr_cam{0};          // the camera whose field holds our bits
std::atomic<uint32_t> g_fr_token{0};        // 1 between a mode-0 tick and the proj[2] call that follows it
std::atomic<uint32_t> g_fr_pair_field{0};   // the field bits that tick left behind, for that call
std::atomic<uint32_t> g_fr_pair_ours{0};    // whether those bits are ours
std::atomic<uint32_t> g_fr_rprev{0};        // the previous paired ratio, float bits (the stability test)
std::atomic<uint32_t> g_fr_high{0};         // consecutive paired samples with ratio/splay above 1.30 while ours
std::atomic<uint32_t> g_fr_relatch{0};      // 1 = restore and forget the play ratio at the next tick
std::atomic<uint64_t> g_fr_skip_nosplay{0}; // ticks that wrote nothing because no play ratio has latched yet
std::atomic<uint64_t> g_fr_relatches{0};
std::atomic<uint64_t> g_fr_skip_target{0};  // ticks whose target fell outside (0.30, 3.0)
std::atomic<uint64_t> g_fr_foreign{0};      // mode-0 ticks on a camera other than the one we own, left untouched
// BUILD 194
std::atomic<int>      g_fov_root_mode{1};   // cfg fov_root_mode: 0 = hold the field all frame; 1 = restore it right after the projection call that consumed it
std::atomic<uint32_t> g_fov_root_aimk{0x3F800000u}; // cfg fov_root_aimk, float bits (1.0): gain on injected head-aim deltas while the root is armed
std::atomic<uint64_t> g_fr_owner_tick{0};   // g_fr_ticks at the owned camera's last tick (the age-out)
std::atomic<uint64_t> g_fr_releases{0};     // mode 1: restores made at the consuming projection call
std::atomic<uint64_t> g_fr_ownerlost{0};    // ownership dropped because the owned camera stopped ticking
// BUILD 195: one write per frame in mode 1, on the tick the consumer follows.
std::atomic<uint64_t> g_fr_write_tick{0};   // g_fr_ticks at the last write
std::atomic<uint64_t> g_fr_release_tick{0}; // g_fr_ticks at the last release
std::atomic<int>      g_fr_phase{-1};       // the write tick's index modulo 3 that a release followed; -1 unknown
std::atomic<uint64_t> g_fr_skip_phase{0};   // ticks that did not write because they are not that phase
std::atomic<uint64_t> g_fr_wticks{0};       // BUILD 196: the player camera's own ticks only (the phase counter)

// BUILD 197: THE SOURCE FOV (the clouds, item 4.6 step 2), behind cfg fov_src.
// The engine's camera director, in its normal gameplay mode, builds the player
// camera's fov as (gameplay camera component fov) + (CameraPoseOffset+0x30) and
// stores the sum into Camera+0x2B0 itself, once per Present, with its own
// instruction (kCamOffsetSig resolves the consumer; the add is the
// addss xmm6,[rax+0x30] that follows it). The offset is 0 from construction and
// no engine writer of it has been seen (RE-notes, THE CAMERA POSE OFFSET). One
// float, fs_fov minus the engine's own fov, written there makes the ENGINE
// author the wide field for the whole frame, so the sky, both recomputes and
// every other reader agree and the mod never writes the camera. Build 193 held
// the field wide by writing the camera after the engine's re-author: clouds
// fixed, head feel off. Build 196 proved the sky reads the field after the
// consuming projection call. This removes the disagreement instead of racing it.
std::atomic<uint32_t> g_fov_src{0};          // cfg fov_src
std::atomic<uint64_t> g_fs_cam{0};           // the mode-0 camera whose field is our gauge
std::atomic<uint64_t> g_fs_owner_tick{0};    // g_fr_ticks at that camera's last tick (the age-out)
std::atomic<uint64_t> g_fs_obj{0};           // the CameraPoseOffset object, re-read every tick
std::atomic<uint32_t> g_fs_off{0};           // the offset bits we last wrote; 0 = nothing of ours in there
std::atomic<uint32_t> g_fs_pre{0};           // the field bits at the tick we wrote (the not-applied-yet gauge)
std::atomic<uint32_t> g_fs_comp{0};          // the engine's own fov as last derived (field minus offset), float bits
std::atomic<uint32_t> g_fs_mode34{0};        // CameraPoseOffset+0x34 as last read (1 = additive; 0 = never write)
std::atomic<uint32_t> g_fs_ident{0};         // 1 = the fov add was found after the consumer on this image
std::atomic<uint64_t> g_fs_scene{0};         // [[cam+0x748]+0x30], the view's scene object (identity, read only)
std::atomic<uint32_t> g_fs_s286{0};          // its +0x286, the free camera's fov override enable (0 in play)
std::atomic<uint32_t> g_fs_s290{0};          // its +0x290, float bits
std::atomic<uint32_t> g_fs_armed{0};         // 1 while a non-zero offset of ours is in the object
std::atomic<uint32_t> g_fs_disabled{0};      // 1 = stood down for the run (three flips or three unhonoured writes)
std::atomic<uint32_t> g_fs_rapid{0};         // flips inside the last 600 ticks
std::atomic<uint64_t> g_fs_rapid_tick{0};
std::atomic<uint64_t> g_fs_write_tick{0};    // g_fr_ticks at the last write
std::atomic<uint64_t> g_fs_last_zero_tick{0};
std::atomic<uint64_t> g_fs_backoff_until{0}; // g_fr_ticks before which nothing is written
std::atomic<uint64_t> g_fs_offw{0};          // non-zero offset writes
std::atomic<uint64_t> g_fs_zeroes{0};        // zero writes (stand-downs and band exits)
std::atomic<uint64_t> g_fs_pending{0};       // ticks spent waiting for the engine to apply a write
std::atomic<uint64_t> g_fs_skip_band{0};     // ticks with the engine's own fov outside the world band
std::atomic<uint64_t> g_fs_skip_target{0};   // ticks whose target fell outside (-3, 3)
std::atomic<uint64_t> g_fs_foreign{0};       // +0x30 held a value that is neither 0 nor ours
std::atomic<uint64_t> g_fs_foreign_cam{0};   // mode-0 ticks on a camera other than the gauge
std::atomic<uint64_t> g_fs_flips{0};         // non-zero writes within four Presents of a zero
std::atomic<uint64_t> g_fs_unhonoured{0};    // ticks past nine with our write in and the field where it was
std::atomic<uint64_t> g_fs_unapplied{0};     // ticks whose field was authored without our offset (the blend path, another director mode)
std::atomic<uint32_t> g_fs_replaced{0};      // our live offset replaced by a value we did not write
std::atomic<uint32_t> g_fs_oob{0};           // consecutive ticks with the engine's own fov outside the band while ours is in
std::atomic<uint64_t> g_fs_applied_tick{0};  // g_fr_ticks at the last write or applied frame (the hygiene clock)
std::atomic<uint64_t> g_fs_seen_applied{0};  // g_fr_ticks at the last frame that SHOWED our offset applied (never stamped by a write)
std::atomic<uint32_t> g_fs_foreign_run{0};   // consecutive mode-0 ticks on a camera other than the gauge
std::atomic<uint32_t> g_fs_since_own{0};     // ticks of any camera since the gauge last ticked
std::atomic<uint64_t> g_fs_rebuilt{0};       // the offset object went away or changed identity while ours was in it
std::atomic<uint64_t> g_fs_seh{0};
std::atomic<uint8_t*> g_fs_img_lo{nullptr};  // the module bounds: the object must never be inside them
std::atomic<uint8_t*> g_fs_img_hi{nullptr};

// BUILD 198: THE SCENE FOV OVERRIDE (the clouds, item 4.6 step 2b), behind cfg
// fov_scene. Build 197 wrote the pose-offset fov and the engine never added it:
// in play the field read the engine's own value at every tick with the offset
// in place (grwxr-26920, 2026-09-06: off 1.0956, f2B0 0.8244, unhonoured
// climbing every tick, never once applied). So the director's mode-1 apply is
// not what authors the player camera's field in play, or the virtual after the
// add discards the sum. The scene object of the camera's own view carries a
// fov override the engine keeps for its free camera: a byte at +0x286 and a
// float at +0x290, and the three writers of Camera+0x2B0 that author it in
// play select on it before storing (the pre-recompute publish, the mode-1
// apply and the blend path: one hit each on the installed exe, and the only
// three instructions image-wide that pair +0x286 with +0x290; the dispatcher's
// inline stores for a blend in progress and for director mode 3 are not known
// to select on it, so unhonoured may climb about one per Present through
// those windows, and dblend and dmode on the line say when). With the byte
// set and the float at fs_fov, the engine stores fs_fov into the field itself
// before every recompute. Costs, known: while set, the
// engine's own fov is not observable from the field, so the aim-down-sights
// gate cannot come from the field; it comes from the aim trigger instead (cfg
// fov_scene_ads: the override is released while the merged left trigger is
// past XInput's own threshold, and the engine's zoom lands one Present later,
// as on build 190); the menu and the load screen render at fs_fov while set.
// Rules: the object is the view's scene object, re-read every tick and never
// inside the module; never written while +0x286 is non-zero and not ours (the
// free camera owns it); +0x290 first, then +0x286, and the reverse to clear;
// the original +0x290 bits go back on every clear; ours replaced three times
// stands the route down for the run; the camera leaving mode 0 clears ours;
// fov_src and fov_root are forced off while this is on. Read-only alongside:
// the director global (kDirectorSig, the dispatcher's own rip-relative load),
// its mode at +0x6C and its blend byte at +0x60 at every player camera tick,
// and a histogram of that mode: the one thing the 197 log could not say.
std::atomic<uint32_t> g_fov_scene{1};         // cfg fov_scene; BUILD 199: default 1, CONFIRMED 2026-09-06 (the clouds, CX-014 closed)
std::atomic<uint32_t> g_fov_scene_ads{1};     // cfg fov_scene_ads: release while the aim trigger is held
std::atomic<uint32_t> g_ads_input{0};         // the merged left trigger past the threshold (XInputMerge)
std::atomic<uint32_t> g_sc_ident{0};          // kSiteBStoreSig found once: this image selects on +0x286
std::atomic<uint64_t> g_sc_cam{0};            // the mode-0 camera whose view's scene object carries ours
std::atomic<uint64_t> g_sc_owner_tick{0};
std::atomic<uint32_t> g_sc_foreign_run{0};    // consecutive mode-0 ticks on a camera other than ours
std::atomic<uint64_t> g_sc_scene{0};          // the scene object that carries ours (0 = nothing of ours anywhere)
std::atomic<uint64_t> g_sc_scene_seen{0};     // the scene object as last read (identity, read only)
std::atomic<uint32_t> g_sc_orig290{0};        // its +0x290 bits before ours, put back on every clear
std::atomic<uint32_t> g_sc_wrote290{0};       // the +0x290 bits we wrote (fs_fov at the time)
std::atomic<uint32_t> g_sc_armed{0};          // 1 while +0x286 = 1 and +0x290 = ours in that object
std::atomic<uint32_t> g_sc_disabled{0};       // 1 = stood down for the run (ours replaced three times)
std::atomic<uint32_t> g_sc_replaced{0};
std::atomic<uint64_t> g_sc_set_tick{0};
std::atomic<uint64_t> g_sc_seen_applied{0};   // g_fr_ticks at the last tick whose field read our fov
std::atomic<uint64_t> g_sc_sets{0};
std::atomic<uint64_t> g_sc_clears{0};
std::atomic<uint64_t> g_sc_ads_clears{0};     // clears made for the aim trigger
std::atomic<uint64_t> g_sc_ads_ticks{0};      // ticks with the aim trigger held
std::atomic<uint64_t> g_sc_applied{0};        // ticks whose field read our fov while ours was set
std::atomic<uint64_t> g_sc_unhonoured{0};     // ticks past nine since the set with the field elsewhere
std::atomic<uint64_t> g_sc_foreign{0};        // +0x286 non-zero and not ours, or ours replaced
std::atomic<uint64_t> g_sc_rebuilt{0};        // the scene object changed identity while ours was in it
std::atomic<uint64_t> g_sc_noscene{0};        // no readable off-module view or scene object at the tick
std::atomic<uint64_t> g_sc_seh{0};
std::atomic<uint32_t> g_sc_s286{0};
std::atomic<uint32_t> g_sc_s290{0};
std::atomic<uint32_t> g_sc_s14d{0};           // scene+0x14D, the per-camera gate byte the mode-1 apply checks
std::atomic<uint64_t> g_dir_obj{0};           // the director object as last read
std::atomic<uint32_t> g_dir_mode{0};          // [director+0x6C]
std::atomic<uint32_t> g_dir_blend{0};         // [director+0x60], the blend byte
std::atomic<uint64_t> g_dir_hist[8] = {};     // director mode at player camera ticks: 0..6, and 7 for anything higher
std::atomic<uint64_t> g_dir_reads{0};
std::atomic<uint64_t> g_dir_bad{0};           // director pointer null, unreadable or inside the module

// BUILD 200: THE SPRINT PROBE (item 4.13, CX-020), log only, behind cfg
// sprint_probe (default 1). The weapon sinks into the body at a sprint start.
// The candidate: the hands and the gun are pinned to a camera the engine
// moves relative to the body, so at a sprint start or stop the body moves
// against the camera and the gun looks a beat late. The port study measured
// exactly this on REDengine: the first-person camera rides about 0.20 m ahead
// of the entity in a sprint and the lead collapses in 0.13 s at the stop. This
// build only measures. Ten times a second, on the present thread (reads only,
// no logging), one sample of: the camera the frame is composed with
// (g_vr_cam_pos), the view forward, the Head, Spine1 and Hips bones, the gun
// root and the right hand in world (read_bone_world on the armed skeleton),
// and the merged left stick and buttons. The samples go into a ring the 1 Hz
// heartbeat drains into `sprint:` lines, so a sprint's start and stop are on
// record at 100 ms resolution: the camera's lead over the body along the view
// forward, the gun's offset from the camera and from the spine, the speeds.
constexpr uint32_t kSpRing = 32;
struct SpSample {
    uint32_t ms;        // GetTickCount64, low 32 bits
    uint32_t ok;        // bit0 cam, bit1 head, bit2 spine, bit3 hips, bit4 gun, bit5 rhand, bit6 fwd
    float cam[3], fwd[3], head[3], spine[3], hips[3], gun[3], rhand[3];
    float lx, ly;
    uint32_t btn;
};
std::atomic<uint32_t> g_sprint_probe{1};
SpSample g_sp_ring[kSpRing] = {};
std::atomic<uint32_t> g_sp_wr{0};        // samples written; the next slot is wr % kSpRing
std::atomic<uint64_t> g_sp_skel{0};      // the skeleton the node indices below belong to
std::atomic<int> g_sp_head{-1};
std::atomic<int> g_sp_spine{-1};
std::atomic<int> g_sp_hips{-1};
std::atomic<float> g_mv_lx{0.0f};        // the merged left stick, as the game sees it (XInputMerge)
std::atomic<float> g_mv_ly{0.0f};
std::atomic<uint32_t> g_mv_btn{0};
std::atomic<uint64_t> g_sp_samples{0};
std::atomic<uint64_t> g_sp_noskel{0};    // ticks with no armed skeleton (nothing to measure)
std::atomic<uint64_t> g_sp_dropped{0};   // samples the drain found overwritten

// BUILD 73: ONE COUNTER PER GATE. This is the third time a single counter
// behind several early returns has cost a headset run. Mode 2's subs=0 was read
// as "the node never matches" when it was the controller-ray gate; build 72's
// frozen subs has three candidate explanations (no anchor, stale pivot, or a
// controller that was asleep at load) and the instrument cannot separate them.
// A counter per reason turns every future "nothing happened" into a sentence.
volatile uint64_t g_gate_noanchor = 0;   // no anchor elected
volatile uint64_t g_gate_stale    = 0;   // anchor elected but not yet placed
volatile uint64_t g_gate_outside  = 0;   // part outside the radius, correct
volatile uint64_t g_gate_noview   = 0;   // the game's aim direction unavailable
volatile uint64_t g_gate_noray    = 0;   // the controller ray unavailable
std::atomic<uint32_t> g_cen_seen{0};      // in-radius placements observed
std::atomic<uint32_t> g_cen_rejected{0};  // out-of-radius, never given a slot
std::atomic<float>    g_cen_ref[3];
std::atomic<int>      g_cen_ref_ok{0};
std::atomic<float>    g_cen_radius{1.5f};

inline void census_note(uint64_t n, const float* m) {
    if (!g_cen_ref_ok.load(std::memory_order_relaxed)) return;
    const float rx = g_cen_ref[0].load(std::memory_order_relaxed);
    const float ry = g_cen_ref[1].load(std::memory_order_relaxed);
    const float rz = g_cen_ref[2].load(std::memory_order_relaxed);
    const float dx = m[12] - rx, dy = m[13] - ry, dz = m[14] - rz;
    const float d2 = dx*dx + dy*dy + dz*dz;
    const float rad = g_cen_radius.load(std::memory_order_relaxed);
    if (!(d2 <= rad * rad)) {
        g_cen_rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const float d = sqrtf(d2);
    g_cen_seen.fetch_add(1, std::memory_order_relaxed);

    uint32_t i = (uint32_t)((n >> 4) ^ (n >> 21)) & (kCenSlots - 1);
    for (int probe = 0; probe < 4; ++probe, i = (i + 1) & (kCenSlots - 1)) {
        const uint64_t cur = g_cen[i].node.load(std::memory_order_relaxed);
        if (cur == n) {
            g_cen[i].hits.fetch_add(1, std::memory_order_relaxed);
            g_cen[i].p[0] = m[12]; g_cen[i].p[1] = m[13]; g_cen[i].p[2] = m[14];
            // Rank on the CLOSEST this node has ever been placed, not the last
            // sample. A weapon node passes through its bind position during a
            // draw animation, and a last-sample ranking taken mid-animation
            // would push the right answer down the list.
            if (d < g_cen[i].dmin) g_cen[i].dmin = d;
            return;
        }
        if (cur == 0) {
            uint64_t expect = 0;
            if (g_cen[i].node.compare_exchange_strong(expect, n,
                                                      std::memory_order_relaxed)) {
                g_cen[i].hits.store(1, std::memory_order_relaxed);
                g_cen[i].p[0] = m[12]; g_cen[i].p[1] = m[13]; g_cen[i].p[2] = m[14];
                g_cen[i].dmin = d;
            }
            return;
        }
    }
    // Four collisions: drop the sample. A node placed twice a frame will win a
    // slot within the first few frames regardless.
}

// Build 68: called from grwxr_wnode_entry for EVERY TransformNode placement in
// the game, so the first thing it does is the identity gate. Returns null for
// everything that is not our weapon, and the stub then leaves the engine's own
// argument completely untouched.
//
// Rule 8 holds: no logging, no lock, no allocation, no COM.
//
// Mode 1 offsets the translation row: the mechanism test. Unlike the bone
// write it CANNOT be overwritten, so a negative here means the drawn mesh does
// not follow this transform, rather than meaning we lost a race.
// Mode 2 is the feature: keep the engine's position, so the gun stays in the
// hand, and rebuild the rotation basis so the barrel follows the controller
// ray. That needs no controller-position plumbing, which is why it ships now.
// Build 72: the rotation that carries direction a onto direction b, both unit,
// as a row-major 3x3. Convention-free by construction: we never have to know
// which local axis of a weapon part is its barrel, because we rotate the part
// by a world-space delta rather than rebuilding its basis from scratch. Build
// 68's mode 2 did rebuild it, guessing the axis order, and that guess is what
// put the rifle sideways in the tester's screenshot.
//
// If a == b the result is the identity, so pointing the controller where you
// are already looking changes nothing. That makes the whole feature safe at
// its own boundary rather than by a guard.
inline void rot_between(const float a[3], const float b[3], float R[9]) {
    const float c = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    float k[3] = {a[1]*b[2] - a[2]*b[1],
                  a[2]*b[0] - a[0]*b[2],
                  a[0]*b[1] - a[1]*b[0]};
    const float s = sqrtf(k[0]*k[0] + k[1]*k[1] + k[2]*k[2]);
    if (s < 1e-6f) {   // parallel, or antiparallel which we decline to invent
        R[0]=1; R[1]=0; R[2]=0; R[3]=0; R[4]=1; R[5]=0; R[6]=0; R[7]=0; R[8]=1;
        return;
    }
    k[0] /= s; k[1] /= s; k[2] /= s;
    const float ang = atan2f(s, c), C = cosf(ang), S = sinf(ang), t = 1.0f - C;
    R[0] = t*k[0]*k[0] + C;        R[1] = t*k[0]*k[1] - S*k[2];  R[2] = t*k[0]*k[2] + S*k[1];
    R[3] = t*k[0]*k[1] + S*k[2];   R[4] = t*k[1]*k[1] + C;       R[5] = t*k[1]*k[2] - S*k[0];
    R[6] = t*k[0]*k[2] - S*k[1];   R[7] = t*k[1]*k[2] + S*k[0];  R[8] = t*k[2]*k[2] + C;
}

inline void rot_apply(const float R[9], const float v[3], float o[3]) {
    o[0] = R[0]*v[0] + R[1]*v[1] + R[2]*v[2];
    o[1] = R[3]*v[0] + R[4]*v[1] + R[5]*v[2];
    o[2] = R[6]*v[0] + R[7]*v[1] + R[8]*v[2];
}

extern "C" const void* grwxr_wnode_prep(void* node, const float* m) {
    // Build 70: the census sees EVERY placement, before any identity gate, so
    // it can name the node we should have been targeting all along. It never
    // modifies anything, so it is safe to run with wnode = 0.
    if (m && node && g_cen_on.load(std::memory_order_relaxed))
        census_note((uint64_t)node, m);

    // BUILD 184, probe B: latch the first part placed within 0.15 m of our
    // gun root, then record that part's matrix on every placement. Never
    // modifies anything; a seqlock guards the copy for the 1 Hz reader.
    if (m && node && g_wroll_probe.load(std::memory_order_relaxed) &&
        g_gs_seq.load(std::memory_order_relaxed) != 0) {
        const uint64_t want = g_wr_node.load(std::memory_order_relaxed);
        bool take = false;
        if (want) {
            take = ((uint64_t)node == want);
        } else {
            float p[3];
            for (int i = 0; i < 3; ++i) {
                const uint32_t b = g_wr_pos[i].load(std::memory_order_relaxed);
                memcpy(&p[i], &b, sizeof(p[i]));
            }
            const float dx = m[12] - p[0], dy = m[13] - p[1], dz = m[14] - p[2];
            if (dx*dx + dy*dy + dz*dz < 0.15f * 0.15f) {   // NaN compares false
                uint64_t expect = 0;
                take = g_wr_node.compare_exchange_strong(
                    expect, (uint64_t)node, std::memory_order_relaxed);
            }
        }
        if (take) {
            // Single writer by CAS (even -> odd). A second job thread
            // placing the same node at the same instant, or a thread past
            // its identity check when the tick relatches, drops its sample
            // rather than interleaving two copies under one even count.
            uint64_t s0 = g_wr_seq.load(std::memory_order_relaxed);
            if (!(s0 & 1) &&
                g_wr_seq.compare_exchange_strong(s0, s0 + 1,
                                                 std::memory_order_acq_rel)) {
                memcpy(g_wr_mat, m, sizeof(g_wr_mat));
                g_wr_seq.fetch_add(1, std::memory_order_release);   // even: done
                g_wr_hits.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    const int mode = g_wnode_mode.load(std::memory_order_relaxed);
    if (mode <= 0 || !m) return nullptr;

    // BUILD 72, MODE 3: the geometric gate. Any part the engine places within
    // the radius of the anchor is part of the held weapon and gets the same
    // rigid rotation about it. The pistol on the leg is far outside, so it is
    // excluded without being identified, and so is everything else in the
    // world. This is what replaces build 68's search for a single node that
    // the census proved does not exist.
    if (mode == 3) {
        const uint64_t anchor = g_wanchor.load(std::memory_order_relaxed);
        if (!anchor) { g_gate_noanchor = g_gate_noanchor + 1; return nullptr; }
        __try {
            // Track the anchor from the engine's own placement of it, and only
            // then declare the pivot usable. Used one frame later at worst,
            // which at 72 Hz is below perception.
            if ((uint64_t)node == anchor) {
                g_wanchor_pos[0].store(m[12], std::memory_order_relaxed);
                g_wanchor_pos[1].store(m[13], std::memory_order_relaxed);
                g_wanchor_pos[2].store(m[14], std::memory_order_relaxed);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        g_wanchor_basis[r*3+c].store(m[r*4+c],
                                                     std::memory_order_relaxed);
                g_wanchor_ok.store(1, std::memory_order_relaxed);
                g_wanchor_seen.fetch_add(1, std::memory_order_relaxed);
                // Score the six signed basis candidates against the camera
                // forward. Whichever is the barrel wins over a few seconds.
                float vf[3];
                if (g_axis_idx.load(std::memory_order_relaxed) < 0 &&
                    aimtrace::view_fwd(vf)) {
                    for (int r = 0; r < 3; ++r) {
                        const float dot = m[r*4+0]*vf[0] + m[r*4+1]*vf[1] +
                                          m[r*4+2]*vf[2];
                        g_axis_score[r]     = g_axis_score[r]     + dot;
                        g_axis_score[r + 3] = g_axis_score[r + 3] - dot;
                    }
                    g_axis_samples.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (!g_wanchor_ok.load(std::memory_order_relaxed)) {
                g_gate_stale = g_gate_stale + 1;
                return nullptr;
            }
            const float ax = g_wanchor_pos[0].load(std::memory_order_relaxed);
            const float ay = g_wanchor_pos[1].load(std::memory_order_relaxed);
            const float az = g_wanchor_pos[2].load(std::memory_order_relaxed);
            const float dx = m[12] - ax, dy = m[13] - ay, dz = m[14] - az;
            const float rad = g_wnode_radius.load(std::memory_order_relaxed);
            if (dx*dx + dy*dy + dz*dz > rad * rad) {
                g_gate_outside = g_gate_outside + 1;
                return nullptr;
            }

            float aim[3], ray[3];
            if (!aimtrace::view_fwd(aim)) { g_gate_noview = g_gate_noview + 1; return nullptr; }
            if (!aimtrace::ctrl_ray(ray)) { g_gate_noray  = g_gate_noray  + 1; return nullptr; }
            // BUILD 74: once the barrel axis is known, rotate from the GUN's
            // own forward rather than the camera's. Until then the camera
            // forward is used, which is the build 73 behaviour and is correct
            // in ADS, so the calibration period degrades to what already
            // worked rather than to something new.
            const int ai = g_axis_idx.load(std::memory_order_relaxed);
            if (ai >= 0) {
                const int r = ai % 3;
                const float sgn = (ai < 3) ? 1.0f : -1.0f;
                float g[3] = {
                    sgn * g_wanchor_basis[r*3+0].load(std::memory_order_relaxed),
                    sgn * g_wanchor_basis[r*3+1].load(std::memory_order_relaxed),
                    sgn * g_wanchor_basis[r*3+2].load(std::memory_order_relaxed)};
                const float gn = sqrtf(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
                if (gn > 1e-4f) {
                    aim[0] = g[0] / gn; aim[1] = g[1] / gn; aim[2] = g[2] / gn;
                }
            }
            float R[9];
            rot_between(aim, ray, R);

            // One buffer per call: the gun is many parts placed from several
            // job threads, and a shared buffer hands one part another's matrix.
            const uint32_t ri =
                g_wnode_ring_i.fetch_add(1, std::memory_order_relaxed) & (kMatRing - 1);
            float* const dst = g_wnode_ring[ri];
            memcpy(dst, m, sizeof(g_wnode_ring[0]));
            // Rotate the part rigidly about the anchor: its three basis rows
            // are world directions, and its position is a world point.
            for (int r = 0; r < 3; ++r) {
                const float row[3] = {m[r*4+0], m[r*4+1], m[r*4+2]};
                float out[3];
                rot_apply(R, row, out);
                dst[r*4+0] = out[0];
                dst[r*4+1] = out[1];
                dst[r*4+2] = out[2];
            }
            const float rel[3] = {dx, dy, dz};
            float rot[3];
            rot_apply(R, rel, rot);
            dst[12] = ax + rot[0];
            dst[13] = ay + rot[1];
            dst[14] = az + rot[2];
            g_wnode_hits = g_wnode_hits + 1;
            return dst;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    const uint64_t want = g_wnode_target.load(std::memory_order_relaxed);
    if (!want || (uint64_t)node != want) return nullptr;

    __try {
        memcpy(g_wnode_mat, m, sizeof(g_wnode_mat));
        if (mode >= 2) {
            // The gun points along the SAME ray the bullets ride, so the two
            // cannot diverge: that divergence is exactly the defect class
            // behind the tester's ADS complaint.
            float d[3];
            if (!aimtrace::ctrl_ray(d)) return nullptr;
            const float dn = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            if (!(dn > 1e-4f)) return nullptr;
            d[0] /= dn; d[1] /= dn; d[2] /= dn;
            // World up is z here. Re-derive a true up after the cross so the
            // basis stays orthonormal even aiming straight up or down.
            float up[3] = {0.0f, 0.0f, 1.0f};
            if (fabsf(d[2]) > 0.999f) { up[0] = 1.0f; up[2] = 0.0f; }
            float r[3] = {up[1]*d[2] - up[2]*d[1],
                          up[2]*d[0] - up[0]*d[2],
                          up[0]*d[1] - up[1]*d[0]};
            const float rn = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
            if (!(rn > 1e-4f)) return nullptr;
            r[0] /= rn; r[1] /= rn; r[2] /= rn;
            const float u[3] = {d[1]*r[2] - d[2]*r[1],
                                d[2]*r[0] - d[0]*r[2],
                                d[0]*r[1] - d[1]*r[0]};
            g_wnode_mat[0] = r[0]; g_wnode_mat[1] = r[1]; g_wnode_mat[2]  = r[2];
            g_wnode_mat[4] = d[0]; g_wnode_mat[5] = d[1]; g_wnode_mat[6]  = d[2];
            g_wnode_mat[8] = u[0]; g_wnode_mat[9] = u[1]; g_wnode_mat[10] = u[2];
        } else {
            g_wnode_mat[14] += g_wnode_dz.load(std::memory_order_relaxed);
        }
        g_wnode_hits = g_wnode_hits + 1;
        return g_wnode_mat;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void wskel_write_apply(uint64_t tgt, uint64_t rig) {
    __try {
        if (!rig || *(uint64_t*)(tgt + 0x220) != rig) return;
        if (g_wskel_mode.load(std::memory_order_relaxed) >= 2) {
            const uint64_t pose = *(uint64_t*)(tgt + 0x238);
            if (pose < 0x10000 || (pose & 7)) return;
            if (*(uint32_t*)(pose + 0x8C) & 0x04000000u) return;  // world space
            *(float*)(pose + 0x08) += 0.30f;   // rootT z lane (game up axis)
        } else {
            *(float*)(tgt + 0x128) += 0.30f;   // +0x120 origin, z lane
            *(float*)(tgt + 0x258) += 0.30f;   // +0x250 copy, z lane
        }
        g_wskel_writes.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
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

// BUILD 131: every cPlayerComponent seen this session, not just the newest.
// See the capture site for why the single pointer was the bug. Sized and
// filled exactly like g_skel_rcx, through the same capture_unique().
std::atomic<uint64_t> g_pc_tab[kSkelPtrs] = {};
std::atomic<uint64_t> g_player_entity{0};

// BUILD 132: CX-001. LOG ONLY, nothing here changes a decision.
//
// Owner, 2026-08-23: "it almost looked like the camera was trying to decide
// what it attaches to as when i load in there is a helicopter flying overhead
// and it has gotten attached to that", and separately that the player is
// thumbstick-controllable DURING the intro swoop, before the body is visible.
//
// `[VERIFIED, log grwxr-23964]` the anchor took 58 SECONDS to engage: ten
// samples of "NOT anchored, camera rides the third-person rig, reads=0" before
// it flipped. Until it engages the mod places nothing, so a short run measures
// nothing about the gun. That is the cost being measured here.
//
// Three numbers, and each answers a question no existing counter answers:
//   g_pin_changes    how many DISTINCT entities the pin has landed on. This is
//                    "trying to decide what it attaches to", counted.
//   g_pin_first_ms   when the pin first landed on anything.
//   g_anchor_first_ms  when the anchor first engaged. The delta is the cost.
//
// The entity's VTABLE is logged rather than resolved in process: it is one
// 8-byte guarded read at an event that happens a handful of times per session,
// and the class name comes from tools/anvil_symbols.py OFFLINE against the
// 5,439 named classes. No symbol lookup, no allocation, no engine call.
// That is what makes "is it the helicopter" answerable without guessing.
std::atomic<uint32_t> g_pin_changes{0};
std::atomic<uint64_t> g_pin_first_ms{0};
std::atomic<uint64_t> g_anchor_first_ms{0};

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
// BUILD 125: +1 = the sign every build since 10m has used, -1 = the other one.
// See the long comment at the offset site. This is the A/B that build 93 said
// was needed and that no log line can settle.
std::atomic<int> g_ipd_sign{1};

// 2026-09-01: THE HORIZON LOCK (cfg cam_level_horizon, default 1).
//
// The owner, from the headset: "the camera wasn't normalized to the horizon as
// a 6dof should be", and separately "we need to be able to roll the head and
// eventually lean left or right".
//
// Those are one problem. The rotation write composes rows' = H * base, where H
// is your head against a yaw-only reference (so your roll is already in it) and
// base is the ENGINE's own chase camera. Any roll the engine has goes straight
// through that multiply into your eyes, and nothing removes it. Inheriting a
// third-person camera's orientation wholesale is not a 6DOF camera.
//
// The cure was already sitting here as the aim_cum branch below, which rebuilds
// the basis from yaw and pitch alone; note its rebuilt[2] = 0, forcing the right
// vector horizontal, which is a roll-free basis by construction. It just never
// ran unless head-aim accumulation was non-zero. This makes it unconditional
// while the lock is on, with the aim terms at zero when there is no aim to
// subtract.
//
// It levels the BASE, never H, so your own head roll is untouched: it is the
// only remaining source of roll, which is exactly the ask. Confined to the view
// rotation; g_base itself is left alone, so the weapon's controller-to-world
// mapping and the shot chain are not on this path at all.
std::atomic<int> g_cam_level{1};

// BUILD 100: the COMPOSED camera the frame was rendered with, latched once per
// frame, and the frame number it belongs to.
//
// Build 98 latched g_base / g_base_pos instead, and that was the wrong end of
// the write. Those hold the ENGINE's chase camera, captured BEFORE our write,
// which in first person sits about 1.7 m up and behind the head. Feeding that
// to the weapon put the gun 1.7 m high, and when the latch went stale the
// fallback returned the composed viewpoint instead, so the two values the gun
// alternated between were still 1.9 m apart. `[VERIFIED, headset, 2026-08-15]`
// tester on build 99: "THE GUN IS TOO FAR UP AND JUST PHASES IN AND OUT OF
// REALITY... THE JUTTER IS MUCH WORSE." All three symptoms are that inversion.
//
// These hold `out` and `pos` from the bottom of write_pose_head: the rotation
// the frame is composed with (the engine's own when cam_pose_rot is off, the
// composed one when it is on) and the viewpoint before the per-eye offset. That
// is the camera the image is actually rendered from, which is what every
// consumer of base_frame wants, and it is single valued for the whole frame.
float g_vr_cam_rot[9] = {};
float g_vr_cam_pos[3] = {};
// BUILD 282: the Head bone world the latched camera was composed from, and whether it was (on foot, head anchored).
// Written with g_vr_cam_pos before the stamp's release store, so a reader that re-checks the stamp sees a matching pair.
float g_vr_cam_head[3] = {};
bool  g_vr_cam_head_ok = false;
static thread_local float t_head_used[3] = {};
static thread_local bool  t_head_used_ok = false;
std::atomic<uint64_t> g_vr_cam_stamp{0};
std::atomic<int> g_gpair{0};          // BUILD 277: cfg gpair (defined here so the camera compose can test it)
// BUILD 279 (log only): the Head bone the camera compose read this call, held until the latch store.
struct HeadRead { bool valid; float h[3]; float orig[3]; uint64_t pose, buf; };
static thread_local HeadRead t_h3 = {};

// BUILD 98 telemetry, so the next headset run grades itself. latch = reads
// served from the per-frame engine capture, live = reads that fell through to
// the camera object because the capture was stale. gap is the distance between
// the two answers at the moment of a latch hit, which IS the jitter amplitude:
// it read 1.91 m in grwxr-13140.log with the player standing still.
std::atomic<uint64_t> g_bf_latch{0};
std::atomic<uint64_t> g_bf_live{0};
float g_bf_gap_last = 0.0f, g_bf_gap_max = 0.0f;

// BUILD 10b.2 diagnostics: is the engine refreshing the POSITION row of
// Camera+0x000 every frame, the way build 7 verified for rotation? If it is
// NOT, each frame's base capture reads back our previous eye-offset write and
// the offsets compound. Discriminator: the distance between this frame's
// captured base position and the position we WROTE last frame.
//   vs-write ~ 0.0000  -> slot still holds our write: NOT refreshed, bug.
//   vs-write ~ 0.03    -> engine rebuilt it: refreshed, offsets are clean.
// Plain floats, engine thread writes, drain reads racily (diagnostic only).
float g_diag_written[3]   = {};
// Build 114: capture pollution guard. rescues = lifetime count,
// rescue_run = consecutive rescues, capped so it can never stick.
unsigned long long g_cap_rescues = 0;
int g_cap_rescue_run = 0;
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
// Build 89: of those writes, how many came from the selector standing in for
// an underived on_calc_mvp. Zero on a fully derived build.
std::atomic<uint64_t> g_head_sel_writes{0};

// BUILD 10b.1: the eye identity of every built frame travels WITH the frame.
// Build 10b derived it from frame parity at present, assuming the frame built
// between two Presents is the next one presented; the engine pipelines builds
// ahead of Presents by an unknown depth, so that assumption swaps the eyes
// whenever the phase is odd (user report: stereo "totally unusable"). Now the
// camera hook alternates its own eye toggle per built frame, offsets the
// camera toward that eye, and pushes the tag into the HeadPose FIFO; VRMirror
// pops one tag per present.
//
// Build 93, CORRECTED: this comment used to say "eye 0 is the left eye, offset
// -IPD/2 along the camera's right axis; eye 1 is +IPD/2", which is the OPPOSITE
// of what the code has done since build 10m. The code is
// `s = g_eye_toggle ? -half : +half`, so eye 0 gets +half. Eye 0 IS the left
// eye (OpenXR PRIMARY_STEREO view 0, and VRMirror submits index 0 as left), and
// row 0 IS the camera's right axis, so positive ipd_scale displaces the LEFT
// eye to the RIGHT, which is geometrically the swapped direction.
//
// That is deliberate, not a typo: build 10m folded the sign in after the user's
// depth verdict landed at -0.50. But it can only be correct if the tag-to-
// present pairing carries an odd frame shift that cancels it, and 10m measured
// the COMPOSITE of sign and pairing parity without being able to separate them.
// Build 89 then moved the toggle and the push to a different point relative to
// the frame counter bump, which is exactly what can change that parity, and
// build 93 fixed a doff bug that could have inverted the eyes mid-calibration.
// So the stored sign is [UNKNOWN] rather than wrong, and it needs re-measuring
// with the cfg A/B. Nothing is flipped here. NO LOG LINE CAN SETTLE IT: a
// pairing shift moves no counter we print.
// Build 15e: is the anchored path actually live? Reported at 1 Hz.
std::atomic<bool> g_fp_anchored{false};
// BUILD 260 (log only): the distance from the character origin to the engine camera at the last anchor check,
// BEFORE the 12 m reach test, so a vehicle chase camera beyond the reach still shows its real distance. -1 = never read.
std::atomic<float> g_veh_dist{-1.0f};

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

// Separate functions so the SEH blocks contain no C++ object unwinding.
bool write_u16(uint64_t dst, uint16_t v) {
    __try {
        *(volatile uint16_t*)dst = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_block(uint64_t dst, const void* src, size_t n) {
    __try {
        memcpy((void*)dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// 2026-08-31: HIDE ONE BODY PIECE, ported from the FP tree (FpCamera.cpp, the
// helmet hider) for the hands-hold-the-gun work: the arms are allowed to be
// invisible, and this is the lever the FP tuner proved by accident.
//
// The engine's own SetHidden walks array 1 only (self+0x40 / count +0x4A),
// the head group. Array 2 (self+0x4C / count +0x56) is the WHOLE character
// and must never be written wholesale: doing so hides the body and thrashes
// the frame time. An arm is one array-2 entry; this sets that one entry's bit.
// Bit 0x0004 at child+0x54 is HIDDEN (FP docs/RE-notes.md:1878; measured in
// game 2026-08-17: array 1 held 5 children flags 0x00E5, array 2 held 14, the
// nine non-head entries flags 0x00E1).
//
// THE MEASUREMENT THIS EXISTS TO MAKE, before anything is built on it: is the
// hand a SEPARATE array-2 child from the arm? The FP accident proved the arms
// can vanish; it did not prove the hands survive it. Two slots (cfg
// hide_child, hide_child2) because there are two arms.
// ---------------------------------------------------------------------------
std::atomic<int32_t> g_hide_child{-1};
std::atomic<int32_t> g_hide_child2{-1};

// Sets the hidden bit on array2[idx] of the latched head object. Reads array 1
// first as a positive control: those offsets are the ones the engine's own
// setter uses, so if they do not read back sane this is not that struct and
// nothing is written. slot is 0 or 1 and exists only so each cfg key logs its
// own index changes.
void hide_one_child(uint64_t self, int slot, int32_t idx, bool on) {
    if (!self || idx < 0) return;
    uint64_t a1 = 0, a2 = 0;
    uint16_t n1 = 0, n2 = 0;
    if (!read_block(self + 0x40, &a1, sizeof(a1)) ||
        !read_block(self + 0x4A, &n1, sizeof(n1)) ||
        !read_block(self + 0x4C, &a2, sizeof(a2)) ||
        !read_block(self + 0x56, &n2, sizeof(n2)))
        return;
    if (!a1 || n1 == 0 || n1 > 64) return;          // control failed
    if (!a2 || n2 == 0 || n2 > 64 || idx >= n2) return;
    uint64_t c = 0;
    if (!read_block(a2 + (uint64_t)idx * 8, &c, sizeof(c)) || !c) return;
    uint16_t f = 0;
    if (!read_block(c + 0x54, &f, sizeof(f))) return;
    const uint16_t nf = (uint16_t)((f & (uint16_t)0xFFFB) | (on ? 0x0004 : 0));
    if (nf != f) write_u16(c + 0x54, nf);

    // DISCOVERY AID, straight from the FP port. Which index is which piece is
    // not known and has to be found by stepping, so this reports what each
    // step actually did, once per index change per slot: without it a user
    // steps values watching for something to vanish and cannot tell "wrong
    // index" from "index out of range" from "entry already hidden". Log only,
    // on the 1 Hz tick thread, and only when the index changes.
    static int32_t s_logged[2] = {-2, -2};
    if (slot >= 0 && slot < 2 && idx != s_logged[slot]) {
        s_logged[slot] = idx;
        LOG_INFO("hide: hide_child%s=%d -> child %d of %u, flags 0x%04X%s "
                 "(head group is the first %u; the arms are among the other "
                 "%u). Step until the piece you want disappears, and SAY "
                 "whether the hand went with the arm: that answer decides the "
                 "whole approach.",
                 slot ? "2" : "", idx, idx, (unsigned)n2, f,
                 nf != f ? (on ? " -> HIDDEN" : " -> shown")
                         : " (already that way)",
                 (unsigned)n1, (unsigned)(n2 > n1 ? n2 - n1 : 0));
    }
}

// ---------------------------------------------------------------------------
// BUILD 224: THE HEAD HIDES AT SPAWN, WITHOUT ADS (4.2, CX-035).
//
// WHY THE HEAD NEEDED ADS. The head hide is a detour on the engine's own
// visibility setter and forces every call the engine makes to hidden. The
// engine makes that call only from its proximity test, which runs from the
// ADS camera family (hide-body-part enabled at 0.42 m) and sometimes at the
// F2 transition; in the 2026-09-09 17:30 run first person was on for 27 s
// with setter calls=0 and obj=0, so the head stayed. With pip_block_ads = 1
// the trigger never reaches the game, so nothing ever calls the setter and
// nothing ever hands us the object. We were waiting to be told.
//
// THE ROUTE THAT DOES NOT WAIT: the flat mod's pre-latch (FpCamera.cpp,
// 2026-08-12), a detour on the predicate the per-frame visibility-apply
// function calls with the render object in rcx on EVERY pass. It offers a
// candidate at spawn. The flat mod never promoted one because it could not
// tell the local player's head assembly from a teammate's of the same class
// ([cand+0x80], the 2023 build's position field, reads structural data now).
//
// THE TIE, read off the installed binary's proximity test (head class table
// slot 0x0D, impl 0x14E18A50 on the 2026-08-19 image):
//
//     mov   rax, [rsi+0x38]            ; rsi = the head object
//     movsxd rbp, dword [rax+0xC]      ; sign bit = handle valid
//     shl rbp,32 ; sar rbp,63 ; and rbp,[rax]   ; rbp = the ENTITY, or 0
//     ... call GetAttachment(rbp, &out, 0x07C159A2 "Head")
//
// So [obj+0x38] -> handle block; [block+0] is the owning entity when the i32
// at [block+0xC] is negative. The pinned player entity (g_player_entity, the
// owner test [skel+0x10] == pent) is the same object. A candidate whose
// entity is the pin is the player's head assembly; any other is refused and
// named in the log. This is the entity walk 4.2 has asked for since
// 2026-09-05, run backwards: from the object to the entity, one compare.
//
// THE RE-ASSERT. The same proximity test has a show-all tail (the global
// predicate at its entry) that clears bit 0x0004 on every child of both
// arrays and resets the +0x68/+0x69 state bytes without going through the
// setter: the 2026-09-05 log held setter calls=643 constant while the helmet
// was visibly back after a roll. So once the object is known, hold the state:
// read the array-1 children and write back any cleared bit, exactly the
// setter's own loop (RE-notes: SetHidden walks array 1 only, bit 0x0004 at
// child+0x54, state byte +0x68). Array 2 is the whole character and is never
// touched here (HEAD-HIDE-METHOD.md section 2).
// ---------------------------------------------------------------------------
std::atomic<int> g_head_prelatch{1};   // cfg head_prelatch (default on)
std::atomic<int> g_head_reassert{1};   // cfg head_reassert (default on)
std::atomic<uint64_t> g_hl_promoted{0};     // empty slot filled from the pre-latch
std::atomic<uint64_t> g_hl_replaced{0};     // a different player object replaced the old
std::atomic<uint64_t> g_hl_tie_fail{0};     // candidate's entity is not the pin
std::atomic<uint64_t> g_hl_no_pin{0};       // candidate seen with no player pin yet
std::atomic<uint64_t> g_hl_ctl_fail{0};     // candidate's arrays did not read sane
std::atomic<uint64_t> g_hr_checks{0};       // re-assert passes
std::atomic<uint64_t> g_hr_rewrites{0};     // child bits found clear and written
std::atomic<uint64_t> g_hr_state_resets{0}; // +0x68 found 0 and written
std::atomic<uint64_t> g_hr_stale_drops{0};  // pointer dropped: control failed 3x
std::atomic<uint64_t> g_hr_tie_drops{0};    // pointer dropped: entity tie failed 3x
std::atomic<uint64_t> g_hr_last_drop{0};    // the pointer last dropped, for the log

// BUILD 225: the head hidden at all times, no F2, no trigger.
std::atomic<int>      g_head_always{1};        // cfg head_always: force regardless of fp
std::atomic<int>      g_fp_auto{1};            // cfg fp_auto: first person on by itself
std::atomic<uint64_t> g_fp_auto_fires{0};
std::atomic<int>      g_fp_user_off{0};        // review fix: F2 off holds until the pin changes
std::atomic<uint32_t> g_head_pulse_frames{30}; // cfg head_pulse: frames per phase, 0 = off (226: 30)
std::atomic<uint64_t> g_hl_pos_promos{0};      // 226: promoted on the position tie
std::atomic<int>      g_head_populate{1};      // cfg head_populate: call the engine's fill routine
std::atomic<uint64_t> g_populate_fn{0};        // VA of Populate impl, 0 = not derived
std::atomic<uint64_t> g_populate_calls{0};
std::atomic<uint64_t> g_populate_ok{0};        // arrays read populated afterwards
std::atomic<uint64_t> g_populate_empty{0};     // called, arrays still empty
std::atomic<uint64_t> g_populate_fault{0};     // the call raised
std::atomic<uint64_t> g_populate_nolink{0};    // [obj+0x20] or [[obj+0x20]+0x10] null: not Linked yet
std::atomic<uint64_t> g_hl_pos_refused{0};     // 226: refused on the position tie (far)
std::atomic<uint64_t> g_pulse_t0{0};           // frame the first press is due
std::atomic<uint32_t> g_pulse_n{0};            // frames per phase while armed, 0 = idle
std::atomic<uint32_t> g_pulse_attempts{0};     // arms since the pin last changed
std::atomic<uint64_t> g_pulse_arms{0};
std::atomic<uint64_t> g_pulse_frames{0};       // input polls that carried the press
std::atomic<uint64_t> g_pulse_done{0};         // pulses that ran to their end
std::atomic<uint64_t> g_pulse_skipped{0};      // dropped: the block was not on (no weapon)
std::atomic<uint64_t> g_pulse_expired{0};      // dropped: the frame counter ran past
std::atomic<uint64_t> g_pulse_wait_weapon{0};  // ticks the arm waited for a drawn weapon
std::atomic<uint64_t> g_hl_tie_unread{0};      // candidate's handle empty or unreadable
std::atomic<uint64_t> g_hl_arr_fail{0};        // candidate's arrays not populated
std::atomic<uint64_t> g_hl_stable_promos{0};   // promoted on stability, tie unreadable

bool write_u8(uint64_t dst, uint8_t v) {
    __try {
        *(volatile uint8_t*)dst = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// BUILD 226: call the engine's own Populate(obj, obj). SEH guarded; no C++
// objects in this frame. Returns false if the call raised.
bool call_populate(uint64_t fn, uint64_t obj) {
    __try {
        ((void (*)(void*, void*))fn)((void*)obj, (void*)obj);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The entity tie. Returns false when a read faults (not this struct, or
// freed); true with *ent = 0 when the handle block says "no entity".
bool head_obj_entity(uint64_t obj, uint64_t* ent) {
    *ent = 0;
    uint64_t blk = 0;
    if (!obj || !read_block(obj + 0x38, &blk, sizeof(blk))) return false;
    if (!blk || blk < 0x10000) return false;
    int32_t gen = 0;
    if (!read_block(blk + 0x0C, &gen, sizeof(gen))) return false;
    if (gen >= 0) return true;                       // handle not valid
    uint64_t e = 0;
    if (!read_block(blk, &e, sizeof(e))) return false;
    *ent = e;
    return true;
}

// The positive control the engine's own setter relies on: array 1 and its
// count must read sane. 1 = sane, 0 = readable but not this struct, -1 = a
// read faulted.
int head_obj_control(uint64_t obj, uint64_t* a1, uint16_t* n1) {
    *a1 = 0; *n1 = 0;
    if (!obj) return 0;
    if (!read_block(obj + 0x40, a1, sizeof(*a1)) ||
        !read_block(obj + 0x4A, n1, sizeof(*n1)))
        return -1;
    if (!*a1 || *a1 < 0x10000 || *n1 == 0 || *n1 > 64) return 0;
    return 1;
}

// Hold the hidden state on the latched object: the setter's own loop, run
// only over children whose bit is NOT already set. Returns the number of
// children rewritten, or -1 when the control failed. Every read and write is
// SEH guarded; a fault costs a counter, never a frame.
int head_reassert(uint64_t obj) {
    uint64_t a1 = 0;
    uint16_t n1 = 0;
    const int ctl = head_obj_control(obj, &a1, &n1);
    if (ctl < 0) return -1;    // a read faulted: not this struct, or freed
    if (ctl == 0) return -2;   // readable but the arrays are not populated yet
    int wrote = 0;
    for (uint16_t i = 0; i < n1; ++i) {
        uint64_t c = 0;
        if (!read_block(a1 + (uint64_t)i * 8, &c, sizeof(c))) return -1;
        if (!c) continue;
        uint16_t f = 0;
        if (!read_block(c + 0x54, &f, sizeof(f))) return -1;
        if (f & 0x0004) continue;
        if (write_u16(c + 0x54, (uint16_t)(f | 0x0004))) ++wrote;
    }
    uint8_t st = 0;
    if (read_block(obj + 0x68, &st, sizeof(st)) && st == 0) {
        if (write_u8(obj + 0x68, 1))
            g_hr_state_resets.fetch_add(1, std::memory_order_relaxed);
    }
    return wrote;
}

// BUILD 79: THE BARREL AXIS, MEASURED RATHER THAN SCORED.
//
// Rotating the gun-root bone onto the controller ray needs to know which axis
// of that bone's own frame points down the barrel. Build 74 answered this by
// scoring six signed basis rows against the CAMERA forward and returned a
// confident wrong answer, because on a canted weapon the gun's right axis
// correlates with gaze better than its barrel does.
//
// This measures instead. While the player aims, the gun points where the GAME
// is aiming, and aimtrace publishes that direction in engine world space. So
// the bone axis whose world direction tracks `view_fwd` IS the barrel, and the
// numbers go in the log for a human to read rather than into a heuristic that
// picks in the dark.
//
// Convention-free by construction: instead of packing a rotation matrix and
// having to be right about row-vector versus column-vector order, it rotates
// the three unit model axes by the bone's world quaternion. A dot product of a
// rotated unit axis with a unit direction means the same thing under either
// convention.
//
// Returns the three world-space axis directions in ax/ay/az. Read-only.
bool read_bone_world_axes(uint64_t skel, unsigned int idx,
                          float ax[3], float ay[3], float az[3]) {
    if (!skel || idx == 0xFFFFu) return false;
    uint64_t pose = 0, buf = 0;
    if (!read_block(skel + 0x238, &pose, sizeof(pose))) return false;
    if (pose < 0x10000 || (pose & 7)) return false;
    if (!read_block(pose + 0x178, &buf, sizeof(buf))) return false;
    if (buf < 0x10000 || (buf & 7)) return false;

    float rec[8];   // { float4 T, float4 Q }
    if (!read_block(buf + (uint64_t)idx * 0x20, rec, sizeof(rec))) return false;
    const float* bq = rec + 4;
    for (int i = 0; i < 4; ++i) if (!isfinite(bq[i])) return false;

    float q[4] = {bq[0], bq[1], bq[2], bq[3]};   // xyzw

    // Model space unless bit 26 says the buffer is already world space, in
    // which case the bone quaternion IS the world one and the root must not be
    // applied a second time. Same gate read_bone_world uses.
    uint32_t flags = 0;
    read_block(pose + 0x8C, &flags, sizeof(flags));
    if (!(flags & 0x04000000u)) {
        float rq[4];
        if (!read_block(pose + 0x10, rq, sizeof(rq))) return false;
        for (int i = 0; i < 4; ++i) if (!isfinite(rq[i])) return false;
        const float rn = sqrtf(rq[0] * rq[0] + rq[1] * rq[1] +
                               rq[2] * rq[2] + rq[3] * rq[3]);
        if (!(rn > 1e-6f)) return false;
        const float ax_ = rq[0] / rn, ay_ = rq[1] / rn,
                    az_ = rq[2] / rn, aw_ = rq[3] / rn;
        const float bx = q[0], by = q[1], bz = q[2], bw = q[3];
        // Hamilton product worldQ = rootQ * boneQ (bone applied first).
        q[3] = aw_ * bw - ax_ * bx - ay_ * by - az_ * bz;
        q[0] = aw_ * bx + ax_ * bw + ay_ * bz - az_ * by;
        q[1] = aw_ * by - ax_ * bz + ay_ * bw + az_ * bx;
        q[2] = aw_ * bz + ax_ * by - ay_ * bx + az_ * bw;
    }

    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-6f)) return false;
    const float qx = q[0] / n, qy = q[1] / n, qz = q[2] / n, qw = q[3] / n;

    // v' = v + 2*qw*(q x v) + 2*(q x (q x v)), the same form read_bone_world
    // uses, applied to each unit model axis in turn.
    auto rot = [&](const float v[3], float out[3]) {
        const float cx = qy * v[2] - qz * v[1];
        const float cy = qz * v[0] - qx * v[2];
        const float cz = qx * v[1] - qy * v[0];
        const float dx = qy * cz - qz * cy;
        const float dy = qz * cx - qx * cz;
        const float dz = qx * cy - qy * cx;
        out[0] = v[0] + 2.0f * (qw * cx + dx);
        out[1] = v[1] + 2.0f * (qw * cy + dy);
        out[2] = v[2] + 2.0f * (qw * cz + dz);
    };
    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    rot(ex, ax); rot(ey, ay); rot(ez, az);
    return isfinite(ax[0]) && isfinite(ay[0]) && isfinite(az[0]);
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
// Build 46: the slot RVAs live in GameBuild.cpp per binary. The expected
// bytes, including the dispatch offsets 0x570/0x5D0, are IDENTICAL in both
// analysed builds (the store scan matched the full sequence exactly once).
constexpr uint8_t   kSetYawExpect[10] = {0x48, 0x8B, 0x01,              // mov rax,[rcx]
                                         0x48, 0xFF, 0xA0,              // jmp qword ptr [rax+
                                         0x70, 0x05, 0x00, 0x00};       //   0x570]

// Build 19: the pitch setter, same slot shape, dispatch offset 0x5D0
// (RE-notes "THE ABSOLUTE AIM ANGLE EXISTS", printer-proved accessor set).
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
// Build 46: table/thunk/impl RVAs live in GameBuild.cpp per binary.
constexpr const char* kHeadSlotFnSig  =
    "48 89 5C 24 08 57 48 83 EC 20 48 83 7A 20 00 48 89 D3 48 89 CF 74 ? "
    "49 89 D0 31 D2 E8";
// BUILD 224, PRE-LATCH: the per-frame visibility-apply function. The engine
// calls SetHidden only on transitions, so before the first one there is no
// pointer to latch. The 2023 community mod closed the same gap by hooking
// this function's per-frame path; the flat mod re-derived it on the 2026-08
// binaries (E:\GRW-FP\docs\RE-notes.md 2026-08-12 and 2026-08-17 late). The
// signature is the function's core (cmp byte [rbx+30],0 / je / test sil,sil
// / jne / mov rcx,rbx / call) and matches ONCE in the image at the build's
// apply_rva. 0x15 bytes before it sits `call <predicate>`: a thunk the
// function calls with the render object in rcx on EVERY pass, before any
// transition check. That thunk is what gets hooked; its impl must byte-match
// kHeadVisPredImplBytes (movzx eax, byte ptr [rcx+0x10]; ret), which both
// proves the target and makes the tail-jump trivially safe.
//
// The three RVAs are BUILD-KEYED and the pins SELECT, they never TRUST: every
// value is verified against the live image before a byte is written. A build
// with no row reports "not derived" and the head still hides at the first
// setter call, exactly as 223 did.
constexpr const char* kHeadVisApplySig =
    "80 7B 30 00 74 24 40 84 F6 75 41 48 89 D9 E8";

struct HeadVisPins {
    uint32_t  pe_timestamp;   // IMAGE_FILE_HEADER.TimeDateStamp, the build key
    uintptr_t apply_rva;      // where kHeadVisApplySig matches
    uintptr_t pred_thunk;     // E9 slot, int3 padded
    uintptr_t pred_impl;      // the movzx/ret body
};

constexpr HeadVisPins kHeadVisPinTable[] = {
    // 2026-08-13 Last Rites: derived by two independent scanners, confirmed
    // in the running game by the flat mod (grwfp-27492.log, hits climbing).
    {0x6A75F2F4, 0x14EC434F, 0x01150B20, 0x0D161AA0},
    // 2026-08-19 update, the installed image. Re-verified 2026-09-09 on
    // C:\Steam\steamapps\common\Wildlands\GRW.exe itself: the signature
    // hits once in the whole file at this RVA, the E8 at -0x15 targets this
    // thunk, the thunk is E9 + int3 to this impl, the impl is 0F B6 41 10 C3.
    {0x6A7C5143, 0x14EC434F, 0x01150B20, 0x0D161AA0},
    // 2026-07-28 update, the original derivation, never re-verified since.
    {0x6A692948, 0x146C755F, 0x00454FA0, 0x06FD0FB0},
};

constexpr uint8_t kHeadVisPredImplBytes[5] = {0x0F, 0xB6, 0x41, 0x10, 0xC3};

// BUILD 226: the Populate call inside the proximity test:
//   mov rdx,rdi ; mov rcx,rsi ; call <thunk> ; movaps xmm1,xmm6 ; mov rcx,rsi ; call
constexpr const char* kHeadPopulateCallSig =
    "48 89 FA 48 89 F1 E8 ? ? ? ? 0F 28 CE 48 89 F1 E8";
// Populate's prologue: push rbx ; sub rsp,80h ; mov [rsp+70h],rdi ; xor edi,edi ;
// mov [rcx+4Ah],di ; mov [rcx+56h],di ; mov rax,[rdx+20h]
constexpr uint8_t kHeadPopulatePrologue[] = {
    0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0x89, 0x7C, 0x24,
    0x70, 0x31, 0xFF, 0x66, 0x89, 0x79, 0x4A, 0x66, 0x89, 0x79, 0x56, 0x48, 0x8B,
    0x42, 0x20};

const HeadVisPins* head_vis_pins(uint32_t pe_timestamp) {
    for (const HeadVisPins& p : kHeadVisPinTable)
        if (p.pe_timestamp == pe_timestamp) return &p;
    return nullptr;
}

// The setter's own body signature is PER BUILD (GameBuild head_setter_sig):
// the 2026-08 recompile re-registered it, so one constant cannot serve all
// three binaries. Unique, no rel32/rip operands (RE-notes + campaign doc).

hook::ThunkHook g_headhide_hook;
hook::ThunkHook g_headlatch_hook;   // BUILD 224: the pre-latch predicate
hook::ThunkHook g_wgun_hook;
hook::ThunkHook g_wnode_hook;


// Build 67 state, all owned by the 1 Hz census tick.
std::atomic<bool>  g_wgun_on{false};
std::atomic<float> g_wgun_dz_cfg{0.30f};
constexpr uint32_t kFakeGunRootHash = 0x826846F3u;   // Fake_gunroot, VERIFIED
constexpr uint32_t kGunRootGameplay = 0x08B4DDD5u;   // FakeGunRoot_Gameplay
std::atomic<uint32_t> g_wgun_hash{kFakeGunRootHash};

// Build 79: the barrel-axis measurement, log only (cfg wbaxis).
std::atomic<int> g_wbaxis{0};

// Build 80: the rotation's filter, cfg wgun_smooth / wgun_maxstep_deg, and a
// generation counter so a mode change restarts the filter from the live ray
// instead of slewing out of a stale one.
std::atomic<float> g_wgun_smooth{0.25f};
std::atomic<float> g_wgun_maxstep{5.0f};
std::atomic<uint32_t> g_wgun_gen{0};

// 2026-08-13: two-handed aim and roll. Both default ON: a weapon you hold with
// two hands and can twist is the point of the feature, and both degrade to the
// previous one-handed behaviour on their own when the inputs are not there.
std::atomic<int>   g_wgun_twohand{1};
std::atomic<int>   g_wgun_roll{1};
std::atomic<float> g_wgun_roll_deg{0.0f};   // the unknown gun-up constant
// BUILD 189: cancel the gun-up half-turn inversion the fall animation leaves
// behind, using croll as the witness. 1 = on. 0 = build 188 behaviour.
// REFUTED in its one run (a false latch held the gun a half turn over for
// 25 s), so the default is now off; the key only matters with wgun_own = 0.
std::atomic<int>   g_wgun_roll_unflip{0};
// BUILD 190: OWN THE BONE. 1 = the gun-root orientation is built from the aim
// and the controller's up alone, never from what the animation left in the
// record. 0 = the swing-and-measure path of 186 to 189, which is the fault the
// owner reported (the roll dies after a fall, a roll or a vehicle exit).
// Hot-reloadable, so it is the rollback.
std::atomic<int>   g_wgun_own{1};

// 2026-08-31: THE HANDS HOLD THE GUN (cfg wgun_hands). Every call that moves
// the gun-root record also measured the rigid change it made, and that same
// change is applied to the character's hand and finger records, so the visible
// hands keep exactly the grip the animation authored, wherever the controllers
// put the weapon. No IK, no placement constants: the animated hand-to-gunroot
// relationship IS the grip, and preserving it is the whole feature.
std::atomic<int> g_wgun_hands{1};

// 2026-08-31: build 123's standing single change, made at last (cfg
// wgun_two_anchor). When the front hand measures as the RIGHT one, the
// position anchor crossfades to the rear (left) controller on the same
// authority curve the direction uses, instead of anchoring to the front hand
// and standing the handguard slide down.
std::atomic<int> g_wgun_two_anchor{1};

// The hand chains, by CRC32 of the verified bone names (the bone-name table,
// exact-match against GR_PCF_Skeleton_Average; the hash function is proven by
// the 11 runtime-verified names, all plain CRC32). Hand first, then the five
// InHand carpals, then the fifteen phalanges. A name the live rig does not
// carry simply fails rig_find_node and is skipped, so this list can only
// under-move a hand, never write a wrong bone.
constexpr uint32_t kHandChain[2][21] = {
  { 0xB675F36Cu, 0x98E1EE97u, 0x0DA5584Du,  // LeftHand LeftInHandThumb LeftInHandIndex
    0xE167C14Bu, 0x51EE4C69u, 0x19910DC9u,  // LeftInHandMiddle LeftInHandRing LeftInHandPinky
    0x78F4E1ACu, 0xE1FDB016u, 0x96FA8080u,  // LeftHandThumb1 LeftHandThumb2 LeftHandThumb3
    0x1E679ED0u, 0x876ECF6Au, 0xF069FFFCu,  // LeftHandIndex1 LeftHandIndex2 LeftHandIndex3
    0x911FBACFu, 0x0816EB75u, 0x7F11DBE3u,  // LeftHandMiddle1 LeftHandMiddle2 LeftHandMiddle3
    0xD34048CFu, 0x4A491975u, 0x3D4E29E3u,  // LeftHandRing1 LeftHandRing2 LeftHandRing3
    0xF4A6EDBCu, 0x6DAFBC06u, 0x1AA88C90u },// LeftHandPinky1 LeftHandPinky2 LeftHandPinky3
  { 0x75F94D30u, 0x4CBA032Au, 0xD9FEB5F0u,  // RightHand RightInHandThumb RightInHandIndex
    0x54635597u, 0xB133130Au, 0xCDCAE074u,  // RightInHandMiddle RightInHandRing RightInHandPinky
    0x9829BECFu, 0x0120EF75u, 0x7627DFE3u,  // RightHandThumb1 RightHandThumb2 RightHandThumb3
    0xFEBAC1B3u, 0x67B39009u, 0x10B4A09Fu,  // RightHandIndex1 RightHandIndex2 RightHandIndex3
    0x45445772u, 0xDC4D06C8u, 0xAB4A365Eu,  // RightHandMiddle1 RightHandMiddle2 RightHandMiddle3
    0xDBF635C5u, 0x42FF647Fu, 0x35F854E9u,  // RightHandRing1 RightHandRing2 RightHandRing3
    0x147BB2DFu, 0x8D72E365u, 0xFA75D3F3u } // RightHandPinky1 RightHandPinky2 RightHandPinky3
};

// The resolved node indices, re-resolved from the live rig's own name map on
// every census tick and never cached across ticks, exactly the discipline the
// gun node itself follows: a rig swap self-corrects within one second and a
// rig that lacks a bone drops it rather than writing a wrong one. The count is
// the publish gate: it is stored 0 before the entries are rewritten and stored
// last with release order, so the engine-thread reader can never walk a
// half-written table.
uint16_t              g_hand_node[2][21] = {};
std::atomic<uint32_t> g_hand_n[2] = {0, 0};

// ---------------------------------------------------------------------------
// BUILD 155: THE ANIMATION-SURVIVAL PROBE. Log only, writes nothing, no new
// hook.
//
// HANDOFF section 12 item 4 has stood as UNVERIFIED, HIGH VALUE since it was
// written: "animations can completely take over the arms and hands when not
// braced". The owner confirmed it again from the headset on 2026-09-02 in
// different words, that the guns "feel floaty and unpredictable due to
// animations". It is the suspected parent of items 2, 3 and 5, so it is worth
// settling BEFORE any hand fix is designed, and settling it is free.
//
// THE TEST. hands_apply already reads every bone record as FOUND at the top of
// its own loop, once per call. So remember the hand bone exactly as we WROTE
// it, and on the next call compare that against what is now THERE.
//
//   found == what we wrote  ->  our write survived the frame
//   found != what we wrote  ->  something else authored the bone in between,
//                               and the animation is the only candidate
//
// The two outcomes need OPPOSITE fixes, which is why guessing has been
// expensive. If our writes survive, the hand complaints are ordinary bugs on
// our side (a missing publish, a wrong target). If they do not survive, then
// no amount of writing these bones can ever hold, and the answer is the
// engine's own pattern: FakeGunRoot_Gameplay is a ZERO-OFFSET DUPLICATE bone
// that exists precisely so something can write it without disturbing the
// animated original. The weapon got that treatment; the hands never did.
//
// A 1 mm threshold rather than exact equality: the engine re-encodes these
// records through its own transform pipeline, so bit equality would report
// false clobbers, while 1 mm is far below any animation's per-frame motion and
// far above float noise at this magnitude.
//
// Reported PER SIDE, which matters: the right chain always rides at full
// authority and the left blends, so one number for both would average two
// different questions.
constexpr float kHbKeptM = 0.001f;
float                 g_hb_wrote[2][3] = {};
uint32_t              g_hb_wrote_ok[2] = {0, 0};
std::atomic<uint32_t> g_hb_checks[2]   = {0, 0};
std::atomic<uint32_t> g_hb_kept[2]     = {0, 0};
std::atomic<uint32_t> g_hb_drift_mm[2] = {0, 0};   // worst seen, millimetres
std::atomic<uint32_t> g_hb_last_mm[2]  = {0, 0};   // most recent, millimetres

// BUILD 156: THE PROBE WAS COMPARING TWO RIGS AGAINST ONE MEMORY. The 155
// runs read kept at almost exactly 50 percent on BOTH sides all run long
// (61/121 after one second, 78771/157824 after twelve minutes), with worst
// drift in metres. 50 percent is not an animation verdict, it is an
// alternation signature: hands_apply has two call sites with two different
// buf targets, and g_hb_wrote is one slot per side, so every other check
// compared a bone in one rig against a value written into the other. The
// metre-scale "drift" is the distance between the rigs, not authorship.
// Fix: remember WHICH buffer the write went into and only compare like with
// like. Cross-buffer encounters are counted rather than discarded silently,
// so the interleave that faked the 50 percent is itself measured: xbuf near
// checks confirms the 155 reading was this artifact.
uint64_t              g_hb_buf[2]      = {0, 0};
std::atomic<uint32_t> g_hb_xbuf[2]     = {0, 0};

// BUILD 140: THE GRAB (cfg wgun_hands_grab). The headset confirmed the hands
// ride the gun, and the owner's next direction was that the idle animation
// must not FORCE both hands onto it. The animation is not touched, and must
// not be: the transported grip is sampled from it every frame. Instead the
// character's LEFT hand chain (the animation's foregrip hand; the character
// is right-handed regardless of the player's main_hand) inherits the gun's
// motion scaled by the SAME two-hand authority weight that already runs the
// direction, the anchor and the slide. Real off hand down: weight 0, the left
// chain stays the animation's. Real off hand on the foregrip: weight 1, full
// ride. Between: a smooth magnetic grab, and the attach band's release edge
// is automatically the let-go.
std::atomic<int> g_wgun_grab{1};

// BUILD 141: HAND PRESENCE (cfg wgun_hand_free). The owner's direction:
// complete control of the hands independent of the weapon, because reaching
// over the shoulder for the rifle and down to the thigh for the sidearm are
// coming. When the left chain is not on the weapon (grab weight below 1) the
// remainder now follows the LEFT CONTROLLER instead of the animation:
// position only, wrist and fingers keep the animated pose, because aimtrace
// publishes only ctrl_pos_l for that hand. The grab therefore blends between
// YOUR real hand and the foregrip, and a free hand visibly reaches where you
// reach. The reach is clamped to kFreeReach of the hand's current position
// per call, so a tracking pop cannot throw the hand across the map: the same
// reasoning as the gun's own position clamp, sized to an arm instead of a
// weapon.
std::atomic<int> g_wgun_free{1};
constexpr float kFreeReach = 1.20f;   // m per call, beyond any real arm

// BUILD 158: THE WRIST (cfg wgun_wrist, default 1). CX-017, the owner's
// words: "the hand looks broken and is stuck in what looks like the brace
// position". Build 141 moved the free hand's POSITION onto the controller
// and left the wrist "keeping the animated pose, because aimtrace publishes
// only ctrl_pos_l for that hand". HANDOFF section 12 item 2 names the fix:
// a missing publish, not a broken solve. The left controller's forward and
// up now arrive as verified vectors; a frame is built from them in model
// space, a REFERENCE frame is latched on entry into the free path, and the
// whole free chain is rotated by the delta between the two, about the hand
// bone. Identity on the latch frame, so engaging it can never snap; a
// dropout drops the latch and relatches clean. Absolute controller-to-bone
// mapping is deliberately NOT attempted: the bone's axis convention is
// unmeasured, and a delta needs no convention.
//
// WHY THE STATE IS KEYED PER BUFFER, and build 157's one-hour life is the
// reason. hands_apply runs about twice per frame against more than one rig
// buffer (F-142), and 157 kept ONE latched frame for all of them, so each
// call compared against the other rig's reference and the delta came out as
// a large bogus rotation at call rate. The owner saw it the moment he loaded
// in and it was reverted the same morning. Same failure family as the 155
// animsurv artifact: two rigs, one memory. Every latch below is (buf -> ref)
// and a call only ever measures against its own buffer's latch.
//
// BUILD 161: THE TABLE IS SIZED FOR THE POOL, and the owner's verdict on 158
// is the reason: "the hand is flickering between the braced position and the
// controller rotations, like it half works." 158 keyed the latch per buffer
// but gave the table TWO slots and always evicted slot 1, while the run that
// produced the verdict (grwxr-27388) shows SIX distinct rig buffers rotating
// through wpose. Against a rotating pool a two-slot table relatches at
// identity on most calls and shows the accumulated delta on the rest: a
// two-state flicker at frame rate, which is what he saw. Three changes, all
// inside this one mechanism, nothing else touched:
//   1. eight slots, evicted by age, so every buffer in the pool keeps its
//      own latch. ev= counts evictions; nonzero in steady state means the
//      pool is bigger than eight or the buffer address is the wrong key.
//   2. a buffer entering the table while others already carry a delta is
//      SEEDED with the pool's live delta instead of identity, so a rig swap
//      or a late buffer cannot bring the two-state flicker back. The first
//      entry into an EMPTY table is still identity, so engaging the free
//      path still cannot snap.
//   3. a controller-vector dropout no longer wipes the table on the spot:
//      the slot HOLDS its last delta for that call (hold=) and the table is
//      wiped only after kWrMissWipe consecutive misses (wipe=). Leaving the
//      free path (grab, ride, wrist off) still wipes at once, as 158
//      designed, so a release cannot snap by the grabbed-time delta.
// Counters ride on the wgun line as wrist lat/ev/seed/miss/hold/wipe/n.
// Decided readings: flicker gone with lat settling near the pool size and
// ev=0 confirms the mechanism; flicker persisting with the same counters
// means the slot table was not the cause and miss/hold say whether the
// dropout is; ev climbing without end means buffers are transient and the
// key must move from the buffer address to the rig.
std::atomic<int> g_wgun_wrist{1};
struct WrRef {
    uint64_t buf;       // the rig buffer this latch belongs to
    float    B[9];      // latched model-space frame, rows right/fwd/up
    float    D[9];      // last delta computed for this buffer, B^T * ref
    uint64_t seen;      // g_wr_call at last touch, for eviction by age
    int      dok;       // D holds a computed delta
};
constexpr int kWrSlots    = 8;
constexpr int kWrMissWipe = 12;      // consecutive misses before a wipe
WrRef    g_wr_ref[kWrSlots] = {};    // engine thread only, like g_seg_len
int      g_wr_n    = 0;              // live entries; 0 = nothing latched
uint64_t g_wr_call = 0;              // live free-path calls, the age clock
int      g_wr_miss = 0;              // consecutive misses on the free path
volatile uint64_t grwxr_wr_latch = 0;  // slot created into a free entry
volatile uint64_t grwxr_wr_evict = 0;  // slot created by evicting the oldest
volatile uint64_t grwxr_wr_seed  = 0;  // new slot seeded from the live delta
volatile uint64_t grwxr_wr_miss  = 0;  // free path live, no usable vectors
volatile uint64_t grwxr_wr_hold  = 0;  // a miss served by the last delta
volatile uint64_t grwxr_wr_wipe  = 0;  // table wiped after kWrMissWipe misses

// BUILD 162: THE ABSOLUTE WRIST (cfg wgun_wrist_abs, default 1). The owner
// on 161: "the rotation is ok on the hand but not great as we never set up
// how its orientation should be for both." The delta route above only ever
// says how far the controller has turned since a latch; where the palm
// FACES was whatever the animation held at that instant. This route says it
// outright, from two descriptions of the same anatomy:
//   the OpenXR GRIP pose, whose axes are anatomical by specification: +X is
//   the palm normal, -Z runs through the tube of the curled fingers from
//   little finger to thumb, +Y follows by the right-hand rule, so it runs
//   knuckles to wrist;
//   the rig's own knuckles: middle knuckle to wrist is the palm's long axis,
//   index knuckle to pinky knuckle runs across the fist, and Y x Z is the
//   palm normal. The same three axes, read from the bones themselves.
// Every call the bone's anatomical frame is rebuilt from its found records
// and rotated onto the controller's grip frame about the hand bone, the
// whole chain travelling with it. Stateless, so the rig buffer pool cannot
// touch it, and there is no latch to seed or wipe. Mirror-correct for both
// hands by construction: the specification's left/right palm-normal rule and
// the anatomy's cross product flip together. The 161 delta route stays as
// the fallback when the grip pose or the knuckles are unavailable, and the
// counters say which: abs= absolute writes, nogrip= no grip frame published,
// noanat= knuckles unresolved or unreadable. Both sides resolve their
// anatomy; only the free hand (s == 0) is written here, the right hand rides
// the gun until the stow state exists.
std::atomic<int>      g_wgun_wrist_abs{1};
uint16_t              g_hand_anat[2][4] = {};   // hand, index1, middle1, pinky1
std::atomic<uint32_t> g_hand_anat_n[2]  = {};   // 4 when all four resolved
volatile uint64_t grwxr_wr_abs    = 0;   // absolute writes
volatile uint64_t grwxr_wr_nogrip = 0;   // live, no grip frame published
volatile uint64_t grwxr_wr_noanat = 0;   // live, knuckles unresolved or unread

// BUILD 163: THE FOREARM TAKES THE ROLL (cfg wgun_twist, default 1). The
// owner on 162's screenshot: "that texture warp has got to go", "it has to
// be linked to the idle animations". It is: the animated forearm keeps
// rolling with the idle cycle while the hand, now absolute, no longer
// follows it, and the wrist joint takes the whole difference, which a real
// wrist never does. The split lives in arms_solve, on the same rigid carry
// the forearm helpers already ride. Per side the last roll angle in
// degrees, and for side 0 the four helpers' measured fractions along the
// forearm, ride the wgun line as twist L= R= t=. twists= counts solves
// that carried more than a degree of roll.
std::atomic<int>      g_wgun_twist{1};
// BUILD 167: THE FOREARM BONE TAKES A SHARE (cfg wgun_twist_share, default
// 0.40). The 166 probe (grwxr-26680) settled where the roll can live. After
// our write the four helpers read rolls of exactly 1/5, 2/5, 3/5, 4/5 of the
// hand's roll, not the position fractions 163 to 165 wrote, and their
// quaternions differed from what we wrote by 20 to 156 degrees while the
// ForeArm bone held at 0: the ENGINE has its own twist pass, it runs after
// us, and it repaints O_01..O_04 (and O_Elbow, and the look-at) from the
// hand's roll relative to the ForeArm. We never owned those bones. The fins
// of 163 were that pass hitting the same half-turn ambiguity 165 fixed on
// our side, because the absolute palm sits a half turn from the animated
// forearm. So the roll goes into the one bone we do own: the whole forearm
// segment rolls rigidly by share x phi about the new forearm axis, and the
// engine's pass, now seeing a hand inside its stable range, spreads the
// remaining (1 - share) x phi along the helpers in fifths as designed. The
// share the elbow end takes is absorbed the way a real arm absorbs a big
// pronation, at the shoulder. 0 = 162 behaviour, 1 = the segment rolls
// with the hand and the helpers carry nothing.
// BUILD 169: THREE JOINTS, NOT ONE. The 168 run (grwxr-27956, 152 probe
// samples, every wrist movement he could make) pinned the helpers down:
// relative to the ForeArm bone WE write they sat at fifths of the same 163
// degrees the whole run (o1..o4 = -130/-98/-65/-32) while our hand's roll
// swung from -41 to +167 and our forearm share moved them by nothing. So
// the engine composes the helpers as children of our ForeArm with a local
// twist baked from its own animation, the brace pose's, and nothing we
// write reaches that twist. The roll change we introduce therefore lands
// at the joints we own, and only there: the shoulder (the Arm bone about
// its own axis), the elbow (the ForeArm bone about the new forearm axis,
// relative to the Arm) and the wrist (whatever is left between the ForeArm
// and the absolute hand). Shares: the Arm rolls wgun_twist_arm x phi about
// the upper-arm axis; the ForeArm rolls wgun_twist_share x phi about the
// forearm axis, so the elbow crease is (share - arm) x phi and the wrist
// crease is (1 - share) x phi. Defaults a third each: 60 degrees per joint
// at a full half turn, under 30 for the rolls of that run. Both keys
// hot-reload for tuning by eye.
// BUILD 170: left arm only, and the shoulder share defaults to 0 until the
// elbow-and-wrist split alone has been judged.
// BUILD 171: THE HELPERS CARRY THE RAMP AGAIN, AND THE SECOND CALL KEEPS IT.
// What the 168/169/170 probe lines actually show, 400 samples: after our
// write O_01..O_04 sit at 1/5, 2/5, 3/5, 4/5 of ONE roll near a half turn,
// the 4/5 helper nearest the wrist (t=0.81), and that roll follows neither
// our hand (which swung -157 to +168) nor our ForeArm share. That is a
// wrist-driven twist ramp fed from something we never write. Two things
// were never tested in front of the headset: 163's fins proved that our
// helper writes DO reach the skin (they changed the render), and 165's
// well-conditioned ramp died in a 3 s run before anyone saw it, then 167
// removed it on the strength of a keep= reading that only says the pass
// runs between our two calls, not which call the render reads. So: the
// first call writes 165's ramp on top of 167's share (each helper takes
// share + t x (1 - share) of the roll, elbow to wrist), and the second
// call writes back what the first call put in the seven segment bones
// instead of carrying the engine's repaint. Whichever call the skinning
// reads, it reads our ramp. The ForeArm share defaults to 0 because the
// helpers now do the job it was standing in for: an elbow that does not
// roll, a wrist that takes a fifth, the rest along the forearm as the rig
// was built. kept= on the wgun line counts second calls that re-asserted.
std::atomic<float>    g_wgun_twist_share{0.00f};
std::atomic<float>    g_wgun_twist_arm{0.00f};
// BUILD 176: THE WRIST-FIRST ARM (cfg wgun_wristfirst, default 1; 0 is the
// 162 two-bone solve). Builds 172 to 174 settled it from the log: our
// writes are the pose the frame draws (172), and the visible warp is the
// BEND the two-bone solve imposes at the wrist, 78 to 156 degrees off any
// bend the animation ever used where the arm warps, 0 to 15 where it looks
// normal (173), with the elbow placed from the brace animation's pole and
// the hand's orientation absolute since 162, so nothing related the two.
// The owner, on the 176 screenshot: "lets just fix ik, at least on the
// left". So the left arm is now solved from the hand BACKWARD: the forearm
// runs from the wrist up the hand's own long axis (wrist to middle knuckle,
// reversed, from the records hands_apply just wrote), which is a straight
// wrist by construction; the elbow is the forearm's length along it; and
// the upper arm points from the shoulder at that elbow. When the elbow
// lands farther from the shoulder than the upper arm reaches, the SHOULDER
// record is pulled toward it by the difference, so the overreach shows as a
// small stretch at the clavicle instead of a broken wrist (3.7's shoulder
// placement, the cheap form). Left arm only; the right rides the gun and
// keeps the 162 solve until its own target anomaly (twelfth pass) is read.
// wf= solves, wfsh= solves that moved the shoulder, shmax= the largest
// shoulder shift in the last second, mm, on the wgun line.
std::atomic<int>      g_wgun_wristfirst{1};
// BUILD 178: THE WRIST LIMIT (cfg wgun_wrist_limit, degrees, default 30,
// clamped 0..180, hot-reload). The pure wrist-first solve of 176 puts the
// elbow wherever the fingers point, with nothing in between: fingers at
// the shoulder and the shoulder is pulled half a metre (review finding).
// A real wrist bends within a range. So the two-bone elbow (the 162
// solve, the animation's own elbow side) is computed first, and the bend
// it implies at the wrist is measured against the hand's long axis; if it
// is within the limit that elbow stands, and if not the forearm is rotated
// toward the hand's axis by the excess only. 0 is 176 exactly (a straight
// wrist, the elbow follows the fingers); 180 is 162 exactly (the animated
// elbow, any bend). The shoulder follows the elbow when the arm is short,
// as in 176.
std::atomic<float>    g_wgun_wrist_limit{30.0f};
// BUILD 179: THE SHOULDER CAP (cfg wgun_shoulder_max, mm, default 80,
// hot-reload). The 178 runs (grwxr-24080, 28992) pulled the shoulder record
// 500 to 736 mm from the body whenever the fingers pointed toward it: the
// wrist limit rotated the forearm away by the excess bend, the elbow flew,
// and the shoulder followed, while the clavicle bone stayed with the torso,
// so every vertex blended between them fanned from the arm down to the
// body (the owner's 00:48 screenshot). A stretched clavicle looks worse than
// a wrist bent past the limit. So the shoulder may travel at most this far;
// when the corrected forearm would need more, the correction is scaled back
// until it fits and the wrist takes the rest. wfcap= counts those solves.
std::atomic<float>    g_wgun_shoulder_max{80.0f};
// BUILD 213 (CX-025): THE SHOULDER RIDES THE VIEW OFFSET.
//
// Since build 204 the viewpoint carries the head's translation and the hands
// are built from it, so a hand goes where his real hand is: correct, and not
// in question. The character's shoulder does not move with it, so the whole
// view-to-body displacement is charged to the arm. Measured on the 211 run
// (grwxr-18104.log): the right hand target sat past the arm's full length in
// 88 of 92 first-person seconds, median reach 1.50 where 1.10 is the bound,
// against 0.77 on build 198 before 6DOF existed. Past the bound the target is
// scaled back onto a sphere about the animated shoulder, so the hand leaves
// the weapon and the elbow locks straight: his words, "the ik is messed up,
// the elbow is broken". The collarbone was never in it (shmax=0mm clav=0 all
// run); wgun_shoulder_max is 80 mm and the gap has a median of 0.21 m.
//
// So the shoulder joint is offset by EXACTLY the vector the camera hook added
// to the viewpoint (g_view_off_world, published there), rotated into model
// space. The hand target moved by that vector and nothing else, so moving the
// shoulder by it restores the original shoulder-to-hand geometry exactly: the
// arm keeps the reach it had before 6DOF, at any offset, with no scaling and
// no clamping. The cost is that the shoulder leaves the torso mesh, and the
// skin between spine and deltoid stretches by the offset; arm_anchor_max caps
// how far that is allowed to go. Distributing it over the spine is the next
// build if the stretch reads badly.
std::atomic<int>      g_arm_anchor{0};          // cfg arm_anchor
std::atomic<float>    g_arm_anchor_max{0.15f};  // cfg arm_anchor_max, metres
float                 g_view_off_world[3] = {0.0f, 0.0f, 0.0f};   // present thread writes, rig hook reads
std::atomic<uint64_t> g_view_off_stamp{0};      // d3d11 frame it was written on
std::atomic<uint32_t> g_arm_anchor_bits{0};     // the last anchor length, m, float bits
// BUILD 214: THE ELBOW'S POLE. Which point of the circle about the
// shoulder-to-hand axis the elbow takes. Build 162 took it from the animated
// elbow, which is right while the animation holds what he holds and arbitrary
// when it does not: with no weapon the animated arm hangs at the side, and
// every time his real hand crosses that arm's own line the perpendicular
// collapses through the dead zone and comes out the other side, which is the
// elbow inverting (his words, 2026-09-07 00:10). The fallback is anatomy: an
// elbow hangs BELOW the shoulder-to-hand line, so the seed is model-space
// down, which cannot invert because it does not depend on the animation.
std::atomic<int>      g_arm_pole{0};            // cfg arm_pole
std::atomic<float>    g_arm_pole_min{0.06f};    // cfg arm_pole_min, metres of perpendicular on a 0.28 m upper arm
std::atomic<uint64_t> g_pole_anim[2] = {};      // frames the animated pole was strong enough
std::atomic<uint64_t> g_pole_anat[2] = {};      // frames anatomy supplied it
std::atomic<uint64_t> g_pole_held[2] = {};      // frames neither could and the last one was held
std::atomic<uint64_t> g_pole_flip[2] = {};      // frames the pole came out more than 90 deg from the last
std::atomic<uint64_t> g_pole_rej[2] = {};       // frames the animated pole pointed UP and anatomy took it instead
std::atomic<uint32_t> g_pole_pn_bits[2] = {};   // the last perpendicular, m, float bits
// BUILD 285: the elbow side may not jump. cfg arm_pole_smooth (code default ON), arm_pole_tau_ms, arm_pole_rate.
std::atomic<int>      g_pole_smooth{1};
std::atomic<float>    g_pole_tau_ms{50.0f};     // low-pass time constant of the pole direction
std::atomic<float>    g_pole_rate{540.0f};      // degrees a second, the most the pole may turn about the arm axis
std::atomic<uint64_t> g_pole_clip[2] = {};      // calls the rate limit held the pole back
std::atomic<uint64_t> g_pole_n[2] = {};         // calls with a pole step measured
std::atomic<uint64_t> g_pole_raw_ud[2] = {};    // sum of the raw pole's call-to-call turn, micro-degrees
std::atomic<uint64_t> g_pole_out_ud[2] = {};    // the same for the pole actually used
// BUILD 293: the gravity bias on the elbow (cfg arm_pole_down_l / arm_pole_down_r, 0..1, times the two-hand authority)
std::atomic<float>    g_pole_down_l{0.5f};
std::atomic<float>    g_pole_down_r{0.0f};
std::atomic<uint64_t> g_pole_bias_n[2] = {};    // calls the bias changed the pole
std::atomic<uint64_t> g_pole_bias_ud[2] = {};   // sum of the angle it moved the pole by, micro-degrees
// BUILD 294: the torso turn state (see torso_turn)
std::atomic<int>      g_torso_on{1};            // cfg torso_turn
std::atomic<float>    g_torso_dead{45.0f};      // cfg torso_dead, degrees
std::atomic<float>    g_torso_rate{240.0f};     // cfg torso_rate, degrees a second the body catches up at
std::atomic<float>    g_torso_hand_w{0.35f};    // cfg torso_hand_w, 0..1, the weight of the right controller's bearing
std::atomic<float>    g_torso_max{70.0f};       // cfg torso_max, degrees, the most the torso is turned from the animated one
std::atomic<uint64_t> g_torso_n{0};             // updates the torso was turned
std::atomic<uint64_t> g_torso_catch{0};         // updates spent catching up
std::atomic<uint64_t> g_torso_skip{0};          // updates skipped (not first person, in a vehicle, no frame)
std::atomic<uint64_t> g_torso_clamp{0};         // updates the +-max limit held
std::atomic<uint32_t> g_torso_dbg[6] = {};      // last phi, psi_h, psi_a, psi_b, psi_hand, nodes (float bits; nodes as count)
static float          g_torso_phi_max = 0.0f;
std::atomic<uint64_t> g_torso_why[12] = {};     // BUILD 295: why torso_turn returned: 0 entered, 1 other skeleton, 2 second entry, 3 not fp/vehicle, 4 rig read, 5 nodes, 6 model_down, 7 bone reads, 8 shoulder line, 9 base_frame, 10 reached the state machine, 11 wrote
// BUILD 300: THE LEAN. The torso hinges at the hips toward the head's tracked offset (torso_lean), so the shoulders ride under the head.
// That moves the Head bone, which the camera reads as its anchor, so the rig hook publishes the Head bone before and after the hinge
// (world, seqlock) and the camera takes the displacement back out of what it read (lean_unhinge) before its filter, then adds the
// tracked offset itself as since build 204. The view is therefore exact and never counts the lean twice; the body is only decoration.
std::atomic<int>      g_lean_hinge{1};          // cfg lean_hinge
std::atomic<float>    g_lean_theta_max{65.0f};  // cfg lean_theta, degrees, the most the hinge turns
std::atomic<float>    g_lean_slide{0.05f};      // cfg lean_slide, metres the upper body may slide beyond the hinge to reach the head
struct LeanPub {
    std::atomic<uint32_t> seq;
    std::atomic<uint64_t> frame;
    float pre[3];
    float post[3];
};
static LeanPub        g_lean_pub;
std::atomic<uint64_t> g_lean_c[10] = {};        // 0 entered, 1 other skeleton, 2 second entry, 3 not fp/vehicle, 4 rig or nodes, 5 offset stale, 6 geometry, 7 offset tiny (published zero), 8 hinged, 9 angle clamped
std::atomic<uint64_t> g_lean_slidecap{0};       // hinges whose slide was capped
std::atomic<uint64_t> g_lean_rd[5] = {};        // camera reads: 0 no hinge in force, 1 hinged read undone, 2 UNHINGED read seen (race), 3 no fresh publish, 4 small displacement assumed hinged
std::atomic<uint32_t> g_lean_dbg[5] = {};       // last |offset| m, last angle deg, last head displacement m, last slide m, largest |offset| m (float bits)
static void lean_dbg_put(int i, float v) {
    uint32_t b;
    memcpy(&b, &v, sizeof(b));
    g_lean_dbg[i].store(b, std::memory_order_relaxed);
}
// The camera side. h is the Head bone as read (world); on return it is the animated position when the read had the hinge in it.
static bool lean_unhinge(float h[3]) {
    if (!g_lean_hinge.load(std::memory_order_relaxed)) return false;
    const uint32_t s1 = g_lean_pub.seq.load(std::memory_order_acquire);
    if (s1 == 0 || (s1 & 1u)) { g_lean_rd[3].fetch_add(1, std::memory_order_relaxed); return false; }
    float pre[3], post[3];
    for (int i = 0; i < 3; ++i) { pre[i] = g_lean_pub.pre[i]; post[i] = g_lean_pub.post[i]; }
    const uint64_t fr = g_lean_pub.frame.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_lean_pub.seq.load(std::memory_order_relaxed) != s1) { g_lean_rd[3].fetch_add(1, std::memory_order_relaxed); return false; }
    const uint64_t now = d3d11::frame_count();
    if (now < fr || now - fr > 2) { g_lean_rd[3].fetch_add(1, std::memory_order_relaxed); return false; }
    const float d[3] = {post[0] - pre[0], post[1] - pre[1], post[2] - pre[2]};
    const float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (!(dl >= 0.02f) || !isfinite(dl)) { g_lean_rd[0].fetch_add(1, std::memory_order_relaxed); return false; }
    bool hinged = true;
    if (dl >= 0.06f) {
        // the read sits nearer the hinged position than the animated one, or it is the animation (the camera got there before the hook)
        const float s = ((h[0]-pre[0])*d[0] + (h[1]-pre[1])*d[1] + (h[2]-pre[2])*d[2]) / dl;
        hinged = s > 0.5f * dl;
    } else {
        g_lean_rd[4].fetch_add(1, std::memory_order_relaxed);
    }
    if (!hinged) { g_lean_rd[2].fetch_add(1, std::memory_order_relaxed); return false; }
    h[0] -= d[0]; h[1] -= d[1]; h[2] -= d[2];
    g_lean_rd[1].fetch_add(1, std::memory_order_relaxed);
    return true;
}
volatile uint64_t     grwxr_wg_wfcap = 0;
// BUILD 180: THE COLLARBONE TURNS (cfg wgun_clav_max, degrees, default 25,
// hot-reload; 0 disables). The owner, 00:55: "Why is the collarbone fixed,
// can we do something to add credence to the upper body IK?" Until now the
// Shoulder bone (the clavicle, parent of the Arm) was never written, so
// when 176's shoulder pull moved the Arm record the clavicle stayed with
// the torso and the skin between them fanned. Now the clavicle rotates
// about its own root so its tip meets the moved shoulder joint, the way a
// shoulder girdle rises and reaches; the joint lands on the clavicle's own
// sphere (a clavicle turns, it does not stretch), the rotation is capped,
// and whatever the cap leaves is the upper arm's small stretch. Left side,
// wrist-first path only. clav= on the wgun line is the largest clavicle
// turn in the last second, degrees.
std::atomic<float>    g_wgun_clav_max{25.0f};
std::atomic<uint32_t> g_clav_max_bits{0};   // deg, float bits, reset at print
volatile uint64_t     grwxr_wg_wf   = 0;
volatile uint64_t     grwxr_wg_wfsh = 0;
std::atomic<uint32_t> g_wf_shmax_bits{0};   // mm, float bits, reset at print
std::atomic<uint32_t> g_twist_bits[2]   = {};   // degrees, float bits
std::atomic<uint32_t> g_twist_t_bits[4] = {};   // O_01..O_04 fractions, float bits
volatile uint64_t     grwxr_wg_twist = 0;
volatile uint64_t     grwxr_wg_keep  = 0;       // BUILD 171: second calls re-asserted
static inline float bitsf(uint32_t b) { float f; memcpy(&f, &b, sizeof(f)); return f; }

// BUILD 164: THE TWIST PROBE, log only, side 0. Build 163 spread the hand's
// roll along the forearm helpers by their position and the skin answered
// with fins and stacked wedges instead of the 162 spike, so those bones do
// not take roll the way that assumed. This reads how the ENGINE poses the
// seven forearm-segment bones, in the animated state (the first call on a
// buffer, before our arm write) and in ours (the second call, after it):
// each bone's fraction along the found forearm, its distance off the
// elbow-to-wrist line in mm, and its twist about that line relative to the
// ForeArm bone, plus the hand's own twist the same way. Two decided
// readings: animated helper twists that scale with the animated hand's
// twist say the engine distributes roll through them and give its own
// fractions to copy; twists near zero say the helpers are static in the
// animation and the skin is not built to be rolled through them from here,
// and the roll has to be carried another way. The ours line checks that
// 163's write landed with the sign and size intended.
std::atomic<uint32_t> g_tp_anim[22] = {};   // 7 x {t, d, tw} + hand tw, float bits
std::atomic<uint32_t> g_tp_ours[15] = {};   // 7 x tw + hand tw + 7 x keep, float bits
static uint64_t       g_tp_wrote[2] = {0, 0};   // buffer arms_solve wrote last, per side
// BUILD 166: the quaternion our last write put in each forearm-segment bone
// (side 0, kForeSeg order), so the ours state can say whether it is still
// there. 164's twist_deg was the same ill-conditioned decomposition 165
// retired for the write, so its readings near a half turn were noise; the
// probe now measures every bone's roll as a plane angle too.
static float g_tw_wq[7][4] = {};
static int   g_tw_wq_ok[7] = {};

// BUILD 172: THE SURVIVAL PROBE, log only, side 0, nothing new written.
// The 171 run (grwxr-26684, clip 220406): our helper ramp was written on the
// first call and re-asserted on the second (kept= climbed with twists=) and
// the skin still warped. The owner's read, from the headset: put the real
// hand where the animated arm is and the wrist looks normal; move away and
// it warps, pulling toward the body. The frames agree: the hand renders where
// we put it, the forearm skin stretches from it back toward the animated
// arm. RE-notes section 3 (2026-08-19) said why in advance: a derived-pose
// write at PublishAttachments moves ONE bone and "posing an arm from the
// same site would move the shoulder and leave the forearm behind". Every
// arm build since 163 wrote at that site. Before the arm path moves to an
// earlier window, measure what the renderer's frame actually holds: the
// eleven left-arm records as the first call FOUND them (animated) and as our
// second call LEFT them, then the same records read at Present, once a
// second. Per bone: mm and degrees from our write, mm from the animated
// state. The hand is the control (it visibly follows the controller): a
// sample where the hand reads far from our write is a rotated buffer, not
// evidence, and is counted as stale. Slots 0..9 are the arm chain in
// kArmChain order, 10 is the hand.
static float    g_sv_anim[11][8] = {};   // first call, before our write
static float    g_sv_ours[11][8] = {};   // second call, after our write
static int      g_sv_ok[11]      = {};   // bit 1 anim captured, bit 2 ours
static uint64_t g_sv_buf = 0;            // the buffer both came from
static std::atomic<uint64_t> g_sv_seq{0};
std::atomic<uint32_t> g_sv_out[33] = {};   // per slot: dw mm, dw deg, da mm
std::atomic<uint32_t> g_sv_n{0};
std::atomic<uint32_t> g_sv_stale{0};
std::atomic<uint32_t> g_sv_unstable{0};

// BUILD 173: THE WRIST AND THE REACH, log only, both sides. The 172 run
// (grwxr-21192, 50 samples) read every left-arm record at Present at 0 mm
// and 0 deg from our second-call write: the derived pose we write is the
// pose the frame is drawn from, nothing after us re-derives it, and the
// warp is therefore GEOMETRY we author, not a lost write. Two candidates
// live exactly where the owner places it (three quarters down the forearm
// to the hand, the wrist gone as the forearm sinks into the hand): the
// elbow direction comes from the brace animation's pole, so the solved
// forearm can arrive at the absolute-palm hand from a side no wrist
// allows; and past reach the hand sits up to ten percent beyond the
// forearm's end (ikcl 62 percent of solves in that run). Per call, per
// side: reach = shoulder-to-hand distance over L1 + L2, before the clamp;
// wrist = angle between the written hand's forearm-facing axis (the axis
// that pointed down the animated forearm in the animated hand's frame,
// first call only) and our new forearm axis. Min and max since the last
// print. The two numbers separate the candidates from the log alone.
static float    g_rw_min[2][2] = {{9.0f, 999.0f}, {9.0f, 999.0f}};
static float    g_rw_max[2][2] = {{0.0f, -1.0f}, {0.0f, -1.0f}};
static uint32_t g_rw_n[2] = {0, 0};

// BUILD 160: THE STOW SIGNAL, log only, writes nothing. HANDOFF 12.3: the
// stow fling exists because no equipped/stowed state reaches the hand write
// path, and no located field carries one (m_bWeaponInHolster is unfound,
// gated RE). The cheapest candidate signal is already in this function's
// hands: the ANIMATED right hand against the ANIMATED gun root, both as
// found before any write. Build 69 measured that distance at 0.258 m,
// stable to a millimetre, while gripping. What it does during a stow has
// never been measured, and the gate cannot ship on a guess (build 157's
// lesson). So measure it: min and max since the last 1 Hz print, in the
// wgun line as stowd=. Two decided readings: the stow animation separates
// hand from gun root cleanly past ~0.4 m, and the per-call gate ships next
// build on that threshold; or they stay glued and the drawn state must come
// from the engine's own flag or the DRAW state machine instead.
std::atomic<uint32_t> g_stow_d_min{0xFFFFFFFFu};   // mm since last print
std::atomic<uint32_t> g_stow_d_max{0};

// BUILD 142: THE ARMS FOLLOW (cfg wgun_arms). The owner's redirect: translate
// the arms rather than hide them. The handoff's original choice of
// hide-over-solve was made when the hands were scenery; with the hands
// confirmed riding the gun and reaching, the arm is now the visible thing
// left behind, so it gets the solve after all. A two-bone analytic elbow:
// shoulder joint stays where the animation put it, the elbow lands on the
// circle the two segment lengths define, on the side the ANIMATION's own
// elbow already points (the pole is measured, not tuned), and each segment's
// bones are carried rigidly by the shortest arc that takes the old segment
// direction onto the new one, so the animated twist and skinning survive.
// Segment lengths are the running minimum seen since the skeleton latched:
// the animation never stretches a bone, our own beyond-reach frames only
// ever inflate, so the minimum is the truth.
std::atomic<int> g_wgun_arms{1};

// BUILD 143: the arm segment lengths, running minimums keyed to the armed
// skeleton. File scope because BOTH the free-hand bound (hands_apply) and the
// elbow solve (arms_solve) need them, and both run inside the same
// engine-thread call, so plain floats are safe. 1e9 = not yet measured.
float    g_seg_len[2][2] = {{1e9f, 1e9f}, {1e9f, 1e9f}};
uint64_t g_seg_skel = 0;

// BUILD 144: THE RAY PROBE (cfg ray_probe, default 1, log only). The
// 2026-08-31 night session showed the written shot pitch sinking 11 degrees
// over 8 seconds under a rock-steady head, and moving inconsistently during
// deliberate head sweeps, with every shot WROTE from the muzzle. Three
// candidates cannot be separated by firing: head pitch leaking through the
// controller-to-world mapping (inv HMD composed with a lagging or partial
// camera basis), plain arm droop, and the flapping two-hand gate bending the
// ray toward the off hand. This publishes the inputs at 1 Hz so a braced
// controller plus a head sweep separates them in one minute: raw = the
// controller ray BEFORE the two-hand blend, sdir = the published barrel
// direction AFTER blend and filter, tw = the blend authority that frame,
// cam = the camera forward. Braced hand + head sweep: raw moves = the
// mapping leaks; raw still but sdir moves = the blend; both still = droop.
std::atomic<int>      g_ray_probe{1};
std::atomic<uint32_t> g_dbg_raw[3] = {};    // float bits
std::atomic<uint32_t> g_dbg_sdir[3] = {};
std::atomic<uint32_t> g_dbg_tw{0};

// BUILD 143: the visible-barrel pitch trim (cfg wgun_pitch_deg). The weapon
// MESH's barrel sits a constant pitch below the bone axis the swing aligns
// (the section-6 open item, finally seen because the owner now shoulders and
// sights). The shot follows s_dir and must keep doing so, therefore the trim
// rotates the SWING TARGET only: positive degrees raise the bone axis, which
// raises the visible barrel onto the unchanged shot line. Converged at a
// wall: fire, and raise the trim until the holes sit on the barrel.
std::atomic<float> g_wgun_pitch{0.0f};

// BUILD 142: THE CALIBRATION CAPTURE (cfg hand_calib, momentary), measurement
// only, the owner's wingspan idea. Set hand_calib to 1 and the capture logs
// your controller span against the avatar's arm span plus the head height.
// APPLYING a scale from those numbers is the next build's decision, made from
// this measurement, not from a guess.
//
// 2026-08-31, THE ARMING DELAY, the owner's own fix for a procedure that could
// not be followed: the capture wants both arms straight out to the sides, and
// arming it needed a hand on the panel, so the pose and the trigger fought each
// other and the sample was always taken mid-reach. Arming now starts a timer
// instead of capturing, which buys the time to select the row, let go and get
// into the pose. Voice marks both ends so the pose can be held blind with the
// headset on. The 1 Hz census owns the capture, so the true wait is one tick
// of slack past the delay; the log prints the delay that was actually used.
constexpr uint32_t    kCalibDelayMs = 3500;
std::atomic<uint64_t> g_calib_due_ms{0};   // 0 = nothing pending

// 2026-09-01: THE ARM SCALE, and the reason the capture stopped being log-only.
//
// The owner's design: one button, and after it fires you are in first person
// with the correct arm length already applied. Measuring and then asking a
// human to act on the number is how the ratio went unmeasured for seven builds.
//
// The scale is avatar reach over player reach. It multiplies the FREE-HAND IK
// target about the shoulder, so a full real reach maps onto a full avatar
// reach instead of being clipped by the anatomical bound. Every session so far
// clamped 93 to 100 percent of free-hand calls (hfar against free), which is
// what being asked to reach somewhere the character's arm cannot go looks like
// from the inside.
//
// WHERE IT DELIBERATELY DOES NOT APPLY: the two-hand aim reads the raw
// controller positions through aimtrace::ctrl_pos_l, not this target, so the
// scale cannot move the barrel. That separation is the whole reason this is
// safe to apply automatically, and it is the thing to re-check before ever
// widening it. The existing reach clamp stays as a safety net behind it.
//
// 1.00 is identity. If the capture reports a ratio near 1.000 this changes
// nothing, which is the honest outcome and not a failure.
std::atomic<float> g_hand_scale{1.0f};

// BUILD 283: THE SHOULDER PULL (cfg wgun_pull, default 0 = build 282 exactly). F-270 and F-271: from the animated shoulders
// his hand targets are out of reach on a third to nine tenths of the frames, so the arm locks straight; the gap on failing
// frames is 82 to 180 mm and a clavicle swing gains only 45 to 50 mm. Here the whole limb is carried toward the hand by
// exactly what the reach needs (the build 213 anchor, now per side and per frame): the shoulder moves, the controller does
// not, and the arm stays at wgun_pull_k of full extension. The left hand also stops being scaled about the shoulder
// (hand_scale) and its reach bound grows by the pull cap, so the visible hand is on the controller.
std::atomic<int>   g_wgun_pull{0};
std::atomic<float> g_wgun_pull_k{0.97f};       // the arm never gets past this fraction of L1+L2
std::atomic<float> g_wgun_pull_max{250.0f};    // mm, the most the limb may be carried
static uint64_t    g_pull_calls[2] = {0, 0};   // first calls on a buffer (rig-write thread only; read by the 1 s tick)
static uint64_t    g_pull_n[2]     = {0, 0};   // of those, how many needed a pull
static uint64_t    g_pull_cap[2]   = {0, 0};   // of those, how many hit the cap (the arm still lacks reach)
static uint64_t    g_pull_sum_um[2] = {0, 0};  // sum of the pulls, micrometres
static uint32_t    g_pull_max_um[2] = {0, 0};  // the largest pull this second, micrometres
static uint64_t    g_seg_rej[2]    = {0, 0};   // BUILD 284: first-call segment-length samples refused as not an animated arm

// Set by a successful capture, consumed by VRMirror's key poll, which owns the
// first-person path (and the recenter folded into it since build 44). Routed
// as a request rather than called directly so there is ONE first-person path
// and it stays the tested one.
std::atomic<int> g_fp_request{0};
std::atomic<uint32_t> g_armlen_bits[2][2] = {};   // float bits, census reads

// BUILD 140: HIDE THE ARMS AT BONE LEVEL (cfg wgun_hide_arms, default 0, a
// DIAGNOSTIC until the headset rules). The confirmed cost of hands-on-gun is
// extreme mesh stretching between the animated forearm and the transported
// hand. The piece-level cure is stepping hide_child; THIS is the fallback for
// the case the handoff flagged, one render piece covering arm and hand
// together. Each write call collapses the ten arm and helper records a side
// onto the shoulder record's position and shrinks their quaternions (the
// pose root note says a quaternion's norm carries uniform scale; if that
// holds for bone records the arm shrinks to nothing, and if it does not the
// arm still bunches at the shoulder instead of stretching to the hand).
// Runtime only, per frame, on top of the engine's own solve: switching it
// off restores the stock arms on the next animation solve.
std::atomic<int> g_wgun_hide_arms{0};

constexpr uint32_t kShoulderHash[2] = {0x2D4660A8u, 0xF60647E5u};
constexpr uint32_t kArmChain[2][10] = {
  { 0xEB830ADAu, 0x89B93A80u, 0x38E4FAFFu,  // LeftArm LeftForeArm O_LeftElbow
    0xEE73A194u, 0xF91EC076u, 0x52F4D844u,  // L_01LeftArm O_02LeftArm O_01LeftForeArm
    0x256A0AB4u, 0xBECF46DBu, 0xCA57AF54u,  // O_02LeftForeArm O_03LeftForeArm O_04LeftForeArm
    0xEC3F5AB0u },                           // L_LeftForeArmLookAt
  { 0x6BB3F727u, 0x7257A1AAu, 0xEB35514Au,  // RightArm RightForeArm O_RightElbow
    0xFAD26B1Bu, 0xB4C1852Eu, 0xEE85EAE1u,  // L_01RightArm O_02RightArm O_01RightForeArm
    0x534F862Fu, 0x8ED95FAAu, 0xF3AA59F2u,  // O_02RightForeArm O_03RightForeArm O_04RightForeArm
    0x2C4D0487u }                            // L_RightForeArmLookAt
};
uint16_t              g_arm_node[2][10] = {};
// BUILD 182: THE WRIST BONE. GR_PCF_Skeleton_Average (file 0x0ea138a544,
// parsed from the game data 2026-09-03 01:30) has TWO children of LeftForeArm the
// arm chain never carried: bone 11, name hash 0x6f93f700, whose name ends
// in "LeftWrist" (the prefix is not in the wordlist; the hash is what the
// rig resolves), and bone 38, hash 0xe295263a, unresolved. Mirrors under
// RightForeArm: bone 52, 0x6773ca27, "...RightWrist", and bone 80,
// 0x607986f8. A wrist bone exists to carry the cuff skin. Every build since
// 142 moved the hand and the ten chain bones and left these two where the
// animation put them, at the animated wrist near the chest in the brace
// pose, so the cuff vertices weighted to them fanned from the real hand
// back to that point and moved with the character's sway: the owner's
// "IT IS FOLLOWING THE ANIMATIONS", and the fan in his 01:25 screenshot
// with every other record proven ours at Present (172). They now ride the
// forearm segment like the helpers, with the hand's whole roll when the
// ramp is on, both sides.
constexpr uint32_t    kForeExtra[2][2] = {{0x6F93F700u, 0xE295263Au},
                                          {0x6773CA27u, 0x607986F8u}};
uint16_t              g_fore_extra[2][2] = {{0xFFFFu, 0xFFFFu},
                                            {0xFFFFu, 0xFFFFu}};

// BUILD 183: THE MOUNT MIRROR. The gun-root write goes to node 10,
// FakeGunRoot_Gameplay, and all night the held weapon's origin sat on it
// (mount: lines, grwxr-3128, 80 of 84 samples). The other four: the
// weapon on Prop_LeftHand 01:13:15, on Prop_RightHand 01:16:48 to
// 01:17:03, on Fake_gunroot 01:19:29 to 01:19:50 and 01:22:52 to 01:22:57,
// each inside a roll or a climb, and the owner lost the weapon's roll twice,
// each time after such an animation. The engine re-attaches the held
// weapon to the ANIMATED gun root or to a hand prop bone for those
// animations (m_GunRootBone, weapon+0xB0, resolves FakeGunRoot_Gameplay
// and FALLS BACK to Fake_gunroot, AIM-2026-08-27), and while it is there
// our node-10 record is not what renders. So the record we write is copied
// onto whichever bone the engine might attach to instead: Fake_gunroot
// always at mode 1, plus the two hand props at mode 2. Node indices are
// re-resolved from the rig's name map every tick like the gun node; 0xFFFF
// = not on this rig. The copy is translation xyz and the quaternion; each
// target keeps its own w lane. cfg wgun_mirror, 0 = build 182. The code
// default is 1 (the engine's own documented fallback); the props were 2
// of the 4 windows, so the first test runs at 2 from the live cfg.
constexpr uint32_t    kMirrorHash[3] = {0x826846F3u,    // Fake_gunroot
                                        0x53135E44u,    // Prop_RightHand
                                        0x85562B5Cu};   // Prop_LeftHand
uint16_t              g_mirror_node[3] = {0xFFFFu, 0xFFFFu, 0xFFFFu};
std::atomic<int>      g_wgun_mirror{1};
static float          g_tw_wq_x[2][2][4] = {};
static int            g_tw_wq_x_ok[2][2] = {};
uint16_t              g_shoulder_node[2] = {0xFFFFu, 0xFFFFu};
std::atomic<uint32_t> g_arm_n[2] = {0, 0};

// The separation band over which the front hand takes authority. Below Lo the
// weapon is aimed as it was before; above Hi the front hand has it entirely.
//
// Sized for compressed holds, not for the 0.497 m measured standing. Full
// authority is kept down to 0.22 m so the front hand never goes soft in a
// working position. The fade below that is not a preference, it is the point
// where the geometry stops carrying a direction: error scales as jitter over
// separation, so at 0.12 m a millimetre of tracking noise is already about
// half a degree of muzzle, and it diverges as the hands meet.
constexpr float kTwoHandLo    = 0.12f;   // m
constexpr float kTwoHandHi    = 0.22f;   // m

// How far the hand-to-hand line may disagree with the rear controller's own
// forward before the frame is refused. Loose on purpose: it is there to catch
// tracking pops and an off hand somewhere impossible, not to constrain how the
// weapon is held.
constexpr float kTwoHandAgree = 0.35f;   // cos, about 69 degrees

// BUILD 120: the gate is TUNABLE now, and it MEASURES itself.
//
// Why: the 2026-08-20 headset run reported two=4028 tworej=9556. The gate
// refused SEVENTY PERCENT of two-handed frames, which is not a tracking-pop
// rate, it is a systematic disagreement. A gate written to catch "an off hand
// somewhere impossible" was instead deciding whether two-handed works at all,
// and it was hard-coded so the owner could not chase it from the headset.
//
// Default is kTwoHandAgree exactly, so this build changes NOTHING until the
// value is moved. Band is -1.00 to 1.00, which is the full range of a cosine:
// -1.00 accepts every frame including a fully reversed hold, 1.00 accepts only
// a perfectly aligned one. Those are the real limits of the quantity, not a
// guess about what a tester could want. At -1.00 the gate is off, which is a
// legitimate diagnostic setting: if two-handed becomes reliable there, the
// gate was the whole problem.
std::atomic<float> g_wg_two_agree{kTwoHandAgree};

// 2026-08-31b: THE ATTACH BAND (cfg wgun_two_attach), the upper edge the
// authority band never had. kTwoHandLo/Hi fade the front hand IN as the hands
// separate and then nothing ever fades it OUT, so a left hand hanging at your
// side, half a metre from the weapon, still owned the direction, and with the
// rear-hand anchor it would own the position too. That is the documented
// split: the off hand measuring 0.507 m from the gun, "the direction comes
// from a hand holding nothing".
//
// The numbers are the record's, not a guess: real holds measure 0.105 to
// 0.409 m hand to hand, the authored aim clips hold 0.240 to 0.283 m, and the
// one documented failure sat at 0.507 m. Release begins at this separation
// and completes 0.15 m later (0.45 releases fully by 0.60), smoothstepped
// like the lower edge so the failure mode is "the front hand quietly lets
// go", never a jump. Everything scaled by two_w fades together: direction,
// anchor and slide, one curve.
std::atomic<float> g_wg_two_attach{0.45f};

// BUILD 145: THE GRIP GATE (cfg wgun_two_grip), from the night the probe
// convicted the blend: tw=1.00 through the whole shooting window, the
// published direction 6 to 12 degrees above the controller ray, and the
// confirmed one-handed aim of 08-30 buried under a hand-to-hand line the
// owner never asked for that frame. Like native VR titles: the two-handed
// hold has authority only while the OFF hand is physically squeezing its
// grip button, through a 0.35..0.65 smoothstep so half-pressure is half
// authority and a release is a fade, not a snap. It multiplies INTO two_w
// after the attach band, so separation, agreement and attach all still
// apply; it can only ever REMOVE authority, never add it.
std::atomic<int>      g_wg_two_grip{0};
std::atomic<uint32_t> g_grip_off_bits{0};   // off-hand squeeze 0..1, float bits

// BUILD 123: WHICH HAND IS IN FRONT IS MEASURED, NOT ASSUMED.
//
// The block below was written around "the rear hand is the RIGHT hand and the
// front hand is the LEFT", which is how a right-handed shooter holds a rifle.
// `[VERIFIED, log grwxr-29396, 128 samples]` that is inverted for this tester.
// Of the 11 samples with the hands more than 0.30 m apart, the only ones that
// can be a two-handed hold at all, the median angle between the hand-to-hand
// line and the right controller's own aim was 140 degrees, and the five widest
// read 141, 137, 148, 146 and 147. A front hand cannot sit behind the muzzle.
// Flipped, those same samples read 39, 43, 32, 34 and 33 degrees, which is an
// ordinary rifle hold. His RIGHT hand is the front hand.
//
// That single sign is the whole reason the gate refused 100% of frames
// (mean 90 deg, best 81 deg, threshold 70 deg, zero accepted). The gate was
// never tight. It was reading the barrel backwards, and every plan to widen
// the threshold would have made it worse, because at 0.00 it lands exactly on
// the measured mean and would flip between accepting and refusing at 72 Hz.
//
// So the direction is taken rear-to-front with the rear hand DETERMINED, by
// projecting the hand-to-hand line onto the aim the rear controller already
// publishes and flipping it when it points backwards. This is handedness
// agnostic: it works for a left-handed and a right-handed hold with no setting,
// and it cannot make a correct hold worse, because a correct hold never trips
// the flip. 1 = measure (default), 0 = the pre-123 fixed assumption.
std::atomic<int> g_wg_two_auto{1};
volatile uint64_t grwxr_wg_twoflip = 0;   // frames the front hand was the right

std::atomic<uint64_t> g_wg_agree_n{0};       // refused samples
std::atomic<uint64_t> g_wg_agree_sum_milli{0};   // sum of (agree*1000)+1000
std::atomic<int32_t>  g_wg_agree_best_milli{-2000};  // best refused, *1000
std::atomic<int32_t>  g_wg_agree_acc_worst_milli{2000};  // worst ACCEPTED

// Drained at 1 Hz. Reports the gate's own decisions so the threshold can be
// chosen from measurement. `best` is the closest a REFUSED frame came to
// passing, `mean` is the average refused cosine, and `acc_worst` is the worst
// frame that was ACCEPTED. If `best` sits just under the threshold the gate is
// merely a little tight; if `mean` is far below it, the hold genuinely does
// not match the rear controller's forward and the fix is elsewhere.
void two_agree_report(char* out, size_t n) {
    const uint64_t cnt = g_wg_agree_n.load(std::memory_order_relaxed);
    const float thr = g_wg_two_agree.load(std::memory_order_relaxed);
    if (!cnt) {
        snprintf(out, n, "twoagree: thr=%.2f (%.0f deg), no refusals | "
                 "auto=%d frontRIGHT=%llu",
                 thr, acosf(thr < -1.f ? -1.f : (thr > 1.f ? 1.f : thr)) * 57.29578f,
                 g_wg_two_auto.load(std::memory_order_relaxed),
                 (unsigned long long)grwxr_wg_twoflip);
        return;
    }
    const float mean = (float)((double)g_wg_agree_sum_milli.load(std::memory_order_relaxed)
                               / (double)cnt) / 1000.0f - 1.0f;
    const float best = (float)g_wg_agree_best_milli.load(std::memory_order_relaxed) / 1000.0f;
    const int32_t aw = g_wg_agree_acc_worst_milli.load(std::memory_order_relaxed);
    snprintf(out, n,
             "twoagree: thr=%.2f (%.0f deg) | refused=%llu best=%.2f (%.0f deg) "
             "mean=%.2f (%.0f deg) | accepted_worst=%.2f | auto=%d frontRIGHT=%llu",
             thr, acosf(thr) * 57.29578f,
             (unsigned long long)cnt,
             best, acosf(best < -1.f ? -1.f : (best > 1.f ? 1.f : best)) * 57.29578f,
             mean, acosf(mean < -1.f ? -1.f : (mean > 1.f ? 1.f : mean)) * 57.29578f,
             aw > 1000 ? 1.0f : (float)aw / 1000.0f,
             g_wg_two_auto.load(std::memory_order_relaxed),
             (unsigned long long)grwxr_wg_twoflip);
}


// The measurement, so the threshold stops being an argument. Every frame that
// reaches the gate records its cosine; the drain reports the worst and the
// mean of the REFUSED ones, which is the number that says how far the gate
// would have to move to accept his actual hold.



// Build 81: position, off by default so the verified rotation cannot regress
// behind it. clamp is the hard cap in metres on how far the gun may sit from
// the engine's own placement, which bounds every possible tracking failure.
std::atomic<int>   g_wgun_pos{0};
std::atomic<float> g_wgun_pos_scale{1.0f};
std::atomic<float> g_wgun_pos_clamp{1.0f};
std::atomic<float> g_wgun_pos_smooth{0.35f};

// Build 99: the grip. See CameraProbe.h. All zero = the build 81 behaviour.
std::atomic<float> g_wgun_grip_fwd{0.0f};
std::atomic<float> g_wgun_grip_lat{0.0f};
std::atomic<float> g_wgun_grip_up{0.0f};
std::atomic<float> g_wgun_grip_two{0.0f};
// BUILD 126: bypass the static grip offset for the blob test. Does NOT
// overwrite the tuned values, so the test costs nothing to undo.
std::atomic<int>   g_wgun_grip_bypass{0};
// BUILD 219: THE GRIP BONE. Every gun body's own rig carries wb-HandRight
// (106 of 106, GRIP-2026-09-05), parented to wb-gunroot at the origin on 105
// of them, so in the weapon's MODEL-space pose buffer its record IS the grip's
// offset from the root: +X weapon-right, +Y along the barrel, +Z up, metres.
// Read at pick (1 Hz, the drain tick), published here, consumed by the anchor,
// which moves the root by MINUS this so the grip lands on the controller.
// BUILD 220: taken only from a pick ON the gun root and HELD through a lost
// or far pick; only the switch clears it (the 219 run's jumps to the stock).
// The archive table is the check, the live rig is the source. Position only.
std::atomic<int>   g_wgun_grip_bone{1};
std::atomic<int>   g_wgun_grip_bone_ok{0};
std::atomic<float> g_wgun_grip_bone_x{0.0f};
std::atomic<float> g_wgun_grip_bone_y{0.0f};
std::atomic<float> g_wgun_grip_bone_z{0.0f};
// BUILD 220: the offset is HELD across a lost pick; this logs that once.
static bool        g_gb_nopick_logged = false;
// BUILD 221: THE PIP SCOPE. The eyepiece is the weapon's wb-AimingPoint
// (0x90AAC2D0; the FAL's ACOG sub-rig puts it 0.300 m along the barrel and
// 0.038 m above the root), read from the live rig at pick exactly like the
// grip bone and held the same way; the cfg fallback stands in when a rig has
// none (23 of 106 bodies carry it themselves, most get it from the optic).
// The anchor publishes the eyepiece's WORLD position every write; the present
// thread draws the disc there. Position only; nothing here touches the aim.
std::atomic<int>   g_pip_on{0};   // review 3: off in code, the cfg turns it on
std::atomic<int>   g_pip_aim_ok{0};
std::atomic<float> g_pip_aim_x{0.0f};
std::atomic<float> g_pip_aim_y{0.0f};
std::atomic<float> g_pip_aim_z{0.0f};
std::atomic<float> g_pip_fb_lat{0.0f};
std::atomic<float> g_pip_fb_fwd{0.30f};
std::atomic<float> g_pip_fb_up{0.04f};
std::atomic<int>   g_pip_lens_ok{0};
std::atomic<float> g_pip_lens_x{0.0f};
std::atomic<float> g_pip_lens_y{0.0f};
std::atomic<float> g_pip_lens_z{0.0f};
std::atomic<uint64_t> g_pip_lens_frame{0};   // review 1: the frame it was published; stale after 4
std::atomic<int>   g_pip_block_ads{0};   // review 3: off in code, the cfg turns it on
std::atomic<int>   g_pip_user_on{0};     // BUILD 237: the L3 latch, scope up/down
std::atomic<int>   g_pip_toggle{1};      // BUILD 237: 1 = L3 latch, 0 = hold the trigger
// BUILD 238: NATIVE VR ADS. The magnified sight picture is the GAME'S OWN, drawn
// full screen by the flat-scope path of 11d, and the rifle drives where it looks.
// Measured 2026-09-13 on his machine: the FAL's magnified optic settles at 0.2549
// rad, iron sights at 0.6882, hip at 0.8150, so a gate anywhere between 0.26 and
// 0.68 separates magnified optics from everything else. What it does NOT separate
// is a settled optic from an iron-sight ADS TRANSITION, which sweeps through
// ~0.248: seven milliradians apart. That is why raising mono_scope_fov alone
// produced the session that crossed the gate 48 times and reported the head
// "swinging on a stick", and why this is stability gated rather than instantaneous.
std::atomic<int>      g_scope_native{0};   // cfg scope_native: the rifle keeps the aim in the game's ADS
std::atomic<uint32_t> g_scope_hold{6};     // cfg scope_hold_frames: frames the fov must HOLD past the gate
std::atomic<int>      g_scope_engaged{0};  // the latched flat-scope state, read by the present thread
std::atomic<uint64_t> g_scope_enters{0};   // diagnostics: latch transitions, both ways
std::atomic<uint64_t> g_scope_exits{0};
uint32_t g_scope_below = 0, g_scope_above = 0;   // engine thread only, inside first_of_frame
// Review 3 of build 221: the trigger block needs a DRAWN weapon, not the body
// anchor (which stays armed seated and holstered): a pick sitting on the gun
// root within 0.10 m, published from the 1 Hz pick; stowed and holstered gear
// reads 0.4 to 0.8 m, a vehicle stows the weapon, so the trigger goes back to
// the game as brake, reverse and descend within a tick of getting in.
std::atomic<int>   g_pip_drawn{0};
// BUILD 222: the drawn flag is HELD across empty census ticks. The 221 run's
// ring was empty on 50 of 210 ticks with the rifle in hand, and every one
// dropped the flag, handed the trigger to the game for a second and snapped
// it into ADS. Cleared only by two consecutive FAR picks (stowed or
// holstered gear, 0.4 to 0.8 m) or four consecutive empty ticks.
static int         g_pip_far_ticks  = 0;
static int         g_pip_none_ticks = 0;
// `[VERIFIED, 2026-08-13]` the support hand sits this far along the barrel from
// the gun root, measured on the ANIMATED skeleton (so it is the engine's own
// idea of where the front hand belongs), stable to one millimetre over 112
// samples and cross-checking a prior session's 0.498 m total distance.
const float kHandguardFwd = 0.481f;

// Build 35: NO CAMERA BLUR (RE-notes "NO CAMERA BLUR, decoded from the
// community FP mod's cheat table", session 23). The community FP mod ships
// without the FP body blur; its cheat table's whole mechanism is one byte,
// the immediate of `mov sil, 1` forced to 0 inside the function that also
// compares the name hash 0x826846F3.
//
// CRACKED 2026-08-20: 0x826846F3 is zlib crc32("Fake_gunroot"). The engine
// stores bone names only as CRC32 (they exist as strings nowhere in GRW.exe
// nor in any of the 29,640 extracted DataPC files), which is why this sat
// unidentified. So the function we patch is gun-root related, not a generic
// blur routine, and the same immediate appears in GetGunRootBone
// (0x14D531BC, `mov r8d, 0x826846F3`) as its fallback bone name.
//
// This matters for anti-tamper review: one of the 28 named triggers is
// GR_sCameraDepthRangeBlurSubComponent_TamperedBlur, and this is a real
// .text write in the blur subsystem. Whether that trigger can observe this
// byte is [UNKNOWN] and is under investigation. Do not widen this patch.
//
// Re-derived in our build:
// expected match RVA 0x124DE4CC, patch byte at match+5 (RVA 0x124DE4D1).
// WRITE-CLASS NOTE, stated honestly: this is our first write inside a real
// function body rather than an int3-padded thunk slot (same class as the
// parked accuracy patch). The AOB must be unique AND at the documented RVA
// AND the byte must read 01 before anything is written; any failure leaves
// the game untouched (rule 7). uninstall() restores the byte.
constexpr size_t      kNoBlurImmOff   = 5;   // match RVA is per-build (GameBuild.cpp)
constexpr const char* kNoBlurSig      =
    "45 89 E6 40 B6 01 E8 ? ? ? ? 3D F3 46 68 82";
uint8_t* g_noblur_byte = nullptr;

// BUILD 119: THE ENGINE'S OWN CAMERA POSE OFFSET. Data only, no hook.
//
// The owner's insight O-5 was that the engine ARBITRATES the camera and we are
// an unregistered contender. This is the registration. The engine ships a
// singleton whose whole purpose is "add this to the gameplay camera", and it
// reads it every frame of normal play:
//
//   struct CameraPoseOffset {        // vtable 0x039B6D88, ctor 0x0C1945D0
//     void*  vtbl;                   // +0x00
//     float  pos[4];                 // +0x10  init 0     WORLD space
//     float  quat[4];                // +0x20  init identity, CAMERA-LOCAL
//     float  fov;                    // +0x30  init 0     radians
//     uint8  mode;                   // +0x34  init 1     see below
//   };
//
// *** +0x34 IS NOT AN ENABLE FLAG. CORRECTED 2026-08-20. ***
// [VERIFIED, asm at 0x0C233F18] it selects between two branches:
//     CMP byte [RAX+0x34],0
//     JZ  -> MOVAPS XMM0,[RAX+0x10]   ; ZERO  = ABSOLUTE: pose := offset
//                                     ;   VERBATIM, discarding the game camera
//            ADDPS  XMM0,[RAX+0x10]   ; NONZERO = additive (ctor default 1)
//
// So writing 0 to "turn it off" would SLAM THE CAMERA to whatever pos holds.
// To stand down, write identity (pos 0, quat 0,0,0,1, fov 0) and LEAVE +0x34
// ALONE. Never write +0x34 unless you specifically want absolute camera
// authority, which is a different feature with different risks.
//
// THREE MORE THINGS THAT WILL BITE, all [VERIFIED]:
//  1. pos is WORLD space and is NOT rotated by the camera basis. A headset
//     head translation must be rotated into world space by the composed
//     camera orientation first, or leaning goes in a fixed world direction
//     regardless of which way the player is facing.
//  2. KEEP pos.w = 0. The add covers all four lanes and the matrix build ORs
//     the bit pattern of 1.0f into w, so a non-zero w corrupts it rather than
//     being ignored.
//  3. The FOV add is CONDITIONALLY DISCARDED: per viewport, if
//     sceneObject+0x286 != 0 the fov comes from +0x290 instead and the
//     composed fov is thrown away. That is the same override the scope work
//     wants to use. Do not build anything load-bearing on offset.fov.
//
// LIFETIME: [VERIFIED] the destructor at 0x0C19C124 sets the GLOBAL to NULL.
// Re-read the pointer every frame; a null deref here is a crash, not a
// glitch. A pointer that is null or has changed means the object was
// reconstructed (which also resets pos/quat/fov and forces additive mode),
// so re-assert. Never cache the OBJECT address across a level load.
//
// MODE GATING IS BETTER THAN FEARED: [VERIFIED] this is the ONLY read site,
// reached only from director mode 1. Modes 0, 2, 3, 4 and 5 take different
// branches and never touch the offset, so a stale value CANNOT corrupt
// cutscene, vehicle, drone, photo or menu views through this path. But during
// a camera BLEND mode 1 falls through and the offset is not applied, so head
// translation freezes for the blend. Let it freeze; do not compensate.
//
// COLLISION ORDER, and it is the opposite of the feared one: [INFERRED,
// strong] the camera collision solve has already finished when we are called,
// and the engine never re-solves against our result, so head translation does
// NOT fight the spring rail and cannot oscillate. The real hazard is that
// leaning toward a wall puts the eye THROUGH it with no push-back. A
// mod-side clamp is required; the engine will not provide one.
//
// Consumed at 0x0C233EEC inside CameraDirector's mode-1 (normal gameplay)
// apply path: `cmp byte [rax+0x34],0` then `addps xmm0,[rax+0x10]` and a
// Hamilton product q_component (x) q_offset, then the result goes through
// Camera::SetWorldTransform for every live camera.
//
// [VERIFIED] It is UNCONTENDED. A scan of every disp32 in .reloc/.link
// resolving to the global, validated against real instruction boundaries,
// finds three construction sites and EXACTLY ONE READ. No shipped code ever
// writes a non-zero value into it.
//
// WHY THIS IS DIFFERENT FROM SceneView+0x14D: there, the interesting branch
// has almost certainly never executed in a shipped game. Here the CONSUMING
// branch executes 72 times a second because the constructor sets enabled=1.
// Only the values are zero. Writing them turns `addps xmm0,[rax+0x10]` from
// adding zero into adding something. No new control flow anywhere.
//
// Three further properties that matter:
//   - the quaternion is applied in the CAMERA'S OWN FRAME, which is exactly
//     the convention a VR head rotation wants
//   - NO ALIGNMENT HAZARD: the engine's movaps/addps read ITS OWN aligned
//     object, unlike Camera+0x2A0 where we supply the movaps source
//   - capture pollution becomes structurally impossible, because a clean
//     engine pose stays readable at component+0xE0/+0xF0 at all times
//
// HONEST LIMITATION: the consumer runs in the GRAPHICS phase, so this fixes
// the VIEW and does NOT reach the *AfterCamera bone tasks. Camera-relative
// arm and head posing still animates against the engine's own camera.
//
// The signature anchors the global by its own rip-relative load, so no
// address is hardcoded. Scanned against the LIVE 2026-08-19 GRW.exe: 1 hit.
constexpr const char* kCamOffsetSig =
    "48 8B 05 ? ? ? ? 0F 28 87 E0 00 00 00 0F 28 AF F0 00 00 00 "
    "F3 0F 10 B7 00 01 00 00 F3 0F 11 75 30 0F 29 44 24 30 "
    "0F 29 6C 24 20 80 78 34 00 0F 84 ? ? ? ? 0F 58 40 10";
uint8_t** g_camoff_global = nullptr;   // -> CameraPoseOffset*

// BUILD 198: the director dispatcher's own load of the director global, then
// the mode at +0x6C: mov rdi,[rip+disp]; mov ecx,[rdi+0x6C]; lea eax,[rcx-4];
// cmp eax,1; ja. One hit on the installed 2026-08-12 image and on the archive.
constexpr const char* kDirectorSig =
    "48 8B 3D ? ? ? ? 8B 4F 6C 8D 41 FC 83 F8 01 0F 87";
uint8_t** g_director_global = nullptr;  // -> the camera director (read only)
// BUILD 198: the mode-1 apply's select and store: cmp byte [rax+0x286],0; je;
// mov eax,[rax+0x290]; mov [rdi+0x2B0],eax; jmp; movss xmm0,[rbp+0x30]; movss
// [rdi+0x2B0],xmm0. One hit. Its presence is the proof that this image selects
// the scene fov override before storing the field; without it nothing is written.
constexpr const char* kSiteBStoreSig =
    "80 B8 86 02 00 00 00 74 0E 8B 80 90 02 00 00 89 87 B0 02 00 00 EB 0D "
    "F3 0F 10 45 30 F3 0F 11 87 B0 02 00 00";
std::atomic<uint64_t> g_camoff_writes{0};
std::atomic<uint32_t> g_camoff_state{0};   // 0 unresolved, 1 resolved, 2 disabled by engine

// Build 16a diagnostics, written by the camera hook, read by the 1 Hz drain.
// Plain floats: a torn diagnostic read is harmless.
float g_head_pos[3]    = {};
float g_head_dz        = 0.0f;   // head height above the character origin
bool  g_head_valid     = false;

// Build 91: viewpoint placement state. Engine thread only, same as g_base and
// g_head_pos above, and written under the same SEH guard.
//
// The last non-degenerate HORIZONTAL camera forward. When the view looks near
// straight up or down the forward row has almost no horizontal part, so the
// forward eye offset would collapse to zero and leave the viewpoint on the bare
// head joint, which is inside the skull. Reusing the last good one keeps the
// eyes clear of it.
float g_lastfwd[2]     = {};
bool  g_lastfwd_ok     = false;

// Build 93: THE HEAD ROTATION THE EYE TAG ADVERTISED, latched per frame.
//
// The tag is pushed once, at the frame boundary, carrying the Q from the FIRST
// call of that frame. But headpose::read(H, Q) was re-read on EVERY call, and
// since build 89 there are about 3 calls per frame from two distinct call sites
// (measured 2026-08-15: 0x0138D49C at ~1.95/frame and 0x0D7B8DAC at ~0.97).
// So if the render thread published a new orientation between call 1 and call
// 3, the camera was composed with a NEWER rotation than the tag advertised, and
// the compositor then timewarped from a pose the content was never rendered
// with. That is precisely the error build 13a exists to remove, reintroduced by
// the higher call rate at the selector site.
//
// Latching here is what "frame-idempotent" was always supposed to mean.
float g_frameH[9]      = {};
// BUILD 270: the XR quaternion latched with g_frameH (the one the frame's tag carries), for the gtrec W rows only.
// Written only while gtrec is recording; nothing else reads it.
float g_frameQ[4]      = {0.0f, 0.0f, 0.0f, 1.0f};

// The EMA position filter, held in ORIGIN-RELATIVE space. See HeadPose.h.
float g_flt[3]         = {};
bool  g_flt_valid      = false;
int64_t g_flt_qpc      = 0;
int64_t g_qpc_freq     = 0;

// Diagnostics for the placement, printed on the fp: line.
std::atomic<uint64_t> g_fp_clamped{0};   // anchor clamp fired
std::atomic<uint64_t> g_fp_snaps{0};     // filter reset to raw (jump or NaN)
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

// BUILD 261: THE VEHICLE CAMERA. In a vehicle the game's own camera owns the view direction: it follows the car and
// swings to the rear window when the car reverses (build 260 log, 2026-10-07: during a six second reverse the pose
// root's forward axis held +160..+170 degrees while the engine camera sat 171..179 degrees round from it). So while a
// vehicle is detected the head is composed onto the BODY's heading instead: out = H * B with B the level basis of the
// pose root's forward axis, which the camera swing cannot reach. 0.7.0 composed the head onto the engine basis in
// vehicles and worked; this is the same compose with a base that does not swing.
//
// Detection (VEHICLE-2026-09-08 section 6.2, measured on the 260 drive): the head bone sits 0.55..0.85 m above the
// character origin (seated; standing reads 1.5..1.66, crouch 0.92..0.98), the first person anchor is live, the pose
// root reads and its forward axis is horizontal, and, to ENTER only, the engine camera is more than 3 m from the
// character (low-passed: the raw read dips to 0.8 m about one sample in six). Frame counted hysteresis, so a crouch
// that passes through the dz band cannot flip it. Every failure leaves it off, which is exactly build 260 behaviour.
std::atomic<int>      g_veh_cam{1};        // cfg veh_camera (default ON; 0 is the rollback and takes effect at once)
std::atomic<int>      g_veh_on{0};         // the vehicle compose is active
std::atomic<uint32_t> g_veh_edges{0};      // transitions of g_veh_on
std::atomic<uint32_t> g_veh_rootfail{0};   // frames the pose root could not be read while seated
std::atomic<int>      g_roll_level{0};     // head_roll_fix mirrored from VRMirror: the picture carries no head roll
float g_veh_B[9]       = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the level body basis, rows right / forward / up
float g_veh_a          = 0.0f;             // low-passed A
int   g_veh_enter_n    = 0;
int   g_veh_exit_n     = 0;
// The head's yaw (game basis, atan2(x, y) of the forward row of H) that counts as "straight along the car". Taken
// when the vehicle compose switches on, so the view starts on the car's heading wherever the player happens to be
// facing (there is no controller recenter, and the runtime's own recenter is absorbed and moves nothing); retaken
// as 0 when the head's yaw reference is recaptured on purpose (Home / Space / F2).
float g_veh_yaw0       = 0.0f;
unsigned g_veh_epoch   = 0;
// BUILD 262: the body heading is SMOOTHED before it is used. The hard-turn jitter of build 261 is the raw pose root being
// consumed at the display rate while it only changes at the game's simulation rate, so at a high yaw rate a stale frame
// is followed by a catch-up. An alpha-beta tracker (position gain alpha, velocity gain beta = alpha^2 / (2 - alpha),
// the critically damped pair) follows a steady turn with no lag and spreads a stale-then-catch-up pair across frames.
// cfg veh_smooth is alpha: 0 = off (the raw heading, build 261), 1 = the raw heading too; default 0.40.
std::atomic<float> g_veh_smooth{0.40f};
// BUILD 263: THE EARLY WRITE (the culling fix). The engine culls for the camera's rotation as it stands when the visibility
// work runs, which is BEFORE the selector composes the frame, so a picture turned away from the engine camera (looking sideways,
// or forward while the engine camera looks out of the rear window in a reverse) loses the world the engine did not cull for.
// The last composed rotation is kept here; the graph-frame builder entry (Cam2.cpp), which runs before the visibility work,
// writes it into both cameras' rotation rows. Counters say whether the engine left it alone until the selector.
// BUILD 264: the compose moves to the graph-frame builder entry. Build 263's eye recordings showed the two eyes disagreeing by
// a fraction of a degree on 12 to 15 percent of the frames in which the camera was turning (2 percent when still): camera 0 and
// camera 2 each got their rotation from a write made at a different moment, one of them sometimes a frame old (the second
// camera's selector call comes first on about 5 percent of frames), and before true stereo there was only ever ONE camera per
// frame, which is why the same compose was smooth then. Now the frame's rotation is composed ONCE, at the builder entry (the
// latest head, the body heading read now, the tracker stepped once), written to both cameras there, and the selector calls of
// that frame take that exact rotation, that exact head and that exact tag orientation instead of composing again.
float    g_vb_H[9]     = {1, 0, 0, 0, 1, 0, 0, 0, 1};     // the head matrix the graph-entry compose used
float    g_vb_Q[4]     = {0, 0, 0, 1};                     // the same head as an XR quaternion, for the eye tag
float    g_vb_out[9]   = {1, 0, 0, 0, 1, 0, 0, 0, 1};     // the composed camera rotation
std::atomic<int>      g_vb_fresh{0};       // a graph-entry compose is waiting for its frame's first selector call
bool                  g_vb_frame_used = false;   // the current frame is using it
std::atomic<uint32_t> g_vb_begins{0};      // graph-entry composes made
std::atomic<uint32_t> g_vb_used_n{0};      // frames whose selector took the graph-entry compose
std::atomic<uint32_t> g_vb_missed_n{0};    // vehicle frames whose selector found none and composed itself

// BUILD 267: THE COMPOSE INSIDE THE ENGINE'S OWN CAMERA RECOMPUTE (MvpHook.h). A BUILD is one pass of recompute entries,
// camera 0 and camera 2 once each, counted by a sequence number that is NOT the Present counter: builds and Presents do not
// map one to one (build 266 recording: 13 percent of builds were a second one under the same Present count, and the stalls
// follow them). While a vehicle is detected and the hook is live, the FIRST recompute entry of each build takes one head sample
// (matrix and XR quaternion together), composes the rotation ONCE, writes both cameras BEFORE the engine's body reads the pose,
// and records exactly what it used; the layer is then labelled with that record, not with a lag estimate. Everything is gated
// on veh_on and on the hook being live: on foot the recorder only counts, and the selector path is exactly build 251's.
std::atomic<int>      g_mvp_observe{1};         // cfg mvp_observe: count and compare, never write
std::atomic<int>      g_mvp_drive{1};           // cfg mvp_drive: 1 = compose in the hook while veh_on (0 = observer only)
std::atomic<uint32_t> g_mvp_calls[3];           // recompute entries: [0] camera 0, [1] camera 2, [2] any other camera
std::atomic<uint32_t> g_mvp_per_build[3];       // builds that saw [0] one camera only, [1] cam0 and cam2, [2] unused
std::atomic<uint32_t> g_mvp_cam2_first{0};      // builds whose first entry was camera 2
std::atomic<uint32_t> g_mvp_same_tid{0}, g_mvp_diff_tid{0};   // entry thread equals / differs from the thread that last ran the selector
std::atomic<uint32_t> g_mvp_rows_eq{0}, g_mvp_rows_ne{0};     // veh only: pose rows at entry equal the composed rotation of this build
std::atomic<uint32_t> g_mvp_label_hit{0}, g_mvp_label_miss{0};   // the layer label found / did not find its build
uint64_t g_build_seq = 0;                       // increments at the first entry of each build (frame-build thread only)
uint64_t g_last_build = ~0ull;                  // the build write_pose_head last composed for
uint8_t  g_mvp_seen = 0;                        // bit 0 camera 0, bit 1 camera 2, this build
uint32_t g_mvp_tid_sel = 0;                     // thread id of the last selector call
uint64_t g_last_tag_f = ~0ull;                  // the Present count the last eye tag was pushed for (the tag FIFO is per Present)
struct BuildRec { uint64_t seq, frame; float H[9], Q[4], out[9]; };
BuildRec g_brec[16];
std::atomic<uint64_t> g_brec_n{0};
inline bool mvp_drives() {   // the hook is live, driving is on, a vehicle is detected, and both stereo cameras exist
    return g_veh_on.load(std::memory_order_relaxed) != 0 && g_mvp_drive.load(std::memory_order_relaxed) != 0 && mvp::live() &&
           ::grwxr::cam2::stereo_live();
}
// BUILD 269: THE ENGINE FRAME. IDA 9.3 on TU25 (2026-10-07): the engine writes the camera pose rows ONCE per frame (0x0BFCADE0 via the
// dispatcher 0x0C0022C0), then recomputes each camera TWICE: pass A in the PreUpdateGraphicComponents task (the call at RVA 0x0D6454A5, return
// 0x0D6454AA) and pass B in MainVisibilitySetup (the call at 0x0DA25349, return 0x0DA2534E, which snapshots the pose rows and the frustum
// planes for culling right after); sun cascade fit reads the rows between them. Other recompute sites (the weapon spread code on the sim thread,
// which saves the rows, writes an aim pose, recomputes and restores; TAA; cube and terrain cameras) must NEVER be written: a head rotation there
// would reach the spread code. So the hook acts only at A and B, identified by the return address of the recompute frame, which sits at
// stack_args + 0x108 (the on_calc_mvp entry rsp is stack_args - 8; the recompute frame is 5 pushes + 0xE0 locals + its return slot above it).
// The frame is composed ONCE, at the first pass A entry, and every later A or B entry of either camera re-asserts exactly that pose.
constexpr uint32_t kSiteA = 0x0D6454AAu, kSiteB = 0x0DA2534Eu;
uint64_t g_img_base = 0;
uint8_t  g_mvp_seenA = 0;                       // bit 0 camera 0, bit 1 camera 2: pass A entries of this engine frame
float    g_vf_rows[12];                         // the frame's composed pose for camera 0: rotation rows 0..2 (3 floats each), then the position
bool     g_vf_valid = false;
std::atomic<uint32_t> g_mvp_site[3];            // recompute entries by site: [0] A, [1] B, [2] any other (not touched)
static void veh_write_cam0_rows(uint64_t cam) {
    __try {
        float* m = (float*)(cam + 0);
        for (int r = 0; r < 3; ++r) memcpy(m + r * 4, g_vf_rows + r * 3, 12);
        memcpy(m + 12, g_vf_rows + 9, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
std::atomic<uint32_t> g_pre_writes{0};     // early writes made (Cam2.cpp)
std::atomic<uint32_t> g_pre_kept{0};       // at the selector the camera's rotation was still the early write
std::atomic<uint32_t> g_pre_lost{0};       // at the selector the engine had put its own rotation back
std::atomic<int>      g_pre_pending{0};    // an early write was made since the last selector frame
float g_pre_wrote[9]   = {};
float    g_vf_y        = 0.0f;     // tracked heading, radians
float    g_vf_v        = 0.0f;     // tracked rate, radians per built frame
uint64_t g_vf_f        = 0;        // the built frame it was last updated on
bool     g_vf_init     = false;
// The record: every built frame of the vehicle compose, raw and tracked, so the smoothing can be judged offline.
struct VehRow { uint64_t frame; float raw, filt, cam, head; };
constexpr uint32_t kVehRing = 4096;
VehRow   g_vring[kVehRing];
std::atomic<uint32_t> g_vring_n{0};
static float wrap_pi_f(float a) {
    while (a >  3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}
constexpr int kVehEnterFrames = 36;        // about half a second of a steady seated reading
constexpr int kVehExitFrames  = 12;        // about a sixth of a second of not-seated

// The pose root's world axes (Pose+0x10, xyzw): rows X, Y, Z of the owner entity's body orientation. Guarded reads.
static bool read_pose_root_axes(uint64_t obj, float ax[9]) {
    uint64_t pose = 0;
    if (!obj || !read_block(obj + 0x238, &pose, sizeof(pose)) || pose < 0x10000 || (pose & 7)) return false;
    float rq[4];
    if (!read_block(pose + 0x10, rq, sizeof(rq))) return false;
    for (int i = 0; i < 4; ++i) if (!isfinite(rq[i])) return false;
    const float n = sqrtf(rq[0] * rq[0] + rq[1] * rq[1] + rq[2] * rq[2] + rq[3] * rq[3]);
    if (!(n > 1e-6f)) return false;
    const float qx = rq[0] / n, qy = rq[1] / n, qz = rq[2] / n, qw = rq[3] / n;
    const float basis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int a = 0; a < 3; ++a) {
        const float* v = basis[a];
        const float cx = qy * v[2] - qz * v[1];
        const float cy = qz * v[0] - qx * v[2];
        const float cz = qx * v[1] - qy * v[0];
        const float dx = qy * cz - qz * cy;
        const float dy = qz * cx - qx * cz;
        const float dz = qx * cy - qy * cx;
        ax[a * 3 + 0] = v[0] + 2.0f * (qw * cx + dx);
        ax[a * 3 + 1] = v[1] + 2.0f * (qw * cy + dy);
        ax[a * 3 + 2] = v[2] + 2.0f * (qw * cz + dz);
    }
    return isfinite(ax[0]) && isfinite(ax[4]) && isfinite(ax[8]);
}

// The body's heading from the pose root's axes: raw, then tracked (g_veh_smooth), then the level basis g_veh_B. Hr is the head
// matrix whose yaw goes into the record.
static void veh_heading_update(const float ax[9], const float* Hr) {
    const float raw = atan2f(ax[3], ax[4]);
    float yaw = raw;
    const uint64_t fr = d3d11::frame_count();
    const float al = g_veh_smooth.load(std::memory_order_relaxed);
    if (al > 0.0f && al < 0.999f) {
        if (g_vf_init && fr == g_vf_f) {
            // a second step inside one built frame (a second graph entry before the Present): keep what the first one made
        } else if (!g_vf_init || fr < g_vf_f || fr - g_vf_f > 8) {
            g_vf_y = raw; g_vf_v = 0.0f; g_vf_init = true;
        } else {
            float n = (float)(fr - g_vf_f);
            if (n < 1.0f) n = 1.0f;
            const float pred = g_vf_y + g_vf_v * n;
            const float r = wrap_pi_f(raw - pred);
            if (fabsf(r) > 0.6f) {            // not a physical turn rate: a teleport or a respawn, follow it at once
                g_vf_y = raw; g_vf_v = 0.0f;
            } else {
                const float be = al * al / (2.0f - al);
                g_vf_y = wrap_pi_f(pred + al * r);
                g_vf_v += (be / n) * r;
            }
        }
        g_vf_f = fr;
        yaw = g_vf_y;
    } else {
        g_vf_init = false;
    }
    {
        const uint32_t k = g_vring_n.load(std::memory_order_relaxed);
        VehRow& w = g_vring[k & (kVehRing - 1)];
        w.frame = fr; w.raw = raw; w.filt = yaw;
        w.cam  = atan2f(g_base[3], g_base[4]);
        w.head = wrap_pi_f(atan2f(Hr[3], Hr[4]) - g_veh_yaw0);
        g_vring_n.store(k + 1, std::memory_order_release);
    }
    // The level basis of the body's forward heading: row 0 right, row 1 forward, row 2 up (world z up).
    const float cy = cosf(yaw), sy = sinf(yaw);
    g_veh_B[0] = cy;   g_veh_B[1] = -sy;  g_veh_B[2] = 0.0f;
    g_veh_B[3] = sy;   g_veh_B[4] = cy;   g_veh_B[5] = 0.0f;
    g_veh_B[6] = 0.0f; g_veh_B[7] = 0.0f; g_veh_B[8] = 1.0f;
}

// out = H * B for the vehicle compose: the head's yaw re-referenced to the moment the compose switched on (g_veh_yaw0), its roll
// removed when head_roll_fix is on (the layer pose carries none), then turned onto the body's level basis B.
static void veh_compose_rot(const float* H, const float* B, float out[9]) {
    float FHr[9], FHl[9];
    const float* FH = H;
    {
        const float c0 = cosf(g_veh_yaw0), s0 = sinf(g_veh_yaw0);
        for (int r = 0; r < 3; ++r) {
            FHr[r * 3 + 0] = FH[r * 3 + 0] * c0 - FH[r * 3 + 1] * s0;
            FHr[r * 3 + 1] = FH[r * 3 + 0] * s0 + FH[r * 3 + 1] * c0;
            FHr[r * 3 + 2] = FH[r * 3 + 2];
        }
        FH = FHr;
    }
    if (g_roll_level.load(std::memory_order_relaxed) != 0) {
        float sp = FH[5];
        if (sp >  0.9999f) sp =  0.9999f;
        if (sp < -0.9999f) sp = -0.9999f;
        const float Y = atan2f(FH[3], FH[4]);
        const float P = asinf(sp);
        const float cyaw = cosf(Y), syaw = sinf(Y);
        const float cpit = cosf(P), spit = sinf(P);
        FHl[0] =  cyaw;        FHl[1] = -syaw;        FHl[2] = 0.0f;
        FHl[3] =  syaw * cpit; FHl[4] =  cyaw * cpit; FHl[5] = spit;
        FHl[6] = -syaw * spit; FHl[7] = -cyaw * spit; FHl[8] = cpit;
        FH = FHl;
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = FH[r * 3 + 0] * B[0 + c] + FH[r * 3 + 1] * B[3 + c] + FH[r * 3 + 2] * B[6 + c];
}

// Once per built frame, from the first selector call. Reads the values the previous frame left (one frame stale).
static void veh_step() {
    if (g_veh_cam.load(std::memory_order_relaxed) == 0) {
        if (g_veh_on.exchange(0, std::memory_order_relaxed) != 0) g_veh_edges.fetch_add(1, std::memory_order_relaxed);
        g_veh_enter_n = g_veh_exit_n = 0;
        return;
    }
    const bool  anchored = g_fp_anchored.load(std::memory_order_relaxed);
    const float dz       = g_head_dz;
    const float dist     = g_veh_dist.load(std::memory_order_relaxed);
    g_veh_a = g_veh_a * 0.95f + ((dist > 3.0f) ? 0.05f : 0.0f);
    const bool seated = anchored && dz >= 0.55f && dz <= 0.85f;
    bool root_ok = false;
    float ax[9];
    if (seated) {
        const uint64_t obj = (uint64_t)headpose::player_obj();
        root_ok = obj && read_pose_root_axes(obj, ax);
        if (root_ok) {
            // The forward axis must be horizontal enough to carry a heading.
            const float hl = sqrtf(ax[3] * ax[3] + ax[4] * ax[4]);
            if (!(hl > 0.7f)) root_ok = false;
        }
        if (!root_ok) g_veh_rootfail.fetch_add(1, std::memory_order_relaxed);
    }
    const bool stay  = seated && root_ok;
    const bool enter = stay && g_veh_a > 0.5f;
    const bool on    = g_veh_on.load(std::memory_order_relaxed) != 0;
    if (!on) {
        g_veh_enter_n = enter ? g_veh_enter_n + 1 : 0;
        g_veh_exit_n = 0;
        if (g_veh_enter_n >= kVehEnterFrames) {
            g_veh_on.store(1, std::memory_order_relaxed);
            g_veh_edges.fetch_add(1, std::memory_order_relaxed);
            g_veh_enter_n = 0;
            g_veh_yaw0 = atan2f(g_frameH[3], g_frameH[4]);   // the previous frame's latched head, one frame stale
            g_veh_epoch = headpose::ref_epoch();
        }
    } else {
        g_veh_exit_n = stay ? 0 : g_veh_exit_n + 1;
        g_veh_enter_n = 0;
        if (g_veh_exit_n >= kVehExitFrames) {
            g_veh_on.store(0, std::memory_order_relaxed);
            g_veh_edges.fetch_add(1, std::memory_order_relaxed);
            g_veh_exit_n = 0;
        }
    }
    if (g_veh_on.load(std::memory_order_relaxed) != 0) {
        const unsigned ep = headpose::ref_epoch();
        if (ep != g_veh_epoch) { g_veh_epoch = ep; g_veh_yaw0 = 0.0f; }   // recentered on purpose: forward is where he faces now
    }
    // BUILD 264: when the graph-entry compose already stepped the tracker for this frame, do not step it twice.
    if (g_veh_on.load(std::memory_order_relaxed) != 0 && root_ok && g_vb_fresh.load(std::memory_order_acquire) == 0)
        veh_heading_update(ax, g_frameH);
}

bool write_pose_head(uint64_t cam, const float* H, const float* Q) {
    float* m = (float*)(cam + kOffPose);
    // frame_count() is bumped in the Present hook, so it is stable across the
    // frame build; the calls/frame ratio in the drain log verifies that.
    const uint64_t f = d3d11::frame_count();
    // Build 100: captured before the block below clears it, so the latch at the
    // bottom of this function fires on the FIRST call of a frame only. The
    // selector is entered about three times per frame and the EMA advances on
    // each, so latching on every call would put a millimetre of per-call wobble
    // back into the value this exists to make frame-stable.
    // BUILD 267: while the recompute hook drives the vehicle frame, a BUILD (not a Present count) is the unit.
    const bool by_build = mvp_drives();
    const bool first_of_frame = by_build ? (g_build_seq != g_last_build) : (f != g_last_frame);
    // BUILD 247: TRUE STEREO. This call is for camera 0, the player camera, which is in viewport mode 4 while the
    // second-camera experiment runs (so the old `mode == 0` identification no longer holds for it, and the callers
    // key it on the POINTER instead). With cam2_stereo on, camera 0 is the LEFT eye and camera 2 the RIGHT eye:
    // this ONE call composes the frame once and writes BOTH cameras, camera 0 at +half and camera 2 at -half along the
    // composed right axis. No eye toggle, one tag per built frame whose eye field says "stereo" (2). With cam2_stereo
    // off both values are false and everything below is the build 246 code.
    const bool stereo = ::grwxr::cam2::stereo_live() && cam != 0 && cam == ::grwxr::cam2::first_camera();
    // BUILD 270: what this call did that a gtrec W row carries (set below, read only by the W rows; log only).
    bool     w_polluted  = false;
    t_head_used_ok = false;   // BUILD 282: set below only when this call composes from the Head bone
    int      w_tag_eye   = -1;
    unsigned w_tag_flags = 0;
    __try {
        if (first_of_frame) {
            // BUILD 114: THE CAPTURE POLLUTION GUARD. Session 37 NEXT item 1,
            // specced then and never built. This is the jitter fix, and it is
            // independent of the pin.
            //
            // The mod writes the camera, then next frame re-reads the SAME
            // field to recover the engine's base. About half the time it reads
            // back its own write because the engine has not restored yet. The
            // 2026-08-19 20:12 run shows the result as discrete levels rather
            // than noise: vs-write sat at 0.0000, 1.5470, 1.9397, 2.7867 and
            // 4.4721. Those are 0x, 1x and 2x the push lever, i.e. the error
            // COMPOUNDS when a polluted capture is itself written from.
            //
            // The tester sees exactly that, and described it before this was
            // measured: three image positions per eye, "once too far left then
            // left, and between that a correct image", cycling. Three levels,
            // mirrored per eye because the eye offset rides on the same base.
            // It happens standing still, which fits: the race is frame to
            // frame, not motion driven.
            //
            // The guard: if the candidate capture lands within epsilon of our
            // own last write, the engine has not restored, so keep the
            // previous frame's base instead of laundering our own output back
            // in as if it were the engine's.
            //
            // Only while the push is active. Session 37 measured healthy
            // vs-write at 0.027 with FP off, which is not separable from a
            // polluted read, so guarding there would misfire constantly.
            //
            // SAFETY VALVE, and it is deliberate: at most two consecutive
            // rescues, then the capture is accepted whatever it looks like. A
            // guard that can hold the base forever would freeze the camera if
            // the epsilon is ever wrong, which is a worse failure than the one
            // being fixed. Two builds regressed tonight; this one cannot get
            // stuck.
            float cand_pos[3];
            for (int c = 0; c < 3; ++c) cand_pos[c] = m[12 + c];
            // BUILD 263: did the early write (Cam2.cpp) survive until now? Compared on the rows BEFORE any capture below.
            if (g_pre_pending.exchange(0, std::memory_order_relaxed) != 0) {
                float dmax = 0.0f;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) {
                        const float d = fabsf(m[r * 4 + c] - g_pre_wrote[r * 3 + c]);
                        if (d > dmax) dmax = d;
                    }
                if (dmax < 1e-3f) g_pre_kept.fetch_add(1, std::memory_order_relaxed);
                else              g_pre_lost.fetch_add(1, std::memory_order_relaxed);
            }

            bool polluted = false;
            if (g_diag_have_written && g_diag_have_prev &&
                headpose::fp_enabled() && g_cap_rescue_run < 2) {
                polluted = dist3(cand_pos, g_diag_written) < 0.10f;
            }
            w_polluted = polluted;   // BUILD 270

            if (polluted) {
                ++g_cap_rescue_run;
                ++g_cap_rescues;
                // g_base and g_base_pos deliberately keep last frame's values.
            } else {
                g_cap_rescue_run = 0;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        g_base[r * 3 + c] = m[r * 4 + c];
                for (int c = 0; c < 3; ++c)
                    g_base_pos[c] = cand_pos[c];
            }
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
            g_last_build = g_build_seq;
            veh_step();   // BUILD 261: the vehicle detector and its body basis, once per built frame
            {   // BUILD 264: does this frame have a graph-entry compose waiting? It is consumed here, once, whether or not it is used.
                const int fresh = g_vb_fresh.exchange(0, std::memory_order_acq_rel);
                const bool vnow = g_veh_on.load(std::memory_order_relaxed) != 0;
                g_vb_frame_used = vnow && fresh == 1;
                if (vnow) {
                    if (g_vb_frame_used) g_vb_used_n.fetch_add(1, std::memory_order_relaxed);
                    else                 g_vb_missed_n.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (!stereo) g_eye_toggle ^= 1;   // BUILD 247: both eyes exist every frame in stereo mode, nothing alternates
            // Build 13a: the tag carries the XR-space orientation this frame
            // is being composed with, so the present side can submit the true
            // render pose (see HeadPose.h).
            // BUILD 118-I: the tag also carries the engine camera base this
            // frame's write is built from, plus the guard state, so the
            // present side can measure per-eye base disagreement (fault C
            // candidate C1) without opening a new channel. g_base_pos, NOT
            // g_vr_cam_pos: g_vr_cam_pos is stamped at the bottom of
            // write_pose_head, which runs AFTER this block, so reading it
            // here would silently hand over the PREVIOUS frame's value.
            unsigned char tflags = 0;
            if (polluted)               tflags |= 0x01u;
            if (headpose::fp_enabled()) tflags |= 0x04u;
            // BUILD 247: eye 2 = "stereo frame", the one tag the present pops for a frame in which BOTH cameras
            // were written; flag 0x08 = camera 2 is not trusted yet for this frame (the warm-up after creation or
            // after a viewport drift), so the present submits the left half to both eyes instead.
            // BUILD 267: the tag FIFO is popped once per PRESENT, so it is pushed once per Present count even when
            // a build is the unit (a second build under the same count must not push a second tag).
            if (f != g_last_tag_f) {
                g_last_tag_f = f;
                if (stereo) {
                    if (!::grwxr::cam2::stereo_warm()) tflags |= 0x08u;
                    headpose::push_eye_tag(2, g_vb_frame_used ? g_vb_Q : Q, g_base_pos, tflags);
                } else {
                    headpose::push_eye_tag(g_eye_toggle, g_vb_frame_used ? g_vb_Q : Q, g_base_pos, tflags);
                }
                w_tag_eye   = stereo ? 2 : g_eye_toggle;   // BUILD 270
                w_tag_flags = tflags;
            }
            // Build 93: latch the rotation that this frame's tag advertises, so
            // every later call of the same frame composes with exactly it. See
            // the comment at g_frameH.
            memcpy(g_frameH, g_vb_frame_used ? g_vb_H : H, sizeof(g_frameH));   // BUILD 264: the head the graph-entry compose used
            if (::grwxr::gtrec::on()) memcpy(g_frameQ, g_vb_frame_used ? g_vb_Q : Q, sizeof(g_frameQ));   // BUILD 270
            g_head_frames.fetch_add(1, std::memory_order_relaxed);

            // BUILD 238: THE MONO-SCOPE LATCH, UPDATED ONCE PER BUILT FRAME.
            // It lives here, not at the gate below, because the gate runs on
            // EVERY call of this hook and the hook is called several times per
            // frame: counting there would count calls, not frames, and the
            // hold would expire in a fraction of the intended time.
            {
                const float fovn = headpose::read_fov(0.7853982f);
                const float thr  = headpose::mono_scope_fov();
                uint32_t need = g_scope_hold.load(std::memory_order_relaxed);
                if (need < 1u)  need = 1u;    // 1 = the pre-238 one-frame behaviour
                if (need > 60u) need = 60u;
                const bool below = (thr > 0.0f) && (fovn < thr);
                if (below) { if (g_scope_below < need) ++g_scope_below; g_scope_above = 0; }
                else       { if (g_scope_above < need) ++g_scope_above; g_scope_below = 0; }
                const bool was = g_scope_engaged.load(std::memory_order_relaxed) != 0;
                if (!was && g_scope_below >= need) {
                    g_scope_engaged.store(1, std::memory_order_relaxed);
                    g_scope_enters.fetch_add(1, std::memory_order_relaxed);
                } else if (was && g_scope_above >= need) {
                    g_scope_engaged.store(0, std::memory_order_relaxed);
                    g_scope_exits.fetch_add(1, std::memory_order_relaxed);
                }
            }
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
        // BUILD 238: the live read is replaced by the stability-gated latch
        // above. Identical behaviour at scope_hold_frames = 1; at the default 6
        // an ADS transition sweeping through the gate no longer trips it, which
        // is what made raising mono_scope_fov unusable before.
        if (g_scope_engaged.load(std::memory_order_relaxed)) {
            // BUILD 247: camera 0 is left exactly as the engine has it (the flat scope view), so camera 2 takes
            // the same pose, MONO: no separation, both eyes see the one scoped picture, as the alternate path does.
            if (stereo) {
                float m2[16];
                memcpy(m2, m, sizeof(m2));
                ::grwxr::cam2::stereo_publish_pose(f, m2);
            }
            if (::grwxr::gtrec::on()) {   // BUILD 270: gtrec W row, log only. Camera 0 is NOT written here (cam0 = 0, its
                                          // engine-own bytes are still read into m0); camera 2 got its pose, mono.
                ::grwxr::gtrec::WRow gw;
                memset(&gw, 0, sizeof(gw));
                gw.kind      = ::grwxr::gtrec::kWScopeMono;
                gw.cam0      = 0;
                gw.cam2      = stereo ? ::grwxr::cam2::second_camera() : 0;
                gw.build_seq = (uint32_t)g_build_seq;
                gw.fl        = (by_build ? 1u : 0u) | (first_of_frame ? 2u : 0u) | (stereo ? 4u : 0u) |
                               (g_vb_frame_used ? 32u : 0u) | (w_polluted ? 64u : 0u) | (w_tag_eye >= 0 ? 1024u : 0u);
                gw.tag_eye   = w_tag_eye;
                gw.tag_flags = w_tag_flags;
                gw.yaw0      = g_veh_yaw0;
                memcpy(gw.Q, g_frameQ, sizeof(gw.Q));
                memcpy(gw.H, g_frameH, sizeof(gw.H));
                ::grwxr::gtrec::copy64(gw.m0, cam);
                if (gw.cam2) ::grwxr::gtrec::copy64(gw.m2, gw.cam2);
                ::grwxr::gtrec::w_emit(gw);
            }
            return true;
        }

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
        // Build 90: ROTATION COMPOSE IS OPTIONAL, and off by default at the
        // selector site.
        //
        // The 2026-08-15 run put first person on screen for the first time
        // since the update and the tester's verdict was that the camera
        // fights: it answered to both controllers and to the headset at once.
        // The cause is that composing here puts a SECOND authority on a
        // channel that already has one. Head look already reaches the view
        // through the SetYaw/SetPitch absorb-and-inject path, which is why
        // build 88 feels correct with no camera write at all, and the barrel
        // aim steers that same channel by design.
        //
        // Corroboration from an independent implementation of the same feature
        // on the same engine (the flat first-person mod, FpCamera.cpp, which
        // worked on the previous binary): "Pose is READ AND WRITTEN (fourth
        // row only)". It moves the viewpoint and never touches the basis,
        // because on flat the mouse owns rotation. Here the headset owns it,
        // through the setters, and the same reasoning applies.
        //
        // With rotation off, this function does exactly two things, both of
        // them position: put the viewpoint at the head bone, and offset it per
        // eye. Neither can argue with where you are looking.
        // BUILD 261: in a vehicle the head is composed onto the body heading (see veh_step), whatever cam_pose_rot says.
        const bool veh = g_veh_on.load(std::memory_order_relaxed) != 0;
        const bool rot = headpose::cam_pose_rot() || veh;

        const float* B = g_base;
        float rebuilt[9];
        float cy = 0, cp = 0;
        // 2026-09-01: the horizon lock runs this same rebuild with nothing to
        // subtract. Roll is removed by construction (rebuilt[2] = 0), yaw and
        // pitch are kept, and H still carries your head's roll on top.
        const bool have_aim = headpose::aim_cum(&cy, &cp);
        const bool level    = g_cam_level.load(std::memory_order_relaxed) != 0;
        if (rot && !veh && (have_aim || level)) {
            if (!have_aim) { cy = 0.0f; cp = 0.0f; }
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
        if (rot) {
            // Build 93: g_frameH, the LATCHED rotation, not the argument H. The
            // argument is re-read on every call of the frame and can be newer
            // than the eye tag says. See the comment at g_frameH.
            const float* FH = g_frameH;
            if (veh) {
                // BUILD 264: the graph-entry compose when this frame has one (identical for every call of the frame), else composed here.
                if (g_vb_frame_used) memcpy(out, g_vb_out, sizeof(out));
                else                 veh_compose_rot(FH, g_veh_B, out);
            } else {
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        out[r * 3 + c] = FH[r * 3 + 0] * B[0 + c]
                                       + FH[r * 3 + 1] * B[3 + c]
                                       + FH[r * 3 + 2] * B[6 + c];
                    }
                }
            }
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m[r * 4 + c] = out[r * 3 + c];
            if (by_build && veh && first_of_frame) {
                // BUILD 267: exactly what this build was composed from, for the layer label (veh_build_label).
                BuildRec& br = g_brec[g_brec_n.load(std::memory_order_relaxed) & 15];
                br.seq = g_build_seq;
                br.frame = f;
                memcpy(br.H, g_frameH, sizeof(br.H));
                memcpy(br.Q, g_vb_frame_used ? g_vb_Q : Q, sizeof(br.Q));
                memcpy(br.out, out, sizeof(br.out));
                g_brec_n.fetch_add(1, std::memory_order_release);
            }
        } else {
            // Position-only: the engine keeps its own basis, so the eye offset
            // rides the ENGINE's right axis rather than a composed one. Row 0
            // of g_base is that axis, already unit length. Nothing is written
            // to the rotation rows at all.
            memcpy(out, g_base, sizeof(out));
        }

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
        // BUILD 125: THE EYE-SIGN A/B. The one thing no log line can settle.
        //
        // The build 93 comment above this function states that positive
        // ipd_scale displaces the LEFT eye to the RIGHT, "geometrically the
        // swapped direction", and that the stored sign is [UNKNOWN] rather
        // than wrong because build 10m measured the COMPOSITE of sign and
        // frame-pairing parity without being able to separate them. Builds 89
        // and 93 both moved things that can change that parity.
        //
        // Why this is worth a build: REVERSED STEREO FUSES FINE AT TINY
        // DISPARITY AND HURTS AT LARGE. That is exactly the reported shape.
        // `[VERIFIED, 479-log sweep]` ipd_scale has only ever been PERSISTED at
        // 0.50 and 0.90, 1.00 has NEVER been run, and 0.05 is a saturated
        // hotkey reached in 4.1 seconds, not an optimum. If the sign is
        // inverted then 0.05 is simply the largest reversed disparity the eyes
        // will tolerate, and every "world is 20x too big" symptom follows from
        // running a 3 mm eye separation to get there.
        //
        // Live and hotkeyable ON PURPOSE. A cfg edit plus a relaunch cannot
        // A/B a fusion judgement: the eyes adapt over seconds and the
        // comparison has to be back to back on the same view. Default +1 is
        // the pre-125 behaviour exactly.
        const float half = 0.5f * headpose::read_ipd(0.063f) * headpose::ipd_scale()
                           * (float)g_ipd_sign.load(std::memory_order_relaxed);
        // BUILD 247: in stereo mode camera 0 takes eye 0's offset exactly as the alternate path gives it (+half,
        // half carrying ipd_scale and the ipd_swap sign) and camera 2 takes eye 1's (-half): the same formula
        // and the same cfg keys, with eye 0 = the left eye = OpenXR view 0 = the left half of the backbuffer.
        const float s    = stereo ? +half : (g_eye_toggle ? -half : +half);
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
                // BUILD 260: the distance is published for the veh: log line before the reach test (log only).
                float d_cam = -1.0f;
                if (isfinite(o[0]) && isfinite(o[1]) && isfinite(o[2])) {
                    d_cam = dist3(o, g_base_pos);
                    g_veh_dist.store(d_cam, std::memory_order_relaxed);
                }
                if (d_cam >= 0.0f && d_cam < 12.0f) {
                    // Build 91: keep the CHARACTER ORIGIN before the head bone
                    // overwrites o below. The clamp measures against it, and
                    // the EMA filters relative to it, which is the whole reason
                    // the filter costs no velocity lag.
                    float orig[3] = {o[0], o[1], o[2]};
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
                            const float h_raw[3] = {h[0], h[1], h[2]};   // BUILD 300: as the skeleton holds it, hinge included
                            lean_unhinge(h);                              // BUILD 300: the anchor is the ANIMATED head; the tracked offset is added below
                            if (g_gpair.load(std::memory_order_relaxed)) {   // BUILD 279, log only
                                t_h3.valid = true;
                                for (int i = 0; i < 3; ++i) { t_h3.h[i] = h[i]; t_h3.orig[i] = orig[i]; }
                                t_h3.pose = 0; t_h3.buf = 0;
                                read_block(obj + 0x238, &t_h3.pose, sizeof(t_h3.pose));
                                if (t_h3.pose) read_block(t_h3.pose + 0x178, &t_h3.buf, sizeof(t_h3.buf));
                            }
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
                                headpose::set_head_dz(hdz, true);   // BUILD 205
                                head_ok    = true;
                                t_head_used[0] = h_raw[0]; t_head_used[1] = h_raw[1]; t_head_used[2] = h_raw[2];   // BUILD 282; 300: the raw read, so drawcam_predict compares like with like
                                t_head_used_ok = true;
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
                    // BUILD 269: in a vehicle the direction the eye offsets ride is the COMPOSED view, not the engine's chase camera
                    // (g_base follows the head on foot through the aim injection, in a car it is the chase camera, so the forward eye
                    // offset pushed the viewpoint sideways by up to fp_fwd when the head turned). On foot fdir is g_base, unchanged.
                    const float* fdir = veh ? out : g_base;
                    const float as   = headpose::fp_anchor_side();
                    const float rise = head_ok ? headpose::fp_head_eye()
                                               : headpose::fp_eye();
                    pos[0] = o[0] + as * fdir[0];
                    pos[1] = o[1] + as * fdir[1];
                    pos[2] = o[2] + as * fdir[2] + rise;  // world up is +Z

                    // Build 91: THE PLACEMENT TRIO. Only meaningful on the head
                    // bone anchor, which is the animated one; the character
                    // origin does not bob and is already where the clamp would
                    // put it. See HeadPose.h for why each of these exists.
                    if (head_ok) {
                        // 1. FORWARD EYE OFFSET, horizontalized. The joint is
                        // at the base of the skull and the eyes are forward of
                        // it. Horizontalized so this frame's pitch cannot
                        // translate this frame's position, which would be a
                        // feedback path between looking down and moving.
                        const float fwd = headpose::fp_fwd();
                        if (fwd > 0.0f) {
                            float dx = fdir[3], dy = fdir[4];
                            const float l2 = dx * dx + dy * dy;
                            bool have = false;
                            if (l2 >= 0.04f) {
                                const float inv = 1.0f / sqrtf(l2);
                                dx *= inv;
                                dy *= inv;
                                g_lastfwd[0] = dx;
                                g_lastfwd[1] = dy;
                                g_lastfwd_ok = true;
                                have = true;
                            } else if (g_lastfwd_ok) {
                                dx   = g_lastfwd[0];
                                dy   = g_lastfwd[1];
                                have = true;
                            }
                            if (have) {
                                pos[0] += dx * fwd;
                                pos[1] += dy * fwd;
                            }
                        }

                        // 2. ANCHOR CLAMP against the character ORIGIN, which
                        // is why orig was kept before the head overwrote o.
                        const float cl = headpose::fp_clamp();
                        if (cl > 0.0f) {
                            const float dx = pos[0] - orig[0];
                            const float dy = pos[1] - orig[1];
                            const float d2 = dx * dx + dy * dy;
                            if (d2 > cl * cl) {
                                const float k = cl / sqrtf(d2);
                                pos[0] = orig[0] + dx * k;
                                pos[1] = orig[1] + dy * k;
                                g_fp_clamped.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        }

                        // 3. EMA FILTER, in origin-relative space. dt from QPC,
                        // which is lock-free user mode and rule 8 clean. The
                        // clamp on dt keeps several calls per frame advancing
                        // the filter consistently rather than in one jump.
                        const float tau_h = headpose::fp_smooth();
                        const float tau_z = headpose::fp_smooth_z();
                        if (tau_h > 0.0f || tau_z > 0.0f) {
                            if (!g_qpc_freq) {
                                LARGE_INTEGER qf;
                                QueryPerformanceFrequency(&qf);
                                g_qpc_freq = qf.QuadPart;
                            }
                            const float rel[3] = {pos[0] - orig[0],
                                                  pos[1] - orig[1],
                                                  pos[2] - orig[2]};
                            LARGE_INTEGER now;
                            QueryPerformanceCounter(&now);
                            float dt = 0.0f;
                            if (g_flt_qpc && g_qpc_freq > 0 &&
                                now.QuadPart > g_flt_qpc) {
                                dt = (float)((double)(now.QuadPart - g_flt_qpc)
                                             * 1000.0 / (double)g_qpc_freq);
                            }
                            g_flt_qpc = now.QuadPart;
                            if (dt > 100.0f) dt = 100.0f;
                            if (g_flt_valid) {
                                const float ex = rel[0] - g_flt[0];
                                const float ey = rel[1] - g_flt[1];
                                const float ez = rel[2] - g_flt[2];
                                // A deviation more than 2 m from the filter's
                                // is not a real pose: the clamp and the
                                // plausibility gates bound real values under a
                                // metre. Snap rather than glide across it.
                                if (ex * ex + ey * ey + ez * ez > 4.0f) {
                                    g_flt[0] = rel[0];
                                    g_flt[1] = rel[1];
                                    g_flt[2] = rel[2];
                                    g_fp_snaps.fetch_add(
                                        1, std::memory_order_relaxed);
                                } else {
                                    const float ah = tau_h > 0.0f
                                        ? dt / (tau_h + dt) : 1.0f;
                                    const float az = tau_z > 0.0f
                                        ? dt / (tau_z + dt) : 1.0f;
                                    g_flt[0] += ah * ex;
                                    g_flt[1] += ah * ey;
                                    g_flt[2] += az * ez;
                                }
                            } else {
                                g_flt[0]    = rel[0];
                                g_flt[1]    = rel[1];
                                g_flt[2]    = rel[2];
                                g_flt_valid = true;
                            }
                            pos[0] = orig[0] + g_flt[0];
                            pos[1] = orig[1] + g_flt[1];
                            pos[2] = orig[2] + g_flt[2];
                        }
                    } else {
                        // Build 94: THE HEAD-READ FAILURE PATH.
                        //
                        // When the head bone does not read back sane, `rise`
                        // switches from fp_head_eye (0.10, joint to eyes) to
                        // fp_eye (0.85, origin to eyes). That is a 0.75 m jump
                        // in the viewpoint, and the placement trio above is
                        // skipped entirely, so nothing filters it. Worse, the
                        // filter state was left VALID, so the return to the head
                        // anchor then glided in from stale deviation rather than
                        // snapping to the live head.
                        //
                        // rejects has been 0 in every measured session, so this
                        // has never fired. But the plausibility gate it depends
                        // on is at half margin in prone (measured 2026-08-15:
                        // the prone head sits 0.38 m out against a 1.0 m reject
                        // radius), so it is reachable.
                        g_flt_valid = false;
                        g_lastfwd_ok = false;
                    }
                    anchored = true;
                }
            }
            if (!anchored) {
                // Build 91: every stand-down invalidates the position filter,
                // so resuming snaps to the live head instead of gliding in from
                // wherever the filter was left.
                g_flt_valid = false;
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
            if (!anchored) headpose::set_head_dz(0.0f, false);   // BUILD 205: no bone, no stance
        }
        // BUILD 204: TRUE 6DOF, the head's translation (item 4.15, CX-023),
        // behind cfg head_pos. The present thread publishes the head's
        // displacement since the reference in head-local metres, exactly the
        // conversion the hands ride (controller minus head, head-local, then
        // the composed basis rows: right, forward, up). Added here, after the
        // first-person placement and its filter and BEFORE the per-eye offset
        // and the latch, so the latched viewpoint the hands are built from
        // carries it and the hands follow the eyes by construction. Zero
        // outside first person and whenever nothing valid is published.
        bool published213 = false;
        if (headpose::fp_enabled() && !veh) {   // BUILD 300: not in a vehicle, whose view is the 271 baseline
            float hl[4];
            bool use_h = false;
            if (headpose::head_offset(hl, &use_h)) {
                // BUILD 207: the head-local step rides the camera basis (the
                // hands' mapping, the pitch cancels); the height model's
                // vertical is a world quantity (the floor and the bone are
                // gravity-aligned) and rides world up, z, not the camera's
                // pitched up row (the internal dig, 3.4).
                float dw[3];
                for (int c = 0; c < 3; ++c)
                    dw[c] = hl[0] * out[0 + c] + hl[1] * out[3 + c] + hl[2] * out[6 + c];
                pos[0] += dw[0];
                pos[1] += dw[1];
                pos[2] += use_h ? hl[3] : dw[2];
                // BUILD 213: the arm is carried by exactly this, so the
                // shoulder-to-hand geometry is what it was before 6DOF.
                g_view_off_world[0] = dw[0];
                g_view_off_world[1] = dw[1];
                g_view_off_world[2] = use_h ? hl[3] : dw[2];
                g_view_off_stamp.store(d3d11::frame_count(), std::memory_order_release);
                published213 = true;
            }
        }
        // BUILD 213: no first person, no offset published, and the arm falls
        // back to build 212 on the NEXT frame rather than two frames later
        // (review 4: the shoulder stayed carried for a sub-33 ms tail).
        if (!published213) g_view_off_stamp.store(0, std::memory_order_release);
        for (int c = 0; c < 3; ++c) {
            m[12 + c] = pos[c] + s * out[c];
            g_diag_written[c] = m[12 + c];
        }
        g_diag_have_written = true;
        // BUILD 247: camera 2, the right eye. Camera 0's 4x4 as it now stands (the rotation rows this call wrote,
        // or the engine's own when cam_pose_rot is off, plus the w column), with the position on the other side of
        // the viewpoint along the same right axis. Written immediately, latched for camera 2's own selector call.
        if (stereo) {
            float m2[16];
            memcpy(m2, m, sizeof(m2));
            for (int c = 0; c < 3; ++c) m2[12 + c] = pos[c] - s * out[c];
            ::grwxr::cam2::stereo_publish_pose(f, m2);
        }
        // BUILD 270: the gtrec W row of this compose (log only): what the compose used (the frame's head sample and
        // matrix, the base, the composed rotation, the viewpoint, the eye offset) and the 64 bytes each camera now holds,
        // read back, so the recompute that reads them can be matched to this row bit for bit. WRow is POD (legal here).
        if (::grwxr::gtrec::on()) {
            ::grwxr::gtrec::WRow gw;
            memset(&gw, 0, sizeof(gw));
            gw.kind      = ::grwxr::gtrec::kWCompose;
            gw.cam0      = cam;
            gw.cam2      = stereo ? ::grwxr::cam2::second_camera() : 0;
            gw.build_seq = (uint32_t)g_build_seq;
            gw.fl        = (by_build ? 1u : 0u) | (first_of_frame ? 2u : 0u) | (stereo ? 4u : 0u) | (veh ? 8u : 0u) |
                           (rot ? 16u : 0u) | (g_vb_frame_used ? 32u : 0u) | (w_polluted ? 64u : 0u) |
                           (g_roll_level.load(std::memory_order_relaxed) ? 128u : 0u) | (have_aim ? 256u : 0u) |
                           (level ? 512u : 0u) | (w_tag_eye >= 0 ? 1024u : 0u) | (headpose::fp_enabled() ? 2048u : 0u) |
                           (g_fp_anchored.load(std::memory_order_relaxed) ? 4096u : 0u) | (g_head_valid ? 8192u : 0u);
            gw.tag_eye   = w_tag_eye;
            gw.tag_flags = w_tag_flags;
            gw.yaw0      = g_veh_yaw0;
            gw.aim_cy    = cy;
            gw.aim_cp    = cp;
            gw.s         = s;
            memcpy(gw.Q, g_frameQ, sizeof(gw.Q));
            memcpy(gw.H, g_frameH, sizeof(gw.H));
            memcpy(gw.out, out, sizeof(gw.out));
            memcpy(gw.B, veh ? (const float*)g_veh_B : B, sizeof(gw.B));
            memcpy(gw.pos, pos, sizeof(gw.pos));
            ::grwxr::gtrec::copy64(gw.m0, cam);
            if (gw.cam2) ::grwxr::gtrec::copy64(gw.m2, gw.cam2);
            ::grwxr::gtrec::w_emit(gw);
        }

        // BUILD 100: LATCH THE COMPOSED CAMERA. See the comment at
        // g_vr_cam_pos for why this and not g_base_pos.
        //
        // `pos` WITHOUT the per-eye offset on purpose: the offset alternates
        // sign every frame, and publishing it would hand every consumer a few
        // centimetres of left-right shudder at frame rate. `out` is the
        // rotation this frame is composed with, which is the engine's own when
        // cam_pose_rot is off and the composed one when it is on, so it is
        // correct either way with no extra branch.
        //
        // Stamped LAST with release ordering, so a reader on the present thread
        // that sees frame f sees the rows belonging to frame f.
        if (first_of_frame) {
            // BUILD 261: in a vehicle the consumers (hands, weapon, ray, PiP) keep the ENGINE basis, as before.
            memcpy(g_vr_cam_rot, veh ? g_base : out, sizeof(g_vr_cam_rot));

            memcpy(g_vr_cam_pos, pos, sizeof(g_vr_cam_pos));
            memcpy(g_vr_cam_head, t_head_used, sizeof(g_vr_cam_head));   // BUILD 282
            g_vr_cam_head_ok = t_head_used_ok && !veh;
            g_vr_cam_stamp.store(f, std::memory_order_release);
            if (t_h3.valid) {   // BUILD 279, log only
                gpair_note(3, t_h3.h, t_h3.orig, pos, t_h3.pose, t_h3.buf);
                t_h3.valid = false;
            }
        }
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
// ---------------------------------------------------------------------------
// BUILD 191 to 194: THE FOV ROOT. Item 4.6, the clouds (CX-014).
//
// The fault, proven by the flat mod's A/B in the game on 2026-08-17: the
// fullscreen override rewrites the fovy ARGUMENT of proj[2] (persp2) and
// nothing else, so the world is drawn at fs_fov (1.92 rad) while every pass
// that derives its own projection from the camera, the sky among them, keeps
// the engine's narrow value. The blit then places the whole frame at 1.92 and
// the sky's angular content is stretched and lags the head. "The clouds
// follow the headset."
//
// What the headset settled on 2026-09-05, in three runs:
//   192: Camera+0x2B0 is NOT what the selector scales into proj[2]'s fovy
//        (the field read 1.92 while proj[2] still received 0.82 from most
//        callers), so a ratio test against it flipped the field every tick,
//        and under alternate-frame stereo the owner saw the clouds static in
//        one eye and sliding in the other. So the sky DOES read the field.
//   193: holding the field at fs_fov all frame fixes the clouds in both eyes
//        (owner: "clouds work fine"), but the head turn felt hesitant and
//        jittery. The engine re-authors the field once a frame and we rewrite
//        it a tick later, so within one frame some consumers see 0.82 and
//        others 1.92; one proj[2] caller per frame receives the field's own
//        value (fovy == ours), which is the consumer the sky rides on.
//   194: mode 1 holds the field wide only until that consuming call, then
//        restores it at once, so the camera controller and everything else
//        see the engine's value. Mode 0 is 193's hold. fov_root_aimk is a
//        gain on the injected head-aim deltas while armed, the fallback if
//        the feel was the engine scaling its look response by the field.
//
// Rules that hold in every mode: a written value is recognised by its exact
// bits and never scaled again; the field is restored on stand-down (key off,
// fullscreen off, a mode that is not 0, the engine's own narrowing below the
// world band, ownership lost); the engine's value is never overwritten on
// restore once it has re-authored the field; one camera is owned at a time,
// and ownership ages out if that camera stops ticking. Only the mode-0
// camera is ever written.
//
// Hot path (the selector runs about three times per frame): a few SEH reads,
// relaxed atomics, at most one SEH write, no logging (rule 8).
// ---------------------------------------------------------------------------
static bool fr_write_u32(uint64_t addr, uint32_t v) {
    __try {
        *(volatile uint32_t*)addr = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static float fr_f(uint32_t b) { float f; memcpy(&f, &b, sizeof(f)); return f; }
static uint32_t fr_b(float f) { uint32_t b; memcpy(&b, &f, sizeof(b)); return b; }

// Rule 3: put the engine's value back only if the field still holds ours,
// and only on the camera we own.
static void fr_restore(uint64_t cam) {
    if (!g_fr_armed.load(std::memory_order_relaxed)) return;
    if (cam != g_fr_cam.load(std::memory_order_relaxed)) return;
    uint32_t cur = 0;
    if (read_block(cam + 0x2B0, &cur, sizeof(cur)) &&
        cur == g_fr_written.load(std::memory_order_relaxed)) {
        if (fr_write_u32(cam + 0x2B0, g_fr_orig.load(std::memory_order_relaxed)))
            g_fr_restores.fetch_add(1, std::memory_order_relaxed);
        else
            g_fr_seh.fetch_add(1, std::memory_order_relaxed);
    }
    g_fr_armed.store(0, std::memory_order_relaxed);
}

// Hand the proj[2] call that follows this tick the field it is about to read
// and whose bits those are. One token, consumed once.
static void fr_pair(uint32_t field_bits, bool ours) {
    g_fr_pair_field.store(field_bits, std::memory_order_relaxed);
    g_fr_pair_ours.store(ours ? 1u : 0u, std::memory_order_relaxed);
    g_fr_token.store(1u, std::memory_order_relaxed);
}

// BUILD 197: the source fov. Rules: exact bits; the engine's own fov stays
// observable as the field minus the offset we wrote; the offset is 0 whenever
// that fov is outside the world band (ADS, scopes, the menu, the load screen),
// so the engine's zoom is untouched; a write is judged only once the field
// reflects it (the tick after a write still shows the old field); a Present
// authored by one of the engine's OTHER field writers (the blend path, the
// director's other modes, the pre-recompute publish), which never add the
// offset, is left alone: under them the offset is inert, and zeroing it there
// only starts an oscillation when the mode-1 apply resumes; the object is
// re-read every tick and must be off-module (the pointer slot shares a section
// with the anti-tamper flag: never write there); 0 goes back on every
// stand-down; nothing is written while +0x34 is 0 (absolute mode would slam the
// camera) or while the view's scene object carries the free camera's fov
// override (+0x286); an offset that keeps snapping back (four re-arms within
// two Presents of a band zero inside about three seconds) or that another
// writer keeps replacing stands the route down for the run and the 1 Hz line
// says so. Render thread: relaxed atomics, SEH reads and writes, no logging
// (rule 8).
static bool fovsrc_obj_ok(uint64_t obj) {
    const uint8_t* lo = g_fs_img_lo.load(std::memory_order_relaxed);
    const uint8_t* hi = g_fs_img_hi.load(std::memory_order_relaxed);
    if (!obj || !lo || !hi) return false;
    const uint8_t* p = (const uint8_t*)obj;
    return !(p >= lo && p < hi);
}

// Stood down for the run: 0 into the object if ours is in it, and no more writes.
static void fovsrc_disable(uint64_t obj) {
    g_fs_disabled.store(1, std::memory_order_relaxed);
    if (g_fs_off.load(std::memory_order_relaxed) != 0) {
        if (fr_write_u32(obj + 0x30, 0u)) g_fs_zeroes.fetch_add(1, std::memory_order_relaxed);
        else g_fs_seh.fetch_add(1, std::memory_order_relaxed);
    }
    g_fs_off.store(0, std::memory_order_relaxed);
    g_fs_armed.store(0, std::memory_order_relaxed);
}

// One write into the object's +0x30. `field` is what Camera+0x2B0 showed at
// this tick, kept as the gauge that tells not-applied-yet from applied.
// `anchor` marks a zero written because the engine's own fov left the world
// band: only those start the flip clock (a stand-down zero never does).
static void fovsrc_write(uint64_t obj, uint32_t bits, uint32_t field, uint64_t now, bool anchor) {
    if (!fr_write_u32(obj + 0x30, bits)) {
        g_fs_seh.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_fs_off.store(bits, std::memory_order_relaxed);
    g_fs_pre.store(field, std::memory_order_relaxed);
    g_fs_write_tick.store(now, std::memory_order_relaxed);
    g_fs_oob.store(0, std::memory_order_relaxed);
    if (bits == 0) {
        g_fs_zeroes.fetch_add(1, std::memory_order_relaxed);
        g_fs_armed.store(0, std::memory_order_relaxed);
        if (anchor) g_fs_last_zero_tick.store(now, std::memory_order_relaxed);
        return;
    }
    g_fs_offw.fetch_add(1, std::memory_order_relaxed);
    g_fs_armed.store(1, std::memory_order_relaxed);
    g_fs_applied_tick.store(now, std::memory_order_relaxed);
    // A re-arm within two Presents of a band zero is a flip. Four flips inside
    // about three seconds, with no frame in those three seconds that showed
    // the offset applied, means the field does not take the offset (a clamp
    // downstream): stand down for the run rather than flash the world at two
    // rates. A blend the engine authors without the offset also flips, but it
    // always follows an applied frame, so it never disables. The engine's own
    // zoom leaves the band for far longer than two Presents, so it never
    // counts at all.
    const uint64_t lz = g_fs_last_zero_tick.load(std::memory_order_relaxed);
    if (lz && now - lz <= 6) {
        g_fs_flips.fetch_add(1, std::memory_order_relaxed);
        const uint64_t rt = g_fs_rapid_tick.load(std::memory_order_relaxed);
        uint32_t n = g_fs_rapid.load(std::memory_order_relaxed);
        if (!rt || now - rt > 600) n = 0;
        ++n;
        g_fs_rapid.store(n, std::memory_order_relaxed);
        g_fs_rapid_tick.store(now, std::memory_order_relaxed);
        if (n >= 4 && now - g_fs_seen_applied.load(std::memory_order_relaxed) > 600)
            fovsrc_disable(obj);
    }
}

// Put 0 back if a non-zero offset of ours is in the object and it still reads.
static void fovsrc_stand_down(uint64_t now) {
    const uint32_t off = g_fs_off.load(std::memory_order_relaxed);
    if (off == 0) { g_fs_armed.store(0, std::memory_order_relaxed); return; }
    uint64_t obj = 0;
    uint32_t cur = 0;
    if (g_camoff_global && read_block((uint64_t)g_camoff_global, &obj, sizeof(obj)) &&
        fovsrc_obj_ok(obj) && read_block(obj + 0x30, &cur, sizeof(cur)) && cur == off) {
        fovsrc_write(obj, 0u, g_fs_pre.load(std::memory_order_relaxed), now, false);
        return;
    }
    // Not ours any more, or not readable: forget it rather than write blind.
    g_fs_off.store(0, std::memory_order_relaxed);
    g_fs_armed.store(0, std::memory_order_relaxed);
}

// Every selector tick, with the camera's mode and field already read.
static void fovsrc_tick(uint64_t cam, uint32_t mode, uint32_t f2b0) {
    // Build 196 byte for byte while the key is off and nothing of ours is in
    // the object: no reads, no counters.
    if (g_fov_src.load(std::memory_order_relaxed) == 0 &&
        g_fs_off.load(std::memory_order_relaxed) == 0) return;
    if (!g_camoff_global) return;   // the signature missed: the route is off, said at init
    const uint64_t now = g_fr_ticks.load(std::memory_order_relaxed);
    const bool on = g_fov_src.load(std::memory_order_relaxed) != 0 &&
                    g_fs_ident.load(std::memory_order_relaxed) != 0 &&
                    g_fs_disabled.load(std::memory_order_relaxed) == 0 &&
                    headpose::fs_enabled() && headpose::fs_fov() > 0.0f;
    uint64_t own = g_fs_cam.load(std::memory_order_relaxed);
    // Ticks of anything since the gauge last ticked, and consecutive mode-0
    // ticks of another camera: the two age-outs, immune to how many cameras
    // of other modes tick in between.
    uint32_t since = 0;
    if (own && cam != own) since = g_fs_since_own.fetch_add(1, std::memory_order_relaxed) + 1;
    if (mode != 0) {
        if (cam == own) {
            // The gauge camera left mode 0 (a vehicle, a cutscene): the
            // director's other branches do not add the offset. Put 0 back
            // and let go.
            fovsrc_stand_down(now);
            g_fs_cam.store(0, std::memory_order_relaxed);
        } else if (own && since > 12 && g_fs_off.load(std::memory_order_relaxed) != 0) {
            // The gauge has not ticked for four Presents of other cameras
            // while ours is live: it stopped or paused. Put 0 back now, keep
            // the gauge for when it returns.
            fovsrc_stand_down(now);
        }
        return;
    }
    if (own && cam != own) {
        // Another mode-0 camera. Six of its ticks in a row without one of
        // ours means ours is gone (replaced, a load): put 0 back and let this
        // one be the gauge. Fewer means both are live: leave this one alone.
        if (g_fs_foreign_run.fetch_add(1, std::memory_order_relaxed) + 1 < 6) {
            g_fs_foreign_cam.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        fovsrc_stand_down(now);
        own = 0;
    }
    if (!own) { g_fs_cam.store(cam, std::memory_order_relaxed); own = cam; }
    g_fs_owner_tick.store(now, std::memory_order_relaxed);
    g_fs_since_own.store(0, std::memory_order_relaxed);
    g_fs_foreign_run.store(0, std::memory_order_relaxed);

    uint64_t obj = 0;
    if (!read_block((uint64_t)g_camoff_global, &obj, sizeof(obj))) {
        g_fs_seh.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!fovsrc_obj_ok(obj)) {
        // Null this early, or gone (a load nulls the global before the
        // constructor makes a new one): whatever was ours is gone with it.
        g_fs_obj.store(0, std::memory_order_relaxed);
        if (g_fs_off.load(std::memory_order_relaxed) != 0) {
            g_fs_off.store(0, std::memory_order_relaxed);
            g_fs_armed.store(0, std::memory_order_relaxed);
            g_fs_rebuilt.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const uint64_t prev_obj = g_fs_obj.exchange(obj, std::memory_order_relaxed);
    if (prev_obj && prev_obj != obj && g_fs_off.load(std::memory_order_relaxed) != 0) {
        // A new object (a respawn, a fast travel): its +0x30 is the
        // constructor's 0, not a writer replacing ours. Forget ours quietly.
        g_fs_off.store(0, std::memory_order_relaxed);
        g_fs_armed.store(0, std::memory_order_relaxed);
        g_fs_rebuilt.fetch_add(1, std::memory_order_relaxed);
    }
    uint8_t m34 = 0;
    uint32_t cur_off = 0;
    if (!read_block(obj + 0x34, &m34, sizeof(m34)) ||
        !read_block(obj + 0x30, &cur_off, sizeof(cur_off))) {
        g_fs_seh.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_fs_mode34.store(m34, std::memory_order_relaxed);

    // Identity, read only: the scene object of the view this camera belongs
    // to; its +0x286 is the free camera's fov override, 0 in play.
    uint64_t view = 0, scene = 0;
    uint8_t s286 = 0;
    uint32_t s290 = 0;
    if (read_block(cam + 0x748, &view, sizeof(view)) && view &&
        read_block(view + 0x30, &scene, sizeof(scene)) && scene) {
        read_block(scene + 0x286, &s286, sizeof(s286));
        read_block(scene + 0x290, &s290, sizeof(s290));
    } else {
        scene = 0;
    }
    g_fs_scene.store(scene, std::memory_order_relaxed);
    g_fs_s286.store(s286, std::memory_order_relaxed);
    g_fs_s290.store(s290, std::memory_order_relaxed);

    const uint32_t off = g_fs_off.load(std::memory_order_relaxed);
    if (cur_off != off) {
        // Not what we last wrote. Never write over a value that is not ours.
        g_fs_foreign.fetch_add(1, std::memory_order_relaxed);
        if (off != 0) {
            // Ours was replaced while live: a writer we do not see. Forget
            // ours; three times and the route stands down for the run.
            g_fs_off.store(0, std::memory_order_relaxed);
            g_fs_armed.store(0, std::memory_order_relaxed);
            if (g_fs_replaced.fetch_add(1, std::memory_order_relaxed) + 1 >= 3)
                g_fs_disabled.store(1, std::memory_order_relaxed);
        }
        return;
    }
    if (!on || m34 == 0 || s286 != 0 ||
        now < g_fs_backoff_until.load(std::memory_order_relaxed)) {
        if (off != 0) fovsrc_write(obj, 0u, f2b0, now, false);
        return;
    }
    const float fs = headpose::fs_fov();
    const float field = fr_f(f2b0);
    if (off != 0) {
        if (fabsf(field - fs) <= 1e-3f) {
            // Converged: the engine authored component plus ours.
            g_fs_applied_tick.store(now, std::memory_order_relaxed);
            g_fs_seen_applied.store(now, std::memory_order_relaxed);
            g_fs_rapid.store(0, std::memory_order_relaxed);
            g_fs_oob.store(0, std::memory_order_relaxed);
            return;
        }
        if (f2b0 == g_fs_pre.load(std::memory_order_relaxed)) {
            // Not applied yet, or a writer that ignores the offset holds the
            // field where it was. Wait. After three seconds without a single
            // applied frame, put 0 back (hygiene) and try again later.
            if (now - g_fs_write_tick.load(std::memory_order_relaxed) <= 9)
                g_fs_pending.fetch_add(1, std::memory_order_relaxed);
            else
                g_fs_unhonoured.fetch_add(1, std::memory_order_relaxed);
            if (now - g_fs_applied_tick.load(std::memory_order_relaxed) > 600)
                fovsrc_write(obj, 0u, f2b0, now, false);
            return;
        }
        const float comp = field - fr_f(off);
        g_fs_comp.store(fr_b(comp), std::memory_order_relaxed);
        if (!(comp >= 0.05f)) {
            // A fov below 0.05 is impossible, so this Present was authored
            // WITHOUT our offset (the blend path, another director mode).
            // Ours is inert there: leave it, do not judge it.
            g_fs_unapplied.fetch_add(1, std::memory_order_relaxed);
            if (now - g_fs_applied_tick.load(std::memory_order_relaxed) > 600)
                fovsrc_write(obj, 0u, f2b0, now, false);
            return;
        }
        if (isfinite(comp) && comp >= headpose::fs_fov_lo() && comp <= 1.35f) {
            // The engine's own fov moved inside the band (sprint, stance):
            // follow it. The frame showed ours applied.
            g_fs_applied_tick.store(now, std::memory_order_relaxed);
            g_fs_seen_applied.store(now, std::memory_order_relaxed);
            g_fs_rapid.store(0, std::memory_order_relaxed);
            g_fs_oob.store(0, std::memory_order_relaxed);
            const float target = fs - comp;
            if (!(isfinite(target) && target > -3.0f && target < 3.0f)) {
                g_fs_skip_target.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            fovsrc_write(obj, fr_b(target), f2b0, now, false);
            return;
        }
        // The engine's own fov left the band: a zoom (ADS, a scope) gets the
        // field back after one Present, anything wider (the menu, a capture)
        // after two, so a blend's last frames do not count as an exit.
        g_fs_skip_band.fetch_add(1, std::memory_order_relaxed);
        const uint32_t need = comp < headpose::fs_fov_lo() ? 3u : 6u;
        if (g_fs_oob.fetch_add(1, std::memory_order_relaxed) + 1 >= need)
            fovsrc_write(obj, 0u, f2b0, now, true);
        return;
    }
    // Nothing of ours in the object: the field is the engine's own fov.
    g_fs_comp.store(f2b0, std::memory_order_relaxed);
    if (!(isfinite(field) && field >= headpose::fs_fov_lo() && field <= 1.35f)) {
        g_fs_skip_band.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const float target = fs - field;
    if (!(isfinite(target) && target > -3.0f && target < 3.0f)) {
        g_fs_skip_target.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    fovsrc_write(obj, fr_b(target), f2b0, now, false);
}

// BUILD 198: THE SCENE FOV OVERRIDE. See the state block for the design.
// Render thread: relaxed atomics, SEH reads and writes, no logging (rule 8).
static bool fr_write_u8(uint64_t addr, uint8_t v) {
    __try {
        *(volatile uint8_t*)addr = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool fovscene_obj_ok(uint64_t p) {
    const uint8_t* lo = g_fs_img_lo.load(std::memory_order_relaxed);
    const uint8_t* hi = g_fs_img_hi.load(std::memory_order_relaxed);
    if (!p || !lo || !hi) return false;
    const uint8_t* q = (const uint8_t*)p;
    return !(q >= lo && q < hi);
}

// The override goes in: the fov first, then the enable byte.
static void fovscene_set(uint64_t scene, uint32_t orig290, uint64_t now) {
    const uint32_t fsb = fr_b(headpose::fs_fov());
    if (!fr_write_u32(scene + 0x290, fsb)) {
        g_sc_seh.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!fr_write_u8(scene + 0x286, 1u)) {
        // The fov went in and the enable did not: put the fov back.
        fr_write_u32(scene + 0x290, orig290);
        g_sc_seh.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_sc_orig290.store(orig290, std::memory_order_relaxed);
    g_sc_wrote290.store(fsb, std::memory_order_relaxed);
    g_sc_scene.store(scene, std::memory_order_relaxed);
    g_sc_armed.store(1, std::memory_order_relaxed);
    g_sc_set_tick.store(now, std::memory_order_relaxed);
    g_sc_sets.fetch_add(1, std::memory_order_relaxed);
}

// The override comes out: the enable byte first, then the original fov.
static void fovscene_clear(uint64_t scene, bool ads) {
    bool ok = fr_write_u8(scene + 0x286, 0u);
    ok = fr_write_u32(scene + 0x290, g_sc_orig290.load(std::memory_order_relaxed)) && ok;
    if (!ok) g_sc_seh.fetch_add(1, std::memory_order_relaxed);
    g_sc_armed.store(0, std::memory_order_relaxed);
    g_sc_scene.store(0, std::memory_order_relaxed);
    g_sc_clears.fetch_add(1, std::memory_order_relaxed);
    if (ads) g_sc_ads_clears.fetch_add(1, std::memory_order_relaxed);
}

// Clear ours if it is still in the object it went into and still reads as
// ours; otherwise forget it rather than write blind.
static void fovscene_stand_down() {
    const uint64_t scene = g_sc_scene.load(std::memory_order_relaxed);
    if (scene && g_sc_armed.load(std::memory_order_relaxed)) {
        uint8_t s286 = 0;
        uint32_t s290 = 0;
        if (fovscene_obj_ok(scene) &&
            read_block(scene + 0x286, &s286, sizeof(s286)) &&
            read_block(scene + 0x290, &s290, sizeof(s290)) &&
            s286 == 1 && s290 == g_sc_wrote290.load(std::memory_order_relaxed)) {
            fovscene_clear(scene, false);
            return;
        }
    }
    g_sc_armed.store(0, std::memory_order_relaxed);
    g_sc_scene.store(0, std::memory_order_relaxed);
}

// Every selector tick, with the camera's mode and field already read.
static void fovscene_tick(uint64_t cam, uint32_t mode, uint32_t f2b0) {
    const uint64_t now = g_fr_ticks.load(std::memory_order_relaxed);
    // The director, read only, at every player camera tick. Null, unreadable
    // or in-module is counted and never used.
    if (g_director_global && mode == 0) {
        uint64_t d = 0;
        uint32_t dm = 0;
        uint8_t db = 0;
        if (read_block((uint64_t)g_director_global, &d, sizeof(d)) && fovscene_obj_ok(d) &&
            read_block(d + 0x6C, &dm, sizeof(dm)) && read_block(d + 0x60, &db, sizeof(db))) {
            g_dir_obj.store(d, std::memory_order_relaxed);
            g_dir_mode.store(dm, std::memory_order_relaxed);
            g_dir_blend.store(db, std::memory_order_relaxed);
            g_dir_hist[dm < 7 ? dm : 7].fetch_add(1, std::memory_order_relaxed);
            g_dir_reads.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_dir_bad.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Build 197 byte for byte while the key is off and nothing of ours is in
    // any scene object: no reads, no counters.
    if (g_fov_scene.load(std::memory_order_relaxed) == 0 &&
        g_sc_scene.load(std::memory_order_relaxed) == 0) return;
    if (!g_sc_ident.load(std::memory_order_relaxed)) return;   // the select site was not found: off, said at init
    const bool on = g_fov_scene.load(std::memory_order_relaxed) != 0 &&
                    g_sc_disabled.load(std::memory_order_relaxed) == 0 &&
                    headpose::fs_enabled() && headpose::fs_fov() > 0.0f;
    uint64_t own = g_sc_cam.load(std::memory_order_relaxed);
    if (mode != 0) {
        if (cam == own) {
            // The owned camera left mode 0 (a vehicle, a cutscene): the
            // engine's own fov for that state must show. Clear ours, let go.
            fovscene_stand_down();
            g_sc_cam.store(0, std::memory_order_relaxed);
        }
        return;
    }
    if (own && cam != own) {
        // Another mode-0 camera. Six of its ticks in a row without ours means
        // ours is gone (replaced, a load): clear ours and take this one.
        if (g_sc_foreign_run.fetch_add(1, std::memory_order_relaxed) + 1 < 6) return;
        fovscene_stand_down();
        own = 0;
    }
    if (!own) { g_sc_cam.store(cam, std::memory_order_relaxed); own = cam; }
    g_sc_owner_tick.store(now, std::memory_order_relaxed);
    g_sc_foreign_run.store(0, std::memory_order_relaxed);

    // The view's scene object, re-read every tick, never inside the module.
    uint64_t view = 0, scene = 0;
    if (!read_block(cam + 0x748, &view, sizeof(view)) || !fovscene_obj_ok(view) ||
        !read_block(view + 0x30, &scene, sizeof(scene)) || !fovscene_obj_ok(scene)) {
        g_sc_noscene.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t had = g_sc_scene.load(std::memory_order_relaxed);
    bool rebuilt = false;
    if (had && had != scene) {
        // The camera's view has a different scene object now (a load, a
        // respawn, a view switch). The old one may still be live: clear ours
        // there if it still reads as ours (a read first, never blind), else
        // forget it. The new one is judged below.
        g_sc_rebuilt.fetch_add(1, std::memory_order_relaxed);
        fovscene_stand_down();
        rebuilt = true;
    }
    uint8_t s286 = 0, s14d = 0;
    uint32_t s290 = 0;
    if (!read_block(scene + 0x286, &s286, sizeof(s286)) ||
        !read_block(scene + 0x290, &s290, sizeof(s290))) {
        g_sc_seh.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    read_block(scene + 0x14D, &s14d, sizeof(s14d));
    g_sc_scene_seen.store(scene, std::memory_order_relaxed);
    g_sc_s286.store(s286, std::memory_order_relaxed);
    g_sc_s290.store(s290, std::memory_order_relaxed);
    g_sc_s14d.store(s14d, std::memory_order_relaxed);
    if (rebuilt && s286 == 1 && s290 == g_sc_wrote290.load(std::memory_order_relaxed)) {
        // The new object is a copy of the old one, ours included: take it
        // over (the original bits kept from the first set) rather than call
        // our own value foreign for the rest of the run.
        g_sc_scene.store(scene, std::memory_order_relaxed);
        g_sc_armed.store(1, std::memory_order_relaxed);
        g_sc_set_tick.store(now, std::memory_order_relaxed);
    }

    const bool ours = g_sc_armed.load(std::memory_order_relaxed) != 0 &&
                      g_sc_scene.load(std::memory_order_relaxed) == scene;
    if (ours) {
        if (s286 != 1 || s290 != g_sc_wrote290.load(std::memory_order_relaxed)) {
            // Replaced by a writer we do not see (the free camera, a reset).
            // Forget ours; three times and the route stands down for the run.
            g_sc_foreign.fetch_add(1, std::memory_order_relaxed);
            g_sc_armed.store(0, std::memory_order_relaxed);
            g_sc_scene.store(0, std::memory_order_relaxed);
            if (g_sc_replaced.fetch_add(1, std::memory_order_relaxed) + 1 >= 3)
                g_sc_disabled.store(1, std::memory_order_relaxed);
            return;
        }
    } else if (s286 != 0) {
        // Someone else's override (the free camera): never touched.
        g_sc_foreign.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool ads = g_fov_scene_ads.load(std::memory_order_relaxed) != 0 &&
                     g_ads_input.load(std::memory_order_relaxed) != 0;
    if (ads) g_sc_ads_ticks.fetch_add(1, std::memory_order_relaxed);
    const bool want = on && !ads;
    if (!want) {
        if (ours) fovscene_clear(scene, ads);
        return;
    }
    if (!ours) {
        fovscene_set(scene, s290, now);
        return;
    }
    // Ours is in: judge the field. The engine stores +0x290 into it before
    // every recompute, so the tick after the set should already read it.
    const uint32_t fsb = g_sc_wrote290.load(std::memory_order_relaxed);
    if (f2b0 == fsb) {
        g_sc_applied.fetch_add(1, std::memory_order_relaxed);
        g_sc_seen_applied.store(now, std::memory_order_relaxed);
    } else if (now - g_sc_set_tick.load(std::memory_order_relaxed) > 9) {
        g_sc_unhonoured.fetch_add(1, std::memory_order_relaxed);
    }
    // fs_fov changed under us (a cfg reload): follow it.
    const uint32_t cur = fr_b(headpose::fs_fov());
    if (cur != fsb && fr_write_u32(scene + 0x290, cur)) {
        g_sc_wrote290.store(cur, std::memory_order_relaxed);
        g_sc_set_tick.store(now, std::memory_order_relaxed);   // the grace starts again
    }
}

// At the selector's entry, with rcx, before its body reads the field.
static void fovroot_tick(uint64_t cam) {
    if (!cam) return;
    g_fr_ticks.fetch_add(1, std::memory_order_relaxed);
    uint32_t mode = 0, f2b0 = 0, f2bc = 0, f2c0 = 0, f798 = 0;
    if (!read_block(cam + kOffMode, &mode, sizeof(mode)) ||
        !read_block(cam + 0x2B0, &f2b0, sizeof(f2b0))) {
        g_fr_seh.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // BUILD 247: camera 0 in true-stereo mode reads viewport mode 4 only because the engine's viewport setter put
    // it there; it is still the player camera, and the fov routes below stand down for any non-zero mode. Seen as
    // mode 0 here, and only for the pointer the experiment recorded, only while the stereo route is live.
    if (mode == 4 && ::grwxr::cam2::stereo_live() && cam == ::grwxr::cam2::first_camera()) mode = 0;
    fovsrc_tick(cam, mode, f2b0);   // BUILD 197: the source fov, every tick
    fovscene_tick(cam, mode, f2b0); // BUILD 198: the scene fov override, every tick
    if (mode != 0) {
        g_fr_lastmode.store(mode, std::memory_order_relaxed);
        g_fr_notplayer.fetch_add(1, std::memory_order_relaxed);
        g_fr_token.store(0u, std::memory_order_relaxed);
        // The same object may change mode (a vehicle, a cutscene). Rule 2:
        // stand down on it; fr_restore checks the camera and the bits.
        fr_restore(cam);
        // BUILD 195: and if the owned camera has not ticked for about three
        // seconds while only other cameras do, it is gone: drop ownership.
        if (g_fr_armed.load(std::memory_order_relaxed) &&
            g_fr_ticks.load(std::memory_order_relaxed) -
                g_fr_owner_tick.load(std::memory_order_relaxed) >= 600) {
            g_fr_armed.store(0, std::memory_order_relaxed);
            g_fr_ownerlost.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    // ONE CAMERA AT A TIME. While we own a camera, another mode-0 camera is
    // left exactly as the engine has it: not written, not recorded, not
    // paired. It is counted so the log shows a second player camera exists.
    bool armed = g_fr_armed.load(std::memory_order_relaxed) != 0;
    if (armed && cam != g_fr_cam.load(std::memory_order_relaxed)) {
        // BUILD 194: if the owned camera has not ticked for about three
        // seconds it is gone (replaced, freed); drop ownership without a
        // restore, which would write into an object we cannot see, and let
        // this camera be owned.
        const uint64_t since = g_fr_ticks.load(std::memory_order_relaxed) -
                               g_fr_owner_tick.load(std::memory_order_relaxed);
        if (since < 600) {
            g_fr_foreign.fetch_add(1, std::memory_order_relaxed);
            g_fr_token.store(0u, std::memory_order_relaxed);
            return;
        }
        g_fr_armed.store(0, std::memory_order_relaxed);
        g_fr_ownerlost.fetch_add(1, std::memory_order_relaxed);
        armed = false;
    }
    if (armed) g_fr_owner_tick.store(g_fr_ticks.load(std::memory_order_relaxed), std::memory_order_relaxed);
    // BUILD 196: the phase counts this camera's own ticks, nothing else.
    const uint64_t wt = g_fr_wticks.fetch_add(1, std::memory_order_relaxed) + 1;
    read_block(cam + kOffAspect, &f2bc, sizeof(f2bc));
    read_block(cam + 0x2C0, &f2c0, sizeof(f2c0));
    read_block(cam + 0x798, &f798, sizeof(f798));
    g_fr_f2B0.store(f2b0, std::memory_order_relaxed);
    g_fr_f2BC.store(f2bc, std::memory_order_relaxed);
    g_fr_f2C0.store(f2c0, std::memory_order_relaxed);
    g_fr_f798.store(f798, std::memory_order_relaxed);

    bool ours = armed && f2b0 == g_fr_written.load(std::memory_order_relaxed);
    const float cur = fr_f(f2b0);
    if (!ours) {
        // Engine-authored (or the engine re-authored our camera): rule 3's
        // reference, and ownership ends.
        g_fr_orig.store(f2b0, std::memory_order_relaxed);
        g_fr_armed.store(0, std::memory_order_relaxed);
    }
    uint32_t now = f2b0;                      // what the field holds when we leave
    const bool on = g_fov_root.load(std::memory_order_relaxed) != 0 &&
                    headpose::fs_enabled() && headpose::fs_fov() > 0.0f;
    if (!on) {
        if (ours) { fr_restore(cam); now = g_fr_orig.load(std::memory_order_relaxed); ours = false; }
        fr_pair(now, ours);
        return;
    }
    const float engine_base = ours ? fr_f(g_fr_orig.load(std::memory_order_relaxed)) : cur;
    if (!(isfinite(engine_base) && engine_base >= headpose::fs_fov_lo() &&
          engine_base <= 1.35f)) {
        g_fr_skip_band.fetch_add(1, std::memory_order_relaxed);
        if (ours) { fr_restore(cam); now = g_fr_orig.load(std::memory_order_relaxed); ours = false; }
        fr_pair(now, ours);
        return;
    }
    if (g_fr_narrow.load(std::memory_order_relaxed)) {
        // The engine's factor is off the play ratio (ADS, a scope, a sprint
        // starting or ending): hands off until the paired call says otherwise.
        g_fr_skip_narrow.fetch_add(1, std::memory_order_relaxed);
        if (ours) { fr_restore(cam); now = g_fr_orig.load(std::memory_order_relaxed); ours = false; }
        fr_pair(now, ours);
        return;
    }
    // BUILD 193: the target is fs_fov itself. The 192 run showed the field is
    // not what the selector scales into proj[2]; it is what the sky and the
    // other camera-derived passes read, and the world is already drawn at
    // fs_fov by the argument override, so the field must hold fs_fov.
    const float target = headpose::fs_fov();
    if (!(isfinite(target) && target > 0.30f && target < 3.0f)) {
        g_fr_skip_target.fetch_add(1, std::memory_order_relaxed);
        fr_pair(now, ours);
        return;
    }
    const uint32_t tb = fr_b(target);
    if (ours && tb == f2b0) {
        // BUILD 195: armed for 60 ticks without a release in mode 1 means the
        // consumer did not follow the phase we learned; forget it and write
        // on every tick again until the next release teaches it.
        if (g_fov_root_mode.load(std::memory_order_relaxed) == 1 &&
            g_fr_phase.load(std::memory_order_relaxed) >= 0 &&
            wt - g_fr_release_tick.load(std::memory_order_relaxed) > 60)
            g_fr_phase.store(-1, std::memory_order_relaxed);
        fr_pair(now, ours); return;                           // converged, still armed
    }
    // BUILD 195, mode 1: write only on the tick the consuming call follows,
    // once that phase is known, so a later tick in the same frame cannot put
    // the wide value back after the release. BUILD 196: the forget applies
    // here too, whether or not the field still holds ours, so a wrong phase
    // (60 of this camera's ticks without a release) cannot stick.
    if (g_fov_root_mode.load(std::memory_order_relaxed) == 1) {
        if (g_fr_phase.load(std::memory_order_relaxed) >= 0 &&
            wt - g_fr_release_tick.load(std::memory_order_relaxed) > 60)
            g_fr_phase.store(-1, std::memory_order_relaxed);
        const int ph = g_fr_phase.load(std::memory_order_relaxed);
        if (ph >= 0 && (int)(wt % 3) != ph) {
            g_fr_skip_phase.fetch_add(1, std::memory_order_relaxed);
            fr_pair(now, ours);
            return;
        }
    }
    if (fr_write_u32(cam + 0x2B0, tb)) {
        g_fr_write_tick.store(wt, std::memory_order_relaxed);
        g_fr_written.store(tb, std::memory_order_relaxed);
        g_fr_cam.store(cam, std::memory_order_relaxed);
        g_fr_armed.store(1, std::memory_order_relaxed);
        g_fr_owner_tick.store(g_fr_ticks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_fr_writes.fetch_add(1, std::memory_order_relaxed);
        now = tb; ours = true;
    } else {
        g_fr_seh.fetch_add(1, std::memory_order_relaxed);
    }
    fr_pair(now, ours);
}

static void grwxr_probe_record_body(uint64_t index, const void* saved_raw,
                                    void* return_address, uint64_t* stack_args);

// BUILD 215: every probe target is timed by index (CX-029). The body below is
// unchanged; this wrapper only reads the performance counter around it. An
// index past the table lands in the last slot so nothing is ever unmeasured.
static_assert(kNumTargets == perf::kNumCam - 1,
              "BUILD 215: one perf slot per probe target plus one for out of range; grow perf::kNumCam with kTargets");
extern "C" void grwxr_probe_record(uint64_t index, const void* saved_raw,
                                   void* return_address, uint64_t* stack_args) {
    const int slot = index < (uint64_t)(perf::kNumCam - 1) ? (int)index : perf::kNumCam - 1;
    perf::Scope sc(perf::kCam0 + slot);
    grwxr_probe_record_body(index, saved_raw, return_address, stack_args);
}

static void grwxr_probe_record_body(uint64_t index, const void* saved_raw,
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
            // BUILD 103: the lower edge is a setting now. It was 0.60 here,
            // and plain ADS publishes 0.3300 rad on this binary, so ADS fell
            // out of this band, out of the flat-scope path below
            // mono_scope_fov, and out of head-aim injection below 0.65, and
            // got drawn at its true 18.9 degrees into a black canvas. See
            // HeadPose.h at set_fs_fov_lo. Default is still 0.60.
            // BUILD 191: the engine's own fovy before any override (the 4.3
            // instrument). BUILD 193: hands off only on the engine's own
            // absolute fovy below the world band (ADS, scopes). BUILD 194,
            // mode 1: the call whose fovy equals the value we wrote is the
            // consumer the sky rides on; restore the field right here, before
            // persp2 runs (it already has its fovy in hand), so nothing else
            // in the frame sees the wide value.
            {
                g_fr_eng.store(fr_b(fovy), std::memory_order_relaxed);
                if (g_fr_armed.load(std::memory_order_relaxed) != 0 &&
                    g_fov_root_mode.load(std::memory_order_relaxed) == 1 &&
                    fabsf(fovy - fr_f(g_fr_written.load(std::memory_order_relaxed))) < 1e-4f) {
                    const uint64_t oc = g_fr_cam.load(std::memory_order_relaxed);
                    if (oc) {
                        fr_restore(oc);
                        g_fr_releases.fetch_add(1, std::memory_order_relaxed);
                        // BUILD 195: the consumer followed the tick that wrote;
                        // remember that tick's phase and write only there.
                        g_fr_phase.store((int)(g_fr_write_tick.load(std::memory_order_relaxed) % 3),
                                         std::memory_order_relaxed);
                        g_fr_release_tick.store(g_fr_wticks.load(std::memory_order_relaxed),
                                                std::memory_order_relaxed);
                    }
                }
                if (g_fr_token.exchange(0u, std::memory_order_relaxed) != 0u &&
                    fabsf(fovy - 1.5707964f) > 1e-3f) {          // not a capture face
                    const float f = fr_f(g_fr_pair_field.load(std::memory_order_relaxed));
                    if (isfinite(f) && f > 0.05f) {
                        const float r = fovy / f;
                        g_fr_ratio.store(fr_b(r), std::memory_order_relaxed);
                        const bool  pours = g_fr_pair_ours.load(std::memory_order_relaxed) != 0;
                        const bool  off   = fovy < headpose::fs_fov_lo();
                        g_fr_narrow.store(off ? 1u : 0u, std::memory_order_relaxed);
                        const float rprev = fr_f(g_fr_rprev.load(std::memory_order_relaxed));
                        if (!pours && !off && f >= headpose::fs_fov_lo() && f <= 1.35f &&
                            r > 0.5f && r < 2.0f && rprev > 0.0f &&
                            fabsf(r - rprev) <= 0.02f * r) {
                            g_fr_splay.store(fr_b(r), std::memory_order_relaxed);
                        }
                        g_fr_rprev.store(fr_b(r), std::memory_order_relaxed);
                    }
                }
            }
            const float fs = headpose::fs_fov();
            if (headpose::fs_enabled() && fs > 0.0f &&
                fovy >= headpose::fs_fov_lo() && fovy <= 1.35f) {
                fovy = fs;
                memcpy(&stack_args[4], &fovy, sizeof(fovy));
            }
            {   // BUILD 248: in a half-width stereo viewport every WORLD projection is forced to ONE vertical fov (sized to the
                // headset), whatever the engine or the scene override computed, so both cameras render the same frustum and
                // the placement is steady. Argument 6 is the aspect the engine built this projection with. ADS and scope
                // projections (below fs_fov_lo at full width) and cubemap faces are left alone. One relaxed load when stereo is off.
                float asp = 0.0f;
                memcpy(&asp, &stack_args[5], sizeof(asp));
                float forced = 0.0f;
                if (::grwxr::cam2::stereo_override_fovy(fovy, asp, headpose::fs_fov_lo(), &forced)) {
                    fovy = forced;
                    memcpy(&stack_args[4], &fovy, sizeof(fovy));
                }
            }
            headpose::publish_fov(fovy);
            {   // BUILD 247: the aspect the engine built this projection with (argument 6), for the stereo2: line. One relaxed load when off.
                float asp = 0.0f;
                memcpy(&asp, &stack_args[5], sizeof(asp));
                ::grwxr::cam2::stereo_note_proj(asp);
            }

            // BUILD 118: publish the LIVE near and far clip planes.
            //
            // [VERIFIED 2026-08-20] proj[2] on this binary is persp2
            // (lastrites 0x0D7BB920), whose signature is
            //   persp2(Matrix4x4* outProj, Matrix4x4* outInvProj,
            //          float near /*xmm2*/, float far /*xmm3*/,
            //          float fovY /*stack_args[4]*/, float aspect, rect[4])
            // so near and far are already sitting in the saved register block
            // this probe receives. No new hook, no new register save, two
            // loads on a path that is already running.
            //
            // WHY IT MATTERS: XR_KHR_composition_layer_depth needs nearZ and
            // farZ, and the constructor defaults (0.1 near, 1200.0 far, from
            // Camera::Camera 0x0C4FC070) are only defaults. The engine
            // overrides near per frame via Camera+0x7A4, and has a one-shot
            // "far = near + 0.1" sliver mode on Camera+0x7B1 bit 0.
            //
            // IT ALSO SETTLES REVERSED-Z WITHOUT A SEARCH. projbuild
            // (0x0D7C0610) implements reversed-Z by SWAPPING these two
            // arguments, so near > far here means the engine took that branch
            // and near maps to NDC 1. Reading it back is the project rule;
            // assuming the convention is how this class of bug is made.
            const float near_p = a->xmm2[0];
            const float far_p  = a->xmm3[0];
            if (near_p > 0.0f && far_p > 0.0f && near_p != far_p)
                headpose::publish_clip(near_p, far_p);

            // BUILD 246: hand the call's arguments to the constant-buffer content capture. READ ONLY:
            // persp2(outProj rcx, outInv rdx, near xmm2, far xmm3, fovY stack arg 5, aspect stack arg 6, rect*).
            // One relaxed load unless the cbdump window is open. fovy here is the value persp2 will receive.
            if (::grwxr::cbdump::capturing()) {
                float asp = 0.0f;
                memcpy(&asp, &stack_args[5], sizeof(asp));
                if (!(asp > 0.1f && asp < 10.0f)) asp = 0.0f;
                ::grwxr::cbdump::note_proj(a->rcx, a->rdx, near_p, far_p, fovy, asp);
            }
        }
    }

    // Build 87: FALLBACK CAPTURE OF THE CAMERA OBJECT.
    //
    // Only on_calc_mvp writes g_player_cam, and on binaries where that row is
    // not derived it never installs, leaving the pointer null forever. This
    // supplies it from the selector instead, which is hooked, whose rcx is the
    // same camera, and which passes the same read_camera + mode==0 validation.
    //
    // Deliberately does NOT touch the camera pose: it only records the
    // pointer, so base_frame() and the controller ray come back while the
    // stereo write stays exactly where on_calc_mvp owns it. If on_calc_mvp is
    // present it runs first and stores the same value, so this is a no-op on
    // a fully derived build rather than a second writer competing with it.
    //
    // Rule 8 clean: one relaxed store on a path that already reads the camera.
    //
    // Build 89: THE POSE WRITE MOVES HERE TOO, under cfg cam_selector_pose.
    //
    // Build 87 deliberately recorded the pointer and nothing else, on the
    // reasoning that the stereo write should stay where on_calc_mvp owns it.
    // The 2026-08-15 headset log showed what that costs: write_pose_head is
    // called from exactly ONE site, the on_calc_mvp recorder, so on this
    // binary it never runs. `head compose: idle` for the whole session, and
    // with it g_base and g_base_pos stay {0,0,0}. That is not a partial loss:
    //   - FP anchoring tests dist3(head, camera) < 12 m against a camera at
    //     the origin, so it measures the player's world coordinate (6836 m)
    //     and never anchors
    //   - the 11c push fallback adds fp_forward * g_base[3..5], which is zero,
    //     so the FP toggle moves the viewpoint nowhere at all
    //   - the per-eye IPD offset is written by the same function, so both eyes
    //     get the same image and the headset is MONO
    //
    // The selector is the only other target with rcx_is_camera, it passes the
    // same read_camera + mode==0 validation, and it is called ~3 times per
    // frame; write_pose_head is frame-idempotent, so that rate is fine.
    //
    // What is NOT known and only the headset can answer: whether the selector
    // runs before the engine recomputes the camera pose, in which case our
    // write is overwritten and this does nothing. That failure is harmless and
    // self-diagnosing, `aim: aer: basepos step / vs-write` measures it.
    //
    // Deferral, not competition: the write is skipped entirely once
    // on_calc_mvp is seen running, so a build that derives that row keeps the
    // single writer it was designed around without a cfg change.
    // BUILD 243: THE SECOND CAMERA IS NOT THE PLAYER. Every test below says "the player's camera is the
    // one whose Camera+0x290 reads 0", and the experiment's camera 2 (Cam2.h) reads 0 as well, because
    // it is synced to the same view settings. So the two branches that act on that test (the selector
    // and on_calc_mvp) are keyed on the camera POINTER first: a call whose camera is camera 2 is left
    // completely alone (no eye toggle, no eye tag, no head-pose write, no fov tick, no g_player_cam).
    // second_camera() is 0 whenever the experiment is off, so nothing changes in that case.
    const uint64_t cam2_ptr = ::grwxr::cam2::second_camera();
    const bool     is_cam2  = cam2_ptr != 0 && a->rcx == cam2_ptr;
    // BUILD 247: TRUE STEREO re-keys the player camera on the POINTER. In stereo mode camera 0 is in viewport
    // mode 4 (the engine's setter put it there), so `mode == 0` no longer finds it; camera 0 is the pointer the
    // second-camera experiment recorded before it changed that mode. In stereo mode EVERY other camera is left
    // completely alone by these two branches (fail closed: no pose write, no eye tag, no fov tick for them).
    // stereo_live() is false whenever cam2_stereo is 0, and then none of this changes anything.
    const bool     stereo_on   = ::grwxr::cam2::stereo_live();
    const uint64_t cam0_ptr    = stereo_on ? ::grwxr::cam2::first_camera() : 0;
    const bool     stereo_cam0 = cam0_ptr != 0 && a->rcx == cam0_ptr;

    if (index == kSelectorProbe && is_cam2 && stereo_on) {
        // Camera 2's own selector call: re-assert this frame's latched pose (one frame old if this call came
        // first; the counters in the stereo2: line say which order the engine actually uses).
        ::grwxr::cam2::stereo_cam2_call(d3d11::frame_count());
    }

    if (index == kSelectorProbe && !is_cam2 && (!stereo_on || stereo_cam0)) {
        fovroot_tick(a->rcx);                 // BUILD 191, before the body reads the field
        g_mvp_tid_sel = GetCurrentThreadId();
        // BUILD 267: once the recompute hook is live the generic recorder bumps g_calls[6] on EVERY recompute, so the old
        // "on_calc_mvp has never run" test would switch the on-foot selector write off. The selector defers ONLY while
        // the hook is composing the vehicle frame itself (build 251 / 266 behaviour everywhere else).
        const bool want_pose =
            headpose::cam_selector_pose() && !mvp_drives();
        bool wrote_cam0 = false;
        if (want_pose || !g_player_cam.load(std::memory_order_relaxed)) {
            const CamFields cs = read_camera(a->rcx);
            if (cs.valid && (cs.mode == 0 || stereo_cam0)) {
                g_player_cam.store(a->rcx, std::memory_order_relaxed);
                if (want_pose) {
                    float H[9], Q[4];
                    if (headpose::read(H, Q)) {
                        if (write_pose_head(a->rcx, H, Q)) {
                            wrote_cam0 = true;
                            g_head_writes.fetch_add(1, std::memory_order_relaxed);
                            g_head_sel_writes.fetch_add(1,
                                                        std::memory_order_relaxed);
                        } else {
                            g_head_write_fails.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        }
        // BUILD 247: no head pose this call (no session, tracking lost, selector pose off): camera 2 still follows
        // camera 0, moved cam2_ipd along its right axis, so the experiment keeps working on the desktop. The frame
        // pushed no tag, so the present shows the left half only.
        if (stereo_cam0 && !wrote_cam0) ::grwxr::cam2::stereo_mirror(a->rcx);
    }

    // BUILD 269: THE ENGINE'S OWN CAMERA RECOMPUTE (on_calc_mvp, entered through MvpHook.cpp), at ENTRY, before the body reads the pose rows.
    // Always counts (the observer). Composes only while the vehicle compose is on and the hook drives, and only at the two main passes
    // of the engine frame (see kSiteA / kSiteB): one head sample and one rotation per ENGINE FRAME, written to both cameras at the first
    // pass A entry and re-asserted, identical, at every later A and B entry. Cameras other than camera 0 and camera 2 and every other
    // recompute site (the sim-thread weapon spread, TAA, cube, terrain) are never touched.
    if (index == kOnCalcMvpProbe) {
        const uint64_t c0 = ::grwxr::cam2::first_camera();
        const uint64_t c2 = ::grwxr::cam2::second_camera();
        const int role = (c0 && a->rcx == c0) ? 0 : (c2 && a->rcx == c2) ? 1 : 2;
        g_mvp_calls[role].fetch_add(1, std::memory_order_relaxed);
        if (role < 2 && stereo_on) {
            if (!g_img_base) g_img_base = (uint64_t)(uintptr_t)GetModuleHandleW(nullptr);
            int site = 2;
            uint32_t ret_rva = 0xFFFFFFFFu;   // BUILD 270: for the gtrec B row only
            bool     ret_ok  = false;
            __try {
                const uint64_t ra2 = *(const volatile uint64_t*)((const uint8_t*)stack_args + 0x108);
                const uint64_t rva = ra2 - g_img_base;
                ret_rva = (uint32_t)rva;
                ret_ok  = true;
                if (rva == kSiteA) site = 0;
                else if (rva == kSiteB) site = 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                site = 2;
            }
            g_mvp_site[site].fetch_add(1, std::memory_order_relaxed);
            if (GetCurrentThreadId() == g_mvp_tid_sel) g_mvp_same_tid.fetch_add(1, std::memory_order_relaxed);
            else                                       g_mvp_diff_tid.fetch_add(1, std::memory_order_relaxed);
            const uint8_t bit = (uint8_t)(1u << role);
            bool first_of_engine_frame = false;
            if (site == 0) {
                if (g_mvp_seenA & bit) {                  // this camera again at pass A: the previous engine frame is over
                    g_mvp_per_build[(g_mvp_seenA == 3) ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
                    g_mvp_seenA = 0;
                    ++g_build_seq;
                }
                first_of_engine_frame = (g_mvp_seenA == 0);
                if (first_of_engine_frame && role == 1) g_mvp_cam2_first.fetch_add(1, std::memory_order_relaxed);
                g_mvp_seenA |= bit;
            }
            // BUILD 270: gtrec (GtRec.h), LOG ONLY. One B row per entry of camera 0 or camera 2 at EVERY call site (the other
            // sites are classified and never written, exactly as above): the rows before and after the action below, the
            // previous recompute's output, and what the action was. With gtrec = 0 this is one atomic load.
            ::grwxr::gtrec::BRow* gb = nullptr;
            uint32_t gact = ::grwxr::gtrec::kActNone;
            if (::grwxr::gtrec::on())
                gb = ::grwxr::gtrec::b_begin(a->rcx, role == 0 ? c2 : c0, role, (uint32_t)g_build_seq, first_of_engine_frame,
                                             ret_ok ? (uint32_t)site : ::grwxr::gtrec::kSiteUnread, ret_rva, g_mvp_seenA,
                                             (g_veh_on.load(std::memory_order_relaxed) ? 0x20u : 0u) | (mvp_drives() ? 0x40u : 0u) |
                                             0x80u | (g_vf_valid ? 0x100u : 0u));
            if (mvp_drives() && site < 2) {
                if (first_of_engine_frame) {
                    // ONE head sample for the whole engine frame, composed and written to BOTH cameras BEFORE the body runs.
                    float H[9], Q[4];
                    g_vf_valid = false;
                    if (c0 && headpose::read(H, Q)) {
                        if (write_pose_head(c0, H, Q)) {
                            gact = ::grwxr::gtrec::kActCompose;
                            g_head_writes.fetch_add(1, std::memory_order_relaxed);
                            const float* mm = (const float*)(c0 + kOffPose);
                            for (int r = 0; r < 3; ++r) memcpy(g_vf_rows + r * 3, mm + r * 4, 12);
                            memcpy(g_vf_rows + 9, mm + 12, 12);
                            g_vf_valid = true;
                        } else {
                            gact = ::grwxr::gtrec::kActWriteFail;
                            g_head_write_fails.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        gact = ::grwxr::gtrec::kActNoHead;   // BUILD 270: the gtrec action only
                    }
                } else if (g_vf_valid) {
                    // pass A of the other camera, and pass B of either: the same pose again, so nothing the engine does between
                    // two recomputes (the sim-thread spread code restores its saved rows) can leave a stale rotation behind
                    gact = ::grwxr::gtrec::kActReassert;
                    if (role == 0) {
                        veh_write_cam0_rows(c0);
                        if (gb) ::grwxr::gtrec::w_mark(::grwxr::gtrec::kWReassert, c0, 0);
                    } else {
                        ::grwxr::cam2::stereo_reassert_current();   // its W row is Cam2.cpp's
                    }
                }
            } else if (mvp_drives() && site == 1 && g_vf_valid) {
                gact = ::grwxr::gtrec::kActReassert;
                if (role == 0) {
                    veh_write_cam0_rows(c0);
                    if (gb) ::grwxr::gtrec::w_mark(::grwxr::gtrec::kWReassert, c0, 0);
                } else {
                    ::grwxr::cam2::stereo_reassert_current();
                }
            }
            ::grwxr::gtrec::b_end(gb, gact);   // BUILD 270: reads the rows again after the action and publishes the B row
            // the observer: do the rows this camera holds at entry equal the rotation composed for this engine frame? (vehicle only)
            if (g_mvp_observe.load(std::memory_order_relaxed) && g_veh_on.load(std::memory_order_relaxed) &&
                g_last_build == g_build_seq && g_brec_n.load(std::memory_order_relaxed) > 0) {
                const float* mm = (const float*)(a->rcx + kOffPose);
                const BuildRec& br = g_brec[(g_brec_n.load(std::memory_order_acquire) - 1) & 15];
                float d = 0.0f;
                for (int rr = 0; rr < 3; ++rr)
                    for (int c = 0; c < 3; ++c) {
                        const float e = fabsf(mm[rr * 4 + c] - br.out[rr * 3 + c]);
                        if (e > d) d = e;
                    }
                (d < 1e-3f ? g_mvp_rows_eq : g_mvp_rows_ne).fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Build 5: when armed, capture one full argument vector of on_calc_mvp for
    // the player's camera (mode 0). The CAS makes the recorder the single
    // writer; drain() prints the buffer on the init thread. No logging here.
    if (index == kOnCalcMvpProbe && !is_cam2 && (!stereo_on || stereo_cam0)) {
        CamFields c = read_camera(a->rcx);
        if (c.valid && (c.mode == 0 || stereo_cam0)) {
            g_player_cam.store(a->rcx, std::memory_order_relaxed);

            // Build 8: the one behavioural change of this build. See the
            // comment block at write_pose_head. A failed read (nothing
            // published, VR inactive, or a torn seqlock) leaves the camera
            // entirely alone for this call.
            // BUILD 267: the head compose that used to sit here is the BUILD 267 branch above (vehicle only, stereo cameras only).
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
        //
        // BUILD 131: KEEP THEM ALL, not just the last one.
        //
        // A bare store means the LAST cPlayerComponent to initialise becomes
        // "the player" forever: a squadmate, a respawn, a vehicle transition,
        // anything. If that pointer's entity is not the one whose skeleton is
        // in the ring, the owner test `[skel+0x10] == pent` can never match,
        // no pin is ever taken, and the camera sits on the 11c chase-rig push.
        // That is cause (a) written down by build 113 and never resolved:
        // "pent is wrong, so we are comparing against the wrong entity and the
        // player's skeletons are sitting in the ring unmatched."
        //
        // `[VERIFIED, grwxr-27148]` a whole run with first person on where the
        // ring lookup missed on every sample.
        //
        // The donor is the FP mod's resolve_player(), which has always kept a
        // TABLE of components and scored the candidates. Same author, same
        // engine, method ported and re-derived here, not copied wholesale.
        g_playercomp.store(a->rcx, std::memory_order_relaxed);
        capture_unique(g_pc_tab, a->rcx);
    }
    if (index == kSkelPostProbe) {
        capture_unique(g_skel_rcx, a->rcx);
        g_skel_rdx_last.store(a->rdx, std::memory_order_relaxed);
        g_skel_ring[g_skel_ring_w.fetch_add(1, std::memory_order_relaxed) &
                    (kRingSize - 1)].store(a->rcx, std::memory_order_relaxed);
        // Build 65: the write test, pre-update on the engine's thread.
        const uint64_t wt = g_wskel_tgt.load(std::memory_order_relaxed);
        if (wt && a->rcx == wt &&
            g_wskel_writes.load(std::memory_order_relaxed) < kWskelWriteCap)
            wskel_write_apply(
                wt, g_wskel_tgt_rig.load(std::memory_order_relaxed));
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

// cfg safe_mode: a bisection switch for the v0.8.0 Virtual Desktop
// regression (issue #2). When set, the two engine hooks this release
// ADDED (PublishAttachments and SetWorldTransform, both installed at
// startup even while gated inert by wgun/wnode) are NOT installed at all,
// so the running mod behaves like v0.7.0 for the frame loop. This lets a
// tester decide, without a rebuild, whether the freeze is the new hooks or
// the new toolchain. The cfg is not parsed this early (VRMirror reads it
// once the device exists), so read the one key directly. Default off, so
// this is a no-op for everyone until they set it.
bool safe_mode_requested() {
    const std::wstring path = log::data_dir() + L"\\grwxr.cfg";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    char line[256];
    bool on = false;
    while (fgets(line, sizeof(line), f)) {
        float v = 0.0f;
        if (sscanf_s(line, " safe_mode = %f", &v) == 1) on = v > 0.0f;
    }
    fclose(f);
    return on;
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

    // Build 46: the fail-closed pin. No table for this binary means nothing
    // installs, exactly like the old anchor-RVA refusal, but with the reason
    // named up front (see the "build pin:" line above this one in the log).
    const gamebuild::Build* gb = gamebuild::get();
    if (!gb) {
        LOG_ERROR("camera: no address table for this GRW.exe build. Probe "
                  "NOT installed. The game runs unmodified.");
        return false;
    }

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
    if (anchor_rva != gb->cam[0].fn) {
        LOG_ERROR("camera: anchor is at RVA 0x%08zX but the %s thunk table "
                  "was built for 0x%08zX. This is NOT the binary we analysed.",
                  (size_t)anchor_rva, gb->name, (size_t)gb->cam[0].fn);
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
    // Build 91: SKIP UNDERIVED ROWS EXPLICITLY, and say so.
    //
    // A row that is {0, 0} on this binary used to fall through to
    // install(base + 0, base + 0, ...), which ThunkHook correctly refuses
    // because the byte at the image base is not 0xE9. It failed safe, but it
    // logged "thunk at <image base> does not start with E9 (found 0x4D)", which
    // reads like a corrupt hook rather than "this row is not derived on this
    // build". That message cost real time on 2026-08-14 and again on 08-15.
    int missing = 0;
    for (int i = 0; i < kNumTargets; ++i) {
        // BUILD 267: on_calc_mvp has no E9 thunk on TU25; MvpHook builds one in the padding and repoints the single call site.
        // The original is handed over by address so it is set BEFORE the call is repointed. A refusal falls through to the
        // "not derived" accounting below with nothing written.
        if (i == (int)kOnCalcMvpProbe && gb->cam[i].fn && !gb->cam[i].thunk) {
            if (mvp::install(img->base, gb->pe_timestamp, gb->pe_size_of_image, entries[i], &grwxr_probe_originals[i])) {
                ++ok;
                continue;
            }
            grwxr_probe_originals[i] = nullptr;
        }
        if (!gb->cam[i].fn || !gb->cam[i].thunk) {
            ++missing;
            grwxr_probe_originals[i] = nullptr;
            LOG_WARN("camera: %s is NOT DERIVED on this build (rule 7): "
                     "nothing installed for it.", kTargets[i].name);
            continue;
        }
        uint8_t* fn = img->base + gb->cam[i].fn;
        grwxr_probe_originals[i] = fn;
        if (g_hooks[i].install(img->base + gb->cam[i].thunk, fn, entries[i],
                               kTargets[i].name)) {
            ++ok;
        }
    }

    // Build 91: `ok > 0` called ONE hook of eleven a success. On the Last Rites
    // binary that let the mod run a whole session with the camera write site
    // missing while reporting healthy, which is exactly the silent degradation
    // the 2026-08-15 session spent hours diagnosing by hand. The flat FP mod
    // requires ok == want and rolls back otherwise. We do not roll back here,
    // because ten of these rows carry features that work perfectly well without
    // the eleventh and refusing all of them would be worse, but the log now
    // states the shortfall in one line instead of burying it in a count.
    g_any = ok > 0;
    const int want = kNumTargets - missing;
    if (ok != want || missing) {
        LOG_WARN("camera: %d of %d targets hooked, %d row(s) not derived on "
                 "this build, %d derived row(s) FAILED to install. Features "
                 "behind the missing rows are OFF, not broken.",
                 ok, kNumTargets, missing, want - ok);
    }
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
        g_setyaw_hook.install_raw(img->base + gb->setyaw_slot, kSetYawExpect,
                                  sizeof(kSetYawExpect),
                                  (void*)&grwxr_setyaw_entry,
                                  "SetYaw vdispatch");
        memcpy(&grwxr_setpitch_disp, kSetPitchExpect + 6,
               sizeof(grwxr_setpitch_disp));
        g_setpitch_hook.install_raw(img->base + gb->setpitch_slot,
                                    kSetPitchExpect, sizeof(kSetPitchExpect),
                                    (void*)&grwxr_setpitch_entry,
                                    "SetPitch vdispatch");
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
        auto setter = sig::find_unique(*img, gb->head_setter_sig, &m);
        if (!setter || *setter != img->base + gb->head_setter_impl) {
            LOG_ERROR("hide: SetHidden signature %s (matches=%zu, expected "
                      "RVA 0x%08zX). Head hide OFF, everything else runs.",
                      setter ? "matched at the WRONG address" : "missed",
                      m, (size_t)gb->head_setter_impl);
            hh_ok = false;
        }
        // Build 91: head_table is {0} on binaries where it could not be derived
        // (the vtable holds no absolute VAs on disk because Denuvo resolves them
        // at runtime, so a content scan returns zero hits even in the binary
        // where the answer is known). Without this check the memcpy below reads
        // img->base + 0x1F0, which is inside the PE HEADER, gets a zero, and
        // reports "slot +0x1F0 resolves to 0x0 ... NOT the class we analysed".
        // That is a misdiagnosis of a derivation gap as a class mismatch, and
        // the 2026-08-15 log shows it verbatim.
        if (hh_ok && !gb->head_table) {
            LOG_WARN("hide: head_table is NOT DERIVED on this build (rule 7). "
                     "Head hide OFF. This is a missing address, not a class "
                     "mismatch: the on-disk vtable carries no absolute VAs "
                     "because Denuvo resolves them at runtime, so it needs a "
                     "runtime census rather than a scan.");
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
            memcpy(&slotval, img->base + gb->head_table + 0x1F0,
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
            grwxr_headhide_impl = (uint64_t)(img->base + gb->head_setter_impl);
            if (g_headhide_hook.install(img->base + gb->head_setter_thunk,
                                        img->base + gb->head_setter_impl,
                                        (void*)&grwxr_headhide_entry,
                                        "SetHidden")) {
                // Published LAST: the asm entry ignores everything until the
                // table pointer is nonzero.
                grwxr_headhide_table = (uint64_t)(img->base + gb->head_table);
                LOG_INFO("hide: armed. The head object hides whenever first "
                         "person is on (class table verified via slot fn).");
            }
        }
    }

    // BUILD 224: THE PRE-LATCH HOOK. Closes the "head visible until the first
    // setter call" gap by hooking the predicate the per-frame visibility-apply
    // function calls with the render object in rcx (kHeadVisApplySig). NOT
    // load-bearing, same as head hide itself: every failure below logs its
    // reason and leaves only this feature off, with the setter latch still
    // working. Verified before a byte is patched (rule 7): the apply-site
    // signature must match uniquely at the pinned RVA, the extracted call must
    // target the pinned thunk, and the thunk's impl must byte-match the known
    // 5-byte body.
    if (grwxr_headhide_table) {
        const HeadVisPins* pins = head_vis_pins(gb->pe_timestamp);
        if (!pins) {
            LOG_ERROR("hide: pre-latch RVAs have never been derived for build "
                      "%s (TimeDateStamp %08X). Pre-latch OFF; the head still "
                      "hides at the first setter call. This is a missing "
                      "derivation, NOT a signature failure.",
                      gb->name, gb->pe_timestamp);
        } else {
            bool hl_ok = true;
            size_t hm = 0;
            auto site = sig::find_unique(*img, kHeadVisApplySig, &hm);
            if (!site) {
                LOG_ERROR("hide: pre-latch apply-site signature missed or "
                          "ambiguous (matches=%zu, expected 1 at RVA 0x%08zX "
                          "for build %s). Pre-latch OFF.",
                          hm, (size_t)pins->apply_rva, gb->name);
                hl_ok = false;
            } else if ((uintptr_t)(*site - img->base) != pins->apply_rva) {
                LOG_ERROR("hide: pre-latch apply-site signature matched "
                          "uniquely at RVA 0x%08zX, but the pin for build %s "
                          "says 0x%08zX. The PINNED ADDRESS is stale (game "
                          "patched?). Pre-latch OFF.",
                          (size_t)(*site - img->base), gb->name,
                          (size_t)pins->apply_rva);
                hl_ok = false;
            }
            uint8_t* thunk = nullptr;
            if (hl_ok) {
                uint8_t* call_at = *site - 0x15;
                int32_t  rel     = 0;
                if (call_at[0] != 0xE8) {
                    LOG_ERROR("hide: pre-latch call site byte is 0x%02X, "
                              "expected E8. Pre-latch OFF.", call_at[0]);
                    hl_ok = false;
                } else {
                    memcpy(&rel, call_at + 1, sizeof(rel));
                    thunk = call_at + 5 + rel;
                    if ((uintptr_t)(thunk - img->base) != pins->pred_thunk) {
                        LOG_ERROR("hide: pre-latch call targets RVA 0x%08zX, "
                                  "expected 0x%08zX. Pre-latch OFF.",
                                  (size_t)(thunk - img->base),
                                  (size_t)pins->pred_thunk);
                        hl_ok = false;
                    }
                }
            }
            if (hl_ok) {
                uint8_t* impl = img->base + pins->pred_impl;
                if (memcmp(impl, kHeadVisPredImplBytes,
                           sizeof(kHeadVisPredImplBytes)) != 0) {
                    LOG_ERROR("hide: pre-latch predicate impl bytes do not "
                              "match (%02X %02X %02X %02X %02X). Pre-latch "
                              "OFF.", impl[0], impl[1], impl[2], impl[3],
                              impl[4]);
                } else {
                    // Published BEFORE the hook exists so the asm entry can
                    // never read it as 0 on a live call.
                    grwxr_headlatch_impl = (uint64_t)impl;
                    if (g_headlatch_hook.install(thunk, impl,
                                                 (void*)&grwxr_headlatch_entry,
                                                 "HeadVisPredicate")) {
                        LOG_INFO("hide: pre-latch ARMED (build %s pins "
                                 "verified in place). It offers head "
                                 "candidates every frame; one is promoted "
                                 "only when its entity handle is the pinned "
                                 "player entity, and the head then hides "
                                 "within a second of first person, no ADS.",
                                 gb->name);
                    }
                }
            }
        }
    }

    // BUILD 226: derive the engine's Populate from the proximity test's own
    // call site, prologue byte-verified. Not load-bearing: a miss logs and
    // leaves head_populate off, everything else runs.
    if (grwxr_headhide_table) {
        size_t pm = 0;
        auto site = sig::find_unique(*img, kHeadPopulateCallSig, &pm);
        if (!site) {
            LOG_ERROR("hide: populate call-site signature %s (matches=%zu). "
                      "head_populate OFF; the pulse route still runs.",
                      pm ? "AMBIGUOUS" : "missed", pm);
        } else {
            uint8_t* call_at = *site + 6;
            int32_t rel = 0;
            memcpy(&rel, call_at + 1, sizeof(rel));
            uint8_t* thunk = call_at + 5 + rel;
            uint8_t* impl  = thunk;
            if (thunk >= img->base && thunk < img->base + img->size && thunk[0] == 0xE9) {
                memcpy(&rel, thunk + 1, sizeof(rel));
                impl = thunk + 5 + rel;
            }
            if (impl < img->base || impl >= img->base + img->size ||
                memcmp(impl, kHeadPopulatePrologue, sizeof(kHeadPopulatePrologue)) != 0) {
                LOG_ERROR("hide: populate prologue does not match at RVA 0x%08zX. "
                          "head_populate OFF.", (size_t)(impl - img->base));
            } else {
                g_populate_fn.store((uint64_t)impl, std::memory_order_release);
                LOG_INFO("hide: populate DERIVED: call site RVA 0x%08zX, thunk "
                         "0x%08zX, impl 0x%08zX (prologue verified). When the "
                         "promoted head object's arrays read empty, the engine's "
                         "own fill routine is called on it once, then the setter.",
                         (size_t)(call_at - img->base), (size_t)(thunk - img->base),
                         (size_t)(impl - img->base));
            }
        }
    }

    // Build 67: PublishAttachments, for the gun-root bone write. Fail-closed
    // exactly like every other consumer: an un-derived row (0) installs
    // nothing, and ThunkHook verifies the E9 really targets the impl before a
    // byte is written. The stub itself stays inert until the census publishes
    // a player skeleton, which it only does while cfg wgun is 1.
    const bool safe = safe_mode_requested();
    if (safe) {
        LOG_WARN("camera: SAFE MODE (safe_mode=1 in grwxr.cfg). The v0.8.0 "
                 "weapon hooks (PublishAttachments, SetWorldTransform) will "
                 "NOT be installed. This is the Virtual Desktop regression "
                 "bisection switch (issue #2); the weapon feature is off.");
    }
    if (!safe && gb->publish_thunk && gb->publish_impl) {
        grwxr_wgun_impl = (uint64_t)(img->base + gb->publish_impl);
        if (g_wgun_hook.install(img->base + gb->publish_thunk,
                                img->base + gb->publish_impl,
                                (void*)&grwxr_wgun_entry,
                                "PublishAttachments")) {
            LOG_INFO("wgun: PublishAttachments hooked. Inert until cfg "
                     "wgun=1 arms it; then the player's Fake_gunroot bone is "
                     "offset by wgun_dz just before the engine places the "
                     "weapon from it.");
        }
    } else if (!safe) {
        LOG_INFO("wgun: PublishAttachments not derived for this binary. The "
                 "gun-root write is OFF, everything else runs (rule 7).");
    }

    // Build 68: TransformNode::SetWorldTransform, the substitution point.
    // Fail-closed like every other consumer, and inert until the census
    // publishes a weapon node, which it only does while cfg wnode > 0.
    if (!safe && gb->setworld_thunk && gb->setworld_impl) {
        grwxr_wnode_impl = (uint64_t)(img->base + gb->setworld_impl);
        if (g_wnode_hook.install(img->base + gb->setworld_thunk,
                                 img->base + gb->setworld_impl,
                                 (void*)&grwxr_wnode_entry,
                                 "SetWorldTransform")) {
            LOG_INFO("wnode: SetWorldTransform hooked. Inert until cfg "
                     "wnode>0; then the HELD WEAPON's placement is ours "
                     "(1 = lift by wnode_dz, 2 = barrel follows the "
                     "controller ray).");
        }
    } else if (!safe) {
        LOG_INFO("wnode: SetWorldTransform not derived for this binary. The "
                 "weapon placement override is OFF (rule 7).");
    }

    // BUILD 198: the director global (read only, the mode readout) and the
    // proof that this image selects the scene fov override before storing the
    // field. Both by signature; a miss leaves the route off (rule 7).
    g_fs_img_lo.store(img->base, std::memory_order_relaxed);
    g_fs_img_hi.store(img->base + img->size, std::memory_order_relaxed);
    {
        size_t m = 0;
        auto hit = sig::find_unique(*img, kDirectorSig, &m);
        if (!hit) {
            LOG_INFO("fovscene: director signature %s (matches=%zu); the director "
                     "mode readout is off.", m ? "AMBIGUOUS" : "missed", m);
        } else {
            int32_t disp = 0;
            memcpy(&disp, *hit + 3, sizeof(disp));
            uint8_t* global = *hit + 7 + disp;
            uint8_t* dobj = nullptr;
            // The slot itself must be a global inside the image (a
            // rip-relative scan on this image can land in a VM trampoline);
            // only then is it read, once, under SEH. It is never written.
            bool dok = global >= img->base && global < img->base + img->size;
            if (dok) {
                __try {
                    dobj = *(uint8_t**)global;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    dok = false;
                }
            }
            const bool in_mod = dobj && dobj >= img->base && dobj < img->base + img->size;
            if (!dok || in_mod) {
                LOG_INFO("fovscene: director global at RVA 0x%08zX reads %p (%s); "
                         "the readout is off.", (size_t)(global - img->base),
                         (void*)dobj, dok ? "inside the module" : "slot outside the image or unreadable");
            } else {
                g_director_global = (uint8_t**)global;
                LOG_INFO("fovscene: director global at RVA 0x%08zX, object %p%s. "
                         "Its mode (+0x6C) and blend byte (+0x60) are read at every "
                         "player camera tick.", (size_t)(global - img->base),
                         (void*)dobj, dobj ? "" : " (null this early, re-read per tick)");
            }
        }
        m = 0;
        auto hb = sig::find_unique(*img, kSiteBStoreSig, &m);
        if (hb) {
            g_sc_ident.store(1, std::memory_order_relaxed);
            LOG_INFO("fovscene: the mode-1 apply selects the scene fov override "
                     "(+0x286/+0x290) before storing the field on this image, at "
                     "RVA 0x%08zX (build 198 arms when cfg fov_scene = 1).",
                     (size_t)(*hb - img->base));
        } else {
            LOG_INFO("fovscene: the scene-override select signature %s "
                     "(matches=%zu); fov_scene will NOT arm on this image.",
                     m ? "AMBIGUOUS" : "missed", m);
        }
    }

    // BUILD 119: resolve the engine's CameraPoseOffset singleton. Read only
    // here; nothing is written unless cfg cam_offset arms it. The global is
    // recovered from the consuming instruction's own rip-relative
    // displacement, so no address is hardcoded and a game update that moves
    // the code moves the anchor with it.
    {
        size_t m = 0;
        auto hit = sig::find_unique(*img, kCamOffsetSig, &m);
        if (!hit) {
            LOG_ERROR("camoff: CameraPoseOffset signature %s (matches=%zu). "
                      "The engine-offset camera route is OFF; everything else "
                      "runs normally (rule 7).", m ? "AMBIGUOUS" : "missed", m);
        } else {
            // 48 8B 05 <disp32> : mov rax,[rip+disp32]; next insn is at +7.
            int32_t disp = 0;
            memcpy(&disp, *hit + 3, sizeof(disp));
            uint8_t* global = *hit + 7 + disp;
            g_camoff_global = (uint8_t**)global;
            uint8_t* obj = nullptr;
            __try {
                obj = *g_camoff_global;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                obj = nullptr;
                g_camoff_global = nullptr;
            }
            // SAFETY GATE, and it is not optional. The GLOBAL at 0x04BBB2D0
            // lives inside the section that also carries Denuvo's genuine
            // flag (0x046C2E70). Writing a zero into that flag turns on the
            // game's own sabotage, which includes SKIPPING SAVES ENTIRELY.
            // See the ANTI-TAMPER section of docs/RE-notes.md.
            //
            // What we write is the OBJECT, which the constructor heap
            // allocates, not the pointer slot. That is correct by
            // construction, but "correct by construction" is exactly the kind
            // of reasoning that stops being true after a game patch moves an
            // allocator. So prove it every run instead of assuming it: if the
            // object address lands inside any section of the game module, we
            // have misread the indirection and MUST NOT arm.
            bool obj_in_module = false;
            if (obj) {
                const uint8_t* lo = img->base;
                const uint8_t* hi = img->base + img->size;
                obj_in_module = (obj >= lo && obj < hi);
            }
            if (obj_in_module) {
                g_camoff_global = nullptr;
                g_camoff_state.store(3, std::memory_order_relaxed);
                LOG_ERROR("camoff: object %p is INSIDE the game module "
                          "(base %p, size 0x%zX). That means the pointer "
                          "indirection was misread, and writing it would be a "
                          "write into the module image, which is where the "
                          "anti-tamper genuine flag lives. ROUTE DISABLED.",
                          (void*)obj, (void*)img->base, (size_t)img->size);
            } else {
                LOG_INFO("camoff: CameraPoseOffset global at RVA 0x%08zX, "
                         "object %p%s (off-module, heap: safe to write). "
                         "Read only until cfg cam_offset=1 arms it.",
                         (size_t)(global - img->base), (void*)obj,
                         obj ? "" : " null this early, re-resolved per frame");
                g_camoff_state.store(1, std::memory_order_relaxed);
                // BUILD 197: the source fov needs the module bounds on every
                // tick (the object must never be inside the image) and a proof
                // that this image still ADDS the offset's +0x30 into the fov:
                // addss xmm6,[rax+0x30] then movss [rbp+0x30],xmm6, within the
                // consumer's next 0x200 bytes. Missing means the route stays
                // off whatever the cfg says (rule 7).
                g_fs_img_lo.store(img->base, std::memory_order_relaxed);
                g_fs_img_hi.store(img->base + img->size, std::memory_order_relaxed);
                {
                    static const uint8_t kFovAdd[] = {0xF3, 0x0F, 0x58, 0x70, 0x30,
                                                      0xF3, 0x0F, 0x11, 0x75, 0x30};
                    bool add_found = false;
                    __try {
                        for (size_t i = 0; i + sizeof(kFovAdd) <= 0x200; ++i) {
                            if (memcmp(*hit + i, kFovAdd, sizeof(kFovAdd)) == 0) { add_found = true; break; }
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        add_found = false;
                    }
                    g_fs_ident.store(add_found ? 1u : 0u, std::memory_order_relaxed);
                    LOG_INFO("fovsrc: the fov add after the CameraPoseOffset consumer is %s "
                             "on this image%s", add_found ? "PRESENT" : "MISSING",
                             add_found ? " (build 197 arms when cfg fov_src = 1)."
                                   : ". The source fov stays OFF this run whatever the cfg says (rule 7).");
                }
            }
        }
    }

    // Build 35: the no-camera-blur byte. See the constants block for the
    // provenance and the write-class note.
    {
        size_t m = 0;
        auto hit = sig::find_unique(*img, kNoBlurSig, &m);
        if (!hit) {
            LOG_ERROR("blur: no-camera-blur signature %s (matches=%zu). "
                      "Blur patch OFF, everything else runs.",
                      m ? "AMBIGUOUS" : "missed", m);
        } else if (*hit != img->base + gb->noblur_match) {
            LOG_ERROR("blur: signature matched at RVA 0x%08zX, expected "
                      "0x%08zX (game updated?). Blur patch OFF.",
                      (size_t)(*hit - img->base), (size_t)gb->noblur_match);
        } else if ((*hit)[kNoBlurImmOff] != 0x01) {
            LOG_ERROR("blur: byte at match+%zu reads 0x%02X, expected 01. "
                      "Blur patch OFF.",
                      kNoBlurImmOff, (*hit)[kNoBlurImmOff]);
        } else {
            uint8_t* b = *hit + kNoBlurImmOff;
            DWORD prot = 0;
            if (VirtualProtect(b, 1, PAGE_EXECUTE_READWRITE, &prot)) {
                *b = 0x00;
                DWORD dummy = 0;
                VirtualProtect(b, 1, prot, &dummy);
                FlushInstructionCache(GetCurrentProcess(), b, 1);
                g_noblur_byte = b;
                LOG_INFO("blur: camera blur DISABLED (mov sil,1 -> mov sil,0 "
                         "at RVA 0x%08zX, the FP mod's No Camera Blur).",
                         (size_t)(b - img->base));
            } else {
                LOG_ERROR("blur: VirtualProtect failed (err %lu). "
                          "Blur patch OFF.", GetLastError());
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

// Build 64: weapon-skeleton identifier (session 29 route 1). The ADS
// pin-steal (15e.2, 2026-08-02 log correlation) proved the drawn weapon is
// its own SkeletonPostUpdate instance, and the rigid census shows small
// non-humanoid rig classes live near the player (18/20 bones, no Head
// hash). The 1 Hz pass below picks the small-rig instance nearest the
// player's right hand and publishes it; wskel_marker() re-reads its world
// position per frame so the WHITE marker rides the gun without a
// one-second trail. Read-only everywhere.
std::atomic<bool>     g_wskel_on{false};
std::atomic<uint64_t> g_wskel_cand{0};

// BUILD 69: the weapon-pick lock. Touched only by the 1 Hz drain tick, so plain
// scalars: no other thread reads them. See the lock block in the scan for why
// a per-tick argmax was not enough.
uint64_t g_wskel_lock      = 0;   // the weapon we are committed to
uint64_t g_wskel_chal      = 0;   // the challenger currently accumulating wins
int      g_wskel_chal_hits = 0;
constexpr int kLockSwitchTicks = 3;   // consecutive clear wins before a switch

// Build 5 cadence, one call per second from the init thread. Prints a pending
// snapshot as soon as the recorder fills one, and every 20 seconds re-arms the
// snapshot and dumps the matrices. Independent of the survey throttle below so
// neither starves the other.
void drain_head_telemetry(int ticks);   // build 91, defined below

// BUILD 297 (log only): once per rig, the live player rig's whole name-hash map -> GRWVR/rigdump-<pid>-<n>.tsv, so the bones the arms
// and the backpack hang on can be named offline (the bone-name wordlist). Init thread only (file I/O), never the engine or Present thread.
static void rig_dump_once() {
    static uint64_t s_dumped = 0;
    static int      s_dump_n = 0;
    const uint64_t sk = grwxr_wgun_skel;
    uint64_t rg = 0;
    if (!sk || !read_block(sk + 0x220, &rg, sizeof(rg)) || rg < 0x10000 || (rg & 7) || rg == s_dumped) return;
    uint16_t cnt = 0, bn = 0;
    uint64_t map = 0;
    read_block(rg + 0x5A, &cnt, sizeof(cnt));
    read_block(rg + 0x50, &map, sizeof(map));
    read_block(rg + 0x8A, &bn, sizeof(bn));
    if (!(cnt > 0 && cnt < 2048 && map >= 0x10000 && !(map & 3))) return;
    const std::wstring rp = log::data_dir() + L"\\rigdump-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(++s_dump_n) + L".tsv";
    FILE* rf = nullptr;
    if (_wfopen_s(&rf, rp.c_str(), L"w") != 0 || !rf) return;
    fprintf(rf, "# rig 0x%llX bones=%u hashes=%u skel 0x%llX\n", (unsigned long long)rg, (unsigned)bn, (unsigned)cnt, (unsigned long long)sk);
    for (int i = 0; i < (int)cnt; ++i) {
        uint32_t h = 0;
        uint16_t ix = 0;
        if (!read_block(map + (uint64_t)i * 8, &h, sizeof(h)) || !read_block(map + (uint64_t)i * 8 + 4, &ix, sizeof(ix))) break;
        fprintf(rf, "%08X\t%u\n", h, (unsigned)ix);
    }
    fclose(rf);
    s_dumped = rg;
    LOG_INFO("rigdump: wrote %u name hashes of the live player rig (bones=%u) to rigdump-%lu-%d.tsv (build 297, log only)",
             (unsigned)cnt, (unsigned)bn, (unsigned long)GetCurrentProcessId(), s_dump_n);
}

void snap_drain() {
    static int ticks = 0;
    ++ticks;

    // Build 91: FIRST, before any early return. The "player entity is live"
    // branch below ends in an unconditional return, which silently killed this
    // telemetry for the whole of every session once the entity latched.
    drain_head_telemetry(ticks);

    // BUILD 119: THE TAMPER CANARY. A 4-byte READ. Nothing is written.
    //
    // The game ships an anti-tamper apparatus whose responses are all
    // GAMEPLAY SABOTAGE dressed as bugs: player damage, weapon spread
    // randomisation, permanent night, broken revives, exploding loot, and
    // most importantly trigger ID 7, which is WHEN_GENUINE and skips
    // GR_MissionManager_SaveData entirely. Not genuine means NO SAVE.
    //
    // [VERIFIED 2026-08-20, live exe] The flag is the dword at RVA
    // 0x046C2E70. 1 = GENUINE, 0 = TAMPERED, established from branch
    // polarity across all 16 trigger sites (WHEN_GENUINE compiles to
    // cmp reg,1/jne; WHEN_TAMPERED to test reg,reg/jne).
    //
    // [VERIFIED] In the shipped image the flag has exactly ONE writer, at
    // 0x0E0AC01A, and it is unreachable dead code: its guard is
    // `if (time(NULL) == 0)`, its only entry thunk has zero call sites, and
    // the value it writes is 0x0BADBABE, which is neither 0 nor 1 and would
    // disable both paths. So in retail the flag reads 1 forever and every
    // sabotage is dead code.
    //
    // [UNKNOWN] whether Denuvo's runtime layer, which executes from memory
    // outside every loaded module and cannot be read statically, writes it.
    // That is exactly why this canary exists. A read cannot cause what it
    // detects, and it converts a deliberately-deniable failure mode into one
    // log line.
    //
    // THE STANDING SYMPTOM RULE: before calling any blur, softness, weapon
    // inaccuracy, wrong ammo count, unexplained damage, stuck night or failed
    // save a mod bug, read this line first, then rename dxgi.dll and repeat.
    {
        static uint32_t s_last = 0xFFFFFFFFu;
        const uint8_t* img = (const uint8_t*)GetModuleHandleW(nullptr);
        uint32_t g = 0xFFFFFFFFu;
        if (img) {
            __try {
                g = *(const volatile uint32_t*)(img + 0x046C2E70);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g = 0xFFFFFFFFu;
            }
        }
        // Log on change, and once at startup. A steady 1 is the boring,
        // correct answer and does not deserve a line every second.
        if (g != s_last) {
            s_last = g;
            if (g == 1u) {
                LOG_INFO("tamper: canary=1 GENUINE (anti-tamper sabotage is "
                         "inactive; every WHEN_TAMPERED response is dead code)");
            } else if (g == 0u) {
                LOG_ERROR("tamper: canary=0 TAMPERED. THE GAME'S OWN SABOTAGE "
                          "IS ACTIVE: weapon spread, player damage, forced "
                          "night, broken revives, AND SAVING IS SKIPPED. "
                          "Quit, rename dxgi.dll, and verify game files.");
            } else {
                LOG_ERROR("tamper: canary=0x%08X, neither 0 nor 1. Either the "
                          "offset moved with a game patch (re-pin RVA "
                          "0x046C2E70) or the flag was poisoned. Treat any "
                          "odd gameplay as suspect until this reads 1.", g);
            }
        }
    }

    // Build 11a diagnostic: the live published fov, once per second. This is
    // the number the scaling blit places the content with, and the dataset
    // for choosing the mono-scope threshold: have the user scope each optic
    // class and read the values off these lines.
    {
        const float f = headpose::read_fov(0.0f);
        if (f > 0.0f) {
            // BUILD 118: near/far ride the fov line rather than opening a new
            // one. They answer two questions at once: the real clip planes for
            // the depth layer, and whether this build is running reversed-Z
            // (near > far, because projbuild implements it by swapping the
            // arguments). Both were previously assumed from constructor
            // defaults, which the engine overrides per frame.
            float np = 0.0f, fp = 0.0f;
            if (headpose::read_clip(&np, &fp)) {
                LOG_INFO("fov: %.4f rad (%.1f deg) | near=%.4f far=%.1f %s",
                         f, f * 57.29578f, np, fp,
                         np > fp ? "REVERSED-Z (near maps to NDC 1)"
                                 : "normal Z (near maps to NDC 0)");
            } else {
                LOG_INFO("fov: %.4f rad (%.1f deg) | near/far not published yet",
                         f, f * 57.29578f);
            }
        }
    }

    // hide_child / hide_child2, 1 Hz, re-asserted because the engine
    // re-asserts its own visibility state on every camera transition. Gated
    // exactly as the FP original: only while the head object is latched AND
    // the head hide is on, because the latch is the only liveness evidence the
    // pointer has, and hide_one_child's own SEH guards and array-1 positive
    // control bound what a stale one can cost.
    {
        const uint64_t hobj = grwxr_headhide_obj;
        if (hobj && grwxr_headhide_on) {
            hide_one_child(hobj, 0,
                           g_hide_child.load(std::memory_order_relaxed), true);
            hide_one_child(hobj, 1,
                           g_hide_child2.load(std::memory_order_relaxed), true);
        }
    }

    // BUILD 144: the ray probe, 1 Hz, log only. See g_ray_probe for the
    // protocol. Top level of the tick DELIBERATELY: the wgun census line
    // died with the player pin at 20:06:18 last session and took every
    // counter with it, so this one depends on nothing but the camera.
    if (g_ray_probe.load(std::memory_order_relaxed)) {
        float R[9], C[3];
        if (base_frame(R, C)) {
            float raw[3], sd[3], tw;
            for (int i = 0; i < 3; ++i) {
                uint32_t b = g_dbg_raw[i].load(std::memory_order_relaxed);
                memcpy(&raw[i], &b, sizeof(float));
                b = g_dbg_sdir[i].load(std::memory_order_relaxed);
                memcpy(&sd[i], &b, sizeof(float));
            }
            uint32_t bb = g_dbg_tw.load(std::memory_order_relaxed);
            memcpy(&tw, &bb, sizeof(float));
            const float cf[3] = {R[3], R[4], R[5]};   // the forward column
            const float rn2 = raw[0]*raw[0] + raw[1]*raw[1] + raw[2]*raw[2];
            const float sn2 = sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2];
            if (rn2 > 0.25f && sn2 > 0.25f) {
                auto ang = [](const float a[3], const float b2[3]) {
                    float d = a[0]*b2[0] + a[1]*b2[1] + a[2]*b2[2];
                    if (d > 1.0f) d = 1.0f;
                    if (d < -1.0f) d = -1.0f;
                    return acosf(d) * 57.29578f;
                };
                LOG_INFO("rayprobe: raw_z=%+.3f sdir_z=%+.3f cam_z=%+.3f "
                         "tw=%.2f | raw-vs-cam=%.1f raw-vs-sdir=%.1f deg | "
                         "brace the hand, sweep the head: raw moves = the "
                         "mapping leaks; raw still, sdir moves = the blend; "
                         "both still = the arm drooped.",
                         raw[2], sd[2], cf[2], tw,
                         ang(raw, cf), ang(raw, sd));
            }
        }
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
    // Build 64: weapon-skeleton census + candidate pick, 1 Hz while armed
    // (cfg wskel). Candidate = ring skeleton whose rig is small (2..64
    // bones) and has no Head hash, nearest the player's right-hand bone.
    // The drawn weapon rides the hand; holstered gear hangs further away,
    // and the census lines show it either way.
    // 64.1: this block sits ABOVE the pin section because the pin's entity
    // path ends its drain tick with an early return, which silenced the
    // census the moment the player entity went live (03:04:02 in the first
    // build 64 run). The hand bone it reads comes from the PREVIOUS tick's
    // pin, which is one second stale at worst and irrelevant at 1 Hz.
    if (g_wskel_on.load(std::memory_order_relaxed) &&
        g_calls[kSkelPostProbe].load(std::memory_order_relaxed)) {
        float hand[3];
        bool  hand_ok = false;
        // BUILD 69: the gun-root reference. The 2026-08-09 mount census
        // measured the held weapon's origin at 0.002 m from the player rig's
        // Fake_gunroot bone and 0.258 m from Prop_RightHand. Selecting on the
        // hand is therefore selecting on a quantity that is 0.07 to 0.52 m for
        // EVERY weapon the player carries, which is why the pick flapped
        // between the held rifle and the holstered gear tick to tick, and why
        // mode 2 threw the controller ray at a weapon on the player's back.
        // Two millimetres against half a metre is not a better heuristic, it is
        // an identity.
        float groot[3];
        bool  groot_ok = false;
        const uint64_t body = (uint64_t)headpose::player_obj();
        if (body) {
            uint64_t brig = 0;
            if (read_block(body + 0x220, &brig, sizeof(brig)) &&
                brig > 0x10000 && !(brig & 7)) {
                const int nR = rig_find_node(brig, 0x75F94D30u);  // RightHand
                hand_ok = nR >= 0 &&
                          read_bone_world(body, (unsigned int)nR, hand);
                const int nG = rig_find_node(brig, kFakeGunRootHash);
                groot_ok = nG >= 0 &&
                           read_bone_world(body, (unsigned int)nG, groot);
            }
        }
        // The acquire gate is what the census measured, with room to spare; the
        // hold gate is looser so a single noisy frame cannot drop the lock.
        // Without the gun root we fall back to the old hand distance for the
        // census display, but the substitution target is NOT published in that
        // case (see below): writing the wrong weapon is worse than writing none.
        const float kAcquireD = groot_ok ? 0.10f : 1.00f;
        const float kHoldD    = groot_ok ? 0.25f : 1.00f;
        uint64_t best = 0, best_rig = 0;
        float    best_d = 1e9f;
        uint16_t best_bones = 0;
        float    best_o[3] = {0.0f, 0.0f, 0.0f};   // the pick's instance origin
        int      cands = 0;
        // Build 69: the lock's own reading this tick, so a hold can present the
        // lock's real distance and rig rather than the argmax's.
        uint64_t lock_rig = 0;
        float    lock_d = 1e9f;
        uint16_t lock_bones = 0;
        float    lock_o[3] = {0.0f, 0.0f, 0.0f};
        for (uint32_t i = 0; i < kRingSize; ++i) {
            const uint64_t p = g_skel_ring[i].load(std::memory_order_relaxed);
            if (!p || p == body) continue;
            uint64_t rig = 0;
            if (!read_block(p + 0x220, &rig, sizeof(rig)) ||
                rig < 0x10000 || (rig & 7)) continue;
            uint16_t bones = 0;
            if (!read_block(rig + 0x8A, &bones, sizeof(bones))) continue;
            if (bones < 2 || bones > 64) continue;         // humanoids are 100+
            if (rig_has_hash(rig, 0x07C159A2u)) continue;  // has a Head: a body
            float o[3];
            if (!read_block(p + 0x120, o, sizeof(o)) ||
                !isfinite(o[0]) || !isfinite(o[1]) || !isfinite(o[2]))
                continue;
            const float ref[3] = {
                groot_ok ? groot[0] : hand_ok ? hand[0] : g_base_pos[0],
                groot_ok ? groot[1] : hand_ok ? hand[1] : g_base_pos[1],
                groot_ok ? groot[2] : hand_ok ? hand[2] : g_base_pos[2]};
            const float dx = o[0] - ref[0], dy = o[1] - ref[1],
                        dz = o[2] - ref[2];
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > 6.0f) continue;                        // census radius
            ++cands;
            if ((ticks % 5) == 1 && cands <= 8)
                LOG_INFO("wskel: cand 0x%012llX rig=0x%012llX bones=%u "
                         "d%s=%.2fm pos=(%.2f %.2f %.2f)",
                         (unsigned long long)p, (unsigned long long)rig,
                         bones, hand_ok ? "hand" : "cam", d,
                         o[0], o[1], o[2]);
            if (p == g_wskel_lock) {         // the lock, if it is still alive
                lock_d = d; lock_rig = rig; lock_bones = bones;
                lock_o[0] = o[0]; lock_o[1] = o[1]; lock_o[2] = o[2];
            }
            if (d < best_d) {
                best_d = d; best = p; best_rig = rig; best_bones = bones;
                best_o[0] = o[0]; best_o[1] = o[1]; best_o[2] = o[2];
            }
        }

        // BUILD 69: THE LOCK. A per-tick argmax has no memory, so it retargets
        // on any tick where noise reorders the candidates, and every retarget
        // is a visible snap: the held weapon reverts to the engine's placement
        // while some other weapon takes our controller ray. This is the same
        // failure and the same remedy as the main-camera lock in the 6DOF
        // Master Reference section 21: hold the current pick while it remains
        // plausible, and require a challenger to win clearly and repeatedly
        // before switching. On a single-weapon frame it is a no-op.
        const char* lock_what = "acquire";
        if (g_wskel_lock && lock_d <= kHoldD) {
            // The lock is alive and still plausible. Only a decisively closer
            // challenger, sustained, may take it.
            if (best && best != g_wskel_lock && best_d < lock_d * 0.5f) {
                if (best == g_wskel_chal) ++g_wskel_chal_hits;
                else { g_wskel_chal = best; g_wskel_chal_hits = 1; }
            } else {
                g_wskel_chal = 0; g_wskel_chal_hits = 0;
            }
            if (g_wskel_chal_hits >= kLockSwitchTicks) {
                g_wskel_lock = g_wskel_chal;
                g_wskel_chal = 0; g_wskel_chal_hits = 0;
                lock_what = "SWITCHED";
            } else {
                // Hold: present the lock as the pick, not the tick's argmax.
                best = g_wskel_lock; best_d = lock_d; best_rig = lock_rig;
                best_bones = lock_bones;
                best_o[0] = lock_o[0]; best_o[1] = lock_o[1]; best_o[2] = lock_o[2];
                lock_what = "held";
            }
        } else if (best && best_d <= kAcquireD) {
            g_wskel_lock = best;
            g_wskel_chal = 0; g_wskel_chal_hits = 0;
        } else {
            g_wskel_lock = 0;
            g_wskel_chal = 0; g_wskel_chal_hits = 0;
            lock_what = "none";
        }

        if (best && best_d < 1.0f) {
            g_wskel_cand.store(best, std::memory_order_relaxed);
            LOG_INFO("wskel: PICK 0x%012llX rig=0x%012llX bones=%u d%s=%.3fm "
                     "lock=%s (WHITE marker rides it)",
                     (unsigned long long)best, (unsigned long long)best_rig,
                     best_bones,
                     groot_ok ? "groot" : hand_ok ? "hand" : "cam",
                     best_d, lock_what);

            // Build 68: publish the weapon's TRANSFORM NODE as the
            // substitution target, re-derived every tick and never cached.
            // Both FRIK and UEVR report stale-handle bugs at exactly this
            // point (a weapon node stays non-null while invalid, and a swap
            // invalidates the object outright), so identity is resolved
            // fresh: node = [[weapon instance + 0x10] + 0x18], the owner
            // entity's transform node.
            //
            // BUILD 69: FAIL CLOSED. Selection happens here at 1 Hz; the
            // substitution runs 144 times a second on the engine thread and
            // trusts this pointer completely. So the target is published ONLY
            // when the pick is gun-root verified: no gun root, or a pick that
            // is not sitting on it, means no target and the engine keeps its
            // own placement. A run that writes nothing is a bad run; a run that
            // writes the controller ray onto the rifle on your back is a bug
            // report, and this is the difference between them.
            if (g_wnode_hook.installed()) {
                uint64_t ent = 0, nd = 0;
                const bool verified = groot_ok && best_d <= kAcquireD;
                if (verified &&
                    g_wnode_mode.load(std::memory_order_relaxed) > 0 &&
                    read_block(best + 0x10, &ent, sizeof(ent)) &&
                    ent > 0x10000 && !(ent & 7) &&
                    read_block(ent + 0x18, &nd, sizeof(nd)) &&
                    nd > 0x10000 && !(nd & 7)) {
                    g_wnode_target.store(nd, std::memory_order_relaxed);
                } else {
                    g_wnode_target.store(0, std::memory_order_relaxed);
                    if (!verified)
                        LOG_INFO("wnode: target WITHHELD (%s, d=%.3fm). The "
                                 "engine keeps its own placement.",
                                 groot_ok ? "pick is not on the gun root"
                                          : "no Fake_gunroot on the player rig",
                                 best_d);
                }
                // BUILD 70: the census report. Every node the engine placed
                // this run, ranked by how close it sits to the player's
                // Fake_gunroot. The held weapon's node is the one ON the gun
                // root being placed about twice a frame; a shared parent will
                // show a much larger hit count, and the wrong object will not
                // be on the bone at all. Reading this replaces the pointer
                // chain guess with a measurement.
                if (g_cen_on.load(std::memory_order_relaxed) && groot_ok) {
                    // Republish the reference the hook gates on. Once a second
                    // is enough: the gate is 1.5 m wide.
                    g_cen_ref[0].store(groot[0], std::memory_order_relaxed);
                    g_cen_ref[1].store(groot[1], std::memory_order_relaxed);
                    g_cen_ref[2].store(groot[2], std::memory_order_relaxed);
                    g_cen_ref_ok.store(1, std::memory_order_relaxed);

                    struct Row { uint64_t n; uint32_t h; float d; };
                    Row top[16] = {};
                    int  ntop = 0, live = 0;
                    for (uint32_t i = 0; i < kCenSlots; ++i) {
                        const uint64_t n =
                            g_cen[i].node.load(std::memory_order_relaxed);
                        if (!n) continue;
                        ++live;
                        const float d = g_cen[i].dmin;
                        if (!isfinite(d)) continue;
                        Row r{n, g_cen[i].hits.load(std::memory_order_relaxed), d};
                        int at = ntop;
                        while (at > 0 && top[at-1].d > r.d) {
                            if (at < 16) top[at] = top[at-1];
                            --at;
                        }
                        if (at < 16) { top[at] = r; if (ntop < 16) ++ntop; }
                    }
                    // Saturation is stated out loud. Build 70 filled 509 of 512
                    // slots and said nothing, so its ranking was read as "the
                    // nearest" when it was "the nearest of what fitted".
                    const uint32_t seen = g_cen_seen.load(std::memory_order_relaxed);
                    const uint32_t rej  = g_cen_rejected.load(std::memory_order_relaxed);
                    LOG_INFO("wcen: %d/%u slots used%s | in-radius %u, rejected "
                             "%u (radius %.2fm) | groot %.2f %.2f %.2f",
                             live, kCenSlots,
                             live > (int)(kCenSlots * 9 / 10)
                                 ? "  *** SATURATED, ranking is NOT trustworthy ***"
                                 : "",
                             seen, rej,
                             g_cen_radius.load(std::memory_order_relaxed),
                             groot[0], groot[1], groot[2]);
                    for (int i = 0; i < ntop; ++i)
                        LOG_INFO("wcen:   node=0x%012llX  dmin=%.4fm  hits=%u%s",
                                 (unsigned long long)top[i].n, top[i].d,
                                 top[i].h,
                                 top[i].n == g_wnode_target.load(
                                     std::memory_order_relaxed)
                                     ? "   <<< build 68's target" : "");

                    // BUILD 72: pick the anchor. It must be the part nearest
                    // the gun root that is STILL BEING PLACED: the node that
                    // sits exactly on the bone stops after its 333 attach-time
                    // calls, and anchoring to a node the engine has finished
                    // with would freeze the pivot at wherever the player was
                    // standing when he drew. "Still being placed" is measured
                    // against the previous tick, not assumed.
                    // BUILD 73: the rate threshold. Build 72 accepted any node
                    // whose hit count moved at all, so the attach-time node at
                    // 0.0006 m kept winning on distance with 23 and then 54
                    // placements, and the pivot it gave was a point the engine
                    // had finished updating. A real weapon part is placed twice
                    // a frame, about 144 times a second. Demanding a third of
                    // that separates a part the gun is made of from an artefact
                    // of drawing it, and does so by measurement.
                    constexpr uint32_t kAnchorMinRate = 48;
                    uint64_t anchor = 0;
                    float    anchor_d = 1e9f;
                    uint32_t anchor_rate = 0;
                    uint32_t best_rate_seen = 0;
                    // BUILD 75: hold the incumbent. Build 74 re-elected purely
                    // on distance every tick and alternated between two nodes
                    // 6 cm apart, and every alternation moves the pivot the
                    // whole cluster rotates about. An anchor that still
                    // qualifies keeps the job; only one that stops qualifying
                    // is replaced. The pivot's job is to be the same point from
                    // one frame to the next, not the best point.
                    const uint64_t incumbent =
                        g_wanchor.load(std::memory_order_relaxed);
                    uint64_t inc_seen = 0;
                    float    inc_d = 1e9f;
                    uint32_t inc_rate = 0;
                    for (uint32_t i = 0; i < kCenSlots; ++i) {
                        const uint64_t n =
                            g_cen[i].node.load(std::memory_order_relaxed);
                        if (!n) continue;
                        const uint32_t h =
                            g_cen[i].hits.load(std::memory_order_relaxed);
                        const uint32_t rate = h - g_cen[i].prev;
                        g_cen[i].prev = h;
                        if (rate > best_rate_seen) best_rate_seen = rate;
                        if (n == incumbent) {
                            inc_seen = n; inc_d = g_cen[i].dmin; inc_rate = rate;
                        }
                        if (rate < kAnchorMinRate) continue;
                        if (g_cen[i].dmin < anchor_d) {
                            anchor_d    = g_cen[i].dmin;
                            anchor      = n;
                            anchor_rate = rate;
                        }
                    }
                    if (inc_seen && inc_rate >= kAnchorMinRate && inc_d <= 0.40f) {
                        anchor = inc_seen; anchor_d = inc_d; anchor_rate = inc_rate;
                    }
                    if (anchor && anchor_d <= 0.40f) {
                        if (g_wanchor.exchange(anchor, std::memory_order_relaxed)
                                != anchor) {
                            // A new anchor's pivot is NOT usable until the hook
                            // has actually seen it placed. Otherwise the first
                            // frames after a switch rotate the cluster about
                            // the previous anchor's position.
                            g_wanchor_ok.store(0, std::memory_order_relaxed);
                            LOG_INFO("wanchor: 0x%012llX at %.4fm from the gun "
                                     "root, %u placements/s. Parts within %.2fm "
                                     "of it rotate with the gun.",
                                     (unsigned long long)anchor, anchor_d,
                                     anchor_rate,
                                     g_wnode_radius.load(
                                         std::memory_order_relaxed));
                        }
                    } else {
                        if (g_wanchor.exchange(0, std::memory_order_relaxed))
                            LOG_INFO("wanchor: LOST. Nothing within 0.40m of the "
                                     "gun root is being placed at least %u times "
                                     "a second (fastest seen %u/s). Mode 3 "
                                     "writes nothing until it is back.",
                                     kAnchorMinRate, best_rate_seen);
                        g_wanchor_ok.store(0, std::memory_order_relaxed);
                    }

                    // BUILD 74: latch the barrel axis once the scores separate.
                    // Requires both a decent sample count and a clear margin,
                    // so a coincidence during a few seconds of standing still
                    // cannot latch the wrong axis permanently.
                    if (g_axis_idx.load(std::memory_order_relaxed) < 0) {
                        const uint32_t ns =
                            g_axis_samples.load(std::memory_order_relaxed);
                        int   win = 0;
                        float w1 = -1e30f, w2 = -1e30f;
                        for (int i = 0; i < 6; ++i) {
                            const float s = g_axis_score[i];
                            if (s > w1) { w2 = w1; w1 = s; win = i; }
                            else if (s > w2) { w2 = s; }
                        }
                        if (ns >= 200 && w1 > 0.0f && w1 > w2 * 1.30f) {
                            g_axis_idx.store(win, std::memory_order_relaxed);
                            static const char* kName[6] =
                                {"+row0", "+row1", "+row2",
                                 "-row0", "-row1", "-row2"};
                            LOG_INFO("waxis: the barrel is %s of the weapon "
                                     "part basis (score %.1f vs %.1f over %u "
                                     "samples). Hip fire now rotates from the "
                                     "GUN's forward, not the camera's.",
                                     kName[win], w1, w2, ns);
                        } else if (ns >= 600) {
                            LOG_INFO("waxis: undecided after %u samples "
                                     "(best %.1f vs %.1f). Still using the "
                                     "camera forward, which is right in ADS "
                                     "and wrong in hip fire.", ns, w1, w2);
                            g_axis_samples.store(0, std::memory_order_relaxed);
                            for (int i = 0; i < 6; ++i) g_axis_score[i] = 0.0f;
                        }
                    }

                    // One line that says WHY nothing happened, if nothing did.
                    LOG_INFO("wgate: subs=%llu | noanchor=%llu stale=%llu "
                             "outside=%llu noview=%llu noray=%llu",
                             (unsigned long long)g_wnode_hits,
                             (unsigned long long)g_gate_noanchor,
                             (unsigned long long)g_gate_stale,
                             (unsigned long long)g_gate_outside,
                             (unsigned long long)g_gate_noview,
                             (unsigned long long)g_gate_noray);
                }

                LOG_INFO("wnode: mode=%d target=0x%012llX calls=%llu subs=%llu",
                         g_wnode_mode.load(std::memory_order_relaxed),
                         (unsigned long long)g_wnode_target.load(
                             std::memory_order_relaxed),
                         (unsigned long long)grwxr_wnode_calls,
                         (unsigned long long)g_wnode_hits);
            }

            // Build 66, READ-ONLY half (2026-08-09). The user's goal is the
            // GAME'S OWN weapon riding the controller, so the question is
            // which field actually carries the rendered gun. Build 65 already
            // proved the instance origin (+0x120/+0x250) is bookkeeping: 1680
            // identity-checked writes moved nothing. The next consumer down,
            // read off read_bone_world's own verified layout, is the pose
            // ROOT: one rigid transform that carries the whole bone buffer to
            // world, which is the "last consumer" shape.
            //
            // This logs it and nothing else. The GO/NO-GO for the write build
            // is flags bit 26: if it is SET the bone buffer is already in
            // world space and the root is NOT the carrier, so a write there
            // would be pointless and the plan says stop. Printing rootT
            // beside the instance origin also shows at a glance whether the
            // two agree, which tells us if they are even the same object's
            // idea of where the gun is.
            uint64_t wpose = 0;
            if (read_block(best + 0x238, &wpose, sizeof(wpose)) &&
                wpose >= 0x10000 && !(wpose & 7)) {
                float    rt[4] = {}, rq[4] = {};
                uint32_t wflags = 0;
                uint64_t wbuf = 0;
                const bool okt = read_block(wpose + 0x00, rt, sizeof(rt));
                const bool okq = read_block(wpose + 0x10, rq, sizeof(rq));
                read_block(wpose + 0x8C, &wflags, sizeof(wflags));
                read_block(wpose + 0x178, &wbuf, sizeof(wbuf));
                const float qn = sqrtf(rq[0] * rq[0] + rq[1] * rq[1] +
                                       rq[2] * rq[2] + rq[3] * rq[3]);
                LOG_INFO("wpose: pose=0x%012llX buf=0x%012llX flags=0x%08X "
                         "bit26=%s%s | rootT=(%.2f %.2f %.2f) "
                         "rootQ=(%.3f %.3f %.3f %.3f) |q|=%.3f | inst=(%.2f "
                         "%.2f %.2f) d(rootT,inst)=%.2fm",
                         (unsigned long long)wpose, (unsigned long long)wbuf,
                         wflags, (wflags & 0x04000000u) ? "SET" : "clear",
                         (wflags & 0x04000000u)
                             ? "  <<< STOP: buffer is WORLD space, the root is "
                               "NOT the carrier"
                             : "  (model space: the root IS the carrier, write "
                               "test is GO)",
                         okt ? rt[0] : 0.0f, okt ? rt[1] : 0.0f,
                         okt ? rt[2] : 0.0f,
                         okq ? rq[0] : 0.0f, okq ? rq[1] : 0.0f,
                         okq ? rq[2] : 0.0f, okq ? rq[3] : 0.0f, qn,
                         best_o[0], best_o[1], best_o[2],
                         okt ? sqrtf((rt[0] - best_o[0]) * (rt[0] - best_o[0]) +
                                     (rt[1] - best_o[1]) * (rt[1] - best_o[1]) +
                                     (rt[2] - best_o[2]) * (rt[2] - best_o[2]))
                             : -1.0f);

                // BUILD 219: THE GRIP BONE, read from the weapon's OWN rig.
                // wb-HandRight's model-space record is the grip's offset from
                // wb-gunroot (bone 0, at the origin). Both records are read;
                // the root's must sit at the origin, which proves the layout
                // and rules out a world-space buffer in one test. Refused,
                // never transformed, on any doubt: the anchor then falls
                // back to the cfg offsets alone, which is build 218.
                {
                    static uint64_t s_gb_rig = 0;
                    static int      s_gb_ok  = -1;
                    static float    s_gb[3]  = {0.0f, 0.0f, 0.0f};
                    float gt[4] = {}, gq[4] = {}, rt0[4] = {};
                    int  nG = -1, nR = -1;
                    bool ok = false;
                    const bool want =
                        g_wgun_grip_bone.load(std::memory_order_relaxed) != 0;
                    if (want && wbuf >= 0x10000 && !(wbuf & 7) && best_rig) {
                        nG = rig_find_node(best_rig, 0x1BD2DF4Eu);   // wb-HandRight
                        nR = rig_find_node(best_rig, 0xE3B52940u);   // wb-gunroot
                        if (nG >= 0 && nR >= 0 &&
                            read_block(wbuf + (uint64_t)nG * 0x20, gt, sizeof(gt)) &&
                            read_block(wbuf + (uint64_t)nG * 0x20 + 0x10, gq,
                                       sizeof(gq)) &&
                            read_block(wbuf + (uint64_t)nR * 0x20, rt0, sizeof(rt0))) {
                            const float rr = sqrtf(rt0[0] * rt0[0] + rt0[1] * rt0[1] +
                                                   rt0[2] * rt0[2]);
                            const float gr = sqrtf(gt[0] * gt[0] + gt[1] * gt[1] +
                                                   gt[2] * gt[2]);
                            ok = isfinite(rr) && isfinite(gr) && rr < 0.02f &&
                                 gr > 0.03f && gr < 1.0f &&
                                 !(wflags & 0x04000000u);
                        }
                    }
                    // BUILD 220: HOLD, NEVER DROP. The 219 run jumped to the stock
                    // whenever this 1 Hz pick fell off the held weapon for a
                    // tick: the census lost the FAL 33 times (no pick, offset
                    // cleared) and six times fell onto the holstered P227 at
                    // 0.77 m from the gun root (the pistol's offset on the
                    // rifle). A NEW offset is taken only from a pick that sits ON
                    // the gun root, which is the held weapon by the mount
                    // identity (0.002 m, 2026-08-09); a far pick, a lost pick or
                    // an unreadable record keeps the last APPLIED offset, and a
                    // real swap replaces it within a tick because the new weapon
                    // arrives on the root. Only the switch clears it.
                    const bool onroot = groot_ok && best_d <= 0.10f;   // kAcquireD; the FAL read up to 0.046 on-root (review 3)
                    // BUILD 222: on the root = drawn, at once; far = drawn only
                    // after two far ticks in a row (a lock=none flicker onto the
                    // holstered pistol lasts one tick, a stow lasts).
                    g_pip_none_ticks = 0;
                    if (onroot) {
                        g_pip_far_ticks = 0;
                        g_pip_drawn.store(1, std::memory_order_relaxed);
                    } else if (++g_pip_far_ticks >= 2) {
                        g_pip_drawn.store(0, std::memory_order_relaxed);
                    }
                    int state = 0;   // 0 OFF, 1 APPLIED, 2 HELD far pick, 3 HELD unreadable
                    if (!want) {
                        g_wgun_grip_bone_ok.store(0, std::memory_order_relaxed);
                    } else if (ok && onroot) {
                        g_wgun_grip_bone_x.store(gt[0], std::memory_order_relaxed);
                        g_wgun_grip_bone_y.store(gt[1], std::memory_order_relaxed);
                        g_wgun_grip_bone_z.store(gt[2], std::memory_order_relaxed);
                        g_wgun_grip_bone_ok.store(1, std::memory_order_release);  // review 2: pairs with the acquire in the anchor
                        state = 1;
                    } else {
                        state = onroot ? 3 : 2;   // held: the published offset stands
                    }
                    g_gb_nopick_logged = false;
                    // BUILD 221: the eyepiece, wb-AimingPoint, from the same
                    // buffer, same gate (a pick ON the gun root), same hold.
                    // Absent on this rig = the cfg fallback stands; logged once
                    // per rig either way.
                    {
                        static uint64_t s_pa_rig = 0;
                        static int      s_pa_state = -1;
                        float at[4] = {};
                        int nA = -1, pstate = 0;   // 0 held/off, 1 APPLIED, 2 ABSENT
                        if (g_pip_on.load(std::memory_order_relaxed) && onroot &&
                            wbuf >= 0x10000 && !(wbuf & 7) && best_rig) {
                            nA = rig_find_node(best_rig, 0x90AAC2D0u);   // wb-AimingPoint
                            if (nA >= 0 &&
                                read_block(wbuf + (uint64_t)nA * 0x20, at, sizeof(at)) &&
                                isfinite(at[0]) && isfinite(at[1]) && isfinite(at[2]) &&
                                !(wflags & 0x04000000u)) {
                                const float ar = sqrtf(at[0] * at[0] + at[1] * at[1] + at[2] * at[2]);
                                if (ar > 0.03f && ar < 1.5f) {
                                    g_pip_aim_x.store(at[0], std::memory_order_relaxed);
                                    g_pip_aim_y.store(at[1], std::memory_order_relaxed);
                                    g_pip_aim_z.store(at[2], std::memory_order_relaxed);
                                    g_pip_aim_ok.store(1, std::memory_order_release);
                                    pstate = 1;
                                }
                            } else if (nA < 0) {
                                // This rig has no aiming point: the cfg fallback
                                // must stand, so the previous weapon's bone is dropped.
                                g_pip_aim_ok.store(0, std::memory_order_relaxed);
                                pstate = 2;
                            }
                        }
                        if (pstate != 0 && (best_rig != s_pa_rig || pstate != s_pa_state)) {
                            s_pa_rig = best_rig; s_pa_state = pstate;
                            LOG_INFO("pip: wb-AimingPoint node=%d model=(%.3f %.3f %.3f) -> %s "
                                     "(build 221: the eyepiece the PiP scope draws on; "
                                     "ABSENT = the cfg pip_lat/fwd/up fallback)",
                                     nA, at[0], at[1], at[2],
                                     pstate == 1 ? "APPLIED" : "ABSENT");
                        }
                    }
                    const bool changed =
                        best_rig != s_gb_rig || state != s_gb_ok ||
                        fabsf(gt[0] - s_gb[0]) > 0.01f ||
                        fabsf(gt[1] - s_gb[1]) > 0.01f ||
                        fabsf(gt[2] - s_gb[2]) > 0.01f;
                    if (changed) {
                        s_gb_rig = best_rig; s_gb_ok = state;
                        s_gb[0] = gt[0]; s_gb[1] = gt[1]; s_gb[2] = gt[2];
                        char st[64];
                        if (state == 1)      strcpy_s(st, "APPLIED");
                        else if (state == 2) sprintf_s(st, "HELD (pick %.2f m off the gun root)", best_d);
                        else if (state == 3) strcpy_s(st, "HELD (record unreadable)");
                        else                 strcpy_s(st, "OFF");
                        // What the anchor is USING now, not what this tick read.
                        const bool pub = g_wgun_grip_bone_ok.load(std::memory_order_relaxed) != 0;
                        const float ax = pub ? -g_wgun_grip_bone_x.load(std::memory_order_relaxed) : 0.0f;
                        const float ay = pub ? -g_wgun_grip_bone_y.load(std::memory_order_relaxed) : 0.0f;
                        const float az = pub ? -g_wgun_grip_bone_z.load(std::memory_order_relaxed) : 0.0f;
                        LOG_INFO("grip: wb-HandRight node=%d wb-gunroot node=%d "
                                 "model=(%.3f %.3f %.3f) q=(%.3f %.3f %.3f %.3f) "
                                 "root=(%.3f %.3f %.3f) bit26=%s -> %s anchor "
                                 "lat=%.3f fwd=%.3f up=%.3f (build 219: the root "
                                 "moves by minus the grip so the grip lands on the "
                                 "controller; check against "
                                 "GRIP-2026-09-05-wb-handright.tsv)",
                                 nG, nR, gt[0], gt[1], gt[2],
                                 gq[0], gq[1], gq[2], gq[3],
                                 rt0[0], rt0[1], rt0[2],
                                 (wflags & 0x04000000u) ? "SET" : "clear",
                                 st, ax, ay, az);
                    }
                }
            } else {
                // BUILD 220: the grip offset is HELD here (see the grip block).
                // BUILD 222: a pick without a pose counts as an empty tick.
                if (++g_pip_none_ticks >= 4) g_pip_drawn.store(0, std::memory_order_relaxed);
                LOG_INFO("wpose: no pose at [pick+0x238] (read 0x%012llX). The "
                         "weapon rig does not carry a final pose here; the "
                         "root-write route does not apply to it.",
                         (unsigned long long)wpose);
            }
        } else {
            g_wskel_cand.store(0, std::memory_order_relaxed);
            // BUILD 69: losing the pick must also drop the substitution target.
            // Without this the engine thread keeps writing a node we no longer
            // believe in, at 144 a second, which is exactly the stale-handle
            // bug FRIK and UEVR both report at this point.
            g_wnode_target.store(0, std::memory_order_relaxed);
            // BUILD 222: held across empty ticks; four in a row means the weapon
            // is really gone (a vehicle stows it, and the census then finds it
            // far anyway, which clears in two).
            if (++g_pip_none_ticks >= 4) g_pip_drawn.store(0, std::memory_order_relaxed);
            // BUILD 220: the grip offset is HELD across a lost pick; the held
            // weapon did not change because the census blinked. Logged once
            // per loss so the line count stays honest.
            if (!g_gb_nopick_logged &&
                g_wgun_grip_bone_ok.load(std::memory_order_relaxed)) {
                LOG_INFO("grip: HELD (no pick this tick), the last APPLIED offset stands (build 220)");
            }
            g_gb_nopick_logged = true;
            LOG_INFO("wskel: no pick (cands=%d best=%.2fm ref=%s), wnode target "
                     "cleared",
                     cands, best ? best_d : -1.0f,
                     groot_ok ? "groot" : hand_ok ? "hand" : "cam");
        }

        // Build 65: publish or clear the write target. Rig first, target
        // second (the writer reads target then rig; the identity compare
        // makes the race benign). Drawn-gun-only gate: 0.4 m of the hand.
        if (g_wskel_write_on.load(std::memory_order_relaxed)) {
            const uint64_t w =
                g_wskel_writes.load(std::memory_order_relaxed);
            if (best && best_d < 0.4f && hand_ok && w < kWskelWriteCap) {
                g_wskel_tgt_rig.store(best_rig, std::memory_order_relaxed);
                g_wskel_tgt.store(best, std::memory_order_relaxed);
                LOG_INFO("wskelw: ARMED on 0x%012llX writes=%llu target=%s",
                         (unsigned long long)best, (unsigned long long)w,
                         g_wskel_mode.load(std::memory_order_relaxed) >= 2
                             ? "POSE ROOT [[pick+0x238]+0x08]"
                             : "instance origin +0x120/+0x250");
            } else {
                g_wskel_tgt.store(0, std::memory_order_relaxed);
                LOG_INFO("wskelw: not armed (%s, writes=%llu)",
                         w >= kWskelWriteCap ? "CAP reached, toggle to reset"
                         : !best             ? "no pick"
                         : best_d >= 0.4f    ? "pick not in hand"
                                             : "hand absent",
                         (unsigned long long)w);
            }
        } else {
            g_wskel_tgt.store(0, std::memory_order_relaxed);
        }
    }

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
        // BUILD 131: pick the player entity that actually OWNS A BODY IN THE
        // RING, instead of trusting whichever component initialised last.
        //
        // Every candidate component is turned into an entity, and an entity is
        // only accepted if the ring holds a skeleton it owns whose rig
        // contains a Head bone. The Head-bone test is what separates the BODY
        // from the gear rigs: one character is a cluster of skeleton
        // instances, and only the body has a head. A candidate that fails is
        // not the player no matter how recently it initialised.
        //
        // If nothing resolves this tick, the previous entity is KEPT. Losing
        // the pin is what puts the camera on the chase rig, so the pin is only
        // ever replaced, never cleared, by this path.
        {
            uint64_t best_ent = 0, best_skel = 0;
            uint32_t best_node = 0xFFFFu;
            for (int i = 0; i < kSkelPtrs && !best_ent; ++i) {
                const uint64_t pc = g_pc_tab[i].load(std::memory_order_relaxed);
                if (!pc) continue;
                uint64_t ent = 0;
                if (!read_block(pc + 0x10, &ent, sizeof(ent)) ||
                    ent <= 0x10000 || (ent & 7))
                    continue;
                for (uint32_t r = 0; r < kRingSize; ++r) {
                    const uint64_t p =
                        g_skel_ring[r].load(std::memory_order_relaxed);
                    if (!p) continue;
                    uint64_t own = 0;
                    if (!read_block(p + 0x10, &own, sizeof(own)) || own != ent)
                        continue;
                    uint64_t rig = 0;
                    if (!read_block(p + 0x220, &rig, sizeof(rig)) ||
                        rig < 0x10000 || (rig & 7))
                        continue;
                    const int node = rig_find_node(rig, 0x07C159A2u);
                    if (node < 0) continue;   // gear rig, no Head: not the body
                    best_ent  = ent;
                    best_skel = p;
                    best_node = (uint32_t)node;
                    break;
                }
            }
            if (best_ent) {
                const uint64_t was =
                    g_player_entity.exchange(best_ent, std::memory_order_relaxed);
                headpose::set_player_obj(best_skel);
                headpose::set_head_node(best_node);
                if (was != best_ent) {
                    // BUILD 132, CX-001: count the churn and name the thing.
                    const uint32_t nch =
                        g_pin_changes.fetch_add(1, std::memory_order_relaxed) + 1;
                    const uint64_t nowms = (uint64_t)GetTickCount64();
                    uint64_t expect0 = 0;
                    g_pin_first_ms.compare_exchange_strong(
                        expect0, nowms, std::memory_order_relaxed);
                    // One guarded 8-byte read. Resolve the class OFFLINE with
                    // tools/anvil_symbols.py; never look up a symbol in process.
                    uint64_t vt = 0;
                    read_block(best_ent, &vt, sizeof(vt));
                    LOG_INFO("fp: player ENTITY 0x%012llX PINNED, body "
                             "skeleton 0x%012llX, Head node %u (chosen from "
                             "the component table because it owns a rig with "
                             "a head, build 131) | pinchg=%u vtable=0x%012llX "
                             "t=%llums",
                             (unsigned long long)best_ent,
                             (unsigned long long)best_skel, best_node,
                             nch, (unsigned long long)vt,
                             (unsigned long long)nowms);
                    if (nch > 1)
                        LOG_INFO("fp: PIN CHANGED (%u distinct entities so "
                                 "far). Was 0x%012llX, now 0x%012llX. Churn "
                                 "here is the owner's 'trying to decide what "
                                 "it attaches to'; one change at load is "
                                 "normal, many is not.",
                                 nch, (unsigned long long)was,
                                 (unsigned long long)best_ent);
                }
            } else if (const uint64_t pc =
                           g_playercomp.load(std::memory_order_relaxed)) {
                // Nothing in the ring this tick. Keep whatever is pinned and
                // publish the entity only, exactly as before this build.
                uint64_t ent = 0;
                if (read_block(pc + 0x10, &ent, sizeof(ent)) &&
                    ent > 0x10000 && !(ent & 7)) {
                    const uint64_t was = g_player_entity.exchange(
                        ent, std::memory_order_relaxed);
                    if (was != ent)
                        LOG_INFO("fp: player ENTITY 0x%012llX (from "
                                 "cPlayerComponent 0x%012llX)",
                                 (unsigned long long)ent,
                                 (unsigned long long)pc);
                }
            }
        }
        const uint64_t pent = g_player_entity.load(std::memory_order_relaxed);
        // ---------------------------------------------------------------
        // CX-001, HALF ONE: this selection needs a REAL camera position.
        //
        // It used to read g_base_pos directly. That global is written in
        // exactly one place, write_pose_head at :1921, which runs only once
        // VR is armed and publishing, so before then it is {0,0,0} and the
        // loop below ranks the player's skeletons by distance to the WORLD
        // ORIGIN instead of to the camera. The `own != pent` filter means
        // the winner is still one of the player's own skeletons, so this
        // never grabbed a helicopter, but the ranking it performs is
        // meaningless: the parts sit within a couple of metres of each
        // other and several thousand metres from the origin, so the choice
        // between body, weapon and attachment becomes arbitrary.
        //
        // base_pos() prefers the engine's own camera object (g_player_cam,
        // stored on EVERY mode-0 on_calc_mvp call with no VR precondition)
        // and range-checks the read, so it answers correctly in flat play
        // long before VR arms. It returns whether the value is real, and
        // that boolean is honoured here rather than ignored.
        // ---------------------------------------------------------------
        float camp[3] = {0, 0, 0};
        const bool camp_ok = base_pos(camp);
        if (pent && !camp_ok) {
            // No camera means no basis for ranking. KEEP the existing pin
            // rather than picking by an unknown reference: a stale correct
            // pin beats a fresh arbitrary one. Logged on transition only,
            // because this drain runs at 1 Hz and a per-tick line would
            // bury the run.
            static bool said = false;
            if (!said) {
                said = true;
                LOG_WARN("fp: no camera position available, so the skeleton "
                         "pin is NOT re-ranked this tick. Keeping the "
                         "existing pin (CX-001).");
            }
        } else if (pent) {
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
                const float dx = o[0] - camp[0];
                const float dy = o[1] - camp[1];
                const float d  = sqrtf(dx * dx + dy * dy);
                if (d < bestd) {
                    bestd = d;
                    bestp = p;
                    memcpy(bpos, o, sizeof(bpos));
                }
            }
            // BUILD 168: THE RING IS NOT THE PIN. The 128-slot ring holds the
            // skeletons the engine updated most recently, and in a crowded
            // place the player's is evicted before this tick reads it: every
            // run on 2026-09-02 after the owner moved into a base printed "no
            // ring member" on every check, while the 17:38 run in the open
            // resolved regularly (42 misses, 33 re-arms in ten minutes). A
            // miss here used to skip the whole hand census, so the stub kept
            // a stale skeleton pointer and the hands died seconds after
            // arming, four runs with the same shape, and every one of them
            // was read as a test error. Hold the last validated body skeleton
            // instead: it is still the pin while its owner field names this
            // entity and its position reads. The ring only ever CHOSE the
            // skeleton; it was never the proof that it still exists.
            if (!bestp) {
                const uint64_t held = headpose::player_obj();
                uint64_t own = 0;
                float o[3];
                if (held > 0x10000 && !(held & 7) &&
                    read_block(held + 0x10, &own, sizeof(own)) && own == pent &&
                    read_block(held + 0x120, o, sizeof(o)) &&
                    isfinite(o[0]) && isfinite(o[1]) && isfinite(o[2])) {
                    bestp = held;
                    memcpy(bpos, o, sizeof(bpos));
                    const float dx = o[0] - camp[0], dy = o[1] - camp[1];
                    bestd = sqrtf(dx * dx + dy * dy);
                    if ((ticks % 10) == 3)
                        LOG_INFO("fp: pin(entity) ring miss, holding "
                                 "0x%012llX by owner check (build 168); the "
                                 "hand census runs on it",
                                 (unsigned long long)held);
                }
            }
            if (bestp) {
                headpose::set_player_obj(bestp);

                // BUILD 225: fp_auto, the SWOOP plan's route 3
                // (SWOOP-2026-09-05-fp-auto-plan.md). Nothing in the mod ever
                // asked for first person by itself; that was the defect. The
                // pin is valid here (bestp) and bestd is the horizontal metres
                // from the engine camera to the body; the anchor's own gate is
                // 12 m, so the same number keeps one threshold. The request is
                // the F3 calibration's existing route (g_fp_request, consumed
                // in VRMirror's F2 branch, forces ON, brings the recenter),
                // re-armed at most every ten seconds so a deliberate F2 off
                // is not fought frame by frame, and so a death or a vehicle
                // puts him back without a key.
                {
                    // A deliberate F2 off is honoured until the player pin
                    // changes (a death, a world change), so the user is not
                    // fought ten seconds later; a new pin clears the latch.
                    static uint64_t s_auto_pin = 0;
                    if (pent != s_auto_pin) {
                        s_auto_pin = pent;
                        g_fp_user_off.store(0, std::memory_order_relaxed);
                    }
                }
                if (g_fp_auto.load(std::memory_order_relaxed) &&
                    !headpose::fp_enabled() && bestd < 12.0f &&
                    !g_fp_user_off.load(std::memory_order_relaxed)) {
                    static int s_last_auto = -1000;
                    if (ticks - s_last_auto >= 10) {
                        s_last_auto = ticks;
                        g_fp_request.store(1, std::memory_order_relaxed);
                        g_fp_auto_fires.fetch_add(1, std::memory_order_relaxed);
                        LOG_INFO("fp: AUTO first person requested (build 225, "
                                 "fp_auto): the pin is %.2f m from the camera, "
                                 "no F2 needed.", bestd);
                    }
                }

                // -----------------------------------------------------------
                // BUILD 116: ARM THE GUN-ROOT WRITE FROM HERE, EVERY TICK.
                //
                // This block used to live ~320 lines below, inside the
                // (ticks % 5) == 2 hand census, inside `if (wpn)`, five levels
                // down. It had no business being there. Arming acts on the
                // PLAYER rig's gun-root bone, resolved from the PLAYER rig's
                // own name map; it reads nothing from the hand census and
                // nothing from the weapon census. Sitting there made arming a
                // three-way coincidence on a single 1 Hz tick:
                //
                //   1. bestp live. g_skel_ring is 128 entries of
                //      SkeletonPostUpdate rcx, written ~144 times a second PER
                //      SKELETON, so the player's body is evicted within
                //      seconds. Measured window: 1 to 5 s per entity latch,
                //      in good runs and bad alike.
                //   2. (ticks % 5) == 2. One tick in five, for no reason.
                //   3. g_wskel_cand already non-zero. A separate warm-up that
                //      the gun-root write does not depend on.
                //
                // Both 2026-08-20 failures are [VERIFIED] in the logs and they
                // fail on DIFFERENT conditions, which is what proves the nest
                // is the defect rather than any one gate:
                //
                //   grwxr-18852 (115.2): bestp live 00:03:02..00:03:05, i.e.
                //     ticks%5 = 3,4,0,1. The next (ticks%5)==2 tick was
                //     00:03:06 and bestp had died. MISSED BY ONE TICK.
                //   grwxr-18668 (115): hit the (ticks%5)==2 tick at
                //     23:47:19.645, but that same tick logged `wskel: no pick`.
                //     The census picked at 23:47:20.655. MISSED ON COND 3.
                //
                // Every run that DID arm armed EXACTLY ONCE (5700, 3768,
                // 11324: one `wgun: ARMED` line each). A lottery win, not a
                // mechanism. Here only condition 1 remains.
                //
                // The bone index is still re-resolved from this rig's name map
                // on every tick and never cached across ticks, so a rig that
                // does not carry the bone DISARMS rather than writing a wrong
                // one, and a rig swap (respawn, outfit, vehicle) self-corrects
                // within one second. Skeleton pointer is published LAST,
                // exactly as before: the asm stub reads it first and treats
                // zero as disarmed, so a half-published state can never write.
                //
                // Rule 8 is unaffected: this is the 1 Hz init/drain thread,
                // not Present and not a per-draw hook.
                // -----------------------------------------------------------
                if (g_wgun_hook.installed()) {
                    uint64_t arm_rig = 0;
                    int      gn      = -1;
                    if (g_wgun_on.load(std::memory_order_relaxed) &&
                        read_block(bestp + 0x220, &arm_rig, sizeof(arm_rig)) &&
                        arm_rig > 0x10000 && !(arm_rig & 7))
                        gn = rig_find_node(
                            arm_rig,
                            g_wgun_hash.load(std::memory_order_relaxed));
                    if (gn >= 0) {
                        const float dz =
                            g_wgun_dz_cfg.load(std::memory_order_relaxed);
                        memcpy(&grwxr_wgun_dz, &dz, sizeof(grwxr_wgun_dz));
                        grwxr_wgun_node = (uint32_t)gn;
                        // BUILD 183: the mount mirror's targets, from this
                        // rig's name map every tick like the gun node.
                        // Never the gun node itself.
                        for (int m = 0; m < 3; ++m) {
                            const int ni = rig_find_node(arm_rig, kMirrorHash[m]);
                            g_mirror_node[m] =
                                (ni >= 0 && ni != gn) ? (uint16_t)ni : 0xFFFFu;
                        }
                        // 2026-08-31: the hand chains, re-resolved from this
                        // rig's name map every tick exactly as the gun node
                        // is. Count zeroed first and stored last with release
                        // order, so the engine-thread reader can never walk a
                        // half-rewritten table; a bone this rig does not carry
                        // is dropped, never guessed.
                        for (int s = 0; s < 2; ++s) {
                            g_hand_n[s].store(0, std::memory_order_release);
                            uint32_t k = 0;
                            if (g_wgun_hands.load(std::memory_order_relaxed)) {
                                for (int b = 0; b < 21; ++b) {
                                    const int ni = rig_find_node(
                                        arm_rig, kHandChain[s][b]);
                                    // Build 142: the HAND itself is slot 0
                                    // and everything downstream (the free
                                    // reach, the arm solve) reads it there,
                                    // so a rig without the hand publishes an
                                    // empty side, not a shifted one.
                                    if (b == 0 && ni < 0) break;
                                    if (ni >= 0)
                                        g_hand_node[s][k++] = (uint16_t)ni;
                                }
                            }
                            g_hand_n[s].store(k, std::memory_order_release);
                            // BUILD 162: the anatomy set, fixed slots, so
                            // the absolute wrist can find the knuckles even
                            // though the compacted chain above has dropped
                            // a bone (handbones reads 20 of 21 on this rig).
                            g_hand_anat_n[s].store(0, std::memory_order_release);
                            {
                                const uint32_t ah[4] = {
                                    kHandChain[s][0],  kHandChain[s][9],
                                    kHandChain[s][12], kHandChain[s][18]};
                                uint32_t an = 0;
                                for (int b = 0; b < 4; ++b) {
                                    const int ni = rig_find_node(arm_rig, ah[b]);
                                    g_hand_anat[s][b] =
                                        ni >= 0 ? (uint16_t)ni : 0xFFFFu;
                                    if (ni >= 0) ++an;
                                }
                                static uint32_t s_anat_sig[2] =
                                    {0xFFFFFFFFu, 0xFFFFFFFFu};
                                const uint32_t sig =
                                    ((uint32_t)g_hand_anat[s][0] << 16) ^
                                    (uint32_t)g_hand_anat[s][3] ^ (an << 28);
                                if (sig != s_anat_sig[s]) {
                                    s_anat_sig[s] = sig;
                                    int pr[4];
                                    for (int b = 0; b < 4; ++b)
                                        pr[b] = g_hand_anat[s][b] == 0xFFFFu
                                                    ? -1 : (int)g_hand_anat[s][b];
                                    LOG_INFO("wrist: anat side=%d hand=%d "
                                             "idx1=%d mid1=%d pky1=%d "
                                             "(%u of 4 resolved)",
                                             s, pr[0], pr[1], pr[2], pr[3], an);
                                }
                                g_hand_anat_n[s].store(an == 4 ? 4u : 0u,
                                                       std::memory_order_release);
                            }
                            // Build 140/142: the arm set, FIXED SLOTS now
                            // (0xFFFF = missing) because the solve needs to
                            // know which entry is the arm and which the
                            // forearm; a compacted table loses that. Count is
                            // 10 only when both core bones resolved.
                            g_arm_n[s].store(0, std::memory_order_release);
                            uint32_t a = 0;
                            if (g_wgun_hide_arms.load(
                                    std::memory_order_relaxed) ||
                                g_wgun_arms.load(std::memory_order_relaxed)) {
                                const int sh = rig_find_node(
                                    arm_rig, kShoulderHash[s]);
                                g_shoulder_node[s] =
                                    sh >= 0 ? (uint16_t)sh : 0xFFFFu;
                                for (int b = 0; b < 10; ++b) {
                                    const int ni = rig_find_node(
                                        arm_rig, kArmChain[s][b]);
                                    g_arm_node[s][b] =
                                        ni >= 0 ? (uint16_t)ni : 0xFFFFu;
                                }
                                // BUILD 182: the forearm's other children.
                                for (int b = 0; b < 2; ++b) {
                                    const int ni = rig_find_node(
                                        arm_rig, kForeExtra[s][b]);
                                    g_fore_extra[s][b] =
                                        ni >= 0 ? (uint16_t)ni : 0xFFFFu;
                                }
                                static int s_x_logged[2] = {-1, -1};
                                const int xs = (int)g_fore_extra[s][0] * 65536 +
                                               (int)g_fore_extra[s][1];
                                if (xs != s_x_logged[s]) {
                                    s_x_logged[s] = xs;
                                    LOG_INFO("wrist: forearm extras side=%d "
                                             "wrist=%d other=%d (node indices; "
                                             "65535 = not on this rig). These "
                                             "ride the forearm from build 182.",
                                             s, (int)g_fore_extra[s][0],
                                             (int)g_fore_extra[s][1]);
                                }
                                if (g_arm_node[s][0] != 0xFFFFu &&
                                    g_arm_node[s][1] != 0xFFFFu)
                                    a = 10;
                            }
                            g_arm_n[s].store(a, std::memory_order_release);
                        }
                        grwxr_wgun_skel = bestp;          // published LAST
                    } else {
                        g_hand_n[0].store(0, std::memory_order_release);
                        g_hand_n[1].store(0, std::memory_order_release);
                        g_arm_n[0].store(0, std::memory_order_release);
                        g_arm_n[1].store(0, std::memory_order_release);
                        grwxr_wgun_skel = 0;
                    }

                    // Log cadence: every state change, plus the old 1-in-5
                    // beat so the counter row keeps the shape logreport.py's
                    // v_wgun window and the owner's eye already know. Without
                    // the change gate this line would go from 1-in-5 to every
                    // tick and bury the rest of the log.
                    static int      s_arm_node = -2;
                    static uint64_t s_arm_skel = 1;
                    const bool changed = (gn != s_arm_node) ||
                                         (grwxr_wgun_skel != s_arm_skel);
                    s_arm_node = gn;
                    s_arm_skel = grwxr_wgun_skel;
                    if (changed || (ticks % 5) == 2)
                        LOG_INFO("wgun: %s node=%d %s dz=%.2f "
                                 "calls=%llu lifts=%llu rots=%llu "
                                 "pos=%llu | skip nopose=%llu "
                                 "nobuf=%llu norec=%llu noray=%llu "
                                 "nopos=%llu badq=%llu | two=%llu tworej=%llu "
                                 "roll=%llu | grip fwd=%.2f lat=%.2f up=%.2f "
                                 "two=%.2f slid=%llu clamped=%llu/%.2fm | "
                                 "handbones L=%u R=%u hands=%llu anch=%llu "
                                 "far=%llu att=%.2f arms=%llu free=%llu "
                                 "hfar=%llu ik=%llu ikcl=%llu ikskip=%llu "
                                 "pitch=%.1f gveto=%llu stowd=%u..%umm | "
                                 "wrist lat=%llu ev=%llu seed=%llu "
                                 "miss=%llu hold=%llu wipe=%llu n=%d "
                                 "abs=%llu nogrip=%llu noanat=%llu | "
                                 "twist L=%.0f R=%.0f t=%.2f,%.2f,%.2f,%.2f "
                                 "twists=%llu kept=%llu | wf=%llu wfsh=%llu "
                                 "wfcap=%llu shmax=%.0fmm clav=%.0f | "
                                 "mir=%llu mirn=%d,%d,%d",
                                 gn >= 0 ? "ARMED" : "idle", gn,
                                 grwxr_wgun_rot ? "ROTATE" : "lift",
                                 g_wgun_dz_cfg.load(std::memory_order_relaxed),
                                 (unsigned long long)grwxr_wgun_calls,
                                 (unsigned long long)grwxr_wgun_writes,
                                 (unsigned long long)grwxr_wg_rot,
                                 (unsigned long long)grwxr_wg_pos,
                                 (unsigned long long)grwxr_wg_nopose,
                                 (unsigned long long)grwxr_wg_nobuf,
                                 (unsigned long long)grwxr_wg_norec,
                                 (unsigned long long)grwxr_wg_noray,
                                 (unsigned long long)grwxr_wg_nopos,
                                 (unsigned long long)grwxr_wg_badq,
                                 (unsigned long long)grwxr_wg_two,
                                 (unsigned long long)grwxr_wg_tworej,
                                 (unsigned long long)grwxr_wg_roll,
                                 g_wgun_grip_fwd.load(
                                     std::memory_order_relaxed),
                                 g_wgun_grip_lat.load(
                                     std::memory_order_relaxed),
                                 g_wgun_grip_up.load(
                                     std::memory_order_relaxed),
                                 g_wgun_grip_two.load(
                                     std::memory_order_relaxed),
                                 (unsigned long long)grwxr_wg_grip,
                                 (unsigned long long)grwxr_wg_clamped,
                                 g_wgun_pos_clamp.load(
                                     std::memory_order_relaxed),
                                 g_hand_n[0].load(std::memory_order_relaxed),
                                 g_hand_n[1].load(std::memory_order_relaxed),
                                 (unsigned long long)grwxr_wg_hands,
                                 (unsigned long long)grwxr_wg_anchor,
                                 (unsigned long long)grwxr_wg_far,
                                 g_wg_two_attach.load(
                                     std::memory_order_relaxed),
                                 (unsigned long long)grwxr_wg_arms,
                                 (unsigned long long)grwxr_wg_free,
                                 (unsigned long long)grwxr_wg_hfar,
                                 (unsigned long long)grwxr_wg_ik,
                                 (unsigned long long)grwxr_wg_ikcl,
                                 (unsigned long long)grwxr_wg_ikskip,
                                 g_wgun_pitch.load(
                                     std::memory_order_relaxed),
                                 (unsigned long long)grwxr_wg_gripveto,
                                 g_stow_d_min.load(std::memory_order_relaxed),
                                 g_stow_d_max.load(std::memory_order_relaxed),
                                 (unsigned long long)grwxr_wr_latch,
                                 (unsigned long long)grwxr_wr_evict,
                                 (unsigned long long)grwxr_wr_seed,
                                 (unsigned long long)grwxr_wr_miss,
                                 (unsigned long long)grwxr_wr_hold,
                                 (unsigned long long)grwxr_wr_wipe,
                                 g_wr_n,
                                 (unsigned long long)grwxr_wr_abs,
                                 (unsigned long long)grwxr_wr_nogrip,
                                 (unsigned long long)grwxr_wr_noanat,
                                 bitsf(g_twist_bits[0].load(std::memory_order_relaxed)),
                                 bitsf(g_twist_bits[1].load(std::memory_order_relaxed)),
                                 bitsf(g_twist_t_bits[0].load(std::memory_order_relaxed)),
                                 bitsf(g_twist_t_bits[1].load(std::memory_order_relaxed)),
                                 bitsf(g_twist_t_bits[2].load(std::memory_order_relaxed)),
                                 bitsf(g_twist_t_bits[3].load(std::memory_order_relaxed)),
                                 (unsigned long long)grwxr_wg_twist,
                                 (unsigned long long)grwxr_wg_keep,
                                 (unsigned long long)grwxr_wg_wf,
                                 (unsigned long long)grwxr_wg_wfsh,
                                 (unsigned long long)grwxr_wg_wfcap,
                                 bitsf(g_wf_shmax_bits.exchange(0, std::memory_order_relaxed)),
                                 bitsf(g_clav_max_bits.exchange(0, std::memory_order_relaxed)),
                                 (unsigned long long)grwxr_wg_mirror,
                                 (int)g_mirror_node[0], (int)g_mirror_node[1],
                                 (int)g_mirror_node[2]);
                    // BUILD 160: window reset, so stowd= reads per-second
                    // extremes rather than lifetime ones.
                    g_stow_d_min.store(0xFFFFFFFFu, std::memory_order_relaxed);
                    g_stow_d_max.store(0, std::memory_order_relaxed);
                    // BUILD 120: the gate's own numbers, on their own line, so
                    // the two-hand threshold is chosen by measurement.
                    char ta[300];
                    two_agree_report(ta, sizeof(ta));
                    LOG_INFO("%s", ta);

                    // BUILD 155: DOES THE ANIMATION TAKE THE HANDS BACK?
                    // Counters are cumulative, so difference two samples
                    // rather than reading one (traps section 9 item 2).
                    // kept/checks near 100 percent means our writes survive
                    // the frame and the hand complaints are ours to fix.
                    // Near 0 with a drift of centimetres means the animation
                    // reauthors the bone every frame, no write to it can
                    // hold, and the fix is a duplicate bone in the
                    // FakeGunRoot_Gameplay pattern rather than any tuning.
                    const uint32_t cl = g_hb_checks[0].load(
                                            std::memory_order_relaxed);
                    const uint32_t cr = g_hb_checks[1].load(
                                            std::memory_order_relaxed);
                    LOG_INFO("animsurv: LEFT kept=%u/%u xbuf=%u drift "
                             "last=%umm worst=%umm | RIGHT kept=%u/%u xbuf=%u "
                             "drift last=%umm worst=%umm | 100%% kept = our "
                             "writes hold, 0%% = the animation reauthors the "
                             "bone, xbuf = cross-rig calls excluded (156)",
                             g_hb_kept[0].load(std::memory_order_relaxed), cl,
                             g_hb_xbuf[0].load(std::memory_order_relaxed),
                             g_hb_last_mm[0].load(std::memory_order_relaxed),
                             g_hb_drift_mm[0].load(std::memory_order_relaxed),
                             g_hb_kept[1].load(std::memory_order_relaxed), cr,
                             g_hb_xbuf[1].load(std::memory_order_relaxed),
                             g_hb_last_mm[1].load(std::memory_order_relaxed),
                             g_hb_drift_mm[1].load(std::memory_order_relaxed));
                }

                // BUILD 164: the twist probe, side 0, 1 Hz while the arm set
                // is resolved. See g_tp_anim for what the two lines decide.
                if (g_arm_n[0].load(std::memory_order_acquire) >= 10) {
                    float a[22], o[15];
                    for (int i = 0; i < 22; ++i)
                        a[i] = bitsf(g_tp_anim[i].load(std::memory_order_relaxed));
                    for (int i = 0; i < 15; ++i)
                        o[i] = bitsf(g_tp_ours[i].load(std::memory_order_relaxed));
                    LOG_INFO("twistprobe anim: fore t=%.2f d=%.0f tw=%.0f | "
                             "oelb t=%.2f d=%.0f tw=%.0f | o1 t=%.2f d=%.0f "
                             "tw=%.0f | o2 t=%.2f d=%.0f tw=%.0f | o3 t=%.2f "
                             "d=%.0f tw=%.0f | o4 t=%.2f d=%.0f tw=%.0f | la "
                             "t=%.2f d=%.0f tw=%.0f | hand tw=%.0f (t along "
                             "elbow-wrist, d mm off it, tw deg about it vs "
                             "ForeArm, animated state)",
                             a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                             a[8], a[9], a[10], a[11], a[12], a[13], a[14],
                             a[15], a[16], a[17], a[18], a[19], a[20], a[21]);
                    LOG_INFO("twistprobe ours: fore tw=%.0f oelb tw=%.0f "
                             "o1 tw=%.0f o2 tw=%.0f o3 tw=%.0f o4 tw=%.0f "
                             "la tw=%.0f | hand tw=%.0f | phi=%.0f | keep "
                             "fore=%.0f oelb=%.0f o1=%.0f o2=%.0f o3=%.0f "
                             "o4=%.0f la=%.0f (after our write; helpers "
                             "should read anim + t x phi; keep = deg between "
                             "written and found, 0 held, -1 never written)",
                             o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7],
                             bitsf(g_twist_bits[0].load(std::memory_order_relaxed)),
                             o[8], o[9], o[10], o[11], o[12], o[13], o[14]);
                    // BUILD 172: the survival line. Slot order: hand, Arm,
                    // ForeArm, O_Elbow, O_01..O_04, look-at, L_01Arm, O_02Arm.
                    float sv[33];
                    for (int i = 0; i < 33; ++i)
                        sv[i] = bitsf(g_sv_out[i].load(std::memory_order_relaxed));
                    #define SV3(k) sv[(k)*3+0], sv[(k)*3+1], sv[(k)*3+2]
                    LOG_INFO("survive: n=%u stale=%u unstable=%u | "
                             "hand %.0f/%.0f/%.0f | arm %.0f/%.0f/%.0f | "
                             "fore %.0f/%.0f/%.0f | oelb %.0f/%.0f/%.0f | "
                             "o1 %.0f/%.0f/%.0f | o2 %.0f/%.0f/%.0f | "
                             "o3 %.0f/%.0f/%.0f | o4 %.0f/%.0f/%.0f | "
                             "la %.0f/%.0f/%.0f | l01 %.0f/%.0f/%.0f | "
                             "o02 %.0f/%.0f/%.0f (read at Present: mm from "
                             "our 2nd-call write / deg from it / mm from the "
                             "animated 1st-call read; hand is the control, "
                             "-1 not captured)",
                             g_sv_n.load(std::memory_order_relaxed),
                             g_sv_stale.load(std::memory_order_relaxed),
                             g_sv_unstable.load(std::memory_order_relaxed),
                             SV3(10), SV3(0), SV3(1), SV3(2), SV3(5), SV3(6),
                             SV3(7), SV3(8), SV3(9), SV3(3), SV3(4));
                    #undef SV3
                    // BUILD 184, probe A: the gun root and its mirrors at
                    // Present, same rules as the arm line above.
                    if (g_wroll_probe.load(std::memory_order_relaxed)) {
                        float gs[8];
                        for (int i = 0; i < 8; ++i)
                            gs[i] = bitsf(g_gs_out[i].load(std::memory_order_relaxed));
                        LOG_INFO("gsurvive: n=%u | root %.0f/%.1f | fgr %.0f/%.1f "
                                 "| pR %.0f/%.1f | pL %.0f/%.1f (read at Present: "
                                 "mm from our last write / deg from it; -1 not "
                                 "captured; 0/0 = the record the frame drew is "
                                 "ours)",
                                 g_gs_n.load(std::memory_order_relaxed),
                                 gs[0], gs[1], gs[2], gs[3], gs[4], gs[5], gs[6], gs[7]);
                    }
                    // BUILD 173: the wrist and the reach, then reset.
                    // BUILD 174: the running-minimum segment lengths ride
                    // along, both sides, so a poisoned minimum is visible.
                    LOG_INFO("reachwrist: L reach=%.2f..%.2f wrist=%.0f..%.0f "
                             "deg n=%u L1=%.3f L2=%.3f | R reach=%.2f..%.2f "
                             "wrist=%.0f..%.0f deg n=%u L1=%.3f L2=%.3f "
                             "(reach = shoulder-to-hand over L1+L2 before the "
                             "clamp, 1.00 is full extension; wrist = degrees "
                             "the solved forearm axis departs from the "
                             "animated wrist bend, per second min..max; L1 L2 "
                             "the running-minimum upper arm and forearm, m)",
                             g_rw_min[0][0], g_rw_max[0][0],
                             g_rw_min[0][1], g_rw_max[0][1], g_rw_n[0],
                             bitsf(g_armlen_bits[0][0].load(std::memory_order_relaxed)),
                             bitsf(g_armlen_bits[0][1].load(std::memory_order_relaxed)),
                             g_rw_min[1][0], g_rw_max[1][0],
                             g_rw_min[1][1], g_rw_max[1][1], g_rw_n[1],
                             bitsf(g_armlen_bits[1][0].load(std::memory_order_relaxed)),
                             bitsf(g_armlen_bits[1][1].load(std::memory_order_relaxed)));
                    // BUILD 213 (CX-025): its own line and its own format. The
                    // first pass added a field to the line above, which is at
                    // fifteen conversions, and forgot the argument: every field
                    // to its right shifted and the line that judges this build
                    // printed garbage. One conversion here, one argument.
                    LOG_INFO("armanchor: on=%d len=%.3f m (build 213: how far the solved arm is carried "
                             "from where the animation put it, which is how far the viewpoint sits from "
                             "the character's head; 0.000 with the key off or outside first person. The "
                             "arm is solved in the animated frame and translated by this, so reach above "
                             "must read as it did before 6DOF)",
                             g_arm_anchor.load(std::memory_order_relaxed),
                             bitsf(g_arm_anchor_bits.exchange(0, std::memory_order_relaxed)));
                    {   // BUILD 283: the shoulder pull, then reset. capped = the pull hit its cap and the arm still lacks reach.
                        uint64_t pc[2], pn[2], pcap[2], psum[2];
                        uint32_t pmx[2];
                        for (int i = 0; i < 2; ++i) {
                            pc[i] = g_pull_calls[i]; pn[i] = g_pull_n[i]; pcap[i] = g_pull_cap[i];
                            psum[i] = g_pull_sum_um[i]; pmx[i] = g_pull_max_um[i];
                            g_pull_calls[i] = 0; g_pull_n[i] = 0; g_pull_cap[i] = 0;
                            g_pull_sum_um[i] = 0; g_pull_max_um[i] = 0;
                        }
                        LOG_INFO("pull: on=%d k=%.2f max=%.0f mm | L calls=%llu pulled=%llu capped=%llu mean=%.0f max=%.0f mm | "
                                 "R calls=%llu pulled=%llu capped=%llu mean=%.0f max=%.0f mm (build 283: calls = first calls on a "
                                 "rig buffer; pulled = the hand was past k of full extension and the limb was carried toward it; "
                                 "capped = the cap was hit and the arm still lacks reach; mean over pulled calls; build 284: "
                                 "segrej = L %llu R %llu length samples refused this second as not an animated arm)",
                                 g_wgun_pull.load(std::memory_order_relaxed),
                                 g_wgun_pull_k.load(std::memory_order_relaxed),
                                 g_wgun_pull_max.load(std::memory_order_relaxed),
                                 (unsigned long long)pc[0], (unsigned long long)pn[0], (unsigned long long)pcap[0],
                                 pn[0] ? (double)psum[0] / (double)pn[0] * 0.001 : 0.0, (double)pmx[0] * 0.001,
                                 (unsigned long long)pc[1], (unsigned long long)pn[1], (unsigned long long)pcap[1],
                                 pn[1] ? (double)psum[1] / (double)pn[1] * 0.001 : 0.0, (double)pmx[1] * 0.001,
                                 (unsigned long long)g_seg_rej[0], (unsigned long long)g_seg_rej[1]);
                        g_seg_rej[0] = g_seg_rej[1] = 0;
                    }
                    LOG_INFO("pole: on=%d | L anim=%llu anat=%llu held=%llu rej=%llu flips=%llu perp=%.3f | "
                             "R anim=%llu anat=%llu held=%llu rej=%llu flips=%llu perp=%.3f "
                             "(build 214: which point of the circle about the shoulder-to-hand axis the elbow takes. "
                             "anim = the animated elbow supplied it, anat = it hung below the axis by anatomy because the "
                             "animated arm could not, rej = the animated pole pointed above the line and was refused, "
                             "held = neither and the last was kept; perp is the animated elbow's "
                             "perpendicular in metres off a 0.28 m upper arm. FLIPS ARE THE INVERSION and must read 0)",
                             g_arm_pole.load(std::memory_order_relaxed),
                             (unsigned long long)g_pole_anim[0].exchange(0, std::memory_order_relaxed),
                             (unsigned long long)g_pole_anat[0].exchange(0, std::memory_order_relaxed),
                             (unsigned long long)g_pole_held[0].exchange(0, std::memory_order_relaxed),
                             (unsigned long long)g_pole_rej[0].exchange(0, std::memory_order_relaxed),
                             (unsigned long long)g_pole_flip[0].exchange(0, std::memory_order_relaxed),
                             bitsf(g_pole_pn_bits[0].load(std::memory_order_relaxed)),
                             (unsigned long long)g_pole_anim[1].exchange(0, std::memory_order_relaxed),
                             (unsigned long long)g_pole_anat[1].exchange(0, std::memory_order_relaxed),
                             (unsigned long long)g_pole_held[1].exchange(0, std::memory_order_relaxed),
                             (unsigned long long)g_pole_rej[1].exchange(0, std::memory_order_relaxed),
                             (unsigned long long)g_pole_flip[1].exchange(0, std::memory_order_relaxed),
                             bitsf(g_pole_pn_bits[1].load(std::memory_order_relaxed)));
                    {   // BUILD 285
                        const uint64_t sn[2] = {g_pole_n[0].exchange(0, std::memory_order_relaxed),
                                                g_pole_n[1].exchange(0, std::memory_order_relaxed)};
                        const double raw[2] = {(double)g_pole_raw_ud[0].exchange(0, std::memory_order_relaxed) * 1.0e-6,
                                               (double)g_pole_raw_ud[1].exchange(0, std::memory_order_relaxed) * 1.0e-6};
                        const double out[2] = {(double)g_pole_out_ud[0].exchange(0, std::memory_order_relaxed) * 1.0e-6,
                                               (double)g_pole_out_ud[1].exchange(0, std::memory_order_relaxed) * 1.0e-6};
                        LOG_INFO("polesm: on=%d tau=%.0f ms rate=%.0f deg/s | L n=%llu raw=%.2f out=%.2f deg/call clip=%llu | "
                                 "R n=%llu raw=%.2f out=%.2f deg/call clip=%llu (build 285: raw = mean turn of the animated "
                                 "pole between calls, out = of the pole used; out well under raw is the smoothing working; "
                                 "clip = calls the rate cap held it back; pole: flips must now read near 0)",
                                 g_pole_smooth.load(std::memory_order_relaxed),
                                 g_pole_tau_ms.load(std::memory_order_relaxed), g_pole_rate.load(std::memory_order_relaxed),
                                 (unsigned long long)sn[0], sn[0] ? raw[0] / (double)sn[0] : 0.0,
                                 sn[0] ? out[0] / (double)sn[0] : 0.0,
                                 (unsigned long long)g_pole_clip[0].exchange(0, std::memory_order_relaxed),
                                 (unsigned long long)sn[1], sn[1] ? raw[1] / (double)sn[1] : 0.0,
                                 sn[1] ? out[1] / (double)sn[1] : 0.0,
                                 (unsigned long long)g_pole_clip[1].exchange(0, std::memory_order_relaxed));
                        {   // BUILD 294
                            float tv[6];
                            for (int i = 0; i < 6; ++i) {
                                const uint32_t tb = g_torso_dbg[i].load(std::memory_order_relaxed);
                                memcpy(&tv[i], &tb, sizeof(float));
                            }
                            LOG_INFO("torso: on=%d turned=%llu catching=%llu skipped=%llu clamped=%llu nodes=%.0f | last phi %+.1f deg (applied rotation), "
                                     "yaw head %+.1f, animated torso %+.1f, body %+.1f, right hand %+.1f deg; largest |phi| %.1f deg (build 294: phi is how "
                                     "far the torso was turned from the animation's; body is the state machine's yaw)",
                                     g_torso_on.load(std::memory_order_relaxed), (unsigned long long)g_torso_n.load(std::memory_order_relaxed),
                                     (unsigned long long)g_torso_catch.load(std::memory_order_relaxed),
                                     (unsigned long long)g_torso_skip.load(std::memory_order_relaxed),
                                     (unsigned long long)g_torso_clamp.load(std::memory_order_relaxed), tv[5], tv[0], tv[1], tv[2], tv[3], tv[4],
                                     g_torso_phi_max * 57.29578f);
                            LOG_INFO("torsowhy: entered=%llu otherskel=%llu second=%llu notfp=%llu rig=%llu nodes=%llu down=%llu reads=%llu shoulder=%llu "
                                     "baseframe=%llu state=%llu wrote=%llu (build 295: why torso_turn returned; state = reached the state machine; "
                                     "shoulder line is dbg 0..4 above only until the state machine overwrites them: model right %+.2f %+.2f %+.2f)",
                                     (unsigned long long)g_torso_why[0].load(), (unsigned long long)g_torso_why[1].load(),
                                     (unsigned long long)g_torso_why[2].load(), (unsigned long long)g_torso_why[3].load(),
                                     (unsigned long long)g_torso_why[4].load(), (unsigned long long)g_torso_why[5].load(),
                                     (unsigned long long)g_torso_why[6].load(), (unsigned long long)g_torso_why[7].load(),
                                     (unsigned long long)g_torso_why[8].load(), (unsigned long long)g_torso_why[9].load(),
                                     (unsigned long long)g_torso_why[10].load(), (unsigned long long)g_torso_why[11].load(),
                                     tv[0], tv[1], tv[2]);
                            g_torso_phi_max = 0.0f;
                            {   // BUILD 300
                                float lv[5];
                                for (int i = 0; i < 5; ++i) {
                                    const uint32_t lb = g_lean_dbg[i].load(std::memory_order_relaxed);
                                    memcpy(&lv[i], &lb, sizeof(float));
                                }
                                LOG_INFO("lean: hinge=%d hinged=%llu slidecap=%llu anglecap=%llu | offset %.3f m (largest %.3f), angle %+.1f deg, head moved %.3f m, "
                                         "slide %.3f m | camera reads: none-in-force=%llu undone=%llu UNHINGED=%llu nopub=%llu small=%llu | "
                                         "why: entered=%llu otherskel=%llu second=%llu notfp=%llu rig=%llu stale=%llu geom=%llu tiny=%llu "
                                         "(build 300: UNHINGED = the camera read the Head bone before the hinge reached it while a hinge was in "
                                         "force; the camera then keeps the full offset itself, so it is a count of near misses, not of errors; "
                                         "undone = reads where the hinge was taken back out)",
                                         g_lean_hinge.load(std::memory_order_relaxed), (unsigned long long)g_lean_c[8].load(),
                                         (unsigned long long)g_lean_slidecap.load(), (unsigned long long)g_lean_c[9].load(),
                                         lv[0], lv[4], lv[1], lv[2], lv[3],
                                         (unsigned long long)g_lean_rd[0].load(), (unsigned long long)g_lean_rd[1].load(),
                                         (unsigned long long)g_lean_rd[2].load(), (unsigned long long)g_lean_rd[3].load(),
                                         (unsigned long long)g_lean_rd[4].load(),
                                         (unsigned long long)g_lean_c[0].load(), (unsigned long long)g_lean_c[1].load(),
                                         (unsigned long long)g_lean_c[2].load(), (unsigned long long)g_lean_c[3].load(),
                                         (unsigned long long)g_lean_c[4].load(), (unsigned long long)g_lean_c[5].load(),
                                         (unsigned long long)g_lean_c[6].load(), (unsigned long long)g_lean_c[7].load());
                                lean_dbg_put(4, 0.0f);
                            }
                            rig_dump_once();   // BUILD 297
                        }
                        {   // BUILD 293
                            const uint64_t bn[2] = {g_pole_bias_n[0].exchange(0, std::memory_order_relaxed),
                                                    g_pole_bias_n[1].exchange(0, std::memory_order_relaxed)};
                            const double bu[2] = {(double)g_pole_bias_ud[0].exchange(0, std::memory_order_relaxed) * 1.0e-6,
                                                  (double)g_pole_bias_ud[1].exchange(0, std::memory_order_relaxed) * 1.0e-6};
                            LOG_INFO("polebias: down L=%.2f R=%.2f | L n=%llu mean=%.1f deg | R n=%llu mean=%.1f deg (build 293: calls the "
                                     "gravity bias changed the elbow side, and the mean angle it moved it by; n = 0 when one-handed or "
                                     "unarmed since the bias scales with the two-hand authority)",
                                     g_pole_down_l.load(std::memory_order_relaxed), g_pole_down_r.load(std::memory_order_relaxed),
                                     (unsigned long long)bn[0], bn[0] ? bu[0] / (double)bn[0] : 0.0,
                                     (unsigned long long)bn[1], bn[1] ? bu[1] / (double)bn[1] : 0.0);
                        }
                    }
                    for (int i = 0; i < 2; ++i) {
                        g_rw_min[i][0] = 9.0f;  g_rw_max[i][0] = 0.0f;
                        g_rw_min[i][1] = 999.0f; g_rw_max[i][1] = -1.0f;
                        g_rw_n[i] = 0;
                    }
                }

                // BUILD 142: THE CALIBRATION CAPTURE, log only, 1 Hz thread.
                // Owner's procedure: stand, hold both controllers straight
                // out to the sides at shoulder height, arm hand_calib. The
                // avatar side needs the arm lengths, which the solve measures
                // while the weapon is drawn, so draw and move first.
                //
                // 2026-08-31: fires on the first tick at or after the arming
                // delay, so the pose is struck AFTER the trigger rather than
                // during it. Cleared before the capture runs, so a long census
                // tick cannot double-fire it.
                const uint64_t calib_due =
                    g_calib_due_ms.load(std::memory_order_relaxed);
                if (calib_due != 0 &&
                    (uint64_t)GetTickCount64() >= calib_due &&
                    g_calib_due_ms.exchange(0, std::memory_order_relaxed)) {
                    float rp[3] = {}, lp[3] = {};
                    const bool rok = aimtrace::ctrl_pos(rp);
                    const bool lok = aimtrace::ctrl_pos_l(lp);
                    uint64_t crig = 0;
                    float la[3] = {}, ra[3] = {}, hd[3] = {};
                    bool aok = false, hdok = false;
                    if (read_block(bestp + 0x220, &crig, sizeof(crig)) &&
                        crig > 0x10000 && !(crig & 7)) {
                        const int nl = rig_find_node(crig, 0xEB830ADAu);
                        const int nr = rig_find_node(crig, 0x6BB3F727u);
                        const int nh = rig_find_node(crig, 0x07C159A2u);
                        aok = nl >= 0 && nr >= 0 &&
                              read_bone_world(bestp, (unsigned)nl, la) &&
                              read_bone_world(bestp, (unsigned)nr, ra);
                        hdok = nh >= 0 &&
                               read_bone_world(bestp, (unsigned)nh, hd);
                    }
                    float len[2][2];
                    for (int s2 = 0; s2 < 2; ++s2)
                        for (int j = 0; j < 2; ++j) {
                            const uint32_t bits =
                                g_armlen_bits[s2][j].load(
                                    std::memory_order_relaxed);
                            memcpy(&len[s2][j], &bits, sizeof(float));
                        }
                    const float spanP = (rok && lok)
                        ? sqrtf((rp[0]-lp[0])*(rp[0]-lp[0]) +
                                (rp[1]-lp[1])*(rp[1]-lp[1]) +
                                (rp[2]-lp[2])*(rp[2]-lp[2])) : 0.0f;
                    const float shw = aok
                        ? sqrtf((ra[0]-la[0])*(ra[0]-la[0]) +
                                (ra[1]-la[1])*(ra[1]-la[1]) +
                                (ra[2]-la[2])*(ra[2]-la[2])) : 0.0f;
                    const float armsum = len[0][0] + len[0][1] +
                                         len[1][0] + len[1][1];
                    const float spanA =
                        (aok && armsum > 0.0f && armsum < 4.0f)
                            ? shw + armsum : 0.0f;
                    LOG_INFO("calib: CONTROLLERS span=%.3fm (%s). AVATAR "
                             "span=%.3fm (shoulders %.3f + arms L %.3f/%.3f "
                             "R %.3f/%.3f). ratio avatar/player=%.3f. head "
                             "bone world z=%.3f, entity z=%.3f. A ratio "
                             "well off 1.000 is the case for the wingspan "
                             "scale; arms at 0.000 mean the solve has not "
                             "measured yet, draw the weapon and move first.",
                             spanP, rok && lok ? "both tracked" : "MISSING "
                             "A CONTROLLER, capture void",
                             spanA, shw, len[0][0], len[0][1],
                             len[1][0], len[1][1],
                             (spanP > 0.1f && spanA > 0.1f)
                                 ? spanA / spanP : 0.0f,
                             hdok ? hd[2] : 0.0f, bpos[2]);
                    // 2026-09-01: APPLY IT, then put him where he can see the
                    // result. A measurement that needs a human to act on it is
                    // how this number went uncaptured for seven builds.
                    const bool good = spanP > 0.1f && spanA > 0.1f;
                    if (good) {
                        const float ratio = spanA / spanP;
                        // Refuse a ratio that cannot be a human being. Half to
                        // double covers every real body against every avatar;
                        // outside that the capture measured something else and
                        // applying it would be worse than applying nothing.
                        if (ratio > 0.5f && ratio < 2.0f) {
                            g_hand_scale.store(ratio, std::memory_order_relaxed);
                            g_fp_request.store(1, std::memory_order_relaxed);
                            LOG_INFO("calib: ARM SCALE APPLIED %.3f, and first "
                                     "person requested. To make it permanent "
                                     "without recalibrating, put this line in "
                                     "grwxr.cfg:  hand_scale = %.3f", ratio,
                                     ratio);
                            char sv[96];
                            _snprintf_s(sv, sizeof(sv), _TRUNCATE,
                                        "Captured. Arm scale %.2f. First "
                                        "person on.", ratio);
                            voice::say(sv);
                        } else {
                            LOG_INFO("calib: ratio %.3f is outside 0.5 to 2.0 "
                                     "and was NOT applied. That is not a body "
                                     "against an avatar; suspect the span "
                                     "measurement, not the scale.", ratio);
                            voice::say("Captured, but the number is not "
                                       "believable. Nothing applied.");
                        }
                    } else {
                        voice::say("Capture void. Draw a weapon and move "
                                   "around first, then try again.");
                    }
                }

                // -----------------------------------------------------------
                // 2026-08-13: THE OFF-HAND MEASUREMENT, corrected.
                //
                // The first version of this lived at the top of
                // grwxr_wgun_apply and its numbers were unusable. The reason
                // is worth keeping: PublishAttachments is entered TWICE PER
                // FRAME for the player skeleton (measured live at 145.6
                // samples/s on a 72 Hz engine, and independently recorded in
                // the build 77 ledger as 144 writes/s), and nothing re-solves
                // the pose between the two entries. So on entry 2 the bone we
                // read back is OUR OWN entry-1 write, and the measurement was
                // "the animated hand relative to where WE put the gun", which
                // is circular. Because the publish is last-write-wins and the
                // logger samples at 1 Hz, it read the contaminated entry
                // almost every time.
                //
                // Here instead: the init thread, read only, and reachable with
                // wgun = 0, where the mod writes the weapon bone at all. Then
                // the weapon frame IS the animation's.
                //
                // The gun-root node is resolved locally by name hash on this
                // rig. It deliberately does NOT use grwxr_wgun_node, which is
                // only ever assigned while wgun is armed and otherwise holds
                // 0, the rig ROOT: reading it here would silently measure the
                // hand against the wrong bone entirely.
                //
                // Node 10 FakeGunRoot_Gameplay is the live mount (build 78
                // armed node 8 for 50 s and the gun did not move), and its +Y
                // is the barrel (build 79, dot 0.999 over 14 ADS samples).
                //
                // Two hands are measured. LeftHand is the animated skeleton
                // hand. Prop_LeftHand is a rig bone whose name says the engine
                // authored it as a prop attachment point; if the engine holds
                // a support-hand point, that bone is it, and it should read far
                // steadier than the animated one.
                // -----------------------------------------------------------
                {
                    uint64_t org = 0;
                    if (read_block(bestp + 0x220, &org, sizeof(org)) &&
                        org > 0x10000 && !(org & 7)) {
                        const int nG = rig_find_node(org, 0x08B4DDD5u);  // FakeGunRoot_Gameplay
                        float gr[3], ax[3], ay[3], az[3];
                        if (nG >= 0 &&
                            read_bone_world(bestp, (unsigned int)nG, gr) &&
                            read_bone_world_axes(bestp, (unsigned int)nG,
                                                 ax, ay, az)) {
                            struct Probe { uint32_t hash; const char* name; };
                            static const Probe kProbes[] = {
                                {0xB675F36Cu, "LeftHand"},
                                {0x85562B5Cu, "Prop_LeftHand"},
                            };
                            for (const Probe& pr : kProbes) {
                                const int nd = rig_find_node(org, pr.hash);
                                float p[3];
                                if (nd < 0 ||
                                    !read_bone_world(bestp, (unsigned int)nd, p))
                                    continue;
                                const float d[3] = {p[0] - gr[0],
                                                    p[1] - gr[1],
                                                    p[2] - gr[2]};
                                // +Y is the barrel, so Y is distance along it.
                                const float along = d[0]*ay[0] + d[1]*ay[1] + d[2]*ay[2];
                                const float lat   = d[0]*ax[0] + d[1]*ax[1] + d[2]*ax[2];
                                const float vert  = d[0]*az[0] + d[1]*az[1] + d[2]*az[2];
                                const float dist  = sqrtf(d[0]*d[0] + d[1]*d[1] +
                                                          d[2]*d[2]);
                                // This tick reads the pose off the engine's own
                                // thread without its lock, so a torn read is
                                // possible. Drop the impossible ones rather
                                // than logging a number nobody can trust.
                                if (!isfinite(along) || !isfinite(lat) ||
                                    !isfinite(vert) || !(dist < 3.0f))
                                    continue;
                                LOG_INFO("ohand: %-13s vs gun-root: along-barrel="
                                         "%+.3f lateral=%+.3f vertical=%+.3f "
                                         "dist=%.3f m (node %d)",
                                         pr.name, along, lat, vert, dist, nd);
                            }
                        }
                    }
                }

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

                // Build 27: HAND BONE SANITY CHECK, read-only, no behaviour.
                //
                // This is the gate on all remaining IK work. Build 16b hunted
                // the HumanIK effector array by testing whether effector 3
                // landed near the LEFT HAND bone and effector 4 near the
                // RIGHT, and it found nothing. But the node indices it used
                // were 21 and 223, wildly asymmetric in a 312-bone rig, and
                // were NEVER CHECKED. If either resolved to a prop or
                // attachment helper instead of a real wrist, that dual-match
                // test could never pass even where the array does exist, and
                // the whole negative is an artifact of a bad reference point.
                //
                // So: resolve both hands and the head from the rig's own
                // name-hash map and log where they actually are. A real pair
                // of hands on a standing character is under about a metre
                // apart and each within about a metre of the head. Anything
                // wildly outside that means the indices are wrong, and THAT
                // is the bug, not the effector array.
                //
                // Read-only: rig_find_node is a binary search over a sorted
                // map and read_bone_world only touches range-checked memory.
                // Logged every 5th tick and only when the verdict changes, so
                // it cannot spam a play session.
                // BUILD 183: the mount at 1 Hz, one line, so the next log
                // shows what the weapon rides around a roll or a climb.
                // With the mirror on, node 10 and its mirrors hold ONE
                // record, so "which bone did the engine pick" is
                // unobservable here by design; the question that matters
                // is whether the weapon is ON OURS (its origin within
                // 50 mm of node 10). When it is not, the distances to the
                // bones we do not mirror name where it went. With the
                // mirror off the nearest bone is named as before. grep
                // 'mount: NOW'.
                {
                    uint64_t rg1 = 0;
                    float wo1[3] = {};
                    const uint64_t wpn1 =
                        g_wskel_cand.load(std::memory_order_relaxed);
                    if (wpn1 &&
                        read_block(bestp + 0x220, &rg1, sizeof(rg1)) &&
                        rg1 > 0x10000 && !(rg1 & 7) &&
                        read_block(wpn1 + 0x120, wo1, sizeof(wo1)) &&
                        isfinite(wo1[0]) && isfinite(wo1[1]) &&
                        isfinite(wo1[2])) {
                        struct M1 { uint32_t hash; const char* name; };
                        static const M1 kM1[8] = {
                            {0x08B4DDD5u, "FakeGunRoot_Gameplay"},  // ours
                            {0x826846F3u, "Fake_gunroot"},          // mirror 1
                            {0x53135E44u, "Prop_RightHand"},        // mirror 2
                            {0x85562B5Cu, "Prop_LeftHand"},         // mirror 2
                            {0x3FB256E5u, "RightHand_Weapon_Ref"},  // not mirrored
                            {0xA9611103u, "LeftHand_Weapon_Ref"},   // not mirrored
                            {0x75F94D30u, "RightHand"},             // the hand records
                            {0xB675F36Cu, "LeftHand"},
                        };
                        float dd[8];
                        int best = -1;
                        for (int i = 0; i < 8; ++i) {
                            dd[i] = -1.0f;
                            const int nn = rig_find_node(rg1, kM1[i].hash);
                            float p[3];
                            if (nn < 0 ||
                                !read_bone_world(bestp, (unsigned int)nn, p))
                                continue;
                            const float dx = p[0] - wo1[0], dy = p[1] - wo1[1],
                                        dz1 = p[2] - wo1[2];
                            const float d = sqrtf(dx*dx + dy*dy + dz1*dz1);
                            if (!isfinite(d)) continue;
                            dd[i] = d;
                            if (d < 0.15f && (best < 0 || d < dd[best]))
                                best = i;
                        }
                        // Ours wins any tie: at mode 2 the first four are
                        // one record, and the verdict is about ours.
                        // BUILD 184: 150 mm, not 50: the weapon origin the
                        // tick reads is a frame behind the bone, and one
                        // frame at running speed is 60 to 110 mm (183's
                        // run). The pick's lock rides along: OFF with the
                        // lock gone means the weapon left the hand, not
                        // the mount.
                        const bool ours = dd[0] >= 0.0f && dd[0] < 0.15f;
                        const int verdict = ours ? 0 : best;
                        static int s_mount_last = -2;
                        LOG_INFO("mount: NOW %s%s lock=%s | gp=%.3f fgr=%.3f pR=%.3f "
                                 "pL=%.3f refR=%.3f refL=%.3f RH=%.3f "
                                 "LH=%.3f m | mirror=%d nodes=%d,%d,%d "
                                 "copies=%llu",
                                 ours ? "ON OURS"
                                      : best >= 0 ? kM1[best].name
                                                  : "OFF OURS, on none of these",
                                 verdict != s_mount_last ? "  <<< CHANGED" : "",
                                 (g_wskel_lock && g_wskel_lock == wpn1) ? "held" : "none",
                                 dd[0], dd[1], dd[2], dd[3],
                                 dd[4], dd[5], dd[6], dd[7],
                                 g_wgun_mirror.load(std::memory_order_relaxed),
                                 (int)g_mirror_node[0], (int)g_mirror_node[1],
                                 (int)g_mirror_node[2],
                                 (unsigned long long)grwxr_wg_mirror);
                        s_mount_last = verdict;
                    }
                }
                // BUILD 184, probe B report: the latched part's motion since
                // the latch against our written rotation's motion, both as
                // world-space deltas, so the part's own fixed offset from
                // the bone drops out. Reads only. grep 'wroll:'.
                if (g_wroll_probe.load(std::memory_order_relaxed)) {
                    static uint64_t s_wr_cand = 0, s_wr_hits = 0, s_wr_ref_ms = 0;
                    static int      s_wr_quiet = 0;
                    static bool     s_wr_ref_ok = false;
                    static float    s_wr_mref[9] = {}, s_wr_oref[9] = {};
                    const uint64_t cand = g_wskel_cand.load(std::memory_order_relaxed);
                    const uint64_t hits = g_wr_hits.load(std::memory_order_relaxed);
                    const char* why = "";
                    if (cand != s_wr_cand) {
                        s_wr_cand = cand;
                        g_wr_node.store(0, std::memory_order_relaxed);
                        s_wr_ref_ok = false;
                        why = " (pick changed: relatch)";
                    } else if (hits == s_wr_hits) {
                        if (++s_wr_quiet >= 2) {
                            g_wr_node.store(0, std::memory_order_relaxed);
                            s_wr_ref_ok = false;
                            s_wr_quiet = 0;
                            why = " (part silent 2 s: relatch)";
                        }
                    } else {
                        s_wr_quiet = 0;
                    }
                    const uint64_t dh = hits - s_wr_hits;
                    s_wr_hits = hits;
                    float M[16] = {};
                    bool mok = false;
                    for (int t = 0; t < 3 && !mok; ++t) {
                        const uint64_t s1 = g_wr_seq.load(std::memory_order_acquire);
                        if (s1 == 0 || (s1 & 1)) continue;
                        memcpy(M, g_wr_mat, sizeof(M));
                        const uint64_t s2 = g_wr_seq.load(std::memory_order_acquire);
                        mok = (s1 == s2);
                    }
                    float q[4];
                    for (int i = 0; i < 4; ++i)
                        q[i] = bitsf(g_wr_q[i].load(std::memory_order_relaxed));
                    const float qn = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
                    const uint64_t part = g_wr_node.load(std::memory_order_relaxed);
                    if (part && mok && qn > 0.5f && isfinite(qn)) {
                        for (int i = 0; i < 4; ++i) q[i] /= qn;
                        // m3: rows = the part's local axes as world directions.
                        float m3[9];
                        bool fin = true;
                        for (int r = 0; r < 3; ++r) {
                            float n2 = 0.0f;
                            for (int c = 0; c < 3; ++c) {
                                m3[r*3+c] = M[r*4+c];
                                if (!isfinite(m3[r*3+c])) fin = false;
                                n2 += m3[r*3+c] * m3[r*3+c];
                            }
                            if (!(n2 > 0.25f && n2 < 4.0f)) fin = false;   // scaled or torn
                            const float n = sqrtf(n2);
                            if (n > 1e-6f) for (int c = 0; c < 3; ++c) m3[r*3+c] /= n;
                        }
                        // o3: rows = the bone's local axes as world directions,
                        // from our written quaternion (column j of R(q) is the
                        // world direction of local axis j).
                        float o3[9];
                        {
                            const float x = q[0], y = q[1], z = q[2], w = q[3];
                            const float R[9] = {
                                1 - 2*(y*y + z*z), 2*(x*y - z*w),     2*(x*z + y*w),
                                2*(x*y + z*w),     1 - 2*(x*x + z*z), 2*(y*z - x*w),
                                2*(x*z - y*w),     2*(y*z + x*w),     1 - 2*(x*x + y*y)};
                            for (int r = 0; r < 3; ++r)
                                for (int c = 0; c < 3; ++c) o3[r*3+c] = R[c*3+r];
                        }
                        if (fin && !s_wr_ref_ok) {
                            memcpy(s_wr_mref, m3, sizeof(m3));
                            memcpy(s_wr_oref, o3, sizeof(o3));
                            s_wr_ref_ok = true;
                            s_wr_ref_ms = GetTickCount64();
                        }
                        if (fin && s_wr_ref_ok) {
                            // With rows as local axes, cur = Rwl^T, so the
                            // world delta since the latch Rwl_cur * Rwl_ref^T
                            // is cur^T * ref, for the part and for ours.
                            auto tmul = [](const float a[9], const float b[9], float o[9]) {
                                for (int r = 0; r < 3; ++r)
                                    for (int c = 0; c < 3; ++c)
                                        o[r*3+c] = a[0*3+r]*b[0*3+c] + a[1*3+r]*b[1*3+c] + a[2*3+r]*b[2*3+c];
                            };
                            auto mulT = [](const float a[9], const float b[9], float o[9]) {
                                for (int r = 0; r < 3; ++r)
                                    for (int c = 0; c < 3; ++c)
                                        o[r*3+c] = a[r*3+0]*b[c*3+0] + a[r*3+1]*b[c*3+1] + a[r*3+2]*b[c*3+2];
                            };
                            float Dp[9], Do[9], E[9];
                            tmul(m3, s_wr_mref, Dp);
                            tmul(o3, s_wr_oref, Do);
                            mulT(Dp, Do, E);
                            float cth = 0.5f * (E[0] + E[4] + E[8] - 1.0f);
                            if (cth > 1.0f) cth = 1.0f;
                            if (cth < -1.0f) cth = -1.0f;
                            const float err = acosf(cth) * 57.29578f;
                            // The discrepancy's axis against the barrel now
                            // (our +Y in world, o3 row 1): near +-1 = a roll.
                            const float ax[3] = {E[7] - E[5], E[2] - E[6], E[3] - E[1]};
                            const float an = sqrtf(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
                            float ab = 0.0f;
                            if (an > 1e-4f) ab = (ax[0]*o3[3] + ax[1]*o3[4] + ax[2]*o3[5]) / an;
                            float cq = 0.5f * (Do[0] + Do[4] + Do[8] - 1.0f);
                            if (cq > 1.0f) cq = 1.0f;
                            if (cq < -1.0f) cq = -1.0f;
                            const float moved = acosf(cq) * 57.29578f;
                            LOG_INFO("wroll: part=%012llX +%llu/s err=%.1f deg "
                                     "axis.barrel=%+.2f | ours moved %.1f deg "
                                     "since latch %llus ago | written roll=%.0f "
                                     "deg theta=%.0f deg d=%.0f deg%s (err near "
                                     "0 while the roll changes = "
                                     "the part follows our whole rotation; err "
                                     "tracking the roll with axis.barrel near "
                                     "+-1 = the roll is dropped; BUILD 187: "
                                     "written roll must now stay inside +-180 "
                                     "and d is how far it lags the measurement, "
                                     "so a large steady d with the roll dead "
                                     "means theta itself is wrong, not the "
                                     "filter)",
                                     (unsigned long long)part,
                                     (unsigned long long)dh, err, ab, moved,
                                     (unsigned long long)((GetTickCount64() - s_wr_ref_ms) / 1000),
                                     bitsf(g_wr_roll.load(std::memory_order_relaxed)) * 57.29578f,
                                     bitsf(g_wr_theta.load(std::memory_order_relaxed)) * 57.29578f,
                                     (bitsf(g_wr_theta.load(std::memory_order_relaxed)) -
                                      bitsf(g_wr_roll.load(std::memory_order_relaxed))) * 57.29578f,
                                     why);
                        } else {
                            LOG_INFO("wroll: part=%012llX +%llu/s basis not a "
                                     "rotation this sample%s",
                                     (unsigned long long)part,
                                     (unsigned long long)dh, why);
                        }
                    } else {
                        LOG_INFO("wroll: no part latched (part=%012llX mat=%d "
                                 "q=%.2f)%s: the first part placed within "
                                 "0.15 m of our gun root latches",
                                 (unsigned long long)part, mok ? 1 : 0, qn, why);
                    }

                    // BUILD 188: what theta was built from, and what the wrist
                    // actually did, on one line so they can be compared.
                    {
                        const float th = bitsf(g_wr_theta.load(std::memory_order_relaxed));
                        const float cr = bitsf(g_ri_croll.load(std::memory_order_relaxed));
                        LOG_INFO("rollin: %s flips=%llu | theta=%+.0f croll=%+.0f diff=%+.0f deg | "
                                 "dot=%+.3f sgn=%+.3f | gn2=%.3f cn2=%.3f "
                                 "(guard 0.10) | cu=%+.2f,%+.2f,%+.2f "
                                 "barrel=%+.2f,%+.2f,%+.2f | n=%llu skips=%llu "
                                 "(croll is the CONTROLLER's own roll about its "
                                 "own ray against the world horizon, so it is "
                                 "what the wrist did, free of the gun, the swing "
                                 "and every filter. theta tracking croll means "
                                 "the measurement is honest and the fault is "
                                 "downstream. theta sweeping while croll stays "
                                 "put means theta is contaminated by something "
                                 "that is not the wrist, and the shortest-arc "
                                 "swing's up precession is the first suspect. "
                                 "gn2 or cn2 near 0.10 means the projection is "
                                 "collapsing and the angle is noise; skips "
                                 "climbing means the guard is freezing the roll). "
                                 "BUILD 189: UNFLIPPED means the correction is "
                                 "active right now and theta shown is the "
                                 "CORRECTED value, so diff should read near 0 "
                                 "even after a fall; flips counts how many times "
                                 "the engine inverted the gun's up",
                                 g_ri_flipped.load(std::memory_order_relaxed) ? "UNFLIPPED" : "direct",
                                 (unsigned long long)g_ri_flips.load(std::memory_order_relaxed),
                                 th * 57.29578f, cr * 57.29578f,
                                 (th - cr) * 57.29578f,
                                 bitsf(g_ri_dot.load(std::memory_order_relaxed)),
                                 bitsf(g_ri_sgn.load(std::memory_order_relaxed)),
                                 bitsf(g_ri_gn2.load(std::memory_order_relaxed)),
                                 bitsf(g_ri_cn2.load(std::memory_order_relaxed)),
                                 bitsf(g_ri_cu[0].load(std::memory_order_relaxed)),
                                 bitsf(g_ri_cu[1].load(std::memory_order_relaxed)),
                                 bitsf(g_ri_cu[2].load(std::memory_order_relaxed)),
                                 bitsf(g_ri_t[0].load(std::memory_order_relaxed)),
                                 bitsf(g_ri_t[1].load(std::memory_order_relaxed)),
                                 bitsf(g_ri_t[2].load(std::memory_order_relaxed)),
                                 (unsigned long long)g_ri_n.load(std::memory_order_relaxed),
                                 (unsigned long long)g_ri_skips.load(std::memory_order_relaxed));
                    }

                    // BUILD 190: what the own-the-bone path wrote, at 1 Hz.
                    if (g_ro_on.load(std::memory_order_relaxed)) {
                        const float rw = bitsf(g_ro_roll.load(std::memory_order_relaxed));
                        const float rt = bitsf(g_ro_tgt.load(std::memory_order_relaxed));
                        const float rc = bitsf(g_ro_croll.load(std::memory_order_relaxed));
                        float dd = rw - rc;
                        while (dd >  3.14159265f) dd -= 6.28318531f;
                        while (dd < -3.14159265f) dd += 6.28318531f;
                        LOG_INFO("rollown: ON wrote=%+.0f target=%+.0f croll=%+.0f "
                                 "wrote-croll=%+.0f deg | up=%+.2f,%+.2f,%+.2f "
                                 "barrel=%+.2f,%+.2f,%+.2f | n=%llu holds=%llu "
                                 "horizon=%llu (the bone's up is built from the "
                                 "controller's up about the barrel and from nothing "
                                 "the animation writes. wrote = that up against the "
                                 "horizon about the barrel; target = the controller's "
                                 "up the same way, before the filter; croll = the "
                                 "controller's own roll about its ray. wrote tracking "
                                 "croll within a few deg through a fall, a roll or a "
                                 "vehicle exit = the bone carries the wrist and nothing "
                                 "else; a steady gap is the two-hand blend or the trim, "
                                 "not the animation. holds = calls with no usable "
                                 "controller up; horizon = calls seeded upright from "
                                 "the world up)",
                                 rw * 57.29578f, rt * 57.29578f, rc * 57.29578f,
                                 dd * 57.29578f,
                                 bitsf(g_ro_up[0].load(std::memory_order_relaxed)),
                                 bitsf(g_ro_up[1].load(std::memory_order_relaxed)),
                                 bitsf(g_ro_up[2].load(std::memory_order_relaxed)),
                                 bitsf(g_ro_t[0].load(std::memory_order_relaxed)),
                                 bitsf(g_ro_t[1].load(std::memory_order_relaxed)),
                                 bitsf(g_ro_t[2].load(std::memory_order_relaxed)),
                                 (unsigned long long)g_ro_n.load(std::memory_order_relaxed),
                                 (unsigned long long)g_ro_holds.load(std::memory_order_relaxed),
                                 (unsigned long long)g_ro_horizon.load(std::memory_order_relaxed));
                    }
                }
                if ((ticks % 5) == 2) {
                    uint64_t rg = 0;
                    if (read_block(bestp + 0x220, &rg, sizeof(rg)) &&
                        rg > 0x10000 && !(rg & 7)) {
                        const int nL = rig_find_node(rg, 0xB675F36Cu);  // LeftHand
                        const int nR = rig_find_node(rg, 0x75F94D30u);  // RightHand
                        const int nH = rig_find_node(rg, 0x07C159A2u);  // Head
                        float L[3] = {}, R[3] = {}, H[3] = {};
                        const bool okL = nL >= 0 &&
                            read_bone_world(bestp, (unsigned int)nL, L);
                        const bool okR = nR >= 0 &&
                            read_bone_world(bestp, (unsigned int)nR, R);
                        const bool okH = nH >= 0 &&
                            read_bone_world(bestp, (unsigned int)nH, H);

                        auto dist = [](const float* a, const float* b) {
                            const float dx = a[0] - b[0], dy = a[1] - b[1],
                                        dz = a[2] - b[2];
                            return sqrtf(dx * dx + dy * dy + dz * dz);
                        };
                        const float lr = (okL && okR) ? dist(L, R) : -1.0f;
                        const float lh = (okL && okH) ? dist(L, H) : -1.0f;
                        const float rh = (okR && okH) ? dist(R, H) : -1.0f;

                        // One word so the headset report is unambiguous.
                        const char* verdict = "UNKNOWN";
                        if (!okL || !okR || !okH)      verdict = "UNREADABLE";
                        else if (lr < 0.05f)           verdict = "BAD(coincident)";
                        else if (lr > 2.5f)            verdict = "BAD(too far apart)";
                        else if (lh > 1.5f || rh > 1.5f) verdict = "BAD(not near head)";
                        else                           verdict = "PLAUSIBLE";

                        static int s_lastL = -2, s_lastR = -2;
                        static char s_lastv[24] = {};
                        if (nL != s_lastL || nR != s_lastR ||
                            strcmp(s_lastv, verdict) != 0) {
                            s_lastL = nL; s_lastR = nR;
                            strncpy_s(s_lastv, verdict, _TRUNCATE);
                            LOG_INFO("hands: %s  Lnode=%d Rnode=%d Hnode=%d  "
                                     "L-R=%.2fm L-H=%.2fm R-H=%.2fm",
                                     verdict, nL, nR, nH, lr, lh, rh);
                            LOG_INFO("hands: L=(%.2f %.2f %.2f) "
                                     "R=(%.2f %.2f %.2f) H=(%.2f %.2f %.2f)",
                                     L[0], L[1], L[2], R[0], R[1], R[2],
                                     H[0], H[1], H[2]);
                        }

                        // 2026-08-09: THE WEAPON MOUNT CENSUS. Two independent
                        // static passes cracked Skeleton::BipedBoneID out of
                        // the Anvil reflection tables (docs/RE-notes.md), and
                        // it turns out the character rig carries bones whose
                        // only purpose is holding a gun. Resolve them by their
                        // own CRC32 name hashes and measure each against the
                        // weapon instance the wskel census already picked.
                        //
                        // A sub-centimetre match NAMES the mount point and
                        // upgrades "the weapon is parented to the hand" from
                        // strongly inferred to verified. A miss on all of them
                        // kills the theory in one line, which is worth just as
                        // much. Read-only: rig_find_node is a binary search
                        // over a sorted map, read_bone_world only touches
                        // range-checked memory.
                        //
                        // The pair FAKE_GUNROOT and FAKE_GUNROOT_GAMEPLAY is
                        // the engine telling us outright that it keeps a
                        // VISUAL gun root and an AUTHORITATIVE one, which is
                        // skeleton rule 3 in the engine's own vocabulary.
                        // Expect to write both.
                        const uint64_t wpn =
                            g_wskel_cand.load(std::memory_order_relaxed);
                        if (wpn) {
                            float wo[3] = {};
                            if (read_block(wpn + 0x120, wo, sizeof(wo)) &&
                                isfinite(wo[0]) && isfinite(wo[1]) &&
                                isfinite(wo[2])) {
                                struct Mount { uint32_t hash; const char* name; };
                                static const Mount kMounts[] = {
                                    {0x826846F3u, "Fake_gunroot"},
                                    {0x08B4DDD5u, "FakeGunRoot_Gameplay"},
                                    {0x53135E44u, "Prop_RightHand"},
                                    {0x85562B5Cu, "Prop_LeftHand"},
                                    {0x3FB256E5u, "RightHand_Weapon_Ref"},
                                    {0xA9611103u, "LeftHand_Weapon_Ref"},
                                };
                                for (const Mount& m : kMounts) {
                                    const int n = rig_find_node(rg, m.hash);
                                    if (n < 0) {
                                        LOG_INFO("mount: %-22s NOT ON THIS RIG",
                                                 m.name);
                                        continue;
                                    }
                                    float p[3] = {};
                                    if (!read_bone_world(bestp,
                                                         (unsigned int)n, p)) {
                                        LOG_INFO("mount: %-22s node=%d "
                                                 "UNREADABLE", m.name, n);
                                        continue;
                                    }
                                    const float d = dist(p, wo);
                                    LOG_INFO("mount: %-22s node=%-4d "
                                             "pos=(%.2f %.2f %.2f) "
                                             "d(weapon)=%.3fm%s",
                                             m.name, n, p[0], p[1], p[2], d,
                                             d < 0.05f
                                                 ? "   <<< THIS IS THE MOUNT"
                                                 : "");
                                }

                                // BUILD 79: which axis of the gun-root bone is
                                // the barrel. While the player aims, the gun
                                // points where the GAME aims, so the bone axis
                                // whose world direction tracks view_fwd is the
                                // barrel. Signed dots cover all six candidates
                                // at once: the largest magnitude names the
                                // axis and its sign names the direction.
                                //
                                // Every gate reports itself (the rule this
                                // project wrote after one counter behind five
                                // early returns cost three headset runs).
                                if (g_wbaxis.load(std::memory_order_relaxed)) {
                                    const int n10 =
                                        rig_find_node(rg, kGunRootGameplay);
                                    float wx[3], wy[3], wz[3], vf[3];
                                    const bool okn = n10 >= 0;
                                    const bool oka =
                                        okn && read_bone_world_axes(
                                                   bestp, (unsigned int)n10,
                                                   wx, wy, wz);
                                    const bool okv = aimtrace::view_fwd(vf);
                                    float vn = 0.0f;
                                    if (okv)
                                        vn = sqrtf(vf[0] * vf[0] +
                                                   vf[1] * vf[1] +
                                                   vf[2] * vf[2]);
                                    if (oka && okv && vn > 1e-6f) {
                                        vf[0] /= vn; vf[1] /= vn; vf[2] /= vn;
                                        auto dot = [&](const float* a) {
                                            return a[0] * vf[0] +
                                                   a[1] * vf[1] +
                                                   a[2] * vf[2];
                                        };
                                        const float dx = dot(wx);
                                        const float dy = dot(wy);
                                        const float dz = dot(wz);
                                        static double sx = 0, sy = 0, sz = 0;
                                        static int ns = 0;
                                        sx += dx; sy += dy; sz += dz; ++ns;
                                        float cr[3], cd = 0.0f;
                                        const bool okc = aimtrace::ctrl_ray(cr);
                                        if (okc) {
                                            const float cn = sqrtf(
                                                cr[0] * cr[0] + cr[1] * cr[1] +
                                                cr[2] * cr[2]);
                                            if (cn > 1e-6f)
                                                cd = (cr[0] * vf[0] +
                                                      cr[1] * vf[1] +
                                                      cr[2] * vf[2]) / cn;
                                        }
                                        LOG_INFO(
                                            "wbax: node=%d n=%d | dot(view_fwd)"
                                            " X=%+.3f Y=%+.3f Z=%+.3f | mean "
                                            "X=%+.3f Y=%+.3f Z=%+.3f | "
                                            "ctrl.view=%+.3f%s",
                                            n10, ns, dx, dy, dz,
                                            (float)(sx / ns), (float)(sy / ns),
                                            (float)(sz / ns), cd,
                                            okc ? "" : " NO-CTRL-RAY");
                                    } else {
                                        LOG_INFO("wbax: no sample. node10=%s "
                                                 "axes=%s view_fwd=%s",
                                                 okn ? "ok" : "NOT ON RIG",
                                                 oka ? "ok" : "unreadable",
                                                 (okv && vn > 1e-6f)
                                                     ? "ok" : "not published");
                                    }
                                }

                                // BUILD 116: the gun-root arm block that lived here moved
                                // to the top of this tick, to the bestp latch. It never
                                // belonged inside the hand census: arming acts on the
                                // PLAYER rig's gun-root bone and reads nothing from either
                                // census. Sitting here made it a three-way coincidence on a
                                // single 1 Hz tick, and every run that ever armed armed
                                // EXACTLY ONCE. See docs/DESIGN-motion-controls.md.
                            }
                        }
                    }
                }

                // (The builds 28-32 effector walk that lived here was removed
                // per rule 6: its route is EXHAUSTED. It verified the root
                // global 0x04B132B0, verified the +0x50 list and the
                // entry -> +0x10 -> +0xD8 chain, identified the reached
                // objects as skeleton instances, and then refuted every
                // single-indirection placement of the HumanIK effector array
                // on them: not embedded (builds 30/31), not behind any
                // pointer field of the skeleton or of X (build 32). See
                // CURRENT-STATE.md builds 28-32 and the RE-notes session-23
                // sections. The IK workstream continues on the HumanIK-native
                // route: the rig instance pool and the debug flag registry.)


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
                             "reads=%llu rejects=%llu | place fwd=%.2f "
                             "clamp=%.2f(%llu) ema=%.0f/%.0fms snaps=%llu",
                             g_head_pos[0], g_head_pos[1], g_head_pos[2],
                             g_head_dz,
                             (unsigned long long)g_head_reads.load(
                                 std::memory_order_relaxed),
                             (unsigned long long)g_head_rejects.load(
                                 std::memory_order_relaxed),
                             headpose::fp_fwd(), headpose::fp_clamp(),
                             (unsigned long long)g_fp_clamped.load(
                                 std::memory_order_relaxed),
                             headpose::fp_smooth(), headpose::fp_smooth_z(),
                             (unsigned long long)g_fp_snaps.load(
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

                // BUILD 113: RING PROBE. LOG ONLY, no behaviour change.
                //
                // The whole jitter chain hangs off this branch being taken:
                // no ring member owned by pent means no pin, no pin means no
                // anchor, no anchor means the 11c push lever, and the lever is
                // what the capture race swings. Same DLL anchored fine on
                // 2026-08-16, so this is state dependent, not a code
                // regression.
                //
                // Two candidate causes and they need different fixes:
                //   (a) pent is wrong, so we are comparing against the wrong
                //       entity and the player's skeletons are sitting in the
                //       ring unmatched.
                //   (b) the ring genuinely holds no player skeleton.
                // Printing what the ring actually contains separates them in
                // one line: if a single owner dominates and it is NOT pent,
                // that is (a) and the answer is probably that owner.
                //
                // Drain thread, throttled to this same 1-in-10 tick, so rule 8
                // is satisfied. Reads are guarded exactly like the selection
                // loop above.
                uint64_t seen[4] = {0, 0, 0, 0};
                uint32_t hits[4] = {0, 0, 0, 0};
                int      distinct = 0;
                int      live = 0, unread = 0;
                for (uint32_t i = 0; i < kRingSize; ++i) {
                    const uint64_t p =
                        g_skel_ring[i].load(std::memory_order_relaxed);
                    if (!p) continue;
                    ++live;
                    uint64_t own = 0;
                    if (!read_block(p + 0x10, &own, sizeof(own))) {
                        ++unread;
                        continue;
                    }
                    int k = 0;
                    for (; k < distinct; ++k)
                        if (seen[k] == own) { ++hits[k]; break; }
                    if (k == distinct && distinct < 4) {
                        seen[distinct] = own;
                        hits[distinct] = 1;
                        ++distinct;
                    }
                }
                LOG_INFO("fp: RING PROBE live=%d unread=%d distinct=%d%s | "
                         "owners: %012llX x%u, %012llX x%u, %012llX x%u, "
                         "%012llX x%u | pent=%012llX playercomp=%012llX",
                         live, unread, distinct,
                         distinct >= 4 ? " (capped)" : "",
                         (unsigned long long)seen[0], hits[0],
                         (unsigned long long)seen[1], hits[1],
                         (unsigned long long)seen[2], hits[2],
                         (unsigned long long)seen[3], hits[3],
                         (unsigned long long)pent,
                         (unsigned long long)g_playercomp.load(
                             std::memory_order_relaxed));
            }
            // BUILD 130: THE ONE LINE THAT DECIDES WHERE THE CAMERA IS, and
            // until now it was printed only inside the branch that is not
            // being taken.
            //
            // `[VERIFIED, log grwxr-27148, build 129b]` the owner ran with
            // first person confirmed on (`hide: FORCING (fp on)`) and reported
            // the camera still jumping. The log cannot say whether the camera
            // was anchored to his head or had fallen back to the 11c chase-rig
            // push, because BOTH status lines (`camera ANCHORED...` at the top
            // of this block, and `fp: HEAD world=...`) sit inside the
            // ring-member-FOUND branch, and every sample in that run took the
            // NOT-found branch. `fp: HEAD world` printed 0 times in a 2 minute
            // run with the anchor supposedly live.
            //
            // The anchor itself does NOT need the ring: it runs off the HELD
            // pin (`headpose::player_obj()`) and the published head node, both
            // of which survive eviction. So the camera may well be anchored
            // while this diagnostic says nothing at all. That ambiguity is the
            // reason Fault A has stayed open across ten builds, and it costs
            // three lines to end.
            //
            // BUILD 132, CX-001: the cost of the anchor delay, stated once, in
            // seconds, so nobody has to subtract two log timestamps by hand
            // again. Drain thread, same place as the status line below, so it
            // is rule 8 safe for the same reason.
            //
            // This is the number that decides whether a run is interpretable:
            // until the anchor engages the mod places nothing, so gun counters
            // from before this point measure nothing. See QUEUE.md item 1.
            if (g_fp_anchored.load(std::memory_order_relaxed) &&
                !g_anchor_first_ms.load(std::memory_order_relaxed)) {
                const uint64_t nowms = (uint64_t)GetTickCount64();
                uint64_t expect0 = 0;
                if (g_anchor_first_ms.compare_exchange_strong(
                        expect0, nowms, std::memory_order_relaxed)) {
                    const uint64_t p0 =
                        g_pin_first_ms.load(std::memory_order_relaxed);
                    LOG_INFO("fp: ANCHOR ENGAGED after %llu ms from the first "
                             "pin (%u distinct entities pinned on the way). "
                             "Everything before this point placed nothing, so "
                             "no gun counter from before it is interpretable.",
                             (unsigned long long)(p0 && nowms > p0 ? nowms - p0 : 0),
                             g_pin_changes.load(std::memory_order_relaxed));

                    // BUILD 134: say it, do not just log it.
                    //
                    // The tester cannot read a log from inside the headset, so
                    // "wait about a minute before you shoot" has been a stopwatch
                    // exercise, and the measured delay is not even constant: 35.4
                    // to 50.6 s across the corpus, with one run that never
                    // anchored at all in 68 s. Worse, "Shot test armed" is spoken
                    // at STARTUP, which invites firing before the anchor exists,
                    // and every shot before this moment measures nothing.
                    //
                    // One-shot by the compare_exchange above, drain thread, and
                    // say() only copies into a buffer another thread owns, so
                    // this is rule 8 safe for exactly the same reason the
                    // LOG_INFO beside it is.
                    voice::say("Anchor engaged. You can fire now.");
                }
            }

            // Printed every 5th tick, drain thread, rule 8 safe.
            if ((ticks % 5) == 0)
                LOG_INFO("fp: ANCHOR %s | head node=%u reads=%llu rejects=%llu "
                         "dz=%+.2fm | pin=%012llX ring=%s",
                         g_fp_anchored.load(std::memory_order_relaxed)
                             ? "HEAD BONE (anchored)"
                             : "11c push (NOT anchored, camera rides the "
                               "third-person rig)",
                         headpose::head_node(),
                         (unsigned long long)g_head_reads.load(
                             std::memory_order_relaxed),
                         (unsigned long long)g_head_rejects.load(
                             std::memory_order_relaxed),
                         g_head_dz,
                         (unsigned long long)headpose::player_obj(),
                         g_head_valid ? "head live" : "head NOT read");

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
            // BUILD 130b: DO NOT ZERO THE PIN HERE. Zeroing it is what has
            // been putting the camera on the third-person rig every time
            // first person is switched on.
            //
            // The chain: this cleared the pin, and re-acquisition then needs
            // the 128-entry skeleton ring to still hold a member owned by the
            // player entity. `[VERIFIED, logs grwxr-27148 and grwxr-25296]`
            // that lookup fails on EVERY sample once the world populates
            // ("fp: pin(entity) no ring member for entity ..."), because the
            // ring is a FIFO of all skeletons and the player's is evicted by
            // the other 127. So the pin stayed 0, the anchored path is gated
            // behind `if (player_obj())`, and the camera fell through to the
            // 11c push, which rides the chase rig and swings with it.
            //
            // The owner has reported the camera jumping in first person for
            // weeks, and the head bone was never the problem: node 59 resolves
            // every run. This one assignment is what disarmed the anchor at
            // the exact moment first person was requested.
            //
            // Acquisition is still armed, so a better pin replaces this one as
            // soon as the ring offers it. Keeping the old pin cannot be worse
            // than keeping none: the anchored path already sanity-checks the
            // object every frame (finite, and within 12 m of the camera) and
            // falls back to the push by itself if the pin has gone stale.
            s_have_last = false;
            LOG_INFO("fp: acquisition armed by FP toggle, pin %012llX KEPT "
                     "live until a better one is found (build 130b: clearing "
                     "it here is what dropped the camera onto the "
                     "third-person rig)",
                     (unsigned long long)headpose::player_obj());
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

    (void)0;  // Build 91: the head-compose telemetry moved to the TOP of this
              // function. See the note there. This tail is now empty.
}

// Build 91: THE TELEMETRY WAS ONLY EVER PRINTED FROM MENUS.
//
// This block used to sit at the END of snap_drain(). The "player entity is
// live" branch above ends in an unconditional `return`, so from the moment the
// player entity latches, which is a few seconds into any session, this never
// ran again. Every `head compose` and `aer` line in every log we have was
// therefore sampled BEFORE gameplay, and in the 2026-08-15 run the last one is
// at 03:35:32 while the first person the tester graded starts at 03:36:2x.
//
// That is how three sessions were graded on menu telemetry, including the
// vs-write reading that was quoted as evidence about whether the engine
// refreshes the camera row during play. It does not answer that question and
// never did.
//
// It is now called from the top of snap_drain(), before any early return. It
// only reads counters, so it has no dependency on the rest of the tick.
void drain_head_telemetry(int ticks) {
    if ((ticks % 10) == 0) {
        const unsigned long long w  = g_head_writes.load(std::memory_order_relaxed);
        const unsigned long long fr = g_head_frames.load(std::memory_order_relaxed);
        const unsigned long long fl = g_head_write_fails.load(std::memory_order_relaxed);
        const unsigned long long sw =
            g_head_sel_writes.load(std::memory_order_relaxed);
        if (fr) {
            LOG_INFO("head compose: %llu writes over %llu frames (%.2f calls/frame), %llu failed, source %s",
                     w, fr, (double)w / (double)fr, fl,
                     sw ? "SELECTOR (build 89 stand-in)" : "on_calc_mvp");
            // Build 10b.1: the ring occupancy IS the build-to-present pipeline
            // depth, the number parity-based eye matching guessed wrong.
            // Build 93: depth oscillates 0 to 1 every frame by construction
            // (the pop precedes the push within one Present cycle), so neither
            // value is a symptom. drops is the one that matters: any nonzero
            // value means the eye parity flipped and stayed flipped.
            LOG_INFO("aer: pipeline depth %d, pops tagged=%llu mono=%llu drops=%llu",
                     headpose::eye_tag_depth(),
                     headpose::pops_tagged(), headpose::pops_mono(),
                     headpose::tag_drops());
            // Build 10b.2: vs-write ~0 means the engine is NOT refreshing the
            // position row and our eye offsets are compounding (see the
            // comment at g_diag_written). step is capture-to-capture motion.
            LOG_INFO("aer: basepos step last=%.4f max=%.4f, vs-write last=%.4f max=%.4f, cap rescues=%llu",
                     g_diag_step_last, g_diag_step_max,
                     g_diag_vsw_last, g_diag_vsw_max, g_cap_rescues);
            // BUILD 98: the gun-origin fix, grading itself. gap is how far the
            // LIVE camera row is from the frame's engine latch at the moment a
            // controller publish reads it, which is precisely the jitter that
            // used to reach the gun. A large gap with latch climbing and live
            // at or near zero is the fix working: the error exists and nothing
            // is consuming it. live climbing while the camera write is on means
            // the latch is going stale and the jitter is back.
            LOG_INFO("base: latch=%llu live=%llu, live-vs-latch gap last=%.4f max=%.4f",
                     g_bf_latch.load(std::memory_order_relaxed),
                     g_bf_live.load(std::memory_order_relaxed),
                     g_bf_gap_last, g_bf_gap_max);
        } else if (g_calls[kOnCalcMvpProbe].load(std::memory_order_relaxed) == 0 &&
                   !headpose::cam_selector_pose()) {
            // Build 89: name the exact reason. on_calc_mvp is the only other
            // caller of write_pose_head, and on this binary its row is not
            // derived, so with the stand-in off there is nobody to compose:
            // no first person and no stereo separation.
            LOG_INFO("head compose: idle, and on_calc_mvp is NOT hooked "
                     "(0 calls). Nothing can write the camera pose: FP is a "
                     "no-op and both eyes get the same image. Set "
                     "cam_selector_pose = 1 in grwxr.cfg to route it through "
                     "the selector.");
        } else {
            LOG_INFO("head compose: idle (no head pose published; camera untouched)");
        }
        if (!fr) {
            // BUILD 109: SAY WHAT IS MISSING AND WHY.
            //
            // The aer: and base: families are printed inside the branch
            // above, so when the camera pose write is off they do not
            // appear AT ALL. Three subsystems telemetry gated behind a
            // fourth subsystem counter, and the failure is silent:
            // `[VERIFIED]` a reader of grwxr-31412.log concluded build
            // 100 had never been graded, because it grepped for
            // "base: latch" and found nothing. It had been graded, in a
            // different run, and passed.
            //
            // ABSENCE MUST NEVER LOOK LIKE A RESULT. A family that is
            // suppressed says so, names itself, and names the switch
            // that would bring it back.
            LOG_INFO("aer/base: SUPPRESSED this tick, not absent. Both "
                     "families print only while the camera pose write is "
                     "running, and it composed 0 frames. cam_selector_pose "
                     "= %d. Turn it on (or press Numpad 0) and the "
                     "stereo-pipeline and gun-origin telemetry come back.",
                     headpose::cam_selector_pose() ? 1 : 0);
        }
    }
}

}  // namespace

// Build 96: see CameraProbe.h. Plain relaxed stores on counters that are only
// ever incremented elsewhere and only ever read by the drain, so a torn or
// slightly stale read is harmless and no lock is needed. Safe from any thread.
void reset_head_telemetry() {
    g_head_writes.store(0, std::memory_order_relaxed);
    g_head_write_fails.store(0, std::memory_order_relaxed);
    g_head_frames.store(0, std::memory_order_relaxed);
    g_head_sel_writes.store(0, std::memory_order_relaxed);
}

// The live read off the camera object. This IS the old base_frame body,
// renamed; pos may be wanted without rot, so rot is allowed to be null.
static bool base_frame_live(float rot[9], float pos[3]);
static bool base_pos_live(float out[3]);

// -----------------------------------------------------------------------
// BUILD 100 CORRECTION, READ THIS FIRST.
//
// Build 98 latched the wrong value and made the symptom worse rather than
// better. It served `g_base_pos`, which is the ENGINE's chase camera captured
// BEFORE our write. In first person that sits about 1.7 m up and behind the
// head, so the weapon was placed from an origin 1.7 m high, and whenever the
// latch went stale the fallback returned the composed viewpoint instead, so the
// gun still alternated between two values 1.9 m apart.
//
// `[VERIFIED, headset, 2026-08-15]` tester on build 99: "THE GUN IS TOO FAR UP
// AND JUST PHASES IN AND OUT OF REALITY... THE JUTTER IS MUCH WORSE." Gun too
// high, gun flickering between two places, and judder worse than before are all
// three the same inversion.
//
// What every consumer of these two functions wants is THE CAMERA THE FRAME WAS
// RENDERED FROM, because they place hands and weapons relative to the player's
// eyes. That is `pos` at the bottom of write_pose_head, not `g_base_pos` at the
// top of it. Build 100 latches that instead, and the reasoning below about
// frame-stability, freshness and ordering is unchanged and still correct.
//
// The lesson worth keeping: the frame-stability argument was right and the
// SOURCE VALUE was wrong, and a build that is right about the mechanism and
// wrong about the input looks exactly like a build that is wrong about both.
// -----------------------------------------------------------------------
//
// BUILD 98: THE PER-FRAME CAMERA, and why base_pos and base_frame stop
// reading the camera object directly.
//
// `[VERIFIED, headset, 2026-08-15]` The tester reported the gun jittering while
// moving, new since the camera work, and pressing NUMPAD 0 (which turns the
// camera pose write off) removed it completely. That is the control passing.
//
// THE MECHANISM, end to end, all of it in this tree:
//
//   grwxr_wgun_apply places the gun at aimtrace::ctrl_pos(), clamped to
//   wgun_pos_clamp of the engine's own placement (CameraProbe.cpp, the POSITION
//   block). ctrl_pos is published in VRMirror.cpp as base_frame()'s position
//   plus the head-local controller offset. base_frame() read Camera+0x000 row 3
//   LIVE. And since build 89 write_pose_head WRITES that same row, 3.00 times
//   per frame by the drain's own count, while the engine restores it once.
//
// So the origin the weapon is placed from was a row we are ourselves editing
// mid-frame, sampled from the present thread with no ordering against either
// writer. Two candidate values live in it: the engine's chase camera and our
// composed first-person viewpoint.
//
// THE AMPLITUDE IS MEASURED, in grwxr-13140.log, with the player standing
// still: `basepos step last=0.0053, vs-write last=1.9147`. Frame-to-frame
// camera motion of 5 mm, against 1.91 m between the captured row and the row we
// wrote. Clamped to 0.60 m and one-poled at 0.35 that is a shake, not a jump,
// which is exactly what it looked like.
//
// THE FIX, and why it is this one:
//
// write_pose_head already captures the engine's rows once per frame, BEFORE its
// own write, into g_base / g_base_pos. That capture is the value every one of
// these callers actually wants, and it is single valued for the whole frame by
// construction. Serve it, and fall back to the live read whenever it is stale.
//
// Both branches return the ENGINE's camera. When the write is running the latch
// is fresh; when it is not (menus, first person off, the flat-scope stand-down,
// NUMPAD 0) nothing writes the row, so the live read is the same value. There
// is no configuration in which a caller sees our own write any more.
//
// SCOPE. Every caller of these two functions is ours: base_frame feeds the five
// controller publishes in VRMirror.cpp (ctrl_ray, ctrl_pos, ctrl_pos_l,
// ctrl_up, and wp::set_write) and base_pos feeds WeaponProbe's proximity
// tables. The camera-write path itself does NOT go through here, it uses g_base
// and g_base_pos directly, so this cannot disturb first person or the eye
// offset. This also closes the defect recorded at the end of session 29, where
// identical weapon handles reported d=0.38 then d=1.72, a uniform 1.35 m step
// with no object motion.
//
// FRESHNESS. One frame of tolerance, not zero: the present thread reads while
// the engine thread may be part way through the next frame's build, and
// rejecting that would drop the latch on a frame boundary every time and put
// the jitter back at a lower rate. One frame of camera motion is the 5 mm the
// log measured.
//
// ORDERING. The stamp is stored with release after the rows are filled and
// loaded with acquire before they are copied, so a reader that sees frame f
// sees frame f's rows. The rows themselves stay plain floats, unsynchronized,
// exactly as documented in CameraProbe.h: a torn read costs centimetres and
// every caller here tolerates that. No lock, no allocation, rule 8 clean.
// -----------------------------------------------------------------------
// BUILD 277 (log only): which camera the last base_frame on THIS thread returned. 0 the camera built this
// frame, 1 the one built the frame before (the latch's one frame of tolerance), 2 the live row, 255 none.
static thread_local uint8_t t_bf_kind = 255;

static bool base_frame_engine(float rot[9], float pos[3]) {
    const uint64_t stamp = g_vr_cam_stamp.load(std::memory_order_acquire);
    if (stamp) {
        const uint64_t now = d3d11::frame_count();
        if (now >= stamp && (now - stamp) <= 1) {
            float p[3], r[9];
            memcpy(r, g_vr_cam_rot, sizeof(r));
            memcpy(p, g_vr_cam_pos, sizeof(p));
            // The same sanity gate the live read uses. A latch captured during
            // a level transition holds the same readable garbage the live row
            // would, and it must fail the same way rather than silently.
            const float lim = 1.0e6f;
            bool ok = p[0] == p[0] && p[1] == p[1] && p[2] == p[2] &&
                      p[0] > -lim && p[0] < lim &&
                      p[1] > -lim && p[1] < lim &&
                      p[2] > -lim && p[2] < lim;
            for (int i = 0; ok && i < 3; ++i) {
                const float len2 = r[i * 3 + 0] * r[i * 3 + 0] +
                                   r[i * 3 + 1] * r[i * 3 + 1] +
                                   r[i * 3 + 2] * r[i * 3 + 2];
                if (!(len2 > 0.81f && len2 < 1.21f)) ok = false;
            }
            if (ok) {
                // Telemetry only: how far the live row is from the latch right
                // now. This is the jitter that used to reach the gun, and
                // printing it means the next run grades this fix instead of
                // relying on the tester's eye a second time.
                //
                // BUILD 102: SAMPLED, 1 call in 64. This telemetry did a SECOND
                // full camera read plus a square root on EVERY call, and its
                // busiest consumer runs at ~13,500 calls a second, so it was
                // paying for about 13,500 redundant camera reads a second to
                // feed one log line printed once every ten seconds. The line
                // graded builds 98 and 100 correctly off six samples a run; it
                // does not need thirteen thousand a second.
                static uint32_t s_gap_tick = 0;
                if ((++s_gap_tick & 63u) == 0) {
                    float lp[3];
                    if (base_frame_live(nullptr, lp)) {
                        g_bf_gap_last = dist3(p, lp);
                        if (g_bf_gap_last > g_bf_gap_max)
                            g_bf_gap_max = g_bf_gap_last;
                    }
                }
                memcpy(rot, r, sizeof(r));
                memcpy(pos, p, sizeof(p));
                g_bf_latch.fetch_add(1, std::memory_order_relaxed);
                t_bf_kind = (uint8_t)(now - stamp);   // BUILD 277: 0 = this frame's camera, 1 = the previous one
                return true;
            }
        }
    }
    g_bf_live.fetch_add(1, std::memory_order_relaxed);
    t_bf_kind = 2;                                    // BUILD 277: fell back to the live camera row
    return base_frame_live(rot, pos);
}

// Build 39.1: see CameraProbe.h.
//
// FIXED DEFECT (found before build 39 was ever run): the first version read
// g_base_pos, which is written ONLY inside write_pose_head, which runs ONLY
// when headpose::read() succeeds, i.e. only while VR is armed and publishing.
// Everywhere else (menus, loading, before the session arms, headset asleep,
// any desktop run) it stays {0,0,0}, every world position is then thousands of
// metres away, and a proximity probe silently reports NOTHING. That reads as
// "the subsystem does not place this object" when the truth is "we had no
// camera position": a false negative that costs a test run to disprove.
//
// The authoritative source is the camera object itself. g_player_cam is stored
// on EVERY mode-0 on_calc_mvp call, with no VR precondition, and the engine
// keeps Camera+0x000's row 3 current every frame. Read that, and say out loud
// whether the read worked.
// BUILD 98: the latch first, for the reason in the long comment above
// base_frame_engine. base_frame_engine falls back to the live read itself, so
// this keeps every guarantee the comment below describes; the only change is
// WHICH of two values in the same row is returned while we are writing it.
uint64_t player_camera() { return g_player_cam.load(std::memory_order_relaxed); }

bool base_pos(float out[3]) {
    float rot[9];
    if (base_frame_engine(rot, out)) return true;
    // base_frame_engine has already tried the latch and the live read, but it
    // rejects a frame whose ROTATION rows are not unit, which base_pos never
    // cared about, and it has no published-copy tail. Keep both by falling
    // through to the original body, so a caller that used to get an answer
    // still gets exactly that answer.
    return base_pos_live(out);
}

// The live read. Kept as its own function because base_frame_engine falls back
// to it and because the reasoning below is still the reasoning for it.
static bool base_pos_live(float out[3]) {
    const uint64_t cam = g_player_cam.load(std::memory_order_relaxed);
    if (cam) {
        __try {
            const float* m = (const float*)(cam + kOffPose);
            const float x = m[12], y = m[13], z = m[14];
            // BUILD 40: SANITY-CHECK THE READ. Measured 2026-08-05 (log
            // grwxr-25496): during a level transition this returned
            // (6.2e22, 1.0e15, 9.2e5). The pointer was still readable, so SEH
            // caught nothing; the camera object was simply stale or mid-swap.
            // Garbage that READS FINE is the dangerous case, because every
            // distance computed from it is silently wrong rather than absent.
            // Wildlands' world is tens of km, so anything past 1e6 m or any
            // non-finite value is not a camera position.
            const float lim = 1.0e6f;
            if (x == x && y == y && z == z &&          // not NaN
                x > -lim && x < lim &&
                y > -lim && y < lim &&
                z > -lim && z < lim) {
                out[0] = x; out[1] = y; out[2] = z;
                return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // fall through to the published copy
        }
    }
    out[0] = g_base_pos[0];
    out[1] = g_base_pos[1];
    out[2] = g_base_pos[2];
    return (g_base_pos[0] != 0.0f || g_base_pos[1] != 0.0f ||
            g_base_pos[2] != 0.0f);
}

// BUILD 98: the public entry now prefers the per-frame engine latch. See the
// long comment above base_frame_engine for the headset result that forced this
// and for why every caller wants the latch.
bool base_frame(float rot[9], float pos[3]) {
    return base_frame_engine(rot, pos);
}

// BUILD 261: the vehicle camera switches (CameraProbe.h).

// BUILD 262: the vehicle compose record, GRWVR\vehyaw-<pid>.csv. Init thread only. Columns: the built frame, the raw
// pose-root heading, the tracked heading the camera used, the engine camera's heading, and the head's yaw relative to
// the car, all in degrees.
void veh_dump_csv() {
    const uint32_t n = g_vring_n.load(std::memory_order_acquire);
    if (n == 0) return;
    const uint32_t cnt = n < kVehRing ? n : kVehRing;
    const std::wstring path = log::data_dir() + L"\\vehyaw-" + std::to_wstring(GetCurrentProcessId()) + L".csv";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return;
    fprintf(f, "frame,raw_deg,filt_deg,cam_deg,head_deg\n");
    for (uint32_t i = 0; i < cnt; ++i) {
        const VehRow& r = g_vring[(n - cnt + i) & (kVehRing - 1)];
        fprintf(f, "%llu,%.4f,%.4f,%.4f,%.4f\n", (unsigned long long)r.frame, r.raw * 57.29578f, r.filt * 57.29578f,
                r.cam * 57.29578f, r.head * 57.29578f);
    }
    fclose(f);
}
// BUILD 267: the exact head orientation the build that was composed at Present count `frame` used, for the layer label. False
// when no such build is on record (the caller falls back to the lag ring and the label is not shifted).
bool veh_build_label(uint64_t frame, float q_out[4]) {
    const uint64_t n = g_brec_n.load(std::memory_order_acquire);
    for (uint64_t i = 0; i < 16 && i < n; ++i) {
        const BuildRec& r = g_brec[(n - 1 - i) & 15];
        if (r.frame == frame) {
            memcpy(q_out, r.Q, 16);
            g_mvp_label_hit.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    g_mvp_label_miss.fetch_add(1, std::memory_order_relaxed);
    return false;
}
bool veh_hook_drives() { return mvp_drives(); }
// BUILD 270: the engine-frame counter for the gtrec rows (one plain read of the hook thread's counter; log only).
uint32_t build_seq_now() { return (uint32_t)*(const volatile uint64_t*)&g_build_seq; }
void set_mvp_observe(int on) { g_mvp_observe.store(on ? 1 : 0, std::memory_order_relaxed); }
void set_mvp_drive(int on) { g_mvp_drive.store(on ? 1 : 0, std::memory_order_relaxed); }
void mvp_stats(MvpStats* s) {
    s->live = mvp::live();
    for (int i = 0; i < 3; ++i) s->calls[i] = g_mvp_calls[i].load(std::memory_order_relaxed);
    s->builds_one = g_mvp_per_build[0].load(std::memory_order_relaxed);
    s->builds_two = g_mvp_per_build[1].load(std::memory_order_relaxed);
    s->cam2_first = g_mvp_cam2_first.load(std::memory_order_relaxed);
    s->same_tid = g_mvp_same_tid.load(std::memory_order_relaxed);
    s->diff_tid = g_mvp_diff_tid.load(std::memory_order_relaxed);
    s->rows_eq = g_mvp_rows_eq.load(std::memory_order_relaxed);
    s->rows_ne = g_mvp_rows_ne.load(std::memory_order_relaxed);
    s->label_hit = g_mvp_label_hit.load(std::memory_order_relaxed);
    s->label_miss = g_mvp_label_miss.load(std::memory_order_relaxed);
    s->site_a = g_mvp_site[0].load(std::memory_order_relaxed);
    s->site_b = g_mvp_site[1].load(std::memory_order_relaxed);
    s->site_other = g_mvp_site[2].load(std::memory_order_relaxed);
}

// BUILD 264: the frame's vehicle compose, made ONCE at the graph-frame builder entry (Cam2.cpp), which runs before the engine's
// visibility work and before either camera's selector call. Reads the head now, reads the body heading now, steps the heading
// tracker once, composes the rotation, and leaves it (with the head matrix and the XR quaternion it used) for the frame's first
// selector call to take. False (nothing done, the selector composes as before) when the vehicle compose is off or anything is
// unreadable.
bool veh_begin_frame(float out[9]) {
    if (g_veh_on.load(std::memory_order_relaxed) == 0) return false;
    float H[9], Q[4];
    if (!headpose::read(H, Q)) return false;
    const uint64_t obj = (uint64_t)headpose::player_obj();
    float ax[9];
    if (!obj || !read_pose_root_axes(obj, ax)) return false;
    if (!(sqrtf(ax[3] * ax[3] + ax[4] * ax[4]) > 0.7f)) return false;
    const unsigned ep = headpose::ref_epoch();
    if (ep != g_veh_epoch) { g_veh_epoch = ep; g_veh_yaw0 = 0.0f; }   // recentered on purpose
    veh_heading_update(ax, H);
    veh_compose_rot(H, g_veh_B, g_vb_out);
    memcpy(g_vb_H, H, sizeof(g_vb_H));
    memcpy(g_vb_Q, Q, sizeof(g_vb_Q));
    memcpy(out, g_vb_out, sizeof(g_vb_out));
    g_vb_begins.fetch_add(1, std::memory_order_relaxed);
    g_vb_fresh.store(1, std::memory_order_release);
    return true;
}
void veh_note_prewrite(const float out[9]) {
    memcpy(g_pre_wrote, out, sizeof(g_pre_wrote));
    g_pre_writes.fetch_add(1, std::memory_order_relaxed);
    g_pre_pending.store(1, std::memory_order_relaxed);
}
void set_veh_smooth(float a) {
    if (!(a >= 0.0f)) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    g_veh_smooth.store(a, std::memory_order_relaxed);
}
float veh_smooth() { return g_veh_smooth.load(std::memory_order_relaxed); }
void set_veh_camera(int on) { g_veh_cam.store(on ? 1 : 0, std::memory_order_relaxed); }
bool veh_on() { return g_veh_on.load(std::memory_order_relaxed) != 0; }
void set_head_roll_level(int on) { g_roll_level.store(on ? 1 : 0, std::memory_order_relaxed); }

// BUILD 260: the vehicle instrument (CameraProbe.h VehSample). Init thread, 1 Hz, read only, every read guarded by
// read_block. Nothing here feeds back into any camera, pose or aim write.
bool veh_sample(VehSample* s) {
    memset(s, 0, sizeof(*s));
    s->cam_dist = g_veh_dist.load(std::memory_order_relaxed);
    s->anchored = g_fp_anchored.load(std::memory_order_relaxed);
    s->veh_on = g_veh_on.load(std::memory_order_relaxed) != 0;
    s->veh_edges = g_veh_edges.load(std::memory_order_relaxed);
    s->veh_rootfail = g_veh_rootfail.load(std::memory_order_relaxed);
    s->veh_smooth = g_veh_smooth.load(std::memory_order_relaxed);
    s->pre_writes = g_pre_writes.load(std::memory_order_relaxed);
    s->pre_kept = g_pre_kept.load(std::memory_order_relaxed);
    s->pre_lost = g_pre_lost.load(std::memory_order_relaxed);
    s->vb_used = g_vb_used_n.load(std::memory_order_relaxed);
    s->vb_missed = g_vb_missed_n.load(std::memory_order_relaxed);
    float dz = 0.0f;
    if (headpose::head_dz(&dz)) s->dz = dz;
    const uint64_t obj = (uint64_t)headpose::player_obj();
    if (!obj) return false;
    s->have_obj = true;
    float o[3];
    if (read_block(obj + 0x120, o, sizeof(o)) && isfinite(o[0]) && isfinite(o[1]) && isfinite(o[2])) {
        s->origin_ok = true;
        memcpy(s->origin, o, sizeof(o));
    }
    // The pose root: the owner entity's world rotation as the skeleton carries it (Pose+0x10, xyzw). It is the
    // character's own body orientation, which a seated character shares with the vehicle it is attached to.
    s->root_ok = read_pose_root_axes(obj, s->root_ax);
    float ax[3], ay[3], az[3];
    if (read_bone_world_axes(obj, headpose::head_node(), ax, ay, az)) {
        memcpy(s->head_ax + 0, ax, sizeof(ax));
        memcpy(s->head_ax + 3, ay, sizeof(ay));
        memcpy(s->head_ax + 6, az, sizeof(az));
        s->head_ok = true;
    }
    return true;
}

// Build 45: see CameraProbe.h. Same source and sanity rules as base_pos,
// plus a unit-length check on each rotation row, because a camera object
// mid-swap can hold a matrix that reads fine and means nothing. POD locals
// only, so SEH needs no unwinding.
// BUILD 98: renamed from base_frame, and rot may be null when only the position
// is wanted (base_frame_engine's telemetry read).
static bool base_frame_live(float rot[9], float pos[3]) {
    const uint64_t cam = g_player_cam.load(std::memory_order_relaxed);
    if (!cam) return false;
    __try {
        const float* m = (const float*)(cam + kOffPose);
        const float x = m[12], y = m[13], z = m[14];
        const float lim = 1.0e6f;
        if (!(x == x && y == y && z == z &&
              x > -lim && x < lim && y > -lim && y < lim &&
              z > -lim && z < lim))
            return false;
        for (int r = 0; r < 3; ++r) {
            const float a = m[r * 4 + 0];
            const float b = m[r * 4 + 1];
            const float c = m[r * 4 + 2];
            const float len2 = a * a + b * b + c * c;
            if (!(len2 > 0.81f && len2 < 1.21f)) return false;
            if (!rot) continue;
            rot[r * 3 + 0] = a;
            rot[r * 3 + 1] = b;
            rot[r * 3 + 2] = c;
        }
        pos[0] = x; pos[1] = y; pos[2] = z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Build 64: see CameraProbe.h. Selection happens at 1 Hz on the drain
// thread; this per-frame read only refreshes the picked instance's world
// position so the marker tracks the gun smoothly. SEH on the read: a
// recycled instance turns the marker off until the next drain tick
// re-validates the pick.
void set_wskel(bool on) {
    g_wskel_on.store(on, std::memory_order_relaxed);
    if (!on) {
        g_wskel_cand.store(0, std::memory_order_relaxed);
        g_wskel_tgt.store(0, std::memory_order_relaxed);
    }
}

// Build 65: a rising edge resets the write cap so the cfg toggle is the
// re-arm; off clears the target immediately (the drain would too, one
// second later).
// Build 66: the value now selects the TARGET as well as arming.
//   0 = off, 1 = instance origin (build 65, kept to reproduce its negative),
//   2 = pose root translation.
// Build 67: the gun-root write. mode 0 off, 1 Fake_gunroot (visual),
// 2 FakeGunRoot_Gameplay (the engine's own authoritative twin). Disarming
// clears the published skeleton immediately so the stub goes inert on the
// very next call rather than one tick later.
// Build 68: the weapon placement override. 0 off, 1 lift by dz (mechanism
// test), 2 barrel follows the controller ray (the feature). Disarming clears
// the target immediately so the stub goes inert on the very next call.
void set_wnode(int mode, float dz) {
    g_wnode_dz.store(dz, std::memory_order_relaxed);
    g_wnode_mode.store(mode, std::memory_order_relaxed);
    if (mode <= 0) {
        g_wnode_target.store(0, std::memory_order_relaxed);
        g_wanchor.store(0, std::memory_order_relaxed);
        g_wanchor_ok.store(0, std::memory_order_relaxed);
    }
    // Mode 3 cannot find its anchor without the census, so it arms it rather
    // than failing silently if the tester did not know to set both keys.
    if (mode == 3 && !g_cen_on.load(std::memory_order_relaxed))
        set_wnode_census(1);
}

// BUILD 75: cfg wnode_axis. -1 keeps the automatic calibration; 0..5 forces
// which signed basis row is treated as the barrel (+row0, +row1, +row2, -row0,
// -row1, -row2 in that order).
//
// The automatic version scored each candidate against the camera forward and
// latched +row0 with a 3.2x margin, and the tester's "I have to point hard
// right to get it in front of me" says that is the gun's RIGHT axis, not its
// barrel. A confident wrong answer, because on a canted rifle the right axis
// can correlate with gaze better than the barrel does. Six candidates is small
// enough that settling it in the headset costs two minutes and is certain,
// which beats a fourth inference from a proxy measurement.
void set_wnode_axis(int idx) {
    if (idx < 0) {
        g_axis_idx.store(-1, std::memory_order_relaxed);
        g_axis_samples.store(0, std::memory_order_relaxed);
        for (int i = 0; i < 6; ++i) g_axis_score[i] = 0.0f;
        LOG_INFO("waxis: automatic calibration re-armed (wnode_axis=-1).");
        return;
    }
    if (idx > 5) idx = 5;
    static const char* kName[6] =
        {"+row0", "+row1", "+row2", "-row0", "-row1", "-row2"};
    if (g_axis_idx.exchange(idx, std::memory_order_relaxed) != idx)
        LOG_INFO("waxis: FORCED to %s (wnode_axis=%d). The gun's forward is "
                 "taken as this axis of the weapon part basis.",
                 kName[idx], idx);
}

// BUILD 76: cycle the barrel axis from a key, because settling this in the
// headset means editing a text file, and editing a text file means taking the
// headset off, which means the tester is judging orientation from memory rather
// than from what is in front of him. Each press steps to the next candidate and
// the gun re-orients immediately, so the gun itself is the readout: there is
// nothing to read and nothing to remember. This is the same problem the tester
// panel exists to solve generally; the key is the version of it that works
// today.
void cycle_wnode_axis() {
    static const char* kName[6] =
        {"+row0", "+row1", "+row2", "-row0", "-row1", "-row2"};
    int cur = g_axis_idx.load(std::memory_order_relaxed);
    cur = (cur < 0) ? 0 : (cur + 1) % 6;
    g_axis_idx.store(cur, std::memory_order_relaxed);
    LOG_INFO("waxis: NUMPAD 4 -> %s (index %d of 0..5). The gun's forward is "
             "now this axis of the weapon part basis.", kName[cur], cur);
}

void set_wnode_radius(float r) {
    if (r < 0.05f) r = 0.05f;
    if (r > 1.00f) r = 1.00f;
    g_wnode_radius.store(r, std::memory_order_relaxed);
}

// Build 70. Arming clears the table, so a census always describes the run you
// are actually watching and never carries samples from a previous weapon.
void set_wnode_census(int on) {
    const int want = on ? 1 : 0;
    if (g_cen_on.exchange(want, std::memory_order_relaxed) == want) return;
    if (want) {
        for (uint32_t i = 0; i < kCenSlots; ++i) {
            g_cen[i].node.store(0, std::memory_order_relaxed);
            g_cen[i].hits.store(0, std::memory_order_relaxed);
            g_cen[i].dmin = 1e9f;
        }
        g_cen_seen.store(0, std::memory_order_relaxed);
        g_cen_rejected.store(0, std::memory_order_relaxed);
        g_cen_ref_ok.store(0, std::memory_order_relaxed);   // wait for a real groot
        LOG_INFO("wcen: SetWorldTransform census ARMED. It only observes, so it "
                 "is safe with wnode=0, which is how it should be run: no "
                 "substitution, no visual mess, just the ranking.");
    } else {
        LOG_INFO("wcen: census off.");
    }
}

void set_wgun(int mode, float dz) {
    g_wgun_dz_cfg.store(dz, std::memory_order_relaxed);
    g_wgun_hash.store(mode >= 2 ? kGunRootGameplay : kFakeGunRootHash,
                      std::memory_order_relaxed);
    grwxr_wgun_rot = (mode >= 3) ? 1u : 0u;
    const bool on = mode > 0;
    g_wgun_on.store(on, std::memory_order_relaxed);
    // Always disarm on a mode change. The bone INDEX the stub uses is only
    // resolved inside the census tick, so between here and the next tick an
    // armed stub would keep acting on the previous mode's bone. Mode 1 is node
    // 8 and modes 2 and 3 are node 10, so that is a real difference, and a
    // stale node would show the tester the wrong mode's result under the new
    // mode's name. Costs up to 5 s of inactivity; buys an unambiguous reading.
    grwxr_wgun_skel = 0;
    g_wgun_gen.fetch_add(1, std::memory_order_relaxed);   // restart the filter
}

void set_wgun_pos(int on, float scale, float clamp_m, float smooth) {
    if (scale < 0.10f) scale = 0.10f;
    if (scale > 3.00f) scale = 3.00f;
    if (clamp_m < 0.05f) clamp_m = 0.05f;
    if (clamp_m > 3.00f) clamp_m = 3.00f;
    if (smooth < 0.01f) smooth = 0.01f;
    if (smooth > 1.00f) smooth = 1.00f;
    g_wgun_pos_scale.store(scale, std::memory_order_relaxed);
    g_wgun_pos_clamp.store(clamp_m, std::memory_order_relaxed);
    g_wgun_pos_smooth.store(smooth, std::memory_order_relaxed);
    const int want = on ? 1 : 0;
    if (g_wgun_pos.exchange(want, std::memory_order_relaxed) == want) return;
    g_wgun_gen.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("wgun: position %s (scale %.2f, clamp %.2f m, smooth %.2f). The "
             "gun-root is placed ON the controller, never further than the "
             "clamp from where the engine put it.",
             want ? "ON" : "off", scale, clamp_m, smooth);
}

// Build 99: see CameraProbe.h. Clamped, because these reach the player through
// a long lever arm and skeleton rule 5 says every controller-driven transform
// gets one. The bounds are generous enough to cover any real weapon (a rifle is
// under a metre) and tight enough that a stuck key cannot fling the gun.
// BUILD 101: the rails were set by guesswork and they were too tight. The
// tester ran wgun_grip_up into +0.40 and pressed Page Up THIRTY-ONE more times
// against a wall the log recorded as a flat line. `[USER DIRECTIVE]` do not cap
// a tuning range this conservatively. 2.00 m is past anything a rifle-in-hand
// offset can need and still finite, which is all a rail is for.
void set_wgun_grip(float fwd, float lat, float up, float two) {
    if (fwd < -2.00f) fwd = -2.00f;
    if (fwd >  2.00f) fwd =  2.00f;
    if (lat < -2.00f) lat = -2.00f;
    if (lat >  2.00f) lat =  2.00f;
    if (up  < -2.00f) up  = -2.00f;
    if (up  >  2.00f) up  =  2.00f;
    if (two <  0.0f)  two =  0.0f;
    if (two >  1.0f)  two =  1.0f;
    g_wgun_grip_fwd.store(fwd, std::memory_order_relaxed);
    g_wgun_grip_lat.store(lat, std::memory_order_relaxed);
    g_wgun_grip_up.store(up,  std::memory_order_relaxed);
    g_wgun_grip_two.store(two, std::memory_order_relaxed);
}

void get_wgun_grip(float* fwd, float* lat, float* up, float* two) {
    if (fwd) *fwd = g_wgun_grip_fwd.load(std::memory_order_relaxed);
    if (lat) *lat = g_wgun_grip_lat.load(std::memory_order_relaxed);
    if (up)  *up  = g_wgun_grip_up.load(std::memory_order_relaxed);
    if (two) *two = g_wgun_grip_two.load(std::memory_order_relaxed);
}

// BUILD 219: the grip bone switch. See CameraProbe.h.
void set_wgun_grip_bone(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_grip_bone.exchange(want, std::memory_order_relaxed) == want) return;
    if (!want) g_wgun_grip_bone_ok.store(0, std::memory_order_relaxed);
    LOG_INFO("wgun: grip bone %s (build 219: the weapon's own wb-HandRight, read "
             "from its rig at pick, moves the root so the grip lands on the "
             "controller; the cfg wgun_grip_* trims add to it)",
             want ? "ON" : "off");
}
int get_wgun_grip_bone() { return g_wgun_grip_bone.load(std::memory_order_relaxed); }

// BUILD 221: the PiP scope. See CameraProbe.h.
void set_pip(int on) {
    const int want = on ? 1 : 0;
    if (g_pip_on.exchange(want, std::memory_order_relaxed) == want) return;
    if (!want) g_pip_lens_ok.store(0, std::memory_order_relaxed);
    if (!want) g_pip_user_on.store(0, std::memory_order_relaxed);   // BUILD 237
    LOG_INFO("pip: %s (build 237: the magnified disc on the eyepiece, latched up "
             "and down by the left thumbstick click)", want ? "ON" : "off");
}
void set_pip_fallback(float lat, float fwd, float up) {
    if (!isfinite(lat) || !isfinite(fwd) || !isfinite(up)) return;
    if (lat < -1.0f) lat = -1.0f; if (lat > 1.0f) lat = 1.0f;
    if (fwd < -1.0f) fwd = -1.0f; if (fwd > 2.0f) fwd = 2.0f;
    if (up  < -1.0f) up  = -1.0f; if (up  > 1.0f) up  = 1.0f;
    g_pip_fb_lat.store(lat, std::memory_order_relaxed);
    g_pip_fb_fwd.store(fwd, std::memory_order_relaxed);
    g_pip_fb_up.store(up,  std::memory_order_relaxed);
}
void set_pip_block_ads(int on) {
    const int want = on ? 1 : 0;
    if (g_pip_block_ads.exchange(want, std::memory_order_relaxed) == want) return;
    LOG_INFO("pip: the left trigger is %s the game (build 221: pip_block_ads)",
             want ? "WITHHELD from" : "passed to");
}

// BUILD 237: THE SCOPE IS A LATCH ON L3, NOT A HELD TRIGGER.
//
// Until now the disc opened while the LEFT TRIGGER was held, and pip_block_ads
// withheld that trigger from the game for the whole time a weapon was drawn.
// The consequence was not obvious and it cost the head: the head hide is a
// detour on the engine's visibility setter, and the engine only CALLS that
// setter from its proximity test, which runs from the ADS camera family. With
// the trigger withheld the game never entered ADS, so the setter was never
// called, so the head never hid. This file already says so at the top of the
// build 224 block: "we were waiting to be told".
//
// So the trigger goes back to the game and the scope moves to a latch on the
// left thumbstick click. Press once, the scope is up and stays up with nothing
// held; press again, it is down. While it is DOWN the left trigger is the
// game's own ADS again, which is what hides the head.
//
// The press is consumed rather than passed through, because L3 is sprint and a
// scope that made you sprint every time you raised it would be worse than the
// trigger. pip_toggle = 0 restores the pre-237 hold-the-trigger behaviour
// exactly, L3 included.
void pip_toggle_press() {
    if (!g_pip_on.load(std::memory_order_relaxed)) return;
    const int now = g_pip_user_on.load(std::memory_order_relaxed) ? 0 : 1;
    g_pip_user_on.store(now, std::memory_order_relaxed);
    LOG_INFO("pip: scope %s (build 237: L3 latch; the left trigger is the "
             "game's own ADS whenever the scope is down)", now ? "UP" : "down");
}
int pip_user_on() { return g_pip_user_on.load(std::memory_order_relaxed); }
void set_pip_toggle(int on) {
    const int want = on ? 1 : 0;
    if (g_pip_toggle.exchange(want, std::memory_order_relaxed) == want) return;
    if (!want) g_pip_user_on.store(0, std::memory_order_relaxed);
    LOG_INFO("pip: the scope is opened by %s (cfg pip_toggle)",
             want ? "a CLICK of the left thumbstick, and stays up"
                  : "HOLDING the left trigger, as before build 237");
}
int pip_toggle_enabled() { return g_pip_toggle.load(std::memory_order_relaxed); }

// BUILD 238: NATIVE VR ADS.
//
// Build 25 sends the aim back to the HEAD the moment the left trigger is held,
// and its reason was sound at the time: "the game draws its sight picture at
// view center (head), so controller-driven aim during ADS splits picture from
// impacts". That split only exists while OUR camera write is running, because
// then the scene follows the head-composed camera while the game draws its
// overlay at its own aim. Inside the flat-scope path the write is skipped, there
// is exactly one camera, and picture and impacts are the same thing again - so
// the controller can hold the aim and MOVING THE RIFLE PANS THE SCOPE, which is
// the whole point of this mode. Off by default: this changes aiming.
void set_scope_native(int on) {
    const int want = on ? 1 : 0;
    if (g_scope_native.exchange(want, std::memory_order_relaxed) == want) return;
    LOG_INFO("scope: native VR ADS is %s (cfg scope_native). While the game's own "
             "magnified sight picture is up, the aim comes from %s.",
             want ? "ON" : "off", want ? "the RIFLE" : "the head (build 25)");
}

void set_scope_hold(int frames) {
    uint32_t want = (frames < 1) ? 1u : (frames > 60 ? 60u : (uint32_t)frames);
    if (g_scope_hold.exchange(want, std::memory_order_relaxed) == want) return;
    LOG_INFO("scope: the flat-scope gate needs %u consecutive frames past "
             "mono_scope_fov before it engages or releases (cfg "
             "scope_hold_frames). 1 restores the pre-238 instantaneous test.",
             want);
}

// 1 only while BOTH the mode is on and the flat-scope path is actually engaged,
// so nothing changes for anybody who leaves scope_native at 0.
int scope_native_aim() {
    return (g_scope_native.load(std::memory_order_relaxed) &&
            g_scope_engaged.load(std::memory_order_relaxed)) ? 1 : 0;
}

int scope_engaged() { return g_scope_engaged.load(std::memory_order_relaxed); }
// Reviews 1 and 3 of build 221: the left trigger is brake, reverse and descend
// in vehicles, so it is withheld only while a weapon is DRAWN (the pick on the
// gun root); the body anchor stays armed seated and holstered and is not the
// signal.
// BUILD 225: THE ADS PULSE, the consume side. Game input thread, rule 8:
// relaxed loads, one compare chain, a write through the caller's pointer.
// Called from XInputMerge AFTER the pip_block_ads zeroing at both sites, so
// for the press frames this is the last word on the byte and for every other
// frame the block is. Fires only while the block is on: it can only ever lend
// back a trigger that is currently being withheld, never add one.
int pip_block_ads();
bool ads_pulse_value(unsigned char* p) {
    const uint32_t n = g_pulse_n.load(std::memory_order_acquire);
    if (!n) return false;
    const uint64_t t0 = g_pulse_t0.load(std::memory_order_relaxed);
    const uint64_t f  = d3d11::frame_count();
    if (f < t0) return false;
    const uint64_t k = f - t0;
    if (k >= (uint64_t)3 * n + 120) {           // a pause or an alt-tab: drop it
        g_pulse_n.store(0, std::memory_order_relaxed);
        g_pulse_expired.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (k >= (uint64_t)3 * n) {                 // press, gap, press: done
        g_pulse_n.store(0, std::memory_order_relaxed);
        g_pulse_done.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!pip_block_ads() ||                     // the weapon went away: drop it
        !g_fp_anchored.load(std::memory_order_relaxed)) {   // or the anchor (a vehicle)
        g_pulse_n.store(0, std::memory_order_relaxed);
        g_pulse_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (k < n || k >= (uint64_t)2 * n) {
        *p = 200;
        g_pulse_frames.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

int pip_block_ads() {
    // Review 2: the block follows the pip switch, so pip = 0 alone is the
    // whole rollback and the trigger goes back to the game with it.
    if (!g_pip_on.load(std::memory_order_relaxed)) return 0;
    if (!g_pip_block_ads.load(std::memory_order_relaxed)) return 0;
    // BUILD 237: and it follows the LATCH. The trigger is withheld only while
    // the scope is actually up; with the scope down it is the game's own ADS,
    // which is the call path that hides the head. Before this, the trigger was
    // withheld for the whole time a weapon was drawn and the head never hid.
    if (g_pip_toggle.load(std::memory_order_relaxed) &&
        !g_pip_user_on.load(std::memory_order_relaxed)) return 0;
    return g_pip_drawn.load(std::memory_order_relaxed) ? 1 : 0;
}
// BUILD 222: the engine's own per-eye camera shift along the camera's right
// axis, exactly as the camera write applies it (`s = g_eye_toggle ? -half :
// +half`), so a consumer can put a near object where each eye's frame
// rendered it. The latched camera position is the CENTRE viewpoint.
float eye_shift(int eye) {
    const float half = 0.5f * headpose::read_ipd(0.063f) * headpose::ipd_scale()
                       * (float)g_ipd_sign.load(std::memory_order_relaxed);
    return eye ? -half : +half;
}
bool pip_lens(float out[3]) {
    if (!g_pip_on.load(std::memory_order_relaxed)) return false;
    if (!g_pip_lens_ok.load(std::memory_order_acquire)) return false;
    // Review 1 of build 221: the anchor publishes on every rig write while
    // armed; a position older than four frames means a disarm or a bail-out
    // (vehicle, respawn, holster) and must not draw a disc at a fixed point.
    const uint64_t age = d3d11::frame_count() - g_pip_lens_frame.load(std::memory_order_relaxed);
    if (age > 4) return false;
    out[0] = g_pip_lens_x.load(std::memory_order_relaxed);
    out[1] = g_pip_lens_y.load(std::memory_order_relaxed);
    out[2] = g_pip_lens_z.load(std::memory_order_relaxed);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

void set_wgun_filter(float smooth, float maxstep_deg) {
    if (smooth < 0.01f) smooth = 0.01f;
    if (smooth > 1.00f) smooth = 1.00f;
    if (maxstep_deg < 0.10f) maxstep_deg = 0.10f;
    if (maxstep_deg > 90.0f) maxstep_deg = 90.0f;
    g_wgun_smooth.store(smooth, std::memory_order_relaxed);
    g_wgun_maxstep.store(maxstep_deg, std::memory_order_relaxed);
}

// 2026-08-13: two-handed aim and barrel roll. Bumping the generation counter on
// a change reseeds both the direction and the roll filters from the live value,
// so flipping either at runtime cannot make the weapon slew out of stale state.
// BUILD 120: the two-hand agreement gate, live from the tuner.
void  set_wgun_two_agree(float v) {
    if (v < -1.0f) v = -1.0f;
    if (v >  1.0f) v =  1.0f;
    g_wg_two_agree.store(v, std::memory_order_relaxed);
}
float get_wgun_two_agree() { return g_wg_two_agree.load(std::memory_order_relaxed); }

// 2026-08-31b: the attach band's release edge, metres of hand separation.
// Floor 0.25 keeps it above the authority band's own top (kTwoHandHi 0.22
// plus the 0.15 fade must start after full authority exists, or the band
// could never reach 1 and two-handed would quietly cap below full); ceiling
// 3.00 is beyond any human wingspan, which effectively disables the release
// and is the pre-band behaviour for the A/B.
void set_wgun_two_attach(float m) {
    if (m < 0.25f) m = 0.25f;
    if (m > 3.00f) m = 3.00f;
    g_wg_two_attach.store(m, std::memory_order_relaxed);
}
float get_wgun_two_attach() { return g_wg_two_attach.load(std::memory_order_relaxed); }

// BUILD 123: measure which hand is in front instead of assuming the left one.
// Bumps the generation counter for the same reason set_wgun_twohand does: the
// change can reverse the target direction, and the filter must reseed rather
// than slew the weapon through 180 degrees at the rate cap.
void set_wgun_two_auto(int on) {
    const int want = on ? 1 : 0;
    if (g_wg_two_auto.exchange(want, std::memory_order_relaxed) != want)
        g_wgun_gen.fetch_add(1, std::memory_order_relaxed);
}
int  get_wgun_two_auto() { return g_wg_two_auto.load(std::memory_order_relaxed); }

// BUILD 125: the eye-sign A/B. See the long comment at the offset site.
// Anything other than a negative value means +1, so a garbled cfg cannot land
// on zero and collapse the stereo to mono without saying so.
void set_wgun_grip_bypass(int on) {
    g_wgun_grip_bypass.store(on ? 1 : 0, std::memory_order_relaxed);
}
int  get_wgun_grip_bypass() { return g_wgun_grip_bypass.load(std::memory_order_relaxed); }

void set_ipd_sign(int s) {
    g_ipd_sign.store(s < 0 ? -1 : 1, std::memory_order_relaxed);
}
int  get_ipd_sign() { return g_ipd_sign.load(std::memory_order_relaxed); }

// 2026-09-01: the horizon lock. Only bites while cam_pose_rot is on, because
// with the rotation write off the mod never touches orientation at all.
void set_cam_level(int on) {
    g_cam_level.store(on ? 1 : 0, std::memory_order_relaxed);
    LOG_INFO("cam: horizon lock %s. %s", on ? "ON" : "off",
             on ? "The engine's own camera roll is removed from the base "
                  "before your head is composed onto it, so the horizon "
                  "stays level and your head roll is the only source of "
                  "roll. Needs cam_pose_rot = 1 to do anything."
                : "The engine's camera roll is inherited as-is.");
}
int  get_cam_level() { return g_cam_level.load(std::memory_order_relaxed); }

// 2026-09-01: the arm scale, cfg hand_scale, also set live by a good capture.
void set_hand_scale(float s) {
    if (!(s > 0.5f) || !(s < 2.0f)) {
        LOG_INFO("cam: hand_scale %.3f refused, outside 0.5 to 2.0. Left at "
                 "%.3f.", s, g_hand_scale.load(std::memory_order_relaxed));
        return;
    }
    g_hand_scale.store(s, std::memory_order_relaxed);
    LOG_INFO("cam: hand_scale %.3f. %s", s,
             s == 1.0f ? "Identity: the free hand reaches exactly where the "
                         "controller says, as before."
                       : "The free-hand IK target is scaled about the "
                         "shoulder, so a full real reach maps onto a full "
                         "avatar reach. Watch hfar against free in the wgun "
                         "line: it should fall a long way.");
}
float get_hand_scale() { return g_hand_scale.load(std::memory_order_relaxed); }

// BUILD 283: cfg wgun_pull, wgun_pull_k, wgun_pull_max. Hot reload.
void set_wgun_pull(int on, float k, float max_mm) {
    const int want = on ? 1 : 0;
    if (!isfinite(k) || k < 0.80f || k > 1.0f) k = 0.97f;
    if (!isfinite(max_mm) || max_mm < 0.0f || max_mm > 500.0f) max_mm = 250.0f;
    const int prev = g_wgun_pull.exchange(want, std::memory_order_relaxed);
    g_wgun_pull_k.store(k, std::memory_order_relaxed);
    g_wgun_pull_max.store(max_mm, std::memory_order_relaxed);
    static bool s_said = false;
    if (!s_said || prev != want) {
        s_said = true;
        LOG_INFO("pull: shoulder pull %s (cfg wgun_pull; build 283: the limb is carried toward the hand by what the reach "
                 "needs, so the arm stays under %.2f of full extension; cap %.0f mm; the left hand is no longer scaled "
                 "about the shoulder while it is on).", want ? "ON" : "off", k, max_mm);
    }
}

// Consumed once by VRMirror's key poll, which owns the first-person path.
bool take_fp_request() {
    return g_fp_request.exchange(0, std::memory_order_relaxed) != 0;
}

// BUILD 124: restart the weapon direction and roll filters from the live value.
// Anything that can REVERSE the target direction must call this, or the one-pole
// will slew the weapon through 180 degrees at the 5 deg per call rate cap
// instead of snapping to the new target. Swapping handedness does exactly that,
// because it changes which controller the weapon pivots on.
void reseed_wgun() { g_wgun_gen.fetch_add(1, std::memory_order_relaxed); }

void set_wgun_twohand(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_twohand.exchange(want, std::memory_order_relaxed) != want)
        g_wgun_gen.fetch_add(1, std::memory_order_relaxed);
}

// 2026-08-31: the hands and the anchor. Neither can reverse the target
// direction, so neither reseeds the filter. Turning the hands OFF simply stops
// the writes; the engine's own animation re-solves every record next frame, so
// there is no state to restore. Logged on change, from the cfg/tuner thread.
void set_wgun_hands(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_hands.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: hands %s. The character's hand and finger bones %s the "
                 "motion-controlled weapon (watch hands= in the wgun line).",
                 want ? "ON" : "off", want ? "now ride" : "no longer follow");
}

// BUILD 145: the grip gate.
void set_wgun_two_grip(int on) {
    const int want = on ? 1 : 0;
    if (g_wg_two_grip.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: grip-gated hold %s. %s",
                 want ? "ON" : "off",
                 want ? "The two-handed direction engages only while the off "
                        "hand squeezes its grip (watch gveto= for frames the "
                        "gate removed)."
                      : "The hold engages on hand geometry alone, as before.");
}

// Per frame from the VR layer, no logging.
void set_grip_off(float v) {
    if (!(v >= 0.0f)) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    uint32_t b;
    memcpy(&b, &v, sizeof(b));
    g_grip_off_bits.store(b, std::memory_order_relaxed);
}

// BUILD 144: the ray probe.
void set_ray_probe(int on) {
    g_ray_probe.store(on ? 1 : 0, std::memory_order_relaxed);
}

// BUILD 143: the visible-barrel pitch trim.
void set_wgun_pitch(float deg) {
    if (deg < -45.0f) deg = -45.0f;
    if (deg >  45.0f) deg =  45.0f;
    g_wgun_pitch.store(deg, std::memory_order_relaxed);
}

// BUILD 142: the arm solve and the calibration capture.
void set_wgun_arms(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_arms.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: arm solve %s. %s",
                 want ? "ON" : "off",
                 want ? "The elbows follow the hands (watch ik= and ikcl= in "
                        "the wgun line; ikcl counts beyond-reach frames)."
                      : "Arms return to the animation next solve.");
}

void set_hand_calib(int on) {
    if (on) {
        g_calib_due_ms.store((uint64_t)GetTickCount64() + kCalibDelayMs,
                             std::memory_order_relaxed);
        LOG_INFO("calib: ARMED, capture in %u ms (plus up to one 1 Hz tick). "
                 "Let go of the panel and stand with both controllers "
                 "straight out to your sides at shoulder height. Voice says "
                 "when it fires.", kCalibDelayMs);
        voice::say("Arms out to your sides. Hold still.");
    }
}

// BUILD 141: hand presence.
void set_wgun_hand_free(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_free.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: free hand %s. Off the weapon, the character's left "
                 "hand %s (watch free= and hfar= in the wgun line).",
                 want ? "ON" : "off",
                 want ? "follows your left controller"
                      : "stays with the animation, build-140 style");
}

// BUILD 169: the upper arm's share of the roll.
void set_wgun_twist_arm(float v) {
    if (!isfinite(v)) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    const float was = g_wgun_twist_arm.exchange(v, std::memory_order_relaxed);
    if (was != v)
        LOG_INFO("wgun: upper-arm roll share %.2f (the Arm bone rolls this "
                 "fraction of the hand's roll about its own axis, the "
                 "shoulder's part of a pronation)", v);
}

// BUILD 167: the forearm bone's share of the roll.
void set_wgun_twist_share(float v) {
    if (!isfinite(v)) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    const float was = g_wgun_twist_share.exchange(v, std::memory_order_relaxed);
    if (was != v)
        LOG_INFO("wgun: forearm roll share %.2f (the ForeArm bone rolls this "
                 "fraction of the hand's roll; the helpers ramp from it to "
                 "the whole roll at the wrist)", v);
}

// BUILD 180: the clavicle turn cap.
void set_wgun_clav_max(float deg) {
    if (!isfinite(deg)) return;
    if (deg < 0.0f) deg = 0.0f;
    if (deg > 90.0f) deg = 90.0f;
    const float was = g_wgun_clav_max.exchange(deg, std::memory_order_relaxed);
    if (was != deg)
        LOG_INFO("wgun: collarbone turns up to %.0f deg to follow the shoulder "
                 "(0 = the collarbone stays with the torso)", deg);
}

// BUILD 189: the gun-up unflip. This one is NOT log only, it changes the roll.
void set_wgun_roll_unflip(int on) {
    const int want = on ? 1 : 0;
    // Print the state UNCONDITIONALLY the first time. The cfg default matches
    // the atomic's initial value, so a change-only log would never state that
    // the unflip is enabled, and "flips=0 direct" would be ambiguous between
    // "189, no flip happened" and "the wrong DLL is loaded". After the tester
    // sha confusion on 2026-09-04, one guaranteed line is cheap insurance.
    static bool s_said = false;
    if (!s_said) {
        s_said = true;
        LOG_INFO("wgun: gun-up unflip is %s at load (cfg wgun_roll_unflip). "
                 "Build 189.", want ? "ON" : "off");
    }
    if (g_wgun_roll_unflip.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: gun-up unflip %s. %s", want ? "ON" : "off",
                 want ? "The half-turn the fall animation leaves in the gun's "
                        "up reference is cancelled, using the controller's own "
                        "roll as the witness."
                      : "Build 188 behaviour: the roll dies after a fall "
                        "animation and stays dead until the rig re-seeds.");
}

// BUILD 190: own the bone. NOT log only: it changes how the gun-root rotation
// is built. Printed unconditionally once at load for the same reason 189's is.
void set_wgun_own(int on) {
    const int want = on ? 1 : 0;
    static bool s_said = false;
    if (!s_said) {
        s_said = true;
        LOG_INFO("wgun: own-the-bone is %s at load (cfg wgun_own). Build 190.",
                 want ? "ON" : "off");
    }
    if (g_wgun_own.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: own-the-bone %s. %s", want ? "ON" : "off",
                 want ? "The gun-root orientation is built from the aim and the "
                        "controller's up; nothing the animation leaves in the "
                        "bone is read."
                      : "Build 189 behaviour, with the unflip as configured: the "
                        "orientation is swung from what the animation left and "
                        "the roll is measured against it.");
}

// BUILD 191: the fov root. Writes Camera+0x2B0 when on. Printed once at load
// unconditionally, like 189's and 190's, so a log always states the mode.
void set_fov_root(int on) {
    int want = on ? 1 : 0;
    if (want && (g_fov_src.load(std::memory_order_relaxed) ||
                 g_fov_scene.load(std::memory_order_relaxed))) {   // BUILD 198: or the scene override
        // BUILD 197: the two never run together; the source fov wins.
        // BUILD 198: or the scene override, which outranks both.
        static bool s_refused = false;
        if (!s_refused) {
            s_refused = true;
            LOG_INFO("cam: fov root stays off: %s owns the field's value (build %s).",
                     g_fov_scene.load(std::memory_order_relaxed) ? "fov_scene" : "fov_src",
                     g_fov_scene.load(std::memory_order_relaxed) ? "198" : "197");
        }
        want = 0;
    }
    static bool s_said = false;
    if (!s_said) {
        s_said = true;
        LOG_INFO("cam: fov root is %s at load (cfg fov_root). Build 191.",
                 want ? "ON" : "off");
    }
    if (g_fov_root.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("cam: fov root %s. %s", want ? "ON" : "off",
                 want ? "fs_fov is written into the player camera's vertical fov "
                        "field before the projection selector reads it, so the "
                        "sky and the world share one field of view; the field is "
                        "restored during the engine's own ADS and scopes."
                      : "The field is restored at the next selector call. The "
                        "fullscreen override alone widens the world and the sky "
                        "keeps the engine's fov: the cloud slide.");
}

// BUILD 194: the hold mode and the aim gain, both hot-reloadable.
void set_fov_root_mode(int mode) {
    const int want = mode == 1 ? 1 : 0;
    if (g_fov_root_mode.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("cam: fov root mode %d (%s).", want,
                 want ? "the field is wide only until the projection call that "
                        "consumes it, then restored"
                      : "the field is held wide all frame");
}
void set_fov_root_aimk(float k) {
    // BUILD 197: RETIRED. The pump credits the whole delta while handing the
    // engine the scaled one, so any gain but 1.0 desynchronises the view from
    // the head (2026-09-06, 0.41: the look and the roll inverted). The value
    // is ignored; say so once if a cfg still carries one.
    static bool s_said = false;
    if (!s_said && fabsf(k - 1.0f) > 1e-6f) {
        s_said = true;
        LOG_INFO("cam: fov_root_aimk = %.3f is IGNORED: the gain is retired since "
                 "build 197 (the pump's accounting never scaled with it).", k);
    }
    g_fov_root_aimk.store(0x3F800000u, std::memory_order_relaxed);
}
float fovroot_aim_gain() {
    return 1.0f;   // BUILD 197: retired, see set_fov_root_aimk
}

// BUILD 191: the 1 Hz line. Called from the VR side's once-a-second block.
void fovroot_log() {
    const bool on = g_fov_root.load(std::memory_order_relaxed) != 0;
    LOG_INFO("fovroot: %s armed=%u cam=0x%012llX f2B0=%.4f f2BC=%.4f f2C0=%.3f "
             "f798=%.4f | proj2 engine=%.4f published=%.4f ratio=%.3f splay=%.3f "
             "narrow=%u | orig=%.4f written=%.4f | ticks=%llu writes=%llu "
             "restores=%llu releases=%llu skip_band=%llu skip_narrow=%llu "
             "skip_target=%llu foreign=%llu ownerlost=%llu notplayer=%llu "
             "lastmode=%u seh=%llu mode=%u phase=%d skip_phase=%llu aimk=%.2f "
             "(f2B0 is the player camera's vertical fov field, which the sky reads "
             "and the selector does not; engine is proj[2]'s fovy before the "
             "fullscreen override and published is after it; ratio is engine over "
             "the field on the paired call, a diagnostic. Healthy with the key at "
             "1: mode 0 holds f2B0 at fs_fov with writes about one per frame and "
             "restores 0; mode 1 shows writes, releases and restores all about one "
             "per frame with phase learned (0 to 2) and skip_phase climbing at "
             "about two per frame. narrow=1 is the engine's own ADS or "
             "scope, during which the field is restored. lastmode is the last "
             "non-zero camera mode the selector saw; foreign counts a second mode-0 "
             "camera left untouched; ownerlost counts ownership aged out)",
             on ? "ON" : "off",
             g_fr_armed.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_cam.load(std::memory_order_relaxed),
             fr_f(g_fr_f2B0.load(std::memory_order_relaxed)),
             fr_f(g_fr_f2BC.load(std::memory_order_relaxed)),
             fr_f(g_fr_f2C0.load(std::memory_order_relaxed)),
             fr_f(g_fr_f798.load(std::memory_order_relaxed)),
             fr_f(g_fr_eng.load(std::memory_order_relaxed)),
             headpose::read_fov(0.0f),
             fr_f(g_fr_ratio.load(std::memory_order_relaxed)),
             fr_f(g_fr_splay.load(std::memory_order_relaxed)),
             g_fr_narrow.load(std::memory_order_relaxed),
             fr_f(g_fr_orig.load(std::memory_order_relaxed)),
             fr_f(g_fr_written.load(std::memory_order_relaxed)),
             (unsigned long long)g_fr_ticks.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_writes.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_restores.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_releases.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_skip_band.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_skip_narrow.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_skip_target.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_foreign.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_ownerlost.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_notplayer.load(std::memory_order_relaxed),
             g_fr_lastmode.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_seh.load(std::memory_order_relaxed),
             (unsigned)g_fov_root_mode.load(std::memory_order_relaxed),
             g_fr_phase.load(std::memory_order_relaxed),
             (unsigned long long)g_fr_skip_phase.load(std::memory_order_relaxed),
             fovroot_aim_gain());
}

// BUILD 197: the source fov's cfg key and its 1 Hz line.
void set_fov_src(int on) {
    int want = on ? 1 : 0;
    if (want && g_fov_scene.load(std::memory_order_relaxed)) {
        // BUILD 198: the scene override owns the field's value while it is on.
        static bool s_refused = false;
        if (!s_refused) {
            s_refused = true;
            LOG_INFO("cam: source fov stays off: fov_scene owns the field's value (build 198).");
        }
        want = 0;
    }
    static bool s_said = false;
    if (!s_said) {
        s_said = true;
        LOG_INFO("cam: source fov is %s at load (cfg fov_src). Build 197.",
                 want ? "ON" : "off");
    }
    if (g_fov_src.exchange((uint32_t)want, std::memory_order_relaxed) == (uint32_t)want) return;
    LOG_INFO("cam: source fov %s. %s", want ? "ON" : "off",
             want ? "fs_fov minus the engine's own fov is written into the engine's "
                    "camera pose offset, so the engine itself authors the wide "
                    "field of view into its camera every frame and the sky, the "
                    "world and every other reader agree. Put back to 0 during the "
                    "engine's own ADS, scopes, menus and captures, and on any "
                    "stand-down."
                  : "0 is written into the offset at the next tick and the engine "
                    "authors its own field from the next frame.");
    if (want) {
        if (!g_fs_ident.load(std::memory_order_relaxed))
            LOG_INFO("cam: source fov will NOT arm: the fov add was not found on this "
                     "image at load (fovsrc ident=0). Nothing is written.");
        // A fresh arming is the operator's retry: clear a stand-down.
        g_fs_disabled.store(0, std::memory_order_relaxed);
        g_fs_rapid.store(0, std::memory_order_relaxed);
        g_fs_rapid_tick.store(0, std::memory_order_relaxed);
        g_fs_replaced.store(0, std::memory_order_relaxed);
        g_fs_oob.store(0, std::memory_order_relaxed);
        g_fs_foreign_run.store(0, std::memory_order_relaxed);
        g_fs_since_own.store(0, std::memory_order_relaxed);
        g_fs_backoff_until.store(0, std::memory_order_relaxed);
        if (g_fov_root.exchange(0, std::memory_order_relaxed) != 0)
            LOG_INFO("cam: fov root forced off: fov_src owns the field's value "
                     "(build 197; the two never run together).");
    }
}

void fovsrc_log() {
    const bool on = g_fov_src.load(std::memory_order_relaxed) != 0;
    LOG_INFO("fovsrc: %s%s armed=%u cam=0x%012llX obj=0x%012llX off=%.4f comp=%.4f "
             "f2B0=%.4f fs=%.4f mode34=%u ident=%u | scene=0x%012llX s286=%u "
             "s290=%.4f | offw=%llu zeroes=%llu pending=%llu skip_band=%llu "
             "skip_target=%llu foreign=%llu foreign_cam=%llu flips=%llu "
             "unhonoured=%llu unapplied=%llu replaced=%u rebuilt=%llu applied_age=%llu seh=%llu "
             "(the source fov, build 197: one float in the engine's "
             "CameraPoseOffset+0x30 that the director adds to the gameplay "
             "camera's own fov before storing the sum into Camera+0x2B0 itself, "
             "so the engine authors the wide field. Healthy: f2B0 = fs at every "
             "tick, off = fs minus comp, offw a few per second at most once "
             "converged, foreign 0, replaced 0, applied_age small, and the fovroot "
             "line's writes 0; flips climb around the engine's own blends (a "
             "sprint stop) while the offset is inert there, which is not a fault. "
             "mode34 must be 1 (additive); s286 must be 0 in play. STOOD DOWN "
             "means the field would not take the offset, four flips inside three "
             "seconds or ours replaced three times, and nothing more is written "
             "this run. unapplied counts frames the engine's other field writers "
             "authored without the offset, which is left in place and inert there; "
             "applied_age is ticks since a frame last showed the offset applied)",
             on ? "ON" : "off",
             g_fs_disabled.load(std::memory_order_relaxed) ? " STOOD DOWN" : "",
             g_fs_armed.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_cam.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_obj.load(std::memory_order_relaxed),
             fr_f(g_fs_off.load(std::memory_order_relaxed)),
             fr_f(g_fs_comp.load(std::memory_order_relaxed)),
             fr_f(g_fr_f2B0.load(std::memory_order_relaxed)),
             headpose::fs_fov(),
             g_fs_mode34.load(std::memory_order_relaxed),
             g_fs_ident.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_scene.load(std::memory_order_relaxed),
             g_fs_s286.load(std::memory_order_relaxed),
             fr_f(g_fs_s290.load(std::memory_order_relaxed)),
             (unsigned long long)g_fs_offw.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_zeroes.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_pending.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_skip_band.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_skip_target.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_foreign.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_foreign_cam.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_flips.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_unhonoured.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_unapplied.load(std::memory_order_relaxed),
             g_fs_replaced.load(std::memory_order_relaxed),
             (unsigned long long)g_fs_rebuilt.load(std::memory_order_relaxed),
             (unsigned long long)(g_fr_ticks.load(std::memory_order_relaxed) -
                                  g_fs_seen_applied.load(std::memory_order_relaxed)),
             (unsigned long long)g_fs_seh.load(std::memory_order_relaxed));
}

// BUILD 198: the scene fov override's cfg keys, the aim-trigger input and
// its 1 Hz line.
void set_fov_scene(int on) {
    const int want = on ? 1 : 0;
    static bool s_said = false;
    if (!s_said) {
        s_said = true;
        LOG_INFO("cam: scene fov override is %s at load (cfg fov_scene). Build 198.",
                 want ? "ON" : "off");
    }
    if (g_fov_scene.exchange((uint32_t)want, std::memory_order_relaxed) == (uint32_t)want) return;
    LOG_INFO("cam: scene fov override %s. %s", want ? "ON" : "off",
             want ? "the view's scene object gets +0x286 = 1 and +0x290 = fs_fov, so "
                    "every engine writer of the camera's fov field stores the wide "
                    "value itself, in every director mode, before every recompute. "
                    "Released while the aim trigger is held (fov_scene_ads) and "
                    "cleared on every stand-down."
                  : "cleared at the next tick and the engine authors its own field "
                    "from the next frame.");
    if (want) {
        if (!g_sc_ident.load(std::memory_order_relaxed))
            LOG_INFO("cam: scene fov override will NOT arm: the select-and-store site "
                     "was not found on this image at load (fovscene ident=0). Nothing "
                     "is written.");
        // A fresh arming is the operator's retry: clear a stand-down.
        g_sc_disabled.store(0, std::memory_order_relaxed);
        g_sc_replaced.store(0, std::memory_order_relaxed);
        g_sc_foreign_run.store(0, std::memory_order_relaxed);
        if (g_fov_src.exchange(0, std::memory_order_relaxed) != 0)
            LOG_INFO("cam: source fov forced off: fov_scene owns the field's value "
                     "(build 198; the two never run together).");
        if (g_fov_root.exchange(0, std::memory_order_relaxed) != 0)
            LOG_INFO("cam: fov root forced off: fov_scene owns the field's value "
                     "(build 198).");
    }
}

void set_fov_scene_ads(int on) {
    const uint32_t want = on ? 1u : 0u;
    if (g_fov_scene_ads.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("cam: scene fov override %s while the aim trigger is held (cfg "
                 "fov_scene_ads). Build 198.", want ? "RELEASES" : "stays on");
}

void set_ads_input(bool held) {
    g_ads_input.store(held ? 1u : 0u, std::memory_order_relaxed);
}

void fovscene_log() {
    const bool on = g_fov_scene.load(std::memory_order_relaxed) != 0;
    char hist[160];
    snprintf(hist, sizeof(hist), "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
             (unsigned long long)g_dir_hist[0].load(std::memory_order_relaxed),
             (unsigned long long)g_dir_hist[1].load(std::memory_order_relaxed),
             (unsigned long long)g_dir_hist[2].load(std::memory_order_relaxed),
             (unsigned long long)g_dir_hist[3].load(std::memory_order_relaxed),
             (unsigned long long)g_dir_hist[4].load(std::memory_order_relaxed),
             (unsigned long long)g_dir_hist[5].load(std::memory_order_relaxed),
             (unsigned long long)g_dir_hist[6].load(std::memory_order_relaxed),
             (unsigned long long)g_dir_hist[7].load(std::memory_order_relaxed));
    LOG_INFO("fovscene: %s%s armed=%u cam=0x%012llX scene=0x%012llX s286=%u s290=%.4f "
             "s14d=%u f2B0=%.4f fs=%.4f ads=%u ident=%u | dir=0x%012llX dmode=%u "
             "dblend=%u dhist=%s dreads=%llu dbad=%llu | sets=%llu clears=%llu "
             "ads_clears=%llu ads_ticks=%llu applied=%llu unhonoured=%llu foreign=%llu "
             "replaced=%u rebuilt=%llu noscene=%llu seh=%llu applied_age=%llu "
             "(the scene fov override, build 198: the view's scene object carries a "
             "byte at +0x286 and a float at +0x290 that every writer of the camera's "
             "fov field selects before storing; ours is 1 and fs_fov. Healthy: armed 1, "
             "s286 1, s290 = fs, f2B0 = fs at every sample, applied climbing about "
             "three per Present, unhonoured 0, foreign 0, replaced 0; ads_clears are "
             "the aim trigger's releases; dmode is the director's mode at player "
             "camera ticks and dhist its histogram over modes 0 to 6 and 7 or more; "
             "DISABLED means ours was replaced three times and nothing more is "
             "written this run)",
             on ? "ON" : "off",
             g_sc_disabled.load(std::memory_order_relaxed) ? " DISABLED" : "",
             g_sc_armed.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_cam.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_scene_seen.load(std::memory_order_relaxed),
             g_sc_s286.load(std::memory_order_relaxed),
             fr_f(g_sc_s290.load(std::memory_order_relaxed)),
             g_sc_s14d.load(std::memory_order_relaxed),
             fr_f(g_fr_f2B0.load(std::memory_order_relaxed)),
             headpose::fs_fov(),
             g_ads_input.load(std::memory_order_relaxed),
             g_sc_ident.load(std::memory_order_relaxed),
             (unsigned long long)g_dir_obj.load(std::memory_order_relaxed),
             g_dir_mode.load(std::memory_order_relaxed),
             g_dir_blend.load(std::memory_order_relaxed),
             hist,
             (unsigned long long)g_dir_reads.load(std::memory_order_relaxed),
             (unsigned long long)g_dir_bad.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_sets.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_clears.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_ads_clears.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_ads_ticks.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_applied.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_unhonoured.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_foreign.load(std::memory_order_relaxed),
             g_sc_replaced.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_rebuilt.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_noscene.load(std::memory_order_relaxed),
             (unsigned long long)g_sc_seh.load(std::memory_order_relaxed),
             (unsigned long long)(g_fr_ticks.load(std::memory_order_relaxed) -
                                  g_sc_seen_applied.load(std::memory_order_relaxed)));

    // BUILD 238: NATIVE VR ADS, one line, so the latch can be read from a log
    // without the headset. fov is the engine's own camera fov this tick: on this
    // machine 0.8150 at the hip, 0.6882 in an iron-sight ADS and 0.2549 through
    // the FAL's magnified optic, against a gate at mono_scope_fov. Healthy while
    // a magnified optic is aimed: engaged=1, aim=RIFLE, and enters climbing by
    // ONE per aim rather than by tens (tens means the hold is too short and the
    // gate is being tripped by transitions, which is the 48-crossing failure).
    LOG_INFO("scope: native=%s hold=%u | fov=%.4f gate=%.4f engaged=%u aim=%s | "
             "enters=%llu exits=%llu (build 238: the magnified sight picture is "
             "the GAME'S OWN, drawn full screen by the flat-scope path of 11d, "
             "and with native=ON the rifle aims it instead of the head. hold is "
             "how many consecutive frames the fov must stay past the gate before "
             "the latch moves, which is what stops an iron-sight ADS transition "
             "sweeping through ~0.248 from tripping a gate meant for a settled "
             "optic at 0.2549)",
             g_scope_native.load(std::memory_order_relaxed) ? "ON" : "off",
             g_scope_hold.load(std::memory_order_relaxed),
             headpose::read_fov(0.7853982f), headpose::mono_scope_fov(),
             g_scope_engaged.load(std::memory_order_relaxed),
             scope_native_aim() ? "RIFLE" : "head",
             (unsigned long long)g_scope_enters.load(std::memory_order_relaxed),
             (unsigned long long)g_scope_exits.load(std::memory_order_relaxed));
}

// BUILD 184: the roll probes, log only.
void set_wroll_probe(int on) {
    const int want = on ? 1 : 0;
    if (g_wroll_probe.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: roll probes %s (gsurvive: the gun-root records at "
                 "Present; wroll: the placed weapon part against our "
                 "rotation). Log only.", want ? "ON" : "off");
}

// BUILD 183: the mount mirror.
void set_wgun_mirror(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    const int was = g_wgun_mirror.exchange(mode, std::memory_order_relaxed);
    if (was != mode)
        LOG_INFO("wgun: mount mirror %d: %s", mode,
                 mode == 0 ? "off, the gun-root write goes to node 10 only "
                             "(build 182)"
                 : mode == 1 ? "the gun-root record is copied onto "
                               "Fake_gunroot on every write"
                             : "the gun-root record is copied onto "
                               "Fake_gunroot and both hand prop bones on "
                               "every write");
}

// BUILD 179: the shoulder cap.
void set_arm_pole(float on, float minm) {   // BUILD 214
    const int want = on != 0.0f ? 1 : 0;
    g_arm_pole_min.store(isfinite(minm) && minm >= 0.02f && minm <= 0.20f ? minm : 0.06f,
                         std::memory_order_relaxed);
    if (g_arm_pole.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("arm_pole: the elbow's pole %s anatomy when the animation cannot supply one (cfg arm_pole; "
                 "build 214: with no weapon the animated arm hangs at the side, and when the real hand crosses "
                 "its line the perpendicular collapses and comes out the other side, which is the elbow "
                 "inverting. Below %.2f m of perpendicular the elbow now hangs below the shoulder-to-hand axis "
                 "instead, which cannot invert).",
                 want ? "FALLS BACK TO" : "does not use", g_arm_pole_min.load(std::memory_order_relaxed));
}

void set_torso(float on, float dead_deg, float rate_dps, float hand_w, float max_deg) {   // BUILD 294
    const int want = on != 0.0f ? 1 : 0;
    g_torso_dead.store(isfinite(dead_deg) && dead_deg >= 5.0f && dead_deg <= 120.0f ? dead_deg : 45.0f, std::memory_order_relaxed);
    g_torso_rate.store(isfinite(rate_dps) && rate_dps >= 20.0f && rate_dps <= 2000.0f ? rate_dps : 240.0f, std::memory_order_relaxed);
    g_torso_hand_w.store(isfinite(hand_w) && hand_w >= 0.0f && hand_w <= 1.0f ? hand_w : 0.35f, std::memory_order_relaxed);
    g_torso_max.store(isfinite(max_deg) && max_deg >= 10.0f && max_deg <= 120.0f ? max_deg : 70.0f, std::memory_order_relaxed);
    static bool s_first = true;
    if (g_torso_on.exchange(want, std::memory_order_relaxed) != want || s_first) {
        s_first = false;
        LOG_INFO("torso_turn: the upper body %s (cfg torso_turn, torso_dead %.0f deg, torso_rate %.0f deg/s, torso_hand_w %.2f, torso_max %.0f "
                 "deg; build 294: the spine, neck, head, shoulders, arms, hands and the gun root are turned rigidly about the head's vertical "
                 "axis so the torso faces the head, within the deadzone, and part of the way to the right hand).",
                 want ? "TURNS WITH THE HEAD" : "stays as the animation has it", g_torso_dead.load(std::memory_order_relaxed),
                 g_torso_rate.load(std::memory_order_relaxed), g_torso_hand_w.load(std::memory_order_relaxed),
                 g_torso_max.load(std::memory_order_relaxed));
    }
}

void set_lean(int hinge, float theta_deg, float slide_m) {   // BUILD 300
    g_lean_theta_max.store(isfinite(theta_deg) && theta_deg >= 10.0f && theta_deg <= 85.0f ? theta_deg : 65.0f, std::memory_order_relaxed);
    g_lean_slide.store(isfinite(slide_m) && slide_m >= 0.0f && slide_m <= 0.4f ? slide_m : 0.05f, std::memory_order_relaxed);
    const int want = hinge ? 1 : 0;
    static bool s_first = true;
    if (g_lean_hinge.exchange(want, std::memory_order_relaxed) != want || s_first) {
        s_first = false;
        LOG_INFO("lean: the torso %s (cfg lean_hinge, lean_theta %.0f deg, lean_slide %.2f m; build 300: the upper body hinges about the "
                 "Spine bone toward the head's tracked offset; the camera takes that move back out of the Head bone it reads and adds the "
                 "offset itself, so the view is the same with or without the hinge).",
                 want ? "HINGES AT THE HIPS" : "stays put (the view still leans)", g_lean_theta_max.load(std::memory_order_relaxed),
                 g_lean_slide.load(std::memory_order_relaxed));
    }
}

void set_pole_down(float left, float right) {   // BUILD 293
    const float l = isfinite(left)  && left  >= 0.0f && left  <= 1.0f ? left  : 0.5f;
    const float r = isfinite(right) && right >= 0.0f && right <= 1.0f ? right : 0.0f;
    const float ol = g_pole_down_l.exchange(l, std::memory_order_relaxed);
    const float orr = g_pole_down_r.exchange(r, std::memory_order_relaxed);
    static bool s_first = true;
    if (s_first || ol != l || orr != r) {
        s_first = false;
        LOG_INFO("arm_pole_down: the elbow leans toward down by left %.2f, right %.2f times the two-hand authority (cfg arm_pole_down_l, "
                 "arm_pole_down_r; build 293: the animated elbow side is kept, a blend and not the refused fixed anatomical pole; 0 = "
                 "build 285).", l, r);
    }
}

void set_pole_smooth(float on, float tau_ms, float rate_dps) {   // BUILD 285
    static bool s_first = true;
    const int want = on != 0.0f ? 1 : 0;
    g_pole_tau_ms.store(isfinite(tau_ms) && tau_ms >= 0.0f && tau_ms <= 500.0f ? tau_ms : 50.0f,
                        std::memory_order_relaxed);
    g_pole_rate.store(isfinite(rate_dps) && rate_dps >= 30.0f && rate_dps <= 5000.0f ? rate_dps : 540.0f,
                      std::memory_order_relaxed);
    const bool changed = g_pole_smooth.exchange(want, std::memory_order_relaxed) != want;
    if (changed || s_first) {
        s_first = false;
        LOG_INFO("arm_pole_smooth: the elbow side is %s (cfg arm_pole_smooth, arm_pole_tau_ms, arm_pole_rate; "
                 "build 285: the animated pole is low-passed with a %.0f ms time constant and may turn at most "
                 "%.0f deg a second about the shoulder-to-hand axis, both scaled down when the animated elbow lies "
                 "close to that axis, so the stock run animation cannot flip or shake it).",
                 want ? "SMOOTHED" : "the raw animated pole (build 284)",
                 g_pole_tau_ms.load(std::memory_order_relaxed), g_pole_rate.load(std::memory_order_relaxed));
    }
}

void set_arm_anchor(float on, float maxm) {   // BUILD 213
    const int want = on != 0.0f ? 1 : 0;
    g_arm_anchor_max.store(isfinite(maxm) && maxm >= 0.0f && maxm <= 0.60f ? maxm : 0.15f,
                           std::memory_order_relaxed);
    if (g_arm_anchor.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("arm_anchor: the shoulder %s the view offset (cfg arm_anchor; build 213, CX-025: "
                 "the hands are placed from the viewpoint and the shoulder was left on the body, so "
                 "the whole gap was charged to the arm and the hand was clamped off the weapon. The "
                 "shoulder joint now moves by the same vector the camera does, capped at %.2f m).",
                 want ? "RIDES" : "does not ride", g_arm_anchor_max.load(std::memory_order_relaxed));
}

void set_wgun_shoulder_max(float mm) {
    if (!isfinite(mm)) return;
    if (mm < 0.0f) mm = 0.0f;
    if (mm > 1000.0f) mm = 1000.0f;
    const float was = g_wgun_shoulder_max.exchange(mm, std::memory_order_relaxed);
    if (was != mm)
        LOG_INFO("wgun: shoulder cap %.0f mm (the wrist bends past its limit "
                 "rather than pull the shoulder farther than this)", mm);
}

// BUILD 178: the wrist limit.
void set_wgun_wrist_limit(float deg) {
    if (!isfinite(deg)) return;
    if (deg < 0.0f) deg = 0.0f;
    if (deg > 180.0f) deg = 180.0f;
    const float was = g_wgun_wrist_limit.exchange(deg, std::memory_order_relaxed);
    if (was != deg)
        LOG_INFO("wgun: wrist limit %.0f deg (0 = straight wrist, the elbow "
                 "follows the fingers; 180 = the animation's elbow, any bend)",
                 deg);
}

// BUILD 176: the wrist-first arm solve.
void set_wgun_wristfirst(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_wristfirst.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: wrist-first arm %s (%s)", want ? "ON" : "off",
                 want ? "the left forearm runs up the hand's own long axis "
                        "and the shoulder follows the elbow when the arm "
                        "is short"
                      : "the 162 two-bone solve with the animation's elbow");
}

// BUILD 163: the forearm roll split.
void set_wgun_twist(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_twist.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: forearm roll %s. The hand's roll %s.",
                 want ? "ON" : "off",
                 want ? "is spread along the forearm's twist helpers, elbow "
                        "to wrist"
                      : "lands on the wrist joint alone, as build 162");
}

// BUILD 162: the absolute wrist.
void set_wgun_wrist_abs(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_wrist_abs.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: wrist absolute %s. Off the weapon, the free hand's "
                 "palm %s.",
                 want ? "ON" : "off",
                 want ? "faces where the grip pose says it faces"
                      : "keeps the build-161 delta from its latch pose");
}

// BUILD 158: the wrist follow.
void set_wgun_wrist(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_wrist.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: wrist follow %s. Off the weapon, the free hand's "
                 "wrist %s.",
                 want ? "ON" : "off",
                 want ? "turns with your left controller, relative to where "
                        "it was when you let go"
                      : "keeps the animated pose, build-141 style");
}

// BUILD 140: the grab and the arm hide.
void set_wgun_hands_grab(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_grab.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: hands grab %s. The character's LEFT hand %s.",
                 want ? "ON" : "off",
                 want ? "rides the weapon only while your real off hand "
                        "holds it (the two-hand authority weight)"
                      : "is carried with the weapon always, build-139 style");
}

void set_wgun_hide_arms(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_hide_arms.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: bone-level arm hide %s. %s",
                 want ? "ON" : "off",
                 want ? "Both arms collapse onto the shoulders each write "
                        "(watch arms= for engagement). Diagnostic for the "
                        "stretch; hide_child pieces are the cleaner cure if "
                        "they separate hand from arm."
                      : "The engine's own solve restores the arms next "
                        "frame.");
}

void set_wgun_two_anchor(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_two_anchor.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("wgun: two-hand anchor %s (0 is the exact build-123 "
                 "behaviour: root anchored to the right controller even when "
                 "it measures as the FRONT hand).",
                 want ? "REAR HAND (corrected)" : "build-123");
}

// 2026-08-31: the hide-one-piece slots, FP port. The values are only ACTED on
// once the head object latches and the head hide is on (the 1 Hz tick), so
// setting them early is safe and takes effect when the engine first shows us
// the object.
void set_hide_child(int slot, int idx) {
    if (idx < -1) idx = -1;
    if (idx > 63) idx = 63;
    (slot ? g_hide_child2 : g_hide_child)
        .store(idx, std::memory_order_relaxed);
}

void set_wgun_roll(int on, float trim_deg) {
    if (trim_deg < -180.0f) trim_deg = -180.0f;
    if (trim_deg >  180.0f) trim_deg =  180.0f;
    g_wgun_roll_deg.store(trim_deg, std::memory_order_relaxed);
    const int want = on ? 1 : 0;
    if (g_wgun_roll.exchange(want, std::memory_order_relaxed) != want)
        g_wgun_gen.fetch_add(1, std::memory_order_relaxed);
}

// BUILD 78: cycle the gun-root bone from a key, same rationale as build 76's
// NUMPAD 4. The two candidate bones are coincident to 2 mm, so telling them
// apart means A/B-ing them, and A/B-ing them by editing a text file means
// taking the headset off between the two halves of the comparison.
//
// The immediate disarm matters. grwxr_wgun_node is only resolved inside the
// 5 Hz census tick, from the rig's own name map, so between the press and the
// next tick the stub would otherwise keep offsetting the PREVIOUS bone. That
// would show the tester the old mode's result and label it as the new one,
// which is exactly the class of measurement lie this project has already paid
// for three times. Zeroing the skeleton pointer makes the gun snap back to
// normal first, so the transition is visible and unambiguous.
void cycle_wgun() {
    static const char* kName[4] = {
        "off",
        "LIFT node 8, Fake_gunroot (the FALLBACK bone: verified to do nothing)",
        "LIFT node 10, FakeGunRoot_Gameplay (verified: raises the gun 60 cm)",
        "ROTATE node 10: the barrel is set ON the controller ray, absolutely"};
    int cur = 0;
    if (g_wgun_on.load(std::memory_order_relaxed)) {
        cur = (g_wgun_hash.load(std::memory_order_relaxed) == kGunRootGameplay)
                  ? 2 : 1;
        if (grwxr_wgun_rot) cur = 3;
    }
    const int next = (cur + 1) % 4;
    grwxr_wgun_skel = 0;              // disarm BEFORE the mode changes
    set_wgun(next, g_wgun_dz_cfg.load(std::memory_order_relaxed));
    LOG_INFO("wgun: NUMPAD 5 -> mode %d, %s. Disarmed now; re-arms on the next "
             "census tick (up to 5 s), so expect the gun to return to normal "
             "briefly before the new bone takes effect.",
             next, kName[next]);
}

// BUILD 80: THE BARREL RIDES THE CONTROLLER.
//
// Runs on the engine's own thread at PublishAttachments entry, gated on the
// player skeleton by the asm stub, so it is the last writer before the engine
// composes the weapon from this bone. No allocation, no lock, no logging:
// rule 8 holds. Counters only, drained at 1 Hz elsewhere.
//
// ABSOLUTE, not a delta from the game's aim. Mode 3 of the old wnode path
// rotated from "where the game is aiming" onto "where the controller points",
// and that is exactly the incremental shape both Halo MCC VR and the Cyberpunk
// port had to abandon after it ratcheted. Here the barrel is SET to the ray, so
// hip fire and ADS become the same operation and there is nothing to creep.
//
// The axis is `[VERIFIED, headset, build 79]`: +Y of the FakeGunRoot_Gameplay
// bone frame is the barrel, 0.999 against the game's aim direction across 14
// consecutive held-ADS samples.
//
// Skeleton rule 5: the controller direction is filtered and rate-limited
// before it reaches the game. The filter is on the TARGET DIRECTION rather
// than on the output quaternion, which avoids quaternion sign handling
// entirely and is what the Cyberpunk port settled on after its own attempts.
// ---------------------------------------------------------------------------
// 2026-08-31: THE HANDS RIDE THE GUN.
//
// (bq0, bt0) is the gun-root record as this call FOUND it, (qfin, tfin) is the
// record as this call LEAVES it. The rigid change between the two, dq about
// bt0 then onto tfin, is applied to every resolved hand and finger record, so
// whatever grip the animation authored is preserved exactly under our move.
// This composes correctly across the two entries per frame with no re-solve
// between them: entry 2 measures its delta from entry 1's write, which is
// precisely the state the hands were left consistent with. All in MODEL space,
// so the pose root never enters into it.
//
// Rule 8: engine thread, no lock, no allocation, no logging; every read and
// write SEH-guarded; a bone that fails any check is skipped, not guessed.
// Returns true if at least one record was written.
//
// BUILD 140: lw is the LEFT chain's ride weight, 0 to 1. The right chain (the
// animation's firing hand) always rides in full; the left chain (the foregrip
// hand) blends toward the full ride by lw, which the callers feed from the
// two-hand authority weight, so the visible left hand grabs the weapon
// exactly as the real one takes authority over it and lets go on the attach
// band's own release edge.
//
// BUILD 141: what the left chain blends FROM. With flp (the left controller's
// engine-world position) present and wgun_hand_free on, the free end of the
// blend is YOUR HAND: the chain is translated so the hand bone sits on the
// controller, wrist and fingers keeping the animated pose. Without it the
// free end is the animation and lw = 0 writes nothing, which is exactly the
// build-140 behaviour. rootn is the pose root rotation already normalised,
// rt0 the root translation, both needed to move a world-space reach into
// model space.
// ---------------------------------------------------------------------------
// BUILD 142: hand_out/hand_ok report each side's hand record position as
// written (model space), which is the arm solve's target. A side that was
// not written reports false and its arm stays the animation's.
// BUILD 143: hand_found reports the hand record position as FOUND, before
// any write, because the solve's length and direction measurements must come
// from the unmodified state; 142 measured them from the target and starved
// its own sanity guard with the inflated distance.
// BUILD 213: the view offset in MODEL space, capped. Read on the rig-write
// thread; the vector is published by the camera hook on the present thread
// and is stale by at most a frame, which is the same currency every other
// cross-thread read here uses. Zero and false whenever the key is off, the
// publish is older than two frames, the skeleton is not resolved, or the
// pose's root cannot be read: the caller then behaves exactly as build 212.
bool arm_anchor_model(float out[3]) {
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f;
    if (!g_arm_anchor.load(std::memory_order_relaxed)) return false;
    const uint64_t st = g_view_off_stamp.load(std::memory_order_acquire);
    if (!st) return false;
    const uint64_t now = d3d11::frame_count();
    if (now < st || now - st > 2) return false;
    float w[3] = {g_view_off_world[0], g_view_off_world[1], g_view_off_world[2]};
    if (!isfinite(w[0]) || !isfinite(w[1]) || !isfinite(w[2])) return false;
    const uint64_t skel = grwxr_wgun_skel;
    if (!skel) return false;
    uint64_t pose = 0;
    if (!read_block(skel + 0x238, &pose, sizeof(pose))) return false;
    if (pose < 0x10000 || (pose & 7)) return false;
    float m[3];
    uint32_t flags = 0;
    read_block(pose + 0x8C, &flags, sizeof(flags));
    if (flags & 0x04000000u) {          // the rig is already in world space
        m[0] = w[0]; m[1] = w[1]; m[2] = w[2];
    } else {
        float rq[4];
        if (!read_block(pose + 0x10, rq, sizeof(rq))) return false;
        const float qn = sqrtf(rq[0]*rq[0] + rq[1]*rq[1] + rq[2]*rq[2] + rq[3]*rq[3]);
        if (!(qn > 1e-6f) || !isfinite(qn)) return false;
        // world -> model is the conjugate of the normalised root rotation
        const float x = -rq[0]/qn, y = -rq[1]/qn, z = -rq[2]/qn, ww = rq[3]/qn;
        const float tx = 2.0f * (y*w[2] - z*w[1]);
        const float ty = 2.0f * (z*w[0] - x*w[2]);
        const float tz = 2.0f * (x*w[1] - y*w[0]);
        m[0] = w[0] + ww*tx + (y*tz - z*ty);
        m[1] = w[1] + ww*ty + (z*tx - x*tz);
        m[2] = w[2] + ww*tz + (x*ty - y*tx);
    }
    if (!isfinite(m[0]) || !isfinite(m[1]) || !isfinite(m[2])) return false;
    const float len = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    if (!isfinite(len)) return false;
    float cap = g_arm_anchor_max.load(std::memory_order_relaxed);
    if (!isfinite(cap) || cap < 0.0f) cap = 0.15f;
    if (len > cap && len > 1e-6f) {
        const float k = cap / len;
        m[0] *= k; m[1] *= k; m[2] *= k;
    }
    out[0] = m[0]; out[1] = m[1]; out[2] = m[2];
    const float rl = len > cap ? cap : len;
    uint32_t rb = 0;
    memcpy(&rb, &rl, sizeof(rb));   // no strict-aliasing pun (review 6)
    g_arm_anchor_bits.store(rb, std::memory_order_relaxed);
    return true;
}

// BUILD 214: world DOWN expressed in the rig's model space, by the same root
// rotation the anchor uses. World up is +z here (the sprint probe's head bone
// reads 0.7 m above the hips in z), so down is (0, 0, -1) before the rotation.
bool model_down(float out[3]) {
    out[0] = 0.0f; out[1] = 0.0f; out[2] = -1.0f;
    const uint64_t skel = grwxr_wgun_skel;
    if (!skel) return false;
    uint64_t pose = 0;
    if (!read_block(skel + 0x238, &pose, sizeof(pose))) return false;
    if (pose < 0x10000 || (pose & 7)) return false;
    uint32_t flags = 0;
    read_block(pose + 0x8C, &flags, sizeof(flags));
    if (flags & 0x04000000u) return true;          // the rig is in world space already
    float rq[4];
    if (!read_block(pose + 0x10, rq, sizeof(rq))) return false;
    const float qn = sqrtf(rq[0]*rq[0] + rq[1]*rq[1] + rq[2]*rq[2] + rq[3]*rq[3]);
    if (!(qn > 1e-6f) || !isfinite(qn)) return false;
    const float x = -rq[0]/qn, y = -rq[1]/qn, z = -rq[2]/qn, w = rq[3]/qn;
    const float v[3] = {0.0f, 0.0f, -1.0f};
    const float tx = 2.0f * (y*v[2] - z*v[1]);
    const float ty = 2.0f * (z*v[0] - x*v[2]);
    const float tz = 2.0f * (x*v[1] - y*v[0]);
    const float m[3] = {v[0] + w*tx + (y*tz - z*ty),
                        v[1] + w*ty + (z*tx - x*tz),
                        v[2] + w*tz + (x*ty - y*tx)};
    if (!isfinite(m[0]) || !isfinite(m[1]) || !isfinite(m[2])) return false;
    out[0] = m[0]; out[1] = m[1]; out[2] = m[2];
    return true;
}

// BUILD 213, second pass: ONE anchor per rig buffer. Both consumers and both
// calls on a buffer must use the same vector, or the clamp and the solve
// disagree by up to the whole anchor for a frame (review 2). Same thread as
// every other static here (the rig-write hook), same per-buffer key as the
// build 177 s_bc cache.
bool arm_anchor_frame(uint64_t buf, float out[3]) {
    static uint64_t s_buf = 0;
    static uint64_t s_frame = 0;
    static float    s_v[3] = {0.0f, 0.0f, 0.0f};
    static bool     s_ok = false;
    // BOTH REVIEWS, second round: keyed on the frame as well as the buffer.
    // The pose pool can hand back the same address on consecutive frames, and
    // on that address a cached s_ok = true would outlive both the staleness
    // gate and the first-person stand-down.
    const uint64_t fr = d3d11::frame_count();
    if (buf != s_buf || fr != s_frame) {
        s_ok = arm_anchor_model(s_v);
        s_buf = buf;
        s_frame = fr;
    }
    out[0] = s_ok ? s_v[0] : 0.0f;
    out[1] = s_ok ? s_v[1] : 0.0f;
    out[2] = s_ok ? s_v[2] : 0.0f;
    return s_ok;
}

bool hands_apply(uint64_t buf, const float bq0[4], const float bt0[3],
                 const float qfin[4], const float tfin[3], float lw,
                 const float rootn[4], const float rt0[3], const float* flp,
                 float hand_out[2][3], float hand_found[2][3],
                 bool hand_ok[2], float hand_rot[2][4],
                 float hand_fq[2][4], float hand_ax[2][3]) {
    hand_ok[0] = hand_ok[1] = false;
    for (int s = 0; s < 2; ++s) {   // BUILD 164: found hand rotation, raw
        hand_fq[s][0] = hand_fq[s][1] = hand_fq[s][2] = 0.0f;
        hand_fq[s][3] = 1.0f;
        // BUILD 165: the found across-fist axis (index knuckle to pinky
        // knuckle), model space, zero when the knuckles are unresolved.
        hand_ax[s][0] = hand_ax[s][1] = hand_ax[s][2] = 0.0f;
    }
    // BUILD 163: the model-space rotation each hand bone receives this call,
    // identity until a write lands, for the arm solve's roll split.
    for (int s = 0; s < 2; ++s) {
        hand_rot[s][0] = hand_rot[s][1] = hand_rot[s][2] = 0.0f;
        hand_rot[s][3] = 1.0f;
    }
    auto qmul = [](const float a[4], const float b[4], float o[4]) {
        o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
        o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
        o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
        o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    };
    auto qrot = [](const float q[4], const float v[3], float o[3]) {
        const float cx = q[1]*v[2] - q[2]*v[1];
        const float cy = q[2]*v[0] - q[0]*v[2];
        const float cz = q[0]*v[1] - q[1]*v[0];
        o[0] = v[0] + 2.0f*(q[3]*cx + q[1]*cz - q[2]*cy);
        o[1] = v[1] + 2.0f*(q[3]*cy + q[2]*cx - q[0]*cz);
        o[2] = v[2] + 2.0f*(q[3]*cz + q[0]*cy - q[1]*cx);
    };

    // The found rotation, normalised (the record's w can carry scale exactly
    // as the pose root's does), then dq = qfin * conj(bq0n).
    const float bn = sqrtf(bq0[0]*bq0[0] + bq0[1]*bq0[1] +
                           bq0[2]*bq0[2] + bq0[3]*bq0[3]);
    if (!(bn > 1e-6f)) return false;
    const float bc[4] = {-bq0[0]/bn, -bq0[1]/bn, -bq0[2]/bn, bq0[3]/bn};
    float dq[4];
    qmul(qfin, bc, dq);
    const float dn = sqrtf(dq[0]*dq[0] + dq[1]*dq[1] +
                           dq[2]*dq[2] + dq[3]*dq[3]);
    if (!(dn > 1e-6f)) return false;
    for (int i = 0; i < 4; ++i) dq[i] /= dn;

    bool wrote = false;
    for (int s = 0; s < 2; ++s) {
        // Side 0 is the left chain. Weight 0 means the free end keeps it:
        // the controller when tracked (build 141), the animation otherwise.
        const float w = (s == 0) ? lw : 1.0f;
        const uint32_t n = g_hand_n[s].load(std::memory_order_acquire);

        // Build 141: the free reach. Build 143 rewrote the bound: the old
        // one clamped the step at 1.2 m from the hand's CURRENT position,
        // and on every animation-reset entry that position is the animated
        // hand, which at rest legitimately sits over a metre from a lowered
        // real hand, so the clamp oscillated at frame rate and pinned the
        // hand at its edge (the 2026-08-31 screenshot). The bound is now
        // ANATOMICAL: the target may sit anywhere within the arm's own
        // measured reach of the SHOULDER JOINT, which is stable, cannot
        // oscillate, and turns over-reach into a straight arm toward the
        // controller instead of a pinned mess.
        float dmodel[3] = {0.0f, 0.0f, 0.0f};
        bool  freeok = false;
        if (s == 0 && flp && n > 0) {
            float r0[4];
            if (read_block(buf + (uint64_t)g_hand_node[0][0] * 0x20,
                           r0, sizeof(r0)) &&
                isfinite(r0[0]) && isfinite(r0[1]) && isfinite(r0[2])) {
                // The controller into model space, then the target.
                const float cr[4] = {-rootn[0], -rootn[1],
                                     -rootn[2],  rootn[3]};
                const float rel[3] = {flp[0] - rt0[0], flp[1] - rt0[1],
                                      flp[2] - rt0[2]};
                float tgt[3];
                qrot(cr, rel, tgt);
                // BUILD 213: the anchor OUT of the target, so everything from
                // here to the write is in the animated frame: the bound, the
                // arm scale about the shoulder, and the found-hand fallback,
                // all measured against records the animation authored. Put
                // back at the bottom, so the hand is written where his real
                // hand is and the chain is what moves.
                float anchH[3];
                const bool anchoredH = arm_anchor_frame(buf, anchH);
                if (anchoredH) {
                    tgt[0] -= anchH[0]; tgt[1] -= anchH[1]; tgt[2] -= anchH[2];
                }
                if (isfinite(tgt[0]) && isfinite(tgt[1]) &&
                    isfinite(tgt[2])) {
                    // Reach bound about the shoulder joint (the Arm record)
                    // when the arm is resolved; a plain 1.0 m about the
                    // found hand otherwise.
                    float aj[4];
                    const bool armok =
                        g_arm_n[0].load(std::memory_order_acquire) >= 10 &&
                        read_block(buf + (uint64_t)g_arm_node[0][0] * 0x20,
                                   aj, sizeof(aj)) &&
                        isfinite(aj[0]) && isfinite(aj[1]) && isfinite(aj[2]);
                    const float L1 = g_seg_len[0][0], L2 = g_seg_len[0][1];
                    const float lensane = (L1 < 1.0f && L2 < 1.0f) ? 1 : 0;
                    // BUILD 177: the centre is the Arm record as the FIRST
                    // call on this buffer found it, the animated shoulder.
                    // On the second call that record is our own moved
                    // shoulder (176) and the target would chase it between
                    // the two writes (review finding 3).
                    static float    s_bc[3]   = {};
                    static uint64_t s_bc_buf  = 0;
                    if (armok && buf != s_bc_buf) {
                        s_bc[0] = aj[0]; s_bc[1] = aj[1]; s_bc[2] = aj[2];
                        s_bc_buf = buf;
                    }
                    const float* c = armok ? s_bc : r0;
                    // BUILD 213: the target arrives one anchor short (above),
                    // so this bound is measured in the animated frame, about
                    // the animated shoulder, exactly as build 212 measured it.
                    // BUILD 283: with the shoulder pull on, the bound also grows by the pull cap (the limb is carried
                    // that far toward the hand in arms_solve), and the arm scale below is skipped.
                    const bool pullon = g_wgun_pull.load(std::memory_order_relaxed) != 0;
                    const float R = (armok && lensane)
                                        ? (L1 + L2) * 1.10f +
                                          (pullon ? g_wgun_pull_max.load(std::memory_order_relaxed) * 0.001f : 0.0f)
                                        : 1.00f;
                    float v[3] = {tgt[0]-c[0], tgt[1]-c[1], tgt[2]-c[2]};
                    // 2026-09-01: THE ARM SCALE, about the shoulder, BEFORE
                    // the bound below. Maps a full real reach onto a full
                    // avatar reach instead of letting the clamp clip it, which
                    // is what 93 to 100 percent hfar was. Identity at 1.00,
                    // and it only ever touches this IK target: the two-hand
                    // aim reads the raw controller positions, so the barrel
                    // cannot move from here. The clamp stays as the net.
                    const float hs = pullon ? 1.0f : g_hand_scale.load(std::memory_order_relaxed);
                    if (hs != 1.0f && isfinite(hs)) {
                        v[0] *= hs; v[1] *= hs; v[2] *= hs;
                        tgt[0] = c[0] + v[0];
                        tgt[1] = c[1] + v[1];
                        tgt[2] = c[2] + v[2];
                    }
                    const float vl = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
                    if (isfinite(vl)) {
                        if (vl > R && vl > 1e-4f) {
                            const float sc = R / vl;
                            tgt[0] = c[0] + v[0]*sc;
                            tgt[1] = c[1] + v[1]*sc;
                            tgt[2] = c[2] + v[2]*sc;
                            grwxr_wg_hfar = grwxr_wg_hfar + 1;
                        }
                        // BUILD 213: back to the frame the hand is written in
                        if (anchoredH) {
                            tgt[0] += anchH[0]; tgt[1] += anchH[1]; tgt[2] += anchH[2];
                        }
                        dmodel[0] = tgt[0] - r0[0];
                        dmodel[1] = tgt[1] - r0[1];
                        dmodel[2] = tgt[2] - r0[2];
                        freeok = isfinite(dmodel[0]) &&
                                 isfinite(dmodel[1]) && isfinite(dmodel[2]);
                    }
                }
            }
        }

        // BUILD 158: the wrist delta, model space. Built only while the free
        // path is live; leaving the free path drops the latch so the next
        // entry relatches at identity.
        // BUILD 161: pool-sized table, seeded entry, held delta on a miss.
        // The WrRef block above carries the three changes and the reason.
        float wq[4] = {0, 0, 0, 1};
        bool  wrot = false;
        float r0pos[3] = {0, 0, 0};
        if (s == 0) {
            // Matrix to quaternion, trace-major branch. Shared by the fresh
            // path and the held path, so both produce the same rotation from
            // the same delta.
            auto m2q = [](const float D[9], float q[4]) -> bool {
                const float tr = D[0] + D[4] + D[8];
                if (tr > 0.0f) {
                    const float sq = sqrtf(tr + 1.0f) * 2.0f;
                    q[3] = 0.25f * sq;
                    q[0] = (D[7] - D[5]) / sq;
                    q[1] = (D[2] - D[6]) / sq;
                    q[2] = (D[3] - D[1]) / sq;
                } else if (D[0] > D[4] && D[0] > D[8]) {
                    const float sq = sqrtf(1.0f + D[0] - D[4] - D[8]) * 2.0f;
                    q[3] = (D[7] - D[5]) / sq;
                    q[0] = 0.25f * sq;
                    q[1] = (D[1] + D[3]) / sq;
                    q[2] = (D[2] + D[6]) / sq;
                } else if (D[4] > D[8]) {
                    const float sq = sqrtf(1.0f + D[4] - D[0] - D[8]) * 2.0f;
                    q[3] = (D[2] - D[6]) / sq;
                    q[0] = (D[1] + D[3]) / sq;
                    q[1] = 0.25f * sq;
                    q[2] = (D[5] + D[7]) / sq;
                } else {
                    const float sq = sqrtf(1.0f + D[8] - D[0] - D[4]) * 2.0f;
                    q[3] = (D[3] - D[1]) / sq;
                    q[0] = (D[2] + D[6]) / sq;
                    q[1] = (D[5] + D[7]) / sq;
                    q[2] = 0.25f * sq;
                }
                const float wn = sqrtf(q[0]*q[0] + q[1]*q[1] +
                                       q[2]*q[2] + q[3]*q[3]);
                if (!(wn > 1e-6f) || !isfinite(wn)) return false;
                for (int i = 0; i < 4; ++i) q[i] /= wn;
                return true;
            };
            // The hand bone's found position in THIS buffer, the pivot the
            // finger bones rotate about. Read fresh on every path, never
            // remembered across buffers.
            auto pivot = [&](float out[3]) -> bool {
                float rp[4];
                if (read_block(buf + (uint64_t)g_hand_node[0][0] * 0x20,
                               rp, sizeof(rp)) &&
                    isfinite(rp[0]) && isfinite(rp[1]) && isfinite(rp[2])) {
                    out[0] = rp[0]; out[1] = rp[1]; out[2] = rp[2];
                    return true;
                }
                return false;
            };

            // w at or above the full-ride threshold, or the wrist switched
            // off, drops the latch at once: the gun owns the hand there, and
            // a latch that survived a grab would snap the wrist by the whole
            // grabbed-time delta on release. Everything else on the free
            // path is "live", and a live call without usable vectors is a
            // MISS, held rather than wiped (BUILD 161, change 3).
            const bool live = w < 0.999f &&
                              g_wgun_wrist.load(std::memory_order_relaxed);
            // World vectors into model space with the same conjugate the
            // position took. Shared by both routes below.
            const float cr2[4] = {-rootn[0], -rootn[1], -rootn[2], rootn[3]};
            // An orthonormal frame from two vectors, rows X Y Z with Y kept,
            // Z squared against it, X = Y x Z. False on a degenerate pair.
            auto orthoframe = [](float y[3], float z[3], float out[9]) -> bool {
                const float yl = sqrtf(y[0]*y[0] + y[1]*y[1] + y[2]*y[2]);
                if (!(yl > 1e-4f)) return false;
                for (int i = 0; i < 3; ++i) y[i] /= yl;
                const float d = z[0]*y[0] + z[1]*y[1] + z[2]*y[2];
                for (int i = 0; i < 3; ++i) z[i] -= d * y[i];
                const float zl = sqrtf(z[0]*z[0] + z[1]*z[1] + z[2]*z[2]);
                if (!(zl > 1e-4f)) return false;
                for (int i = 0; i < 3; ++i) z[i] /= zl;
                out[0] = y[1]*z[2] - y[2]*z[1];
                out[1] = y[2]*z[0] - y[0]*z[2];
                out[2] = y[0]*z[1] - y[1]*z[0];
                out[3] = y[0]; out[4] = y[1]; out[5] = y[2];
                out[6] = z[0]; out[7] = z[1]; out[8] = z[2];
                return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
            };
            if (!live) { g_wr_n = 0; g_wr_miss = 0; }

            // BUILD 162: THE ABSOLUTE ROUTE. See g_wgun_wrist_abs for what
            // and why. The bone's anatomical frame from its own knuckles,
            // the controller's from the grip pose, and the rotation that
            // lays the first onto the second, about the hand bone.
            bool absdone = false;
            if (live && freeok &&
                g_wgun_wrist_abs.load(std::memory_order_relaxed)) {
                float gy[3], gz[3];
                if (!aimtrace::ctrl_gripf_l(gy, gz)) {
                    grwxr_wr_nogrip = grwxr_wr_nogrip + 1;
                } else if (g_hand_anat_n[0].load(std::memory_order_acquire) < 4) {
                    grwxr_wr_noanat = grwxr_wr_noanat + 1;
                } else {
                    // The hand and three knuckles as found in THIS buffer.
                    float ph[4], pi[4], pm[4], pp[4];
                    bool rok =
                        read_block(buf + (uint64_t)g_hand_anat[0][0] * 0x20,
                                   ph, sizeof(ph)) &&
                        read_block(buf + (uint64_t)g_hand_anat[0][1] * 0x20,
                                   pi, sizeof(pi)) &&
                        read_block(buf + (uint64_t)g_hand_anat[0][2] * 0x20,
                                   pm, sizeof(pm)) &&
                        read_block(buf + (uint64_t)g_hand_anat[0][3] * 0x20,
                                   pp, sizeof(pp));
                    for (int i = 0; rok && i < 3; ++i)
                        rok = isfinite(ph[i]) && isfinite(pi[i]) &&
                              isfinite(pm[i]) && isfinite(pp[i]);
                    float Bb[9], G[9];
                    bool fok = false;
                    if (rok) {
                        // Bone frame: Y knuckles to wrist, Z index knuckle
                        // to pinky knuckle, X = Y x Z the palm normal.
                        float yb[3] = {ph[0]-pm[0], ph[1]-pm[1], ph[2]-pm[2]};
                        float zb[3] = {pp[0]-pi[0], pp[1]-pi[1], pp[2]-pi[2]};
                        fok = orthoframe(yb, zb, Bb);
                    }
                    if (fok) {
                        // Grip frame, into model space, squared the same way.
                        float ym[3], zm[3];
                        qrot(cr2, gy, ym);
                        qrot(cr2, gz, zm);
                        fok = orthoframe(ym, zm, G);
                    }
                    if (fok) {
                        // W = G^T * Bb: takes the bone's anatomical axes
                        // onto the controller's, rows-as-basis, the same
                        // product shape as the delta below.
                        float W[9];
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j)
                                W[i*3+j] = G[0*3+i]*Bb[0*3+j] +
                                           G[1*3+i]*Bb[1*3+j] +
                                           G[2*3+i]*Bb[2*3+j];
                        if (m2q(W, wq) && pivot(r0pos)) {
                            wrot = true;
                            absdone = true;
                            grwxr_wr_abs = grwxr_wr_abs + 1;
                            // The delta table idles while this route runs,
                            // so a fallback relatches clean.
                            g_wr_n = 0;
                            g_wr_miss = 0;
                        }
                    }
                    if (!absdone) grwxr_wr_noanat = grwxr_wr_noanat + 1;
                }
            }

            float fwv[3], upv[3];
            const bool vecs = live && !absdone && freeok &&
                              aimtrace::ctrl_vecs_l(fwv, upv);
            bool fresh = false;
            if (live && !absdone) ++g_wr_call;
            if (vecs) {
                // World vectors into model space with the same conjugate the
                // position took, then an orthonormal frame: right = fwd x up
                // (engine basis x right, y forward, z up), up rebuilt from
                // right x fwd so the frame closes even if the inputs drift
                // off perpendicular. cr2 is shared with the absolute route
                // above.
                float fm[3], um[3];
                qrot(cr2, fwv, fm);
                qrot(cr2, upv, um);
                float rt[3] = {fm[1]*um[2] - fm[2]*um[1],
                               fm[2]*um[0] - fm[0]*um[2],
                               fm[0]*um[1] - fm[1]*um[0]};
                const float rl = sqrtf(rt[0]*rt[0] + rt[1]*rt[1] +
                                       rt[2]*rt[2]);
                const float fl = sqrtf(fm[0]*fm[0] + fm[1]*fm[1] +
                                       fm[2]*fm[2]);
                if (rl > 0.5f && fl > 0.5f) {
                    for (int i = 0; i < 3; ++i) { rt[i] /= rl; fm[i] /= fl; }
                    um[0] = rt[1]*fm[2] - rt[2]*fm[1];
                    um[1] = rt[2]*fm[0] - rt[0]*fm[2];
                    um[2] = rt[0]*fm[1] - rt[1]*fm[0];
                    float B[9] = {rt[0], rt[1], rt[2],
                                  fm[0], fm[1], fm[2],
                                  um[0], um[1], um[2]};
                    // BUILD 158: this buffer's own latch, never another's.
                    int slot = -1;
                    for (int i = 0; i < g_wr_n; ++i)
                        if (g_wr_ref[i].buf == buf) { slot = i; break; }
                    if (slot < 0) {
                        // BUILD 161, change 2: the pool's live delta, from
                        // the most recently touched slot that has one.
                        int src = -1;
                        for (int i = 0; i < g_wr_n; ++i)
                            if (g_wr_ref[i].dok &&
                                (src < 0 ||
                                 g_wr_ref[i].seen > g_wr_ref[src].seen))
                                src = i;
                        if (g_wr_n < kWrSlots) {
                            slot = g_wr_n++;
                            grwxr_wr_latch = grwxr_wr_latch + 1;
                        } else {
                            // BUILD 161, change 1: evict the oldest, never a
                            // fixed slot.
                            slot = 0;
                            for (int i = 1; i < g_wr_n; ++i)
                                if (g_wr_ref[i].seen < g_wr_ref[slot].seen)
                                    slot = i;
                            grwxr_wr_evict = grwxr_wr_evict + 1;
                            if (src == slot) src = -1;
                        }
                        g_wr_ref[slot].buf = buf;
                        g_wr_ref[slot].dok = 0;
                        if (src >= 0) {
                            // ref = B * Dlive, so that B^T * ref == Dlive:
                            // this buffer starts where the pool already is.
                            const float* Dl = g_wr_ref[src].D;
                            float* R = g_wr_ref[slot].B;
                            for (int i = 0; i < 3; ++i)
                                for (int j = 0; j < 3; ++j)
                                    R[i*3+j] = B[i*3+0]*Dl[0*3+j] +
                                               B[i*3+1]*Dl[1*3+j] +
                                               B[i*3+2]*Dl[2*3+j];
                            grwxr_wr_seed = grwxr_wr_seed + 1;
                        } else {
                            memcpy(g_wr_ref[slot].B, B, sizeof(B));
                        }
                    }
                    g_wr_ref[slot].seen = g_wr_call;
                    // D = B^T * ref, rows-as-basis, the rotation that takes
                    // the latched frame onto the current one in model space.
                    // Kept in the slot so a miss can replay it.
                    float* D = g_wr_ref[slot].D;
                    const float* R = g_wr_ref[slot].B;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            D[i*3+j] = B[0*3+i]*R[0*3+j] +
                                       B[1*3+i]*R[1*3+j] +
                                       B[2*3+i]*R[2*3+j];
                    g_wr_ref[slot].dok = 1;
                    if (m2q(D, wq) && pivot(r0pos)) wrot = true;
                    fresh = true;
                    g_wr_miss = 0;
                }
            }
            if (live && !absdone && !fresh) {
                // BUILD 161, change 3: a miss on the live free path. No
                // vectors, a degenerate frame, or no position this call.
                // Hold this buffer's last delta instead of showing the
                // animated wrist for one call, and wipe only after a run of
                // misses long enough to mean tracking is gone rather than
                // one publish skipped.
                grwxr_wr_miss = grwxr_wr_miss + 1;
                if (++g_wr_miss >= kWrMissWipe) {
                    g_wr_n = 0;
                    g_wr_miss = 0;
                    grwxr_wr_wipe = grwxr_wr_wipe + 1;
                } else {
                    for (int i = 0; i < g_wr_n; ++i) {
                        if (g_wr_ref[i].buf != buf) continue;
                        if (g_wr_ref[i].dok && m2q(g_wr_ref[i].D, wq) &&
                            pivot(r0pos)) {
                            wrot = true;
                            g_wr_ref[i].seen = g_wr_call;
                            grwxr_wr_hold = grwxr_wr_hold + 1;
                        }
                        break;
                    }
                }
            }
        }
        if (!(w > 0.0f) && !freeok) continue;
        if (freeok && w < 0.999f) grwxr_wg_free = grwxr_wg_free + 1;

        for (uint32_t k = 0; k < n && k < 21; ++k) {
            const uint64_t hrec = buf + (uint64_t)g_hand_node[s][k] * 0x20;
            float r[8];
            if (!read_block(hrec, r, sizeof(r))) continue;
            bool fin = true;
            for (int i = 0; i < 8; ++i)
                if (!isfinite(r[i])) { fin = false; break; }
            if (!fin) continue;

            // BUILD 160: the stow signal, right hand only, found state only.
            if (s == 1 && k == 0) {
                const float sdx = r[0] - bt0[0];
                const float sdy = r[1] - bt0[1];
                const float sdz = r[2] - bt0[2];
                const float sd  = sqrtf(sdx*sdx + sdy*sdy + sdz*sdz);
                if (isfinite(sd)) {
                    const uint32_t mm = (uint32_t)(sd * 1000.0f);
                    if (mm < g_stow_d_min.load(std::memory_order_relaxed))
                        g_stow_d_min.store(mm, std::memory_order_relaxed);
                    if (mm > g_stow_d_max.load(std::memory_order_relaxed))
                        g_stow_d_max.store(mm, std::memory_order_relaxed);
                }
            }

            // BUILD 155: the survival check, on the hand bone only, using the
            // record we have just read as FOUND and the value we wrote into
            // this same record on the previous call. Reads and counters only.
            // BUILD 156: only when this call's buffer is the one the
            // remembered write went into; a cross-buffer encounter is counted
            // in xbuf and skipped, never compared.
            if (k == 0 && g_hb_wrote_ok[s] && buf != g_hb_buf[s]) {
                g_hb_xbuf[s].fetch_add(1, std::memory_order_relaxed);
            } else if (k == 0 && g_hb_wrote_ok[s]) {
                const float dx = r[0] - g_hb_wrote[s][0];
                const float dy = r[1] - g_hb_wrote[s][1];
                const float dz = r[2] - g_hb_wrote[s][2];
                const float d  = sqrtf(dx*dx + dy*dy + dz*dz);
                if (isfinite(d)) {
                    g_hb_checks[s].fetch_add(1, std::memory_order_relaxed);
                    if (d <= kHbKeptM)
                        g_hb_kept[s].fetch_add(1, std::memory_order_relaxed);
                    const uint32_t mm = (uint32_t)(d * 1000.0f);
                    g_hb_last_mm[s].store(mm, std::memory_order_relaxed);
                    if (mm > g_hb_drift_mm[s].load(std::memory_order_relaxed))
                        g_hb_drift_mm[s].store(mm, std::memory_order_relaxed);
                }
            }

            const float rel[3] = {r[0] - bt0[0], r[1] - bt0[1], r[2] - bt0[2]};
            float nt[3];
            qrot(dq, rel, nt);
            nt[0] += tfin[0]; nt[1] += tfin[1]; nt[2] += tfin[2];
            float nq[4];
            qmul(dq, r + 4, nq);

            // Build 140: the partial grab. Lerp toward the full ride from the
            // FREE END, hemisphere-corrected for the quaternion and NOT
            // renormalised, because both endpoints carry the bone's own norm
            // (which may be scale) and a blend of near-equal norms keeps it.
            // At w = 1 this is exactly the pre-140 write.
            // Build 141: the free end's position is the record plus the
            // reach, so at w = 0 the hand sits on the controller and the
            // grab slides it from your hand onto the foregrip.
            if (w < 0.999f) {
                // BUILD 158: the free endpoint carries the wrist delta. The
                // chain rotates about the hand bone by wq, so the fingers
                // travel with the palm; position-only when the wrist is off
                // or unlatched, which is exactly the build-141 endpoint.
                float f0 = r[0] + dmodel[0];
                float f1 = r[1] + dmodel[1];
                float f2 = r[2] + dmodel[2];
                float fq[4] = {r[4], r[5], r[6], r[7]};
                if (wrot) {
                    const float rel2[3] = {r[0] - r0pos[0], r[1] - r0pos[1],
                                           r[2] - r0pos[2]};
                    float rr[3];
                    qrot(wq, rel2, rr);
                    f0 = r0pos[0] + dmodel[0] + rr[0];
                    f1 = r0pos[1] + dmodel[1] + rr[1];
                    f2 = r0pos[2] + dmodel[2] + rr[2];
                    qmul(wq, r + 4, fq);
                }
                nt[0] = f0 + w * (nt[0] - f0);
                nt[1] = f1 + w * (nt[1] - f1);
                nt[2] = f2 + w * (nt[2] - f2);
                const float d = fq[0]*nq[0] + fq[1]*nq[1] +
                                fq[2]*nq[2] + fq[3]*nq[3];
                const float sg = d < 0.0f ? -1.0f : 1.0f;
                nq[0] = fq[0] + w * (sg * nq[0] - fq[0]);
                nq[1] = fq[1] + w * (sg * nq[1] - fq[1]);
                nq[2] = fq[2] + w * (sg * nq[2] - fq[2]);
                nq[3] = fq[3] + w * (sg * nq[3] - fq[3]);
                // BUILD 146: THE MELT FIX. A lerped quaternion's norm is the
                // CHORD and always dips below the endpoints', and in these
                // records the norm carries scale, so every partial-authority
                // frame was shrinking the hand a few percent: the owner's
                // "weird texture warping". Restore the found norm; both
                // endpoints carry it (dq is unit), so this loses nothing.
                const float bn2 = sqrtf(r[4]*r[4] + r[5]*r[5] +
                                        r[6]*r[6] + r[7]*r[7]);
                const float ln2 = sqrtf(nq[0]*nq[0] + nq[1]*nq[1] +
                                        nq[2]*nq[2] + nq[3]*nq[3]);
                if (ln2 > 1e-6f && bn2 > 1e-6f) {
                    const float k2 = bn2 / ln2;
                    nq[0] *= k2; nq[1] *= k2; nq[2] *= k2; nq[3] *= k2;
                }
            }
            if (!isfinite(nt[0]) || !isfinite(nt[1]) || !isfinite(nt[2]) ||
                !isfinite(nq[0]) || !isfinite(nq[1]) || !isfinite(nq[2]) ||
                !isfinite(nq[3]))
                continue;

            // The record's w lane at +0x0C is left exactly as the engine
            // wrote it, so the translation write is 12 bytes, not 16.
            if (write_block(hrec + 0x00, nt, sizeof(nt)) &&
                write_block(hrec + 0x10, nq, sizeof(nq))) {
                wrote = true;
                if (k == 0) {                     // build 142: the hand bone
                    hand_out[s][0] = nt[0];
                    hand_out[s][1] = nt[1];
                    hand_out[s][2] = nt[2];
                    hand_found[s][0] = r[0];      // build 143: as found
                    hand_found[s][1] = r[1];
                    hand_found[s][2] = r[2];
                    hand_ok[s] = true;
                    hand_fq[s][0] = r[4]; hand_fq[s][1] = r[5];   // BUILD 164
                    hand_fq[s][2] = r[6]; hand_fq[s][3] = r[7];
                    // BUILD 165: the across-fist axis as FOUND, read before
                    // the knuckles are written (they come later in k).
                    if (g_hand_anat_n[s].load(std::memory_order_acquire) >= 4) {
                        float pi2[4], pp2[4];
                        if (read_block(buf + (uint64_t)g_hand_anat[s][1] * 0x20,
                                       pi2, sizeof(pi2)) &&
                            read_block(buf + (uint64_t)g_hand_anat[s][3] * 0x20,
                                       pp2, sizeof(pp2))) {
                            float z[3] = {pp2[0]-pi2[0], pp2[1]-pi2[1],
                                          pp2[2]-pi2[2]};
                            const float zl = sqrtf(z[0]*z[0] + z[1]*z[1] +
                                                   z[2]*z[2]);
                            if (zl > 1e-4f && isfinite(zl)) {
                                hand_ax[s][0] = z[0]/zl;
                                hand_ax[s][1] = z[1]/zl;
                                hand_ax[s][2] = z[2]/zl;
                            }
                        }
                    }
                    // BUILD 163: written over found, both normalised (the
                    // record's norm carries scale), so the arm solve can
                    // split the roll out of exactly what the hand received.
                    {
                        const float fn = sqrtf(r[4]*r[4] + r[5]*r[5] +
                                               r[6]*r[6] + r[7]*r[7]);
                        const float wn2 = sqrtf(nq[0]*nq[0] + nq[1]*nq[1] +
                                                nq[2]*nq[2] + nq[3]*nq[3]);
                        if (fn > 1e-6f && wn2 > 1e-6f) {
                            const float fc[4]  = {-r[4]/fn, -r[5]/fn,
                                                  -r[6]/fn,  r[7]/fn};
                            const float wq2[4] = {nq[0]/wn2, nq[1]/wn2,
                                                  nq[2]/wn2, nq[3]/wn2};
                            qmul(wq2, fc, hand_rot[s]);
                        }
                    }
                    // BUILD 155: what the next call will compare against.
                    // Stored only on a write that actually landed, so a
                    // skipped bone cannot be mistaken for a clobbered one.
                    // BUILD 156: and WHERE it landed, so the check can
                    // refuse to compare across rigs.
                    g_hb_wrote[s][0] = nt[0];
                    g_hb_wrote[s][1] = nt[1];
                    g_hb_wrote[s][2] = nt[2];
                    g_hb_buf[s]      = buf;
                    g_hb_wrote_ok[s] = 1;
                }
            }
        }
    }
    return wrote;
}

// ---------------------------------------------------------------------------
// BUILD 140: the bone-level arm hide. See g_wgun_hide_arms for what and why.
// Every resolved arm and helper record is moved onto its side's shoulder
// record position and its quaternion is shrunk to 2 percent. Same guards,
// same thread, same "skip, never guess" as hands_apply.
// BUILD 175: HANDS ONLY. Owner, 2026-09-02 22:50, after 163 to 174: "can we
// just go just hands remove the forearms for now we have got to move on."
// A skin vertex blended between the hand and a forearm bone lands at the
// weighted mix of the two transforms, so a forearm collapsed onto the
// SHOULDER drags every wrist vertex toward the shoulder as a fan. The
// forearm family (ForeArm, O_Elbow, O_01..O_04, the look-at; kArmChain
// slots 1, 2, 5..9) therefore collapses onto the HAND record as hands_apply
// just wrote it, so the blended wrist vertices stay at the wrist and the
// hand ends cleanly; the upper-arm family (Arm, L_01Arm, O_02Arm; slots
// 0, 3, 4) still collapses onto the shoulder. Both sides. Nothing else
// changes: the hand write, the gun root and the shot chain are untouched,
// and with the arms hidden arms_solve does not run at all.
// ---------------------------------------------------------------------------
bool arms_collapse(uint64_t buf) {
    bool wrote = false;
    for (int s = 0; s < 2; ++s) {
        const uint32_t n = g_arm_n[s].load(std::memory_order_acquire);
        if (!n || g_shoulder_node[s] == 0xFFFFu) continue;
        float sh[4];
        if (!read_block(buf + (uint64_t)g_shoulder_node[s] * 0x20,
                        sh, sizeof(sh)))
            continue;
        if (!isfinite(sh[0]) || !isfinite(sh[1]) || !isfinite(sh[2]))
            continue;
        // BUILD 175: the hand's position, the forearm family's collapse
        // point. Falls back to the shoulder if the hand cannot be read.
        float hp[4] = {sh[0], sh[1], sh[2], 0.0f};
        {
            float hr[4];
            if (read_block(buf + (uint64_t)g_hand_node[s][0] * 0x20,
                           hr, sizeof(hr)) &&
                isfinite(hr[0]) && isfinite(hr[1]) && isfinite(hr[2])) {
                hp[0] = hr[0]; hp[1] = hr[1]; hp[2] = hr[2];
            }
        }
        for (uint32_t k = 0; k < n && k < 10; ++k) {
            if (g_arm_node[s][k] == 0xFFFFu) continue;   // build 142 slots
            const uint64_t arec = buf + (uint64_t)g_arm_node[s][k] * 0x20;
            float r[8];
            if (!read_block(arec, r, sizeof(r))) continue;
            bool fin = true;
            for (int i = 0; i < 8; ++i)
                if (!isfinite(r[i])) { fin = false; break; }
            if (!fin) continue;
            const bool fore = (k == 1 || k == 2 || k >= 5);
            const float* cp = fore ? hp : sh;
            const float nt[3] = {cp[0], cp[1], cp[2]};
            const float nq[4] = {r[4] * 0.02f, r[5] * 0.02f,
                                 r[6] * 0.02f, r[7] * 0.02f};
            if (write_block(arec + 0x00, nt, sizeof(nt)) &&
                write_block(arec + 0x10, nq, sizeof(nq)))
                wrote = true;
        }
    }
    return wrote;
}

// ---------------------------------------------------------------------------
// BUILD 142: THE ARM SOLVE. See g_wgun_arms. All model space. hand is the
// hand record position as hands_apply wrote it. Segment lengths are running
// minimums keyed to the armed skeleton (reset on a swap): the animation
// never stretches a bone and our own writes can only inflate the measured
// wrist span in the beyond-reach case, so the minimum is the rig's truth.
// Returns 1 solved, 2 solved-with-straight-arm-clamp, 0 skipped.
// ---------------------------------------------------------------------------
// BUILD 143: hfound is the hand record as FOUND (lengths and the old
// direction are measured against it), hand is the written target the arm
// must reach. 142 measured both from the target, which inflated the wrist
// length past the sanity guard and the solve never ran (ik=0 all session).
int arms_solve(uint64_t buf, int s, const float hfound[3],
               const float hand_in[3], const float hrot[4],
               const float hfq[4], const float hax[3]) {
    // BUILD 213, second pass (CX-025). The whole solve below is build 212's,
    // untouched, and it runs in the ANIMATED frame: the shoulder is where the
    // animation left it, l1 and l2 measure the real bones, the segment minima
    // are honest, the pole, dmax, the 179 shoulder cap and the 180 collarbone
    // all compare records the animation authored. The only thing that changes
    // is that the hand target arrives one anchor short, and the finished limb
    // is translated by that anchor at the write. hfound is the animated hand
    // and is NOT shifted; the orientations are translation-free.
    float anchA[3];
    // BOTH REVIEWS, second round: arms_solve runs TWICE on a rig buffer, and
    // the second call re-reads recA, which by then holds OUR first-call write.
    // Applying the anchor again put the shoulder at A + 2*anch and solved the
    // upper arm for a span one anchor short. On the second call the records
    // are ALREADY in the anchored frame and the true target is already the
    // right target, so the second call must do neither the shift nor the
    // translation: it is build 212's solve on already-carried records. The
    // same key the build 177 segment minima use.
    bool anchoredA = arm_anchor_frame(buf, anchA) && (buf != g_tp_wrote[s]);
    float hand[3] = {hand_in[0] - (anchoredA ? anchA[0] : 0.0f),
                           hand_in[1] - (anchoredA ? anchA[1] : 0.0f),
                           hand_in[2] - (anchoredA ? anchA[2] : 0.0f)};
    auto skip = []() {
        grwxr_wg_ikskip = grwxr_wg_ikskip + 1;
        return 0;
    };
    auto qmul = [](const float a[4], const float b[4], float o[4]) {
        o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
        o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
        o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
        o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    };
    auto qrot = [](const float q[4], const float v[3], float o[3]) {
        const float cx = q[1]*v[2] - q[2]*v[1];
        const float cy = q[2]*v[0] - q[0]*v[2];
        const float cz = q[0]*v[1] - q[1]*v[0];
        o[0] = v[0] + 2.0f*(q[3]*cx + q[1]*cz - q[2]*cy);
        o[1] = v[1] + 2.0f*(q[3]*cy + q[2]*cx - q[0]*cz);
        o[2] = v[2] + 2.0f*(q[3]*cz + q[0]*cy - q[1]*cx);
    };
    // Shortest arc taking unit a onto unit b; false only at the exact 180.
    auto arc = [](const float a[3], const float b[3], float o[4]) {
        const float c = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
        if (c < -0.9999f) return false;
        o[0] = a[1]*b[2] - a[2]*b[1];
        o[1] = a[2]*b[0] - a[0]*b[2];
        o[2] = a[0]*b[1] - a[1]*b[0];
        o[3] = 1.0f + c;
        const float n = sqrtf(o[0]*o[0] + o[1]*o[1] + o[2]*o[2] + o[3]*o[3]);
        if (!(n > 1e-6f)) return false;
        o[0] /= n; o[1] /= n; o[2] /= n; o[3] /= n;
        return true;
    };

    if (g_arm_n[s].load(std::memory_order_acquire) < 10) return skip();
    const uint64_t recA = buf + (uint64_t)g_arm_node[s][0] * 0x20;
    const uint64_t recF = buf + (uint64_t)g_arm_node[s][1] * 0x20;
    float A[8], F[8];
    if (!read_block(recA, A, sizeof(A)) || !read_block(recF, F, sizeof(F)))
        return skip();
    for (int i = 0; i < 8; ++i)
        if (!isfinite(A[i]) || !isfinite(F[i])) return skip();

    const float S[3]  = {A[0], A[1], A[2]};       // shoulder joint, kept
    const float E0[3] = {F[0], F[1], F[2]};       // elbow as found
    float u1[3] = {E0[0]-S[0], E0[1]-S[1], E0[2]-S[2]};
    const float l1 = sqrtf(u1[0]*u1[0] + u1[1]*u1[1] + u1[2]*u1[2]);
    // Build 143: length and old direction from the FOUND hand.
    float u2[3] = {hfound[0]-E0[0], hfound[1]-E0[1], hfound[2]-E0[2]};
    const float l2now = sqrtf(u2[0]*u2[0] + u2[1]*u2[1] + u2[2]*u2[2]);
    if (!(l1 > 0.05f) || !(l2now > 0.05f)) return skip();

    // BUILD 164: the twist probe, side 0, reads only. See g_tp_anim.
    // BUILD 166: roll as a plane angle per bone, and write survival.
    if (s == 0) {
        const bool ours = (buf == g_tp_wrote[0]);
        const float fnrm = sqrtf(F[4]*F[4] + F[5]*F[5] + F[6]*F[6] + F[7]*F[7]);
        if (fnrm > 1e-6f) {
            const float fq[4] = {F[4]/fnrm, F[5]/fnrm, F[6]/fnrm, F[7]/fnrm};
            const float n2o0[3] = {u2[0]/l2now, u2[1]/l2now, u2[2]/l2now};
            // A bone-local axis into model space, projected across the
            // forearm axis; false when it lies along it.
            auto across = [&](const float q[4], int axis, float out[3]) -> bool {
                const float qn = sqrtf(q[0]*q[0] + q[1]*q[1] +
                                       q[2]*q[2] + q[3]*q[3]);
                if (!(qn > 1e-6f)) return false;
                const float qu[4] = {q[0]/qn, q[1]/qn, q[2]/qn, q[3]/qn};
                const float loc[3] = {axis == 0 ? 1.0f : 0.0f,
                                      axis == 1 ? 1.0f : 0.0f,
                                      axis == 2 ? 1.0f : 0.0f};
                float v[3];
                qrot(qu, loc, v);
                const float dd = v[0]*n2o0[0] + v[1]*n2o0[1] + v[2]*n2o0[2];
                for (int i = 0; i < 3; ++i) out[i] = v[i] - dd*n2o0[i];
                const float n = sqrtf(out[0]*out[0] + out[1]*out[1] +
                                      out[2]*out[2]);
                if (!(n > 0.3f) || !isfinite(n)) return false;
                for (int i = 0; i < 3; ++i) out[i] /= n;
                return true;
            };
            auto angle_about = [&](const float a[3], const float b[3]) -> float {
                const float cr[3] = {a[1]*b[2] - a[2]*b[1],
                                     a[2]*b[0] - a[0]*b[2],
                                     a[0]*b[1] - a[1]*b[0]};
                const float sn = cr[0]*n2o0[0] + cr[1]*n2o0[1] + cr[2]*n2o0[2];
                const float cs = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
                const float dg = atan2f(sn, cs) * 57.29578f;
                return isfinite(dg) ? dg : 0.0f;
            };
            auto put = [](std::atomic<uint32_t>& dst, float v) {
                uint32_t b; memcpy(&b, &v, sizeof(b));
                dst.store(b, std::memory_order_relaxed);
            };
            // The ForeArm's own reference across the forearm: local Y, or Z
            // where Y lies along the bone. Every bone is read on the same
            // local axis so the angle is roll and nothing else.
            float fref[3]; int faxis = 1;
            bool fok = across(fq, 1, fref);
            if (!fok) { faxis = 2; fok = across(fq, 2, fref); }
            static const int kProbeSeg[7] = {1, 2, 5, 6, 7, 8, 9};
            for (int i = 0; fok && i < 7; ++i) {
                const uint16_t node = g_arm_node[0][kProbeSeg[i]];
                if (node == 0xFFFFu) continue;
                float r[8];
                if (!read_block(buf + (uint64_t)node * 0x20, r, sizeof(r)))
                    continue;
                bool fin = true;
                for (int j = 0; j < 8; ++j)
                    if (!isfinite(r[j])) { fin = false; break; }
                if (!fin) continue;
                const float rel[3] = {r[0]-E0[0], r[1]-E0[1], r[2]-E0[2]};
                const float t = (rel[0]*n2o0[0] + rel[1]*n2o0[1] +
                                 rel[2]*n2o0[2]) / l2now;
                const float off[3] = {rel[0] - t*l2now*n2o0[0],
                                      rel[1] - t*l2now*n2o0[1],
                                      rel[2] - t*l2now*n2o0[2]};
                const float d = sqrtf(off[0]*off[0] + off[1]*off[1] +
                                      off[2]*off[2]) * 1000.0f;
                float bx[3];
                const float tw = across(r + 4, faxis, bx)
                                     ? angle_about(fref, bx) : 0.0f;
                if (ours) {
                    put(g_tp_ours[i], tw);
                    // Survival: degrees between what we last wrote into
                    // this bone and what is in it now. 0 means held.
                    float keep = -1.0f;
                    if (g_tw_wq_ok[i]) {
                        const float qn = sqrtf(r[4]*r[4] + r[5]*r[5] +
                                               r[6]*r[6] + r[7]*r[7]);
                        const float* w = g_tw_wq[i];
                        const float wn = sqrtf(w[0]*w[0] + w[1]*w[1] +
                                               w[2]*w[2] + w[3]*w[3]);
                        if (qn > 1e-6f && wn > 1e-6f) {
                            float dot = (r[4]*w[0] + r[5]*w[1] +
                                         r[6]*w[2] + r[7]*w[3]) / (qn*wn);
                            if (dot < 0.0f) dot = -dot;
                            if (dot > 1.0f) dot = 1.0f;
                            keep = 2.0f * acosf(dot) * 57.29578f;
                        }
                    }
                    put(g_tp_ours[8 + i], keep);
                } else {
                    put(g_tp_anim[3*i+0], t);
                    put(g_tp_anim[3*i+1], d);
                    put(g_tp_anim[3*i+2], tw);
                }
            }
            // The hand: its found across-fist axis against the same
            // reference, a plane angle like the rest.
            if (fok && hax && hax[0]*hax[0] + hax[1]*hax[1] + hax[2]*hax[2] > 0.25f) {
                float hx[3];
                const float dd = hax[0]*n2o0[0] + hax[1]*n2o0[1] + hax[2]*n2o0[2];
                for (int i = 0; i < 3; ++i) hx[i] = hax[i] - dd*n2o0[i];
                const float n = sqrtf(hx[0]*hx[0] + hx[1]*hx[1] + hx[2]*hx[2]);
                if (n > 0.3f) {
                    for (int i = 0; i < 3; ++i) hx[i] /= n;
                    put(ours ? g_tp_ours[7] : g_tp_anim[21], angle_about(fref, hx));
                }
            }
        }
    }

    // The running-minimum lengths, reset when the armed skeleton changes.
    // File scope (g_seg_len) since build 143: the free-hand bound needs them.
    const uint64_t cur = grwxr_wgun_skel;
    if (cur != g_seg_skel) {
        g_seg_skel = cur;
        g_seg_len[0][0] = g_seg_len[0][1] = 1e9f;
        g_seg_len[1][0] = g_seg_len[1][1] = 1e9f;
    }
    // BUILD 177: the minimum is taken from the FIRST call on a buffer only,
    // the animated frame. On the second call the found elbow is our own
    // write, and since 176 the wrist-first elbow sits closer to the
    // shoulder than L1 on every ordinary frame, so reading it as a length
    // would ratchet L1 down to the floor within seconds (review finding 1).
    // BUILD 284: AND ONLY A SAMPLE THAT IS PLAUSIBLY AN ANIMATED ARM. Build 283's run (grwxr-2984) showed the left upper arm
    // minimum fall from 0.278 to 0.172 m at 17:33:44 and stay there (282's logs never read under 0.278), so the reach was
    // over-measured and the pull carried the left arm too far on 71 percent of frames: a "first call" had read a buffer the
    // engine had not re-authored, which still held our own carried limb. The reference is the first sample of this skeleton
    // that lies in the human range; a later sample counts only within 6 percent of it. Rejections are counted (pull: line).
    if (buf != g_tp_wrote[s]) {
        static float s_ref[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
        static uint64_t s_ref_skel = 0;
        if (s_ref_skel != cur) {
            s_ref_skel = cur;
            s_ref[0][0] = s_ref[0][1] = s_ref[1][0] = s_ref[1][1] = 0.0f;
        }
        if (!(s_ref[s][0] > 0.0f) && l1 > 0.20f && l1 < 0.35f && l2now > 0.20f && l2now < 0.32f) {
            s_ref[s][0] = l1;
            s_ref[s][1] = l2now;
        }
        const bool plausible = s_ref[s][0] > 0.0f &&
                               fabsf(l1 / s_ref[s][0] - 1.0f) <= 0.06f &&
                               fabsf(l2now / s_ref[s][1] - 1.0f) <= 0.06f;
        if (plausible) {
            if (l1 < g_seg_len[s][0]) g_seg_len[s][0] = l1;
            if (l2now < g_seg_len[s][1]) g_seg_len[s][1] = l2now;
        } else {
            g_seg_rej[s] = g_seg_rej[s] + 1;
        }
    }
    const float L1 = g_seg_len[s][0], L2 = g_seg_len[s][1];
    if (L1 > 1.0f || L2 > 1.0f) return skip();    // not yet measured sane
    g_armlen_bits[s][0].store(*(const uint32_t*)&L1,
                              std::memory_order_relaxed);
    g_armlen_bits[s][1].store(*(const uint32_t*)&L2,
                              std::memory_order_relaxed);

    // BUILD 283: THE SHOULDER PULL, first call on a buffer only (the second call reads the carried records and needs none).
    // The target is moved toward the shoulder by exactly the overreach past wgun_pull_k of full extension, solved there in
    // the animated frame, and the finished limb is carried the same distance at the write (the build 213 anchor path), so
    // the hand lands on the controller and the arm is never fully straight. Capped by wgun_pull_max; a capped frame is
    // counted and the old straight-arm clamp takes the rest.
    if (g_wgun_pull.load(std::memory_order_relaxed) && buf != g_tp_wrote[s]) {
        float pk = g_wgun_pull_k.load(std::memory_order_relaxed);
        if (!isfinite(pk) || pk < 0.80f || pk > 1.0f) pk = 0.97f;
        float pmax = g_wgun_pull_max.load(std::memory_order_relaxed) * 0.001f;
        if (!isfinite(pmax) || pmax < 0.0f || pmax > 0.5f) pmax = 0.25f;
        const float pv[3] = {hand[0]-S[0], hand[1]-S[1], hand[2]-S[2]};
        const float pd = sqrtf(pv[0]*pv[0] + pv[1]*pv[1] + pv[2]*pv[2]);
        g_pull_calls[s] = g_pull_calls[s] + 1;
        const float need = pd - pk * (L1 + L2);
        if (isfinite(pd) && pd > 1e-4f && need > 0.0f) {
            const bool capd = need > pmax;
            const float take = capd ? pmax : need;
            const float kk = take / pd;
            const float pull[3] = {pv[0]*kk, pv[1]*kk, pv[2]*kk};
            hand[0] -= pull[0]; hand[1] -= pull[1]; hand[2] -= pull[2];
            anchA[0] += pull[0]; anchA[1] += pull[1]; anchA[2] += pull[2];
            anchoredA = true;
            g_pull_n[s] = g_pull_n[s] + 1;
            if (capd) g_pull_cap[s] = g_pull_cap[s] + 1;
            const uint32_t um = (uint32_t)(take * 1.0e6f);
            g_pull_sum_um[s] += um;
            if (um > g_pull_max_um[s]) g_pull_max_um[s] = um;
        }
    }

    // Reach and elbow. The pole is the animation's own elbow offset off the
    // shoulder-to-target line; a measured direction, not a preference.
    float sh[3] = {hand[0]-S[0], hand[1]-S[1], hand[2]-S[2]};
    float d = sqrtf(sh[0]*sh[0] + sh[1]*sh[1] + sh[2]*sh[2]);
    if (!(d > 1e-4f)) return skip();
    int result = 1;
    const float dmax = L1 + L2 - 0.005f;
    {   // BUILD 173: the reach ratio, before the clamp.
        const float reach = d / (L1 + L2);
        if (isfinite(reach)) {
            if (reach < g_rw_min[s][0]) g_rw_min[s][0] = reach;
            if (reach > g_rw_max[s][0]) g_rw_max[s][0] = reach;
        }
    }
    const float dmin = fabsf(L1 - L2) + 0.005f;
    float E[3];
    float Snew[3] = {S[0], S[1], S[2]};   // BUILD 176: the shoulder may move
    bool wf = false;

    // BUILD 178: the two-bone elbow first, always (the 162 solve: the
    // circle about the shoulder-to-hand line, on the animation's own elbow
    // side). It is the answer when the wrist limit allows it, and the
    // reference the wrist-first solve bends away from when it does not.
    float E2[3] = {0.0f, 0.0f, 0.0f};
    bool  e2ok = false;
    {
        float dd = d;
        if (dd > dmax) dd = dmax;
        if (dd < dmin) dd = dmin;
        const float un = sqrtf(sh[0]*sh[0] + sh[1]*sh[1] + sh[2]*sh[2]);
        const float ux = sh[0]/un, uy = sh[1]/un, uz = sh[2]/un;
        const float a = (L1*L1 - L2*L2 + dd*dd) / (2.0f*dd);
        const float h2 = L1*L1 - a*a;
        const float h = h2 > 0.0f ? sqrtf(h2) : 0.0f;
        // Pole: perpendicular component of the found elbow off the new axis.
        const float ed = u1[0]*ux + u1[1]*uy + u1[2]*uz;
        float p[3] = {u1[0]-ed*ux, u1[1]-ed*uy, u1[2]-ed*uz};
        const float pn = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        static float s_pole[2][3] = {};
        {   // BUILD 214: where the pole comes from, and it may never invert.
            uint32_t pb = 0;
            memcpy(&pb, &pn, sizeof(pb));
            g_pole_pn_bits[s].store(pb, std::memory_order_relaxed);
        }
        const bool poleon = g_arm_pole.load(std::memory_order_relaxed) != 0;
        float pmin = g_arm_pole_min.load(std::memory_order_relaxed);
        if (!isfinite(pmin) || pmin < 0.02f || pmin > 0.20f) pmin = 0.06f;
        if (!poleon) pmin = 0.02f;      // build 213 exactly
        // Anatomy first, because the guard below is measured against it: the
        // elbow hangs BELOW the shoulder-to-hand axis, so the seed is model
        // down projected perpendicular to that axis. Independent of the
        // animation, so it cannot flip when the animated arm crosses the axis.
        float anat[3] = {0.0f, 0.0f, 0.0f};
        bool  anatok = false;
        if (poleon) {
            float dn[3];
            if (model_down(dn)) {
                const float dd2 = dn[0]*ux + dn[1]*uy + dn[2]*uz;
                float q2[3] = {dn[0]-dd2*ux, dn[1]-dd2*uy, dn[2]-dd2*uz};
                const float qn2 = sqrtf(q2[0]*q2[0] + q2[1]*q2[1] + q2[2]*q2[2]);
                if (qn2 > 0.15f) {      // not within about 9 deg of vertical
                    anat[0] = q2[0]/qn2; anat[1] = q2[1]/qn2; anat[2] = q2[2]/qn2;
                    anatok = true;
                }
            }
        }
        // REVIEW, third round: the threshold itself could snap the elbow, both
        // because the animated pole just above it can point anywhere on the
        // circle and because pn dithering across it chatters between sources.
        // Two guards, both the reviewer's: hysteresis on the crossing (enter
        // anatomy at pmin, leave only at 1.5*pmin), and a hemisphere check that
        // refuses an animated pole more than about 107 degrees from down, which
        // is an elbow above the line and is the inversion itself. Ordinary
        // raised elbows are well inside that and are kept.
        static bool s_inanat[2] = {false, false};
        const float need = (poleon && s_inanat[s]) ? pmin * 1.5f : pmin;
        bool useanat = poleon && anatok && pn <= need;
        float cand[3] = {0.0f, 0.0f, 0.0f};
        bool  candok = false;
        if (!useanat && pn > need) {
            cand[0] = p[0]/pn; cand[1] = p[1]/pn; cand[2] = p[2]/pn;
            candok = true;
            if (poleon && anatok &&
                cand[0]*anat[0] + cand[1]*anat[1] + cand[2]*anat[2] < -0.3f) {
                useanat = true; candok = false;
                g_pole_rej[s].fetch_add(1, std::memory_order_relaxed);
            }
        }
        float used[3] = {0.0f, 0.0f, 0.0f};
        bool  haveused = false;
        if (useanat && anatok) {
            used[0] = anat[0]; used[1] = anat[1]; used[2] = anat[2];
            haveused = true;
            g_pole_anat[s].fetch_add(1, std::memory_order_relaxed);
        } else if (candok) {
            used[0] = cand[0]; used[1] = cand[1]; used[2] = cand[2];
            haveused = true;
            g_pole_anim[s].fetch_add(1, std::memory_order_relaxed);
        }
        s_inanat[s] = useanat && anatok;
        if (!haveused) {
            // Neither: hold the last one, which is build 213's only branch here.
            used[0] = s_pole[s][0]; used[1] = s_pole[s][1]; used[2] = s_pole[s][2];
            g_pole_held[s].fetch_add(1, std::memory_order_relaxed);
        }
        // BUILD 285: the pole may not jump. 284 run (grwxr-30596): the left pole reversed side more than 90 deg
        // in 3073 of 31065 calls (right 11 of 31195), and he sees the elbow rotate and invert two-handed and
        // while moving; the stock run animation moves the animated elbow and the pole follows it. The animated
        // pole stays the SOURCE (the anatomical fallback is his ruling 2026-09-08, refused); what changes is the
        // path to it: a low-pass on the direction, a turn-rate cap, both scaled by how far the animated elbow
        // lies off the shoulder-to-hand axis (the perpendicular pn is the confidence: near the axis the
        // direction is noise). Measured, not tuned: pole_raw and pole_out are the mean turn per call.
        if (haveused) {
            static float    s_raw[2][3] = {};
            static LONGLONG s_pt[2] = {};
            LARGE_INTEGER qn, qf;
            QueryPerformanceCounter(&qn);
            QueryPerformanceFrequency(&qf);
            const double gap = s_pt[s] ? (double)(qn.QuadPart - s_pt[s]) / (double)qf.QuadPart : 1.0e9;
            s_pt[s] = qn.QuadPart;
            // the previous poles, brought into the plane perpendicular to the new axis
            float pr[3], po[3];
            bool  prok = false, pook = false;
            {
                const float dr = s_raw[s][0]*ux + s_raw[s][1]*uy + s_raw[s][2]*uz;
                pr[0] = s_raw[s][0]-dr*ux; pr[1] = s_raw[s][1]-dr*uy; pr[2] = s_raw[s][2]-dr*uz;
                const float n = sqrtf(pr[0]*pr[0] + pr[1]*pr[1] + pr[2]*pr[2]);
                if (n > 0.2f) { pr[0] /= n; pr[1] /= n; pr[2] /= n; prok = true; }
                const float dq = s_pole[s][0]*ux + s_pole[s][1]*uy + s_pole[s][2]*uz;
                po[0] = s_pole[s][0]-dq*ux; po[1] = s_pole[s][1]-dq*uy; po[2] = s_pole[s][2]-dq*uz;
                const float m = sqrtf(po[0]*po[0] + po[1]*po[1] + po[2]*po[2]);
                if (m > 0.2f) { po[0] /= m; po[1] /= m; po[2] /= m; pook = true; }
            }
            // signed angle from a to b about the axis u (both perpendicular to it)
            auto turn_of = [&](const float* a, const float* b) -> float {
                const float c = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
                const float cx = a[1]*b[2]-a[2]*b[1], cy = a[2]*b[0]-a[0]*b[2], cz = a[0]*b[1]-a[1]*b[0];
                return atan2f(cx*ux + cy*uy + cz*uz, c);
            };
            const bool recent = gap < 0.2;
            if (prok && recent) {
                g_pole_n[s].fetch_add(1, std::memory_order_relaxed);
                g_pole_raw_ud[s].fetch_add((uint64_t)(fabsf(turn_of(pr, used)) * 57.29578f * 1.0e6f),
                                           std::memory_order_relaxed);
            }
            float outp[3] = {used[0], used[1], used[2]};
            if (g_pole_smooth.load(std::memory_order_relaxed) != 0 && pook && recent) {
                const float conf = useanat ? 1.0f : fminf(1.0f, fmaxf(0.1f, pn / 0.10f));
                const float tau = g_pole_tau_ms.load(std::memory_order_relaxed) * 0.001f;
                float alpha = tau > 0.0f ? 1.0f - expf(-(float)gap / tau) : 1.0f;
                alpha *= conf;
                float want = turn_of(po, used) * alpha;
                const float cap = g_pole_rate.load(std::memory_order_relaxed) * 0.0174533f * (float)gap * conf;
                if (fabsf(want) > cap) {
                    want = want < 0.0f ? -cap : cap;
                    g_pole_clip[s].fetch_add(1, std::memory_order_relaxed);
                }
                const float cs = cosf(want), sn = sinf(want);
                // po rotated by want about u: po*cos + (u x po)*sin  (u is perpendicular to po)
                outp[0] = po[0]*cs + (uy*po[2]-uz*po[1])*sn;
                outp[1] = po[1]*cs + (uz*po[0]-ux*po[2])*sn;
                outp[2] = po[2]*cs + (ux*po[1]-uy*po[0])*sn;
                const float on = sqrtf(outp[0]*outp[0] + outp[1]*outp[1] + outp[2]*outp[2]);
                if (on > 0.5f) { outp[0] /= on; outp[1] /= on; outp[2] /= on; }
                else { outp[0] = used[0]; outp[1] = used[1]; outp[2] = used[2]; }
            }
            if (pook && recent)
                g_pole_out_ud[s].fetch_add((uint64_t)(fabsf(turn_of(po, outp)) * 57.29578f * 1.0e6f),
                                           std::memory_order_relaxed);
            s_raw[s][0] = used[0]; s_raw[s][1] = used[1]; s_raw[s][2] = used[2];
            used[0] = outp[0]; used[1] = outp[1]; used[2] = outp[2];
        }
        if (used[0]*used[0] + used[1]*used[1] + used[2]*used[2] > 0.5f) {
            const float dotp = used[0]*s_pole[s][0] + used[1]*s_pole[s][1] + used[2]*s_pole[s][2];
            const float had = s_pole[s][0]*s_pole[s][0] + s_pole[s][1]*s_pole[s][1] + s_pole[s][2]*s_pole[s][2];
            if (had > 0.5f && dotp < 0.0f) g_pole_flip[s].fetch_add(1, std::memory_order_relaxed);
            s_pole[s][0] = used[0]; s_pole[s][1] = used[1]; s_pole[s][2] = used[2];
        }
        p[0] = used[0]; p[1] = used[1]; p[2] = used[2];
        // BUILD 293: GRAVITY BIAS ON THE ELBOW. His word 2026-10-11: with the rifle two-handed the LEFT elbow is too high. The
        // 292 run (A rows, before the solve): the animated left elbow sat 7 cm above the wrist on average and up to 17 cm above
        // the shoulder line. The elbow side stays the animation's (smoothed, 285); it is leaned toward 'down' by
        // k = cfg arm_pole_down_{l,r} times the two-hand authority (g_dbg_tw, 0 one-handed or unarmed). A blend, not the refused
        // fixed anatomical pole of build 214: with k = 0 this is 285 exactly.
        {
            const float kb = (s == 0 ? g_pole_down_l : g_pole_down_r).load(std::memory_order_relaxed);
            if (kb > 0.0f && p[0]*p[0] + p[1]*p[1] + p[2]*p[2] > 0.5f) {
                // BUILD 299: the 298 log (grwxr-12792) had polebias n=0 for the whole run: the two-hand authority g_dbg_tw was almost always 0
                // (two=67 accepted against tworej=3249 on the wgun: line) even with the rifle held two-handed, so the bias never acted. The
                // support hand is now read from the controllers themselves: full weight when they are within 40 cm of each other, none
                // beyond 75 cm, linear between (a long gun held with both hands has them 30 to 50 cm apart).
                float tw = 0.0f;
                {
                    float hr[3], hl[3];
                    if (aimtrace::ctrl_pos(hr) && aimtrace::ctrl_pos_l(hl)) {
                        const float dx = hr[0]-hl[0], dy = hr[1]-hl[1], dz = hr[2]-hl[2];
                        const float dd = sqrtf(dx*dx + dy*dy + dz*dz);
                        tw = (0.75f - dd) / 0.35f;
                        if (!(tw == tw) || tw < 0.0f) tw = 0.0f;
                        if (tw > 1.0f) tw = 1.0f;
                    }
                }
                const float kk = kb * tw;
                float dn[3];
                if (kk > 0.001f && model_down(dn)) {
                    const float dd2 = dn[0]*ux + dn[1]*uy + dn[2]*uz;
                    float q2[3] = {dn[0]-dd2*ux, dn[1]-dd2*uy, dn[2]-dd2*uz};
                    const float qn2 = sqrtf(q2[0]*q2[0] + q2[1]*q2[1] + q2[2]*q2[2]);
                    if (qn2 > 0.15f) {      // not within about 9 deg of vertical (the down direction is the axis itself)
                        q2[0] /= qn2; q2[1] /= qn2; q2[2] /= qn2;
                        float bl[3] = {(1.0f-kk)*p[0] + kk*q2[0], (1.0f-kk)*p[1] + kk*q2[1], (1.0f-kk)*p[2] + kk*q2[2]};
                        const float nb = sqrtf(bl[0]*bl[0] + bl[1]*bl[1] + bl[2]*bl[2]);
                        if (nb > 0.2f) {    // a near-opposite pair would blend to nothing: leave the pole alone
                            bl[0] /= nb; bl[1] /= nb; bl[2] /= nb;
                            const float cs = p[0]*bl[0] + p[1]*bl[1] + p[2]*bl[2];
                            g_pole_bias_n[s].fetch_add(1, std::memory_order_relaxed);
                            g_pole_bias_ud[s].fetch_add((uint64_t)(acosf(cs > 1.0f ? 1.0f : cs < -1.0f ? -1.0f : cs) * 57.29578f * 1.0e6f),
                                                        std::memory_order_relaxed);
                            p[0] = bl[0]; p[1] = bl[1]; p[2] = bl[2];
                        }
                    }
                }
            }
        }
        if (p[0]*p[0] + p[1]*p[1] + p[2]*p[2] > 0.5f) {
            E2[0] = S[0] + a*ux + h*p[0];
            E2[1] = S[1] + a*uy + h*p[1];
            E2[2] = S[2] + a*uz + h*p[2];
            e2ok = true;
        }
    }

    // BUILD 176: the wrist-first solve, side 0. See g_wgun_wristfirst.
    if (s == 0 && g_wgun_wristfirst.load(std::memory_order_relaxed) &&
        g_hand_anat_n[0].load(std::memory_order_acquire) >= 4 &&
        g_hand_anat[0][2] != 0xFFFFu) {
        float mk[4];
        if (read_block(buf + (uint64_t)g_hand_anat[0][2] * 0x20,
                       mk, sizeof(mk)) &&
            isfinite(mk[0]) && isfinite(mk[1]) && isfinite(mk[2])) {
            // The hand's long axis, knuckle to wrist, continued up the arm.
            float f[3] = {hand[0]-mk[0], hand[1]-mk[1], hand[2]-mk[2]};
            const float fnm = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
            if (fnm > 0.03f) {
                f[0] /= fnm; f[1] /= fnm; f[2] /= fnm;
                // BUILD 178: bend the two-bone forearm toward the hand's
                // axis only by what exceeds the wrist limit.
                if (e2ok) {
                    float f2[3] = {E2[0]-hand[0], E2[1]-hand[1], E2[2]-hand[2]};
                    const float f2n = sqrtf(f2[0]*f2[0] + f2[1]*f2[1] + f2[2]*f2[2]);
                    if (f2n > 1e-4f) {
                        f2[0] /= f2n; f2[1] /= f2n; f2[2] /= f2n;
                        float lim = g_wgun_wrist_limit.load(std::memory_order_relaxed);
                        if (!isfinite(lim)) lim = 30.0f;
                        if (lim < 0.0f) lim = 0.0f;
                        if (lim > 180.0f) lim = 180.0f;
                        lim *= 0.017453292f;
                        float c = f2[0]*f[0] + f2[1]*f[1] + f2[2]*f[2];
                        if (c > 1.0f) c = 1.0f;
                        if (c < -1.0f) c = -1.0f;
                        const float theta = acosf(c);
                        if (theta <= lim) {
                            f[0] = f2[0]; f[1] = f2[1]; f[2] = f2[2];
                        } else {
                            // Rotate f2 toward f about their common normal
                            // by theta - lim. A degenerate normal (the two
                            // are opposite) leaves f, the straight wrist.
                            const float ax[3] = {f2[1]*f[2] - f2[2]*f[1],
                                                 f2[2]*f[0] - f2[0]*f[2],
                                                 f2[0]*f[1] - f2[1]*f[0]};
                            const float an = sqrtf(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
                            if (an > 1e-4f) {
                                const float a0 = ax[0]/an, a1 = ax[1]/an, a2 = ax[2]/an;
                                // (a x f2) is the in-plane perpendicular.
                                const float w[3] = {a1*f2[2] - a2*f2[1],
                                                    a2*f2[0] - a0*f2[2],
                                                    a0*f2[1] - a1*f2[0]};
                                auto rot = [&](float ph, float out[3]) -> bool {
                                    const float cp = cosf(ph), sp = sinf(ph);
                                    float g[3] = {f2[0]*cp + w[0]*sp,
                                                  f2[1]*cp + w[1]*sp,
                                                  f2[2]*cp + w[2]*sp};
                                    const float gn = sqrtf(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
                                    if (!(gn > 1e-4f) || !isfinite(gn)) return false;
                                    out[0] = g[0]/gn; out[1] = g[1]/gn; out[2] = g[2]/gn;
                                    return true;
                                };
                                // BUILD 179: how far the shoulder would have
                                // to travel for a given forearm direction.
                                auto shift_of = [&](const float ff[3]) -> float {
                                    const float ex = hand[0] + L2*ff[0] - S[0];
                                    const float ey = hand[1] + L2*ff[1] - S[1];
                                    const float ez = hand[2] + L2*ff[2] - S[2];
                                    return sqrtf(ex*ex + ey*ey + ez*ez) - L1;
                                };
                                float cap = g_wgun_shoulder_max.load(std::memory_order_relaxed);
                                if (!isfinite(cap) || cap < 0.0f) cap = 80.0f;
                                cap *= 0.001f;
                                const float ph_full = theta - lim;
                                float fc[3];
                                if (rot(ph_full, fc)) {
                                    if (shift_of(fc) <= cap) {
                                        f[0] = fc[0]; f[1] = fc[1]; f[2] = fc[2];
                                    } else {
                                        // Scale the correction back until
                                        // the shoulder fits; the wrist
                                        // bends past the limit instead.
                                        float lo = 0.0f, hi = ph_full;
                                        for (int it = 0; it < 7; ++it) {
                                            const float mid = 0.5f * (lo + hi);
                                            float fm[3];
                                            if (rot(mid, fm) && shift_of(fm) <= cap) lo = mid;
                                            else hi = mid;
                                        }
                                        if (lo > 0.0f && rot(lo, fc)) {
                                            f[0] = fc[0]; f[1] = fc[1]; f[2] = fc[2];
                                        } else {
                                            f[0] = f2[0]; f[1] = f2[1]; f[2] = f2[2];
                                        }
                                        grwxr_wg_wfcap = grwxr_wg_wfcap + 1;
                                    }
                                } else {
                                    // BUILD 181: no rotation could be built;
                                    // keep the two-bone forearm, never the
                                    // uncapped straight wrist.
                                    f[0] = f2[0]; f[1] = f2[1]; f[2] = f2[2];
                                    grwxr_wg_wfcap = grwxr_wg_wfcap + 1;
                                }
                            } else {
                                // BUILD 181: f2 and the hand's axis are
                                // opposite (the hand bent fully back): no
                                // plane to turn in. The 180 run read shmax
                                // 460 mm here because this case fell through
                                // to the straight wrist, uncapped. Keep the
                                // two-bone forearm.
                                f[0] = f2[0]; f[1] = f2[1]; f[2] = f2[2];
                                grwxr_wg_wfcap = grwxr_wg_wfcap + 1;
                            }
                        }
                    }
                }
                E[0] = hand[0] + L2*f[0];
                E[1] = hand[1] + L2*f[1];
                E[2] = hand[2] + L2*f[2];
                const float v[3] = {E[0]-S[0], E[1]-S[1], E[2]-S[2]};
                const float dv = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
                auto note_shift = [&](float shift_m) {
                    const float shift = shift_m * 1000.0f;
                    uint32_t ob = g_wf_shmax_bits.load(std::memory_order_relaxed);
                    for (;;) {
                        float of; memcpy(&of, &ob, sizeof(of));
                        if (!(shift > of)) break;
                        uint32_t nb; memcpy(&nb, &shift, sizeof(nb));
                        if (g_wf_shmax_bits.compare_exchange_weak(
                                ob, nb, std::memory_order_relaxed))
                            break;
                    }
                    grwxr_wg_wfsh = grwxr_wg_wfsh + 1;
                };
                if (dv > L1 && dv > 1e-4f) {
                    // Farther than the upper arm reaches: the shoulder
                    // follows the elbow by the difference.
                    const float k = L1 / dv;
                    Snew[0] = E[0] - v[0]*k;
                    Snew[1] = E[1] - v[1]*k;
                    Snew[2] = E[2] - v[2]*k;
                    note_shift(dv - L1);
                } else if (dv < dmin) {
                    // BUILD 177: the elbow landed on top of the shoulder
                    // (the hand's long axis points at it). Push the shoulder
                    // back out along the elbow's direction, or along the
                    // found upper arm when there is no direction left, so
                    // the upper arm never collapses to a point (review
                    // finding 2).
                    float n[3];
                    if (dv > 1e-4f) {
                        n[0] = v[0]/dv; n[1] = v[1]/dv; n[2] = v[2]/dv;
                    } else {
                        n[0] = u1[0]/l1; n[1] = u1[1]/l1; n[2] = u1[2]/l1;
                    }
                    Snew[0] = E[0] - n[0]*dmin;
                    Snew[1] = E[1] - n[1]*dmin;
                    Snew[2] = E[2] - n[2]*dmin;
                    note_shift(dmin - dv);
                }
                // BUILD 180: the clavicle turns so its tip meets Snew.
                {
                    float cmax = g_wgun_clav_max.load(std::memory_order_relaxed);
                    if (!isfinite(cmax) || cmax < 0.0f) cmax = 0.0f;
                    if (cmax > 90.0f) cmax = 90.0f;
                    const bool moved = (Snew[0] != S[0] || Snew[1] != S[1] ||
                                        Snew[2] != S[2]);
                    float cr[8];
                    if (cmax > 0.0f && moved &&
                        g_shoulder_node[0] != 0xFFFFu &&
                        read_block(buf + (uint64_t)g_shoulder_node[0] * 0x20,
                                   cr, sizeof(cr))) {
                        bool fin = true;
                        for (int j = 0; j < 8; ++j)
                            if (!isfinite(cr[j])) { fin = false; break; }
                        const float d0[3] = {S[0]-cr[0], S[1]-cr[1], S[2]-cr[2]};
                        const float l0 = sqrtf(d0[0]*d0[0] + d0[1]*d0[1] + d0[2]*d0[2]);
                        float d1[3] = {Snew[0]-cr[0], Snew[1]-cr[1], Snew[2]-cr[2]};
                        const float l1c = sqrtf(d1[0]*d1[0] + d1[1]*d1[1] + d1[2]*d1[2]);
                        if (fin && l0 > 0.02f && l0 < 0.5f && l1c > 1e-4f) {
                            const float n0[3] = {d0[0]/l0, d0[1]/l0, d0[2]/l0};
                            float n1[3] = {d1[0]/l1c, d1[1]/l1c, d1[2]/l1c};
                            float c = n0[0]*n1[0] + n0[1]*n1[1] + n0[2]*n1[2];
                            if (c > 1.0f) c = 1.0f;
                            if (c < -1.0f) c = -1.0f;
                            float ang = acosf(c);
                            const float lim = cmax * 0.017453292f;
                            if (ang > lim) {
                                // Turn only as far as the cap allows.
                                const float ax[3] = {n0[1]*n1[2] - n0[2]*n1[1],
                                                     n0[2]*n1[0] - n0[0]*n1[2],
                                                     n0[0]*n1[1] - n0[1]*n1[0]};
                                const float an = sqrtf(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
                                if (an > 1e-4f) {
                                    const float a0 = ax[0]/an, a1 = ax[1]/an, a2 = ax[2]/an;
                                    const float w[3] = {a1*n0[2] - a2*n0[1],
                                                        a2*n0[0] - a0*n0[2],
                                                        a0*n0[1] - a1*n0[0]};
                                    const float cp = cosf(lim), sp = sinf(lim);
                                    n1[0] = n0[0]*cp + w[0]*sp;
                                    n1[1] = n0[1]*cp + w[1]*sp;
                                    n1[2] = n0[2]*cp + w[2]*sp;
                                    ang = lim;
                                } else {
                                    n1[0] = n0[0]; n1[1] = n0[1]; n1[2] = n0[2];
                                    ang = 0.0f;
                                }
                            }
                            // The joint lands on the clavicle's sphere.
                            Snew[0] = cr[0] + n1[0]*l0;
                            Snew[1] = cr[1] + n1[1]*l0;
                            Snew[2] = cr[2] + n1[2]*l0;
                            float dqc[4];
                            if (ang > 1e-4f && arc(n0, n1, dqc)) {
                                float qn[4];
                                qmul(dqc, cr + 4, qn);
                                if (isfinite(qn[0]) && isfinite(qn[1]) &&
                                    isfinite(qn[2]) && isfinite(qn[3]))
                                    write_block(buf + (uint64_t)g_shoulder_node[0] * 0x20 + 0x10,
                                                qn, sizeof(qn));
                            }
                            const float deg = ang * 57.29578f;
                            uint32_t ob = g_clav_max_bits.load(std::memory_order_relaxed);
                            for (;;) {
                                float of; memcpy(&of, &ob, sizeof(of));
                                if (!(deg > of)) break;
                                uint32_t nb; memcpy(&nb, &deg, sizeof(nb));
                                if (g_clav_max_bits.compare_exchange_weak(
                                        ob, nb, std::memory_order_relaxed))
                                    break;
                            }
                        }
                    }
                }
                grwxr_wg_wf = grwxr_wg_wf + 1;
                wf = true;
            }
        }
    }
    if (!wf) {
        // The 162 solve. With no pole yet, stand down rather than invent
        // an elbow direction.
        if (!e2ok) return skip();
        if (d > dmax) result = 2;
        E[0] = E2[0]; E[1] = E2[1]; E[2] = E2[2];
    }

    // Segment deltas: shortest arcs old-direction to new-direction.
    const float n1o[3] = {u1[0]/l1, u1[1]/l1, u1[2]/l1};
    float v1[3] = {E[0]-Snew[0], E[1]-Snew[1], E[2]-Snew[2]};
    const float v1n = sqrtf(v1[0]*v1[0] + v1[1]*v1[1] + v1[2]*v1[2]);
    if (!(v1n > 1e-5f)) return skip();
    v1[0] /= v1n; v1[1] /= v1n; v1[2] /= v1n;
    const float n2o[3] = {u2[0]/l2now, u2[1]/l2now, u2[2]/l2now};
    float v2[3] = {hand[0]-E[0], hand[1]-E[1], hand[2]-E[2]};
    const float v2n = sqrtf(v2[0]*v2[0] + v2[1]*v2[1] + v2[2]*v2[2]);
    if (!(v2n > 1e-5f)) return skip();
    v2[0] /= v2n; v2[1] /= v2n; v2[2] /= v2n;
    float dq1[4], dq2[4];
    if (!arc(n1o, v1, dq1) || !arc(n2o, v2, dq2)) return skip();

    // BUILD 173: the wrist bend our geometry imposes, first call only (the
    // second call's found hand is our own write). u is the animated forearm
    // axis expressed in the animated hand's frame; carried by the WRITTEN
    // hand and compared with our new forearm axis it reads 0 at the
    // animated bend and grows as the solved forearm arrives at the hand
    // from a side the animation never used.
    if (buf != g_tp_wrote[s] && hfq) {
        const float fn = sqrtf(hfq[0]*hfq[0] + hfq[1]*hfq[1] +
                               hfq[2]*hfq[2] + hfq[3]*hfq[3]);
        float hw[8];
        if (fn > 1e-6f &&
            read_block(buf + (uint64_t)g_hand_node[s][0] * 0x20, hw, sizeof(hw))) {
            const float wn = sqrtf(hw[4]*hw[4] + hw[5]*hw[5] +
                                   hw[6]*hw[6] + hw[7]*hw[7]);
            if (wn > 1e-6f) {
                const float fq[4]  = {hfq[0]/fn, hfq[1]/fn, hfq[2]/fn, hfq[3]/fn};
                const float fqc[4] = {-fq[0], -fq[1], -fq[2], fq[3]};
                const float wq[4]  = {hw[4]/wn, hw[5]/wn, hw[6]/wn, hw[7]/wn};
                float u[3], dv[3];
                qrot(fqc, n2o, u);     // animated forearm axis, hand frame
                qrot(wq, u, dv);       // carried by the written hand
                float c = dv[0]*v2[0] + dv[1]*v2[1] + dv[2]*v2[2];
                if (c > 1.0f) c = 1.0f;
                if (c < -1.0f) c = -1.0f;
                const float dev = acosf(c) * 57.29578f;
                if (isfinite(dev)) {
                    if (dev < g_rw_min[s][1]) g_rw_min[s][1] = dev;
                    if (dev > g_rw_max[s][1]) g_rw_max[s][1] = dev;
                    g_rw_n[s] = g_rw_n[s] + 1;
                }
            }
        }
    }

    // BUILD 163: THE FOREARM TAKES THE ROLL (cfg wgun_twist, default 1).
    // hrot is the model-space rotation hands_apply gave this hand bone on
    // this call: 162's absolute palm on the free side, the gun's delta on
    // the riding side. Whatever part of it, beyond the segment swing dq2,
    // is a twist ABOUT THE NEW FOREARM AXIS is roll, and a real arm carries
    // roll in the forearm (radius over ulna), never in the wrist. Until now
    // the whole of it landed on the wrist joint and the skin between the
    // last helper and the hand pinched into a spike that breathed with the
    // idle cycle, because the animated forearm kept rolling under a hand
    // that no longer followed it. So each forearm-segment bone takes the
    // roll times its fraction of the way from the elbow to the wrist,
    // measured from its own found position along the found forearm: the
    // ForeArm bone at the elbow takes none, the helper nearest the wrist
    // takes nearly all, and the skin twists gradually the way the rig's
    // four helpers were built to let it. Swing-twist decomposition about
    // v2, hemisphere fixed so the angle is the short way round. 0 is 162.
    // BUILD 165: THE ROLL IS MEASURED, NOT DECOMPOSED. Build 164's probe
    // (grwxr-26048) showed why 163 made fins: the animated helpers are
    // static relative to the ForeArm (o1..o4 read 14/10/7/5 degrees on
    // every sample while the animated hand swung through 120, 173, -80,
    // 126), our helper writes DO hold (the ours line carried the ramp), and
    // the roll we had to carry is close to a HALF TURN, because the brace
    // animation holds the left palm up under the handguard and the real
    // hand holds the controller palm down. A swing-twist decomposition of a
    // rotation near 180 degrees is ill-conditioned: any 180-degree rotation
    // not perpendicular to the axis decomposes to plus or minus 180 with
    // the sign set by a hair, so the helpers flipped between +t x 180 and
    // -t x 180 from call to call. That flicker is the fins.
    //
    // So the roll is now measured as a plane angle, which is what roll is:
    // the hand's across-fist axis as the animation left it, carried by the
    // segment swing, against the same axis as our write left it, both
    // projected perpendicular to the new forearm axis, signed about it.
    // Well-conditioned everywhere a wrist can go (it fails only with the
    // fist edge-on to the forearm). Continuous across the seam by unwrapping
    // toward the previous first-call value per side, so a half turn stays
    // the same half turn from frame to frame. Computed on the first call on
    // a buffer only; the second call carries the twisted helpers rigidly
    // and must not re-measure an already-written hand as a zero.
    // BUILD 170: THE LEFT ARM ONLY. Build 169 rolled both arms and the owner
    // reported the rendered weapon no longer following the controller
    // while the root write ran on every call with the barrel exactly on the
    // ray (grwxr-13956: rots=19934, raw-vs-sdir 0.0). The right arm carries
    // the gun; nothing here may touch it until the coupling between the
    // right forearm's roll and the rendered weapon is understood. The right
    // side is byte-for-byte the 162 arm solve again.
    float phi = 0.0f;
    const bool firstcall = (buf != g_tp_wrote[s]);
    if (s == 0 && firstcall) memset(g_sv_ok, 0, sizeof(g_sv_ok));   // BUILD 172
    if (s == 0 && hrot && hax && firstcall &&
        g_wgun_twist.load(std::memory_order_relaxed) &&
        hax[0]*hax[0] + hax[1]*hax[1] + hax[2]*hax[2] > 0.25f) {
        float a[3], b[3];
        qrot(dq2,  hax, a);          // animated axis, carried by the swing
        qrot(hrot, hax, b);          // the written hand's axis
        const float da = a[0]*v2[0] + a[1]*v2[1] + a[2]*v2[2];
        const float db = b[0]*v2[0] + b[1]*v2[1] + b[2]*v2[2];
        for (int i = 0; i < 3; ++i) { a[i] -= da*v2[i]; b[i] -= db*v2[i]; }
        const float an = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
        const float bn = sqrtf(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
        if (an > 0.2f && bn > 0.2f) {
            for (int i = 0; i < 3; ++i) { a[i] /= an; b[i] /= bn; }
            const float cr[3] = {a[1]*b[2] - a[2]*b[1],
                                 a[2]*b[0] - a[0]*b[2],
                                 a[0]*b[1] - a[1]*b[0]};
            const float sn = cr[0]*v2[0] + cr[1]*v2[1] + cr[2]*v2[2];
            const float cs = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
            float d = atan2f(sn, cs);
            if (isfinite(d)) {
                static float s_roll_prev[2] = {0.0f, 0.0f};
                const float prev = s_roll_prev[s];
                while (d - prev >  3.14159265f) d -= 6.28318531f;
                while (d - prev < -3.14159265f) d += 6.28318531f;
                if (d >  3.49f) d -= 6.28318531f;   // never past 200 deg
                if (d < -3.49f) d += 6.28318531f;
                s_roll_prev[s] = d;
                phi = d;
            }
        }
    }
    if (firstcall) {
        const float deg = phi * 57.29578f;
        uint32_t bits; memcpy(&bits, &deg, sizeof(bits));
        g_twist_bits[s].store(bits, std::memory_order_relaxed);
    }

    // Carry each segment's bones rigidly. Slots: 0 Arm, 3 L_01Arm, 4 O_02Arm
    // rotate about the shoulder; 1 ForeArm, 2 O_Elbow, 5..9 the forearm
    // helpers move from the found elbow to the new one, each with its share
    // of the roll on top of the swing (build 163).
    static const int kArmSeg[3]  = {0, 3, 4};
    static const int kForeSeg[7] = {1, 2, 5, 6, 7, 8, 9};
    bool wrote = false;
    bool twisted = false;
    bool kept = false;
    for (int pass = 0; pass < 2; ++pass) {
        const int*   idx = pass == 0 ? kArmSeg : kForeSeg;
        const int    cnt = pass == 0 ? 3 : 7;
        const float* piv0 = pass == 0 ? S : E0;
        const float* piv1 = pass == 0 ? Snew : E;   // BUILD 176
        const float* dq   = pass == 0 ? dq1 : dq2;
        for (int i = 0; i < cnt; ++i) {
            const uint16_t node = g_arm_node[s][idx[i]];
            if (node == 0xFFFFu) continue;
            const uint64_t rec = buf + (uint64_t)node * 0x20;
            float r[8];
            if (!read_block(rec, r, sizeof(r))) continue;
            bool fin = true;
            for (int j = 0; j < 8; ++j)
                if (!isfinite(r[j])) { fin = false; break; }
            if (!fin) continue;
            // BUILD 172: the animated state of this bone, first call only.
            if (s == 0 && firstcall) {
                memcpy(g_sv_anim[idx[i]], r, sizeof(r));
                g_sv_ok[idx[i]] |= 1;
            }
            const float rel[3] = {r[0]-piv0[0], r[1]-piv0[1], r[2]-piv0[2]};
            float dqi[4] = {dq[0], dq[1], dq[2], dq[3]};
            // BUILD 169: the upper-arm segment rolls rigidly by the arm
            // share about the new upper-arm axis v1, the shoulder's part.
            if (pass == 0 && phi != 0.0f) {
                const float ashare =
                    g_wgun_twist_arm.load(std::memory_order_relaxed);
                if (ashare > 0.0f) {
                    const float hh = 0.5f * ashare * phi;
                    const float sh = sinf(hh);
                    const float tw[4] = {v1[0]*sh, v1[1]*sh, v1[2]*sh,
                                         cosf(hh)};
                    qmul(tw, dq, dqi);
                }
            }
            // BUILD 167: the whole forearm segment rolls rigidly by the
            // share, the ForeArm bone included (see g_wgun_twist_share).
            // BUILD 171: the four helpers take share + t x (1 - share) of
            // the roll, t their measured fraction along the found forearm,
            // so the twist ramps from the ForeArm's share at the elbow to
            // the hand's whole roll at the wrist: 165's ramp on top of
            // 167's share. ForeArm, O_Elbow and the look-at take the share
            // alone. phi is non-zero on the first call on a buffer only.
            if (pass == 1 && phi != 0.0f) {
                float frac =
                    g_wgun_twist_share.load(std::memory_order_relaxed);
                if (s == 0 && i >= 2 && i <= 5) {   // O_01..O_04ForeArm
                    float t = (rel[0]*n2o[0] + rel[1]*n2o[1] +
                               rel[2]*n2o[2]) / l2now;
                    if (!(t > 0.0f)) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    uint32_t tb; memcpy(&tb, &t, sizeof(tb));
                    g_twist_t_bits[i-2].store(tb, std::memory_order_relaxed);
                    frac += t * (1.0f - frac);
                }
                if (frac > 0.0f) {
                    const float hh = 0.5f * frac * phi;
                    const float sh = sinf(hh);
                    const float tw[4] = {v2[0]*sh, v2[1]*sh, v2[2]*sh,
                                         cosf(hh)};
                    qmul(tw, dq, dqi);
                    twisted = true;
                }
            }
            // BUILD 171: on the second call on a buffer the engine's twist
            // pass has already repainted the helpers, O_Elbow and the
            // look-at from its own animation (166's keep line: 20 to 180
            // degrees from what we wrote, the ForeArm itself held at 0).
            // Carrying that repaint rigidly, as 165 to 170 did, handed the
            // render the engine's helpers every frame. So the base for the
            // second call's write is the quaternion our FIRST call put in
            // the bone this frame, carried by this call's near-identity
            // swing. Side 0, the seven forearm-segment bones, and only
            // while the feature is on, so wgun_twist = 0 is still exactly
            // 162.
            const bool reassert =
                (s == 0 && pass == 1 && !firstcall && g_tw_wq_ok[i] &&
                 g_wgun_twist.load(std::memory_order_relaxed) != 0);
            const float* qbase = reassert ? g_tw_wq[i] : (r + 4);
            if (reassert) kept = true;
            float nt[3];
            qrot(dqi, rel, nt);
            nt[0] += piv1[0]; nt[1] += piv1[1]; nt[2] += piv1[2];
            // BUILD 213: the limb, solved in the animated frame, is carried
            // to where his shoulder actually is. Both passes, so the arm and
            // the forearm move together and the chain stays rigid: the only
            // effect is a translation of the whole limb by the anchor.
            if (anchoredA) {
                nt[0] += anchA[0]; nt[1] += anchA[1]; nt[2] += anchA[2];
            }
            float nq[4];
            qmul(dqi, qbase, nq);
            if (!isfinite(nt[0]) || !isfinite(nt[1]) || !isfinite(nt[2]) ||
                !isfinite(nq[0]) || !isfinite(nq[1]) || !isfinite(nq[2]) ||
                !isfinite(nq[3]))
                continue;
            if (write_block(rec + 0x00, nt, sizeof(nt)) &&
                write_block(rec + 0x10, nq, sizeof(nq))) {
                wrote = true;
                // BUILD 172: what our second call left in the bone.
                if (s == 0 && !firstcall) {
                    memcpy(g_sv_ours[idx[i]], nt, sizeof(nt));
                    memcpy(g_sv_ours[idx[i]] + 4, nq, sizeof(nq));
                    g_sv_ok[idx[i]] |= 2;
                }
                // BUILD 166: remember the forearm-segment write, side 0,
                // for the probe's survival check on the next call.
                if (s == 0 && pass == 1) {
                    memcpy(g_tw_wq[i], nq, sizeof(nq));
                    g_tw_wq_ok[i] = 1;
                }
            }
        }
    }
    // BUILD 182: the wrist bone and its sibling ride the forearm segment.
    // They sit at the hand's end of the forearm, so with the ramp on they
    // take the hand's whole roll; with it off, the swing alone.
    for (int k = 0; k < 2; ++k) {
        const uint16_t node = g_fore_extra[s][k];
        if (node == 0xFFFFu) continue;
        const uint64_t rec = buf + (uint64_t)node * 0x20;
        float r[8];
        if (!read_block(rec, r, sizeof(r))) continue;
        bool fin = true;
        for (int j = 0; j < 8; ++j)
            if (!isfinite(r[j])) { fin = false; break; }
        if (!fin) continue;
        const float rel[3] = {r[0]-E0[0], r[1]-E0[1], r[2]-E0[2]};
        float dqi[4] = {dq2[0], dq2[1], dq2[2], dq2[3]};
        if (phi != 0.0f) {
            const float hh = 0.5f * phi;
            const float sn_ = sinf(hh);
            const float tw[4] = {v2[0]*sn_, v2[1]*sn_, v2[2]*sn_, cosf(hh)};
            qmul(tw, dq2, dqi);
        }
        const bool reassert =
            (s == 0 && !firstcall && g_tw_wq_x_ok[s][k] &&
             g_wgun_twist.load(std::memory_order_relaxed) != 0);
        const float* qbase = reassert ? g_tw_wq_x[s][k] : (r + 4);
        float nt[3];
        qrot(dqi, rel, nt);
        nt[0] += E[0]; nt[1] += E[1]; nt[2] += E[2];
        // BUILD 283: the wrist bones ride the carried limb (they were left behind by the build 213 anchor, which was off).
        if (anchoredA) {
            nt[0] += anchA[0]; nt[1] += anchA[1]; nt[2] += anchA[2];
        }
        float nq[4];
        qmul(dqi, qbase, nq);
        if (!isfinite(nt[0]) || !isfinite(nt[1]) || !isfinite(nt[2]) ||
            !isfinite(nq[0]) || !isfinite(nq[1]) || !isfinite(nq[2]) ||
            !isfinite(nq[3]))
            continue;
        if (write_block(rec + 0x00, nt, sizeof(nt)) &&
            write_block(rec + 0x10, nq, sizeof(nq))) {
            wrote = true;
            if (s == 0) {
                memcpy(g_tw_wq_x[s][k], nq, sizeof(nq));
                g_tw_wq_x_ok[s][k] = 1;
            }
        }
    }
    if (twisted && fabsf(phi) > 0.01745f)
        grwxr_wg_twist = grwxr_wg_twist + 1;
    if (kept) grwxr_wg_keep = grwxr_wg_keep + 1;   // BUILD 171
    // BUILD 172: the hand as the control, then publish this frame's pair.
    if (s == 0 && wrote) {
        if (firstcall) {
            memcpy(g_sv_anim[10], hfound, 3 * sizeof(float));
            g_sv_anim[10][3] = 0.0f;
            memcpy(g_sv_anim[10] + 4, hfq, 4 * sizeof(float));
            g_sv_ok[10] |= 1;
        } else {
            float hr[8];
            if (read_block(buf + (uint64_t)g_hand_node[0][0] * 0x20,
                           hr, sizeof(hr))) {
                memcpy(g_sv_ours[10], hr, sizeof(hr));
                g_sv_ok[10] |= 2;
            }
            g_sv_buf = buf;
            g_sv_seq.fetch_add(1, std::memory_order_release);
        }
    }
    if (wrote) g_tp_wrote[s] = buf;   // BUILD 164: the probe's ours/anim key
    return wrote ? result : 0;
}

// BUILD 172: sample the renderer's frame. Called from on_present on the
// render thread, once a second at most, reads only. Two reads must agree
// byte for byte or the sample is dropped as unstable; the hand far from our
// write marks the sample stale (a rotated buffer) and it is counted, not
// trusted.
void survive_sample() {
    static uint64_t s_last_ms = 0, s_last_seq = 0;
    const uint64_t now = GetTickCount64();
    if (now - s_last_ms < 1000) return;
    const uint64_t seq = g_sv_seq.load(std::memory_order_acquire);
    if (seq == 0 || seq == s_last_seq) return;
    if (!grwxr_wgun_skel || !g_sv_buf) return;
    s_last_ms = now;
    s_last_seq = seq;

    float anim[11][8], ours[11][8];
    int   ok[11];
    memcpy(anim, g_sv_anim, sizeof(anim));
    memcpy(ours, g_sv_ours, sizeof(ours));
    memcpy(ok,   g_sv_ok,   sizeof(ok));
    const uint64_t buf = g_sv_buf;
    if (g_sv_seq.load(std::memory_order_acquire) != seq) return;   // torn

    uint16_t nodes[11];
    for (int i = 0; i < 10; ++i) nodes[i] = g_arm_node[0][i];
    nodes[10] = g_hand_node[0][0];

    float a[11][8] = {}, b[11][8] = {};
    for (int i = 0; i < 11; ++i) {
        if (ok[i] != 3 || nodes[i] == 0xFFFFu) { ok[i] = 0; continue; }
        if (!read_block(buf + (uint64_t)nodes[i] * 0x20, a[i], sizeof(a[i])))
            ok[i] = 0;
    }
    for (int i = 0; i < 11; ++i) {
        if (!ok[i]) continue;
        if (!read_block(buf + (uint64_t)nodes[i] * 0x20, b[i], sizeof(b[i])))
            ok[i] = 0;
    }
    if (memcmp(a, b, sizeof(a)) != 0) {
        g_sv_unstable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto put = [](std::atomic<uint32_t>& dst, float v) {
        uint32_t bits; memcpy(&bits, &v, sizeof(bits));
        dst.store(bits, std::memory_order_relaxed);
    };
    auto dist_mm = [](const float* p, const float* q) {
        const float dx = p[0]-q[0], dy = p[1]-q[1], dz = p[2]-q[2];
        return sqrtf(dx*dx + dy*dy + dz*dz) * 1000.0f;
    };
    auto ang_deg = [](const float* p, const float* q) {
        const float pn = sqrtf(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]+p[3]*p[3]);
        const float qn = sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
        if (!(pn > 1e-6f) || !(qn > 1e-6f)) return -1.0f;
        float d = (p[0]*q[0]+p[1]*q[1]+p[2]*q[2]+p[3]*q[3]) / (pn*qn);
        if (d < 0.0f) d = -d;
        if (d > 1.0f) d = 1.0f;
        return 2.0f * acosf(d) * 57.29578f;
    };
    bool stale = false;
    for (int i = 0; i < 11; ++i) {
        if (!ok[i]) {
            put(g_sv_out[i*3+0], -1.0f);
            put(g_sv_out[i*3+1], -1.0f);
            put(g_sv_out[i*3+2], -1.0f);
            continue;
        }
        const float dw = dist_mm(a[i], ours[i]);
        const float dq = ang_deg(a[i] + 4, ours[i] + 4);
        const float da = dist_mm(a[i], anim[i]);
        put(g_sv_out[i*3+0], isfinite(dw) ? dw : -1.0f);
        put(g_sv_out[i*3+1], isfinite(dq) ? dq : -1.0f);
        put(g_sv_out[i*3+2], isfinite(da) ? da : -1.0f);
        if (i == 10 && dw > 50.0f) stale = true;
    }
    if (stale) g_sv_stale.fetch_add(1, std::memory_order_relaxed);
    g_sv_n.fetch_add(1, std::memory_order_relaxed);
}

// BUILD 200 and 209: the three body nodes of the armed skeleton, resolved
// once per skeleton on its rig (CRC32 name hashes).
static void sp_resolve(uint64_t skel) {
    if (g_sp_skel.load(std::memory_order_relaxed) == skel) return;
    uint64_t rig = 0;
    int h = -1, s = -1, p = -1;
    if (read_block(skel + 0x220, &rig, sizeof(rig)) && rig >= 0x10000 && !(rig & 7)) {
        h = rig_find_node(rig, 0x07C159A2u);   // CRC32("Head")
        s = rig_find_node(rig, 0x8F39FA4Eu);   // CRC32("Spine1")
        p = rig_find_node(rig, 0xDED10611u);   // CRC32("Hips")
    }
    g_sp_head.store(h, std::memory_order_relaxed);
    g_sp_spine.store(s, std::memory_order_relaxed);
    g_sp_hips.store(p, std::memory_order_relaxed);
    g_sp_skel.store(skel, std::memory_order_relaxed);
}

// BUILD 209: the character's Head bone in world, for the body follow's
// reference advance. Present thread, reads only.
bool body_head_world(float out[3]) {
    const uint64_t skel = grwxr_wgun_skel;
    if (!skel || !out) return false;
    sp_resolve(skel);
    const int h = g_sp_head.load(std::memory_order_relaxed);
    if (h < 0) return false;
    return read_bone_world(skel, (unsigned)h, out);
}

bool move_input(float* lx, float* ly, unsigned int* buttons) {
    if (lx) *lx = g_mv_lx.load(std::memory_order_relaxed);
    if (ly) *ly = g_mv_ly.load(std::memory_order_relaxed);
    if (buttons) *buttons = g_mv_btn.load(std::memory_order_relaxed);
    return true;
}

// BUILD 200: the sprint probe's sample. Present thread, reads only, no
// logging (rule 8): one sample every 100 ms into the ring.
void sprint_sample() {
    if (!g_sprint_probe.load(std::memory_order_relaxed)) return;
    static uint64_t s_last_ms = 0;
    const uint64_t now = GetTickCount64();
    if (now - s_last_ms < 100) return;
    s_last_ms = now;
    const uint64_t skel = grwxr_wgun_skel;
    if (!skel) { g_sp_noskel.fetch_add(1, std::memory_order_relaxed); return; }
    sp_resolve(skel);
    SpSample sp = {};
    sp.ms = (uint32_t)now;
    if (g_vr_cam_stamp.load(std::memory_order_acquire) != 0) {
        sp.cam[0] = g_vr_cam_pos[0]; sp.cam[1] = g_vr_cam_pos[1]; sp.cam[2] = g_vr_cam_pos[2];
        if (isfinite(sp.cam[0]) && isfinite(sp.cam[1]) && isfinite(sp.cam[2])) sp.ok |= 0x01u;
    }
    if (aimtrace::view_fwd(sp.fwd)) sp.ok |= 0x40u;
    const int h = g_sp_head.load(std::memory_order_relaxed);
    const int s = g_sp_spine.load(std::memory_order_relaxed);
    const int p = g_sp_hips.load(std::memory_order_relaxed);
    if (h >= 0 && read_bone_world(skel, (unsigned)h, sp.head))  sp.ok |= 0x02u;
    if (s >= 0 && read_bone_world(skel, (unsigned)s, sp.spine)) sp.ok |= 0x04u;
    if (p >= 0 && read_bone_world(skel, (unsigned)p, sp.hips))  sp.ok |= 0x08u;
    if (grwxr_wgun_node && read_bone_world(skel, grwxr_wgun_node, sp.gun)) sp.ok |= 0x10u;
    const uint16_t rh = g_hand_node[1][0];
    if (rh != 0xFFFFu && rh != 0 && read_bone_world(skel, rh, sp.rhand)) sp.ok |= 0x20u;
    sp.lx = g_mv_lx.load(std::memory_order_relaxed);
    sp.ly = g_mv_ly.load(std::memory_order_relaxed);
    sp.btn = g_mv_btn.load(std::memory_order_relaxed);
    const uint32_t wr = g_sp_wr.load(std::memory_order_relaxed);
    g_sp_ring[wr % kSpRing] = sp;
    g_sp_wr.store(wr + 1, std::memory_order_release);
    g_sp_samples.fetch_add(1, std::memory_order_relaxed);
}

// BUILD 200: the drain, heartbeat thread, 1 Hz: every sample since the last
// drain becomes one `sprint:` line. Horizontal means the world's x and y with
// z up (the rig's positions read that way in every log to date); the raw
// positions ride along so the assumption can be checked.
void sprint_log() {
    static uint32_t s_rd = 0;
    static bool s_legend = false;
    static SpSample s_prev = {};
    static bool s_have_prev = false;
    const uint32_t wr = g_sp_wr.load(std::memory_order_acquire);
    if (wr == s_rd) return;
    if (wr - s_rd > kSpRing) {
        g_sp_dropped.fetch_add(wr - s_rd - kSpRing, std::memory_order_relaxed);
        s_rd = wr - kSpRing;
    }
    if (!s_legend) {
        s_legend = true;
        LOG_INFO("sprint: legend (build 200, 10 Hz while armed, reads only): "
                 "cam-head is the camera the frame is composed with minus the Head "
                 "bone, split along the view forward (fwd, + = the camera AHEAD of "
                 "the head), the right of it (side) and up; gun-cam and gun-spine "
                 "the gun root minus the camera and minus Spine1 in the same axes, "
                 "metres; rhand-gun mm; vcam vhead vspine the horizontal speeds "
                 "over the previous sample, m/s; stick the merged left stick "
                 "magnitude, btn the XInput buttons (0x0040 = left stick click, "
                 "the sprint); ok bits: 1 cam 2 head 4 spine 8 hips 16 gun 32 "
                 "rhand 64 fwd. A sprint START that moves the camera against the "
                 "head shows as fwd changing while the gun rides the camera "
                 "(gun-cam steady, gun-spine changing); a gun that lags the "
                 "camera shows as gun-cam changing.");
    }
    auto hsplit = [](const float* d, const float* fwd, float out[3]) {
        // out: along the horizontal forward, along the horizontal right, up
        float fx = fwd[0], fy = fwd[1];
        const float n = sqrtf(fx * fx + fy * fy);
        if (n > 1e-6f) { fx /= n; fy /= n; } else { fx = 1.0f; fy = 0.0f; }
        out[0] = d[0] * fx + d[1] * fy;
        out[1] = d[0] * fy - d[1] * fx;
        out[2] = d[2];
    };
    auto hspeed = [](const float* a, const float* b, float dt) {
        if (!(dt > 0.0f)) return -1.0f;
        const float dx = b[0] - a[0], dy = b[1] - a[1];
        return sqrtf(dx * dx + dy * dy) / dt;
    };
    for (; s_rd != wr; ++s_rd) {
        SpSample sp = g_sp_ring[s_rd % kSpRing];
        if (g_sp_wr.load(std::memory_order_acquire) - s_rd > kSpRing) {
            g_sp_dropped.fetch_add(1, std::memory_order_relaxed);
            continue;   // overwritten while copying
        }
        float ch[3] = {0, 0, 0}, gc[3] = {0, 0, 0}, gs[3] = {0, 0, 0};
        const bool have_fwd = (sp.ok & 0x40u) != 0;
        if ((sp.ok & 0x03u) == 0x03u && have_fwd) {
            const float d[3] = {sp.cam[0] - sp.head[0], sp.cam[1] - sp.head[1], sp.cam[2] - sp.head[2]};
            hsplit(d, sp.fwd, ch);
        }
        if ((sp.ok & 0x11u) == 0x11u && have_fwd) {
            const float d[3] = {sp.gun[0] - sp.cam[0], sp.gun[1] - sp.cam[1], sp.gun[2] - sp.cam[2]};
            hsplit(d, sp.fwd, gc);
        }
        if ((sp.ok & 0x14u) == 0x14u && have_fwd) {
            const float d[3] = {sp.gun[0] - sp.spine[0], sp.gun[1] - sp.spine[1], sp.gun[2] - sp.spine[2]};
            hsplit(d, sp.fwd, gs);
        }
        float rg = -1.0f;
        if ((sp.ok & 0x30u) == 0x30u) {
            const float dx = sp.rhand[0] - sp.gun[0], dy = sp.rhand[1] - sp.gun[1], dz = sp.rhand[2] - sp.gun[2];
            rg = sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f;
        }
        float vcam = -1.0f, vhead = -1.0f, vspine = -1.0f;
        uint32_t dt = 0;
        if (s_have_prev) {
            dt = sp.ms - s_prev.ms;
            const float dts = (float)dt * 0.001f;
            if (dt > 0 && dt < 2000) {
                if ((sp.ok & s_prev.ok & 0x01u)) vcam = hspeed(s_prev.cam, sp.cam, dts);
                if ((sp.ok & s_prev.ok & 0x02u)) vhead = hspeed(s_prev.head, sp.head, dts);
                if ((sp.ok & s_prev.ok & 0x04u)) vspine = hspeed(s_prev.spine, sp.spine, dts);
            }
        }
        const float stick = sqrtf(sp.lx * sp.lx + sp.ly * sp.ly);
        LOG_INFO("sprint: t=%u dt=%u | cam-head fwd=%+.3f side=%+.3f up=%+.3f | "
                 "gun-cam fwd=%+.3f side=%+.3f up=%+.3f | gun-spine fwd=%+.3f "
                 "side=%+.3f up=%+.3f | rhand-gun=%.0fmm | vcam=%.2f vhead=%.2f "
                 "vspine=%.2f | stick=%.2f btn=0x%04X ok=0x%02X | cam=(%.2f %.2f %.2f) "
                 "head=(%.2f %.2f %.2f) spine=(%.2f %.2f %.2f) hips=(%.2f %.2f %.2f) "
                 "gun=(%.2f %.2f %.2f)",
                 sp.ms, dt, ch[0], ch[1], ch[2], gc[0], gc[1], gc[2], gs[0], gs[1], gs[2],
                 rg, vcam, vhead, vspine, stick, sp.btn, sp.ok,
                 sp.cam[0], sp.cam[1], sp.cam[2], sp.head[0], sp.head[1], sp.head[2],
                 sp.spine[0], sp.spine[1], sp.spine[2], sp.hips[0], sp.hips[1], sp.hips[2],
                 sp.gun[0], sp.gun[1], sp.gun[2]);
        s_prev = sp;
        s_have_prev = true;
    }
}

void set_sprint_probe(int on) {
    const uint32_t want = on ? 1u : 0u;
    if (g_sprint_probe.exchange(want, std::memory_order_relaxed) != want)
        LOG_INFO("sprint: probe %s (cfg sprint_probe; build 200, log only: ten samples "
                 "a second of the camera, the head, the spine, the gun root and the "
                 "right hand in world while armed).", want ? "ON" : "off");
}

void set_move_input(float lx, float ly, unsigned short buttons) {
    g_mv_lx.store(lx, std::memory_order_relaxed);
    g_mv_ly.store(ly, std::memory_order_relaxed);
    g_mv_btn.store(buttons, std::memory_order_relaxed);
}

// BUILD 201: THE CONTROLLER LEAD (item 4.13, CX-020), behind cfg wgun_lead
// (frames, default 1.0; 0 = build 200). Build 200 measured the sink: the
// composed camera falls behind the Head bone by 1.47 cm per m/s of speed
// (r = 0.93) and the gun root falls behind the camera by another 1.50 cm per
// m/s, and one frame at the run's 76 fps is 1.31 cm per m/s. So the gun is
// paired with the camera of the frame before, and the camera with the body
// of the frame before: frame pairing, not smoothing, and at a sprint's 7.5
// m/s the gun sits 22 cm closer to the spine than at rest, which is the
// weapon sinking into the body. The port measured the same class of fault
// and its first fix was "publish with one-tick lead". Here: the controllers'
// world positions (VRMirror, the build 81 route) are Cp + the head-local
// offset in the base frame's basis, where Cp is the latched composed camera;
// this adds wgun_lead times the camera's own motion over the last frame to
// Cp before the offset goes on, so the gun and the hands land where the
// camera will be on the frame that draws them. At rest the lead is zero by
// construction; a jump of more than 0.35 m in one frame (a teleport, a
// respawn, a cut) is not led. The direction of the hands and the barrel is
// untouched; only their world position moves with speed, so the shot chain
// (155) is unchanged at rest and at speed the round still leaves the muzzle
// where the gun is drawn. The probe of build 200 measures the result: with
// wgun_lead = 1 the gun-cam slope against speed should go to about zero; the
// camera's own lag behind the body stays (half the sink); wgun_lead = 2
// hides that too at the cost of the gun riding a frame ahead of the view.
std::atomic<float> g_wgun_lead{1.0f};
std::atomic<uint64_t> g_lead_applied{0};
std::atomic<uint64_t> g_lead_teleports{0};
std::atomic<uint64_t> g_lead_gaps{0};        // stamps that did not follow the previous one by exactly one
std::atomic<float> g_lead_v[3] = {};         // the last per-frame camera motion, m per frame

void apply_ctrl_lead(float pos[3]) {
    static uint64_t s_stamp = 0;
    static float s_prev[3] = {0.0f, 0.0f, 0.0f};
    static float s_v[3] = {0.0f, 0.0f, 0.0f};
    static bool s_have = false;
    const uint64_t stamp = g_vr_cam_stamp.load(std::memory_order_acquire);
    if (stamp != s_stamp) {
        const float cur[3] = {g_vr_cam_pos[0], g_vr_cam_pos[1], g_vr_cam_pos[2]};
        float v[3] = {0.0f, 0.0f, 0.0f};
        if (s_have && stamp == s_stamp + 1) {
            v[0] = cur[0] - s_prev[0]; v[1] = cur[1] - s_prev[1]; v[2] = cur[2] - s_prev[2];
            const float m2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            if (!(m2 < 0.35f * 0.35f)) {   // a teleport, a cut, or not finite: no lead this frame
                v[0] = v[1] = v[2] = 0.0f;
                g_lead_teleports.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (s_have) {
            g_lead_gaps.fetch_add(1, std::memory_order_relaxed);
        }
        s_v[0] = v[0]; s_v[1] = v[1]; s_v[2] = v[2];
        s_prev[0] = cur[0]; s_prev[1] = cur[1]; s_prev[2] = cur[2];
        s_stamp = stamp;
        s_have = true;
        g_lead_v[0].store(v[0], std::memory_order_relaxed);
        g_lead_v[1].store(v[1], std::memory_order_relaxed);
        g_lead_v[2].store(v[2], std::memory_order_relaxed);
    }
    const float k = g_wgun_lead.load(std::memory_order_relaxed);
    if (!pos || k == 0.0f || !isfinite(k)) return;
    pos[0] += k * s_v[0];
    pos[1] += k * s_v[1];
    pos[2] += k * s_v[2];
    g_lead_applied.fetch_add(1, std::memory_order_relaxed);
}

void set_wgun_lead(float k) {
    if (!isfinite(k)) k = 0.0f;
    if (k < -4.0f) k = -4.0f;
    if (k > 4.0f) k = 4.0f;
    const float prev = g_wgun_lead.exchange(k, std::memory_order_relaxed);
    static bool s_said = false;
    if (!s_said || prev != k) {
        s_said = true;
        LOG_INFO("lead: controller lead %.2f frames (cfg wgun_lead; build 201: the "
                 "hands and the gun are placed from the camera led by that many "
                 "frames of its own motion; 0 = build 200).", k);
    }
}

// BUILD 277: THE GUN PAIRING RECORDER (cfg gpair, default 0). LOG ONLY: it reads and writes nothing the game
// or the shot chain sees. The question (CX-020, the gun shaking while moving, G1 build 276 did not cure it):
// which camera is the gun placed from, how stale is it, and does the shake line up with that. Two row kinds
// on one clock (QueryPerformanceCounter): kind 1 at Present, where the hands are placed from base_frame()
// (cam = the camera position that call returned, bfk = which camera it was: 0 this frame's, 1 the previous
// frame's, 2 the live row; gun = the published right hand world position; aux = the hand's head-local offset,
// so hand motion can be taken out); kind 2 on the engine thread at each gun write (cam = the latched camera
// right now, stamp = its frame; gun = the filtered root we wrote; aux = the unfiltered target). Rule 8: a
// relaxed flag load when off; one fetch_add and plain stores into a static ring when on; no lock, no
// allocation, no I/O. The 1 Hz thread prints the rows (gpair_log).
namespace {
struct PairRow {
    std::atomic<uint64_t> ready;     // index + 1 once the row is complete
    uint64_t qpc, now, stamp;
    uint32_t kind, bfk;
    float    cam[3], gun[3], aux[3];
    uint64_t id0, id1;               // BUILD 279: the pose pointer and the bone buffer pointer the row read (kind 3 and 4)
};
constexpr uint32_t kPairRing = 1u << 14;           // 16384 rows, about 76 s of both kinds at 72 fps
PairRow g_pair[kPairRing];
std::atomic<uint64_t> g_pair_wr{0};
}  // namespace

void set_gpair(int on) {
    const int want = on ? 1 : 0;
    const int prev = g_gpair.exchange(want, std::memory_order_relaxed);
    static bool s_said = false;
    if (!s_said || prev != want) {
        s_said = true;
        LOG_INFO("gpair: pairing recorder %s (cfg gpair; build 277, log only: rows P at Present and G at each gun "
                 "write, one clock).", want ? "ON" : "off");
    }
}

void gpair_note(int kind, const float gun[3], const float aux[3], const float cam_in[3], uint64_t id0, uint64_t id1) {
    if (!g_gpair.load(std::memory_order_relaxed)) return;
    const uint64_t idx = g_pair_wr.fetch_add(1, std::memory_order_relaxed);
    PairRow& r = g_pair[idx & (kPairRing - 1)];
    r.ready.store(0, std::memory_order_relaxed);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    r.qpc = (uint64_t)q.QuadPart;
    r.now = d3d11::frame_count();
    r.stamp = g_vr_cam_stamp.load(std::memory_order_acquire);
    r.kind = (uint32_t)kind;
    r.id0 = id0;
    r.id1 = id1;
    r.bfk = kind == 1 ? (uint32_t)t_bf_kind : 255u;
    if (cam_in) { r.cam[0] = cam_in[0]; r.cam[1] = cam_in[1]; r.cam[2] = cam_in[2]; }
    else        { r.cam[0] = g_vr_cam_pos[0]; r.cam[1] = g_vr_cam_pos[1]; r.cam[2] = g_vr_cam_pos[2]; }
    for (int i = 0; i < 3; ++i) { r.gun[i] = gun ? gun[i] : 0.0f; r.aux[i] = aux ? aux[i] : 0.0f; }
    r.ready.store(idx + 1, std::memory_order_release);
}

void gpair_log() {
    static uint64_t s_rd = 0;
    static uint64_t s_q0 = 0;
    static double   s_inv = 0.0;
    const uint64_t wr = g_pair_wr.load(std::memory_order_acquire);
    if (wr == s_rd) return;
    if (wr - s_rd > kPairRing) s_rd = wr - kPairRing;            // lapped: the oldest rows are gone
    if (s_inv == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_inv = 1000.0 / (double)f.QuadPart;
        LOG_INFO("gpair: legend (build 277): 'gpair: P|G n t_ms now stamp bfk cam gun aux'. n = row index, t_ms = ms on "
                 "the QPC clock, now = Present count, stamp = frame of the latched camera, bfk 0/1/2 = this/previous/"
                 "live camera (P only). P: cam = base_frame() result, gun = right hand world, aux = hand head-local "
                 "(right, fwd, up). G: cam = latch now, gun = root written, aux = unfiltered target. BUILD 279 adds C "
                 "(camera compose, once per engine frame: gun = the Head bone world the compose READ, aux = the character "
                 "origin, cam = the camera it produced, then pose and bone-buffer pointers) and W (gun write: gun = the Head "
                 "bone world in the skeleton being written, aux = the pose root, cam = the latch, then the pointers). "
                 "BUILD 282 adds, once per skeleton at the gun write: D (gun = the predicted drawing camera, aux = this Head "
                 "bone, cam = the latch, ids = pose, latch stamp), A per side (cam = clavicle, gun = shoulder joint, aux = "
                 "elbow, ids = side 0 left 1 right, pose) and H per side (cam = animated wrist, gun = controller hand as "
                 "published, aux = that hand moved to the drawing camera, ids = side, pose); all before the write.");
    }
    for (; s_rd != wr; ++s_rd) {
        const PairRow& r = g_pair[s_rd & (kPairRing - 1)];
        if (r.ready.load(std::memory_order_acquire) != s_rd + 1) break;   // a writer is mid-row: next tick
        if (s_q0 == 0) s_q0 = r.qpc;
        LOG_INFO("gpair: %c %llu %.3f %llu %llu %u %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %llx %llx",
                 r.kind == 1 ? 'P' : (r.kind == 2 ? 'G' : (r.kind == 3 ? 'C' : (r.kind == 4 ? 'W' : (r.kind == 7 ? 'D' : (r.kind == 8 ? 'A' : 'H'))))), (unsigned long long)s_rd, (double)(r.qpc - s_q0) * s_inv,
                 (unsigned long long)r.now, (unsigned long long)r.stamp, r.bfk,
                 r.cam[0], r.cam[1], r.cam[2], r.gun[0], r.gun[1], r.gun[2], r.aux[0], r.aux[1], r.aux[2],
                 (unsigned long long)r.id0, (unsigned long long)r.id1);
    }
}

// BUILD 282: THE HANDS ARE PLACED FROM THE CAMERA THAT DRAWS THEM (cfg wgun_drawcam, default 0 = build 279 exactly).
// F-268, F-269 and the build 281 timeline: each engine frame starts at pass A, where the camera is composed from the
// skeleton the PREVIOUS gun write produced (that skeleton is what this frame draws); about 3 ms later the gun write puts
// the hands into the NEXT skeleton, which is drawn one frame later by a camera composed from ITS Head bone. The hands
// were placed from the latched camera, one frame older than the camera that draws them: about 72 mm at a run, and
// 0, 1 or 2 frames whenever a frame is missed (the sink and the jitter, CX-020). Here the Present publishes the camera
// each hand was placed from (note_pub_cam, the build 278 seqlock pair), and at the gun write the hand is moved by
// (the camera that will draw it) minus (the camera it was placed from), where the drawing camera is predicted as
// the latched camera plus the Head bone's move from the skeleton that latch was composed from to the skeleton being
// written now. That is not build 278: 278 bound the hands to the latch itself, one frame behind the shoulders, and
// the arms stretched. Translation only; the direction, the basis and the shot chain are untouched. On foot only (no
// shift while the latch was composed in a vehicle or without the Head bone). A non-finite or over 0.35 m correction,
// or a published pair that does not match the position being read, leaves the hand as it was, and is counted.
std::atomic<int> g_wgun_drawcam{0};
static thread_local float t_hthis[3] = {};     // BUILD 282: the Head bone in the skeleton this gun write is writing
static thread_local bool  t_hthis_ok = false;
namespace {
struct PubCam {
    std::atomic<uint32_t> seq;
    float cam[3];
    float pw[3];
};
PubCam g_pubcam[2];                                  // 0 the right hand, 1 the left
std::atomic<uint64_t> g_late_applied{0}, g_late_nohead{0}, g_late_big{0}, g_late_race{0}, g_late_nopub{0};
std::atomic<uint64_t> g_late_live{0}, g_late_split{0};   // Presents on the live camera row (not published), two-hand pairs undone
std::atomic<uint32_t> g_late_last_mm{0};             // the last correction magnitude, mm (log only)
}  // namespace

void set_wgun_drawcam(int on) {
    const int want = on ? 1 : 0;
    const int prev = g_wgun_drawcam.exchange(want, std::memory_order_relaxed);
    static bool s_said = false;
    if (!s_said || prev != want) {
        s_said = true;
        LOG_INFO("drawcam: hands placed from the camera that draws them %s (cfg wgun_drawcam; build 282: the hand is "
                 "moved by the latched camera plus the Head bone's move since it, minus the camera it was placed from, "
                 "translation only, on foot only).", want ? "ON" : "off");
    }
}

void note_pub_cam(int hand, const float cam[3], const float pw[3]) {   // Present thread, once per hand per Present
    if (hand < 0 || hand > 1) return;
    // The live row carries the per-eye offset (about half the IPD), so the step from it is not camera motion
    // (review of a0b5ae6, item 6a): leave the published pair stale, which the reader counts as a race and skips.
    if (t_bf_kind == 2) { g_late_live.fetch_add(1, std::memory_order_relaxed); return; }
    PubCam& p = g_pubcam[hand];
    const uint32_t s0 = p.seq.load(std::memory_order_relaxed);
    p.seq.store(s0 + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (int i = 0; i < 3; ++i) { p.cam[i] = cam[i]; p.pw[i] = pw[i]; }
    p.seq.store(s0 + 2, std::memory_order_release);
}

// BUILD 282: the camera that will draw this gun write's skeleton, predicted as the latched camera plus the Head bone's
// move from the skeleton the latch was composed from to the one being written (t_hthis). False when the latch was not
// composed on foot from the Head bone, the Head bone of this skeleton is unread, or the latch changed while read.
static bool drawcam_predict(float out[3]) {
    if (!t_hthis_ok) return false;
    const uint64_t st = g_vr_cam_stamp.load(std::memory_order_acquire);
    if (!st) return false;
    const bool ok = g_vr_cam_head_ok;
    float lc[3], lh[3];
    for (int i = 0; i < 3; ++i) { lc[i] = g_vr_cam_pos[i]; lh[i] = g_vr_cam_head[i]; }
    if (g_vr_cam_stamp.load(std::memory_order_acquire) != st || !ok) return false;
    for (int i = 0; i < 3; ++i) out[i] = lc[i] + (t_hthis[i] - lh[i]);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

// p in and out: the hand position just read from aimtrace::ctrl_pos / ctrl_pos_l. Engine thread, per gun write.
static bool late_shift(int hand, float p[3], bool count = true) {
    if (count && !g_wgun_drawcam.load(std::memory_order_relaxed)) return false;   // count = false: the log-only H row
    PubCam& pc = g_pubcam[hand];
    const uint32_t s1 = pc.seq.load(std::memory_order_acquire);
    float cam[3], pw[3];
    for (int i = 0; i < 3; ++i) { cam[i] = pc.cam[i]; pw[i] = pc.pw[i]; }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s1 == 0 || (s1 & 1u) || pc.seq.load(std::memory_order_relaxed) != s1) {
        if (count) g_late_nopub.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (fabsf(pw[0] - p[0]) > 1.0e-4f || fabsf(pw[1] - p[1]) > 1.0e-4f || fabsf(pw[2] - p[2]) > 1.0e-4f) {
        if (count) g_late_race.fetch_add(1, std::memory_order_relaxed);   // the position read is not the one this camera went with
        return false;
    }
    float dc[3];
    if (!drawcam_predict(dc)) { if (count) g_late_nohead.fetch_add(1, std::memory_order_relaxed); return false; }
    const float dv[3] = {dc[0] - cam[0], dc[1] - cam[1], dc[2] - cam[2]};
    const float m2 = dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2];
    if (!(m2 < 0.35f * 0.35f)) { if (count) g_late_big.fetch_add(1, std::memory_order_relaxed); return false; }   // also catches non-finite
    p[0] += dv[0]; p[1] += dv[1]; p[2] += dv[2];
    if (count) {
        g_late_applied.fetch_add(1, std::memory_order_relaxed);
        g_late_last_mm.store((uint32_t)(sqrtf(m2) * 1000.0f), std::memory_order_relaxed);
    }
    return true;
}

static bool ctrl_pos_late(float out[3], bool* shifted = nullptr) {
    if (!aimtrace::ctrl_pos(out)) return false;
    const bool sh = late_shift(0, out);
    if (shifted) *shifted = sh;
    return true;
}

static bool ctrl_pos_l_late(float out[3], bool* shifted = nullptr) {
    if (!aimtrace::ctrl_pos_l(out)) return false;
    const bool sh = late_shift(1, out);
    if (shifted) *shifted = sh;
    return true;
}

void late_log() {
    LOG_INFO("drawcam: on=%d applied=%llu nohead=%llu big=%llu race=%llu nopub=%llu live=%llu split=%llu last=%u mm (build 282; "
             "applied = hand moves made at the gun write; nohead = no on-foot Head bone pair to predict from; big = correction over 0.35 m; "
             "race = the published pair did not match the position read; live = Presents on the live camera row, not "
             "published; split = two-hand pairs where only one hand was moved, both put back)",
             g_wgun_drawcam.load(std::memory_order_relaxed),
             (unsigned long long)g_late_applied.load(std::memory_order_relaxed),
             (unsigned long long)g_late_nohead.load(std::memory_order_relaxed),
             (unsigned long long)g_late_big.load(std::memory_order_relaxed),
             (unsigned long long)g_late_race.load(std::memory_order_relaxed),
             (unsigned long long)g_late_nopub.load(std::memory_order_relaxed),
             (unsigned long long)g_late_live.load(std::memory_order_relaxed),
             (unsigned long long)g_late_split.load(std::memory_order_relaxed),
             (unsigned)g_late_last_mm.load(std::memory_order_relaxed));
}

void lead_log() {
    const float v0 = g_lead_v[0].load(std::memory_order_relaxed);
    const float v1 = g_lead_v[1].load(std::memory_order_relaxed);
    const float v2 = g_lead_v[2].load(std::memory_order_relaxed);
    const float m = sqrtf(v0 * v0 + v1 * v1 + v2 * v2);
    LOG_INFO("lead: k=%.2f applied=%llu teleports=%llu gaps=%llu v=(%+.4f %+.4f %+.4f) "
             "m/frame |v|=%.3f m/frame (about %.2f m/s at 72 fps)",
             g_wgun_lead.load(std::memory_order_relaxed),
             (unsigned long long)g_lead_applied.load(std::memory_order_relaxed),
             (unsigned long long)g_lead_teleports.load(std::memory_order_relaxed),
             (unsigned long long)g_lead_gaps.load(std::memory_order_relaxed),
             v0, v1, v2, m, m * 72.0f);
}

// BUILD 184, probe A, record side. Engine thread, after the gun-root record
// is final: the four records as we left them, the buffer they are in, and
// our gun root's world pose for probe B. Reads only; no logging (rule 8).
static void gun_survive_record(uint64_t buf, uint64_t rec, const float root[4],
                               const float rt0[4], const float wq[4]) {
    if (!g_wroll_probe.load(std::memory_order_relaxed)) return;
    int ok = 0;
    float r[8];
    if (read_block(rec, r, sizeof(r))) {
        memcpy(g_gs_ours[0], r, sizeof(r));
        ok |= 1;
        // Our gun root in world: root * T + rootT, the head-bone read's
        // own formula (the buffer is model space).
        const float v[3] = {r[0], r[1], r[2]};
        const float cx = root[1]*v[2] - root[2]*v[1];
        const float cy = root[2]*v[0] - root[0]*v[2];
        const float cz = root[0]*v[1] - root[1]*v[0];
        const float w[3] = {
            v[0] + 2.0f*(root[3]*cx + root[1]*cz - root[2]*cy) + rt0[0],
            v[1] + 2.0f*(root[3]*cy + root[2]*cx - root[0]*cz) + rt0[1],
            v[2] + 2.0f*(root[3]*cz + root[0]*cy - root[1]*cx) + rt0[2]};
        for (int i = 0; i < 3; ++i) {
            uint32_t b; memcpy(&b, &w[i], sizeof(b));
            g_wr_pos[i].store(b, std::memory_order_relaxed);
        }
        for (int i = 0; i < 4; ++i) {
            uint32_t b; memcpy(&b, &wq[i], sizeof(b));
            g_wr_q[i].store(b, std::memory_order_relaxed);
        }
    }
    for (int m = 0; m < 3; ++m) {
        const uint16_t node = g_mirror_node[m];
        if (node == 0xFFFFu) continue;
        if (read_block(buf + (uint64_t)node * 0x20, r, sizeof(r))) {
            memcpy(g_gs_ours[1 + m], r, sizeof(r));
            ok |= (2 << m);
        }
    }
    g_gs_ok  = ok;
    g_gs_buf = buf;
    g_gs_seq.fetch_add(1, std::memory_order_release);
}

// BUILD 184, probe A, Present side: the same four records read back from
// the buffer of our last write, mm and deg from what we left. Same shape and
// rules as survive_sample above: 1 Hz, reads only, torn samples dropped.
void gun_survive_sample() {
    static uint64_t s_last_ms = 0, s_last_seq = 0;
    const uint64_t now = GetTickCount64();
    if (now - s_last_ms < 1000) return;
    const uint64_t seq = g_gs_seq.load(std::memory_order_acquire);
    if (seq == 0 || seq == s_last_seq) return;
    if (!grwxr_wgun_skel || !g_gs_buf) return;
    s_last_ms = now;
    s_last_seq = seq;
    float ours[4][8];
    memcpy(ours, g_gs_ours, sizeof(ours));
    const int      ok  = g_gs_ok;
    const uint64_t buf = g_gs_buf;
    if (g_gs_seq.load(std::memory_order_acquire) != seq) return;   // torn
    const uint16_t nodes[4] = {(uint16_t)grwxr_wgun_node, g_mirror_node[0],
                               g_mirror_node[1], g_mirror_node[2]};
    auto put = [](std::atomic<uint32_t>& dst, float v) {
        uint32_t bits; memcpy(&bits, &v, sizeof(bits));
        dst.store(bits, std::memory_order_relaxed);
    };
    for (int i = 0; i < 4; ++i) {
        float a[8], a2[8];
        float dw = -1.0f, dq = -1.0f;
        // Read twice and drop a tear, as survive_sample does.
        if ((ok & (1 << i)) && nodes[i] != 0xFFFFu &&
            read_block(buf + (uint64_t)nodes[i] * 0x20, a, sizeof(a)) &&
            read_block(buf + (uint64_t)nodes[i] * 0x20, a2, sizeof(a2)) &&
            memcmp(a, a2, sizeof(a)) == 0) {
            const float dx = a[0] - ours[i][0], dy = a[1] - ours[i][1],
                        dz = a[2] - ours[i][2];
            dw = sqrtf(dx*dx + dy*dy + dz*dz) * 1000.0f;
            const float* p = a + 4;
            const float* q = ours[i] + 4;
            const float pn = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2] + p[3]*p[3]);
            const float qn = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
            if (pn > 1e-6f && qn > 1e-6f) {
                float d = (p[0]*q[0] + p[1]*q[1] + p[2]*q[2] + p[3]*q[3]) / (pn * qn);
                if (d < 0.0f) d = -d;
                if (d > 1.0f) d = 1.0f;
                dq = 2.0f * acosf(d) * 57.29578f;
            }
        }
        put(g_gs_out[i*2+0], isfinite(dw) ? dw : -1.0f);
        put(g_gs_out[i*2+1], isfinite(dq) ? dq : -1.0f);
    }
    g_gs_n.fetch_add(1, std::memory_order_relaxed);
}

// BUILD 183: copy the gun-root record onto the mirror bones. Called once per
// write, after the record is final (rotation only, or rotation and position).
// Engine thread, inside PublishAttachments: range-checked reads, SEH-guarded
// writes, no logging, no allocation (rule 8). Translation xyz and the
// quaternion only; each target's own w lane stays as the engine wrote it.
// Returns the number of records written.
static int mirror_gunroot(uint64_t buf, uint64_t rec) {
    const int mode = g_wgun_mirror.load(std::memory_order_relaxed);
    if (mode <= 0) return 0;
    float r[8];
    if (!read_block(rec, r, sizeof(r))) return 0;
    for (int j = 0; j < 8; ++j)
        if (!isfinite(r[j])) return 0;
    const int n = (mode >= 2) ? 3 : 1;
    int wrote = 0;
    for (int m = 0; m < n; ++m) {
        const uint16_t node = g_mirror_node[m];
        if (node == 0xFFFFu) continue;
        const uint64_t mrec = buf + (uint64_t)node * 0x20;
        if (mrec == rec) continue;
        if (write_block(mrec + 0x00, r, 3 * sizeof(float)) &&
            write_block(mrec + 0x10, r + 4, 4 * sizeof(float)))
            ++wrote;
    }
    return wrote;
}

// ---------------------------------------------------------------------------
// BUILD 294: THE TORSO TURNS (spec layer 3, "body yaw follows head yaw with a deadzone (~45 deg) and also turns toward where
// the hands point"). His word 2026-10-11: "head can rotate without turning the body"; his choices: a 45 degree deadzone with
// the body catching up to the middle of the zone, the chest following head and hands, body turn first (lean and roomscale
// are a later build).
//
// WHAT IT DOES. Once per skeleton update (the pose pointer changes every update; the second entry of the same frame sees
// our own write and is skipped), BEFORE any other read of the bones: the upper body (Spine, Spine1, Spine2, Neck, Neck1,
// Neck2, Head, both shoulders, arms, forearms, hands and fingers, and the gun root the hands are measured against) is
// rotated rigidly about the world-vertical axis through the HEAD bone, by the angle that takes the animated torso's yaw
// (from the shoulder line) to the yaw this function wants. Rotating about the head's own vertical axis leaves the head bone,
// the camera's anchor, exactly where it was, so the view cannot move. The arms are then solved from the turned shoulders by
// the existing arms_solve, so their elbows and reach are relative to a torso that faces the right way.
//
// THE YAW IT WANTS (world yaw, game basis, psi = atan2(x, y)). The target psi_t is the view's yaw (the engine camera, which the
// aim pump keeps on the head), plus torso_hand_w times the right controller's bearing from the head, limited to the deadzone.
// The body yaw psi_b starts at the animated torso's and holds still while the target is inside +-torso_dead of it; outside, it
// "catches up" at torso_rate until the target is back near the middle (within 6 deg), so small turns do not re-trigger it.
// The correction is limited to +-torso_max. Only in first person, on foot.
//
// [INFERRED] the buffer is a flat table of model-space bone records {translation, quaternion} (verified for the bones the arms
// code already writes); that rotating the listed bones as a rigid group is anatomically sound; that bones not listed (twist
// and corrective helpers, vest and pack bones) stay behind, which may tear the skin around the shoulders when looking down.
// ---------------------------------------------------------------------------

static void torso_dbg_put(int i, float v) {
    uint32_t b;
    memcpy(&b, &v, sizeof(b));
    g_torso_dbg[i].store(b, std::memory_order_relaxed);
}

// BUILD 300: the node set torso_turn and torso_lean both move, re-resolved when the rig changes (it was inside torso_turn)
static uint64_t tn_rig = 0;
static uint16_t tn_nd[320];
static int      tn_nn = 0, tn_ls = -1, tn_rs = -1, tn_hd = -1, tn_sp = -1;

static void torso_resolve_nodes(uint64_t rig) {
        // BUILD 297: the WHOLE subtree under Spine of GR_PCF_Skeleton_Average (file 0x0ea138a544, parsed from the game data 2026-10-12:
        // Spine, Spine1, Spine2, Fake_gunroot, both shoulder trees with scapulae, twist and elbow helpers, the wrist and hand
        // extras, all fingers, Neck, Neck1, Head: 81 bones). 294 to 296 turned only the main bones and the arms came out mangled,
        // because arms_solve carries the helper bones from where the animation had them. Names the rig lacks are skipped.
        static const uint32_t kUpper[] = {
    0x530EC1CBu, 0x8F39FA4Eu, 0x1630ABF4u, 0x826846F3u, 0x2D4660A8u, 0xEB830ADAu,
    0xEE73A194u, 0x89B93A80u, 0xEC3F5AB0u, 0x6F93F700u, 0xB675F36Cu, 0x78F4E1ACu,
    0xE1FDB016u, 0x96FA8080u, 0x0DA5584Du, 0x1E679ED0u, 0x876ECF6Au, 0xF069FFFCu,
    0xE167C14Bu, 0x911FBACFu, 0x0816EB75u, 0x7F11DBE3u, 0x19910DC9u, 0xF4A6EDBCu,
    0x6DAFBC06u, 0x1AA88C90u, 0x51EE4C69u, 0xD34048CFu, 0x4A491975u, 0x3D4E29E3u,
    0xDE15CAC7u, 0xB30C4CBFu, 0x52F4D844u, 0x256A0AB4u, 0xBECF46DBu, 0xCA57AF54u,
    0xE295263Au, 0xF91EC076u, 0x38E4FAFFu, 0x758F3DD5u, 0x02B429EFu, 0x8023796Du,
    0xB05FD12Bu, 0x07C159A2u, 0xF60647E5u, 0x6BB3F727u, 0xFAD26B1Bu, 0xB4C1852Eu,
    0xEB35514Au, 0x7257A1AAu, 0x6773CA27u, 0x2C4D0487u, 0xEE85EAE1u, 0x534F862Fu,
    0x8ED95FAAu, 0xF3AA59F2u, 0x75F94D30u, 0x9829BECFu, 0x0120EF75u, 0x7627DFE3u,
    0xD9FEB5F0u, 0xFEBAC1B3u, 0x67B39009u, 0x10B4A09Fu, 0x54635597u, 0x45445772u,
    0xDC4D06C8u, 0xAB4A365Eu, 0xCDCAE074u, 0x147BB2DFu, 0x8D72E365u, 0xFA75D3F3u,
    0xB133130Au, 0xDBF635C5u, 0x42FF647Fu, 0x35F854E9u, 0x4F870480u, 0x31E0EC7Du,
    0x607986F8u, 0x8E61A6FFu, 0x347602AEu };
        tn_nn = 0;
        auto add = [&](int idx) {
            if (idx < 0 || idx >= 0xFFFE) return;
            for (int i = 0; i < tn_nn; ++i) if (tn_nd[i] == (uint16_t)idx) return;
            if (tn_nn < 320) tn_nd[tn_nn++] = (uint16_t)idx;
        };
        for (uint32_t h : kUpper) add(rig_find_node(rig, h));
        add(rig_find_node(rig, 0x29568091u));       // Neck2, if this rig has one
        // BUILD 298: THE WHOLE SPINE RANGE. The live player rig has 312 nodes in depth-first order (rigdump-29308-1.tsv, 2026-10-12): Hips 1,
        // Spine 2, Spine1 3, Spine2 4, then 5 to 283 are all under the spine (shoulder trees at 15 and 211, Neck 57, Head 59, and 212 nodes
        // whose names the wordlist lacks: the gear, pack, helmet and face bones), and 284 to 286 are Hips' other children, 288 LeftUpLeg.
        // His word on 297: the arms are better but the backpack does not turn. Turn every node from Spine to the node before the first of
        // Hips' other children, so whatever the pack hangs on turns with the torso.
        {
            const int sp = rig_find_node(rig, 0x530EC1CBu);
            int hi = 100000;
            static const uint32_t kHipsKids[] = { 0x84E665BBu, 0x4BD68519u, 0xAAB34E4Au, 0x176183F0u, 0x757F1291u };   // 3 unknown, LeftUpLeg, RightUpLeg
            for (uint32_t h : kHipsKids) {
                const int ix = rig_find_node(rig, h);
                if (ix > sp && ix < hi) hi = ix;
            }
            if (sp > 0 && hi < 400 && hi - sp > 40) {
                for (int ix = sp; ix < hi; ++ix) add(ix);
                torso_dbg_put(5, (float)(hi - sp));     // replaced just below by the node count
            }
        }
        add((int)grwxr_wgun_node);                  // the gun root the hands are measured against
        // BUILD 296: the yaw is read from the upper-arm joints (about 35 cm apart); the clavicle roots (LeftShoulder, RightShoulder) are
        // only about 10 cm apart, which put the 294 shoulder-line test (> 10 cm) just under its threshold on every call
        tn_ls = rig_find_node(rig, 0xEB830ADAu);    // LeftArm
        tn_rs = rig_find_node(rig, 0x6BB3F727u);    // RightArm
        tn_hd = rig_find_node(rig, 0x07C159A2u);
        tn_sp = rig_find_node(rig, 0x530EC1CBu);   // BUILD 300: Spine, the lean's pivot
        tn_rig = rig;
        torso_dbg_put(5, (float)tn_nn);
}

static void torso_turn(uint64_t skel, uint64_t pose, uint64_t buf) {
    if (!g_torso_on.load(std::memory_order_relaxed)) return;
    g_torso_why[0].fetch_add(1, std::memory_order_relaxed);
    float rq[4];
    if (!read_block(pose + 0x10, rq, sizeof(rq))) return;
    // only the player's skeleton, and only the first entry of a skeleton update: the pose pointer cycles through three buffers
    // and the second entry of the same update comes within a few milliseconds, so the same pointer inside 6 ms is our own write
    if (grwxr_wgun_skel && skel != grwxr_wgun_skel) { g_torso_why[1].fetch_add(1, std::memory_order_relaxed); return; }
    static uint64_t s_last_pose = 0;
    static LONGLONG s_last_tick = 0;
    LARGE_INTEGER tnow, tfreq;
    QueryPerformanceCounter(&tnow);
    QueryPerformanceFrequency(&tfreq);
    if (pose == s_last_pose && s_last_tick &&
        (double)(tnow.QuadPart - s_last_tick) / (double)tfreq.QuadPart < 0.006) { g_torso_why[2].fetch_add(1, std::memory_order_relaxed); return; }
    if (!g_fp_anchored.load(std::memory_order_relaxed) || g_veh_on.load(std::memory_order_relaxed) != 0) {
        g_torso_skip.fetch_add(1, std::memory_order_relaxed);
        g_torso_why[3].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t rig = 0;
    if (!read_block(skel + 0x220, &rig, sizeof(rig)) || rig < 0x10000 || (rig & 7)) { g_torso_why[4].fetch_add(1, std::memory_order_relaxed); return; }
    if (rig != tn_rig) torso_resolve_nodes(rig);
    if (tn_ls < 0 || tn_rs < 0 || tn_hd < 0 || tn_nn < 6) { g_torso_skip.fetch_add(1, std::memory_order_relaxed); g_torso_why[5].fetch_add(1, std::memory_order_relaxed); return; }

    uint32_t flags = 0;
    read_block(pose + 0x8C, &flags, sizeof(flags));
    const bool world = (flags & 0x04000000u) != 0;
    const float rn = sqrtf(rq[0]*rq[0] + rq[1]*rq[1] + rq[2]*rq[2] + rq[3]*rq[3]);
    if (!(rn > 1e-6f)) return;
    const float root[4] = {rq[0]/rn, rq[1]/rn, rq[2]/rn, rq[3]/rn};
    float up[3] = {0.0f, 0.0f, 1.0f};
    if (!world) {
        float dn[3];
        if (!model_down(dn)) { g_torso_why[6].fetch_add(1, std::memory_order_relaxed); return; }
        up[0] = -dn[0]; up[1] = -dn[1]; up[2] = -dn[2];
    }
    auto qrot = [](const float q[4], const float v[3], float o[3]) {
        const float cx = q[1]*v[2] - q[2]*v[1];
        const float cy = q[2]*v[0] - q[0]*v[2];
        const float cz = q[0]*v[1] - q[1]*v[0];
        o[0] = v[0] + 2.0f*(q[3]*cx + q[1]*cz - q[2]*cy);
        o[1] = v[1] + 2.0f*(q[3]*cy + q[2]*cx - q[0]*cz);
        o[2] = v[2] + 2.0f*(q[3]*cz + q[0]*cy - q[1]*cx);
    };
    auto wrap = [](float a) {
        while (a >  3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    };

    float L[8], Rr[8], Hd[8];
    if (!read_block(buf + (uint64_t)tn_ls * 0x20, L, sizeof(L)) ||
        !read_block(buf + (uint64_t)tn_rs * 0x20, Rr, sizeof(Rr)) ||
        !read_block(buf + (uint64_t)tn_hd * 0x20, Hd, sizeof(Hd))) { g_torso_why[7].fetch_add(1, std::memory_order_relaxed); return; }
    for (int i = 0; i < 3; ++i)
        if (!isfinite(L[i]) || !isfinite(Rr[i]) || !isfinite(Hd[i])) { g_torso_why[7].fetch_add(1, std::memory_order_relaxed); return; }

    // the animated torso's yaw, from the shoulder line
    float rm[3] = {Rr[0]-L[0], Rr[1]-L[1], Rr[2]-L[2]};
    float rw[3];
    if (world) { rw[0] = rm[0]; rw[1] = rm[1]; rw[2] = rm[2]; } else qrot(root, rm, rw);
    {   // BUILD 295: what the shoulder line looks like, for the log (model-space right vector, world right vector)
        torso_dbg_put(0, rm[0]); torso_dbg_put(1, rm[1]); torso_dbg_put(2, rm[2]);
        torso_dbg_put(3, rw[0]); torso_dbg_put(4, rw[1]);
    }
    if (rw[0]*rw[0] + rw[1]*rw[1] < 0.0225f) { g_torso_why[8].fetch_add(1, std::memory_order_relaxed); return; }   // under 15 cm of horizontal shoulder width: no yaw to read
    const float psi_a = atan2f(-rw[1], rw[0]);          // forward is the right vector turned 90 deg counter-clockwise

    // the view's yaw and the right hand's bearing, world, game basis
    float vR[9], vC[3];
    if (!base_frame(vR, vC)) { g_torso_skip.fetch_add(1, std::memory_order_relaxed); g_torso_why[9].fetch_add(1, std::memory_order_relaxed); return; }
    g_torso_why[10].fetch_add(1, std::memory_order_relaxed);
    const float psi_h = atan2f(vR[3], vR[4]);
    float psi_t = psi_h, psi_hand = psi_h;
    {
        float hp[3];
        const float hw = g_torso_hand_w.load(std::memory_order_relaxed);
        if (hw > 0.0f && aimtrace::ctrl_pos(hp)) {
            const float dx = hp[0] - vC[0], dy = hp[1] - vC[1];
            if (dx*dx + dy*dy > 0.0625f) {              // the hand is at least 25 cm out from the head
                psi_hand = atan2f(dx, dy);
                float off = wrap(psi_hand - psi_h);
                if (off >  0.7853982f) off =  0.7853982f;
                if (off < -0.7853982f) off = -0.7853982f;
                psi_t = psi_h + (hw > 1.0f ? 1.0f : hw) * off;
            }
        }
    }

    // the state machine
    static float    s_psi_b = 0.0f;
    static bool     s_init = false, s_catching = false;
    static LONGLONG s_t = 0;
    LARGE_INTEGER qn, qf;
    QueryPerformanceCounter(&qn);
    QueryPerformanceFrequency(&qf);
    double dt = s_t ? (double)(qn.QuadPart - s_t) / (double)qf.QuadPart : 1.0e9;
    s_t = qn.QuadPart;
    if (!s_init || dt > 0.5) { s_psi_b = psi_a; s_init = true; s_catching = false; dt = 0.0; }
    if (dt > 0.1) dt = 0.1;
    const float dead = g_torso_dead.load(std::memory_order_relaxed) * 0.01745329f;
    const float rate = g_torso_rate.load(std::memory_order_relaxed) * 0.01745329f * (float)dt;
    const float diff = wrap(psi_t - s_psi_b);
    if (!s_catching && fabsf(diff) > dead) s_catching = true;
    if (s_catching) {
        g_torso_catch.fetch_add(1, std::memory_order_relaxed);
        if (fabsf(diff) <= rate) s_psi_b = psi_t;
        else s_psi_b = wrap(s_psi_b + (diff > 0.0f ? rate : -rate));
        if (fabsf(wrap(psi_t - s_psi_b)) < 0.1047f) s_catching = false;     // back near the middle: 6 deg
    }
    float phi = wrap(psi_a - s_psi_b);                  // rotation about +z that takes yaw psi_a to yaw psi_b
    const float pmax = g_torso_max.load(std::memory_order_relaxed) * 0.01745329f;
    if (phi >  pmax) { phi =  pmax; g_torso_clamp.fetch_add(1, std::memory_order_relaxed); }
    if (phi < -pmax) { phi = -pmax; g_torso_clamp.fetch_add(1, std::memory_order_relaxed); }
    torso_dbg_put(0, phi * 57.29578f);
    torso_dbg_put(1, psi_h * 57.29578f);
    torso_dbg_put(2, psi_a * 57.29578f);
    torso_dbg_put(3, s_psi_b * 57.29578f);
    torso_dbg_put(4, psi_hand * 57.29578f);
    if (fabsf(phi) > g_torso_phi_max) g_torso_phi_max = fabsf(phi);
    s_last_pose = pose;
    s_last_tick = tnow.QuadPart;
    if (fabsf(phi) < 0.0017f) return;                   // under 0.1 deg: nothing to write

    const float c = cosf(phi), s = sinf(phi);
    const float hs = sinf(phi * 0.5f), hc = cosf(phi * 0.5f);
    const float qR[4] = {up[0]*hs, up[1]*hs, up[2]*hs, hc};
    for (int i = 0; i < tn_nn; ++i) {
        const uint64_t rec = buf + (uint64_t)tn_nd[i] * 0x20;
        float r[8];
        if (!read_block(rec, r, sizeof(r))) continue;
        bool ok = true;
        for (int j = 0; j < 8; ++j) if (!isfinite(r[j])) ok = false;
        if (!ok) continue;
        // translation: rotate about the vertical axis through the head
        const float d[3] = {r[0]-Hd[0], r[1]-Hd[1], r[2]-Hd[2]};
        const float ud = up[0]*d[0] + up[1]*d[1] + up[2]*d[2];
        const float cr[3] = {up[1]*d[2] - up[2]*d[1], up[2]*d[0] - up[0]*d[2], up[0]*d[1] - up[1]*d[0]};
        float nr[8];
        for (int k = 0; k < 3; ++k) nr[k] = Hd[k] + d[k]*c + cr[k]*s + up[k]*ud*(1.0f - c);
        nr[3] = r[3];
        // orientation: qR * q
        nr[7] = qR[3]*r[7] - qR[0]*r[4] - qR[1]*r[5] - qR[2]*r[6];
        nr[4] = qR[3]*r[4] + qR[0]*r[7] + qR[1]*r[6] - qR[2]*r[5];
        nr[5] = qR[3]*r[5] - qR[0]*r[6] + qR[1]*r[7] + qR[2]*r[4];
        nr[6] = qR[3]*r[6] + qR[0]*r[5] - qR[1]*r[4] + qR[2]*r[7];
        // qR is a unit quaternion, so the product keeps the record's own norm (the w lane may carry a scale); no renormalising
        bool fin = true;
        for (int k = 0; k < 8; ++k) if (!isfinite(nr[k])) fin = false;
        if (!fin) continue;
        write_block(rec, nr, sizeof(nr));
    }
    g_torso_n.fetch_add(1, std::memory_order_relaxed);
    g_torso_why[11].fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// BUILD 300: THE LEAN (torso_lean). The same node range torso_turn moves (Spine to the node before Hips' other children, the gun root
// included) is rotated rigidly about the Spine bone, the pivot, so that the Head bone moves toward where the tracked head is: the
// torso hinges at the hips toward the lean instead of the view leaving the body behind. The offset it chases is the one the camera
// already adds to the viewpoint (g_view_off_world, since build 204: the head's displacement since the recentre, soft limited to
// about 0.4 m in the present thread). The head is matched exactly when the hinge alone can reach it, otherwise the upper body is slid
// the rest of the way, at most lean_slide metres. The camera does not trust the bone: it takes this write back out of what it reads
// (lean_unhinge), so the view is the animated head plus the tracked offset whatever the body does. Once per skeleton update, after
// torso_turn, in first person on foot. [INFERRED] a rigid hinge of the whole upper rig about Spine is anatomically acceptable.
// ---------------------------------------------------------------------------
static void torso_lean(uint64_t skel, uint64_t pose, uint64_t buf) {
    if (!g_lean_hinge.load(std::memory_order_relaxed)) return;
    g_lean_c[0].fetch_add(1, std::memory_order_relaxed);
    if (grwxr_wgun_skel && skel != grwxr_wgun_skel) { g_lean_c[1].fetch_add(1, std::memory_order_relaxed); return; }
    static uint64_t s_lpose = 0;
    static LONGLONG s_ltick = 0;
    static float    s_vmax = 0.0f;
    static float    s_post[3] = {};                 // the Head record as this function last left it (model space), for the second-entry test
    static bool     s_have_post = false;
    LARGE_INTEGER tnow, tfreq;
    QueryPerformanceCounter(&tnow);
    QueryPerformanceFrequency(&tfreq);
    if (pose == s_lpose && s_ltick &&
        (double)(tnow.QuadPart - s_ltick) / (double)tfreq.QuadPart < 0.006) { g_lean_c[2].fetch_add(1, std::memory_order_relaxed); return; }
    if (!g_fp_anchored.load(std::memory_order_relaxed) || g_veh_on.load(std::memory_order_relaxed) != 0) {
        g_lean_c[3].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t rig = 0;
    if (!read_block(skel + 0x220, &rig, sizeof(rig)) || rig < 0x10000 || (rig & 7)) { g_lean_c[4].fetch_add(1, std::memory_order_relaxed); return; }
    if (rig != tn_rig) torso_resolve_nodes(rig);
    if (tn_sp < 0 || tn_hd < 0 || tn_nn < 6) { g_lean_c[4].fetch_add(1, std::memory_order_relaxed); return; }

    // the offset the camera added to the viewpoint, world, published by the camera hook on the present thread
    const uint64_t st = g_view_off_stamp.load(std::memory_order_acquire);
    const uint64_t nowf = d3d11::frame_count();
    if (!st || nowf < st || nowf - st > 2) { g_lean_c[5].fetch_add(1, std::memory_order_relaxed); return; }
    const float w[3] = {g_view_off_world[0], g_view_off_world[1], g_view_off_world[2]};
    if (!isfinite(w[0]) || !isfinite(w[1]) || !isfinite(w[2])) { g_lean_c[5].fetch_add(1, std::memory_order_relaxed); return; }
    const float wl = sqrtf(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]);
    if (wl > 1.5f) { g_lean_c[5].fetch_add(1, std::memory_order_relaxed); return; }

    uint32_t flags = 0;
    read_block(pose + 0x8C, &flags, sizeof(flags));
    const bool world = (flags & 0x04000000u) != 0;
    float rq[4], rt0[4];
    if (!read_block(pose + 0x10, rq, sizeof(rq)) || !read_block(pose + 0x00, rt0, sizeof(rt0))) { g_lean_c[6].fetch_add(1, std::memory_order_relaxed); return; }
    const float rn = sqrtf(rq[0]*rq[0] + rq[1]*rq[1] + rq[2]*rq[2] + rq[3]*rq[3]);
    if (!(rn > 1e-6f) || !isfinite(rn)) { g_lean_c[6].fetch_add(1, std::memory_order_relaxed); return; }
    const float root[4] = {rq[0]/rn, rq[1]/rn, rq[2]/rn, rq[3]/rn};
    const float rooti[4] = {-root[0], -root[1], -root[2], root[3]};
    auto qrot = [](const float q[4], const float v[3], float o[3]) {
        const float cx = q[1]*v[2] - q[2]*v[1];
        const float cy = q[2]*v[0] - q[0]*v[2];
        const float cz = q[0]*v[1] - q[1]*v[0];
        o[0] = v[0] + 2.0f*(q[3]*cx + q[1]*cz - q[2]*cy);
        o[1] = v[1] + 2.0f*(q[3]*cy + q[2]*cx - q[0]*cz);
        o[2] = v[2] + 2.0f*(q[3]*cz + q[0]*cy - q[1]*cx);
    };
    float m[3];                                     // the offset in model space
    if (world) { m[0] = w[0]; m[1] = w[1]; m[2] = w[2]; } else qrot(rooti, w, m);

    float P[8], H[8];
    if (!read_block(buf + (uint64_t)tn_sp * 0x20, P, sizeof(P)) || !read_block(buf + (uint64_t)tn_hd * 0x20, H, sizeof(H))) {
        g_lean_c[6].fetch_add(1, std::memory_order_relaxed); return;
    }
    for (int i = 0; i < 3; ++i) if (!isfinite(P[i]) || !isfinite(H[i]) || !isfinite(m[i])) { g_lean_c[6].fetch_add(1, std::memory_order_relaxed); return; }
    const float r[3] = {H[0]-P[0], H[1]-P[1], H[2]-P[2]};
    const float rl = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    if (!(rl > 0.25f && rl < 1.5f)) { g_lean_c[6].fetch_add(1, std::memory_order_relaxed); return; }

    // the hinge adds to what the head's offset from the pivot is now, so it is not idempotent like the yaw: a second entry on the same
    // buffer that comes later than 6 ms must not hinge again. The Head record still holding exactly what this function left is the tell.
    if (pose == s_lpose && s_have_post) {
        const float e[3] = {H[0]-s_post[0], H[1]-s_post[1], H[2]-s_post[2]};
        if (e[0]*e[0] + e[1]*e[1] + e[2]*e[2] < 4.0e-8f) { g_lean_c[2].fetch_add(1, std::memory_order_relaxed); return; }
    }
    s_lpose = pose;
    s_ltick = tnow.QuadPart;
    s_have_post = false;
    if (wl > s_vmax) s_vmax = wl;
    lean_dbg_put(0, wl);
    lean_dbg_put(4, s_vmax);

    auto publish = [&](const float pre_m[3], const float post_m[3]) {
        float a[3], b[3];
        if (world) { memcpy(a, pre_m, sizeof(a)); memcpy(b, post_m, sizeof(b)); }
        else {
            qrot(root, pre_m, a);  a[0] += rt0[0]; a[1] += rt0[1]; a[2] += rt0[2];
            qrot(root, post_m, b); b[0] += rt0[0]; b[1] += rt0[1]; b[2] += rt0[2];
        }
        const uint32_t s0 = g_lean_pub.seq.load(std::memory_order_relaxed);
        g_lean_pub.seq.store(s0 + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < 3; ++i) { g_lean_pub.pre[i] = a[i]; g_lean_pub.post[i] = b[i]; }
        g_lean_pub.frame.store(d3d11::frame_count(), std::memory_order_relaxed);
        g_lean_pub.seq.store(s0 + 2, std::memory_order_release);
    };
    const float hpre[3] = {H[0], H[1], H[2]};
    if (wl < 0.004f) {                              // nothing to chase: published as no displacement, so the camera subtracts nothing
        g_lean_c[7].fetch_add(1, std::memory_order_relaxed);
        lean_dbg_put(1, 0.0f); lean_dbg_put(2, 0.0f); lean_dbg_put(3, 0.0f);
        publish(hpre, hpre);
        memcpy(s_post, hpre, sizeof(s_post)); s_have_post = true;
        return;
    }

    // the hinge: the smallest rotation about Spine that takes the head's offset from the pivot, r, toward r + offset
    const float rt[3] = {r[0]+m[0], r[1]+m[1], r[2]+m[2]};
    const float cr[3] = {r[1]*rt[2] - r[2]*rt[1], r[2]*rt[0] - r[0]*rt[2], r[0]*rt[1] - r[1]*rt[0]};
    const float crl = sqrtf(cr[0]*cr[0] + cr[1]*cr[1] + cr[2]*cr[2]);
    const float dt  = r[0]*rt[0] + r[1]*rt[1] + r[2]*rt[2];
    float th = crl > 1e-6f ? atan2f(crl, dt) : 0.0f;
    const float thmax = g_lean_theta_max.load(std::memory_order_relaxed) * 0.01745329f;
    if (th > thmax) { th = thmax; g_lean_c[9].fetch_add(1, std::memory_order_relaxed); }
    float k[3] = {0.0f, 0.0f, 1.0f};
    if (crl > 1e-6f) { k[0] = cr[0]/crl; k[1] = cr[1]/crl; k[2] = cr[2]/crl; } else th = 0.0f;
    const float c = cosf(th), s = sinf(th);
    const float hs = sinf(th * 0.5f), hc = cosf(th * 0.5f);
    const float qR[4] = {k[0]*hs, k[1]*hs, k[2]*hs, hc};
    auto rot = [&](const float x[3], float o[3]) {          // R (x - P) + P
        const float d[3] = {x[0]-P[0], x[1]-P[1], x[2]-P[2]};
        const float kd = k[0]*d[0] + k[1]*d[1] + k[2]*d[2];
        const float kx[3] = {k[1]*d[2] - k[2]*d[1], k[2]*d[0] - k[0]*d[2], k[0]*d[1] - k[1]*d[0]};
        for (int a = 0; a < 3; ++a) o[a] = P[a] + d[a]*c + kx[a]*s + k[a]*kd*(1.0f - c);
    };
    float hrot[3];
    rot(hpre, hrot);
    float T[3] = {hpre[0] + m[0] - hrot[0], hpre[1] + m[1] - hrot[1], hpre[2] + m[2] - hrot[2]};
    const float tl = sqrtf(T[0]*T[0] + T[1]*T[1] + T[2]*T[2]);
    const float tcap = g_lean_slide.load(std::memory_order_relaxed);
    float tused = tl;
    if (tl > tcap && tl > 1e-6f) {
        const float kk = tcap / tl;
        T[0] *= kk; T[1] *= kk; T[2] *= kk;
        tused = tcap;
        g_lean_slidecap.fetch_add(1, std::memory_order_relaxed);
    }
    for (int i = 0; i < tn_nn; ++i) {
        const uint64_t rec = buf + (uint64_t)tn_nd[i] * 0x20;
        float q[8];
        if (!read_block(rec, q, sizeof(q))) continue;
        bool ok = true;
        for (int j = 0; j < 8; ++j) if (!isfinite(q[j])) ok = false;
        if (!ok) continue;
        const float x[3] = {q[0], q[1], q[2]};
        float xr[3];
        rot(x, xr);
        float nr[8];
        nr[0] = xr[0] + T[0]; nr[1] = xr[1] + T[1]; nr[2] = xr[2] + T[2]; nr[3] = q[3];
        nr[7] = qR[3]*q[7] - qR[0]*q[4] - qR[1]*q[5] - qR[2]*q[6];
        nr[4] = qR[3]*q[4] + qR[0]*q[7] + qR[1]*q[6] - qR[2]*q[5];
        nr[5] = qR[3]*q[5] - qR[0]*q[6] + qR[1]*q[7] + qR[2]*q[4];
        nr[6] = qR[3]*q[6] + qR[0]*q[5] - qR[1]*q[4] + qR[2]*q[7];
        bool fin = true;
        for (int j = 0; j < 8; ++j) if (!isfinite(nr[j])) fin = false;
        if (!fin) continue;
        write_block(rec, nr, sizeof(nr));
    }
    const float hpost[3] = {hrot[0] + T[0], hrot[1] + T[1], hrot[2] + T[2]};
    publish(hpre, hpost);
    memcpy(s_post, hpost, sizeof(s_post)); s_have_post = true;
    const float dh[3] = {hpost[0]-hpre[0], hpost[1]-hpre[1], hpost[2]-hpre[2]};
    lean_dbg_put(1, th * 57.29578f);
    lean_dbg_put(2, sqrtf(dh[0]*dh[0] + dh[1]*dh[1] + dh[2]*dh[2]));
    lean_dbg_put(3, tused);
    g_lean_c[8].fetch_add(1, std::memory_order_relaxed);
}

static void grwxr_wgun_apply_body(void* skelp);

// BUILD 215: the rig write is timed (CX-029). The body is unchanged and keeps
// its __try blocks; a Scope cannot share a function with __try (C2712), so the
// timer sits on this wrapper, which keeps the name the assembly stub calls.
extern "C" void grwxr_wgun_apply(void* skelp) {
    perf::Scope sc(perf::kRig);
    grwxr_wgun_apply_body(skelp);
}

static void grwxr_wgun_apply_body(void* skelp) {
    const uint64_t skel = (uint64_t)skelp;

    auto bump = [](volatile uint64_t* c) {
        _InterlockedIncrement64((volatile long long*)c);
    };

    uint64_t pose = 0, buf = 0;
    if (!read_block(skel + 0x238, &pose, sizeof(pose)) ||
        pose < 0x10000 || (pose & 7)) { bump(&grwxr_wg_nopose); return; }
    if (!read_block(pose + 0x178, &buf, sizeof(buf)) ||
        buf < 0x10000 || (buf & 7)) { bump(&grwxr_wg_nobuf); return; }

    // BUILD 294: the torso turn comes first, so every read below sees the turned upper body.
    torso_turn(skel, pose, buf);
    // BUILD 300: then the lean, so every read below sees the hinged upper body (and the Head bone at the lean)
    torso_lean(skel, pose, buf);

    const uint64_t rec = buf + (uint64_t)grwxr_wgun_node * 0x20;
    // 2026-08-31: bt0 is read here, BEFORE any write, because the hands ride
    // the rigid change this call makes to the root record and the change is
    // measured found-state to left-state.
    float bq[4], rq[4], bt0[4], rt0[4];
    if (!read_block(rec + 0x00, bt0, sizeof(bt0)) ||
        !read_block(rec + 0x10, bq, sizeof(bq)) ||
        !read_block(pose + 0x00, rt0, sizeof(rt0)) ||
        !read_block(pose + 0x10, rq, sizeof(rq))) {
        bump(&grwxr_wg_norec); return;
    }
    for (int i = 0; i < 4; ++i)
        if (!isfinite(bq[i]) || !isfinite(rq[i])) { bump(&grwxr_wg_badq); return; }
    for (int i = 0; i < 3; ++i)
        if (!isfinite(bt0[i]) || !isfinite(rt0[i])) { bump(&grwxr_wg_badq); return; }

    // BUILD 282: the Head bone in THIS skeleton, for the drawing-camera prediction. Read only when it is used.
    const bool want_hb = g_wgun_drawcam.load(std::memory_order_relaxed) || g_gpair.load(std::memory_order_relaxed);
    t_hthis_ok = want_hb && read_bone_world(skel, headpose::head_node(), t_hthis);
    // BUILD 279 (log only): the Head bone in THIS skeleton's pose at the gun write, with the pointers it came from.
    if (g_gpair.load(std::memory_order_relaxed)) {
        if (t_hthis_ok) gpair_note(4, t_hthis, rt0, nullptr, pose, buf);
        // BUILD 282 (log only), once per skeleton: D = the predicted drawing camera (gun), this Head bone (aux), the
        // latch (cam); then per side (0 left, 1 right) A = the animated clavicle (cam), shoulder joint (gun) and elbow
        // (aux), and H = the animated wrist (cam), the controller hand as published (gun) and the same hand moved to
        // the drawing camera (aux, equal to gun when the move is refused). All read BEFORE this call writes anything.
        static thread_local uint64_t s_d_pose = 0;
        if (pose != s_d_pose) {
            s_d_pose = pose;
            float dc[3];
            if (drawcam_predict(dc)) gpair_note(7, dc, t_hthis, nullptr, pose, g_vr_cam_stamp.load(std::memory_order_relaxed));
            for (int sd = 0; sd < 2; ++sd) {
                float cl[3] = {}, sh[3] = {}, el[3] = {}, wr[3] = {}, hp[3] = {}, hm[3] = {};
                const bool a_ok = g_shoulder_node[sd] != 0xFFFFu && g_arm_node[sd][0] != 0xFFFFu && g_arm_node[sd][1] != 0xFFFFu &&
                                  read_bone_world(skel, g_shoulder_node[sd], cl) &&
                                  read_bone_world(skel, g_arm_node[sd][0], sh) &&
                                  read_bone_world(skel, g_arm_node[sd][1], el);
                if (a_ok) gpair_note(8, sh, el, cl, (uint64_t)sd, pose);
                const bool w_ok = g_hand_node[sd][0] != 0xFFFFu && read_bone_world(skel, g_hand_node[sd][0], wr);
                const bool h_ok = sd == 0 ? aimtrace::ctrl_pos_l(hp) : aimtrace::ctrl_pos(hp);
                if (w_ok && h_ok) {
                    for (int i = 0; i < 3; ++i) hm[i] = hp[i];
                    late_shift(sd == 0 ? 1 : 0, hm, false);   // log only: computes the move even when the key is off
                    gpair_note(9, hp, hm, wr, (uint64_t)sd, pose);
                }
            }
        }
    }

    // 2026-08-12: the controller ray is acquired BELOW, after the off-hand
    // measurement, not here. The measurement needs only the skeleton, and
    // gating it behind the ray cost a whole test session: the controllers went
    // untracked, every call bailed at noray, and the measurement never ran
    // even though the animated hands were readable the entire time.
    float ray[3];

    // The root quaternion carries a uniform scale in w (offline note), so it is
    // normalised before use exactly as read_bone_world does.
    const float rn = sqrtf(rq[0] * rq[0] + rq[1] * rq[1] +
                           rq[2] * rq[2] + rq[3] * rq[3]);
    if (!(rn > 1e-6f)) { bump(&grwxr_wg_badq); return; }
    const float rx = rq[0] / rn, ry = rq[1] / rn,
                rz = rq[2] / rn, rw = rq[3] / rn;

    auto qmul = [](const float a[4], const float b[4], float o[4]) {
        o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
        o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
        o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
        o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    };
    auto qrot = [](const float q[4], const float v[3], float o[3]) {
        const float cx = q[1]*v[2] - q[2]*v[1];
        const float cy = q[2]*v[0] - q[0]*v[2];
        const float cz = q[0]*v[1] - q[1]*v[0];
        o[0] = v[0] + 2.0f*(q[3]*cx + q[1]*cz - q[2]*cy);
        o[1] = v[1] + 2.0f*(q[3]*cy + q[2]*cx - q[0]*cz);
        o[2] = v[2] + 2.0f*(q[3]*cz + q[0]*cy - q[1]*cx);
    };

    const float root[4] = {rx, ry, rz, rw};
    float worldQ[4];
    qmul(root, bq, worldQ);
    const float wn = sqrtf(worldQ[0]*worldQ[0] + worldQ[1]*worldQ[1] +
                           worldQ[2]*worldQ[2] + worldQ[3]*worldQ[3]);
    if (!(wn > 1e-6f)) { bump(&grwxr_wg_badq); return; }
    for (int i = 0; i < 4; ++i) worldQ[i] /= wn;

    // The off-hand measurement was taken here until 2026-08-13. It is gone
    // because it could not be trusted from this site: this function is entered
    // twice per frame with no re-solve in between, so on the second entry the
    // bone read above is our own first-entry write and the measurement was
    // measuring us. It now lives on the init-thread census tick, where it can
    // run with wgun = 0 and see the animation instead.
    if (!aimtrace::ctrl_ray(ray)) { bump(&grwxr_wg_noray); return; }
    const float rl = sqrtf(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
    if (!(rl > 1e-6f)) { bump(&grwxr_wg_noray); return; }
    ray[0] /= rl; ray[1] /= rl; ray[2] /= rl;

    // Build 144: the ray BEFORE the two-hand blend, for the probe.
    for (int i = 0; i < 3; ++i) {
        uint32_t b;
        memcpy(&b, &ray[i], sizeof(b));
        g_dbg_raw[i].store(b, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // 2026-08-13: TWO-HANDED AIM. The rear hand is the pivot and says WHERE the
    // weapon is; the front hand says WHERE IT POINTS. That is how a long gun is
    // actually held, and it is what the tester asked for.
    //
    // The direction becomes the line from the rear hand to the front hand.
    // Everything downstream is untouched: this only replaces the target
    // direction fed to the same shortest-arc rotation that has been confirmed
    // working. We never rebuild the weapon's basis. Build 68 did, had to guess
    // the axis order, and put the rifle sideways in a tester's screenshot.
    //
    // THE FAILURE MODE THIS HAS TO SURVIVE: a direction between two points gets
    // noisier as the points converge, roughly as (tracking jitter / separation).
    // Bring your hands together and small tremor swings the muzzle wildly. So
    // authority fades out smoothly with separation instead of switching off at
    // a threshold: below kTwoHandLo it IS the old one-handed behaviour, above
    // kTwoHandHi the front hand has it entirely, and in between it crossfades.
    // The failure mode is therefore "the feature quietly stops", never a jump.
    //
    // The band sits well clear of real use. Measured 2026-08-13 on the animated
    // skeleton: the support hand sits 0.481 m along the barrel from the gun
    // root, 0.497 m away in total, stable to ONE MILLIMETRE over 112 samples.
    // At half a metre, 1-2 mm of tracking jitter is about a tenth of a degree.
    // -----------------------------------------------------------------------
    const bool want_two = g_wgun_twohand.load(std::memory_order_relaxed) != 0;
    // 2026-08-31: whether the hand chains ride the gun this call.
    const bool want_hands = g_wgun_hands.load(std::memory_order_relaxed) != 0;
    // Build 141: the left controller, read once for the free hand. Null when
    // untracked or switched off, which hands_apply treats as build 140.
    float flp_s[3];
    const float* flp = (want_hands &&
                        g_wgun_free.load(std::memory_order_relaxed) &&
                        ctrl_pos_l_late(flp_s) &&
                        isfinite(flp_s[0]) && isfinite(flp_s[1]) &&
                        isfinite(flp_s[2])) ? flp_s : nullptr;
    float lpos[3], rpos[3];
    bool  late_l = false, late_r = false;   // BUILD 278
    // Build 99: how much authority the front hand took this call, 0 when it
    // took none. The POSITION block below uses it so the front-hand grip fades
    // in and out on exactly the same curve the direction does, instead of on a
    // second threshold that could disagree with it.
    float two_w = 0.0f;
    bool  two_have_l = false;
    // BUILD 123: set when the front hand turned out to be the RIGHT one, which
    // is also the hand the gun root is anchored to. The handguard slide below
    // measures the front hand's distance ALONG the barrel from that same root,
    // so with both on one controller it would measure ~0, read that as a
    // -0.48 m error against kHandguardFwd, and drag the weapon half a metre
    // backwards. It stood down instead, and its own comment named the anchor
    // correction as the next build's single change. 2026-08-31: that change is
    // MADE, behind cfg wgun_two_anchor: the anchor crossfades to the rear
    // (left) controller on the same authority curve the direction uses, and
    // the slide then runs from a root that is actually at the rear of the
    // weapon. wgun_two_anchor = 0 is the exact build-123 behaviour back.
    bool  two_front_is_right = false;
    if (want_two &&
        ctrl_pos_l_late(lpos, &late_l) && ctrl_pos_late(rpos, &late_r)) {
        // BUILD 278: the hand-to-hand line feeds the barrel direction (INV-22). If only ONE hand was moved this write
        // the line would carry a camera step, so put both back exactly as the parent read them (review item 2).
        if (late_l != late_r) {
            aimtrace::ctrl_pos_l(lpos);
            aimtrace::ctrl_pos(rpos);
            g_late_split.fetch_add(1, std::memory_order_relaxed);
        }
        const float v[3] = {lpos[0] - rpos[0],
                            lpos[1] - rpos[1],
                            lpos[2] - rpos[2]};
        const float sep = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (sep > kTwoHandLo && isfinite(sep)) {
            float th[3] = {v[0]/sep, v[1]/sep, v[2]/sep};
            // Agreement gate: if the hand-to-hand line disagrees wildly with
            // the rear controller's own forward, something is wrong (a
            // tracking pop, the off hand behind the shoulder, a controller
            // located to the floor). Refuse the frame rather than swing the
            // weapon through the player.
            float agree = th[0]*ray[0] + th[1]*ray[1] + th[2]*ray[2];
            // BUILD 123: the line is rear-to-front, and which hand that makes
            // the front one is measured. See the long comment at g_wg_two_auto.
            // Only the SIGN is corrected here; the magnitude, and therefore
            // every decision the gate makes below, is untouched.
            if (g_wg_two_auto.load(std::memory_order_relaxed) && agree < 0.0f) {
                th[0] = -th[0]; th[1] = -th[1]; th[2] = -th[2];
                agree = -agree;
                two_front_is_right = true;
                grwxr_wg_twoflip = grwxr_wg_twoflip + 1;
            }
            // BUILD 120: measure every frame that reaches the gate, so the
            // threshold is chosen from his actual hold instead of argued
            // about. Relaxed atomics only, no logging, rule 8 clean.
            const int32_t am = (int32_t)(agree * 1000.0f);
            if (agree > g_wg_two_agree.load(std::memory_order_relaxed)) {
                const int32_t accw = g_wg_agree_acc_worst_milli.load(std::memory_order_relaxed);
                if (am < accw) g_wg_agree_acc_worst_milli.store(am, std::memory_order_relaxed);
                float w = (sep - kTwoHandLo) / (kTwoHandHi - kTwoHandLo);
                if (w > 1.0f) w = 1.0f;
                w = w * w * (3.0f - 2.0f * w);        // smoothstep
                // 2026-08-31b: the attach band's release edge. See the long
                // comment at g_wg_two_attach: beyond real foregrip distance
                // the off hand is holding nothing and must let go of the
                // direction, the anchor and the slide together, which scaling
                // w does, because all three ride two_w.
                const float far0 =
                    g_wg_two_attach.load(std::memory_order_relaxed);
                if (sep > far0) {
                    float r = (sep - far0) / 0.15f;
                    if (r > 1.0f) r = 1.0f;
                    r = r * r * (3.0f - 2.0f * r);    // smoothstep
                    w *= 1.0f - r;
                }
                // BUILD 145: the grip gate, last, so it composes with every
                // band above and can only remove authority. See g_wg_two_grip.
                if (g_wg_two_grip.load(std::memory_order_relaxed)) {
                    const uint32_t gb =
                        g_grip_off_bits.load(std::memory_order_relaxed);
                    float gv;
                    memcpy(&gv, &gb, sizeof(gv));
                    float gw = (gv - 0.35f) / 0.30f;
                    if (gw < 0.0f) gw = 0.0f;
                    if (gw > 1.0f) gw = 1.0f;
                    gw = gw * gw * (3.0f - 2.0f * gw);
                    if (w > 0.0f && !(w * gw > 0.0f))
                        grwxr_wg_gripveto = grwxr_wg_gripveto + 1;
                    w *= gw;
                }
                if (w > 0.0f) {
                    float b[3] = {ray[0] + w * (th[0] - ray[0]),
                                  ray[1] + w * (th[1] - ray[1]),
                                  ray[2] + w * (th[2] - ray[2])};
                    const float bn = sqrtf(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
                    if (bn > 0.5f) {                   // both are unit, ~1
                        ray[0] = b[0]/bn; ray[1] = b[1]/bn; ray[2] = b[2]/bn;
                        grwxr_wg_two = grwxr_wg_two + 1;
                        two_w      = w;                // build 99
                        two_have_l = true;
                    }
                } else {
                    grwxr_wg_far = grwxr_wg_far + 1;
                }
            } else {
                grwxr_wg_tworej = grwxr_wg_tworej + 1;
                // BUILD 120: how far off was it, really?
                g_wg_agree_n.fetch_add(1, std::memory_order_relaxed);
                g_wg_agree_sum_milli.fetch_add((uint64_t)(am + 1000),
                                               std::memory_order_relaxed);
                int32_t b = g_wg_agree_best_milli.load(std::memory_order_relaxed);
                if (am > b) g_wg_agree_best_milli.store(am, std::memory_order_relaxed);
            }
        }
    }

    // FILTER. One-pole toward the ray, with a hard per-call angular cap. At
    // ~144 calls per second a 5 degree cap is 720 deg/s, far faster than a
    // hand moves and slow enough that a tracking dropout cannot snap the gun
    // across the world in one frame.
    static float s_dir[3] = {0, 0, 0};
    static uint32_t s_gen = 0xFFFFFFFFu;
    const uint32_t gen = g_wgun_gen.load(std::memory_order_relaxed);
    if (gen != s_gen) {                       // fresh arm: start ON the ray
        s_gen = gen;
        s_dir[0] = ray[0]; s_dir[1] = ray[1]; s_dir[2] = ray[2];
    } else {
        float d = s_dir[0]*ray[0] + s_dir[1]*ray[1] + s_dir[2]*ray[2];
        if (d > 1.0f) d = 1.0f;
        if (d < -1.0f) d = -1.0f;
        const float ang = acosf(d);
        if (ang > 1e-5f) {
            const float alpha = g_wgun_smooth.load(std::memory_order_relaxed);
            const float cap =
                g_wgun_maxstep.load(std::memory_order_relaxed) * 0.01745329f;
            float step = alpha * ang;
            if (step > cap) step = cap;
            const float t = step / ang;
            s_dir[0] += t * (ray[0] - s_dir[0]);
            s_dir[1] += t * (ray[1] - s_dir[1]);
            s_dir[2] += t * (ray[2] - s_dir[2]);
            const float sn = sqrtf(s_dir[0]*s_dir[0] + s_dir[1]*s_dir[1] +
                                   s_dir[2]*s_dir[2]);
            if (!(sn > 1e-6f)) { bump(&grwxr_wg_badq); return; }
            s_dir[0] /= sn; s_dir[1] /= sn; s_dir[2] /= sn;
        }
    }

    // BUILD 84: publish the barrel direction for the aim side.
    //
    // Here, and not earlier: s_dir is the two-hand blend AND the filter AND the
    // rate limit, and the swing below sets the bone's +Y onto exactly this
    // vector. So this is the one direction in the process that is the barrel by
    // construction rather than by argument. Anything that aims off ctrl_ray
    // instead is aiming off an unfiltered input the gun does not follow.
    //
    // A plain store of three floats plus a counter bump: no lock, no clock, no
    // allocation, which is what rule 8 requires of a path entered twice per
    // frame. It is published BEFORE the swing can bail on a degenerate 180,
    // because the aim wants the target direction, not the write's success.
    aimtrace::set_barrel_dir(s_dir, true);

    // Build 153b: the same vector in the camera's own axes, published HERE so
    // it carries the basis instant with it. See AimTrace.h at
    // set_barrel_local for the bug this closes. Drawing only; the shot chain
    // above takes the world vector and is untouched by this.
    {
        float Rb[9], Cb[3];
        if (base_frame(Rb, Cb)) {
            const float bl[3] = {
                s_dir[0]*Rb[0] + s_dir[1]*Rb[1] + s_dir[2]*Rb[2],   // right
                s_dir[0]*Rb[3] + s_dir[1]*Rb[4] + s_dir[2]*Rb[5],   // forward
                s_dir[0]*Rb[6] + s_dir[1]*Rb[7] + s_dir[2]*Rb[8]};  // up
            aimtrace::set_barrel_local(bl, true);
        }
    }

    // Build 144: the published direction and the blend authority, probe side.
    for (int i = 0; i < 3; ++i) {
        uint32_t b;
        memcpy(&b, &s_dir[i], sizeof(b));
        g_dbg_sdir[i].store(b, std::memory_order_relaxed);
    }
    {
        uint32_t b;
        memcpy(&b, &two_w, sizeof(b));
        g_dbg_tw.store(b, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // BUILD 143: THE VISIBLE-BARREL PITCH TRIM. The weapon mesh's barrel sits
    // a constant pitch below the bone axis this swing aligns, so the rounds
    // (which follow s_dir, published above, UNCHANGED by this) print high of
    // the barrel the player sees. The trim rotates only the SWING TARGET:
    // positive degrees raise the bone axis, which raises the visible barrel
    // onto the shot line. It cannot split barrel from bullet further than the
    // model already does; at the converged value it closes that split.
    // -----------------------------------------------------------------------
    float aim_t[3] = {s_dir[0], s_dir[1], s_dir[2]};
    {
        const float th =
            g_wgun_pitch.load(std::memory_order_relaxed) * 0.01745329f;
        if (th != 0.0f) {
            // Axis: cross(s_dir, world up), the horizontal right-hand axis.
            float ax[3] = {s_dir[1], -s_dir[0], 0.0f};
            const float an = sqrtf(ax[0]*ax[0] + ax[1]*ax[1]);
            if (an > 0.05f) {   // near-vertical aim: no defined pitch, skip
                ax[0] /= an; ax[1] /= an;
                const float c2[3] = {ax[1]*s_dir[2] - ax[2]*s_dir[1],
                                     ax[2]*s_dir[0] - ax[0]*s_dir[2],
                                     ax[0]*s_dir[1] - ax[1]*s_dir[0]};
                const float ct = cosf(th), st = sinf(th);
                aim_t[0] = s_dir[0]*ct + c2[0]*st;
                aim_t[1] = s_dir[1]*ct + c2[1]*st;
                aim_t[2] = s_dir[2]*ct + c2[2]*st;
            }
        }
    }

    // -----------------------------------------------------------------------
    // BUILD 190: OWN THE BONE. The owner's directive, said twice: the bone the
    // animation writes to is ours, whole. Every build from 186 to 189 read the
    // orientation the animation left in the record (worldQ above), swung its
    // barrel onto the aim and then MEASURED the wrist against that swung
    // frame, so whatever a clip left about the barrel became part of the
    // reference the roll was taken from. The V2 fall-land clips roll
    // Fake_gunroot 177 deg about its own barrel (from the game data, the archive,
    // 2026-09-05), and 186 proved the record we write is what the frame
    // draws, so the number we wrote was the fault.
    //
    // Here the world orientation is built from two things the animation cannot
    // touch: the filtered, trimmed aim (aim_t, the barrel, model +Y) and the
    // controller's up (model +Z), projected perpendicular to the barrel. +Y is
    // the barrel and +Z is the up on this rig, proven from the bind pose
    // (trigger to muzzle is +0.25 m along +Y, bind roll about the barrel is
    // zero), so the basis is no longer the guess build 68 had to make. The
    // roll is filtered as an ANGLE about the barrel, between the up we carried
    // from the last call and the controller's up now, with the same alpha and
    // per-call cap the aim uses, so a tracking dropout cannot spin the gun in
    // one frame and there is no fixed reference to wind up against. Replayed
    // against the logged inputs of the 188 and 189 runs before the headset:
    // this construction agrees with the controller's own roll about its ray
    // (croll) to a median of 0.3 deg and is continuous through the 02:15:27
    // fall where the measured theta swung by 180.
    //
    // The barrel is aim_t by construction, so aim is untouched: s_dir was
    // published above and nothing here reads or writes it. The block below
    // (the swing and the measured roll, exactly as 189 left it) runs only when
    // this is off, which is the rollback: cfg wgun_own = 0, hot reload. The
    // second entry per frame reads our own first write through worldQ, which
    // this path does not use, so both entries build the same orientation.
    // -----------------------------------------------------------------------
    float desired[4];
    bool  own_done = false;
    if (g_wgun_own.load(std::memory_order_relaxed) != 0 &&
        g_wgun_roll.load(std::memory_order_relaxed) != 0) {
        const float t[3] = {aim_t[0], aim_t[1], aim_t[2]};
        // v projected onto the plane normal to the barrel, unit; false when
        // v lies within about 6 deg of the barrel and the projection is noise.
        auto perp_unit = [&t](const float v[3], float o[3]) -> bool {
            const float d = v[0]*t[0] + v[1]*t[1] + v[2]*t[2];
            o[0] = v[0] - d*t[0]; o[1] = v[1] - d*t[1]; o[2] = v[2] - d*t[2];
            const float n = sqrtf(o[0]*o[0] + o[1]*o[1] + o[2]*o[2]);
            if (!(n > 0.10f) || !isfinite(n)) return false;
            o[0] /= n; o[1] /= n; o[2] /= n;
            return true;
        };
        // The target: the controller's up, in the plane normal to the barrel.
        float cu[3] = {0.0f, 0.0f, 0.0f}, tgt[3];
        const bool cu_ok  = aimtrace::ctrl_up(cu) &&
                            isfinite(cu[0]) && isfinite(cu[1]) && isfinite(cu[2]);
        const bool tgt_ok = cu_ok && perp_unit(cu, tgt);
        // The up we wrote last call, re-projected onto the new barrel's plane.
        static float    s_oup[3] = {0.0f, 0.0f, 0.0f};
        static bool     s_oup_ok = false;
        static uint32_t s_ogen   = 0xFFFFFFFFu;
        if (gen != s_ogen) { s_ogen = gen; s_oup_ok = false; }
        float prev[3];
        const bool prev_ok = s_oup_ok && perp_unit(s_oup, prev);
        if (tgt_ok) {
            if (prev_ok) {
                // The angle from prev to tgt about t, filtered like the aim.
                const float dot = prev[0]*tgt[0] + prev[1]*tgt[1] + prev[2]*tgt[2];
                const float cr[3] = {prev[1]*tgt[2] - prev[2]*tgt[1],
                                     prev[2]*tgt[0] - prev[0]*tgt[2],
                                     prev[0]*tgt[1] - prev[1]*tgt[0]};
                const float ang = atan2f(cr[0]*t[0] + cr[1]*t[1] + cr[2]*t[2], dot);
                const float alpha = g_wgun_smooth.load(std::memory_order_relaxed);
                const float cap =
                    g_wgun_maxstep.load(std::memory_order_relaxed) * 0.01745329f;
                float step = alpha * ang;
                if (step >  cap) step =  cap;
                if (step < -cap) step = -cap;
                // Rotate prev by step about t. prev is perpendicular to t, so
                // Rodrigues reduces to prev*cos + (t x prev)*sin.
                const float cs = cosf(step), sn = sinf(step);
                const float txp[3] = {t[1]*prev[2] - t[2]*prev[1],
                                      t[2]*prev[0] - t[0]*prev[2],
                                      t[0]*prev[1] - t[1]*prev[0]};
                s_oup[0] = prev[0]*cs + txp[0]*sn;
                s_oup[1] = prev[1]*cs + txp[1]*sn;
                s_oup[2] = prev[2]*cs + txp[2]*sn;
            } else {
                s_oup[0] = tgt[0]; s_oup[1] = tgt[1]; s_oup[2] = tgt[2];
            }
            s_oup_ok = true;
        } else if (prev_ok) {
            // No usable controller up this call (untracked, or its up lies
            // along the barrel): hold the last up, re-projected. Counted.
            s_oup[0] = prev[0]; s_oup[1] = prev[1]; s_oup[2] = prev[2];
            g_ro_holds.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Nothing carried and nothing measured: seed upright from the
            // world up so the gun is level rather than unwritten. Counted.
            const float wu[3] = {0.0f, 0.0f, 1.0f};
            // Review fix 1: perp_unit writes its output before its length
            // test, so the flag must follow the result, or a failed seed
            // would leave a stale flag over a garbage vector.
            s_oup_ok = perp_unit(wu, s_oup);
            if (s_oup_ok) g_ro_horizon.fetch_add(1, std::memory_order_relaxed);
        }
        if (s_oup_ok) {
            // The trim about the barrel (wgun_roll_deg), the same sense as the
            // measured path: positive turns the gun's up past the controller's
            // up about +t.
            float up[3] = {s_oup[0], s_oup[1], s_oup[2]};
            const float trim =
                g_wgun_roll_deg.load(std::memory_order_relaxed) * 0.01745329f;
            if (trim != 0.0f) {
                const float cs = cosf(trim), sn = sinf(trim);
                const float txu[3] = {t[1]*up[2] - t[2]*up[1],
                                      t[2]*up[0] - t[0]*up[2],
                                      t[0]*up[1] - t[1]*up[0]};
                up[0] = up[0]*cs + txu[0]*sn;
                up[1] = up[1]*cs + txu[1]*sn;
                up[2] = up[2]*cs + txu[2]*sn;
            }
            // Model +X = +Y x +Z, so the frame is right-handed. Columns X = rt,
            // Y = t, Z = up; m[row][col]. Verified against qrot on 20000 random
            // frames through all four branches before this was written.
            const float rt[3] = {t[1]*up[2] - t[2]*up[1],
                                 t[2]*up[0] - t[0]*up[2],
                                 t[0]*up[1] - t[1]*up[0]};
            const float m00 = rt[0], m01 = t[0], m02 = up[0];
            const float m10 = rt[1], m11 = t[1], m12 = up[1];
            const float m20 = rt[2], m21 = t[2], m22 = up[2];
            float q[4];
            const float tr = m00 + m11 + m22;
            if (tr > 0.0f) {
                const float s = sqrtf(tr + 1.0f) * 2.0f;
                q[3] = 0.25f * s;
                q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
            } else if (m00 > m11 && m00 > m22) {
                const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
                q[3] = (m21 - m12) / s; q[0] = 0.25f * s;
                q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
            } else if (m11 > m22) {
                const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
                q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s;
                q[1] = 0.25f * s;       q[2] = (m12 + m21) / s;
            } else {
                const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
                q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s;
                q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
            }
            const float qn = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
            if (qn > 1e-6f && isfinite(qn)) {
                for (int i = 0; i < 4; ++i) desired[i] = q[i] / qn;
                own_done = true;
                grwxr_wg_roll = grwxr_wg_roll + 1;       // the roll was applied
                g_ro_n.fetch_add(1, std::memory_order_relaxed);

                // Log only from here to the end of the block: the up we wrote
                // against the horizon about the barrel, the target's, and the
                // controller's own roll about its ray (build 188's croll), so
                // the three read side by side on the rollown: line.
                auto st = [](std::atomic<uint32_t>& a, float v) {
                    uint32_t b; memcpy(&b, &v, sizeof(b));
                    a.store(b, std::memory_order_relaxed);
                };
                float ref[3];
                const float wu[3] = {0.0f, 0.0f, 1.0f};
                if (perp_unit(wu, ref)) {
                    auto about = [&t, &ref](const float v[3]) -> float {
                        const float d = ref[0]*v[0] + ref[1]*v[1] + ref[2]*v[2];
                        const float c[3] = {ref[1]*v[2] - ref[2]*v[1],
                                            ref[2]*v[0] - ref[0]*v[2],
                                            ref[0]*v[1] - ref[1]*v[0]};
                        return atan2f(c[0]*t[0] + c[1]*t[1] + c[2]*t[2], d);
                    };
                    const float rwrote = about(s_oup);
                    st(g_ro_roll, rwrote);
                    if (tgt_ok) st(g_ro_tgt, about(tgt));
                    // The wroll: line's "written roll" and its theta, so that
                    // probe keeps reading something true in this mode.
                    st(g_wr_roll, rwrote);
                    st(g_wr_theta, rwrote);
                }
                float cray[3];
                if (cu_ok && aimtrace::ctrl_ray(cray)) {
                    const float crn = sqrtf(cray[0]*cray[0] + cray[1]*cray[1] + cray[2]*cray[2]);
                    if (crn > 1e-4f && isfinite(crn)) {
                        const float r[3] = {cray[0]/crn, cray[1]/crn, cray[2]/crn};
                        const float wd = r[2];                       // world up . r
                        const float ud = cu[0]*r[0] + cu[1]*r[1] + cu[2]*r[2];
                        float wp[3]  = {-wd*r[0], -wd*r[1], 1.0f - wd*r[2]};
                        float up2[3] = {cu[0] - ud*r[0], cu[1] - ud*r[1], cu[2] - ud*r[2]};
                        const float hwn = sqrtf(wp[0]*wp[0] + wp[1]*wp[1] + wp[2]*wp[2]);
                        const float un  = sqrtf(up2[0]*up2[0] + up2[1]*up2[1] + up2[2]*up2[2]);
                        if (hwn > 0.10f && un > 0.10f) {
                            for (int i = 0; i < 3; ++i) { wp[i] /= hwn; up2[i] /= un; }
                            const float wdot = wp[0]*up2[0] + wp[1]*up2[1] + wp[2]*up2[2];
                            const float wcr[3] = {wp[1]*up2[2] - wp[2]*up2[1],
                                                  wp[2]*up2[0] - wp[0]*up2[2],
                                                  wp[0]*up2[1] - wp[1]*up2[0]};
                            st(g_ro_croll, atan2f(wcr[0]*r[0] + wcr[1]*r[1] + wcr[2]*r[2], wdot));
                        }
                    }
                }
                for (int i = 0; i < 3; ++i) { st(g_ro_up[i], up[i]); st(g_ro_t[i], t[i]); }
            }
        }
    }
    // Review fix 2: stored outside the gate, so a hot reload to wgun_own = 0
    // silences the rollown: line instead of freezing it.
    g_ro_on.store(own_done ? 1u : 0u, std::memory_order_relaxed);
    if (!own_done) {   // BUILD 190: the swing-and-measure path, exactly as 189 left it

    // Where the barrel points right now: the bone's +Y in world space.
    const float ey[3] = {0, 1, 0};
    float fwd[3];
    qrot(worldQ, ey, fwd);
    const float fn = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    if (!(fn > 1e-6f)) { bump(&grwxr_wg_badq); return; }
    fwd[0] /= fn; fwd[1] /= fn; fwd[2] /= fn;

    // Shortest arc taking fwd onto the trimmed target: q = (fwd x tgt, 1+dot),
    // normalised. The only degenerate case is an exact 180 degrees, where the
    // axis is undefined; we skip rather than pick an arbitrary one, because a
    // wrong axis there would roll the gun unpredictably and it resolves itself
    // on the next call as the filter moves the target off the antipode.
    const float c = fwd[0]*aim_t[0] + fwd[1]*aim_t[1] + fwd[2]*aim_t[2];
    if (c < -0.9999f) { bump(&grwxr_wg_badq); return; }
    float sw[4] = {fwd[1]*aim_t[2] - fwd[2]*aim_t[1],
                   fwd[2]*aim_t[0] - fwd[0]*aim_t[2],
                   fwd[0]*aim_t[1] - fwd[1]*aim_t[0],
                   1.0f + c};
    const float swn = sqrtf(sw[0]*sw[0] + sw[1]*sw[1] + sw[2]*sw[2] + sw[3]*sw[3]);
    if (!(swn > 1e-6f)) { bump(&grwxr_wg_badq); return; }
    for (int i = 0; i < 4; ++i) sw[i] /= swn;

    qmul(sw, worldQ, desired);               // swing applied in WORLD space
                                             // (BUILD 190: desired is declared
                                             // above the own-the-bone block)

    // -----------------------------------------------------------------------
    // 2026-08-13: ROLL, the Z axis. Twisting your wrist now twists the gun.
    //
    // It could not happen before and that was structural, not an oversight: the
    // swing above is the SHORTEST ARC between two directions, which by
    // definition carries no twist about the axis, and rolling your wrist does
    // not change where the controller points, so nothing downstream ever saw
    // it. Roll therefore has to be added, not extracted.
    //
    // It is added as a separate rotation about s_dir, the same filtered
    // direction the barrel was just aligned to. After the swing the gun's +Y IS
    // s_dir, and a rotation about an axis fixes that axis exactly, so ROLL
    // CANNOT DISTURB AIM. That is algebra, not tuning: no trim value, filter
    // setting or tracking dropout can make this bleed into where the gun
    // points.
    //
    // The angle is measured between the gun's up and the controller's up, both
    // projected onto the plane normal to the barrel. Left-multiplied in world
    // space, exactly like the swing, so the confirmed-working block above is
    // composed onto rather than modified.
    //
    // Which way is "up" on a weapon model is NOT known. +Y is the barrel is
    // verified; nothing states which of the remaining axes is up. So this is
    // correct up to a constant, and wgun_roll_deg is that constant, converged
    // once by eye in the headset. Halo MCC VR ships exactly the same trim for
    // exactly the same reason.
    // -----------------------------------------------------------------------
    if (g_wgun_roll.load(std::memory_order_relaxed) != 0) {
        float cu[3];
        if (aimtrace::ctrl_up(cu)) {
            // Build 143: the roll axis is the TRIMMED target, because after
            // the swing that is the bone's +Y, and a rotation about any
            // other axis would bleed into the trimmed alignment.
            const float t[3] = {aim_t[0], aim_t[1], aim_t[2]};
            // The gun's own up, after the swing: rotate model +Z by desired.
            const float ez[3] = {0.0f, 0.0f, 1.0f};
            float gu[3];
            qrot(desired, ez, gu);
            // Project both onto the plane normal to the barrel.
            const float gd = gu[0]*t[0] + gu[1]*t[1] + gu[2]*t[2];
            const float cd = cu[0]*t[0] + cu[1]*t[1] + cu[2]*t[2];
            float gp[3] = {gu[0] - gd*t[0], gu[1] - gd*t[1], gu[2] - gd*t[2]};
            float cp[3] = {cu[0] - cd*t[0], cu[1] - cd*t[1], cu[2] - cd*t[2]};
            const float gn2 = sqrtf(gp[0]*gp[0] + gp[1]*gp[1] + gp[2]*gp[2]);
            const float cn2 = sqrtf(cp[0]*cp[0] + cp[1]*cp[1] + cp[2]*cp[2]);
            // Near-parallel to the barrel the projection vanishes and the
            // angle is pure noise. Skip rather than feed the filter garbage.
            if (gn2 > 0.10f && cn2 > 0.10f) {
                gp[0] /= gn2; gp[1] /= gn2; gp[2] /= gn2;
                cp[0] /= cn2; cp[1] /= cn2; cp[2] /= cn2;
                const float dot = gp[0]*cp[0] + gp[1]*cp[1] + gp[2]*cp[2];
                const float cr[3] = {gp[1]*cp[2] - gp[2]*cp[1],
                                     gp[2]*cp[0] - gp[0]*cp[2],
                                     gp[0]*cp[1] - gp[1]*cp[0]};
                const float sgn = cr[0]*t[0] + cr[1]*t[1] + cr[2]*t[2];
                // BUILD 189 review fix 1: keep the UNTRIMMED angle. croll knows
                // nothing about wgun_roll_deg, so comparing a trimmed theta
                // against it biases the flip detector one-for-one with the trim.
                // The trim is a deliberate offset and the detector must not see
                // it: at trim 120 the latch would fire on the first frame and
                // subtract exactly the half turn the owner just dialled in, and
                // between 60 and 120 it would park in the dead band forever.
                // The trim is a live in-headset tunable with a -180..180 range,
                // so this is one click away, and the symptom is the very fault
                // this build fixes.
                const float traw = atan2f(sgn, dot);
                float theta = traw +
                              g_wgun_roll_deg.load(std::memory_order_relaxed) *
                                  0.01745329f;

                // BUILD 189: croll is no longer only a probe. It is the honest
                // reference the correction below leans on, so it is hoisted out
                // of the log block and its validity is tracked.
                float croll = 0.0f;
                bool  croll_ok = false;

                {   // BUILD 188, LOG ONLY. Capture what theta was built from,
                    // and independently measure what the WRIST actually did, so
                    // the two can be compared instead of assumed equal.
                    auto st = [](std::atomic<uint32_t>& a, float v) {
                        uint32_t b; memcpy(&b, &v, sizeof(b));
                        a.store(b, std::memory_order_relaxed);
                    };
                    st(g_ri_dot, dot);
                    st(g_ri_sgn, sgn);
                    st(g_ri_gn2, gn2);
                    st(g_ri_cn2, cn2);
                    for (int i = 0; i < 3; ++i) { st(g_ri_cu[i], cu[i]); st(g_ri_t[i], t[i]); }
                    g_ri_n.fetch_add(1, std::memory_order_relaxed);

                    // The controller's roll about its OWN ray, against the world
                    // horizon. Independent of the gun, of the swing and of every
                    // filter. Guarded the same way: looking straight up or down
                    // the horizon reference vanishes and the angle is noise.
                    float ray[3];
                    if (aimtrace::ctrl_ray(ray)) {
                        const float rn = sqrtf(ray[0]*ray[0] + ray[1]*ray[1] + ray[2]*ray[2]);
                        if (rn > 1e-4f) {
                            const float r[3] = {ray[0]/rn, ray[1]/rn, ray[2]/rn};
                            const float wu[3] = {0.0f, 0.0f, 1.0f};   // game up, z
                            const float wd = wu[0]*r[0] + wu[1]*r[1] + wu[2]*r[2];
                            const float ud = cu[0]*r[0] + cu[1]*r[1] + cu[2]*r[2];
                            float wp[3] = {wu[0]-wd*r[0], wu[1]-wd*r[1], wu[2]-wd*r[2]};
                            float up[3] = {cu[0]-ud*r[0], cu[1]-ud*r[1], cu[2]-ud*r[2]};
                            const float wn = sqrtf(wp[0]*wp[0]+wp[1]*wp[1]+wp[2]*wp[2]);
                            const float un = sqrtf(up[0]*up[0]+up[1]*up[1]+up[2]*up[2]);
                            if (wn > 0.10f && un > 0.10f) {
                                for (int i = 0; i < 3; ++i) { wp[i] /= wn; up[i] /= un; }
                                const float wdot = wp[0]*up[0] + wp[1]*up[1] + wp[2]*up[2];
                                const float wcr[3] = {wp[1]*up[2]-wp[2]*up[1],
                                                      wp[2]*up[0]-wp[0]*up[2],
                                                      wp[0]*up[1]-wp[1]*up[0]};
                                const float wsg = wcr[0]*r[0] + wcr[1]*r[1] + wcr[2]*r[2];
                                croll = atan2f(wsg, wdot);
                                croll_ok = true;
                            }
                        }
                    }
                    st(g_ri_croll, croll);
                }

                // ---------------------------------------------------------
                // BUILD 189: UNFLIP. This is the fix.
                //
                // theta is the angle from the GUN's own up to the controller's
                // up. The gun's up is model +Z rotated by desired, so it is
                // whatever orientation the engine handed us, and build 188
                // proved the engine can hand it back inverted: after the fall
                // animation theta acquired a stable half-turn offset and kept
                // it for the rest of the run (40 of the last 44 samples over
                // 135 deg, mean 171). The shortest-arc swing aligns the BARREL
                // and by construction carries no twist, so it never corrects a
                // flipped up, and the error persists until something re-seeds
                // the rig. That is why going prone sometimes restores the roll.
                //
                // croll is immune to this. It is the controller's own roll
                // about its own ray against the world horizon: no gun, no
                // swing, no filter, nothing the engine can invert. Build 188
                // showed theta and croll agreeing to within about 20 deg
                // through normal play and diverging to about 180 the instant
                // the animation flipped the reference. So croll is the honest
                // witness, and disagreement near a half turn IS the flip.
                //
                // Correct rather than filter. A filter cannot fix a reference
                // that is upside down, which is why 186 wound up and 187 died.
                //
                // Hysteresis, because the healthy disagreement is not zero: it
                // ran to about 20 deg in normal play, so entering the flipped
                // state needs 120 deg and leaving it needs 60. A single 90 deg
                // threshold would chatter on the noise.
                // ---------------------------------------------------------
                static bool s_flipped  = false;
                static int  s_flipcand = 0;
                static int  s_relcand  = 0;
                const bool unflip_on =
                    g_wgun_roll_unflip.load(std::memory_order_relaxed) != 0;

                // DETECT only while the witness is valid.
                if (unflip_on && croll_ok) {
                    float dv = traw - croll;
                    while (dv >  3.14159265f) dv -= 6.28318531f;
                    while (dv < -3.14159265f) dv += 6.28318531f;
                    const float adv = dv < 0.0f ? -dv : dv;
                    if (!s_flipped) {
                        s_relcand = 0;
                        // Review fix 3: debounce. This block is entered TWICE
                        // PER FRAME and the second entry measures a gun that
                        // already carries the first entry's roll, so the two
                        // entries are not the same quantity; and theta turns
                        // about the filtered aim while croll turns about the
                        // raw controller ray, so a fast whip can spike adv well
                        // past the 20 deg a calm run showed. A real engine flip
                        // is stable for the rest of the run, so requiring three
                        // consecutive samples costs the fix nothing and kills
                        // every transient. A single false latch would slew the
                        // weapon 180 deg at maxstep and drag the arms with it.
                        if (adv > 2.09439510f) {                    // 120 deg
                            if (++s_flipcand >= 3) {
                                s_flipped  = true;
                                s_flipcand = 0;
                                g_ri_flips.fetch_add(1, std::memory_order_relaxed);
                            }
                        } else {
                            s_flipcand = 0;
                        }
                    } else {
                        s_flipcand = 0;
                        // Replay fix, 2026-09-05: the RELEASE is debounced too.
                        // Entry needed three samples and release needed one, so
                        // a single dip below 60 dropped the correction. Replayed
                        // against the real run, the flipped window held a
                        // genuine rig re-seed release and the log could not
                        // bound how long it lasted; at the dip rate the 0.7
                        // percent subsample suggests, the post-fall window could
                        // carry many brief releases, each one a half turn in and
                        // straight back out. That is a roll buzz, and it reads
                        // from the headset as the roll being unstable. A real
                        // re-seed is stable, so symmetric debounce costs nothing.
                        if (adv < 1.04719755f) {                   // 60 deg
                            if (++s_relcand >= 3) {
                                s_flipped = false;
                                s_relcand = 0;
                            }
                        } else {
                            s_relcand = 0;
                        }
                    }
                }

                // APPLY regardless of croll_ok. Review fix 2: the latch is a
                // property of the RIG, not of whether the horizon happens to be
                // measurable this frame. croll's own guard collapses when the
                // muzzle is within about 6 deg of straight up or straight down,
                // which is an ordinary thing to do (looting, idling, prone). If
                // the correction were dropped there, theta would jump a half
                // turn and the filter would slew the weapon at maxstep toward
                // the wrong roll and back again, taking the hands and the
                // build-182 arm solve with it. Holding the correction through a
                // degenerate sample matches how the gn2/cn2 guard already
                // freezes s_roll rather than zeroing it.
                if (unflip_on && s_flipped) {
                    theta -= 3.14159265f;
                    while (theta >  3.14159265f) theta -= 6.28318531f;
                    while (theta < -3.14159265f) theta += 6.28318531f;
                }
                g_ri_flipped.store((unflip_on && s_flipped) ? 1u : 0u,
                                   std::memory_order_relaxed);

                // Filter the ANGLE, wrapped. Lerping a raw angle across the
                // +-180 seam spins the gun the long way round; one-poling the
                // up VECTOR instead would break its perpendicularity to the
                // barrel and leak roll back into aim.
                static float s_roll = 0.0f;
                static uint32_t s_rgen = 0xFFFFFFFFu;
                if (gen != s_rgen) { s_rgen = gen; s_roll = theta; }
                float d = theta - s_roll;
                while (d >  3.14159265f) d -= 6.28318531f;
                while (d < -3.14159265f) d += 6.28318531f;
                const float ralpha = g_wgun_smooth.load(std::memory_order_relaxed);
                const float rcap =
                    g_wgun_maxstep.load(std::memory_order_relaxed) * 0.01745329f;
                float step = ralpha * d;
                if (step >  rcap) step =  rcap;
                if (step < -rcap) step = -rcap;
                s_roll += step;

                // BUILD 187's wrap of s_roll was REVERTED for build 188 and is
                // NOT in this binary. It did stop the accumulator winding past
                // 180 deg, but the owner lost the Z-axis roll from the start of
                // the run, so it is a regression and 188 must measure from the
                // 186 behaviour he already knows. The wrap is still correct in
                // isolation and it is kept in the history at commit 4981f3f;
                // do not reapply it until the measurement below is understood,
                // because 187 proved theta is wild and a filter cannot fix a
                // wild input. Ledger row 187 REFUTED has the full account.

                {   // BUILD 184: the roll we add, for the wroll: line
                    uint32_t b; memcpy(&b, &s_roll, sizeof(b));
                    g_wr_roll.store(b, std::memory_order_relaxed);
                }
                {   // BUILD 187: the measurement behind it, same line
                    uint32_t b; memcpy(&b, &theta, sizeof(b));
                    g_wr_theta.store(b, std::memory_order_relaxed);
                }

                const float h = 0.5f * s_roll;
                const float sh = sinf(h);
                const float tw[4] = {t[0]*sh, t[1]*sh, t[2]*sh, cosf(h)};
                float rolled[4];
                qmul(tw, desired, rolled);
                const float rn2 = sqrtf(rolled[0]*rolled[0] + rolled[1]*rolled[1] +
                                        rolled[2]*rolled[2] + rolled[3]*rolled[3]);
                if (rn2 > 1e-6f) {
                    for (int i = 0; i < 4; ++i) desired[i] = rolled[i] / rn2;
                    grwxr_wg_roll = grwxr_wg_roll + 1;
                }
            } else {
                // BUILD 188, LOG ONLY. The degeneracy guard rejected this
                // sample, so no roll was applied and s_roll kept its old value.
                // This was silent before. 187's run showed the guard never
                // firing (roll == rots), but a run where the roll dies while
                // this counter climbs is a different fault entirely, and until
                // now the log could not tell the two apart.
                g_ri_skips.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    }   // BUILD 190: end of the swing-and-measure path

    // Back to model space: modelQ = conj(rootQ) * desiredWorldQ.
    const float conj[4] = {-rx, -ry, -rz, rw};
    float modelQ[4];
    qmul(conj, desired, modelQ);
    const float mn = sqrtf(modelQ[0]*modelQ[0] + modelQ[1]*modelQ[1] +
                           modelQ[2]*modelQ[2] + modelQ[3]*modelQ[3]);
    if (!(mn > 1e-6f)) { bump(&grwxr_wg_badq); return; }
    for (int i = 0; i < 4; ++i) modelQ[i] /= mn;
    for (int i = 0; i < 4; ++i)
        if (!isfinite(modelQ[i])) { bump(&grwxr_wg_badq); return; }

    __try {
        float* q = (float*)(rec + 0x10);
        q[0] = modelQ[0]; q[1] = modelQ[1];
        q[2] = modelQ[2]; q[3] = modelQ[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bump(&grwxr_wg_badq);
        return;
    }
    bump(&grwxr_wg_rot);
    // BUILD 183: mirror the rotation now, so a bail-out below (no controller
    // position, an unreadable record) cannot leave the mirrors a frame
    // behind node 10; mirrored again after the position write.
    if (mirror_gunroot(buf, rec)) bump(&grwxr_wg_mirror);

    // BUILD 81: POSITION. Rotation alone pivots the gun where the animation
    // put it, so you can point it but not raise it to your eye. This puts the
    // gun-root ON the controller.
    //
    // Off by default and separate from the mode, so rotation-only and
    // rotation-plus-position are one cfg line apart and a regression in this
    // half cannot cost the half that is already verified.
    if (!g_wgun_pos.load(std::memory_order_relaxed)) {
        gun_survive_record(buf, rec, root, rt0, desired);   // BUILD 184
        // 2026-08-31: rotation-only mode still moves the hands, about the
        // root's own unmoved position, so the grip survives the pivot.
        float hout[2][3], hfound[2][3], hrot[2][4], hfq[2][4], hax[2][3]; bool hok[2] = {false, false};
        if (want_hands && hands_apply(buf, bq, bt0, modelQ, bt0,
                g_wgun_grab.load(std::memory_order_relaxed) ? two_w : 1.0f,
                root, rt0, flp, hout, hfound, hok, hrot, hfq, hax))
            bump(&grwxr_wg_hands);
        if (g_wgun_hide_arms.load(std::memory_order_relaxed)) {
            if (arms_collapse(buf)) bump(&grwxr_wg_arms);
        } else if (g_wgun_arms.load(std::memory_order_relaxed)) {
            for (int s = 0; s < 2; ++s) {
                if (!hok[s]) continue;
                const int rr = arms_solve(buf, s, hfound[s], hout[s], hrot[s], hfq[s], hax[s]);
                if (rr)      bump(&grwxr_wg_ik);
                if (rr == 2) bump(&grwxr_wg_ikcl);
            }
        }
        return;
    }

    float cp[3];
    if (!ctrl_pos_late(cp)) { bump(&grwxr_wg_nopos); return; }
    for (int i = 0; i < 3; ++i)
        if (!isfinite(cp[i])) { bump(&grwxr_wg_nopos); return; }

    // -----------------------------------------------------------------------
    // 2026-08-31: THE ANCHOR, build 123's standing single change. cp is the
    // RIGHT controller, and build 123 measured this tester's right hand as the
    // FRONT hand, so the root was being anchored to the handguard end and
    // every downstream number that says "from the root" was measured from the
    // wrong end of the weapon. When the flip is live, crossfade the anchor to
    // the rear (left) controller on exactly the direction's own authority
    // curve: at two_w = 0 this is byte-for-byte the old behaviour, so the
    // confirmed one-handed hold cannot move, and a gate-refused frame changes
    // nothing.
    // -----------------------------------------------------------------------
    const bool anchor_fixed =
        g_wgun_two_anchor.load(std::memory_order_relaxed) != 0;
    if (anchor_fixed && two_have_l && two_w > 0.0f && two_front_is_right) {
        cp[0] += two_w * (lpos[0] - cp[0]);
        cp[1] += two_w * (lpos[1] - cp[1]);
        cp[2] += two_w * (lpos[2] - cp[2]);
        grwxr_wg_anchor = grwxr_wg_anchor + 1;
    }

    // -----------------------------------------------------------------------
    // BUILD 99: THE GRIP. Where the weapon sits in the hand, and the front-hand
    // hold. See CameraProbe.h for what each number means.
    //
    // Both parts are applied to the TARGET position, before the clamp and the
    // filter, so everything that already protects this write (the clamp against
    // the engine's own placement, the one-pole, the generation reset) protects
    // these too and none of it had to be touched.
    //
    // Applied with `desired`, the orientation the gun is being given THIS call,
    // not the one it currently has. Using the current one would make the offset
    // lag the weapon by a frame during a fast hand sweep and read as the gun
    // flexing in the hand.
    // -----------------------------------------------------------------------
    // BUILD 101: kept outside the block, because the clamp below has to know
    // about it. See the long comment at the clamp.
    float grip_w[3] = {0.0f, 0.0f, 0.0f};
    {
        // BUILD 126: GRIP BYPASS, the "park the gun on the blob" test.
        //
        // Owner's idea, and it is the right one: the hand markers are the only
        // part of this mod ever headset-confirmed as tracking correctly
        // ("tracks incredibly well", build 42). They are therefore ground truth
        // for where the mod believes the hand is.
        //
        // What makes them a real instrument here is the step they DO NOT take.
        // The marker goes ctrl_pos minus head_pos, into head-local, and
        // straight to the screen (VRMirror.cpp:1626-1631). The gun anchor takes
        // those same two steps and then ONE MORE: the (lx, lf, lu) swizzle
        // mapped through camera::base_frame into engine world space
        // (VRMirror.cpp:1752-1764). **That third step has never been validated
        // by anything.** The marker cannot validate it, because it never runs
        // it. The comment claiming the gun uses "the hand-marker verified
        // route" is only true of the first half.
        //
        // The ENGINE drawing the weapon is the independent reference the
        // markers cannot be: we hand it a world position and it renders there.
        // So "does the gun sit on the blob" tests exactly the unvalidated step,
        // visually, with no log and no ambiguity.
        //
        // The static grip offset has to be out of the way for that to mean
        // anything: the tester's fwd=-0.32 up=+0.40 is a deliberate 0.512 m
        // displacement, so today the gun is SUPPOSED to miss the blob. This
        // bypasses the offset for the duration of the test WITHOUT overwriting
        // his tuned values, so nothing has to be typed back in afterwards.
        const bool bypass = g_wgun_grip_bypass.load(std::memory_order_relaxed) != 0;
        float gf = bypass ? 0.0f : g_wgun_grip_fwd.load(std::memory_order_relaxed);
        float gl = bypass ? 0.0f : g_wgun_grip_lat.load(std::memory_order_relaxed);
        float gu = bypass ? 0.0f : g_wgun_grip_up.load(std::memory_order_relaxed);
        // BUILD 219: the grip bone. The root goes to controller MINUS the
        // grip's model-space offset, so the grip itself lands on the
        // controller; the cfg trims above add to it. The blob test's bypass
        // zeroes this too, since that test wants the root on the controller.
        if (!bypass && g_wgun_grip_bone.load(std::memory_order_relaxed) &&
            g_wgun_grip_bone_ok.load(std::memory_order_acquire)) {
            gl -= g_wgun_grip_bone_x.load(std::memory_order_relaxed);
            gf -= g_wgun_grip_bone_y.load(std::memory_order_relaxed);
            gu -= g_wgun_grip_bone_z.load(std::memory_order_relaxed);
        }
        if (gf != 0.0f || gl != 0.0f || gu != 0.0f) {
            // Model frame: +X lateral, +Y the barrel, +Z up.
            const float go[3] = {gl, gf, gu};
            float gw[3];
            qrot(desired, go, gw);
            if (isfinite(gw[0]) && isfinite(gw[1]) && isfinite(gw[2])) {
                cp[0] += gw[0]; cp[1] += gw[1]; cp[2] += gw[2];
                grip_w[0] = gw[0]; grip_w[1] = gw[1]; grip_w[2] = gw[2];
            }
        }

        // THE FRONT-HAND HOLD. Slide along the barrel until the handguard sits
        // on the front hand. A translation along the aim axis cannot change
        // where the weapon points, so this is safe by construction in exactly
        // the way the roll trim is.
        const float tw = g_wgun_grip_two.load(std::memory_order_relaxed);
        // 2026-08-31: with the anchor corrected, the front hand is whichever
        // hand MEASURED as front, and the build-123 stand-down remains only
        // for the case it was written for: a flipped hold with the anchor fix
        // switched off, where the root is still on the front hand.
        if (tw > 0.0f && two_have_l && two_w > 0.0f &&
            (!two_front_is_right || anchor_fixed)) {
            const float* fh = two_front_is_right ? rpos : lpos;
            // Where the front hand actually is along the barrel, measured from
            // the root we are about to place.
            const float d[3] = {fh[0] - cp[0], fh[1] - cp[1], fh[2] - cp[2]};
            const float along = d[0]*s_dir[0] + d[1]*s_dir[1] + d[2]*s_dir[2];
            if (isfinite(along)) {
                // Error against where the weapon's own handguard is. Positive
                // means the hand is further out than the handguard, so the
                // weapon slides forward to meet it.
                float e = along - kHandguardFwd;
                // Bound it before it is scaled: a front hand at arm's length or
                // tucked at the chest must not translate the weapon a metre.
                if (e >  0.35f) e =  0.35f;
                if (e < -0.35f) e = -0.35f;
                const float k = tw * two_w;   // fades with the direction blend
                cp[0] += k * e * s_dir[0];
                cp[1] += k * e * s_dir[1];
                cp[2] += k * e * s_dir[2];
                grwxr_wg_grip = grwxr_wg_grip + 1;
            }
        }
    }

    float bt[4], rt[4];
    if (!read_block(rec + 0x00, bt, sizeof(bt)) ||
        !read_block(pose + 0x00, rt, sizeof(rt))) {
        bump(&grwxr_wg_norec); return;
    }
    for (int i = 0; i < 3; ++i)
        if (!isfinite(bt[i]) || !isfinite(rt[i])) { bump(&grwxr_wg_badq); return; }

    // The engine's own placement this frame, in world space, is the anchor the
    // clamp is measured from: world = rootQ * boneT + rootT.
    float engw[3];
    qrot(root, bt, engw);
    engw[0] += rt[0]; engw[1] += rt[1]; engw[2] += rt[2];

    // Scale hand travel about the engine's own gun position rather than about
    // the world origin, so a scale other than 1.0 changes how far the gun
    // moves per centimetre of hand and nothing else.
    const float sc = g_wgun_pos_scale.load(std::memory_order_relaxed);
    float want[3] = {engw[0] + (cp[0] - engw[0]) * sc,
                     engw[1] + (cp[1] - engw[1]) * sc,
                     engw[2] + (cp[2] - engw[2]) * sc};

    // HARD CLAMP, skeleton rule 5. However wrong the controller pose is, the
    // gun stays inside this radius of where the engine put it. A tracking
    // dropout or a bad recenter then misplaces the gun by at most this much
    // instead of throwing it across the map, which is the difference between
    // "that looks off" and "the session is over".
    //
    // BUILD 101, AND THIS IS THE ONE BEHAVIOURAL CHANGE IN THE BUILD. The clamp
    // used to measure from the engine's placement alone, so a DELIBERATE static
    // grip offset spent the safety budget before the player's hand had moved at
    // all. `[VERIFIED, log grwxr-29328]` the tester settled on fwd=-0.32 and
    // up=+0.40, a 0.512 m offset against a 0.60 m clamp: 85% of the rail gone,
    // and every real hand movement then railed instantly. That reads in the
    // headset as the gun refusing to follow, which is exactly what a safety
    // clamp is NOT supposed to do to a value the player chose on purpose.
    //
    // So the anchor is the engine's placement SHIFTED BY THE SAME STATIC GRIP
    // OFFSET. The clamp still bounds the thing it was written to bound, which is
    // how far tracking may throw the weapon from where the engine believes it
    // is, and it no longer bounds the tester's own trim. The front-hand slide
    // stays INSIDE the budget deliberately: it is driven by the left controller,
    // so it is exactly the kind of input the rail exists to survive, and it is
    // already bounded to +-0.35 m of its own.
    const float lim = g_wgun_pos_clamp.load(std::memory_order_relaxed);
    const float anch[3] = {engw[0] + grip_w[0],
                           engw[1] + grip_w[1],
                           engw[2] + grip_w[2]};
    float dv[3] = {want[0] - anch[0], want[1] - anch[1], want[2] - anch[2]};
    const float dl = sqrtf(dv[0]*dv[0] + dv[1]*dv[1] + dv[2]*dv[2]);
    if (dl > lim && dl > 1e-6f) {
        const float k = lim / dl;
        want[0] = anch[0] + dv[0] * k;
        want[1] = anch[1] + dv[1] * k;
        want[2] = anch[2] + dv[2] * k;
        // Measurement rule 1: a gate that can suppress an effect gets its own
        // counter. This one silently ate a tuning session because nothing
        // counted it.
        grwxr_wg_clamped = grwxr_wg_clamped + 1;
    }

    // One-pole in WORLD space, reset on the same generation counter the
    // rotation filter uses so arming never slews out of a stale position.
    static float s_pos[3] = {0, 0, 0};
    static uint32_t s_pgen = 0xFFFFFFFFu;
    if (gen != s_pgen) {
        s_pgen = gen;
        s_pos[0] = want[0]; s_pos[1] = want[1]; s_pos[2] = want[2];
    } else {
        const float pa = g_wgun_pos_smooth.load(std::memory_order_relaxed);
        s_pos[0] += pa * (want[0] - s_pos[0]);
        s_pos[1] += pa * (want[1] - s_pos[1]);
        s_pos[2] += pa * (want[2] - s_pos[2]);
    }
    gpair_note(2, want, s_pos, nullptr);   // BUILD 277, log only

    // BUILD 221: the eyepiece in WORLD, from the root we are about to write and
    // the gun's own frame (+X right, +Y barrel, +Z up through `desired`), for
    // the present thread's PiP disc. The bone when the pick read one, the cfg
    // fallback otherwise. Reads and three relaxed stores; nothing else moves.
    if (g_pip_on.load(std::memory_order_relaxed)) {
        float L[3];
        if (g_pip_aim_ok.load(std::memory_order_acquire)) {
            L[0] = g_pip_aim_x.load(std::memory_order_relaxed);
            L[1] = g_pip_aim_y.load(std::memory_order_relaxed);
            L[2] = g_pip_aim_z.load(std::memory_order_relaxed);
        } else {
            L[0] = g_pip_fb_lat.load(std::memory_order_relaxed);
            L[1] = g_pip_fb_fwd.load(std::memory_order_relaxed);
            L[2] = g_pip_fb_up.load(std::memory_order_relaxed);
        }
        float lw[3];
        qrot(desired, L, lw);
        lw[0] += s_pos[0]; lw[1] += s_pos[1]; lw[2] += s_pos[2];
        if (isfinite(lw[0]) && isfinite(lw[1]) && isfinite(lw[2])) {
            g_pip_lens_x.store(lw[0], std::memory_order_relaxed);
            g_pip_lens_y.store(lw[1], std::memory_order_relaxed);
            g_pip_lens_z.store(lw[2], std::memory_order_relaxed);
            g_pip_lens_frame.store(d3d11::frame_count(), std::memory_order_relaxed);
            g_pip_lens_ok.store(1, std::memory_order_release);
        }
    } else {
        g_pip_lens_ok.store(0, std::memory_order_relaxed);
    }

    // Back to model space: modelT = conj(rootQ) * (world - rootT).
    const float rel[3] = {s_pos[0] - rt[0], s_pos[1] - rt[1], s_pos[2] - rt[2]};
    float modelT[3];
    qrot(conj, rel, modelT);
    for (int i = 0; i < 3; ++i)
        if (!isfinite(modelT[i])) { bump(&grwxr_wg_badq); return; }

    __try {
        float* t = (float*)(rec + 0x00);
        t[0] = modelT[0]; t[1] = modelT[1]; t[2] = modelT[2];
        // t[3] is the record's w lane: left exactly as the engine wrote it.
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bump(&grwxr_wg_badq);
        return;
    }
    bump(&grwxr_wg_pos);
    // BUILD 183: the record is final here; mirror it.
    if (mirror_gunroot(buf, rec)) bump(&grwxr_wg_mirror);
    gun_survive_record(buf, rec, root, rt0, desired);   // BUILD 184

    // 2026-08-31: the hands, last, from the root's full found-to-left change.
    // A call that bailed anywhere above simply skips them; the next call
    // measures its delta from whatever the record then holds, so the hands can
    // never drift, only sit out a frame.
    float hout[2][3], hfound[2][3], hrot[2][4], hfq[2][4], hax[2][3]; bool hok[2] = {false, false};
    if (want_hands && hands_apply(buf, bq, bt0, modelQ, modelT,
            g_wgun_grab.load(std::memory_order_relaxed) ? two_w : 1.0f,
            root, rt0, flp, hout, hfound, hok, hrot, hfq, hax))
        bump(&grwxr_wg_hands);
    if (g_wgun_hide_arms.load(std::memory_order_relaxed)) {
        if (arms_collapse(buf)) bump(&grwxr_wg_arms);
    } else if (g_wgun_arms.load(std::memory_order_relaxed)) {
        for (int s = 0; s < 2; ++s) {
            if (!hok[s]) continue;
            const int rr = arms_solve(buf, s, hfound[s], hout[s], hrot[s], hfq[s], hax[s]);
            if (rr)      bump(&grwxr_wg_ik);
            if (rr == 2) bump(&grwxr_wg_ikcl);
        }
    }
}

void set_wbaxis(int on) {
    const int want = on ? 1 : 0;
    if (g_wbaxis.exchange(want, std::memory_order_relaxed) == want) return;
    LOG_INFO("wbax: barrel-axis measurement %s. Log only, writes nothing. Aim "
             "at something and hold it: the bone axis whose dot with the game's"
             " aim direction stays near 1.0 (or -1.0) is the barrel.",
             want ? "ARMED" : "off");
}

void set_wskel_write(int mode) {
    const bool on  = mode > 0;
    const bool was = g_wskel_write_on.exchange(on, std::memory_order_relaxed);
    g_wskel_mode.store(mode, std::memory_order_relaxed);
    if (on && !was) g_wskel_writes.store(0, std::memory_order_relaxed);
    if (!on) g_wskel_tgt.store(0, std::memory_order_relaxed);
}

bool wskel_marker(float out[3]) {
    const uint64_t p = g_wskel_cand.load(std::memory_order_relaxed);
    if (!p) return false;
    float o[3];
    if (!read_block(p + 0x120, o, sizeof(o))) return false;
    if (!isfinite(o[0]) || !isfinite(o[1]) || !isfinite(o[2])) return false;
    out[0] = o[0]; out[1] = o[1]; out[2] = o[2];
    return true;
}

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

    // Build 20 probe, read-only: the APPLIED GameSettings global (RE-notes
    // 2026-08-03, static 0x04CCDE70, no pointer chase). GRW.ini already holds
    // the three blur keys at 0, so whether the live applied copy agrees is
    // the discriminator for the FP body blur route: nonzero here means the
    // settings write path is worth taking, zero means the blur lives in the
    // proximity fade instead. Logged at first read and on change only.
    if (g_module) {
        static uint8_t s_blur_prev[3] = {0, 0, 0};
        static int     s_blur_have    = -1;
        uint8_t cur[3];
        const uint64_t gs = g_module + 0x04CCDE70;
        const bool ok = read_block(gs + 0xAE, &cur[0], 1) &&
                        read_block(gs + 0xB0, &cur[1], 1) &&
                        read_block(gs + 0xB9, &cur[2], 1);
        if (ok && (s_blur_have != 1 || memcmp(cur, s_blur_prev, 3) != 0)) {
            memcpy(s_blur_prev, cur, sizeof(s_blur_prev));
            s_blur_have = 1;
            LOG_INFO("settings: applied CloseRangeBlur=%u HighQualityDOF=%u "
                     "MotionBlur=%u", cur[0], cur[1], cur[2]);
        } else if (!ok && s_blur_have == -1) {
            s_blur_have = 0;
            LOG_INFO("settings: applied GameSettings unreadable at 0x%012llX",
                     (unsigned long long)gs);
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
        // BUILD 224: PRE-LATCH PROMOTION, 1 Hz. The asm offered a candidate
        // whose class table matched. Promote it only when its entity handle
        // resolves to the pinned player entity (the tie, see head_obj_entity)
        // and its arrays read sane. A refused candidate is named once per
        // pointer so a teammate's head is visible in the log rather than
        // hidden in the world. The slot is cleared after judging, which
        // re-opens the offer, so a candidate that arrives before the pin is
        // judged again a second later.
        if (g_headlatch_hook.installed() &&
            g_head_prelatch.load(std::memory_order_relaxed)) {
            const uint64_t cand = grwxr_headlatch_cand;
            if (cand) {
                const uint64_t pent =
                    g_player_entity.load(std::memory_order_relaxed);
                if (cand == grwxr_headhide_obj) {
                    // already latched, by either route
                } else if (!pent) {
                    g_hl_no_pin.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // BUILD 225: what the 224 run taught. The control refused
                    // every offer (ctl_fail=111): the candidate is real and of
                    // the head class, but its arrays are empty until the
                    // engine's proximity test populates them, which never runs
                    // in our state. So empty arrays no longer refuse, the tie
                    // decides when it can be read, and a candidate seen on
                    // three consecutive ticks is taken when it cannot. A
                    // DIFFERENT entity still refuses. The dump below, once per
                    // pointer, is the measurement for the next reading.
                    uint64_t ent = 0, a1 = 0;
                    uint16_t n1  = 0;
                    static uint64_t s_named[4] = {};
                    static int      s_nnamed   = 0;
                    static uint64_t s_dumped[3] = {};
                    static int      s_ndump     = 0;
                    static uint64_t s_stable    = 0;
                    static int      s_agree     = 0;
                    const bool tie_ok = head_obj_entity(cand, &ent);
                    const int  ctl    = head_obj_control(cand, &a1, &n1);
                    if (ctl != 1) g_hl_arr_fail.fetch_add(1, std::memory_order_relaxed);
                    // BUILD 226: THE POSITION TIE. The 225 run's candidate
                    // carried an entity that was not the pin, every tick, and
                    // it was the only candidate: either the head object refers
                    // to the character through a different entity object than
                    // the cPlayerComponent's, or it is a teammate's. Both
                    // entity classes carry a world position at +0x50 (RE-notes
                    // 1712); the pinned body skeleton's origin is at +0x120.
                    // Same spot = the same character; far = someone else.
                    float epos[3] = {0, 0, 0}, ppos[3] = {0, 0, 0}, spos[3] = {0, 0, 0};
                    bool  epos_ok = false, spos_ok = false, ppos_ok = false;
                    float pdist = -1.0f;
                    {
                        const uint64_t skel = headpose::player_obj();
                        if (ent && read_block(ent + 0x50, epos, sizeof(epos)) &&
                            isfinite(epos[0]) && isfinite(epos[1]) && isfinite(epos[2]))
                            epos_ok = true;
                        if (read_block(pent + 0x50, ppos, sizeof(ppos)) &&
                            isfinite(ppos[0]) && isfinite(ppos[1]) && isfinite(ppos[2]))
                            ppos_ok = true;
                        if (skel && read_block(skel + 0x120, spos, sizeof(spos)) &&
                            isfinite(spos[0]) && isfinite(spos[1]) && isfinite(spos[2]))
                            spos_ok = true;
                        if (epos_ok && spos_ok) {
                            const float dx = epos[0] - spos[0], dy = epos[1] - spos[1],
                                        dz = epos[2] - spos[2];
                            pdist = sqrtf(dx * dx + dy * dy + dz * dz);
                        }
                    }
                    bool dumped = false;
                    for (int i = 0; i < s_ndump; ++i) if (s_dumped[i] == cand) dumped = true;
                    if (!dumped && s_ndump < 3) {
                        s_dumped[s_ndump++] = cand;
                        uint8_t  b10 = 0, b30 = 0, b68 = 0, b69 = 0;
                        uint64_t p38 = 0, p40 = 0, p4c = 0, h0 = 0;
                        uint16_t n4a = 0, n56 = 0;
                        int32_t  hc = 0;
                        read_block(cand + 0x10, &b10, 1);
                        read_block(cand + 0x30, &b30, 1);
                        read_block(cand + 0x38, &p38, 8);
                        read_block(cand + 0x40, &p40, 8);
                        read_block(cand + 0x4A, &n4a, 2);
                        read_block(cand + 0x4C, &p4c, 8);
                        read_block(cand + 0x56, &n56, 2);
                        read_block(cand + 0x68, &b68, 1);
                        read_block(cand + 0x69, &b69, 1);
                        if (p38 >= 0x10000) { read_block(p38, &h0, 8); read_block(p38 + 0xC, &hc, 4); }
                        {
                            uint64_t w = 0, vis = 0;
                            read_block(cand + 0x20, &w, 8);
                            if (w >= 0x10000) read_block(w + 0x10, &vis, 8);
                            LOG_INFO("hide: candidate 0x%012llX link: +20 (W, the camera "
                                     "object)=0x%012llX [W+10] (its visual)=0x%012llX%s",
                                     (unsigned long long)cand, (unsigned long long)w,
                                     (unsigned long long)vis,
                                     (w >= 0x10000 && vis >= 0x10000)
                                         ? " -> Populate can run"
                                         : " -> NOT linked, Populate would fault; waiting");
                        }
                        LOG_INFO("hide: candidate 0x%012llX dump: +10=%u +30=%u "
                                 "+38=0x%012llX [+0]=0x%012llX [+C]=%d | a1=0x%012llX "
                                 "n1=%u a2=0x%012llX n2=%u s1=%u s2=%u | pin=0x%012llX "
                                 "| ent pos=(%.2f %.2f %.2f)%s pin pos=(%.2f %.2f %.2f)%s "
                                 "body origin=(%.2f %.2f %.2f)%s ent-to-body=%.2fm",
                                 (unsigned long long)cand, b10, b30,
                                 (unsigned long long)p38, (unsigned long long)h0, hc,
                                 (unsigned long long)p40, n4a,
                                 (unsigned long long)p4c, n56, b68, b69,
                                 (unsigned long long)pent,
                                 epos[0], epos[1], epos[2], epos_ok ? "" : "?",
                                 ppos[0], ppos[1], ppos[2], ppos_ok ? "" : "?",
                                 spos[0], spos[1], spos[2], spos_ok ? "" : "?",
                                 pdist);
                    }
                    bool promote = false;
                    const char* why = "";
                    if (tie_ok && ent && ent != pent && pdist >= 0.0f && pdist < 1.5f && ctl >= 0) {
                        // 226: a different entity object standing where the
                        // pinned body stands is the same character.
                        promote = true; why = "the position tie (a different entity object at the body)";
                        g_hl_pos_promos.fetch_add(1, std::memory_order_relaxed);
                    } else if (tie_ok && ent && ent != pent && pdist >= 1.5f) {
                        g_hl_pos_refused.fetch_add(1, std::memory_order_relaxed);
                        g_hl_tie_fail.fetch_add(1, std::memory_order_relaxed);
                        bool seen = false;
                        for (int i = 0; i < s_nnamed; ++i) if (s_named[i] == cand) seen = true;
                        if (!seen && s_nnamed < 4) {
                            s_named[s_nnamed++] = cand;
                            LOG_INFO("hide: candidate 0x%012llX REFUSED: its entity "
                                     "0x%012llX stands %.2f m from the pinned body "
                                     "(another character's head assembly; not touched).",
                                     (unsigned long long)cand, (unsigned long long)ent,
                                     pdist);
                        }
                        s_stable = 0; s_agree = 0;
                    } else if (tie_ok && ent && ent != pent && pdist < 0.0f) {
                        // 226: a different entity whose position cannot be read:
                        // judged on stability like an unreadable tie.
                        g_hl_tie_unread.fetch_add(1, std::memory_order_relaxed);
                        if (cand == s_stable) ++s_agree; else { s_stable = cand; s_agree = 1; }
                        if (s_agree >= 3) {
                            promote = true; why = "three stable ticks, a different entity with no readable position";
                            g_hl_stable_promos.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (tie_ok && ent && ent != pent) {
                        g_hl_tie_fail.fetch_add(1, std::memory_order_relaxed);
                        bool seen = false;
                        for (int i = 0; i < s_nnamed; ++i) if (s_named[i] == cand) seen = true;
                        if (!seen && s_nnamed < 4) {
                            s_named[s_nnamed++] = cand;
                            LOG_INFO("hide: candidate 0x%012llX REFUSED: its "
                                     "entity 0x%012llX is not the pinned "
                                     "player 0x%012llX (another character's "
                                     "head assembly; not touched).",
                                     (unsigned long long)cand,
                                     (unsigned long long)ent,
                                     (unsigned long long)pent);
                        }
                        s_stable = 0; s_agree = 0;
                    } else if (tie_ok && ent == pent && ctl >= 0) {
                        promote = true; why = "the entity tie";
                    } else {
                        g_hl_tie_unread.fetch_add(1, std::memory_order_relaxed);
                        if (cand == s_stable) ++s_agree; else { s_stable = cand; s_agree = 1; }
                        if (s_agree >= 3) {
                            promote = true; why = "three stable ticks, tie unreadable";
                            g_hl_stable_promos.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (promote) {
                        const uint64_t prev = grwxr_headhide_obj;
                        grwxr_headhide_obj = cand;
                        if (prev) {
                            uint64_t pa1 = 0;
                            uint16_t pn1 = 0;
                            const uint64_t impl = grwxr_headhide_impl;
                            if (impl && head_obj_control(prev, &pa1, &pn1) == 1)
                                ((void (*)(void*, uint8_t))impl)((void*)prev, 0);
                            g_hl_replaced.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            g_hl_promoted.fetch_add(1, std::memory_order_relaxed);
                        }
                        LOG_INFO("hide: PROMOTED 0x%012llX from the pre-latch on %s "
                                 "(entity 0x%012llX, pin 0x%012llX; array1 n=%u%s)%s",
                                 (unsigned long long)cand, why,
                                 (unsigned long long)ent, (unsigned long long)pent,
                                 (unsigned)n1,
                                 ctl == 1 ? "" : ", EMPTY: the engine's proximity "
                                                 "test has not populated it; the "
                                                 "pulse or the first setter call will",
                                 prev ? ", replacing the previous object" : "");
                        s_stable = 0; s_agree = 0;
                    }
                }
                grwxr_headlatch_cand = 0;   // sample anew next frame
            }
        }
        // BUILD 225: THE ADS PULSE ARM, 1 Hz, HIS DESIGN (2026-09-08 17:50,
        // "fire the oldschool ads automatically twice ... since aiming down
        // sights is what hid the head and helmet and nvg's"; the build is
        // E:\GRW-SHARED\docs\HIDE-2026-09-08-ads-pulse-after-f2.md). Armed
        // here rather than on the F2 edge because first person now comes on
        // by itself (fp_auto) and the arm must wait for the ANCHOR, not the
        // flag. Fires only while no populated head object is held, retried
        // every five seconds, five attempts per pin. The consume side
        // (ads_pulse_value, the input thread) lends the trigger back only
        // while pip_block_ads is already withholding it, so it can never add
        // a press in a vehicle, a stow, a holster or a menu; while no weapon
        // is drawn the arm simply waits and counts.
        {
            static int      s_tick     = 0;
            static int      s_last_arm = -100;
            static uint64_t s_pin      = 0;
            ++s_tick;
            const uint32_t pf   = g_head_pulse_frames.load(std::memory_order_relaxed);
            const uint64_t pent = g_player_entity.load(std::memory_order_relaxed);
            if (pent != s_pin) { s_pin = pent; g_pulse_attempts.store(0, std::memory_order_relaxed); }
            const uint64_t obj = grwxr_headhide_obj;
            uint64_t a1 = 0;
            uint16_t n1 = 0;
            const bool have = obj && head_obj_control(obj, &a1, &n1) == 1;
            {
                const uint32_t pn = g_pulse_n.load(std::memory_order_relaxed);
                if (pn && d3d11::frame_count() >
                        g_pulse_t0.load(std::memory_order_relaxed) + (uint64_t)3 * pn + 120) {
                    g_pulse_n.store(0, std::memory_order_relaxed);
                    g_pulse_expired.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (pf > 0 && grwxr_headhide_on && pent && !have &&
                g_fp_anchored.load(std::memory_order_relaxed) &&
                g_pulse_n.load(std::memory_order_relaxed) == 0 &&
                g_pulse_attempts.load(std::memory_order_relaxed) < 5 &&
                s_tick - s_last_arm >= 5) {
                if (!pip_block_ads()) {
                    g_pulse_wait_weapon.fetch_add(1, std::memory_order_relaxed);
                } else {
                    s_last_arm = s_tick;
                    const uint32_t k = g_pulse_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
                    g_pulse_t0.store(d3d11::frame_count() + 30, std::memory_order_relaxed);
                    g_pulse_n.store(pf, std::memory_order_release);
                    g_pulse_arms.fetch_add(1, std::memory_order_relaxed);
                    LOG_INFO("hide: ADS PULSE ARMED, attempt %u of 5, %u frames per "
                             "phase (build 225, head_pulse). Two synthetic left-trigger "
                             "presses are lent to the game 30 frames from now so the "
                             "engine runs its own ADS transition, populates the head "
                             "object and calls the setter the mod detours. The trigger "
                             "is only lent while pip_block_ads is already withholding it.",
                             k, pf);
                }
            }
        }
        // BUILD 224: PENDING HIDE APPLY, 1 Hz, once per newly latched object.
        // set_head_hide() acts on toggle EDGES only, so an object that
        // arrives AFTER first person came on (what the pre-latch produces at
        // spawn) would otherwise wait for the next F2. The setter is the same
        // idempotent flip set_head_hide() calls from the present thread (this
        // is its first call from the drain thread; the body is pure bit
        // writes, no lock, no callee); the control is checked first so a
        // stale pointer is never called. Behind head_prelatch, so 0 is 223.
        {
            static uint64_t s_applied = 0;
            if (!grwxr_headhide_on ||
                !g_head_prelatch.load(std::memory_order_relaxed))
                s_applied = 0;
            const uint64_t obj  = grwxr_headhide_obj;
            const uint64_t impl = grwxr_headhide_impl;
            uint64_t a1 = 0;
            uint16_t n1 = 0;
            // BUILD 226: THE POPULATE CALL. The object is held and its arrays
            // are empty (control 0): call the engine's own fill routine on
            // it, once per attempt, three attempts per object five seconds
            // apart, only when its camera link and the link's visual read
            // non-null (the test's own guard), then fall through to the
            // setter below on the same tick.
            {
                static uint64_t s_pop_obj = 0;
                static int s_pop_tries = 0, s_pop_tick = -100, s_tick = 0;
                ++s_tick;
                const uint64_t fn = g_populate_fn.load(std::memory_order_acquire);
                uint64_t ca1 = 0;
                uint16_t cn1 = 0;
                if (obj != s_pop_obj) { s_pop_obj = obj; s_pop_tries = 0; }
                if (obj && fn && g_head_populate.load(std::memory_order_relaxed) &&
                    grwxr_headhide_on && s_pop_tries < 3 && s_tick - s_pop_tick >= 5 &&
                    head_obj_control(obj, &ca1, &cn1) == 0) {
                    uint64_t w = 0, vis = 0;
                    const bool linked =
                        read_block(obj + 0x20, &w, sizeof(w)) && w >= 0x10000 &&
                        read_block(w + 0x10, &vis, sizeof(vis)) && vis >= 0x10000;
                    if (!linked) {
                        g_populate_nolink.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        s_pop_tick = s_tick;
                        ++s_pop_tries;
                        g_populate_calls.fetch_add(1, std::memory_order_relaxed);
                        const bool ok = call_populate(fn, obj);
                        uint64_t na1 = 0, na2 = 0;
                        uint16_t nn1 = 0, nn2 = 0;
                        read_block(obj + 0x40, &na1, 8); read_block(obj + 0x4A, &nn1, 2);
                        read_block(obj + 0x4C, &na2, 8); read_block(obj + 0x56, &nn2, 2);
                        if (!ok) g_populate_fault.fetch_add(1, std::memory_order_relaxed);
                        else if (na1 && nn1) g_populate_ok.fetch_add(1, std::memory_order_relaxed);
                        else g_populate_empty.fetch_add(1, std::memory_order_relaxed);
                        LOG_INFO("hide: POPULATE %s on 0x%012llX (attempt %d, W=0x%012llX "
                                 "visual=0x%012llX): array1 n=%u at 0x%012llX, array2 "
                                 "n=%u at 0x%012llX%s",
                                 ok ? "called" : "RAISED", (unsigned long long)obj,
                                 s_pop_tries, (unsigned long long)w,
                                 (unsigned long long)vis, nn1, (unsigned long long)na1,
                                 nn2, (unsigned long long)na2,
                                 (ok && na1 && nn1) ? "; the setter follows on this tick"
                                                    : "");
                    }
                }
            }
            if (grwxr_headhide_on && obj && impl && obj != s_applied &&
                g_head_prelatch.load(std::memory_order_relaxed) &&
                head_obj_control(obj, &a1, &n1) == 1) {
                ((void (*)(void*, uint8_t))impl)((void*)obj, 1);
                grwxr_headhide_direct = grwxr_headhide_direct + 1;
                s_applied = obj;
            }
        }
        // BUILD 224: a drop by the frame path is reported here, on the
        // drain thread, because the frame path never logs.
        {
            static uint64_t s_stale = 0, s_tie = 0;
            const uint64_t st = g_hr_stale_drops.load(std::memory_order_relaxed);
            const uint64_t ti = g_hr_tie_drops.load(std::memory_order_relaxed);
            if (st != s_stale || ti != s_tie) {
                LOG_INFO("hide: latched object 0x%012llX DROPPED by the "
                         "re-assert (%s failed three times in a row); the "
                         "pre-latch or the next setter call re-latches.",
                         (unsigned long long)g_hr_last_drop.load(
                             std::memory_order_relaxed),
                         st != s_stale ? "the array control" : "the entity tie");
                s_stale = st;
                s_tie   = ti;
            }
        }
        if ((++hide_ticks % 10) == 4) {
            LOG_INFO("hide: %s, setter calls=%llu forced=%llu direct=%llu "
                     "obj=0x%012llX | prelatch %s hits=%llu promoted=%llu "
                     "replaced=%llu tie_fail=%llu no_pin=%llu ctl_fail=%llu "
                     "| reassert %s checks=%llu rewrites=%llu "
                     "state_resets=%llu stale_drops=%llu tie_drops=%llu "
                     "| tie_unread=%llu arr_fail=%llu stable_promos=%llu pos_promos=%llu pos_refused=%llu "
                     "| pulse frames=%u arms=%llu delivered=%llu done=%llu "
                     "skipped=%llu expired=%llu wait_weapon=%llu "
                     "| always=%d fp_auto=%d fires=%llu "
                     "| populate %s calls=%llu ok=%llu empty=%llu fault=%llu nolink=%llu",
                     grwxr_headhide_on ? "FORCING (fp on)" : "idle (fp off)",
                     (unsigned long long)grwxr_headhide_calls,
                     (unsigned long long)grwxr_headhide_forced,
                     (unsigned long long)grwxr_headhide_direct,
                     (unsigned long long)grwxr_headhide_obj,
                     !g_headlatch_hook.installed() ? "NOT ARMED"
                         : g_head_prelatch.load(std::memory_order_relaxed) ? "on" : "off",
                     (unsigned long long)grwxr_headlatch_hits,
                     (unsigned long long)g_hl_promoted.load(std::memory_order_relaxed),
                     (unsigned long long)g_hl_replaced.load(std::memory_order_relaxed),
                     (unsigned long long)g_hl_tie_fail.load(std::memory_order_relaxed),
                     (unsigned long long)g_hl_no_pin.load(std::memory_order_relaxed),
                     (unsigned long long)g_hl_ctl_fail.load(std::memory_order_relaxed),
                     g_head_reassert.load(std::memory_order_relaxed) ? "on" : "off",
                     (unsigned long long)g_hr_checks.load(std::memory_order_relaxed),
                     (unsigned long long)g_hr_rewrites.load(std::memory_order_relaxed),
                     (unsigned long long)g_hr_state_resets.load(std::memory_order_relaxed),
                     (unsigned long long)g_hr_stale_drops.load(std::memory_order_relaxed),
                     (unsigned long long)g_hr_tie_drops.load(std::memory_order_relaxed),
                     (unsigned long long)g_hl_tie_unread.load(std::memory_order_relaxed),
                     (unsigned long long)g_hl_arr_fail.load(std::memory_order_relaxed),
                     (unsigned long long)g_hl_stable_promos.load(std::memory_order_relaxed),
                     (unsigned long long)g_hl_pos_promos.load(std::memory_order_relaxed),
                     (unsigned long long)g_hl_pos_refused.load(std::memory_order_relaxed),
                     g_head_pulse_frames.load(std::memory_order_relaxed),
                     (unsigned long long)g_pulse_arms.load(std::memory_order_relaxed),
                     (unsigned long long)g_pulse_frames.load(std::memory_order_relaxed),
                     (unsigned long long)g_pulse_done.load(std::memory_order_relaxed),
                     (unsigned long long)g_pulse_skipped.load(std::memory_order_relaxed),
                     (unsigned long long)g_pulse_expired.load(std::memory_order_relaxed),
                     (unsigned long long)g_pulse_wait_weapon.load(std::memory_order_relaxed),
                     g_head_always.load(std::memory_order_relaxed),
                     g_fp_auto.load(std::memory_order_relaxed),
                     (unsigned long long)g_fp_auto_fires.load(std::memory_order_relaxed),
                     !g_populate_fn.load(std::memory_order_relaxed) ? "NOT DERIVED"
                         : g_head_populate.load(std::memory_order_relaxed) ? "on" : "off",
                     (unsigned long long)g_populate_calls.load(std::memory_order_relaxed),
                     (unsigned long long)g_populate_ok.load(std::memory_order_relaxed),
                     (unsigned long long)g_populate_empty.load(std::memory_order_relaxed),
                     (unsigned long long)g_populate_fault.load(std::memory_order_relaxed),
                     (unsigned long long)g_populate_nolink.load(std::memory_order_relaxed));
        }
        // BUILD 121: the census, printed ONCE when it stops growing, so a
        // three minute run does not repeat the same table 18 times. The head
        // object is marked so the other rows can be read as "everything else
        // this setter can reach", which is where hands would appear.
        {
            static int  s_last_n = -1;
            static int  s_stable = 0;
            int n = 0;
            for (int i = 0; i < 16; ++i) if (grwxr_headhide_ring[i]) ++n;
            if (n != s_last_n) { s_last_n = n; s_stable = 0; }
            else if (n > 0 && s_stable >= 0 && ++s_stable == 3) {
                LOG_INFO("hide census: %d distinct object(s) reached this "
                         "setter. Rows marked HEAD matched the verified head "
                         "class; the rest are other visuals on the same "
                         "route, which is where hands and helmet would be.", n);
                for (int i = 0; i < 16 && grwxr_headhide_ring[i]; ++i) {
                    const bool is_head =
                        grwxr_headhide_table &&
                        grwxr_headhide_ring_cls[i] == grwxr_headhide_table;
                    LOG_INFO("hide census:   [%2d] obj=0x%012llX cls=0x%012llX%s",
                             i,
                             (unsigned long long)grwxr_headhide_ring[i],
                             (unsigned long long)grwxr_headhide_ring_cls[i],
                             is_head ? "   <<< HEAD (the class we force)" : "");
                }
                s_stable = -1;   // printed; do not print again
            }
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
                 "camASPCT skewX    skewY    | xmm1 range        xmm2 range        "
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
    g_headlatch_hook.restore();  // BUILD 224; impl stays published for a call in flight
    if (g_noblur_byte) {
        DWORD prot = 0;
        if (VirtualProtect(g_noblur_byte, 1, PAGE_EXECUTE_READWRITE, &prot)) {
            *g_noblur_byte = 0x01;
            DWORD dummy = 0;
            VirtualProtect(g_noblur_byte, 1, prot, &dummy);
            FlushInstructionCache(GetCurrentProcess(), g_noblur_byte, 1);
        }
        g_noblur_byte = nullptr;
    }
    g_setyaw_hook.restore();
    g_setpitch_hook.restore();
    for (auto& h : g_hooks) h.restore();
    g_any = false;
}

// Build 18: drive the head-hide override from the first-person state. Called
// from VRMirror's per-frame key poll; a plain aligned store, read by the asm
// entry on every engine SetHidden call.
// Build 34: the engine's re-assert is TRANSITION-driven, not continuous
// (build 18 headset run: zero setter calls for 12 s of FP-on play, then a
// burst at the first aim), so the passive override alone leaves the head
// visible until the first aim after FP on, and hidden until the first
// transition after FP off. Once the object is latched, apply the state
// directly on each toggle edge by calling the verified impl: an idempotent
// boolean flip with no lock, callee, allocation or global read (RE-notes,
// "The visibility setter"), so it is safe from the render thread. A
// concurrent engine burst can only re-write the same idempotent state.
// Before the first latch of the session (the engine has not called the
// setter yet, and no other route to the object is verified) the build 18
// behaviour stands: the head hides at the first aim or camera transition.
void set_head_hide(bool on) {
    // BUILD 225: head_always. His words 2026-09-09: "the head must be hidden
    // at all times". The force no longer waits for the first-person flag.
    if (g_head_always.load(std::memory_order_relaxed)) on = true;
    const uint32_t v = on ? 1u : 0u;
    const uint32_t prev = grwxr_headhide_on;
    grwxr_headhide_on = v;
    if (v != prev) {
        const uint64_t obj  = grwxr_headhide_obj;
        const uint64_t impl = grwxr_headhide_impl;
        if (obj && impl) {
            ((void (*)(void*, uint8_t))impl)((void*)obj, (uint8_t)v);
            grwxr_headhide_direct = grwxr_headhide_direct + 1;
        }
    }
}

// BUILD 224: the two cfg switches. Both hot; 0 restores build 223 for that
// half (the pre-latch hook stays installed but offers are ignored; the
// re-assert simply stops).
void set_head_prelatch(int on) {
    g_head_prelatch.store(on ? 1 : 0, std::memory_order_relaxed);
}

void set_head_reassert(int on) {
    g_head_reassert.store(on ? 1 : 0, std::memory_order_relaxed);
}

// BUILD 225.
void set_head_always(int on) {
    g_head_always.store(on ? 1 : 0, std::memory_order_relaxed);
}

void set_head_populate(int on) {
    g_head_populate.store(on ? 1 : 0, std::memory_order_relaxed);
}

void set_fp_auto(int on) {
    g_fp_auto.store(on ? 1 : 0, std::memory_order_relaxed);
}

void set_fp_user_off(int off) {
    g_fp_user_off.store(off ? 1 : 0, std::memory_order_relaxed);
}

void set_head_pulse(int frames) {
    uint32_t f = frames <= 0 ? 0u : (uint32_t)frames;
    if (f && f < 2) f = 2;
    if (f > 60) f = 60;
    g_head_pulse_frames.store(f, std::memory_order_relaxed);
}

// BUILD 224: THE RE-ASSERT, called from VRMirror's per-frame poll right after
// set_head_hide(). Every 6th call (about 12 Hz at 72 fps) while first person
// is on and an object is latched: re-check the entity tie as liveness, then
// read the array-1 children and write back any hidden bit the engine's
// show-all path cleared. No logging, no allocation, no engine call: a handful
// of SEH-guarded reads and at most n1 u16 writes, the same bit the engine's
// own setter writes. Three consecutive failures of either check drop the
// pointer (the flat mod's staleness rule, grwfp-24692.log), and the drain
// thread reports the drop.
void head_hide_frame() {
    if (!g_head_reassert.load(std::memory_order_relaxed)) return;
    static unsigned s_n = 0;
    if ((++s_n % 2) != 0) return;   // 226: every second frame, so cover cannot flash it
    const uint64_t obj = grwxr_headhide_obj;
    if (!obj || !grwxr_headhide_on || !grwxr_headhide_table) return;

    static uint64_t s_obj = 0;
    static int s_tie_bad = 0, s_ctl_bad = 0;
    if (obj != s_obj) { s_obj = obj; s_tie_bad = 0; s_ctl_bad = 0; }

    g_hr_checks.fetch_add(1, std::memory_order_relaxed);

    // Liveness: the object must still belong to the pinned player. With no
    // pin (menu, a world change) the tie is not judged, only the control.
    const uint64_t pent = g_player_entity.load(std::memory_order_relaxed);
    if (pent) {
        uint64_t ent = 0;
        // A faulting read or a DIFFERENT entity is a failure; a handle that
        // is transiently invalid (entity 0, a world or cutscene transition)
        // is not judged, so the pointer is not churned for nothing.
        if (!head_obj_entity(obj, &ent) || (ent && ent != pent)) {
            if (++s_tie_bad >= 3) {
                g_hr_last_drop.store(obj, std::memory_order_relaxed);
                grwxr_headhide_obj = 0;
                g_hr_tie_drops.fetch_add(1, std::memory_order_relaxed);
                s_obj = 0; s_tie_bad = 0; s_ctl_bad = 0;
            }
            return;
        }
        s_tie_bad = 0;
    }

    const int wrote = head_reassert(obj);
    if (wrote == -2) { s_ctl_bad = 0; return; }   // empty arrays: nothing to hold yet
    if (wrote < 0) {
        if (++s_ctl_bad >= 3) {
            g_hr_last_drop.store(obj, std::memory_order_relaxed);
            grwxr_headhide_obj = 0;
            g_hr_stale_drops.fetch_add(1, std::memory_order_relaxed);
            s_obj = 0; s_tie_bad = 0; s_ctl_bad = 0;
        }
        return;
    }
    s_ctl_bad = 0;
    if (wrote > 0)
        g_hr_rewrites.fetch_add((uint64_t)wrote, std::memory_order_relaxed);
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

// BUILD 289 (log only): the pitch of the engine camera base the compose reads (g_base row 1), geometric radians, up positive,
// and the setter call count, for the recoil recorder. Unsynchronized read, an error of a frame.
bool base_pitch_geo(float* out) {
    if (!out) return false;
    const float fx = g_base[3], fy = g_base[4], fz = g_base[5];
    if (!isfinite(fx) || !isfinite(fy) || !isfinite(fz)) return false;
    if (fx*fx + fy*fy + fz*fz < 0.5f) return false;
    float sp = fz;
    if (sp >  0.9999f) sp =  0.9999f;
    if (sp < -0.9999f) sp = -0.9999f;
    *out = asinf(sp);
    return true;
}
uint64_t aim_count(int axis) {
    return axis == 0 ? grwxr_setyaw_count : grwxr_setpitch_count;
}
// BUILD 292 (log only): the object the SetPitch stub was last called on (rcx), for the recoil recorder.
uint64_t aim_lastobj() { return grwxr_setpitch_lastobj; }

// 2026-09-01: the look-reversal probe's third column. The stub captures the
// engine's incoming value on every setter call; this returns it as the float
// it is. A stale read is one engine tick old, which a 1 Hz line cannot see.
float aim_lastval(int axis) {
    const uint32_t bits = axis == 0 ? grwxr_setyaw_lastval
                                    : grwxr_setpitch_lastval;
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
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
