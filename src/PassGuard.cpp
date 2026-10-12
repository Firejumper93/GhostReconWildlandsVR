// PassGuard.cpp - BUILD 259, see PassGuard.h.
//
// PINS (TU25 GRW.exe, TimeDateStamp 0x6A99768A, read from the exe on disk 2026-10-07 with tools\disasm_at.py,
// pdata_lookup.py and find_callers.py; RVAs never carry to another build):
//   0x014DFF20  the body's jump thunk: E9 FB A7 AD 0C, then int3 padding. find_callers: the ONLY rel32
//               reference to the body, and the thunk's own only reference is a tail jmp at 0x0DFBA613.
//   0x0DFBA720  FUN_14dfba720, the ProjShadow pass body. Prologue 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57.
//               rcx = the renderer (mov r14, rcx at +0x29), rdx/r8/r9 = the pass context, no stack arguments
//               (it only saves rbx/rbp/rsi into the caller's home slots).
//   0x0DFBA82E  49 8B 8E 60 0B 00 00 = mov rcx, [r14+0xB60], the atlas read; the call chain after it ends in
//               the 32-bit compare at 0xE7C1810 that faults with rcx = 0x18 when [r14+0xB60] is NULL.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "D3D11Hook.h"
#include "GameBuild.h"
#include "Log.h"
#include "PassGuard.h"
#include "ThunkHook.h"

