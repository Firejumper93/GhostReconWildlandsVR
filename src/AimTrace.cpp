// AimTrace.cpp - build 50. See AimTrace.h for the question and the method.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cmath>
#include <cstring>

#include "AimTrace.h"
#include "GameBuild.h"
#include "Log.h"
#include "ThunkHook.h"
#include "Sig.h"           // build 136: SetShotLine is found, never pinned
#include "CameraProbe.h"   // build 106: base_pos, the world-space control
#include "D3D11Hook.h"     // build 302: frame_count, the shot ray recorder's window
#include "Voice.h"         // build 107: spoken test instructions

// --- shared with ProbeStub.asm ---------------------------------------------
//
// The asm entries save the argument registers, call the recorder with the
// accessor index, the caller's return address and the object, restore
// everything, and then re-emulate the stub's own dispatch:
//
//     mov rax,[rcx] ; mov rdx,[rax+disp] ; jmp rdx
//
// The dispatch offsets are published here from the VERIFIED slot bytes, never
// hardcoded, so a build whose vtable index differs cannot be dispatched wrong:
// the slot bytes would not match and nothing would be installed at all.
extern "C" {
uint32_t grwxr_aimget_disp[2] = {0, 0};   // [0] = yaw getter, [1] = pitch
// Build 52: the last value the override actually saw and shipped, as float
// bits: [0] = what the real accessor returned, [1] = what we handed back.
// Written by the asm override path, read by drain(). This exists because
// build 51's negative could not distinguish "the engine ignores the value"
// from "our addition never happened", and those need opposite next steps.
uint32_t grwxr_aimget_vals[2] = {0, 0};
void grwxr_aimget_yaw_entry();
void grwxr_aimget_pitch_entry();
// Returns the float bits of a delta to ADD to whatever the real accessor
// returns, or 0 for "change nothing". 0 is also the bit pattern of 0.0f, so
// the two meanings agree and the asm can branch on a plain test.
uint32_t grwxr_aimget_record(uint64_t index, uint64_t ret_addr, uint64_t obj);

// Build 17/19 (CameraProbe.cpp): the object the engine's LOOK INPUT last wrote
// its integrated yaw to. That is the player's own aim angle, and it is what
// makes this census answerable at all: these dispatch stubs are GENERIC (they
// call vtable slot +0x5B0 / +0x5F0 on whatever object they are handed), so
// without an object filter the table would fill with unrelated classes.
extern volatile uint64_t grwxr_setyaw_lastobj;

// Build 53: the per-shot weapon routine observer (see ProbeStub.asm).
uint64_t grwxr_wfire_orig = 0;
void grwxr_wfire_entry();
void grwxr_wfire_record(uint64_t ret_addr, uint64_t self, uint64_t ctx,
                        uint32_t f1bits);

// Build 54: the Havok world raycast census (see ProbeStub.asm).
uint64_t grwxr_ray_orig = 0;
void grwxr_ray_entry();
void grwxr_ray_record(uint64_t ret_addr, const float* ray);

// Build 58: TtCastRay, the raycast body behind the wrapper's virtual call,
// hooked at its own thunk. Shares grwxr_ray_record, so its callers land in
// the same census and shot window as the wrapper's.
uint64_t grwxr_ray2_orig = 0;
void grwxr_ray2_entry();

// Build 55: GetAimOrientation (see ProbeStub.asm). post() rotates the
// quaternion the engine just produced, in the caller's own buffer.
uint64_t grwxr_aimq_orig = 0;
void grwxr_aimq_entry();
void grwxr_aimq_post(float* q, uint64_t ret_addr);

// BUILD 105: the anchor world getters (see ProbeStub.asm).
uint64_t grwxr_aimpt_orig = 0;
void grwxr_aimpt_entry();
void grwxr_aimpt_post(float* out, uint64_t iface);
uint64_t grwxr_muzpt_orig = 0;
void grwxr_muzpt_entry();
void grwxr_muzpt_post(const float* out, uint64_t iface);

// Build 56: the ballistic projectile spawn (see ProbeStub.asm).
uint64_t grwxr_spawn_orig = 0;
void grwxr_spawn_entry();
void grwxr_spawn_pre(void* owner);
void grwxr_spawn_post(void* owner, void* proj);

// BUILD 136: SetShotLine (see ProbeStub.asm). One call per shot.
//
// BUILD 137: `dir` loses its const. The stub saves r8, calls this, restores r8
// and calls the original with the SAME pointer, so a write through it here is
// what the engine goes on to use. That is why the direction rewrite needs no
// change to ProbeStub.asm at all: the buffer was always being passed through,
// only nobody had written to it.
uint64_t grwxr_shotline_orig = 0;
void grwxr_shotline_entry();
void grwxr_shotline_pre(uint64_t ctx, const float* org, float* dir);

// BUILD 154: the two trajectory generators. Observers in the stub; the bodies
// are kept because the correction re-invokes them with one argument replaced.
uint64_t grwxr_gen_a_orig = 0;
uint64_t grwxr_gen_b_orig = 0;
void grwxr_gen_a_entry();
void grwxr_gen_b_entry();
void grwxr_gen_record(void* bal, float elev, void* arc, uint32_t flag,
                      uint32_t which);
}

