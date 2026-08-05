// XInputMerge.cpp - see the header. One pointer, one merge, no new files.

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "XInputMerge.h"
#include "HeadPose.h"
#include "Log.h"

namespace grwxr {
namespace xin {
namespace {

// Shadow-IAT slot for XInputGetState (ordinal 2), RE-notes 2026-08-03.
constexpr uintptr_t kSlotRva = 0x03851120;

// XINPUT_STATE, declared locally so this file needs no XInput import.
#pragma pack(push, 1)
struct Gamepad {
    uint16_t wButtons;
    uint8_t  bLeftTrigger;
    uint8_t  bRightTrigger;
    int16_t  sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
#pragma pack(pop)
struct State {
    uint32_t dwPacketNumber;
    Gamepad  Gamepad;
};
using PFN_GetState = DWORD(WINAPI*)(DWORD, State*);

constexpr DWORD kSuccess      = 0;      // ERROR_SUCCESS
constexpr DWORD kNotConnected = 1167;   // ERROR_DEVICE_NOT_CONNECTED

std::atomic<bool> g_enabled{true};      // cfg xinput_touch, default on
bool              g_installed = false;
void**            g_slot = nullptr;
PFN_GetState      g_orig = nullptr;

// Telemetry, written on the game's input thread, drained on ours.
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_merges{0};
std::atomic<uint64_t> g_fabricated{0};   // polls answered with no physical pad
std::atomic<uint64_t> g_placeheld{0};    // build 40: polls answered with a
                                         // neutral CONNECTED pad while Touch
                                         // was not live yet, so the game does
                                         // not latch "no controller"
std::atomic<uint32_t> g_last_btn{0};
std::atomic<uint32_t> g_pkt{0};

int16_t axis_i16(float v) {
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

uint8_t trig_u8(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < 0.0f) v = 0.0f;
    return (uint8_t)(v * 255.0f);
}

// Game input thread. Rule 8: no logging, no locks, no allocation.
DWORD WINAPI hook_GetState(DWORD idx, State* st) {
    const DWORD r = g_orig ? g_orig(idx, st) : kNotConnected;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (!st || idx != 0) return r;
    if (!g_enabled.load(std::memory_order_relaxed)) return r;

    uint32_t btn = 0;
    float    ax[6];
    const bool live = headpose::touch_pad(&btn, ax);

    if (!live) {
        // BUILD 40 FIX, THE STARTUP RACE THAT KILLED TOUCH INPUT ENTIRELY.
        //
        // Measured 2026-08-05 (log grwxr-25496): the merge installed at
        // 00:43:47, the XR session only reached FOCUSED at 00:44:17, and the
        // game polled XInputGetState exactly 23 times in between. Every one of
        // those polls fell through this branch, so the game saw
        // ERROR_DEVICE_NOT_CONNECTED, concluded there was no controller, and
        // NEVER POLLED AGAIN: calls stayed at 23 for the rest of the run even
        // in open-world gameplay. Touch was live 30 seconds later and nothing
        // was listening. That is why "motion controls didn't even work".
        //
        // The fix is to never let the game observe "no pad" while we intend to
        // supply one. Report a CONNECTED pad with neutral state until Touch
        // comes up, so the game keeps polling and simply reads no input.
        // A real pad's state is passed through untouched.
        if (r == kSuccess) return r;          // physical pad: never interfere
        memset(st, 0, sizeof(*st));
        st->dwPacketNumber = g_pkt.fetch_add(1, std::memory_order_relaxed) + 1;
        g_placeheld.fetch_add(1, std::memory_order_relaxed);
        return kSuccess;
    }

    if (r != kSuccess) {
        // No physical pad: fabricate a connected one from Touch alone.
        memset(st, 0, sizeof(*st));
        g_fabricated.fetch_add(1, std::memory_order_relaxed);
    }

    st->Gamepad.wButtons |= (uint16_t)btn;
    const uint8_t lt = trig_u8(ax[4]);
    const uint8_t rt = trig_u8(ax[5]);
    if (lt > st->Gamepad.bLeftTrigger)  st->Gamepad.bLeftTrigger  = lt;
    if (rt > st->Gamepad.bRightTrigger) st->Gamepad.bRightTrigger = rt;
    // Sticks: per axis, the larger magnitude wins, so a physical pad and the
    // Touch sticks coexist instead of averaging each other toward zero.
    const int16_t s[4] = {axis_i16(ax[0]), axis_i16(ax[1]),
                          axis_i16(ax[2]), axis_i16(ax[3])};
    int16_t* dst[4] = {&st->Gamepad.sThumbLX, &st->Gamepad.sThumbLY,
                       &st->Gamepad.sThumbRX, &st->Gamepad.sThumbRY};
    for (int i = 0; i < 4; ++i) {
        const int a = s[i] < 0 ? -s[i] : s[i];
        const int b = *dst[i] < 0 ? -*dst[i] : *dst[i];
        if (a > b) *dst[i] = s[i];
    }
    // Monotonic packet number. The pad object keeps its own previous/current
    // flag arrays per poll, so "changed every poll" costs nothing.
    st->dwPacketNumber = g_pkt.fetch_add(1, std::memory_order_relaxed) + 1;

    g_merges.fetch_add(1, std::memory_order_relaxed);
    g_last_btn.store(btn, std::memory_order_relaxed);
    return kSuccess;
}

// Readable-committed check before touching the slot (the RVA is verified
// offline, but the shadow IAT is resolved at runtime and this must fail
// soft, not fault).
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return ((uint8_t*)p + n) <= ((uint8_t*)mbi.BaseAddress + mbi.RegionSize);
}

bool try_install() {
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (!base) return false;
    void** slot = (void**)(base + kSlotRva);
    if (!readable(slot, sizeof(void*))) return false;
    void* cur = *slot;
    if (!cur) return false;                       // not resolved yet, retry

    // Identity check: the slot must point into the loaded XINPUT1_3.dll.
    HMODULE xm = GetModuleHandleW(L"XINPUT1_3.dll");
    if (!xm) return false;                        // not loaded yet, retry
    const auto* dos = (const IMAGE_DOS_HEADER*)xm;
    const auto* nt  = (const IMAGE_NT_HEADERS*)((const uint8_t*)xm + dos->e_lfanew);
    const uint8_t* lo = (const uint8_t*)xm;
    const uint8_t* hi = lo + nt->OptionalHeader.SizeOfImage;
    if ((uint8_t*)cur < lo || (uint8_t*)cur >= hi) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            LOG_WARN("xin: slot 0x%p holds 0x%p, OUTSIDE XINPUT1_3.dll "
                     "[0x%p..0x%p): identity check failed, NOT patching "
                     "(rule 7)", (void*)slot, cur, (void*)lo, (void*)hi);
        }
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG_WARN("xin: VirtualProtect on the slot failed (%lu), not patching",
                 GetLastError());
        return false;
    }
    g_orig = (PFN_GetState)cur;
    *slot  = (void*)&hook_GetState;
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_slot = slot;
    LOG_INFO("xin: XInputGetState slot 0x%p redirected: 0x%p (XINPUT1_3) -> "
             "0x%p (merge). Touch is now a gamepad; xinput_touch=0 in "
             "grwxr.cfg passes through.", (void*)slot, cur,
             (void*)&hook_GetState);
    return true;
}

}  // namespace

