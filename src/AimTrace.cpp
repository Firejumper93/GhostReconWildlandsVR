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

// Build 55: GetAimOrientation (see ProbeStub.asm). post() rotates the
// quaternion the engine just produced, in the caller's own buffer.
uint64_t grwxr_aimq_orig = 0;
void grwxr_aimq_entry();
void grwxr_aimq_post(float* q, uint64_t ret_addr);

// Build 56: the ballistic projectile spawn (see ProbeStub.asm).
uint64_t grwxr_spawn_orig = 0;
void grwxr_spawn_entry();
void grwxr_spawn_pre(void* owner);
void grwxr_spawn_post(void* owner);
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
constexpr int kMaxRay = 12;
struct RayRow {
    uint32_t rva;
    uint64_t n;
    float    v[8];        // first 32 bytes of the ray input struct
};
volatile RayRow   g_ray[kMaxRay] = {};
volatile int      g_ray_count = 0;
volatile uint64_t g_ray_calls = 0;

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
// keep a small ring of recent caller RVAs, mark the ring position at every
// projectile spawn, and count what happened either side of that mark. A
// shot-specific caller spikes in the window; ambient traffic does not.
constexpr int kRing = 256;
volatile uint32_t g_ring[kRing] = {};
volatile uint32_t g_ring_idx = 0;
// Per-caller tallies inside the windows, accumulated over every shot.
volatile uint32_t g_win_rva[kMaxRay] = {};
volatile uint64_t g_win_before[kMaxRay] = {};
volatile uint64_t g_win_after[kMaxRay] = {};
volatile int      g_win_count = 0;
volatile uint32_t g_pending_mark = 0;   // ring index captured at the last shot
volatile uint32_t g_pending_live = 0;

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
// stores. No logging, no locks, no allocation, no calls out (project rule 8).
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
}

// Build 55. Post-multiplies the engine's own aim quaternion by our rotation,
// which is the same order the engine composes in (base * yaw * pitch * roll),
// so an extra term on the right reads as "more yaw" in its own convention.
// Rule 8: a quaternion product and four stores, nothing else.
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

extern "C" void grwxr_spawn_post(void* owner) {
    using namespace grwxr::aimtrace;
    if (!owner || !g_sp_didwrite) return;
    __try {
        float* dir = (float*)((uint8_t*)owner + 0x140);
        for (int k = 0; k < 4; ++k) dir[k] = g_sp_saved[k];
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_sp_didwrite = 0;
}

namespace grwxr {
namespace aimtrace {

bool install() {
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

    g_getyaw_hook.install_raw(base + gb->getyaw_slot, kGetYawExpect,
                              sizeof(kGetYawExpect),
                              (void*)&grwxr_aimget_yaw_entry,
                              "GetYaw vdispatch");
    g_getpitch_hook.install_raw(base + gb->getpitch_slot, kGetPitchExpect,
                                sizeof(kGetPitchExpect),
                                (void*)&grwxr_aimget_pitch_entry,
                                "GetPitch vdispatch");

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

void uninstall() {
    g_getyaw_hook.restore();
    g_getpitch_hook.restore();
}

}  // namespace aimtrace
}  // namespace grwxr