namespace grwxr {
namespace aimtrace {
namespace {

// The two getter dispatch stubs, byte-exact. Both are `mov rax,[rcx];
// mov rdx,[rax+disp]; jmp rdx` in an int3-padded 16-byte slot, verified in
// BOTH shipped binaries 2026-08-06 (tools/sig_scan.py: exactly one match each,
// the Steam control landing on the RVA docs/RE-notes.md documents).
//
// Note the shape differs from the SETTERS' stubs, which are 10 bytes and jump
// through memory (`jmp qword ptr [rax+disp]`). These load into rdx first and
// are 13 bytes. The 14-byte patch still fits inside the int3 padding, and
// install_raw refuses to write unless it has verified that padding.
constexpr uint8_t kGetYawExpect[13] = {
    0x48, 0x8B, 0x01,                            // mov rax, [rcx]
    0x48, 0x8B, 0x90, 0xB0, 0x05, 0x00, 0x00,    // mov rdx, [rax+0x5B0]
    0x48, 0xFF, 0xE2};                           // jmp rdx
constexpr uint8_t kGetPitchExpect[13] = {
    0x48, 0x8B, 0x01,
    0x48, 0x8B, 0x90, 0xF0, 0x05, 0x00, 0x00,    // mov rdx, [rax+0x5F0]
    0x48, 0xFF, 0xE2};
// The dispatch offset sits at +6 in both, as a little-endian dword.
constexpr size_t kDispAt = 6;

constexpr int kMaxRows = 48;

struct Row {
    uint32_t rva;
    uint32_t index;      // 0 = yaw getter, 1 = pitch getter
    uint64_t n;          // calls from this site on the player's aim object
    uint64_t fire;       // ...of those, calls made while the trigger was held
};

volatile Row      g_rows[kMaxRows] = {};
volatile int      g_row_count = 0;
volatile uint32_t g_firing    = 0;     // set by set_firing from the merge

// ---------------------------------------------------------------------
// BUILD 105: THE AIMING-POINT A/B.
//
// GR_cWeaponComponent's three anchors are BoneHandles, so they cannot be
// moved. What CAN be substituted is the WORLD POINT each resolves to, which
// is what the two hooked getters return.
//
// The question this exists to answer, and it needs no instrumentation in the
// headset: fire ten single shots at a wall and see whether you get ONE group
// of holes or TWO. We alternate the returned aim point 5 m left, then 5 m
// right, on successive trigger pulls. A constant misalignment cannot
// alternate, so two groups prove causation and one group is a clean
// negative. This is the build 51 trick, chosen because the tester has no
// usable crosshair and therefore no reference for "is it offset".
//
// Deliberately NOT touched: no camera read, no aim setter, no aim_pump, no
// g_aim_cum. The camera-hijack constraint is respected by construction.
//
// OWNER GATE IS MANDATORY. The class is shared with AI, which is exactly why
// m_bPlayerOrReplica exists at +0xEA. Without the gate every NPC in the
// province inherits our aim.
volatile uint32_t g_ap_seen     = 0;   // aim getter calls observed
volatile uint32_t g_ap_player   = 0;   //   of those, owner gate passed
volatile uint32_t g_ap_ai       = 0;   //   of those, refused as not-player
volatile uint32_t g_ap_written  = 0;   // returns actually rewritten
volatile uint32_t g_ap_shots    = 0;   // trigger pulls seen by the A/B
volatile uint32_t g_ap_side     = 0;   // 0 = left next, 1 = right next
volatile uint32_t g_ap_armed    = 0;   // cfg aimpoint_ab
volatile uint32_t g_ap_prev_fire= 0;
float    g_ap_shift_m  = 5.0f;         // cfg aimpoint_ab_m
float    g_ap_last[3]  = {0,0,0};      // last aim point the engine produced
float    g_ap_muzzle[3]= {0,0,0};      // last muzzle point, from the twin
volatile uint32_t g_ap_have_muz = 0;

// ---------------------------------------------------------------------------
// BUILD 136: SETSHOTLINE, the one call per shot that carries the whole ray.
//
// Deliberately a LAPPING RING, not a fill-and-stop table. Build 135's capture
// printed "TABLE FULL" 665 times in one 47 minute run, which is what a table
// that stops accepting looks like from the log: the first four shots of the
// session and then silence for the rest of it. A ring that overwrites keeps
// the MOST RECENT shots, which are the ones a tester is producing on purpose
// while watching for the log line, and g_sl_calls records the true total so a
// lap is visible rather than silent.
//
// Rule 8: the recorder does stores and nothing else. Every interpretation
// (vector lengths, the distances to muzzle and camera) happens in the drain.
struct ShotLine {
    uint64_t ctx, orgp, dirp;
    float    org[4], dir[4];
    float    ndir[3];                  // build 137: what we wrote, if we wrote
    float    dpm;                      // build 137: origin to the player muzzle
    uint32_t why;                      // build 137: kSd* below
    uint32_t vtwhy;                    // build 154: kSp*, the vertical
    float    vt_eng;                   // build 154: the elevation the engine used
    float    vt_our;                   // build 154: the barrel elevation we sent
    float    vt_fromdir;               // build 154: asin of the engine's own dir,
                                       // the convention check: it should equal
                                       // vt_eng, and if it is the negative of it
                                       // our sign is inverted and the log says so
                                       // on the very first shot
    uint32_t seq;
};
constexpr uint32_t kSlSlots = 16;      // power of two, masked not modulo
ShotLine g_sl[kSlSlots] = {};
volatile uint32_t g_sl_w      = 0;     // monotonic write counter
volatile uint32_t g_sl_calls  = 0;     // every call, including lapped ones
volatile uint32_t g_sl_printed= 0;     // how far the drain has reported

// ---------------------------------------------------------------------------
// BUILD 137: THE DIRECTION WRITE. See AimTrace.h for the argument; this block
// is the state and the reason codes.
//
// Every refusal gets its OWN code, and the code is printed on the shot's own
// log line. That is deliberate: the recurring failure in this project is a
// feature that was never actually armed reporting a zero that reads exactly
// like a feature that ran and did nothing (shots=0 across 494 logs meant the
// A/B had never been switched on, and nobody could tell from the log). A shot
// that was not rewritten now says which of the interlocks refused it.
constexpr uint32_t kSdWrote    = 0;    // the direction is now the barrel's
constexpr uint32_t kSdOff      = 1;    // cfg shot_dir = 0
constexpr uint32_t kSdNoPtr    = 2;    // the engine handed us a null
constexpr uint32_t kSdNoMuzzle = 3;    // no player muzzle has ever resolved
constexpr uint32_t kSdNotOurs  = 4;    // origin is not at the player's muzzle
constexpr uint32_t kSdStale    = 5;    // the weapon writer is not running
constexpr uint32_t kSdBadVec   = 6;    // degenerate or non-finite
constexpr uint32_t kSdFault    = 7;    // a read faulted; nothing was written
constexpr uint32_t kSdWroteOrg = 8;    // BUILD 152: origin AND direction; the
                                       // engine had moved the origin to the
                                       // camera and it was moved back

const char* sd_why(uint32_t w) {
    switch (w) {
        case kSdWrote:    return "WROTE";
        case kSdOff:      return "off";
        case kSdNoPtr:    return "nullptr";
        case kSdNoMuzzle: return "no-player-muzzle";
        case kSdNotOurs:  return "not-at-our-muzzle";
        case kSdStale:    return "barrel-stale";
        case kSdBadVec:   return "bad-vector";
        case kSdWroteOrg: return "WROTE-ORG";
        default:          return "faulted";
    }
}

// How close the shot origin must sit to the player's own muzzle to be treated
// as the player's own shot. Build 136 measured 0.019 to 0.020 m for hip fire
// across five shots, and 0.896 to 0.913 m for ADS (whose origin is the camera).
// 0.35 m is seventeen times the hip-fire figure and less than half the ADS one,
// so it separates the two cleanly and still leaves an NPC needing to fire with
// its barrel inside a 35 cm sphere around our own muzzle to be mistaken for us.
constexpr float kSdGateM = 0.35f;

// BUILD 152: the relocated-origin signature. The 03:05 session (grwxr-25048)
// measured the two clusters this gate separates: shots whose origin the
// engine had relocated to the camera sat at dOrgCam 0.31 to 0.55 m, and
// healthy muzzle-origin VR hip shots at 0.68 to 1.05 m (the camera is at the
// head, the gun at arm's length). 0.60 splits them with margin both ways. An
// AI shot's origin is at the AI's own muzzle, metres from this camera.
constexpr float kSdCamGateM = 0.60f;

volatile uint32_t g_sd_armed    = 1;   // cfg shot_dir. ON unless turned off.
volatile uint32_t g_sd_live     = 0;   // did the weapon writer publish recently
volatile uint32_t g_sd_gen_prev = 0;   // the drain's own liveness baseline
volatile uint32_t g_sd_wrote_n  = 0;   // shots rewritten, all time
volatile uint32_t g_sd_left_n   = 0;   // shots left as the engine built them
volatile uint32_t g_sd_said     = 0;   // the one spoken confirmation, once
// BUILD 152: the origin write. See AimTrace.h at set_shot_org for the design.
volatile uint32_t g_sd_org_armed = 1;  // cfg shot_org. ON unless turned off.
volatile uint32_t g_sd_worg_n    = 0;  // shots repaired origin-and-direction
volatile uint32_t g_sd_said_org  = 0;  // its one spoken confirmation

// ---------------------------------------------------------------------------
// BUILD 154: THE VERTICAL.
//
// SHOT-2026-09-02: SetShotLine reads [r8] and [r8+4] and NEVER [r8+8]. The
// engine is z-up, so our written elevation has never reached the shot. It is
// taken instead from the direction one step earlier, decomposed to a scalar in
// DEGREES, and handed to a trajectory generator that builds the arc BEFORE
// SetShotLine runs. SetShotLine then spins that finished arc through a
// yaw-only frame. Pitch is read before our write, yaw after it, which is
// exactly the measured asymmetry and is not a bug in our half of the chain.
//
// THE CORRECTION, AND WHY IT IS SHAPED THIS WAY. The generators are hooked as
// pure observers (ProbeStub.asm) because at generator time there is no origin
// and no weapon pointer, so none of the interlocks that keep AI fire untouched
// can run there. Instead the recorded arguments are stashed, and the arc is
// rebuilt from grwxr_shotline_pre AFTER sd_apply has already ruled the shot
// ours by the same gate that has protected AI since build 137. The rebuild
// calls THE ENGINE'S OWN generator with its own saved arguments and only the
// elevation replaced, so no ballistics are reimplemented here.
//
// The hard baseline governs this: it moves the round, so it ships with the ray
// probe on and is re-proven in the headset, never assumed.
constexpr uint32_t kVtWrote  = 0;   // the arc was rebuilt on the barrel angle
constexpr uint32_t kVtOff    = 1;   // cfg shot_pitch = 0
constexpr uint32_t kVtNotOur = 2;   // sd_apply did not rule this shot ours
constexpr uint32_t kVtNoArc  = 3;   // no generator call matched this shot ctx
constexpr uint32_t kVtStale  = 4;   // the weapon writer is not running
constexpr uint32_t kVtBadVec = 5;   // degenerate or non-finite barrel
constexpr uint32_t kVtNoFn   = 6;   // the generator hook never installed
constexpr uint32_t kVtFault  = 7;   // the rebuild faulted; arc left as built

const char* vt_why(uint32_t w) {
    switch (w) {
        case kVtWrote:  return "PITCH";
        case kVtOff:    return "off";
        case kVtNotOur: return "not-ours";
        case kVtNoArc:  return "no-arc";
        case kVtStale:  return "barrel-stale";
        case kVtBadVec: return "bad-vector";
        case kVtNoFn:   return "no-generator";
        default:        return "faulted";
    }
}

// One recorded generator call. Rule 8: the recorder does plain stores into a
// fixed ring and nothing else.
struct GenCall {
    uint64_t ctx;      // r8 - 0x170, the shot context SetShotLine also receives
    uint64_t bal;      // rcx, the ballistic data
    uint64_t arc;      // r8, where the arc is written
    uint32_t flag;     // r9d, the drop-table flag
    float    elev;     // xmm1, the ENGINE's own launch elevation, degrees
    uint32_t which;    // 0 = generator A, 1 = generator B
};

constexpr int kGenSlots = 16;          // power of two, indexed by a free counter
GenCall           g_gen[kGenSlots] = {};
volatile uint32_t g_gen_w     = 0;     // write cursor, free-running
volatile uint32_t g_gen_calls = 0;     // every generator call, all time

volatile uint32_t g_vt_armed   = 1;    // cfg shot_pitch. ON unless turned off.
volatile uint32_t g_vt_wrote_n = 0;    // arcs rebuilt on the barrel angle
volatile uint32_t g_vt_said    = 0;    // its one spoken confirmation

// The generator bodies, for the rebuild call. Its shape is settled by the ABI:
// arg 2 being a float puts it in xmm1, which is where the disassembly shows
// the elevation arriving, so this prototype is the register layout rather than
// a guess about it.
typedef void (*gen_fn)(void* bal, float elev_deg, void* arc_out, uint32_t flag);

// The player's OWN muzzle. g_ap_muzzle above is "most recent, any owner", which
// is right for the A/B's lateral axis and wrong for a gate that decides whether
// to move a bullet, because the muzzle getter runs for AI weapons too.
float             g_sd_muz[3] = {0, 0, 0};
volatile uint32_t g_sd_muz_ok = 0;

// BUILD 133: THE HYBRID SHOOT SYSTEM, read out of the live game.
//
// WHY THIS EXISTS. The shipped archives say GR_DBWeaponConstants
// (class 0x05A8E222) carries
//     v_bEnableHybridShootSystem      1
//     v_fMinAngleToCancelFocusPoint   120.0
//     v_fMinAngleToShootFromCamera    60.0
// byte-exact on two independent records authored nine years apart (the base
// record closes 459 of 459, the patch record 543 of 543, and they agree on
// every field they share). If that system is live, the engine is entitled to
// take the shot from the CAMERA instead of the weapon once the angle between
// them passes a threshold. In third person the camera and the gun are nearly
// aligned so 60 degrees is unreachable. In VR you reach it by turning your
// head, which is the exact condition the owner describes in CX-002, and
// CX-006 is "the bullet does not go where the gun is pointing". Nothing
// anywhere in this mod knows this system exists.
//
// THE ROUTE. Every offset below was re-derived from the INSTALLED GRW.exe's
// reflection records (a record's field offset is dword[rec_rva+0x0C] >> 18),
// not borrowed from a report and not from another binary:
//     weapon = iface - 0x80   is GR_cWeaponComponent (0x1B15F6FA, size 0x260)
//       +0x0A8  m_AssociatedEssence  -> WeaponEssence          (kind 18, ptr)
//                 +0x058  m_WeaponConstants -> GR_DBWeaponConstants
//       +0x128  sGlaringScopeSubComponent                (kind 20, sub-object)
//                 +0x020  m_WeaponConstants -> GR_DBWeaponConstants
//     db +0x13C u8 enable, +0x140 float cancel-focus, +0x144 float from-camera
//
// The class identification is not assumed. GR_cWeaponComponent is the ONLY
// class in the whole 5,439-class registry carrying m_bPlayerOrReplica at
// +0xEA, which is the byte the owner gate above already reads. It is
// corroborated a second way: the registry says +0xB0/+0xC0/+0xD0 are three
// 16-byte BoneHandles, which is exactly the 48 raw bytes build 106 already
// dumps from +0xB0.
//
// TWO ROUTES ON PURPOSE. GR_DBWeaponConstants has exactly ONE instance in the
// entire install, so both routes must land on the SAME pointer. Agreement is
// the self-check. Disagreement is a real finding and is reported as one
// rather than averaged away.
//
// Rule 8: plain stores only, no logging, no allocation, no lock, no COM. It
// is ONE-SHOT, so after the first success it costs one load and one branch
// per call. It runs in its OWN __try so a fault here cannot skip the aim path
// that wraps it.
volatile uint32_t g_wc_state   = 0;    // 0 not tried, 1 captured, 2 faulted
// BUILD 135: the entry claim, separate from g_wc_state because drain() reads
// g_wc_state to decide whether there is anything to print. Claiming through
// the state word would publish "captured" before the capture had run.
volatile long     g_wc_claim   = 0;    // 0 unclaimed, 1 a thread is inside
volatile uint64_t g_wc_weapon  = 0;    // GR_cWeaponComponent
volatile uint64_t g_wc_essence = 0;    // WeaponEssence, via +0xA8
volatile uint64_t g_wc_db_a    = 0;    // datablock via the essence route
volatile uint64_t g_wc_db_b    = 0;    // datablock via the scope route
volatile uint32_t g_wc_hybrid  = 0xFFFFFFFFu;   // v_bEnableHybridShootSystem
float    g_wc_cancel_deg = -1.0f;      // v_fMinAngleToCancelFocusPoint
float    g_wc_camera_deg = -1.0f;      // v_fMinAngleToShootFromCamera
volatile uint32_t g_wc_printed = 0;

// BUILD 106.1: the census is PER WEAPON, not one global snapshot. Three
// reasons, all of which would have made the last run thin:
//
//  1. The tester had TWO weapons out. Capturing "the first component we see"
//     captures an arbitrary one of them, and there is no way afterwards to
//     tell which.
//  2. The muzzle getter had not run yet when the single snapshot was taken,
//     so the muzzle column was zeros and the aim-versus-muzzle comparison,
//     which is the entire point of having both hooks, was empty.
//  3. Pairing aim and muzzle by nothing but "most recent" compares two
//     different weapons the moment more than one exists.
//
// So: a small table keyed on the interface pointer, each entry holding that
// weapon's own raw bytes, its own aim point and its own muzzle point, printed
// once it has both. Four slots is plenty for two carried weapons plus a
// sidearm, and the table reports if it fills, per the measurement rule that a
// probe must state its own capacity limit.
struct ApSlot {
    uint64_t iface;
    uint8_t  raw[48];      // weapon+0xB0 .. +0xDF
    float    aim[4];
    float    muz[4];
    uint32_t have_aim;
    uint32_t have_muz;
    uint32_t player;
    uint32_t printed;
};
constexpr int kApSlots = 4;
ApSlot   g_ap_slot[kApSlots] = {};
volatile uint32_t g_ap_slots_used = 0;
volatile uint32_t g_ap_slots_full = 0;   // a weapon we had no room for
// Build 107: the guided protocol. 0 idle, 1 arm requested, 2 running.
volatile uint32_t g_guide_state = 0;
// 2026-09-01: cfg voice_guide, default 0. Whether the protocol ANNOUNCES
// itself once per session. The test itself is unchanged and End still starts
// it; this only stops it inviting the tester into a different procedure than
// the one they are actually running.
volatile uint32_t g_voice_guide = 0;
uint32_t g_guide_ticks = 0;
uint32_t g_guide_done  = 0;

// Find or create the slot for one weapon. Called from both post handlers, on
// engine threads, so it allocates nothing and takes no lock: the table is
// append-only and a torn read costs at worst one duplicated log line.
ApSlot* ap_slot_for(uint64_t iface) {
    const uint32_t used = g_ap_slots_used;
    for (uint32_t i = 0; i < used && i < (uint32_t)kApSlots; ++i)
        if (g_ap_slot[i].iface == iface) return &g_ap_slot[i];
    if (used >= (uint32_t)kApSlots) { g_ap_slots_full = g_ap_slots_full + 1;
                                      return nullptr; }
    ApSlot* s = &g_ap_slot[used];
    s->iface = iface;
    g_ap_slots_used = used + 1;
    return s;
}
// ---------------------------------------------------------------------

volatile uint64_t g_calls[2]  = {0, 0};   // every call through each stub
volatile uint64_t g_other     = 0;     // ...on some other object (not ours)
volatile uint64_t g_nolatch   = 0;     // ...before the player object was known
volatile uint64_t g_dropped   = 0;     // ...after the table filled
volatile uint64_t g_foreign   = 0;     // callers outside GRW.exe (expect 0)

uintptr_t g_image_base = 0;
uintptr_t g_image_end  = 0;
const uint8_t* g_base  = nullptr;   // GRW.exe, for late site re-verification

// --- build 51: the per-shot override -----------------------------------
//
// Both sites are RETURN addresses inside the per-shot function, verified at
// install to sit directly after a `call` that resolves to the matching getter
// stub. When a delta is armed, the accessor's result is adjusted FOR THAT ONE
// CALLER ONLY: the camera, the look-input integrator and every other reader
// still get the true angle, which is what keeps the standing constraint (the
// controller must not move the view) intact by construction.
uint32_t g_shot_yaw_site   = 0;    // 0 = not verified, override refuses
uint32_t g_shot_pitch_site = 0;
volatile uint32_t g_shot_dyaw   = 0;   // float bits, radians, 0 = off
volatile uint32_t g_shot_dpitch = 0;
volatile uint64_t g_shot_overrides = 0;   // times a delta was actually applied

// ALTERNATING MODE, and it is what makes this testable at all. The user has
// no crosshair, so "is the impact offset from where I aimed" has no reference
// to be offset FROM. Flipping the sign on every shot removes the need for one:
// consecutive rounds land either side of wherever the gun was pointed, so the
// question becomes "one hole or two", which needs no reticle, no ADS overlay
// and no A/B config edit mid-session. The alternation is also what proves
// CAUSATION rather than a fixed misalignment that was always there.
volatile uint32_t g_shot_alternate = 0;
volatile uint64_t g_shot_seq       = 0;   // rounds seen at the yaw site

hook::ThunkHook g_getyaw_hook;
hook::ThunkHook g_getpitch_hook;

// Build 53: the per-shot weapon routine's callers. Same shape as the aim
// census: a small fixed table, unsynchronised, counted per caller.
hook::ThunkHook g_wfire_hook;
constexpr int kMaxWf = 16;
struct WfRow { uint32_t rva; uint64_t n; };
volatile WfRow    g_wf[kMaxWf] = {};
volatile int      g_wf_count = 0;
volatile uint64_t g_wf_calls  = 0;
volatile uint64_t g_wf_self   = 0;   // last `this`
volatile uint64_t g_wf_ctx    = 0;   // last context argument
volatile uint32_t g_wf_f1     = 0;   // last float argument, bits

// Build 54: hknpWorld::castRay callers. There are only eight in the image,
// so 12 rows is generous. Each row keeps the last ray input it saw, which is
// how we read the origin and direction once the shot's caller is identified.
hook::ThunkHook g_ray_hook;
// Build 58: the direct-entry thunk to the same raycast machinery.
hook::ThunkHook g_ray2_hook;
hook::ThunkHook g_aimpt_hook;
hook::ThunkHook g_muzpt_hook;
constexpr int kMaxRay = 12;
struct RayRow {
    uint32_t rva;
    uint64_t n;
    float    v[8];        // first 32 bytes of the ray input struct
};
volatile RayRow   g_ray[kMaxRay] = {};
volatile int      g_ray_count = 0;
volatile uint64_t g_ray_calls = 0;

// BUILD 302: THE SHOT RAY RECORDER (log only, his priority 2026-10-12: "the head orientation controls the bullet trajectory
// again"). The 301 log proved the shot leaves SetShotLine on the barrel while the engine's own direction followed the head
// 35 deg away, so the pull is downstream: the trace (TraceSegment builds the ray from ctx+0x190 to ctx+0x1a0 and casts it,
// HANDOFF-2026-08-28 section 3) or a second cast from the camera. Every raycast whose origin is within kSrNearM of the
// player's muzzle, in the kSrFrames Presents after one of his shots, is stored with that shot's number; the drain prints
// each against the barrel we wrote and the engine's head direction. Rule 8: plain stores, one atomic frame read, no calls.
struct ShotRay { uint32_t tag; uint32_t rva; uint64_t frame; float v[8]; };
constexpr int      kSrSlots  = 256;            // power of two
constexpr uint64_t kSrFrames = 10;             // Presents after a shot during which rays are recorded (the trace fires on the next tick)
constexpr float    kSrNearM  = 4.0f;           // metres from the muzzle the ray's origin must be within
constexpr uint32_t kSrPerShot = 96;            // rays kept per shot, the FIRST ones (the ring must never lap over the first segment)
ShotRay            g_sr[kSrSlots] = {};
volatile uint32_t  g_sr_w       = 0;           // write cursor, free-running
volatile uint32_t  g_sr_printed = 0;
volatile uint64_t  g_sr_shot_frame = 0;        // Present count at the last SetShotLine of HIS shot (0 = never)
volatile uint32_t  g_sr_tag_n   = 0;           // rays stored for the current shot tag
volatile uint32_t  g_sr_tag     = 0;           // the tag g_sr_tag_n counts for
volatile uint64_t  g_sr_seen    = 0;           // rays inside the window, before the distance test

// Build 55: the aim orientation quaternion override. The rotation is
// PRECOMPUTED when the cfg changes, so the hot path is a quaternion multiply
// and four stores: no sin, no cos, no branch beyond the armed test.
hook::ThunkHook   g_aimq_hook;
volatile uint32_t g_aq_armed = 0;
volatile float    g_aq_rot[4] = {0, 0, 0, 1};   // x, y, z, w
volatile uint64_t g_aq_calls = 0;
volatile uint64_t g_aq_applied = 0;
volatile float    g_aq_last[4] = {0, 0, 0, 0};  // last quaternion we shipped

// Build 56: WHO CONSUMES THE AIM QUATERNION. The build 55 run proved
// something does: at 15 degrees the MUZZLE FLASH moved off to the side while
// the bullets did not. So one of these callers is the weapon effect, and a
// caller whose count matches the rounds fired is the FIRE PATH itself, which
// is where the projectile is spawned.
constexpr int kMaxAq = 16;
struct AqRow { uint32_t rva; uint64_t n; };
volatile AqRow g_aq_rows[kMaxAq] = {};
volatile int   g_aq_rowcount = 0;

// Build 56: THE BULLET. The spawn copies [owner+0x140] into the projectile's
// m_vBulletSimulationDirection and [owner+0x150] into m_vBulletShootOrigin
// (both verified byte for byte in the disassembly). Rewriting the direction
// for the duration of the call, then restoring it, steers the round without
// leaving any engine field modified afterwards.
hook::ThunkHook   g_spawn_hook;
volatile uint64_t g_sp_calls = 0;
volatile uint64_t g_sp_turned = 0;
volatile float    g_sp_dir[4] = {0, 0, 0, 0};    // last direction seen
volatile float    g_sp_org[4] = {0, 0, 0, 0};    // last origin seen
volatile uint32_t g_sp_armed = 0;
volatile float    g_sp_cos = 1.0f, g_sp_sin = 0.0f;
// Saved copy so post() can put the engine's own value back verbatim.
volatile float    g_sp_saved[4] = {0, 0, 0, 0};
volatile uint32_t g_sp_didwrite = 0;

// Build 57: CORRELATE THE RAYCASTS WITH THE SHOT.
//
// The bullet's own direction is not what decides the hit, so the damage is
// resolved somewhere else, and the reflection decode points at a SLICED RAY
// (m_vBulletSegmentBegin / m_vBulletSegmentEnd driven by v_fFirstRayRange,
// v_fOptimalRangeRaySlicer, v_fMaxBallisticRayLen). If that is right, firing
// one round produces a BURST of world raycasts from one caller.
//
// The build 54 census could not see that because it only had lifetime totals,
// and the ambient traffic (wheels, AI sight) is in the tens of thousands. So
// keep a ring of recent caller RVAs, mark the ring position at every
// projectile spawn, and count what happened either side of that mark. A
// shot-specific caller spikes in the window; ambient traffic does not.
// The ring must outlive the 2 s drain interval that harvests it: gameplay
// runs ~2700 casts/s (measured, first live run), so 256 entries wrapped ~20x
// before every harvest and the wrap guard discarded every window. 16384
// covers ~6 s at that rate for 64 KB, and the hot path stays two stores.
constexpr int kRing = 16384;
volatile uint32_t g_ring[kRing] = {};
volatile uint32_t g_ring_idx = 0;
// Per-caller tallies inside the windows, accumulated over every shot.
volatile uint32_t g_win_rva[kMaxRay] = {};
volatile uint64_t g_win_before[kMaxRay] = {};
volatile uint64_t g_win_after[kMaxRay] = {};
volatile int      g_win_count = 0;
volatile uint32_t g_pending_mark = 0;   // ring index captured at the last shot
volatile uint32_t g_pending_live = 0;

// Build 59: THE PROJECTILE FLIGHT RECORDER. Builds 57/58 closed every
// reachable raycast ENTRY point without finding a shot-correlated caller, so
// stop hunting the caster and watch the projectile itself: copy the whole
// 0x180-byte cBallisticProjectileComponent once per frame while the round
// flies, then diff the frames on the drain thread. Every field the engine
// updates during flight identifies itself, wherever it lives, including the
// hypothesised segment pair the reflection tables place SOMEWHERE runtime.
// The instance pointer is the spawn's own return value, validated by
// checking its +0x50 (m_vBulletShootOrigin) against the origin the spawn
// verifiably copies there; a mismatch counts a reject and arms nothing.
constexpr int kTrkFrames = 48;          // 2/3 s at 72 Hz, ~300-430 m of flight
constexpr int kTrkBytes  = 0x180;
volatile uintptr_t g_trk_proj  = 0;     // instance being traced
volatile uint32_t  g_trk_live  = 0;     // game thread is sampling
volatile uint32_t  g_trk_n     = 0;     // frames captured so far
volatile uint32_t  g_trk_fault = 0;     // trace ended by a read fault
volatile uint32_t  g_trk_died  = 0;     // trace ended by the liveness check
volatile uint32_t  g_trk_ready = 0;     // complete, drain may print
volatile uint64_t  g_trk_rej   = 0;     // spawn results that failed +0x50
volatile uint64_t  g_trk_steer = 0;     // build 61: in-flight position pushes
uint8_t g_trk_buf[kTrkFrames][kTrkBytes];

// Build 62: THE CONTROLLER RAY. The published ray (world space, unit) and
// the per-flight relocation state. The flight is relocated onto
//   pos = ray_origin + ray_dir * dist + dev
// where dist accumulates the ENGINE's own per-frame step length and dev
// accumulates the engine's own deviation from a straight line (drop), so
// ballistics stay the engine's and only the line is ours.
volatile float    g_cr_dir[3] = {0, 0, 0};   // published by VRMirror
volatile uint32_t g_cr_ok     = 0;
// Build 81: the right controller's POSITION in the same engine world space,
// published from the same pass so origin and direction can never disagree.
volatile float    g_cr_pos[3] = {0, 0, 0};
volatile uint32_t g_cr_pos_ok = 0;
// 2026-08-13: the LEFT controller's position, for the two-handed weapon, and
// the RIGHT controller's up vector, which carries the roll the aim ray throws
// away (a direction vector has no twist about itself).
volatile float    g_cl_pos[3] = {0, 0, 0};
volatile uint32_t g_cl_pos_ok = 0;
volatile float    g_cr_up[3]  = {0, 0, 0};
volatile uint32_t g_cr_up_ok  = 0;
// BUILD 158: the LEFT controller's forward and up, for the free hand's wrist.
volatile float    g_cl_fwd[3] = {0, 0, 0};
volatile float    g_cl_up[3]  = {0, 0, 0};
volatile uint32_t g_cl_vec_ok = 0;
// BUILD 162: the LEFT controller's GRIP-pose +Y and +Z axes, engine world
// space, for the absolute wrist. Same contract: ok=false is a dropout.
volatile float    g_cl_gy[3]   = {0, 0, 0};
volatile float    g_cl_gz[3]   = {0, 0, 0};
volatile uint32_t g_cl_grip_ok = 0;
volatile uint32_t g_cr_armed  = 0;           // cfg bullet_ctrl
volatile uint32_t g_fly_on    = 0;           // this flight is being relocated
volatile float    g_fly_org[3]   = {};       // ray origin = spawn origin
volatile float    g_fly_dir[3]   = {};       // ray direction (snapshot at spawn)
volatile float    g_fly_spdir[3] = {};       // engine spawn dir (for dev calc)
volatile float    g_fly_dist     = 0;        // metres travelled along the ray
volatile float    g_fly_dev[3]   = {};       // engine deviation accum (drop)
volatile float    g_fly_last[3]  = {};       // last position we wrote
volatile uint64_t g_fly_shots  = 0;          // rounds relocated
volatile uint64_t g_fly_denied = 0;          // armed but no usable ray

bool g_logging = false;

// Build 51, rule 7. A shot site is a RETURN address, so the five bytes in
// front of it must be `E8 rel32` resolving to the getter stub we hooked. That
// single check proves the census RVA still names the same call in this image;
// anything else disarms that axis and says so. Nothing is written either way.
bool verify_site(const uint8_t* base, uintptr_t site, uintptr_t stub,
                 const char* what) {
    if (!site) {
        LOG_WARN("aimtrace: no %s shot site derived for this binary, so the "
                 "per-shot override cannot arm on it (rule 7).", what);
        return false;
    }
    // BUILD 83, rule 7. The site can come straight from grwxr.cfg, so it is
    // arbitrary user input and must be range-checked BEFORE it is
    // dereferenced. Setting aim_shot_site_yaw to 1 (mistaking an RVA key for a
    // boolean) made this read `base + 1 - 5`, four bytes below the image, and
    // killed the process 14 seconds into a run. A number in a text file must
    // never be able to do that: rule 7 says a bad address disarms loudly and
    // leaves the game running.
    const uintptr_t span = g_image_end - g_image_base;
    if (!g_image_base || !g_image_end || site < 5 || site >= span) {
        LOG_WARN("aimtrace: %s shot site 0x%08llX is outside this image "
                 "(base 0x%p, size 0x%llX). It is almost certainly a bad "
                 "grwxr.cfg value: aim_shot_site_yaw/pitch take a hex RVA, "
                 "and 0 means 'use the derived per-shot sites'. Override "
                 "DISARMED on this axis; nothing was read or written.",
                 what, (unsigned long long)site, (void*)g_image_base,
                 (unsigned long long)span);
        return false;
    }
    const uint8_t* call = base + site - 5;
    if (call[0] != 0xE8) {
        LOG_WARN("aimtrace: %s shot site 0x%08llX is not preceded by E8 "
                 "(found 0x%02X). Override DISARMED on this axis.",
                 what, (unsigned long long)site, call[0]);
        return false;
    }
    int32_t rel = 0;
    memcpy(&rel, call + 1, sizeof(rel));
    const uintptr_t target = (uintptr_t)(call + 5 + rel);
    const uintptr_t want   = (uintptr_t)(base + stub);
    if (target != want) {
        LOG_WARN("aimtrace: %s shot site 0x%08llX calls 0x%p, not the getter "
                 "stub 0x%p. Override DISARMED on this axis.",
                 what, (unsigned long long)site, (void*)target, (void*)want);
        return false;
    }
    LOG_INFO("aimtrace: %s per-shot site 0x%08llX VERIFIED (the call in front "
             "of it resolves to the hooked getter stub).",
             what, (unsigned long long)site);
    return true;
}

// ---------------------------------------------------------------------------
// BUILD 136: SetShotLine's prologue, 24 bytes.
//
//   48 89 E0              mov  rax, rsp
//   53                    push rbx
//   41 56                 push r14
//   41 57                 push r15
//   48 81 EC E0 00 00 00  sub  rsp, 0E0h
//   F3 41 0F 10 50 04     movss xmm2, [r8+4]
//   48 89 CB              mov  rbx, rcx
//
// NO absolute address and NO rip-relative displacement anywhere in it, which
// is what lets it survive a rebase and, per method rule 6.3, a different store
// of the same game version. The `movss xmm2,[r8+4]` is also the proof that r8
// is a pointer being dereferenced rather than a scalar argument.
//
// This is matched against the BODY. The hook then goes on the body's single
// jmp thunk, found by sig::find_thunk_to, because the prologue begins
// `mov rax, rsp` and ThunkHook.h hazard 2 is precisely that an rsp-relative
// prologue cannot be safely stolen into a trampoline.
constexpr const char* kShotLineSig =
    "48 89 E0 53 41 56 41 57 48 81 EC E0 00 00 00 F3 41 0F 10 50 04 48 89 CB";

hook::ThunkHook g_shotline_hook;

// BUILD 154: the two trajectory generator bodies. Both were confirmed UNIQUE
// in this build, and both prologues are free of absolute and rip-relative
// bytes, so they survive a rebase for the same reason kShotLineSig does. Each
// reaches its callers through exactly one padded thunk (method rule 6.2), so
// the hook goes on the thunk exactly as SetShotLine's does.
constexpr const char* kGenASig =
    "40 53 56 57 41 54 41 55 48 81 EC B0 00 00 00 31 FF 0F 29 B4 24 A0 00 00 "
    "00 45 0F B6 E9 4C 89 C6";
constexpr const char* kGenBSig =
    "40 53 56 57 41 54 41 55 48 81 EC A0 00 00 00 31 F6 0F 29 B4 24 90 00 00 "
    "00 45 89 CD 4C 89 C7 0F";

hook::ThunkHook g_gen_a_hook;
hook::ThunkHook g_gen_b_hook;

}  // namespace
}  // namespace aimtrace
}  // namespace grwxr

