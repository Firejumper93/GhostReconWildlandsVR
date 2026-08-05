// WeaponProbe.cpp - build 39: read-only observer on the pooled placement
// SetTransform. See WeaponProbe.h for what this is and why.
//
// Evidence base (docs/RE-notes.md, derived offline 2026-08-04, all VERIFIED
// against the on-disk image):
//   setter body 0x13E5EA30, thunk 0x030AC6A0 (E9 rel32 in an int3 slot),
//   args (rcx=ctx, edx=handle|type<<24, r8=transform 0x40 bytes, r9d=flags),
//   transform row 3 (offset 0x30) = world position.
// The AOB below matched exactly ONCE in the image; the RVAs are cross-checks
// derived from THIS build, not borrowed from another (hazard 38).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "WeaponProbe.h"
#include "CameraProbe.h"
#include "Log.h"
#include "Sig.h"
#include "ThunkHook.h"

namespace grwxr {
namespace wp {
namespace {

// Setter A prologue, rip-relative displacement wildcarded. Verified unique
// offline against the whole image (single hit at 0x13E5EA30).
constexpr char kSetterSig[] =
    "48 89 5C 24 18 89 54 24 10 55 56 57 48 83 EC 40 F3 0F 10 05 ?? ?? ?? ?? "
    "89 D0 4C 89 C7 F3 0F 11 44 24 78 25 FF FF FF 00";
constexpr uintptr_t kSetterImplRva  = 0x13E5EA30;  // cross-check only
constexpr uintptr_t kSetterThunkRva = 0x030AC6A0;  // verified by ThunkHook

using SetterFn = uint64_t (*)(void* ctx, uint32_t handle, const float* t,
                              uint32_t flags);

hook::ThunkHook g_hook;
SetterFn        g_orig = nullptr;

// Correlation table. Fixed size, cleared each drain. state: 0 empty,
// 1 being claimed, 2 ready. Races lose at worst one count or duplicate an
// entry for one second; both are harmless in a correlation probe.
struct Slot {
    volatile LONG state;
    volatile LONG count;
    uint32_t      key;   // full edx dword: handle low 24, type bits high 8
    void*         ctx;
    float         pos[3];
    float         d2;
};
constexpr int   kSlots = 128;  // power of two
constexpr float kR2    = 2.5f * 2.5f;  // "near the camera" radius, squared

Slot          g_tab[kSlots];
volatile LONG g_total = 0;   // all setter calls seen
volatile LONG g_near  = 0;   // calls within the radius
volatile LONG g_drop  = 0;   // near calls dropped because the table was full

// Hot path. No logging, no allocation, no locks (rule 8): interlocked ops
// and ~15 float ops only.
void record(void* ctx, uint32_t key, const float* t) {
    InterlockedIncrement(&g_total);

    float cam[3];
    camera::base_pos(cam);  // torn read acceptable: metres vs centimetres

    const float* p  = t + 12;  // row 3 = position
    const float  dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
    const float  d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > kR2) return;
    InterlockedIncrement(&g_near);

    uint32_t idx = (key * 2654435761u ^
                    (uint32_t)((uintptr_t)ctx >> 4) * 40503u) & (kSlots - 1);
    for (int i = 0; i < 16; ++i, idx = (idx + 1) & (kSlots - 1)) {
        Slot& s = g_tab[idx];
        const LONG st = s.state;
        if (st == 2) {
            if (s.key == key && s.ctx == ctx) {
                InterlockedIncrement(&s.count);
                s.pos[0] = p[0]; s.pos[1] = p[1]; s.pos[2] = p[2];
                s.d2 = d2;
                return;
            }
            continue;
        }
        if (st == 0 && InterlockedCompareExchange(&s.state, 1, 0) == 0) {
            s.key = key; s.ctx = ctx;
            s.pos[0] = p[0]; s.pos[1] = p[1]; s.pos[2] = p[2];
            s.d2 = d2;
            s.count = 1;
            InterlockedExchange(&s.state, 2);  // publish
            return;
        }
    }
    InterlockedIncrement(&g_drop);
}

uint64_t observer(void* ctx, uint32_t handle, const float* t, uint32_t flags) {
    if (t) record(ctx, handle, t);
    return g_orig(ctx, handle, t, flags);
}

}  // namespace

bool install() {
    auto img = sig::main_image();
    if (!img) {
        LOG_ERROR("wp: cannot read GRW.exe image. Probe OFF.");
        return false;
    }

    size_t m = 0;
    auto body = sig::find_unique(*img, kSetterSig, &m);
    if (!body) {
        LOG_ERROR("wp: placement setter signature %s (matches=%zu). Probe "
                  "OFF, everything else runs (rule 7).",
                  m ? "is ambiguous" : "missed", m);
        return false;
    }
    if (*body != img->base + kSetterImplRva) {
        LOG_ERROR("wp: setter matched at RVA 0x%08zX, analysis says 0x%08zX. "
                  "NOT the function we mapped. Probe OFF.",
                  (size_t)(*body - img->base), (size_t)kSetterImplRva);
        return false;
    }

    // ThunkHook::install verifies the E9 at the slot resolves to *body
    // before writing anything.
    if (!g_hook.install(img->base + kSetterThunkRva, *body, (void*)&observer,
                        "placement SetTransform thunk 0x030AC6A0")) {
        LOG_ERROR("wp: thunk verification failed. Probe OFF.");
        return false;
    }
    g_orig = (SetterFn)g_hook.original();
    LOG_INFO("wp: OBSERVING the pooled placement SetTransform (read-only, "
             "calls through unchanged). Correlating handles within 2.5 m of "
             "the camera; watch the per-second wp: lines.");
    return true;
}

void drain() {
    const LONG total = InterlockedExchange(&g_total, 0);
    const LONG near_ = InterlockedExchange(&g_near, 0);
    const LONG drop  = InterlockedExchange(&g_drop, 0);
    if (total == 0) return;

    // Snapshot and clear the table.
    Slot local[kSlots];
    int  n = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (g_tab[i].state != 2) continue;
        local[n]       = g_tab[i];
        local[n].count = g_tab[i].count;
        ++n;
        g_tab[i].count = 0;
        InterlockedExchange(&g_tab[i].state, 0);
    }

    // Sort by count, descending (n is small; insertion sort is fine).
    for (int i = 1; i < n; ++i) {
        Slot v = local[i];
        int  j = i - 1;
        while (j >= 0 && local[j].count < v.count) {
            local[j + 1] = local[j];
            --j;
        }
        local[j + 1] = v;
    }

    char line[512];
    int  off = _snprintf_s(line, sizeof(line), _TRUNCATE,
                           "wp: calls=%ld near=%ld drop=%ld uniq=%d",
                           (long)total, (long)near_, (long)drop, n);
    const int top = n < 6 ? n : 6;
    for (int i = 0; i < top && off > 0 && off < (int)sizeof(line) - 1; ++i) {
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE,
                           " | h=%06X t=%02X n=%ld d=%.2f ctx=%p",
                           local[i].key & 0xFFFFFF, local[i].key >> 24,
                           (long)local[i].count, sqrtf(local[i].d2),
                           local[i].ctx);
    }
    LOG_INFO("%s", line);
}

}  // namespace wp
}  // namespace grwxr