void poll_install() {
    static int  s_tries = 0;
    static bool s_gave_up = false;
    if (g_installed || s_gave_up) return;
    if (try_install()) { g_installed = true; return; }
    if (++s_tries >= 120) {                       // two minutes is plenty
        s_gave_up = true;
        LOG_WARN("xin: slot never became patchable after %d tries; Touch "
                 "gamepad disabled for this run", s_tries);
    }
}

void drain() {
    if (!g_installed) return;
    static int tick = 0;
    if ((++tick % 10) != 6) return;
    const uint64_t c = g_calls.load(std::memory_order_relaxed);
    if (!c) return;
    // Build 22.1: the published axes ride along, splitting any input defect
    // into publish-side (zeros here while the stick is pushed) vs game-side
    // (real values here, no movement in game).
    uint32_t btn = 0;
    float    ax[6] = {};
    const bool live = headpose::touch_pad(&btn, ax);
    LOG_INFO("xin: calls=%llu merged=%llu fabricated=%llu held=%llu last_btn=0x%04X "
             "%s ax L(%+.2f %+.2f) R(%+.2f %+.2f) T(%.2f %.2f)%s",
             (unsigned long long)c,
             (unsigned long long)g_merges.load(std::memory_order_relaxed),
             (unsigned long long)g_fabricated.load(std::memory_order_relaxed),
             (unsigned long long)g_placeheld.load(std::memory_order_relaxed),
             g_last_btn.load(std::memory_order_relaxed),
             live ? "live" : "dead",
             ax[0], ax[1], ax[2], ax[3], ax[4], ax[5],
             g_enabled.load(std::memory_order_relaxed) ? "" : " (DISABLED)");
    // The engine may re-resolve the slot (a game patch class event). Detect
    // and report; poll_install stays done, we do not fight over it.
    if (g_slot && readable(g_slot, sizeof(void*)) &&
        *g_slot != (void*)&hook_GetState) {
        static bool s_lost = false;
        if (!s_lost) {
            s_lost = true;
            LOG_WARN("xin: slot was REWRITTEN to 0x%p, merge is out of the "
                     "path", *g_slot);
        }
    }
}

void set_enabled(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
}

bool merging_live() {
    if (!g_installed || !g_enabled.load(std::memory_order_relaxed))
        return false;
    return headpose::touch_pad(nullptr, nullptr);
}

}  // namespace xin
}  // namespace grwxr