// The recorder. RULE 8: no logging, no allocation, no locks, no calls out.
// A bounded linear scan over at most 48 rows and plain aligned stores.
//
// Unsynchronised by design (see AimTrace.h): a race can duplicate a row or
// lose an increment, and neither changes which row is obviously the per-shot
// reader.
extern "C" uint32_t grwxr_aimget_record(uint64_t index, uint64_t ret_addr,
                                        uint64_t obj) {
    using namespace grwxr::aimtrace;
    if (index > 1) return 0;
    g_calls[index] = g_calls[index] + 1;

    // Object filter first: these stubs are shared with every other class whose
    // vtable has the same slot, so anything that is not the player's own aim
    // angle is counted and discarded rather than given a row.
    const uint64_t player = grwxr_setyaw_lastobj;
    if (player == 0)   { g_nolatch = g_nolatch + 1; return 0; }
    if (obj != player) { g_other   = g_other   + 1; return 0; }

    if (ret_addr < g_image_base || ret_addr >= g_image_end) {
        g_foreign = g_foreign + 1;
        return 0;
    }
    const uint32_t rva = (uint32_t)(ret_addr - g_image_base);
    const uint32_t idx = (uint32_t)index;
    const bool firing  = g_firing != 0;

    // Build 51: the per-shot override, by call site. Only the two verified
    // shot-reader sites can be adjusted, and only while a delta is armed.
    //
    // The yaw site runs first within a shot (it is the lower address in the
    // same straight-line code), so counting rounds there and using that
    // parity for BOTH axes keeps a single shot's yaw and pitch on the same
    // side. Otherwise alternating yaw would fight alternating pitch.
    uint32_t delta = 0;
    if (idx == 0 && rva == g_shot_yaw_site) {
        g_shot_seq = g_shot_seq + 1;
        delta = g_shot_dyaw;
    } else if (idx == 1 && rva == g_shot_pitch_site) {
        delta = g_shot_dpitch;
    }
    if (delta) {
        if (g_shot_alternate && (g_shot_seq & 1) == 0)
            delta ^= 0x80000000u;          // flip the float's sign bit
        g_shot_overrides = g_shot_overrides + 1;
    }

    const int n = g_row_count;
    for (int i = 0; i < n && i < kMaxRows; ++i) {
        if (g_rows[i].rva == rva && g_rows[i].index == idx) {
            g_rows[i].n = g_rows[i].n + 1;
            if (firing) g_rows[i].fire = g_rows[i].fire + 1;
            return delta;
        }
    }
    if (n >= kMaxRows) { g_dropped = g_dropped + 1; return delta; }
    g_rows[n].rva   = rva;
    g_rows[n].index = idx;
    g_rows[n].n     = 1;
    g_rows[n].fire  = firing ? 1 : 0;
    g_row_count = n + 1;
    return delta;
}

