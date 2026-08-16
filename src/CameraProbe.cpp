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

#include "GameBuild.h"

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
// Build 34: direct setter calls made on the FP toggle edge (instant hide).
volatile uint64_t grwxr_headhide_direct = 0;
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
// The setter's own body signature is PER BUILD (GameBuild head_setter_sig):
// the 2026-08 recompile re-registered it, so one constant cannot serve all
// three binaries. Unique, no rel32/rip operands (RE-notes + campaign doc).

hook::ThunkHook g_headhide_hook;
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


// Build 81: position, off by default so the verified rotation cannot regress
// behind it. clamp is the hard cap in metres on how far the gun may sit from
// the engine's own placement, which bounds every possible tracking failure.
std::atomic<int>   g_wgun_pos{0};
std::atomic<float> g_wgun_pos_scale{1.0f};
std::atomic<float> g_wgun_pos_clamp{1.0f};
std::atomic<float> g_wgun_pos_smooth{0.35f};

// Build 35: NO CAMERA BLUR (RE-notes "NO CAMERA BLUR, decoded from the
// community FP mod's cheat table", session 23). The community FP mod ships
// without the FP body blur; its cheat table's whole mechanism is one byte,
// the immediate of `mov sil, 1` forced to 0 inside the function that also
// compares the uncracked name hash 0x826846F3. Re-derived in our build:
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
            // Build 93: latch the rotation that this frame's tag advertises, so
            // every later call of the same frame composes with exactly it. See
            // the comment at g_frameH.
            memcpy(g_frameH, H, sizeof(g_frameH));
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
        const bool rot = headpose::cam_pose_rot();

        const float* B = g_base;
        float rebuilt[9];
        float cy = 0, cp = 0;
        if (rot && headpose::aim_cum(&cy, &cp)) {
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
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    out[r * 3 + c] = FH[r * 3 + 0] * B[0 + c]
                                   + FH[r * 3 + 1] * B[3 + c]
                                   + FH[r * 3 + 2] * B[6 + c];
                }
            }
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m[r * 4 + c] = out[r * 3 + c];
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
                            float dx = g_base[3], dy = g_base[4];
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
    if (index == kSelectorProbe) {
        const bool want_pose =
            headpose::cam_selector_pose() &&
            g_calls[kOnCalcMvpProbe].load(std::memory_order_relaxed) == 0;
        if (want_pose || !g_player_cam.load(std::memory_order_relaxed)) {
            const CamFields cs = read_camera(a->rcx);
            if (cs.valid && cs.mode == 0) {
                g_player_cam.store(a->rcx, std::memory_order_relaxed);
                if (want_pose) {
                    float H[9], Q[4];
                    if (headpose::read(H, Q)) {
                        if (write_pose_head(a->rcx, H, Q)) {
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

void snap_drain() {
    static int ticks = 0;
    ++ticks;

    // Build 91: FIRST, before any early return. The "player entity is live"
    // branch below ends in an unconditional return, which silently killed this
    // telemetry for the whole of every session once the entity latched.
    drain_head_telemetry(ticks);

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
            } else {
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

                                // Build 67: arm (or disarm) the gun-root
                                // write from this same verified tick. The
                                // node index is resolved from the rig's own
                                // name map rather than hardcoded, so a rig
                                // change disarms instead of writing a wrong
                                // bone. Skeleton pointer is published LAST:
                                // the stub reads it first and treats zero as
                                // disarmed, so a half-published state can
                                // never write.
                                if (g_wgun_hook.installed()) {
                                    const int gn = g_wgun_on.load(
                                        std::memory_order_relaxed)
                                        ? rig_find_node(rg,
                                              g_wgun_hash.load(
                                                  std::memory_order_relaxed))
                                        : -1;
                                    if (gn >= 0) {
                                        const float dz = g_wgun_dz_cfg.load(
                                            std::memory_order_relaxed);
                                        memcpy(&grwxr_wgun_dz, &dz,
                                               sizeof(grwxr_wgun_dz));
                                        grwxr_wgun_node = (uint32_t)gn;
                                        grwxr_wgun_skel = bestp;
                                    } else {
                                        grwxr_wgun_skel = 0;
                                    }
                                    LOG_INFO("wgun: %s node=%d %s dz=%.2f "
                                             "calls=%llu lifts=%llu rots=%llu "
                                             "pos=%llu | skip nopose=%llu "
                                             "nobuf=%llu norec=%llu noray=%llu "
                                             "nopos=%llu badq=%llu | two=%llu tworej=%llu roll=%llu",
                                             gn >= 0 ? "ARMED" : "idle", gn,
                                             grwxr_wgun_rot ? "ROTATE" : "lift",
                                             g_wgun_dz_cfg.load(
                                                 std::memory_order_relaxed),
                                             (unsigned long long)
                                                 grwxr_wgun_calls,
                                             (unsigned long long)
                                                 grwxr_wgun_writes,
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
                                             (unsigned long long)grwxr_wg_roll);

                                    // The off-hand measurement used to print
                                    // here. It moved to the top of this tick on
                                    // 2026-08-13, out of the wgun nest, because
                                    // sitting here made it require wgun armed,
                                    // which is exactly the condition that
                                    // contaminated it.
                                }
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
            LOG_INFO("aer: basepos step last=%.4f max=%.4f, vs-write last=%.4f max=%.4f",
                     g_diag_step_last, g_diag_step_max,
                     g_diag_vsw_last, g_diag_vsw_max);
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
bool base_pos(float out[3]) {
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

// Build 45: see CameraProbe.h. Same source and sanity rules as base_pos,
// plus a unit-length check on each rotation row, because a camera object
// mid-swap can hold a matrix that reads fine and means nothing. POD locals
// only, so SEH needs no unwinding.
bool base_frame(float rot[9], float pos[3]) {
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
void set_wgun_twohand(int on) {
    const int want = on ? 1 : 0;
    if (g_wgun_twohand.exchange(want, std::memory_order_relaxed) != want)
        g_wgun_gen.fetch_add(1, std::memory_order_relaxed);
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
extern "C" void grwxr_wgun_apply(void* skelp) {
    const uint64_t skel = (uint64_t)skelp;

    auto bump = [](volatile uint64_t* c) {
        _InterlockedIncrement64((volatile long long*)c);
    };

    uint64_t pose = 0, buf = 0;
    if (!read_block(skel + 0x238, &pose, sizeof(pose)) ||
        pose < 0x10000 || (pose & 7)) { bump(&grwxr_wg_nopose); return; }
    if (!read_block(pose + 0x178, &buf, sizeof(buf)) ||
        buf < 0x10000 || (buf & 7)) { bump(&grwxr_wg_nobuf); return; }

    const uint64_t rec = buf + (uint64_t)grwxr_wgun_node * 0x20;
    float bq[4], rq[4];
    if (!read_block(rec + 0x10, bq, sizeof(bq)) ||
        !read_block(pose + 0x10, rq, sizeof(rq))) {
        bump(&grwxr_wg_norec); return;
    }
    for (int i = 0; i < 4; ++i)
        if (!isfinite(bq[i]) || !isfinite(rq[i])) { bump(&grwxr_wg_badq); return; }

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
    float lpos[3], rpos[3];
    if (want_two &&
        aimtrace::ctrl_pos_l(lpos) && aimtrace::ctrl_pos(rpos)) {
        const float v[3] = {lpos[0] - rpos[0],
                            lpos[1] - rpos[1],
                            lpos[2] - rpos[2]};
        const float sep = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (sep > kTwoHandLo && isfinite(sep)) {
            const float th[3] = {v[0]/sep, v[1]/sep, v[2]/sep};
            // Agreement gate: if the hand-to-hand line disagrees wildly with
            // the rear controller's own forward, something is wrong (a
            // tracking pop, the off hand behind the shoulder, a controller
            // located to the floor). Refuse the frame rather than swing the
            // weapon through the player.
            const float agree = th[0]*ray[0] + th[1]*ray[1] + th[2]*ray[2];
            if (agree > kTwoHandAgree) {
                float w = (sep - kTwoHandLo) / (kTwoHandHi - kTwoHandLo);
                if (w > 1.0f) w = 1.0f;
                w = w * w * (3.0f - 2.0f * w);        // smoothstep
                float b[3] = {ray[0] + w * (th[0] - ray[0]),
                              ray[1] + w * (th[1] - ray[1]),
                              ray[2] + w * (th[2] - ray[2])};
                const float bn = sqrtf(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
                if (bn > 0.5f) {                       // both are unit, ~1
                    ray[0] = b[0]/bn; ray[1] = b[1]/bn; ray[2] = b[2]/bn;
                    grwxr_wg_two = grwxr_wg_two + 1;
                }
            } else {
                grwxr_wg_tworej = grwxr_wg_tworej + 1;
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

    // Where the barrel points right now: the bone's +Y in world space.
    const float ey[3] = {0, 1, 0};
    float fwd[3];
    qrot(worldQ, ey, fwd);
    const float fn = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    if (!(fn > 1e-6f)) { bump(&grwxr_wg_badq); return; }
    fwd[0] /= fn; fwd[1] /= fn; fwd[2] /= fn;

    // Shortest arc taking fwd onto the filtered target: q = (fwd x tgt, 1+dot),
    // normalised. The only degenerate case is an exact 180 degrees, where the
    // axis is undefined; we skip rather than pick an arbitrary one, because a
    // wrong axis there would roll the gun unpredictably and it resolves itself
    // on the next call as the filter moves the target off the antipode.
    const float c = fwd[0]*s_dir[0] + fwd[1]*s_dir[1] + fwd[2]*s_dir[2];
    if (c < -0.9999f) { bump(&grwxr_wg_badq); return; }
    float sw[4] = {fwd[1]*s_dir[2] - fwd[2]*s_dir[1],
                   fwd[2]*s_dir[0] - fwd[0]*s_dir[2],
                   fwd[0]*s_dir[1] - fwd[1]*s_dir[0],
                   1.0f + c};
    const float swn = sqrtf(sw[0]*sw[0] + sw[1]*sw[1] + sw[2]*sw[2] + sw[3]*sw[3]);
    if (!(swn > 1e-6f)) { bump(&grwxr_wg_badq); return; }
    for (int i = 0; i < 4; ++i) sw[i] /= swn;

    float desired[4];
    qmul(sw, worldQ, desired);               // swing applied in WORLD space

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
            const float t[3] = {s_dir[0], s_dir[1], s_dir[2]};
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
                float theta = atan2f(sgn, dot) +
                              g_wgun_roll_deg.load(std::memory_order_relaxed) *
                                  0.01745329f;

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
            }
        }
    }

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

    // BUILD 81: POSITION. Rotation alone pivots the gun where the animation
    // put it, so you can point it but not raise it to your eye. This puts the
    // gun-root ON the controller.
    //
    // Off by default and separate from the mode, so rotation-only and
    // rotation-plus-position are one cfg line apart and a regression in this
    // half cannot cost the half that is already verified.
    if (!g_wgun_pos.load(std::memory_order_relaxed)) return;

    float cp[3];
    if (!aimtrace::ctrl_pos(cp)) { bump(&grwxr_wg_nopos); return; }
    for (int i = 0; i < 3; ++i)
        if (!isfinite(cp[i])) { bump(&grwxr_wg_nopos); return; }

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
    const float lim = g_wgun_pos_clamp.load(std::memory_order_relaxed);
    float dv[3] = {want[0] - engw[0], want[1] - engw[1], want[2] - engw[2]};
    const float dl = sqrtf(dv[0]*dv[0] + dv[1]*dv[1] + dv[2]*dv[2]);
    if (dl > lim && dl > 1e-6f) {
        const float k = lim / dl;
        want[0] = engw[0] + dv[0] * k;
        want[1] = engw[1] + dv[1] * k;
        want[2] = engw[2] + dv[2] * k;
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
        if ((++hide_ticks % 10) == 4) {
            LOG_INFO("hide: %s, setter calls=%llu forced=%llu direct=%llu "
                     "obj=0x%012llX",
                     grwxr_headhide_on ? "FORCING (fp on)" : "idle (fp off)",
                     (unsigned long long)grwxr_headhide_calls,
                     (unsigned long long)grwxr_headhide_forced,
                     (unsigned long long)grwxr_headhide_direct,
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
