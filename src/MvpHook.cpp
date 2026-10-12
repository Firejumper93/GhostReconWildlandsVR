// MvpHook.cpp - BUILD 267, see MvpHook.h.
// PINS (TU25 GRW.exe, TimeDateStamp 0x6A99768A; the same row the flat FP mod pins, FpCamera.cpp {0x6A99768A, 0x0138E5F0, 0x0138E5D0, 0x0D93292A}):
//   0x0138E5F0  the recompute body, first 51 bytes literal (no rel32, no rip operand), rcx = the Camera
//   0x0138E5D0  16 bytes of int3 padding in front of it
//   0x0D93292A  the ONLY rel32 caller: E8 <rel32> that resolves to the body
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstring>

#include "Log.h"
#include "MvpHook.h"

namespace grwxr {
namespace mvp {

namespace {
constexpr uint32_t kStamp = 0x6A99768Au, kSize = 0x185BA000u;
constexpr uint32_t kBody = 0x0138E5F0u, kSlot = 0x0138E5D0u, kCall = 0x0D93292Au;
const uint8_t kSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C, 0x89, 0x64, 0x24, 0x20,
                          0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xF9, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x81,
                          0x48, 0x07, 0x00, 0x00, 0x49, 0x8B, 0xF9, 0x49, 0x8B, 0xD8};
uint8_t*          g_call = nullptr;
int32_t           g_saved_rel = 0;
std::atomic<bool> g_live{false};

bool patch(void* dst, const void* src, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(dst, src, n);
    DWORD t = 0;
    VirtualProtect(dst, n, old, &t);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}
}  // namespace

bool live() { return g_live.load(std::memory_order_acquire); }

bool install(uint8_t* base, uint32_t stamp, uint32_t size, void* entry, void** original_out) {
    if (g_live.load() || !base || !entry || !original_out) return false;
    if (stamp != kStamp || size != kSize) {
        LOG_ERROR("mvp: REFUSED: not the TU25 image the pins are keyed to. Nothing installed.");
        return false;
    }
    uint8_t* body = base + kBody;
    uint8_t* slot = base + kSlot;
    uint8_t* call = base + kCall;
    __try {
        if (memcmp(body, kSig, sizeof(kSig)) != 0) {
            LOG_ERROR("mvp: REFUSED: the body bytes at RVA 0x%08X do not match the pin. Nothing installed.", kBody);
            return false;
        }
        for (int i = 0; i < 16; ++i) {
            if (slot[i] != 0xCC) {
                LOG_ERROR("mvp: REFUSED: the padding at RVA 0x%08X is not int3 at +%d. Nothing installed.", kSlot, i);
                return false;
            }
        }
        if (call[0] != 0xE8) {
            LOG_ERROR("mvp: REFUSED: the call site at RVA 0x%08X is not E8. Nothing installed.", kCall);
            return false;
        }
        int32_t rel = 0;
        memcpy(&rel, call + 1, 4);
        if (call + 5 + rel != body) {
            LOG_ERROR("mvp: REFUSED: the call site does not resolve to the body. Nothing installed.");
            return false;
        }
        // the displacement is repointed with ONE 4 byte store, which is atomic only inside one cache line
        if (((uintptr_t)(call + 1) & 63u) > 60u) {
            LOG_ERROR("mvp: REFUSED: the call displacement straddles a cache line. Nothing installed.");
            return false;
        }
        // 1. the stub into dead padding, while nothing points at it
        uint8_t stub[14] = {0xFF, 0x25, 0, 0, 0, 0};
        const uint64_t dest = (uint64_t)(uintptr_t)entry;
        memcpy(stub + 6, &dest, 8);
        if (!patch(slot, stub, sizeof(stub))) {
            LOG_ERROR("mvp: REFUSED: VirtualProtect failed on the padding. Nothing installed.");
            return false;
        }
        // 2. the original, for the asm stub's tail jump, BEFORE the call is repointed
        *original_out = body;
        // 3. one atomic 4 byte store takes the hook live
        const int32_t nrel = (int32_t)(slot - (call + 5));
        g_call = call;
        g_saved_rel = rel;
        if (!patch(call + 1, &nrel, 4)) {
            LOG_ERROR("mvp: REFUSED: could not repoint the call. The padding stub is unreferenced and harmless.");
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("mvp: REFUSED: a read faulted while verifying. Nothing installed.");
        return false;
    }
    g_live.store(true, std::memory_order_release);
    LOG_INFO("mvp: INSTALLED. The camera recompute (RVA 0x%08X) is reached through the stub at RVA 0x%08X from the call at RVA 0x%08X.",
             kBody, kSlot, kCall);
    return true;
}

void restore() {
    if (!g_live.exchange(false) || !g_call) return;
    patch(g_call + 1, &g_saved_rel, 4);
    LOG_INFO("mvp: restored the original call.");
}

}  // namespace mvp
}  // namespace grwxr