// Build 53's recorder. Rule 8 as before: bounded scan, plain stores, no
// logging, no locks, no allocation.
extern "C" void grwxr_wfire_record(uint64_t ret_addr, uint64_t self,
                                   uint64_t ctx, uint32_t f1bits) {
    using namespace grwxr::aimtrace;
    g_wf_calls = g_wf_calls + 1;
    g_wf_self  = self;
    g_wf_ctx   = ctx;
    g_wf_f1    = f1bits;
    // Build 59: the flight recorder rides this per-frame call. One guarded
    // 0x180-byte copy per frame while a traced round is in flight, nothing
    // when idle (rule 8). The instance can be freed on impact mid-trace; a
    // fault just ends the trace early and is itself a lifetime data point.
    if (g_trk_live) {
        const uint32_t fn = g_trk_n;
        if (fn >= (uint32_t)kTrkFrames) {
            g_trk_live = 0; g_trk_ready = 1;
        } else {
            const uint8_t* src = (const uint8_t*)g_trk_proj;
            uint8_t* dst = g_trk_buf[fn];
            __try {
                // Liveness first: the shoot origin at +0x50 is constant for
                // the whole flight, so a mismatch means the pool recycled
                // this instance and it now belongs to someone else. Reading
                // recycled memory only wastes a trace; WRITING it would
                // corrupt an unrelated live object, so this check gates the
                // build 60 steer below, not just the recording.
                const float* org = (const float*)(src + 0x50);
                if (org[0] != g_sp_org[0] || org[1] != g_sp_org[1] ||
                    org[2] != g_sp_org[2]) {
                    g_trk_died = 1; g_trk_live = 0; g_trk_ready = 1;
                } else {
                    for (int k = 0; k < kTrkBytes; ++k) dst[k] = src[k];
                    g_trk_n = fn + 1;
                    // Build 60: the spawn-time turn is discarded by the
                    // engine's first flight tick (proved by the build 59
                    // armed run: +0x100 read unrotated on frame 0 of every
                    // trace). So when the yaw override is armed, re-apply
                    // it HERE, every frame, to the live instance. The copy
                    // above happens first, so the trace records what the
                    // engine had, and the next frame's copy shows whether
                    // our write survived its tick or was overwritten again.
                    // Build 62: relocate the round onto the controller ray.
                    // Position writes are AUTHORITATIVE and FEED BACK into
                    // the engine's next integration step (build 61: a 1 m
                    // per-frame push produced a CURVE, not a parallel
                    // line). So each frame: measure the engine's own step
                    // since our last write, bank its length into dist and
                    // its off-axis part (drop) into dev, then snap the
                    // round back onto the ray at the new distance.
                    if (g_fly_on) {
                        uint8_t* p = (uint8_t*)g_trk_proj;
                        float* pos  = (float*)(p + 0x0B0);
                        float* pos2 = (float*)(p + 0x0F0);
                        float* seg  = (float*)(p + 0x0C0);
                        const float st[3] = {pos[0] - g_fly_last[0],
                                             pos[1] - g_fly_last[1],
                                             pos[2] - g_fly_last[2]};
                        const float sl = sqrtf(st[0] * st[0] + st[1] * st[1] +
                                               st[2] * st[2]);
                        if (sl > 0.0f && sl < 50.0f) {   // sane step only
                            // Build 63: the partner point at +0x0C0 keeps
                            // its own path (build 61 trace: its gap to the
                            // head grew under our pushes) and the user saw
                            // damage stay on the ENGINE trajectory while
                            // the tracer followed the ray. So relocate it
                            // too, preserving its distance behind the head
                            // measured from the ENGINE's own values before
                            // we overwrite them.
                            const float gv[3] = {pos[0] - seg[0],
                                                 pos[1] - seg[1],
                                                 pos[2] - seg[2]};
                            float gl = sqrtf(gv[0] * gv[0] + gv[1] * gv[1] +
                                             gv[2] * gv[2]);
                            if (gl > 20.0f) gl = 20.0f;   // sane segment only
                            g_fly_dist = g_fly_dist + sl;
                            for (int k = 0; k < 3; ++k)
                                g_fly_dev[k] = g_fly_dev[k] +
                                               (st[k] - g_fly_spdir[k] * sl);
                            float np[3];
                            for (int k = 0; k < 3; ++k) {
                                np[k] = g_fly_org[k] +
                                        g_fly_dir[k] * g_fly_dist +
                                        g_fly_dev[k];
                                pos[k]  = np[k];
                                pos2[k] = np[k];
                                seg[k]  = np[k] - g_fly_dir[k] * gl;
                                g_fly_last[k] = np[k];
                            }
                            g_trk_steer = g_trk_steer + 1;
                        }
                    } else if (g_sp_armed) {
                        // Build 61's proof probe, kept as the fallback
                        // instrument: a 1 m/frame lateral push.
                        uint8_t* p = (uint8_t*)g_trk_proj;
                        const float* fdir = (const float*)(p + 0x100);
                        float px = -fdir[1], py = fdir[0];
                        const float len = sqrtf(px * px + py * py);
                        if (len > 0.001f) {
                            px /= len; py /= len;
                            float* pos  = (float*)(p + 0x0B0);
                            float* pos2 = (float*)(p + 0x0F0);
                            pos[0]  += px; pos[1]  += py;
                            pos2[0] += px; pos2[1] += py;
                            g_trk_steer = g_trk_steer + 1;
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_trk_fault = 1; g_trk_live = 0; g_trk_ready = 1;
            }
        }
    }
    if (ret_addr < g_image_base || ret_addr >= g_image_end) return;
    const uint32_t rva = (uint32_t)(ret_addr - g_image_base);
    const int n = g_wf_count;
    for (int i = 0; i < n && i < kMaxWf; ++i)
        if (g_wf[i].rva == rva) { g_wf[i].n = g_wf[i].n + 1; return; }
    if (n >= kMaxWf) return;
    g_wf[n].rva = rva;
    g_wf[n].n   = 1;
    g_wf_count  = n + 1;
}

// Build 54's recorder. This runs on the hottest path in the process, so it is
// deliberately dull: a bounded scan of 12 rows, eight float loads, plain
// stores. No logging, no locks, no allocation, no calls out (CLAUDE.md rule 8).
extern "C" void grwxr_ray_record(uint64_t ret_addr, const float* ray) {
    using namespace grwxr::aimtrace;
    g_ray_calls = g_ray_calls + 1;
    if (ret_addr < g_image_base || ret_addr >= g_image_end) return;
    const uint32_t rva = (uint32_t)(ret_addr - g_image_base);

    int slot = -1;
    const int n = g_ray_count;
    for (int i = 0; i < n && i < kMaxRay; ++i)
        if (g_ray[i].rva == rva) { slot = i; break; }
    if (slot < 0) {
        if (n >= kMaxRay) return;
        slot = n;
        g_ray[slot].rva = rva;
        g_ray[slot].n   = 0;
        g_ray_count = n + 1;
    }
    g_ray[slot].n = g_ray[slot].n + 1;
    // Build 57: remember the order of recent casts (one store, one bump).
    const uint32_t ri = g_ring_idx;
    g_ring[ri & (kRing - 1)] = rva;
    g_ring_idx = ri + 1;
    // The ray input's first 32 bytes. Havok query inputs lead with at least
    // two hkVector4s (origin and to/direction), so 32 bytes is inside the
    // struct. Guarded anyway: table-based SEH costs nothing until it throws.
    if (ray) {
        __try {
            for (int k = 0; k < 8; ++k) g_ray[slot].v[k] = ray[k];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // BUILD 302: the shot ray recorder. Inside the window after his shot, a ray starting near his muzzle is stored.
    const uint64_t sf = g_sr_shot_frame;
    if (sf && ray && g_sd_muz_ok) {
        const uint64_t now = grwxr::d3d11::frame_count();
        if (now >= sf && now - sf <= kSrFrames) {
            g_sr_seen = g_sr_seen + 1;
            __try {
                const float dx = ray[0] - g_sd_muz[0], dy = ray[1] - g_sd_muz[1], dz = ray[2] - g_sd_muz[2];
                if (dx * dx + dy * dy + dz * dz < kSrNearM * kSrNearM) {
                    const uint32_t tag = g_sl_w;
                    if (tag != g_sr_tag) { g_sr_tag = tag; g_sr_tag_n = 0; }
                    if (g_sr_tag_n >= kSrPerShot) return;
                    g_sr_tag_n = g_sr_tag_n + 1;
                    const uint32_t wi = g_sr_w;
                    ShotRay* r = &g_sr[wi & (kSrSlots - 1)];
                    r->tag = g_sl_w; r->rva = rva; r->frame = now;
                    for (int k = 0; k < 8; ++k) r->v[k] = ray[k];
                    g_sr_w = wi + 1;               // published last
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
}

// Build 55. Post-multiplies the engine's own aim quaternion by our rotation,
// which is the same order the engine composes in (base * yaw * pitch * roll),
// so an extra term on the right reads as "more yaw" in its own convention.
// Rule 8: a quaternion product and four stores, nothing else.
// ---------------------------------------------------------------------------
// BUILD 105: the muzzle-anchor world getter. OBSERVER ONLY, it never modifies
// the result. It exists so the drain line can print how far the aiming point
// sits from the muzzle, which is one of the four questions section 7b of
// docs/scratch/HUNT-aiming-point.md asks and which needs no headset.
// ---------------------------------------------------------------------------
extern "C" void grwxr_muzpt_post(const float* out, uint64_t iface) {
    using namespace grwxr::aimtrace;
    if (!out || !iface) return;
    __try {
        // The A/B's lateral axis still wants the most recent muzzle point, so
        // keep that; but the CENSUS pairs muzzle to aim per weapon, because
        // with two weapons out "most recent" compares two different guns.
        g_ap_muzzle[0] = out[0];
        g_ap_muzzle[1] = out[1];
        g_ap_muzzle[2] = out[2];
        g_ap_have_muz  = 1;

        // BUILD 137: the same point, published separately and ONLY when the
        // weapon belongs to the player. m_bPlayerOrReplica is at +0xEA of
        // GR_cWeaponComponent and weapon = iface - 0x80, exactly as the aim
        // getter's own owner gate reads them; the census in this file pairs
        // aim to muzzle by iface, so the two getters agree on what iface is.
        //
        // This is the gate that keeps the direction write off AI. It is inside
        // the enclosing __try, so a bad read costs the publish, not the shot.
        if (*(const uint8_t*)(iface - 0x80 + 0xEA)) {
            g_sd_muz[0] = out[0];
            g_sd_muz[1] = out[1];
            g_sd_muz[2] = out[2];
            _ReadWriteBarrier();       // vector before the flag, as elsewhere
            g_sd_muz_ok = 1;
        }
        if (ApSlot* sl = ap_slot_for(iface)) {
            if (!sl->printed) {
                sl->muz[0] = out[0]; sl->muz[1] = out[1];
                sl->muz[2] = out[2]; sl->muz[3] = out[3];
                sl->have_muz = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---------------------------------------------------------------------------
// BUILD 137: the direction write. Called from the recorder, inside its __try.
//
// Arithmetic and stores only, like everything else on this path, and it runs
// once per shot rather than once per frame. Returns the reason code the drain
// prints, and writes the new vector through `dir`, which is the engine's own
// buffer: the stub hands the same pointer to the real SetShotLine afterwards.
//
// WHAT IS NOT DONE HERE, and it is deliberate. The ORIGIN is left alone. In hip
// fire it is already the muzzle, and in ADS it is the camera, which cannot be
// repaired by a direction alone and is a separate question with its own build.
// The magnitude is preserved too: build 136 measured |dir| = 0.9847 in hip fire
// and exactly 1.0000 in ADS, so the engine does not always hand this function a
// unit vector, and normalising it here would be a second, unasked change riding
// along with the first.
static uint32_t sd_apply(float* org, float* dir, float out_new[3],
                         float* out_dpm) {
    using namespace grwxr::aimtrace;
    if (!g_sd_armed)   return kSdOff;
    if (!org || !dir)  return kSdNoPtr;

    // Interlock 1: is this shot ours? The only shot in the game that begins at
    // the player's own muzzle is the player's own, and the same test excludes
    // ADS, whose origin is the camera about a metre away.
    if (!g_sd_muz_ok) return kSdNoMuzzle;
    const float mx = org[0] - g_sd_muz[0];
    const float my = org[1] - g_sd_muz[1];
    const float mz = org[2] - g_sd_muz[2];
    const float dpm = sqrtf(mx * mx + my * my + mz * mz);
    *out_dpm = dpm;
    if (!(dpm < kSdGateM)) {                    // !( < ) also rejects NaN
        // BUILD 152: THE ORIGIN WRITE. "Not at our muzzle" is no longer the
        // end of it. The 03:05 session (grwxr-25048) caught live what the
        // build-133 comment predicted: the engine relocates the PLAYER's shot
        // origin to the camera in ordinary play (eleven shots WROTE, then ten
        // straight with the origin at dOrgCam 0.31..0.55 while the barrel sat
        // as little as 5.6 deg off the gaze). Refusing those hands the round
        // to the engine's camera-converged aim, which is the owner's "acts
        // right for the first couple of shots but it misses".
        //
        // Repair instead of refuse, but ONLY when the shot is PROVEN ours,
        // and every test an AI shot could pass is paired with one it cannot:
        // its origin is at ITS muzzle (metres from our camera), and even one
        // fired point-blank AT the player's head points AGAINST the view.
        if (!g_sd_org_armed) return kSdNotOurs;
        float cam[3];
        if (!grwxr::camera::base_pos(cam)) return kSdNotOurs;
        const float ox = org[0] - cam[0];
        const float oy = org[1] - cam[1];
        const float oz = org[2] - cam[2];
        const float dcam = sqrtf(ox * ox + oy * oy + oz * oz);
        if (!(dcam < kSdCamGateM)) return kSdNotOurs;
        float vf[3];
        if (!view_fwd(vf)) return kSdNotOurs;
        const float dn0 = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] +
                                dir[2] * dir[2]);
        if (!(dn0 > 1e-4f)) return kSdBadVec;
        const float conv = (dir[0] * vf[0] + dir[1] * vf[1] +
                            dir[2] * vf[2]) / dn0;
        if (!(conv > 0.5f)) return kSdNotOurs;
        if (!g_sd_live) return kSdStale;
        float bd2[3];
        uint32_t gen2 = 0;
        if (!barrel_dir_peek(bd2, &gen2) || gen2 == 0) return kSdStale;
        const float bn2 = sqrtf(bd2[0] * bd2[0] + bd2[1] * bd2[1] +
                                bd2[2] * bd2[2]);
        if (!(bn2 > 1e-4f)) return kSdBadVec;
        const float k2 = dn0 / bn2;
        const float rx = bd2[0] * k2, ry = bd2[1] * k2, rz = bd2[2] * k2;
        if (!(rx == rx) || !(ry == ry) || !(rz == rz)) return kSdBadVec;
        // Origin back to the player's own muzzle (the getter published it
        // fresh this frame, at the hands, while the engine's copy sat at the
        // camera: the 03:05 refused shots carried dPlayerMuz 0.65..1.52 with
        // the muzzle tracking live). Direction onto the barrel with the
        // engine's own magnitude, w components untouched: org and dir are
        // rows of one engine struct (dPtr = 16) and both rows are writable.
        org[0] = g_sd_muz[0];
        org[1] = g_sd_muz[1];
        org[2] = g_sd_muz[2];
        dir[0] = rx;
        dir[1] = ry;
        dir[2] = rz;
        out_new[0] = rx;
        out_new[1] = ry;
        out_new[2] = rz;
        g_sd_worg_n = g_sd_worg_n + 1;
        return kSdWroteOrg;
    }

    // Interlock 2: is the barrel direction from this second, or from whenever
    // the weapon writer last ran? Liveness is sampled by the drain, so there is
    // no clock call here.
    if (!g_sd_live) return kSdStale;
    float bd[3];
    uint32_t gen = 0;
    if (!barrel_dir_peek(bd, &gen) || gen == 0) return kSdStale;

    const float bn = sqrtf(bd[0] * bd[0] + bd[1] * bd[1] + bd[2] * bd[2]);
    if (!(bn > 1e-4f)) return kSdBadVec;

    // Interlock 3: finite and non-degenerate, carrying the engine's own length.
    const float dn = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    const float k  = (dn > 1e-4f) ? (dn / bn) : (1.0f / bn);
    const float nx = bd[0] * k, ny = bd[1] * k, nz = bd[2] * k;
    if (!(nx == nx) || !(ny == ny) || !(nz == nz)) return kSdBadVec;

    dir[0] = nx;
    dir[1] = ny;
    dir[2] = nz;
    out_new[0] = nx;
    out_new[1] = ny;
    out_new[2] = nz;
    g_sd_wrote_n = g_sd_wrote_n + 1;
    return kSdWrote;
}

// ---------------------------------------------------------------------------
// BUILD 136: the SetShotLine recorder. Rule 8: stores only.
//
// Both vectors are read as float4 because the disassembled prologue reads
// [r8+4] as an SSE scalar, so the direction is at least a float2 and the
// engine's own vector type here is 16 bytes. Reading four keeps the option of
// seeing a w component that distinguishes a point from a direction, which is
// question 2 in HANDOFF-2026-08-28 section 5.
//
// The pointers are recorded as well as the values. dPtr = dirp - orgp answers,
// for free, whether the caller handed us two rows of one transform (a small
// constant like 16 or 32) or two unrelated buffers.
//
// BUILD 137: it is no longer only a recorder. The engine's own vectors are
// captured FIRST and unmodified, so `dir=` in the log still means what it has
// always meant, and only then is sd_apply given the chance to replace one.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// BUILD 154: the generator recorder. RULE 8, and it is the strictest case in
// this file because it runs on the engine's own shot path: a handful of plain
// aligned stores into a fixed ring, no logging, no allocation, no locks, no
// calls out, and NO modification of anything the engine owns.
//
// Unsynchronised on purpose, like every other recorder here. A race can only
// cost one stashed call, and a lost stash degrades to why=no-arc on that shot
// rather than to a wrong arc.
// ---------------------------------------------------------------------------
extern "C" void grwxr_gen_record(void* bal, float elev, void* arc,
                                 uint32_t flag, uint32_t which) {
    using namespace grwxr::aimtrace;
    g_gen_calls = g_gen_calls + 1;
    if (!arc) return;
    const uint32_t i = g_gen_w;
    GenCall* g = &g_gen[i & (kGenSlots - 1)];
    g->ctx   = (uint64_t)arc - 0x170;   // the disassembled r8 = shotctx + 0x170
    g->bal   = (uint64_t)bal;
    g->arc   = (uint64_t)arc;
    g->flag  = flag;
    g->elev  = elev;
    g->which = which;
    g_gen_w  = i + 1;                   // published last
}

namespace grwxr {
namespace aimtrace {
namespace {

// BUILD 154: rebuild the arc on the barrel's own elevation.
//
// Called ONLY after sd_apply has ruled the shot ours, so every AI-safety
// interlock build 137 established has already passed. It re-invokes the
// engine's own generator with the arguments that generator was given, and the
// elevation replaced. Same output buffer, so the arc is rebuilt in place and
// SetShotLine, running next, transforms the corrected one.
//
// out_eng receives the elevation the engine used, out_our what we substituted.
// Logging both is what makes the sign convention self-proving in the first
// shot of the first session rather than an argument afterwards.
uint32_t vt_apply(uint64_t ctx, float* out_eng, float* out_our) {
    if (!g_vt_armed) return kVtOff;

    // Most recent stashed generator call for THIS shot context. Newest first,
    // because a ctx can be reused across shots and the live one is the last.
    const uint32_t w = g_gen_w;
    const GenCall* hit = nullptr;
    for (uint32_t k = 0; k < (uint32_t)kGenSlots; ++k) {
        const GenCall* g = &g_gen[(w - 1 - k) & (kGenSlots - 1)];
        if (g->arc && g->ctx == ctx) { hit = g; break; }
    }
    if (!hit) return kVtNoArc;

    gen_fn fn = (gen_fn)(hit->which == 0 ? (void*)grwxr_gen_a_orig
                                         : (void*)grwxr_gen_b_orig);
    if (!fn) return kVtNoFn;

    // The same barrel the direction write uses, and the same freshness gate,
    // so the two halves of one shot can never come from different frames.
    float bd[3];
    uint32_t gen = 0;
    if (!g_sd_live) return kVtStale;
    if (!barrel_dir_peek(bd, &gen) || gen == 0) return kVtStale;
    const float bn = sqrtf(bd[0] * bd[0] + bd[1] * bd[1] + bd[2] * bd[2]);
    if (!(bn > 1e-4f)) return kVtBadVec;

    float s = bd[2] / bn;                       // engine is z-up
    if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
    float deg = asinf(s) * 57.29577951f;
    if (!(deg == deg)) return kVtBadVec;
    // The engine's own clamp, matched rather than guessed: the disassembled
    // path clamps to +-89.99 before the generator ever sees the value.
    if (deg > 89.99f) deg = 89.99f; else if (deg < -89.99f) deg = -89.99f;

    *out_eng = hit->elev;
    *out_our = deg;

    __try {
        fn((void*)hit->bal, deg, (void*)hit->arc, hit->flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kVtFault;
    }
    g_vt_wrote_n = g_vt_wrote_n + 1;
    return kVtWrote;
}

}  // namespace
}  // namespace aimtrace
}  // namespace grwxr

extern "C" void grwxr_shotline_pre(uint64_t ctx, const float* org, float* dir) {
    using namespace grwxr::aimtrace;
    g_sl_calls = g_sl_calls + 1;

    const uint32_t i = g_sl_w;
    ShotLine* s = &g_sl[i & (kSlSlots - 1)];
    s->ctx  = ctx;
    s->orgp = (uint64_t)org;
    s->dirp = (uint64_t)dir;
    for (int k = 0; k < 4; ++k) { s->org[k] = 0.0f; s->dir[k] = 0.0f; }
    s->ndir[0] = s->ndir[1] = s->ndir[2] = 0.0f;
    s->dpm = -1.0f;
    s->vtwhy = kVtNotOur;
    s->vt_eng = s->vt_our = s->vt_fromdir = 0.0f;
    // Set before the guarded section, so a fault reports itself as a fault
    // rather than inheriting whatever the previous occupant of this slot said.
    s->why = kSdFault;
    __try {
        if (org) for (int k = 0; k < 4; ++k) s->org[k] = org[k];
        if (dir) for (int k = 0; k < 4; ++k) s->dir[k] = dir[k];
        // BUILD 152: org is handed on writable. It is the engine's own shot
        // descriptor (orgp and dirp sit 16 bytes apart in one struct), and
        // the direction row has been written through since build 137; the
        // const on this signature was documentation, not a property of the
        // memory.
        s->why = sd_apply(const_cast<float*>(org), dir, s->ndir, &s->dpm);
        // BUILD 154: THE VERTICAL, and it runs here for a reason. sd_apply has
        // just ruled on whether this shot is the player's, using the interlocks
        // that have kept AI fire untouched since build 137. Only a shot that
        // passed them gets its arc rebuilt, which is why the generators
        // themselves modify nothing.
        if (s->why == kSdWrote || s->why == kSdWroteOrg) {
            // The convention check, from the engine's OWN direction as captured
            // above, before anything was written through it.
            const float ex = s->dir[0], ey = s->dir[1], ez = s->dir[2];
            const float en = sqrtf(ex * ex + ey * ey + ez * ez);
            if (en > 1e-4f) {
                float t = ez / en;
                if (t > 1.0f) t = 1.0f; else if (t < -1.0f) t = -1.0f;
                s->vt_fromdir = asinf(t) * 57.29577951f;
            }
            s->vtwhy = vt_apply(ctx, &s->vt_eng, &s->vt_our);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (s->why != kSdWrote && s->why != kSdWroteOrg)
        g_sd_left_n = g_sd_left_n + 1;
    else
        g_sr_shot_frame = grwxr::d3d11::frame_count();   // BUILD 302: open the ray recorder's window on HIS shot only
    s->seq = i;
    // Published last, so the drain never reads a half-written slot.
    g_sl_w = i + 1;
}

// ---------------------------------------------------------------------------
// BUILD 133: one-shot read of the hybrid-shoot constants. See the state block
// at g_wc_state for the derivation of every offset and why two routes.
//
// Own __try, and every dereference is a separate step so a fault names itself
// by which pointer is still zero in the log rather than reporting a blanket
// failure. Returns nothing: the log is written by drain(), never from here.
static void wc_capture_once(uint64_t weapon) {
    using namespace grwxr::aimtrace;
    if (g_wc_state != 0 || !weapon) return;

    // BUILD 135: the guard is a CLAIM, not a bare read. Build 133 read
    // g_wc_state, ran the whole body, then stored, so three threads entered
    // before the first store and took three access violations inside our own
    // DLL (dxgi.dll+0xA7D18, all at 03:28:09.113 in grwxr-28604). __except
    // contained them and the run continued 68 s, but three faults per run is
    // noise in a probe, and the next reuse of this machinery is the O-7
    // handle build. Same idiom as WeaponProbe.cpp:156. The loser returns
    // immediately: this is a one-shot, so there is nothing to wait for.
    if (InterlockedCompareExchange(&g_wc_claim, 1, 0) != 0) return;

    __try {
        g_wc_weapon = weapon;

        // Route A: the associated essence. kind 18, an ordinary class pointer.
        const uint64_t essence = *(const uint64_t*)(weapon + 0xA8);
        g_wc_essence = essence;
        const uint64_t db_a = essence ? *(const uint64_t*)(essence + 0x58) : 0;
        g_wc_db_a = db_a;

        // Route B: the glaring-scope sub-component. kind 20 is a sub-object
        // HANDLE, which may be a pointer or may need resolving, so this is the
        // cross-check and never the primary. A zero here is not a failure of
        // the read, it is an answer about what kind 20 means.
        const uint64_t scope = *(const uint64_t*)(weapon + 0x128);
        const uint64_t db_b = scope ? *(const uint64_t*)(scope + 0x20) : 0;
        g_wc_db_b = db_b;

        // Route A is authoritative. Nothing is read out of a null.
        if (!db_a) { g_wc_state = 2; return; }
        g_wc_hybrid     = *(const uint8_t*)(db_a + 0x13C);
        g_wc_cancel_deg = *(const float*)  (db_a + 0x140);
        g_wc_camera_deg = *(const float*)  (db_a + 0x144);
        g_wc_state = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_wc_state = 2;
    }
}

// BUILD 105: the aiming-point world getter. This is the A/B.
//
// Contract, fully known and verified in all four binaries:
//   float4* f(float4* out /*rcx*/, IBallisticGenerator* iface /*rdx*/)
//   weapon = iface - 0x80
//
// Rule 8: no logging, no allocation, no lock, no COM. Everything here is a
// handful of guarded reads and some arithmetic; the census is captured into
// plain globals and PRINTED by the drain thread, never from this function.
// ---------------------------------------------------------------------------
extern "C" void grwxr_aimpt_post(float* out, uint64_t iface) {
    using namespace grwxr::aimtrace;
    if (!out || !iface) return;
    g_ap_seen = g_ap_seen + 1;

    const uint64_t weapon = iface - 0x80;

    __try {
        // The owner gate, and it is not optional. m_bPlayerOrReplica exists
        // at +0xEA precisely because this class is shared with AI.
        const uint8_t is_player = *(const uint8_t*)(weapon + 0xEA);

        // The 7b census: captured ONCE, from the first weapon we see, so it
        // costs one branch per call afterwards. Handle layout: the bone-name
        // CRC32 sits at handle+0x00 and the resolved index at handle+0x08.
        // BUILD 106: RAW, because build 105 asserted a BoneHandle layout and
        // got three impossible answers out of it: two different handles
        // reporting the SAME name hash, an index of -1867857200, and an "aim
        // world" of (0.00 0.35 0.04) in a game whose world coordinates run in
        // the thousands. Dump the bytes and read the layout off them.
        //
        // Refreshed until the slot has been PRINTED, not captured once, so a
        // snapshot taken before the muzzle getter first ran does not freeze an
        // empty comparison in place. That is what happened on the 00:45 run.
        if (ApSlot* sl = ap_slot_for(iface)) {
            if (!sl->printed) {
                for (int i = 0; i < 48; ++i)
                    sl->raw[i] = *(const uint8_t*)(weapon + 0xB0 + i);
                sl->aim[0] = out[0]; sl->aim[1] = out[1];
                sl->aim[2] = out[2]; sl->aim[3] = out[3];
                sl->player = is_player ? 1u : 0u;
                sl->have_aim = 1;
            }
        }

        g_ap_last[0] = out[0];
        g_ap_last[1] = out[1];
        g_ap_last[2] = out[2];

        if (!is_player) { g_ap_ai = g_ap_ai + 1; return; }
        g_ap_player = g_ap_player + 1;

        // BUILD 133: one-shot, player weapons only. Costs one load and one
        // branch per call once it has answered.
        wc_capture_once(weapon);

        if (!g_ap_armed) return;

        // One alternation per TRIGGER PULL, not per call: this getter runs
        // more than once a shot and flipping per call would average the two
        // sides back into one group and manufacture a false negative.
        const uint32_t firing = g_firing;
        if (firing && !g_ap_prev_fire) {
            g_ap_side  = g_ap_side ^ 1u;
            g_ap_shots = g_ap_shots + 1;
        }
        g_ap_prev_fire = firing;
        if (!firing) return;

        // Lateral shift about the world up axis, taken perpendicular to the
        // muzzle-to-aim-point line so it is a genuine left/right of the shot
        // rather than of the world. Falls back to world X if that line is
        // degenerate, which only happens before the muzzle getter has run.
        float ax = out[0] - g_ap_muzzle[0];
        float ay = out[1] - g_ap_muzzle[1];
        float lx, ly;
        const float alen = sqrtf(ax*ax + ay*ay);
        if (g_ap_have_muz && alen > 0.05f) {
            ax /= alen; ay /= alen;
            lx = -ay; ly = ax;              // z-up: perpendicular in plan
        } else {
            lx = 1.0f; ly = 0.0f;
        }

        const float k = g_ap_side ? g_ap_shift_m : -g_ap_shift_m;
        const float nx = out[0] + lx * k;
        const float ny = out[1] + ly * k;
        if (nx == nx && ny == ny) {         // NaN guard, cheap
            out[0] = nx;
            out[1] = ny;
            g_ap_written = g_ap_written + 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

extern "C" void grwxr_aimq_post(float* q, uint64_t ret_addr) {
    using namespace grwxr::aimtrace;
    g_aq_calls = g_aq_calls + 1;
    // Caller census, always on: it is what identifies the fire path.
    if (ret_addr >= g_image_base && ret_addr < g_image_end) {
        const uint32_t rva = (uint32_t)(ret_addr - g_image_base);
        const int n = g_aq_rowcount;
        int slot = -1;
        for (int i = 0; i < n && i < kMaxAq; ++i)
            if (g_aq_rows[i].rva == rva) { slot = i; break; }
        if (slot < 0 && n < kMaxAq) {
            slot = n; g_aq_rows[slot].rva = rva; g_aq_rows[slot].n = 0;
            g_aq_rowcount = n + 1;
        }
        if (slot >= 0) g_aq_rows[slot].n = g_aq_rows[slot].n + 1;
    }
    if (!g_aq_armed || !q) return;
    const float ax = q[0], ay = q[1], az = q[2], aw = q[3];
    const float bx = g_aq_rot[0], by = g_aq_rot[1],
                bz = g_aq_rot[2], bw = g_aq_rot[3];
    q[0] = aw * bx + ax * bw + ay * bz - az * by;
    q[1] = aw * by - ax * bz + ay * bw + az * bx;
    q[2] = aw * bz + ax * by - ay * bx + az * bw;
    q[3] = aw * bw - ax * bx - ay * by - az * bz;
    g_aq_last[0] = q[0]; g_aq_last[1] = q[1];
    g_aq_last[2] = q[2]; g_aq_last[3] = q[3];
    g_aq_applied = g_aq_applied + 1;
}

// Build 56. Rule 8: reads eight floats, writes at most four, no logging.
extern "C" void grwxr_spawn_pre(void* owner) {
    using namespace grwxr::aimtrace;
    g_sp_didwrite = 0;
    if (!owner) return;
    g_sp_calls = g_sp_calls + 1;
    g_pending_mark = g_ring_idx;      // build 57: where the shot happened
    g_pending_live = 1;
    float* dir = (float*)((uint8_t*)owner + 0x140);
    float* org = (float*)((uint8_t*)owner + 0x150);
    __try {
        for (int k = 0; k < 4; ++k) { g_sp_dir[k] = dir[k]; g_sp_org[k] = org[k]; }
        if (!g_sp_armed) return;
        for (int k = 0; k < 4; ++k) g_sp_saved[k] = dir[k];
        // Yaw about world up (+Z in this basis: x right, y forward, z up),
        // so the round swings left/right and its elevation is untouched.
        const float c = g_sp_cos, s = g_sp_sin;
        const float x = dir[0], y = dir[1];
        dir[0] = x * c - y * s;
        dir[1] = x * s + y * c;
        g_sp_didwrite = 1;
        g_sp_turned = g_sp_turned + 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_sp_didwrite = 0; }
}

extern "C" void grwxr_spawn_post(void* owner, void* proj) {
    using namespace grwxr::aimtrace;
    if (owner && g_sp_didwrite) {
        __try {
            float* dir = (float*)((uint8_t*)owner + 0x140);
            for (int k = 0; k < 4; ++k) dir[k] = g_sp_saved[k];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_sp_didwrite = 0;
    }
    // Build 59: arm the flight recorder on this round, one trace at a time.
    // The +0x50 check is what makes the rax-is-the-projectile assumption
    // safe to act on: the spawn verifiably copies [owner+0x150] there, and
    // pre() captured that same value into g_sp_org moments ago.
    if (!proj || g_trk_live || g_trk_ready) return;
    __try {
        const float* org = (const float*)((uint8_t*)proj + 0x50);
        if (org[0] == g_sp_org[0] && org[1] == g_sp_org[1] &&
            org[2] == g_sp_org[2]) {
            g_trk_proj  = (uintptr_t)proj;
            g_trk_n     = 0;
            g_trk_fault = 0;
            g_trk_died  = 0;
            // Build 62: snapshot the controller ray for this flight. The
            // forward-hemisphere guard (dot > 0 against the engine's own
            // spawn direction) keeps a dangling or backwards controller
            // from firing a round through the player (skeleton rule 5:
            // clamp everything controller-driven before it reaches the
            // game). A denied round flies unmodified and is counted.
            g_fly_on = 0;
            if (g_cr_armed) {
                if (g_cr_ok) {
                    const float cd[3] = {g_cr_dir[0], g_cr_dir[1], g_cr_dir[2]};
                    float sd[3] = {g_sp_dir[0], g_sp_dir[1], g_sp_dir[2]};
                    const float sl =
                        sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
                    const float dot =
                        sl > 0.001f ? (cd[0] * sd[0] + cd[1] * sd[1] +
                                       cd[2] * sd[2]) / sl
                                    : -1.0f;
                    if (dot > 0.0f) {
                        for (int k = 0; k < 3; ++k) {
                            g_fly_org[k]   = g_sp_org[k];
                            g_fly_dir[k]   = cd[k];
                            g_fly_spdir[k] = sd[k] / sl;
                            g_fly_dev[k]   = 0.0f;
                            g_fly_last[k]  = g_sp_org[k];
                        }
                        g_fly_dist = 0.0f;
                        g_fly_on   = 1;
                        g_fly_shots = g_fly_shots + 1;
                    } else {
                        g_fly_denied = g_fly_denied + 1;
                    }
                } else {
                    g_fly_denied = g_fly_denied + 1;
                }
            }
            g_trk_live  = 1;
        } else {
            g_trk_rej = g_trk_rej + 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_trk_rej = g_trk_rej + 1; }
}

namespace grwxr {
namespace aimtrace {

// The research probes hook some of the busiest functions in the process (the
// Havok world raycast most of all). That is acceptable while we are hunting,
// and NOT acceptable to ship to testers unmeasured, so the whole module is
// opt-in. The cfg is not parsed yet when install() runs (VRMirror reads it
// once the device exists), so read the single key directly here. Init thread,
// once, no hot path involved.
bool probes_requested() {
    const std::wstring path = log::data_dir() + L"\\grwxr.cfg";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    char line[256];
    bool on = false;
    while (fgets(line, sizeof(line), f)) {
        float v = 0.0f;
        if (sscanf_s(line, " aim_probes = %f", &v) == 1) on = v > 0.0f;
    }
    fclose(f);
    return on;
}

// ---------------------------------------------------------------------------
// BUILD 105: THE ANCHOR GETTERS INSTALL ON THEIR OWN, and this placement is
// deliberate rather than tidy.
//
// install() above returns early when cfg aim_probes is 0, which is now the
// shipping AND the local value, because those probes cost 4,561 hook entries
// a second and were measured on 2026-08-15. Putting these two hooks inside it
// would have made them silently never install, and the headset result would
// have read as a clean negative.
//
// That is the build 64 -> 64.1 lesson (a probe appended below an early return
// dies the moment the return fires) and the build 52 lesson (prove the
// instrument before believing a negative), and it has now cost this project a
// session twice. Hence: separate entry point, no shared gate.
// ---------------------------------------------------------------------------
bool install_aimpoint() {
    const gamebuild::Build* gb = gamebuild::get();
    if (!gb) {
        LOG_WARN("aimpoint: no build pin, so no verified getter RVAs. The "
                 "aim-point probe is OFF for this run (rule 7).");
        return false;
    }
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (!base) return false;
    g_base = base;

    // BUILD 105: the anchor world getters. Installed unconditionally, because
    // the log-only half (section 7b of docs/scratch/HUNT-aiming-point.md)
    // answers four standing unknowns and costs a few guarded reads. The WRITE
    // half stays inert until cfg aimpoint_ab arms it.
    if (gb->aimpt_thunk && gb->aimpt_impl) {
        grwxr_aimpt_orig = (uint64_t)(base + gb->aimpt_impl);
        g_aimpt_hook.install(base + gb->aimpt_thunk, base + gb->aimpt_impl,
                             (void*)&grwxr_aimpt_entry, "aim-point anchor");
    } else {
        LOG_WARN("aimtrace: the aiming-point world getter is not derived for "
                 "this binary; the aim-point A/B is OFF (rule 7).");
    }
    if (gb->muzzlept_thunk && gb->muzzlept_impl) {
        grwxr_muzpt_orig = (uint64_t)(base + gb->muzzlept_impl);
        g_muzpt_hook.install(base + gb->muzzlept_thunk, base + gb->muzzlept_impl,
                             (void*)&grwxr_muzpt_entry, "muzzle anchor");
    }

    // -----------------------------------------------------------------------
    // BUILD 136: SETSHOTLINE. The only hook in this file with no pinned RVA.
    //
    // The whole image is scanned, not one section. HANDOFF-2026-08-28 section
    // 5 said "scan .link only", but method rule 6.3 in the same document
    // records that Denuvo RENAMES SECTIONS PER BUILD, and Sig.h standing rule
    // 3 forbids filtering by section name because Wildlands' real code already
    // lives in a section called .sbss. Scanning by name would be the one
    // choice guaranteed to break on the next update. find_unique skips
    // uncommitted pages, so a full sweep is safe and costs a moment on the
    // init thread.
    //
    // Every failure path below installs NOTHING and logs why (CLAUDE.md rule
    // 7). A missing probe must never stop the game.
    // -----------------------------------------------------------------------
    {
        size_t m = 0;
        auto img = sig::main_image();
        if (!img) {
            LOG_WARN("shotline: main image headers unreadable. Probe NOT "
                     "installed; the game runs normally.");
        } else if (auto body = sig::find_unique(*img, kShotLineSig, &m)) {
            const size_t body_rva = (size_t)((uint8_t*)*body - base);
            size_t tm = 0;
            auto thunk = sig::find_thunk_to(*img, *body, &tm);
            if (!thunk) {
                LOG_WARN("shotline: body found at RVA 0x%08zX, but %zu padded "
                         "thunks jump to it and exactly 1 was expected "
                         "(method rule 6.2). Probe NOT installed; the game "
                         "runs normally.", body_rva, tm);
            } else {
                const size_t thunk_rva = (size_t)((uint8_t*)*thunk - base);
                grwxr_shotline_orig = (uint64_t)*body;
                if (g_shotline_hook.install(*thunk, (void*)*body,
                                            (void*)&grwxr_shotline_entry,
                                            "SetShotLine")) {
                    LOG_INFO("shotline: INSTALLED. body RVA 0x%08zX, thunk RVA "
                             "0x%08zX, 1 signature match, 1 thunk. Fires once "
                             "per shot.", body_rva, thunk_rva);
                    // BUILD 137: say the arming state HERE, at install, rather
                    // than leaving it to be inferred from a counter later. A
                    // feature that is off looks identical in the log to a
                    // feature that is on and never triggered, and this project
                    // has spent whole sessions on exactly that ambiguity.
                    LOG_INFO("shot_dir: %s at startup (cfg shot_dir, "
                             "hot-reloadable, default 1). Gate: the shot must "
                             "start within %.2f m of your own muzzle, and the "
                             "weapon writer (cfg wgun) must be publishing.",
                             g_sd_armed ? "ARMED" : "off", kSdGateM);
                } else {
                    grwxr_shotline_orig = 0;
                    LOG_WARN("shotline: the thunk at RVA 0x%08zX refused the "
                             "rewrite. Probe NOT installed; the game runs "
                             "normally.", thunk_rva);
                }
            }
        } else {
            LOG_WARN("shotline: prologue signature matched %zu times, exactly "
                     "1 expected. Probe NOT installed; the game runs normally. "
                     "%s", m,
                     m == 0 ? "0 matches usually means the game was updated."
                            : ">1 means the pattern is too short for this "
                              "binary.");
        }
    }

    // -----------------------------------------------------------------------
    // BUILD 154: THE TWO TRAJECTORY GENERATORS. Same discipline as SetShotLine
    // above: signature to body, body to its single padded thunk, and every
    // failure path installs NOTHING and says why (rule 7). The vertical simply
    // stays broken if either is missing, which is exactly today's behaviour.
    // -----------------------------------------------------------------------
    {
        auto img = sig::main_image();
        if (!img) {
            LOG_WARN("shot_pitch: main image headers unreadable. The vertical "
                     "correction is NOT installed; the game runs normally.");
        } else {
            struct GenTarget {
                const char*      sig;
                uint64_t*        orig;
                void             (*entry)();
                hook::ThunkHook* hook;
                const char*      name;
            } targets[2] = {
                { kGenASig, &grwxr_gen_a_orig, &grwxr_gen_a_entry,
                  &g_gen_a_hook, "trajectory generator A" },
                { kGenBSig, &grwxr_gen_b_orig, &grwxr_gen_b_entry,
                  &g_gen_b_hook, "trajectory generator B" },
            };
            int installed = 0;
            for (int i = 0; i < 2; ++i) {
                size_t gm = 0;
                auto body = sig::find_unique(*img, targets[i].sig, &gm);
                if (!body) {
                    LOG_WARN("shot_pitch: %s signature matched %zu times, 1 "
                             "expected. NOT installed. %s", targets[i].name, gm,
                             gm == 0 ? "0 usually means the game was updated."
                                     : ">1 means the pattern is too short.");
                    continue;
                }
                const size_t body_rva = (size_t)((uint8_t*)*body - base);
                size_t tm = 0;
                auto thunk = sig::find_thunk_to(*img, *body, &tm);
                if (!thunk) {
                    LOG_WARN("shot_pitch: %s body at RVA 0x%08zX, but %zu "
                             "padded thunks jump to it and exactly 1 was "
                             "expected (method rule 6.2). NOT installed.",
                             targets[i].name, body_rva, tm);
                    continue;
                }
                *targets[i].orig = (uint64_t)*body;
                if (targets[i].hook->install(*thunk, (void*)*body,
                                             (void*)targets[i].entry,
                                             targets[i].name)) {
                    ++installed;
                    LOG_INFO("shot_pitch: %s INSTALLED. body RVA 0x%08zX, "
                             "thunk RVA 0x%08zX. Observer only: it records the "
                             "launch elevation and modifies nothing.",
                             targets[i].name, body_rva,
                             (size_t)((uint8_t*)*thunk - base));
                } else {
                    *targets[i].orig = 0;
                    LOG_WARN("shot_pitch: %s thunk refused the rewrite. NOT "
                             "installed.", targets[i].name);
                }
            }
            LOG_INFO("shot_pitch: %s at startup (cfg shot_pitch, "
                     "hot-reloadable, default 1), %d of 2 generators hooked. "
                     "SetShotLine never reads the vertical (it reads [r8] and "
                     "[r8+4] only), so the arc is rebuilt on the barrel angle "
                     "AFTER the shot has already passed the same player gate "
                     "that protects AI fire. why=PITCH means it ran.",
                     g_vt_armed ? "ARMED" : "off", installed);
        }
    }
    return g_aimpt_hook.installed();
}

// Its own drain, for exactly the same reason. aimtrace::drain() is gated on
// cfg aim_trace AND on the getyaw/getpitch hooks being installed, and neither
// is true in the configuration this experiment runs in.
void drain_aimpoint() {
    // -----------------------------------------------------------------------
    // BUILD 136: THE SHOT LINES. Drained BEFORE the aim-point guard below,
    // because this hook installs independently of the aim-point getter and a
    // probe that installed must never be silenced by a sibling that did not.
    //
    // A distance of -1.000 means the input it needed was absent, never a real
    // measurement: dOrgMuzzle needs a muzzle anchor to have resolved this run,
    // dOrgCam needs camera::base_pos to succeed. Real distances cannot be
    // negative, so the sentinel is unambiguous to sweep_logs.py.
    // -----------------------------------------------------------------------
    // BUILD 137: barrel liveness, sampled here rather than read from a clock
    // inside the shot hook. grwxr_wgun_apply publishes at frame rate whenever a
    // player skeleton is armed, so a generation that has not moved since the
    // last drain means the weapon writer is not running at all (cfg wgun = 0,
    // the gun holstered, no VR, or no player pin) and whatever direction it
    // left behind is not where the barrel points any more. Worst case staleness
    // is therefore one drain interval, which is one second.
    {
        float bd[3];
        uint32_t gen = 0;
        barrel_dir_peek(bd, &gen);
        const uint32_t live = (gen != g_sd_gen_prev) ? 1u : 0u;
        g_sd_gen_prev = gen;
        // Report the EDGE, not the state, so this is silent in the normal case
        // and loud at the moment the direction write loses its input. Without
        // it, "the barrel stopped publishing" is only visible on a line that is
        // printed when a shot happens, which is exactly when it is too late.
        static uint32_t s_live_said = 2;      // 2 = nothing said yet
        if (live != s_live_said) {
            s_live_said = live;
            if (g_sd_armed) {
                LOG_INFO("shot_dir: the barrel direction is %s (cfg wgun is "
                         "%s publishing). wrote=%u worg=%u left=%u",
                         live ? "LIVE" : "NOT being published",
                         live ? "" : "not",
                         (unsigned)g_sd_wrote_n, (unsigned)g_sd_worg_n,
                         (unsigned)g_sd_left_n);
            }
        }
        g_sd_live = live;
    }

    // BUILD 302: the shot ray drain. Each stored ray against the shot it followed: the barrel we wrote (new), the engine's
    // head-following direction (eng), and the view forward. A ray family that sits on eng while new is far from it is the pull.
    {
        const uint32_t w = g_sr_w;
        uint32_t p = g_sr_printed;
        if (w != p) {
            if (w - p > (uint32_t)kSrSlots) {
                LOG_INFO("SHOTRAY lapped: %u rays since the last drain, the ring holds %d; the oldest are lost.", w - p, kSrSlots);
                p = w - (uint32_t)kSrSlots;
            }
            float vf[3] = {0, 0, 0};
            const bool have_vf = view_fwd(vf);
            auto angle = [](const float a[3], const float b[3]) {
                const float na = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
                const float nb = sqrtf(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
                if (!(na > 1e-6f) || !(nb > 1e-6f)) return -1.0f;
                float c = (a[0]*b[0] + a[1]*b[1] + a[2]*b[2]) / (na * nb);
                if (c > 1.0f) c = 1.0f; else if (c < -1.0f) c = -1.0f;
                return acosf(c) * 57.29577951f;
            };
            int printed = 0;
            for (; p != w; ++p) {
                const ShotRay* r = &g_sr[p & (kSrSlots - 1)];
                if (printed >= 48) { LOG_INFO("SHOTRAY: %u more rays this drain not shown", w - p); break; }
                ++printed;
                const uint32_t seq = r->tag ? r->tag - 1 : 0;
                const ShotLine* s = &g_sl[seq & (kSlSlots - 1)];
                const bool have_s = r->tag != 0 && s->seq == seq && (s->why == kSdWrote || s->why == kSdWroteOrg);
                // the ray's own direction: `to` minus origin when `to` is a point, else `to` as a direction
                float d[3] = {r->v[4] - r->v[0], r->v[5] - r->v[1], r->v[6] - r->v[2]};
                const float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                const float tl = sqrtf(r->v[4]*r->v[4] + r->v[5]*r->v[5] + r->v[6]*r->v[6]);
                const bool as_dir = tl > 0.5f && tl < 1.5f && dl > 2.0f * tl;
                if (as_dir) { d[0] = r->v[4]; d[1] = r->v[5]; d[2] = r->v[6]; }
                float om[3] = {0, 0, 0};
                float dmuz = -1.0f;
                if (have_s) {
                    om[0] = r->v[0] - s->org[0]; om[1] = r->v[1] - s->org[1]; om[2] = r->v[2] - s->org[2];
                    dmuz = sqrtf(om[0]*om[0] + om[1]*om[1] + om[2]*om[2]);
                }
                LOG_INFO("SHOTRAY seq=%u rva=0x%08X frame=+%llu org=(%.3f %.3f %.3f) to=(%.3f %.3f %.3f) %s len=%.2f "
                         "dOrgShot=%.3f | vs_new=%.1f vs_eng=%.1f vs_view=%.1f new_vs_eng=%.1f deg "
                         "(build 302: rays cast within 4 m of your muzzle in the 60 Presents after your shot; new = the barrel we "
                         "wrote, eng = the engine's own head-following direction, view = the view forward now; the pull is a ray on eng)",
                         seq, (unsigned)r->rva,
                         (unsigned long long)(have_s && r->frame >= g_sr_shot_frame ? r->frame - g_sr_shot_frame : 0),
                         r->v[0], r->v[1], r->v[2], r->v[4], r->v[5], r->v[6], as_dir ? "dir" : "point", as_dir ? tl : dl, dmuz,
                         have_s ? angle(d, s->ndir) : -1.0f, have_s ? angle(d, s->dir) : -1.0f,
                         have_vf ? angle(d, vf) : -1.0f, have_s ? angle(s->ndir, s->dir) : -1.0f);
            }
            g_sr_printed = w;
        }
    }

    {
        const uint32_t w = g_sl_w;
        uint32_t p = g_sl_printed;
        if (w != p) {
            // Section 4.5's lesson: say what was dropped rather than printing a
            // silent subset. A lapped ring that reports quietly reads exactly
            // like a ring that caught everything.
            if (w - p > kSlSlots) {
                LOG_INFO("SHOTLINE lapped: %u shots since the last drain but "
                         "the ring holds %u. Reporting the most recent %u; the "
                         "rest are lost.",
                         w - p, kSlSlots, kSlSlots);
                p = w - kSlSlots;
            }
            float cam[3] = {0, 0, 0};
            const bool have_cam = camera::base_pos(cam);
            for (; p != w; ++p) {
                const ShotLine* s = &g_sl[p & (kSlSlots - 1)];
                const float dl = sqrtf(s->dir[0] * s->dir[0] +
                                       s->dir[1] * s->dir[1] +
                                       s->dir[2] * s->dir[2]);
                float dmuz = -1.0f, dcam = -1.0f;
                if (g_ap_have_muz) {
                    const float ax = s->org[0] - g_ap_muzzle[0];
                    const float ay = s->org[1] - g_ap_muzzle[1];
                    const float az = s->org[2] - g_ap_muzzle[2];
                    dmuz = sqrtf(ax * ax + ay * ay + az * az);
                }
                if (have_cam) {
                    const float bx = s->org[0] - cam[0];
                    const float by = s->org[1] - cam[1];
                    const float bz = s->org[2] - cam[2];
                    dcam = sqrtf(bx * bx + by * by + bz * bz);
                }
                // BUILD 137: dir= is what the ENGINE built, which is what this
                // line has always meant, and new= is what the shot actually
                // left with. They are equal only when why=WROTE. Keeping the
                // old fields in the old order matters: every sweep pattern in
                // tools/sweep_logs.py that already matches SHOTLINE keeps
                // working, and the new columns are additions on the end.
                LOG_INFO("SHOTLINE seq=%u ctx=%012llX orgp=%012llX "
                         "dirp=%012llX org=(%.4f %.4f %.4f %.4f) "
                         "dir=(%.4f %.4f %.4f %.4f) |dir|=%.4f dPtr=%lld "
                         "dOrgMuzzle=%.3f dOrgCam=%.3f why=%s dPlayerMuz=%.3f "
                         "new=(%.4f %.4f %.4f)",
                         s->seq, (unsigned long long)s->ctx,
                         (unsigned long long)s->orgp,
                         (unsigned long long)s->dirp,
                         s->org[0], s->org[1], s->org[2], s->org[3],
                         s->dir[0], s->dir[1], s->dir[2], s->dir[3],
                         dl, (long long)s->dirp - (long long)s->orgp,
                         dmuz, dcam, sd_why(s->why), s->dpm,
                         s->ndir[0], s->ndir[1], s->ndir[2]);

                // BUILD 154: the vertical, on its own line so every existing
                // sweep pattern that matches SHOTLINE keeps working untouched.
                //
                // eng= is the elevation the engine handed its own arc builder,
                // fromdir= is that same angle recomputed from the engine's own
                // direction vector, and our= is what we substituted. eng and
                // fromdir agreeing proves the decode; eng and fromdir being
                // NEGATIVES of each other proves our sign is inverted, which
                // is the one convention risk in this build and is settled by
                // reading a single line rather than by another session.
                if (s->why == kSdWrote || s->why == kSdWroteOrg)
                    LOG_INFO("SHOTPITCH seq=%u ctx=%012llX why=%s "
                             "eng=%+.2f fromdir=%+.2f our=%+.2f delta=%+.2f "
                             "deg | arcs=%u rebuilt=%u",
                             s->seq, (unsigned long long)s->ctx,
                             vt_why(s->vtwhy), s->vt_eng, s->vt_fromdir,
                             s->vt_our, s->vt_our - s->vt_eng,
                             g_gen_calls, g_vt_wrote_n);

                // BUILD 137: say it ONCE, the first time a shot is actually
                // rewritten. The tester is in a headset and cannot read any of
                // the above, and "did the feature even arm" is the question
                // that has wasted the most sessions in this project. The drain
                // is one of the two callers Voice.h names as intended.
                if (s->why == kSdWrote && !g_sd_said) {
                    g_sd_said = 1;
                    voice::say("Gun aim is live. The shot now follows the "
                               "barrel.");
                }

                // BUILD 152: same rule for the origin write. The first shot
                // the engine tried to fire from the camera and had moved back
                // to the muzzle announces itself, once.
                if (s->why == kSdWroteOrg && !g_sd_said_org) {
                    g_sd_said_org = 1;
                    voice::say("Shot origin repaired. The round leaves the "
                               "muzzle even when the game moves it.");
                }

                // BUILD 137: the false negative this build is most likely to
                // produce, said out loud the first time it happens.
                //
                // An ADS shot does not start at the muzzle, it starts at the
                // camera, roughly 0.9 m away as build 136 measured it. This
                // build deliberately does not touch those, because a direction
                // alone cannot fix a shot that begins behind your shoulder. So
                // a tester who holds ADS out of habit gets a build that does
                // nothing and no way to know why, and the session is spent.
                // The distance band is what separates it from a genuine
                // stranger's shot, which would be metres out, not one metre.
                static uint32_t s_ads_said = 0;
                if (s->why == kSdNotOurs && !s_ads_said &&
                    s->dpm > 0.40f && s->dpm < 2.00f) {
                    s_ads_said = 1;
                    voice::say("That shot started at the camera, not the gun. "
                               "Do not aim down sights for this test.");
                }
            }
            g_sl_printed = w;
        }
    }

    if (!g_aimpt_hook.installed()) return;

    // The 7b census, once, as soon as a weapon has been seen. idx == -1 means
    // the bone never resolved, the getter is returning the zero vector, and
    // nothing can aim at the world origin: that alone would explain
    // camera-following bullets and it makes the A/B below meaningless.
    // BUILD 106.1: print each weapon once, as soon as it has BOTH its aim
    // point and its muzzle point, so the two columns always describe the same
    // gun. A slot that has an aim point but no muzzle after several seconds is
    // printed anyway, with the gap named, rather than being held back
    // silently: an instrument that waits forever for a value that never comes
    // reads exactly like an instrument that is not installed.
    static int ap_ticks = 0;
    ++ap_ticks;

    // BUILD 107.1: announce the key ONCE, and only once the player is really
    // in the world. Saying it during the loading screen means saying it to
    // nobody, and the whole point of this module is that the tester should not
    // have to remember anything.
    //
    // 2026-09-01: GATED BEHIND cfg voice_guide, DEFAULT 0. The anchor test it
    // advertises belongs to the build 106/107 aim-point question, which build
    // 137 answered and the headset confirmed. It was still announcing itself
    // 17 seconds into every session, and the protocol it offers talks the
    // tester through drawing and switching weapons, which collides with any
    // other spoken procedure running that session (the wingspan capture, for
    // one). The owner asked whether the old voice tests were still live. They
    // were. This silences the invitation without removing the test: End still
    // starts it, and voice_guide = 1 brings the prompt back.
    {
        static uint32_t said_ready = 0;
        float probe[3];
        if (!said_ready && !g_guide_state && g_voice_guide &&
            camera::base_pos(probe)) {
            said_ready = 1;
            voice::say_key("End", "to start the anchor test.");
        }
    }

    // BUILD 107: THE GUIDED PROTOCOL.
    //
    // Driven by what has actually been CAPTURED, never by a stopwatch. The
    // tester is told to move on the moment a weapon's data is in, and told to
    // wait when it is not, which removes the whole class of failure where a
    // run is spent because a step happened before the thing it measured did.
    // Every spoken line is also written to the log, so the run can be
    // reconstructed afterwards without him remembering any of it.
    if (g_guide_state == 1) {              // just armed
        g_guide_state = 2;
        g_guide_ticks = 0;
        g_guide_done  = 0;
        voice::say_wait("Anchor test started. Draw your first weapon and "
                        "hold still.", 1500);
    } else if (g_guide_state >= 2) {
        ++g_guide_ticks;
        uint32_t printed = 0;
        for (uint32_t k = 0; k < g_ap_slots_used && k < (uint32_t)kApSlots; ++k)
            if (g_ap_slot[k].printed) ++printed;

        if (printed > g_guide_done) {
            g_guide_done = printed;
            g_guide_ticks = 0;
            if (printed == 1) {
                voice::say_wait("First weapon captured. Now switch to your "
                                "second weapon and hold still.", 1500);
            } else {
                voice::say("Second weapon captured. That is everything. Quit "
                           "the game and I will read the results.");
                g_guide_state = 0;         // done, stop nagging
            }
        } else if (g_guide_ticks == 20) {
            // Twenty seconds with nothing new. Say so rather than leaving him
            // standing in silence wondering whether the build is working.
            voice::say(g_ap_slots_used == 0
                           ? "Nothing captured yet. Make sure a weapon is "
                             "drawn and you are on foot."
                           : "Still waiting. Keep the weapon drawn and hold "
                             "still.");
            g_guide_ticks = 0;
        }
    }
    const uint32_t used = g_ap_slots_used;
    for (uint32_t k = 0; k < used && k < (uint32_t)kApSlots; ++k) {
        ApSlot* sl = &g_ap_slot[k];
        if (sl->printed >= 2 || !sl->have_aim) continue;
        if (!sl->have_muz && ap_ticks < 6) continue;   // give it ~6 s
        // BUILD 135: print ONCE MORE if the muzzle point ever arrives late.
        //
        // The freeze below made every muzzle reading a six-second window. All
        // 59 captures in the 497-log corpus say "NO MUZZLE POINT", and that was
        // being read as "the engine never resolves a muzzle anchor". It does
        // not support that: NO capture window in the whole corpus overlaps a
        // logged trigger pull. In grwxr-24876 the captures printed at 20:07:46
        // and 20:08:46 and the first pull was at 20:08:58, twelve seconds after
        // the slot froze.
        //
        // So the historical negative is about the first six seconds of a weapon
        // being drawn, NOT about firing, and the muzzle getter is exactly the
        // sort of thing that would run per shot. One extra line per weapon
        // converts a six-second window into a session-long answer.
        //
        // printed: 0 never, 1 printed without a muzzle point, 2 final.
        // Rule 8 safe for the same reason the first print is: drain thread.
        if (sl->printed == 1 && !sl->have_muz) continue;
        sl->printed = sl->have_muz ? 2u : 1u;

        char hex[3 * 48 + 8];
        for (int i2 = 0; i2 < 48; ++i2) {
            static const char* kHex = "0123456789ABCDEF";
            hex[i2 * 3 + 0] = kHex[(sl->raw[i2] >> 4) & 0xF];
            hex[i2 * 3 + 1] = kHex[sl->raw[i2] & 0xF];
            hex[i2 * 3 + 2] = ((i2 & 15) == 15) ? '|' : ' ';
        }
        hex[48 * 3 - 1] = 0;

        LOG_INFO("aimanchor[%u]: iface=%016llX weapon=%016llX player=%u%s",
                 k, (unsigned long long)sl->iface,
                 (unsigned long long)(sl->iface - 0x80), sl->player,
                 sl->have_muz
                     ? "  (MUZZLE POINT ARRIVED: the getter DID run)"
                     : "  (NO MUZZLE POINT YET: not in the first ~6 s. This "
                       "line reprints if it ever arrives, so keep playing and "
                       "FIRE before believing this is a negative)");
        LOG_INFO("aimanchor[%u]: weapon+0xB0 raw, 3 x 16 bytes "
                 "(gunroot|muzzle|aimpoint): %s", k, hex);
        LOG_INFO("aimanchor[%u]: aim=(%.4f %.4f %.4f %.4f) "
                 "muzzle=(%.4f %.4f %.4f %.4f)", k,
                 sl->aim[0], sl->aim[1], sl->aim[2], sl->aim[3],
                 sl->muz[0], sl->muz[1], sl->muz[2], sl->muz[3]);

        // The control. camera::base_pos is the mod's own VERIFIED world
        // reading, so the space these getters work in is settled by comparison
        // rather than by assertion. If the returned point is thousands of
        // metres from the camera it is not world space, and every reading
        // built on the assumption that it is would be void.
        float cam[3] = {0, 0, 0};
        if (camera::base_pos(cam)) {
            const float ax = sl->aim[0] - cam[0];
            const float ay = sl->aim[1] - cam[1];
            const float az = sl->aim[2] - cam[2];
            const float mx = sl->muz[0] - cam[0];
            const float my = sl->muz[1] - cam[1];
            const float mz = sl->muz[2] - cam[2];
            LOG_INFO("aimanchor[%u]: camera=(%.2f %.2f %.2f) "
                     "|aim-cam|=%.2f |muzzle-cam|=%.2f  <<< a few metres means "
                     "WORLD space, thousands means it is not", k,
                     cam[0], cam[1], cam[2],
                     sqrtf(ax*ax + ay*ay + az*az),
                     sqrtf(mx*mx + my*my + mz*mz));
        } else {
            LOG_INFO("aimanchor[%u]: camera position unavailable, so the "
                     "world-space control could not run this tick.", k);
        }
    }
    if (g_ap_slots_full)
        LOG_INFO("aimanchor: TABLE FULL, %u weapon(s) not captured (capacity "
                 "%d). Raise kApSlots if this matters.",
                 (unsigned)g_ap_slots_full, kApSlots);

    // BUILD 108: count the shots for him. Counting to ten while wearing a
    // headset, holding a rifle and watching a wall is exactly the kind of
    // bookkeeping that gets a run thrown away.
    if (g_ap_armed) {
        static uint32_t said_shots = 0;
        const uint32_t sh = g_ap_shots;
        if (sh >= 10 && said_shots < 10) {
            said_shots = 10;
            voice::say("That is ten shots. Stop firing and look at the wall. "
                       "One group of holes, or two?");
        } else if (sh == 5 && said_shots < 5) {
            said_shots = 5;
            voice::say("Five.");
        }
        if (sh == 0) said_shots = 0;       // re-arm resets the count
    }

    // BUILD 133: the hybrid-shoot read, printed ONCE and never repeated. It
    // is deliberately above the idle early-return: this line is the whole
    // question the build carries and it must not be suppressed by a quiet
    // frame. The archives predict enable=1 cancel=120.0 camera=60.0.
    if (g_wc_state != 0 && !g_wc_printed) {
        g_wc_printed = 1;
        if (g_wc_state == 1) {
            LOG_INFO("hybrid: ENABLE=%u cancel_focus=%.2fdeg "
                     "shoot_from_camera=%.2fdeg | weapon=%012llX "
                     "essence=%012llX db=%012llX",
                     (unsigned)g_wc_hybrid, g_wc_cancel_deg, g_wc_camera_deg,
                     (unsigned long long)g_wc_weapon,
                     (unsigned long long)g_wc_essence,
                     (unsigned long long)g_wc_db_a);
            // The self-check. One instance exists install-wide, so the two
            // routes must agree. Say which way it failed, never average.
            if (g_wc_db_b == g_wc_db_a)
                LOG_INFO("hybrid: route check AGREE (scope route reached the "
                         "same datablock %012llX)",
                         (unsigned long long)g_wc_db_b);
            else if (!g_wc_db_b)
                LOG_INFO("hybrid: route check scope route was NULL "
                         "(sub-object handle at +0x128 is not a raw pointer). "
                         "The essence route stands alone but is unrefuted.");
            else
                LOG_WARN("hybrid: route check DISAGREE, essence=%012llX "
                         "scope=%012llX. One instance exists install-wide, so "
                         "one of these two offsets is wrong. Trust neither.",
                         (unsigned long long)g_wc_db_a,
                         (unsigned long long)g_wc_db_b);
        } else {
            LOG_WARN("hybrid: read FAULTED or hit a null pointer. "
                     "weapon=%012llX essence=%012llX db=%012llX. The zero "
                     "names the step that failed.",
                     (unsigned long long)g_wc_weapon,
                     (unsigned long long)g_wc_essence,
                     (unsigned long long)g_wc_db_a);
        }
    }

    static uint32_t last_seen = 0;
    if (g_ap_seen == last_seen && !g_ap_armed) return;   // quiet when idle
    last_seen = g_ap_seen;
    LOG_INFO("aimpoint: %s shift=%.1fm | seen=%u player=%u ai_refused=%u "
             "rewritten=%u shots=%u next=%s",
             g_ap_armed ? "ARMED" : "idle", g_ap_shift_m,
             (unsigned)g_ap_seen, (unsigned)g_ap_player, (unsigned)g_ap_ai,
             (unsigned)g_ap_written, (unsigned)g_ap_shots,
             g_ap_side ? "RIGHT" : "LEFT");
}

bool install() {

    if (!probes_requested()) {
        LOG_INFO("aimtrace: research probes are OFF (aim_probes=0 or absent "
                 "in grwxr.cfg). Nothing is hooked; this is the shipping "
                 "default because these probes sit on very hot engine "
                 "functions and their frame cost is not yet measured.");
        return false;
    }
    const gamebuild::Build* gb = gamebuild::get();
    if (!gb) {
        LOG_WARN("aimtrace: no build pin, so no verified getter RVAs. The aim "
                 "reader census is OFF for this run (rule 7).");
        return false;
    }
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (!base) {
        LOG_WARN("aimtrace: GRW.exe module handle unavailable. Not installing.");
        return false;
    }
    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    const auto* nt  = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    g_image_base = (uintptr_t)base;
    g_image_end  = g_image_base + nt->OptionalHeader.SizeOfImage;

    // Publish the dispatch offsets BEFORE the patch makes the entries
    // reachable, exactly as build 17 does for the setters.
    memcpy(&grwxr_aimget_disp[0], kGetYawExpect + kDispAt, sizeof(uint32_t));
    memcpy(&grwxr_aimget_disp[1], kGetPitchExpect + kDispAt, sizeof(uint32_t));

    if (gb->getyaw_slot)
        g_getyaw_hook.install_raw(base + gb->getyaw_slot, kGetYawExpect,
                                  sizeof(kGetYawExpect),
                                  (void*)&grwxr_aimget_yaw_entry,
                                  "GetYaw vdispatch");
    else
        LOG_WARN("aimtrace: GetYaw stub is not derived for this binary; "
                 "its census is OFF (rule 7).");
    if (gb->getpitch_slot)
        g_getpitch_hook.install_raw(base + gb->getpitch_slot, kGetPitchExpect,
                                    sizeof(kGetPitchExpect),
                                    (void*)&grwxr_aimget_pitch_entry,
                                    "GetPitch vdispatch");
    else
        LOG_WARN("aimtrace: GetPitch stub is not derived for this binary; "
                 "its census is OFF (rule 7).");

    g_base = base;
    set_shot_sites(0, 0);          // 0,0 = the build-pinned per-shot sites

    // Build 53: the per-shot weapon routine. An ordinary E9 thunk, so
    // ThunkHook::install verifies the jump really resolves to the analysed
    // function before writing anything (rule 7).
    if (gb->wfire_thunk && gb->wfire_impl) {
        grwxr_wfire_orig = (uint64_t)(base + gb->wfire_impl);
        g_wfire_hook.install(base + gb->wfire_thunk, base + gb->wfire_impl,
                             (void*)&grwxr_wfire_entry, "per-shot weapon fn");
    } else {
        LOG_WARN("aimtrace: the per-shot weapon routine is not derived for "
                 "this binary; its caller census is OFF (rule 7).");
    }

    // Build 54: hknpWorld::castRay. Another plain E9 thunk, so install()
    // verifies the jump resolves to the analysed function before writing.
    if (gb->raycast_thunk && gb->raycast_impl) {
        grwxr_ray_orig = (uint64_t)(base + gb->raycast_impl);
        g_ray_hook.install(base + gb->raycast_thunk, base + gb->raycast_impl,
                           (void*)&grwxr_ray_entry, "hknpWorld::castRay");
    } else {
        LOG_WARN("aimtrace: hknpWorld::castRay is not derived for this "
                 "binary; the raycast census is OFF (rule 7).");
    }

    // Build 58: TtCastRay, the body the wrapper reaches through a
    // runtime-built virtual table. Three call sites reach it directly
    // through this thunk and were invisible to the build 57 window.
    if (gb->raycast2_thunk && gb->raycast2_impl) {
        grwxr_ray2_orig = (uint64_t)(base + gb->raycast2_impl);
        g_ray2_hook.install(base + gb->raycast2_thunk,
                            base + gb->raycast2_impl,
                            (void*)&grwxr_ray2_entry, "TtCastRay direct");
    } else {
        LOG_WARN("aimtrace: TtCastRay is not derived for this binary; the "
                 "direct-entry raycast census is OFF (rule 7).");
    }

    // Build 55: GetAimOrientation. Installed but INERT until a cfg angle
    // arms it; until then the stub only counts calls.
    if (gb->aimquat_thunk && gb->aimquat_impl) {
        grwxr_aimq_orig = (uint64_t)(base + gb->aimquat_impl);
        g_aimq_hook.install(base + gb->aimquat_thunk, base + gb->aimquat_impl,
                            (void*)&grwxr_aimq_entry, "GetAimOrientation");
    } else {
        LOG_WARN("aimtrace: GetAimOrientation is not derived for this binary; "
                 "the aim quaternion override is OFF (rule 7).");
    }

    // Build 56: the projectile spawn.

    if (gb->spawn_thunk && gb->spawn_impl) {
        grwxr_spawn_orig = (uint64_t)(base + gb->spawn_impl);
        g_spawn_hook.install(base + gb->spawn_thunk, base + gb->spawn_impl,
                             (void*)&grwxr_spawn_entry, "projectile spawn");
    } else {
        LOG_WARN("aimtrace: the projectile spawn is not derived for this "
                 "binary; the bullet probe is OFF (rule 7).");
    }

    const bool any = g_getyaw_hook.installed() || g_getpitch_hook.installed();
    if (any) {
        LOG_INFO("aimtrace: aim-reader census armed (%s%s). LOG-ONLY: both "
                 "stubs pass through unchanged. Set aim_trace=1 in grwxr.cfg "
                 "to print it.",
                 g_getyaw_hook.installed()   ? "yaw " : "",
                 g_getpitch_hook.installed() ? "pitch" : "");
    } else {
        LOG_WARN("aimtrace: neither getter stub verified, census OFF. The "
                 "game is unmodified by this module (rule 7).");
    }
    return any;
}

void drain() {
    if (!g_logging) return;
    if (!g_getyaw_hook.installed() && !g_getpitch_hook.installed()) return;
    static int tick = 0;
    if ((++tick % 2) != 0) return;          // every other second, it is verbose

    const int n = g_row_count;
    const unsigned long long cy = (unsigned long long)g_calls[0];
    const unsigned long long cp = (unsigned long long)g_calls[1];
    LOG_INFO("aimtrace: %d site(s) on the player's aim angle | stub calls "
             "yaw=%llu pitch=%llu | other-object=%llu pre-latch=%llu | "
             "firing=%s%s",
             n, cy, cp,
             (unsigned long long)g_other, (unsigned long long)g_nolatch,
             g_firing ? "YES" : "no",
             g_dropped ? "  (TABLE FULL, sites dropped)" : "");
    if (g_shot_dyaw || g_shot_dpitch) {
        float in = 0.0f, out = 0.0f;
        memcpy(&in,  &grwxr_aimget_vals[0], 4);
        memcpy(&out, &grwxr_aimget_vals[1], 4);
        // THE SELF-CHECK. If in and out differ by the armed angle, our
        // addition demonstrably reached the engine and any lack of effect is
        // the engine's indifference, not our bug. If they are equal, the
        // override never ran and every result from it is void.
        LOG_INFO("  override ARMED (%s) site yaw=0x%08X pitch=0x%08X | "
                 "rounds=%llu applied=%llu | last read %.2f deg -> handed "
                 "back %.2f deg (difference %.2f)",
                 g_shot_alternate ? "alternating" : "constant",
                 g_shot_yaw_site, g_shot_pitch_site,
                 (unsigned long long)g_shot_seq,
                 (unsigned long long)g_shot_overrides,
                 in * 57.29578f, out * 57.29578f, (out - in) * 57.29578f);
    }
    if (n == 0) {
        LOG_INFO("  no reader yet. If this stays 0 while the counts above "
                 "climb, nothing reads the PLAYER's aim through these "
                 "accessors and the DR0 tracer is the next step (AimTrace.h).");
        return;
    }
    // Rows with trigger-window hits print first: those are the candidates for
    // the per-shot reader. No sort and no allocation, two passes over 48
    // entries, on our own thread, once every two seconds.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < n && i < kMaxRows; ++i) {
            const bool cand = g_rows[i].fire > 0;
            if ((pass == 0) != cand) continue;
            LOG_INFO("  %s %s rva=0x%08X  n=%llu  fire=%llu",
                     cand ? "*" : " ",
                     g_rows[i].index == 0 ? "yaw  " : "pitch",
                     (unsigned)g_rows[i].rva,
                     (unsigned long long)g_rows[i].n,
                     (unsigned long long)g_rows[i].fire);
        }
    }
    if (g_foreign)
        LOG_INFO("  (%llu call(s) came from outside GRW.exe)",
                 (unsigned long long)g_foreign);

    // Build 53: who invokes the per-shot weapon routine, and with what.
    if (g_wfire_hook.installed()) {
        float f1 = 0.0f;
        const uint32_t bits = g_wf_f1;
        memcpy(&f1, &bits, 4);
        LOG_INFO("  wfire: calls=%llu this=0x%llX ctx=0x%llX arg_float=%.4f",
                 (unsigned long long)g_wf_calls,
                 (unsigned long long)g_wf_self,
                 (unsigned long long)g_wf_ctx, f1);
        const int wn = g_wf_count;
        for (int i = 0; i < wn && i < kMaxWf; ++i)
            LOG_INFO("    called from rva=0x%08X  n=%llu",
                     (unsigned)g_wf[i].rva, (unsigned long long)g_wf[i].n);
    }

    // Build 55: the aim orientation quaternion.
    if (g_aimq_hook.installed()) {
        float q[4];
        for (int k = 0; k < 4; ++k) q[k] = g_aq_last[k];
        LOG_INFO("  aimquat: calls=%llu applied=%llu %s last=(%.3f %.3f "
                 "%.3f %.3f)",
                 (unsigned long long)g_aq_calls,
                 (unsigned long long)g_aq_applied,
                 g_aq_armed ? "ARMED" : "inert", q[0], q[1], q[2], q[3]);
        const int an = g_aq_rowcount;
        for (int i = 0; i < an && i < kMaxAq; ++i)
            LOG_INFO("    consumer rva=0x%08X  n=%llu",
                     (unsigned)g_aq_rows[i].rva,
                     (unsigned long long)g_aq_rows[i].n);
    }

    // Build 57: harvest the window around the last shot. Done HERE, on our
    // own thread, so the hot paths stay two stores. The ring may have wrapped
    // if the game cast thousands of rays in the meantime; that only costs us
    // one sample, and the tallies accumulate over every shot regardless.
    if (g_pending_live) {
        const uint32_t mark = g_pending_mark;
        const uint32_t now  = g_ring_idx;
        if (now - mark >= 32 || now == mark) {      // the window has filled
            for (int d = -32; d < 32; ++d) {
                const uint32_t at = mark + (uint32_t)d;
                if (at >= now) continue;            // not written yet
                if (now - at > (uint32_t)kRing) continue;   // wrapped away
                const uint32_t rva = g_ring[at & (kRing - 1)];
                if (!rva) continue;
                int slot = -1;
                for (int i = 0; i < g_win_count && i < kMaxRay; ++i)
                    if (g_win_rva[i] == rva) { slot = i; break; }
                if (slot < 0 && g_win_count < kMaxRay) {
                    slot = g_win_count;
                    g_win_rva[slot] = rva;
                    g_win_count = slot + 1;
                }
                if (slot < 0) continue;
                if (d < 0) g_win_before[slot] = g_win_before[slot] + 1;
                else       g_win_after[slot]  = g_win_after[slot] + 1;
            }
            g_pending_live = 0;
        }
    }
    if (g_win_count > 0 && g_sp_calls > 0) {
        LOG_INFO("  shot window (32 casts either side of a spawn, summed over "
                 "%llu shot(s)):", (unsigned long long)g_sp_calls);
        for (int i = 0; i < g_win_count && i < kMaxRay; ++i)
            LOG_INFO("    rva=0x%08X  before=%llu  after=%llu",
                     (unsigned)g_win_rva[i],
                     (unsigned long long)g_win_before[i],
                     (unsigned long long)g_win_after[i]);
    }

    // Build 56: THE BULLET.
    if (g_spawn_hook.installed()) {
        float d[4], o[4];
        for (int k = 0; k < 4; ++k) { d[k] = g_sp_dir[k]; o[k] = g_sp_org[k]; }
        LOG_INFO("  bullet: spawns=%llu turned=%llu %s | dir=(%.3f %.3f %.3f) "
                 "origin=(%.1f %.1f %.1f)",
                 (unsigned long long)g_sp_calls,
                 (unsigned long long)g_sp_turned,
                 g_sp_armed ? "ARMED" : "log-only",
                 d[0], d[1], d[2], o[0], o[1], o[2]);
    }

    // Build 62: the controller-ray relocation state.
    if (g_cr_armed) {
        float cd[3] = {g_cr_dir[0], g_cr_dir[1], g_cr_dir[2]};
        LOG_INFO("  ctrlray: ARMED ray_ok=%u relocated=%llu denied=%llu "
                 "dist=%.1f m | ctrl dir=(%.3f %.3f %.3f)",
                 (unsigned)g_cr_ok,
                 (unsigned long long)g_fly_shots,
                 (unsigned long long)g_fly_denied,
                 (double)g_fly_dist, cd[0], cd[1], cd[2]);
    }

    // Build 59: print the completed flight trace as a per-offset diff. Only
    // offsets that CHANGED during flight print, as hex and float across
    // first, middle and last captured frame. Bounded to 48 rows; a trace
    // ended by a fault says so, because the fault frame is the round's
    // lifetime. Printing re-arms the recorder for the next spawn.
    if (g_trk_ready) {
        const int fn = (int)g_trk_n;
        LOG_INFO("  flight: %d frame(s) of projectile 0x%llX%s%s%s | "
                 "rejects=%llu steers=%llu",
                 fn, (unsigned long long)g_trk_proj,
                 g_trk_fault ? " | ended by FAULT" : "",
                 g_trk_died  ? " | round DIED (instance recycled)" : "",
                 (!g_trk_fault && !g_trk_died && fn < kTrkFrames)
                     ? " | partial" : "",
                 (unsigned long long)g_trk_rej,
                 (unsigned long long)g_trk_steer);
        if (fn >= 2) {
            int printed = 0, changed_total = 0;
            const int mid = fn / 2;
            for (int off = 0; off + 4 <= kTrkBytes; off += 4) {
                uint32_t h0, hm, hl;
                memcpy(&h0, &g_trk_buf[0][off], 4);
                bool changed = false;
                for (int f = 1; f < fn; ++f) {
                    uint32_t v;
                    memcpy(&v, &g_trk_buf[f][off], 4);
                    if (v != h0) { changed = true; break; }
                }
                if (!changed) continue;
                ++changed_total;
                if (printed >= 48) continue;
                memcpy(&hm, &g_trk_buf[mid][off], 4);
                memcpy(&hl, &g_trk_buf[fn - 1][off], 4);
                float f0, fm2, fl;
                memcpy(&f0, &h0, 4); memcpy(&fm2, &hm, 4); memcpy(&fl, &hl, 4);
                LOG_INFO("    off=0x%03X  %08X %08X %08X  as float "
                         "%.6g -> %.6g -> %.6g",
                         off, h0, hm, hl, (double)f0, (double)fm2, (double)fl);
                ++printed;
            }
            if (changed_total > printed)
                LOG_INFO("    ...and %d more changed offset(s) not shown",
                         changed_total - printed);
            if (changed_total == 0)
                LOG_INFO("    no offset changed across %d frames (instance "
                         "is static after spawn, or already recycled)", fn);
        }
        g_trk_ready = 0;
        g_trk_proj  = 0;
    }

    // Build 54: who casts rays, and what ray. THE ONE TO READ: a caller whose
    // count matches the number of rounds fired is the shot trace, exactly the
    // discriminator that found the per-shot aim reader. Per-frame world
    // queries (wheels, AI sight) will sit in the thousands.
    if (g_ray_hook.installed()) {
        const int rn = g_ray_count;
        LOG_INFO("  castRay: total=%llu across %d caller(s)",
                 (unsigned long long)g_ray_calls, rn);
        for (int i = 0; i < rn && i < kMaxRay; ++i) {
            float v[8];
            for (int k = 0; k < 8; ++k) v[k] = g_ray[i].v[k];
            LOG_INFO("    from rva=0x%08X n=%llu  [%.1f %.1f %.1f %.1f]"
                     " [%.1f %.1f %.1f %.1f]",
                     (unsigned)g_ray[i].rva, (unsigned long long)g_ray[i].n,
                     v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        }
    }
}

void set_logging(bool on) { g_logging = on; }

// 2026-09-01: whether the guided anchor test announces itself. Default off.
void set_voice_guide(int on) { g_voice_guide = on ? 1u : 0u; }

void set_shot_offset(float yaw_rad, float pitch_rad) {
    uint32_t y = 0, p = 0;
    if (g_shot_yaw_site   && yaw_rad   != 0.0f) memcpy(&y, &yaw_rad,   4);
    if (g_shot_pitch_site && pitch_rad != 0.0f) memcpy(&p, &pitch_rad, 4);
    const bool was = (g_shot_dyaw | g_shot_dpitch) != 0;
    g_shot_dyaw   = y;
    g_shot_dpitch = p;
    const bool now = (y | p) != 0;
    if (was != now)
        LOG_INFO("aimtrace: per-shot aim override %s (yaw %+.2f deg, pitch "
                 "%+.2f deg, %s). The VIEW is untouched: only the shot "
                 "reader's call sites see this.",
                 now ? "ARMED" : "disarmed",
                 yaw_rad * 57.29578f, pitch_rad * 57.29578f,
                 g_shot_alternate ? "ALTERNATING sign per round"
                                  : "constant sign");
}

void set_shot_alternate(bool on) { g_shot_alternate = on ? 1u : 0u; }

// Build 72: the direction the GAME is currently aiming, in engine world space,
// published from the same per-frame pass as the controller ray so the two are
// always from one instant. The weapon rotation is built as the delta between
// them, which is why it needs no knowledge of the engine's axis convention.
volatile uint32_t g_vf_ok = 0;
float g_vf_dir[3] = {};

void set_view_fwd(const float dir[3], bool ok) {
    if (ok && dir) {
        g_vf_dir[0] = dir[0];
        g_vf_dir[1] = dir[1];
        g_vf_dir[2] = dir[2];
        g_vf_ok = 1;
    } else {
        g_vf_ok = 0;
    }
}

uint32_t shots_total() { return (uint32_t)(g_sd_wrote_n + g_sd_left_n); }

bool view_fwd(float out[3]) {
    if (!g_vf_ok || !out) return false;
    out[0] = g_vf_dir[0];
    out[1] = g_vf_dir[1];
    out[2] = g_vf_dir[2];
    return true;
}

// Build 84: the barrel direction, published by grwxr_wgun_apply after its
// filter. See AimTrace.h for why this is not the same thing as ctrl_ray.
//
// Freshness is a GENERATION COUNTER rather than a timestamp: no clock call on
// a path entered 144 times a second, and it answers the only question the
// reader has, which is "did the weapon writer run since I last looked". With
// wgun = 0 nothing publishes, the counter stands still, and the reader falls
// back instead of aiming at a barrel direction from minutes ago.
volatile uint32_t g_bd_gen = 0;
float             g_bd_dir[3] = {};

void set_barrel_dir(const float dir[3], bool ok) {
    if (!ok || !dir) return;
    g_bd_dir[0] = dir[0];
    g_bd_dir[1] = dir[1];
    g_bd_dir[2] = dir[2];
    // Bumped AFTER the vector, so a reader that sees a new generation is
    // reading the vector that came with it, not the one before.
    _ReadWriteBarrier();
    g_bd_gen = g_bd_gen + 1;
}

// BUILD 153b: THE BARREL DIRECTION IN THE CAMERA'S OWN AXES.
//
// 2026-09-02, owner: "turning the character left or right made the green
// reticle go the opposite way and be misaligned from the barrel", while the
// gaze ring behaved. That is a TIMING bug, not an aim bug, and the asymmetry
// is the proof: the gaze ring is DEFINED by the camera basis so it cannot
// disagree with itself, while the barrel ring was a WORLD vector re-projected
// through a basis captured at a different instant. Turn the view and the two
// instants differ by the rotation you just made, so the ring swings by exactly
// that, opposite the turn.
//
// The fix is to stop re-projecting. The weapon writer already holds the basis
// it used, so it publishes the direction in camera axes at the same moment and
// the drawing side consumes it with no conversion of its own: x right,
// y forward, z up, one instant, no round trip.
//
// It is deliberately NOT a replacement for g_bd_dir. That stays world space
// because the shot chain needs a world vector, and the hard baseline guards
// that path. This is a drawing convenience and nothing may aim from it.
volatile uint32_t g_bl_gen = 0;
float             g_bl_dir[3] = {};

void set_barrel_local(const float dir[3], bool ok) {
    if (!ok || !dir) return;
    g_bl_dir[0] = dir[0];
    g_bl_dir[1] = dir[1];
    g_bl_dir[2] = dir[2];
    _ReadWriteBarrier();
    g_bl_gen = g_bl_gen + 1;
}

bool barrel_local_peek(float out[3], uint32_t* gen) {
    if (!out) return false;
    const uint32_t g0 = g_bl_gen;
    out[0] = g_bl_dir[0];
    out[1] = g_bl_dir[1];
    out[2] = g_bl_dir[2];
    _ReadWriteBarrier();
    const uint32_t g1 = g_bl_gen;
    if (gen) *gen = g1;
    return g0 == g1 && g1 != 0;
}

// BUILD 137: the non-consuming form. See AimTrace.h for why a second consumer
// could not share the accessor below. Reads the generation AFTER the vector,
// against the barrier in the publisher, so a generation that matches on both
// sides of the copy means the three floats belong together.
bool barrel_dir_peek(float out[3], uint32_t* gen) {
    if (!out) return false;
    const uint32_t g0 = g_bd_gen;
    out[0] = g_bd_dir[0];
    out[1] = g_bd_dir[1];
    out[2] = g_bd_dir[2];
    _ReadWriteBarrier();
    const uint32_t g1 = g_bd_gen;
    if (gen) *gen = g1;
    return g0 == g1 && g1 != 0;
}

// BUILD 153: the player's own muzzle in world space, for the evidence
// reticle. g_sd_muz is already maintained for build 137's interlock 1, the
// gate that decides whether to move a bullet, so it is the PLAYER-FLAGGED
// muzzle rather than g_ap_muzzle's "most recent, any owner". This only
// exposes it, and it is deliberately NOT a gate: the consumer is a drawing
// path, so a torn read costs a marker a few centimetres for one frame, which
// is invisible next to the blob it draws. Anything deciding where a BULLET
// goes must use the interlock, never this.
bool player_muzzle(float out[3]) {
    if (!out || !g_sd_muz_ok) return false;
    out[0] = g_sd_muz[0];
    out[1] = g_sd_muz[1];
    out[2] = g_sd_muz[2];
    return true;
}

void set_shot_pitch(bool on) {
    const uint32_t want = on ? 1u : 0u;
    if (g_vt_armed == want) return;
    g_vt_armed = want;
    LOG_INFO("shot_pitch: %s. %s",
             want ? "ARMED" : "off",
             want ? "The ballistic arc is now rebuilt on the barrel's own "
                    "elevation, on shots that already passed the player gate."
                  : "The engine's own launch elevation is left alone, which "
                    "is build 153 behaviour: the round's up and down follows "
                    "the camera.");
}

void set_shot_dir(bool on) {
    const uint32_t want = on ? 1u : 0u;
    if (g_sd_armed == want) return;
    g_sd_armed = want;
    LOG_INFO("shot_dir: %s. %s",
             want ? "ARMED" : "off",
             want ? "The shot direction now follows the barrel on any shot "
                    "that starts at your own muzzle."
                  : "The engine's own shot direction is left alone.");
}

bool shot_dir() { return g_sd_armed != 0; }

// BUILD 152: the origin write. See AimTrace.h.
void set_shot_org(bool on) {
    const uint32_t want = on ? 1u : 0u;
    if (g_sd_org_armed == want) return;
    g_sd_org_armed = want;
    LOG_INFO("shot_org: %s. %s",
             want ? "ARMED" : "off",
             want ? "A player shot the engine builds at the CAMERA is moved "
                    "back to the muzzle, origin and direction both."
                  : "Camera-origin shots are refused, never repaired (the "
                    "build 137 behaviour).");
}

bool shot_org() { return g_sd_org_armed != 0; }

bool barrel_dir(float out[3]) {
    // Starts at 0, which is also the generation before anything has ever been
    // published. So "wgun has never run" reads as stale and returns false,
    // rather than handing back the zero vector the array was born with. That
    // zero would have passed a naive length check nowhere but would have gone
    // straight into an atan2 as a direction.
    static uint32_t s_seen = 0;
    if (!out) return false;
    const uint32_t g = g_bd_gen;
    if (g == s_seen) return false;        // wgun has not run since the last read
    s_seen = g;
    out[0] = g_bd_dir[0];
    out[1] = g_bd_dir[1];
    out[2] = g_bd_dir[2];
    return true;
}

void set_ctrl_ray(const float dir[3], bool ok) {
    if (ok && dir) {
        g_cr_dir[0] = dir[0];
        g_cr_dir[1] = dir[1];
        g_cr_dir[2] = dir[2];
        g_cr_ok = 1;
    } else {
        g_cr_ok = 0;
    }
}

// Build 68: the same ray the bullets ride, exposed so the weapon's rendered
// orientation can be built from it. Sharing one source is the point: the gun
// and the rounds then cannot diverge, which is the defect class that produced
// the tester's ADS complaint.
bool ctrl_ray(float out[3]) {
    if (!g_cr_ok || !out) return false;
    out[0] = g_cr_dir[0];
    out[1] = g_cr_dir[1];
    out[2] = g_cr_dir[2];
    return true;
}

void set_ctrl_pos(const float p[3], bool ok) {
    if (ok && p) {
        g_cr_pos[0] = p[0];
        g_cr_pos[1] = p[1];
        g_cr_pos[2] = p[2];
    }
    g_cr_pos_ok = ok ? 1u : 0u;
}

bool ctrl_pos(float out[3]) {
    if (!g_cr_pos_ok || !out) return false;
    out[0] = g_cr_pos[0];
    out[1] = g_cr_pos[1];
    out[2] = g_cr_pos[2];
    return true;
}

// 2026-08-13: the LEFT controller's position and the RIGHT controller's up
// vector, published from the same pass and the same basis as the two above, so
// a two-handed weapon can never be assembled out of two different instants.
// Same contract as the rest of this file: ok=false means "not tracked THIS
// frame" and the last good value is deliberately NOT held, so a consumer sees
// a dropout rather than a stale pose it cannot distinguish from a real one.
void set_ctrl_pos_l(const float p[3], bool ok) {
    if (ok && p) {
        g_cl_pos[0] = p[0];
        g_cl_pos[1] = p[1];
        g_cl_pos[2] = p[2];
    }
    g_cl_pos_ok = ok ? 1u : 0u;
}

bool ctrl_pos_l(float out[3]) {
    if (!g_cl_pos_ok || !out) return false;
    out[0] = g_cl_pos[0];
    out[1] = g_cl_pos[1];
    out[2] = g_cl_pos[2];
    return true;
}

// BUILD 158: the LEFT controller's forward and up, same contract as the rest
// of this file: ok=false is a dropout, never a held stale frame.
void set_ctrl_vecs_l(const float fwd[3], const float up[3], bool ok) {
    if (ok && fwd && up) {
        g_cl_fwd[0] = fwd[0]; g_cl_fwd[1] = fwd[1]; g_cl_fwd[2] = fwd[2];
        g_cl_up[0]  = up[0];  g_cl_up[1]  = up[1];  g_cl_up[2]  = up[2];
    }
    g_cl_vec_ok = ok ? 1u : 0u;
}

bool ctrl_vecs_l(float fwd[3], float up[3]) {
    if (!g_cl_vec_ok || !fwd || !up) return false;
    fwd[0] = g_cl_fwd[0]; fwd[1] = g_cl_fwd[1]; fwd[2] = g_cl_fwd[2];
    up[0]  = g_cl_up[0];  up[1]  = g_cl_up[1];  up[2]  = g_cl_up[2];
    return true;
}

// BUILD 162: the LEFT controller's grip-pose frame, +Y and +Z axes.
void set_ctrl_gripf_l(const float y[3], const float z[3], bool ok) {
    if (ok && y && z) {
        g_cl_gy[0] = y[0]; g_cl_gy[1] = y[1]; g_cl_gy[2] = y[2];
        g_cl_gz[0] = z[0]; g_cl_gz[1] = z[1]; g_cl_gz[2] = z[2];
    }
    g_cl_grip_ok = ok ? 1u : 0u;
}

bool ctrl_gripf_l(float y[3], float z[3]) {
    if (!g_cl_grip_ok || !y || !z) return false;
    y[0] = g_cl_gy[0]; y[1] = g_cl_gy[1]; y[2] = g_cl_gy[2];
    z[0] = g_cl_gz[0]; z[1] = g_cl_gz[1]; z[2] = g_cl_gz[2];
    return true;
}

void set_ctrl_up(const float u[3], bool ok) {
    if (ok && u) {
        g_cr_up[0] = u[0];
        g_cr_up[1] = u[1];
        g_cr_up[2] = u[2];
    }
    g_cr_up_ok = ok ? 1u : 0u;
}

bool ctrl_up(float out[3]) {
    if (!g_cr_up_ok || !out) return false;
    out[0] = g_cr_up[0];
    out[1] = g_cr_up[1];
    out[2] = g_cr_up[2];
    return true;
}

void set_bullet_ctrl(bool on) {
    const uint32_t was = g_cr_armed;
    g_cr_armed = on ? 1u : 0u;
    if (on && !was)
        LOG_INFO("aimtrace: CONTROLLER-RAY BULLETS ARMED. Every round is "
                 "relocated per frame onto the controller ray captured at "
                 "its spawn; the engine keeps drop and drag. Guards: "
                 "forward-hemisphere check at spawn, +0x50 liveness check "
                 "before every write, 48-frame window.");
    if (!on && was)
        LOG_INFO("aimtrace: controller-ray bullets disarmed.");
}

void set_bullet_yaw(float deg) {
    if (deg == 0.0f) {
        if (g_sp_armed) LOG_INFO("aimtrace: bullet yaw override disarmed");
        g_sp_armed = 0;
        return;
    }
    const float r = deg * 0.01745329f;
    g_sp_cos = cosf(r);
    g_sp_sin = sinf(r);
    g_sp_armed = 1;
    LOG_INFO("aimtrace: BULLET YAW OVERRIDE ARMED, %+.1f deg. This rewrites "
             "m_vBulletSimulationDirection at the spawn and restores the "
             "engine's own value immediately after, so nothing stays "
             "modified. If impacts move, the bullet is ours.", deg);
}

// BUILD 82: step the spawn-direction yaw from a key, because settling "is
// owner+0x140 the authoritative shot direction" means firing under two
// opposite offsets and comparing, and doing that by editing a text file means
// taking the headset off between the two halves of the comparison.
//
// The sequence is 0, +20, -20 deliberately. A single offset can only be judged
// against a remembered impact point, and this project has already learned that
// a tester judging from memory is a measurement that lies. Opposite signs make
// the round its own reference: left then right is unambiguous, and a constant
// misalignment cannot alternate.
void cycle_bullet_yaw() {
    static const float kSteps[3] = {0.0f, 20.0f, -20.0f};
    static int idx = 0;
    idx = (idx + 1) % 3;
    set_bullet_yaw(kSteps[idx]);
    LOG_INFO("aimtrace: NUMPAD 4 -> bullet_yaw %+.0f deg. Fire a group at a "
             "wall: if impacts sit off to one side and SWAP sides on the next "
             "press, m_vBulletSimulationDirection is the shot direction and we "
             "can write the controller ray straight into it.", kSteps[idx]);
}

void set_aim_quat(float deg, int axis) {
    if (deg == 0.0f) {
        if (g_aq_armed) LOG_INFO("aimtrace: aim quaternion override disarmed");
        g_aq_armed = 0;
        return;
    }
    if (axis < 0 || axis > 2) axis = 2;
    const float half = deg * 0.5f * 0.01745329f;
    const float s = sinf(half), c = cosf(half);
    g_aq_rot[0] = (axis == 0) ? s : 0.0f;
    g_aq_rot[1] = (axis == 1) ? s : 0.0f;
    g_aq_rot[2] = (axis == 2) ? s : 0.0f;
    g_aq_rot[3] = c;
    g_aq_armed  = 1;
    LOG_INFO("aimtrace: aim quaternion override ARMED, %+.1f deg about the "
             "%c axis. If IMPACTS move while the VIEW does not, the ballistic "
             "direction is downstream of GetAimOrientation.",
             deg, axis == 0 ? 'X' : (axis == 1 ? 'Y' : 'Z'));
}

// Build 52. Retargets the override. 0 restores the build-pinned per-shot
// site for that axis. Its real purpose is the CONTROL TEST: point the same
// machinery at a read the engine is known to depend on (the look-input
// integrator, RE-notes 0x124D34BB / 0x124D34E7) and confirm that bending it
// visibly breaks the aim. If the control does nothing either, the override
// mechanism itself is broken and build 51's negative means nothing.
//
// Every candidate goes through the SAME rule-7 check as the pinned sites:
// the five bytes in front must be an E8 that resolves to the getter stub we
// hooked, so a mistyped address disarms rather than corrupting a random call.
void set_shot_sites(unsigned yaw_rva, unsigned pitch_rva) {
    const gamebuild::Build* gb = gamebuild::get();
    if (!gb || !g_base) return;
    static unsigned s_last_y = 0xFFFFFFFFu, s_last_p = 0xFFFFFFFFu;
    if (yaw_rva == s_last_y && pitch_rva == s_last_p) return;   // no churn
    s_last_y = yaw_rva;
    s_last_p = pitch_rva;

    const uintptr_t y = yaw_rva   ? yaw_rva   : gb->shot_yaw_site;
    const uintptr_t p = pitch_rva ? pitch_rva : gb->shot_pitch_site;
    const bool custom = (yaw_rva || pitch_rva);
    if (custom)
        LOG_INFO("aimtrace: RETARGETING the override to yaw 0x%08llX / pitch "
                 "0x%08llX (control test; 0 restores the per-shot sites)",
                 (unsigned long long)y, (unsigned long long)p);
    g_shot_yaw_site   = verify_site(g_base, y, gb->getyaw_slot,   "yaw")
                            ? (uint32_t)y : 0u;
    g_shot_pitch_site = verify_site(g_base, p, gb->getpitch_slot, "pitch")
                            ? (uint32_t)p : 0u;
}

bool shot_sites_ready() {
    return g_shot_yaw_site != 0 || g_shot_pitch_site != 0;
}

void set_firing(bool held) { g_firing = held ? 1u : 0u; }

// Build 105. Arming RESETS the accounting, so a verdict is never read off
// counters that accumulated across an earlier configuration.
void set_aimpoint_ab(bool on, float shift_m) {
    if (shift_m < 0.5f)  shift_m = 0.5f;
    if (shift_m > 25.0f) shift_m = 25.0f;
    g_ap_shift_m = shift_m;
    const uint32_t want = on ? 1u : 0u;
    if (g_ap_armed != want) {
        g_ap_written = 0; g_ap_shots = 0; g_ap_side = 0; g_ap_prev_fire = 0;
        g_ap_armed = want;
        voice::say(want ? "Shot test armed. Fire ten single shots at a "
                          "wall."
                        : "Shot test disarmed.");
        LOG_INFO("aimpoint: A/B %s (%.1f m alternating left/right per trigger"
                 " pull, player-gated). THE QUESTION: fire ten single shots"
                 " at a wall. ONE group of holes or TWO?",
                 want ? "ARMED" : "disarmed", g_ap_shift_m);
    }
}

bool aimpoint_ab() { return g_ap_armed != 0; }

// Build 107: the End key. Resets the capture table so a second run in one
// session is not read off the first run's slots, then hands the protocol to
// the guide in drain_aimpoint.
void guide_start() {
    for (int i = 0; i < kApSlots; ++i) g_ap_slot[i] = ApSlot{};
    g_ap_slots_used = 0;
    g_ap_slots_full = 0;
    g_guide_state = 1;
    LOG_INFO("aimanchor: GUIDED RUN STARTED by hotkey. Capture table "
             "cleared; the next two weapons drawn will be captured.");
}
bool firing()              { return g_firing != 0u; }

void uninstall() {
    g_getyaw_hook.restore();
    g_getpitch_hook.restore();
}

}  // namespace aimtrace
}  // namespace grwxr