namespace grwxr {
namespace passguard {

namespace {

constexpr uint32_t kPeStamp = 0x6A99768Au;
constexpr uint32_t kPeSize  = 0x185BA000u;

constexpr uint32_t kThunkRva = 0x014DFF20u;
constexpr uint32_t kBodyRva  = 0x0DFBA720u;
constexpr uint32_t kReadRva  = 0x0DFBA82Eu;

const uint8_t kThunkBytes[16] = {0xE9, 0xFB, 0xA7, 0xAD, 0x0C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
const uint8_t kBodyBytes[16]  = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
const uint8_t kReadBytes[7]   = {0x49, 0x8B, 0x8E, 0x60, 0x0B, 0x00, 0x00};

// renderer fields (Cam2.cpp pins, RENDER-2026-10-05 section 4d)
constexpr uint32_t kRendAtlas = 0xB60, kRendMask = 0x8C0, kRendCull = 0xD90, kRendSlot = 0xD98;
constexpr uint32_t kCullNProj = 0x8570;

constexpr unsigned long long kMinFrames = 600;   // Present entries before the pins are verified and the thunk is written
constexpr int kSnaps = 8;                        // how many skips keep a state snapshot

enum : int { kWait = 0, kInstalled, kRefused };

std::atomic<int>      g_state{kWait};
std::atomic<bool>     g_on{true};
std::atomic<uint32_t> g_calls{0};      // entries into the body (every pass that reached it)
std::atomic<uint32_t> g_skips{0};      // entries skipped because renderer+0xB60 was NULL
std::atomic<uint32_t> g_readfail{0};   // the atlas read itself faulted (treated as present: the body runs)
hook::ThunkHook       g_hook;

struct Snap {
    unsigned long long frame;
    uint64_t           rend;
    uint32_t           tid;
    uint32_t           b8c0;     // 0xFFFFFFFF = unread
    uint32_t           d98;
    uint32_t           nproj;
    uint64_t           d90;
};
Snap                  g_snap[kSnaps];
std::atomic<uint32_t> g_snap_ready[kSnaps];

// SEH helpers: no C++ objects with destructors in these frames.
bool rd_u64(uint64_t addr, uint64_t* out) {
    __try {
        *out = *(volatile const uint64_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool rd_u32(uint64_t addr, uint32_t* out) {
    __try {
        *out = *(volatile const uint32_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool rd_u8(uint64_t addr, uint32_t* out) {
    __try {
        *out = *(volatile const uint8_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void note_skip(uint64_t rend) {
    const uint32_t n = g_skips.fetch_add(1, std::memory_order_relaxed);
    if (n >= (uint32_t)kSnaps) return;
    Snap& s = g_snap[n];
    s.frame = d3d11::frame_count();
    s.rend  = rend;
    s.tid   = GetCurrentThreadId();
    s.b8c0 = s.d98 = s.nproj = 0xFFFFFFFFu;
    s.d90  = 0;
    rd_u8(rend + kRendMask, &s.b8c0);
    rd_u32(rend + kRendSlot, &s.d98);
    uint64_t cull = 0;
    if (rd_u64(rend + kRendCull, &cull)) {
        s.d90 = cull;
        if (cull) rd_u32(cull + kCullNProj, &s.nproj);
    }
    g_snap_ready[n].store(1, std::memory_order_release);
}

// The replacement for the body's thunk. Same signature as the body (rcx = renderer, rdx, r8, r9, no stack
// arguments). The body's return value is not used by its caller; rax is passed through untouched on the
// normal path and zeroed on the skip.
using BodyFn = uint64_t (*)(void*, void*, void*, void*);

uint64_t hk_body(void* rend, void* a, void* b, void* c) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (rend && g_on.load(std::memory_order_relaxed)) {
        uint64_t atlas = 1;
        if (!rd_u64((uint64_t)(uintptr_t)rend + kRendAtlas, &atlas)) {
            g_readfail.fetch_add(1, std::memory_order_relaxed);
            atlas = 1;   // cannot read: do not change the engine's own behaviour
        }
        if (atlas == 0) {
            note_skip((uint64_t)(uintptr_t)rend);
            return 0;
        }
    }
    return ((BodyFn)g_hook.original())(rend, a, b, c);
}

bool bytes_match(uint64_t addr, const uint8_t* want, size_t n) {
    __try {
        return memcmp((const void*)addr, want, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void try_install() {
    const gamebuild::Build* gb = gamebuild::get();
    if (!gb || gb->pe_timestamp != kPeStamp || gb->pe_size_of_image != kPeSize) {
        g_state.store(kRefused, std::memory_order_release);
        LOG_ERROR("passguard: REFUSED: this GRW.exe is not the TU25 build the pins are keyed to (%s). Nothing installed.",
                  gb ? gb->name : "unknown");
        return;
    }
    const HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) {
        g_state.store(kRefused, std::memory_order_release);
        LOG_ERROR("passguard: REFUSED: no main module. Nothing installed.");
        return;
    }
    const uint64_t base = (uint64_t)(uintptr_t)mod;
    if (!bytes_match(base + kThunkRva, kThunkBytes, sizeof(kThunkBytes)) ||
        !bytes_match(base + kBodyRva, kBodyBytes, sizeof(kBodyBytes)) ||
        !bytes_match(base + kReadRva, kReadBytes, sizeof(kReadBytes))) {
        g_state.store(kRefused, std::memory_order_release);
        LOG_ERROR("passguard: REFUSED: the thunk, the body prologue or the atlas-read instruction does not match the TU25 pins. "
                  "Nothing installed.");
        return;
    }
    if (!g_hook.install((uint8_t*)(base + kThunkRva), (void*)(base + kBodyRva), (void*)&hk_body, "projshadow-pass-body")) {
        g_state.store(kRefused, std::memory_order_release);
        LOG_ERROR("passguard: REFUSED: the thunk redirect did not install. Nothing changed.");
        return;
    }
    g_state.store(kInstalled, std::memory_order_release);
    LOG_INFO("passguard: INSTALLED (build 259). The ProjShadow pass body (RVA 0x%X, thunk 0x%X) is skipped whenever renderer+0x%X "
             "is NULL; otherwise it runs untouched. Guard %s.",
             kBodyRva, kThunkRva, kRendAtlas, g_on.load() ? "ON" : "OFF (pass-through)");
}

}  // namespace

void set_enabled(bool on) { g_on.store(on, std::memory_order_relaxed); }

void tick() {
    static uint32_t s_last_calls = 0, s_last_skips = 0, s_logged_snaps = 0, s_quiet = 0;
    const int st = g_state.load(std::memory_order_acquire);
    if (st == kRefused) return;
    if (st == kWait) {
        if (d3d11::frame_count() < kMinFrames) return;
        try_install();
        return;
    }
    // kInstalled: log the first skips in full, then the counters when they move and every 30 s otherwise.
    const uint32_t calls = g_calls.load(std::memory_order_relaxed);
    const uint32_t skips = g_skips.load(std::memory_order_relaxed);
    while (s_logged_snaps < (uint32_t)kSnaps && g_snap_ready[s_logged_snaps].load(std::memory_order_acquire)) {
        const Snap& s = g_snap[s_logged_snaps];
        LOG_WARN("passguard: SKIP #%u: a ProjShadow pass reached its body with renderer+0xB60 (the atlas) NULL, so the body was skipped "
                 "instead of faulting at the null compare. frame %llu tid %u renderer 0x%llX | 8c0 byte 0x%02X (bit 4 = passes allowed) "
                 "bound cull 0x%llX slot %d projector count %d",
                 s_logged_snaps + 1, s.frame, s.tid, (unsigned long long)s.rend, s.b8c0 == 0xFFFFFFFFu ? 0xFFu : s.b8c0,
                 (unsigned long long)s.d90, (int)s.d98, (int)s.nproj);
        ++s_logged_snaps;
    }
    if (skips != s_last_skips || calls != s_last_calls || ++s_quiet >= 30) {
        LOG_INFO("passguard: body entries=%u skipped(atlas NULL)=%u read-faults=%u guard=%s",
                 calls, skips, g_readfail.load(std::memory_order_relaxed), g_on.load() ? "ON" : "OFF");
        s_last_calls = calls;
        s_last_skips = skips;
        s_quiet = 0;
    }
}

}  // namespace passguard
}  // namespace grwxr
