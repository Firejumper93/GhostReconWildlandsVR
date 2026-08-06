// WeaponProbe.cpp - build 39.1: read-only observer on the pooled placement
// SetTransform. See WeaponProbe.h for what this is and why.
//
// Evidence base (docs/RE-notes.md, derived offline 2026-08-04, all VERIFIED
// against the on-disk image):
//   setter body 0x13E5EA30, thunk 0x030AC6A0 (E9 rel32 in an int3 slot),
//   args (rcx=ctx, edx=handle|type<<24, r8=transform 0x40 bytes, r9d=flags),
//   transform row 3 (offset 0x30) = world position.
// The AOB below matched exactly ONCE in the image; the RVAs are cross-checks
// derived from THIS build, not borrowed from another (hazard 38).
//
// DESIGN RULE THIS FILE EXISTS TO HONOUR (hazard 42, and the reason 39.0 was
// rewritten before it ever ran): A PROBE MUST NOT BE ABLE TO RETURN AN
// AMBIGUOUS NULL. Version 39.0 filtered placements to within 2.5 m of the
// camera and reported only survivors. Two independent things could empty that
// report while the subsystem was working perfectly: a camera position that was
// never published (it is VR-gated, see camera::base_pos), or a radius guess
// that was simply wrong. Either would have read as "the weapon is not placed
// here", which is a conclusion, not a measurement.
//
// So this version applies NO filter as a gate. It records every placement it
// sees and reports three things every second:
//   1. how many calls happened at all              (0 = the subsystem is idle)
//   2. the camera position and whether it is VALID  (invalid = cannot correlate)
//   3. the top handles by CALL COUNT and, separately, by CLOSENESS
// Each of the three failure modes therefore names itself in the log instead of
// looking like the others.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "WeaponProbe.h"
#include "CameraProbe.h"
#include "GameBuild.h"
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
// Build 46: impl/thunk RVAs are per-build (GameBuild.cpp), impl as the
// signature cross-check, thunk verified byte-for-byte by ThunkHook.

using SetterFn = uint64_t (*)(void* ctx, uint32_t handle, const float* t,
                              uint32_t flags);

hook::ThunkHook g_hook;
SetterFn        g_orig = nullptr;

// One observed placement target. state: 0 empty, 1 being claimed, 2 ready.
// A race loses at worst one count or duplicates an entry for one second, which
// is harmless in a correlation probe.
struct Slot {
    volatile LONG state;
    volatile LONG count;
    uint32_t      key;      // full edx dword: handle low 24, type bits high 8
    void*         ctx;
    float         pos[3];   // most recent position written
    float         mind;     // closest this handle ever came to the camera
};
// 2026-08-05: a real gameplay second produced uniq=226 and drop=42 at 256
// slots. Widen so the busiest scenes are not truncated silently.
constexpr int kSlots = 1024;  // power of two

Slot          g_tab[kSlots];
volatile LONG g_total = 0;   // every setter call seen this second
volatile LONG g_drop  = 0;   // calls we could not table (table full)

// Build 45: the watch list behind wp::marker(). drain() owns key/on (1 Hz,
// init thread); the hot path only compares keys and stores positions. The
// on flag is checked first on both sides, so a slot being rewritten is at
// worst skipped for one call, and a stale-key position write costs one
// wrong sample for one frame. Colour identity = slot index, so slots are
// never compacted.
struct Watch {
    volatile LONG on;       // 0 empty, 1 live
    uint32_t      key;      // full edx dword, same keying as Slot
    float         pos[3];   // latest position (plain stores, tolerance: cm)
    volatile LONG hits;     // hot-path updates since the last drain
};
Watch g_watch[kWatch] = {};

// Build 47 write state. set_write() stores from the render thread once per
// frame; the hot path reads. Position uses plain float stores (a torn read
// costs centimetres for one call, and the setter runs again next frame); the
// slot gate is a single aligned LONG so off means off immediately. g_w_tick
// makes the write fail-safe: if the render thread stops pushing (VR lost,
// cfg reload in flight), the write stops within 250 ms instead of holding a
// stale controller position forever.
volatile LONG      g_w_handle  = 0;      // 24-bit handle to write, 0 = by slot
volatile LONG      g_w_slot    = -1;     // watch-slot index to write, -1 off
volatile LONG      g_w_mode    = 1;      // 1 lift, 2 ride the controller
float              g_w_up      = 0.30f;  // lift height, metres
float              g_w_ctrl[3] = {};     // right controller, engine world
volatile LONG      g_w_ctrl_ok = 0;
volatile LONG      g_w_count   = 0;      // writes this second, drain logs it
volatile ULONGLONG g_w_tick    = 0;      // last push, GetTickCount64

// Hot path. No logging, no allocation, no locks (rule 8): interlocked ops,
// a handful of float ops, and at most 16 probe steps.
void record(void* ctx, uint32_t key, const float* t) {
    InterlockedIncrement(&g_total);

    const float* p = t + 12;   // row 3 = position

    // Distance is RECORDED, never used as a gate. If the camera position is
    // unavailable we still table the placement and simply mark the distance
    // unknown, so the report can never be emptied by a missing input.
    float cam[3];
    const bool cam_ok = camera::base_pos(cam);
    float d = -1.0f;
    if (cam_ok) {
        const float dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
        d = sqrtf(dx * dx + dy * dy + dz * dz);
    }

    // Build 45: refresh a watched handle's live position. At most kWatch
    // compares and three float stores, inside the rule-8 budget.
    for (int i = 0; i < kWatch; ++i) {
        if (g_watch[i].on && g_watch[i].key == key) {
            g_watch[i].pos[0] = p[0];
            g_watch[i].pos[1] = p[1];
            g_watch[i].pos[2] = p[2];
            InterlockedIncrement(&g_watch[i].hits);
            break;
        }
    }

    uint32_t idx = (key * 2654435761u ^
                    (uint32_t)((uintptr_t)ctx >> 4) * 40503u) & (kSlots - 1);
    for (int i = 0; i < 16; ++i, idx = (idx + 1) & (kSlots - 1)) {
        Slot& s = g_tab[idx];
        const LONG st = s.state;
        if (st == 2) {
            if (s.key == key && s.ctx == ctx) {
                InterlockedIncrement(&s.count);
                s.pos[0] = p[0]; s.pos[1] = p[1]; s.pos[2] = p[2];
                if (d >= 0.0f && (s.mind < 0.0f || d < s.mind)) s.mind = d;
                return;
            }
            continue;
        }
        if (st == 0 && InterlockedCompareExchange(&s.state, 1, 0) == 0) {
            s.key = key; s.ctx = ctx;
            s.pos[0] = p[0]; s.pos[1] = p[1]; s.pos[2] = p[2];
            s.mind = d;
            s.count = 1;
            InterlockedExchange(&s.state, 2);  // publish
            return;
        }
    }
    InterlockedIncrement(&g_drop);
}

uint64_t observer(void* ctx, uint32_t handle, const float* t, uint32_t flags) {
    if (t) {
        record(ctx, handle, t);

        // Build 47: the write. record() above stored the ENGINE's intended
        // position, so the marker keeps showing where the animation wanted
        // the handle while the object (if this works) sits where we put it,
        // which makes the divergence itself visible. The modified transform
        // lives on our stack; the caller's buffer is never touched.
        const LONG wh = g_w_handle;
        const LONG ws = g_w_slot;
        const bool hit =
            wh ? ((handle & 0xFFFFFF) == (uint32_t)wh)
               : (ws >= 0 && g_watch[ws].on && g_watch[ws].key == handle);
        if (hit && (GetTickCount64() - g_w_tick) < 250) {
            float mod[16];
            memcpy(mod, t, sizeof(mod));
            if (g_w_mode == 2) {
                if (!g_w_ctrl_ok)
                    return g_orig(ctx, handle, t, flags);  // no pose, no write
                mod[12] = g_w_ctrl[0];
                mod[13] = g_w_ctrl[1];
                mod[14] = g_w_ctrl[2];
            } else {
                mod[14] += g_w_up;   // engine world is z-up
            }
            InterlockedIncrement(&g_w_count);
            return g_orig(ctx, handle, mod, flags);
        }
    }
    return g_orig(ctx, handle, t, flags);
}

// Append "h=<handle> t=<type> n=<count> d=<dist>" for one slot.
int fmt_slot(char* buf, int off, int cap, const Slot& s) {
    if (off <= 0 || off >= cap - 1) return off;
    char dist[24];
    if (s.mind < 0.0f) _snprintf_s(dist, sizeof(dist), _TRUNCATE, "?");
    else               _snprintf_s(dist, sizeof(dist), _TRUNCATE, "%.2f", s.mind);
    return off + _snprintf_s(buf + off, cap - off, _TRUNCATE,
                             " | h=%06X t=%02X n=%ld d=%s",
                             s.key & 0xFFFFFF, s.key >> 24, (long)s.count, dist);
}

}  // namespace

bool install() {
    auto img = sig::main_image();
    if (!img) {
        LOG_ERROR("wp: cannot read GRW.exe image. Probe OFF.");
        return false;
    }

    const gamebuild::Build* gb = gamebuild::get();
    if (!gb) {
        LOG_ERROR("wp: no address table for this GRW.exe build. Probe OFF.");
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
    if (*body != img->base + gb->wp_setter_impl) {
        LOG_ERROR("wp: setter matched at RVA 0x%08zX, analysis says 0x%08zX. "
                  "NOT the function we mapped (different game build?). "
                  "Probe OFF.",
                  (size_t)(*body - img->base), (size_t)gb->wp_setter_impl);
        return false;
    }

    // ThunkHook::install verifies the E9 at the slot resolves to *body
    // before writing anything.
    if (!g_hook.install(img->base + gb->wp_setter_thunk, *body, (void*)&observer,
                        "placement SetTransform thunk")) {
        LOG_ERROR("wp: thunk verification failed. Probe OFF.");
        return false;
    }
    g_orig = (SetterFn)g_hook.original();
    LOG_INFO("wp: OBSERVING the pooled placement SetTransform (read-only, "
             "calls through unchanged). One wp: line per second: calls= is "
             "how many placements happened, cam= is the camera position we "
             "measure against, then the busiest handles and the closest "
             "handles. The weapon should be a handle that stays close every "
             "second and CHANGES when you swap weapons.");
    return true;
}

void drain() {
    const LONG total = InterlockedExchange(&g_total, 0);
    const LONG drop  = InterlockedExchange(&g_drop, 0);

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

    // CASE 1: the subsystem was not called at all. Say so explicitly; this is
    // a real measurement, not a missing report.
    if (total == 0) {
        static int quiet = 0;
        if ((quiet++ % 10) == 0)
            LOG_INFO("wp: calls=0 this second. The placement subsystem is "
                     "idle here (menu/loading?), so there is nothing to "
                     "correlate yet. Get on foot in gameplay.");
        return;
    }

    // CASE 2: we have placements but no camera position, so distances are
    // meaningless. Report the counts anyway and name the reason.
    float cam[3] = {};
    const bool cam_ok = camera::base_pos(cam);

    char line[1024];
    int  cap = (int)sizeof(line);
    int  off = _snprintf_s(line, sizeof(line), _TRUNCATE,
                           "wp: calls=%ld uniq=%d drop=%ld cam=%s(%.1f,%.1f,%.1f)",
                           (long)total, n, (long)drop,
                           cam_ok ? "" : "UNAVAILABLE",
                           cam[0], cam[1], cam[2]);

    // Busiest handles: sort by count, descending.
    for (int i = 1; i < n; ++i) {
        Slot v = local[i];
        int  j = i - 1;
        while (j >= 0 && local[j].count < v.count) { local[j + 1] = local[j]; --j; }
        local[j + 1] = v;
    }
    if (n > 0 && off > 0 && off < cap - 1)
        off += _snprintf_s(line + off, cap - off, _TRUNCATE, "  BUSIEST:");
    for (int i = 0; i < (n < 5 ? n : 5); ++i) off = fmt_slot(line, off, cap, local[i]);

    // Closest handles: sort by min distance ascending, unknowns last.
    if (cam_ok) {
        for (int i = 1; i < n; ++i) {
            Slot v = local[i];
            const float vk = (v.mind < 0.0f) ? 1e30f : v.mind;
            int j = i - 1;
            while (j >= 0 && ((local[j].mind < 0.0f) ? 1e30f : local[j].mind) > vk) {
                local[j + 1] = local[j]; --j;
            }
            local[j + 1] = v;
        }
        if (off > 0 && off < cap - 1)
            off += _snprintf_s(line + off, cap - off, _TRUNCATE, "  CLOSEST:");
        for (int i = 0; i < (n < 8 ? n : 8); ++i) off = fmt_slot(line, off, cap, local[i]);
    }

    LOG_INFO("%s", line);

    // Build 45: refresh the watch list from the closeness ranking (local is
    // sorted ascending by mind when cam_ok). Sticky by key: a candidate that
    // still qualifies keeps its slot, so its marker colour cannot shuffle
    // mid-test. 10 m bound: the weapon and its attachments live within arm's
    // reach plus the third-person camera arm; world streaming does not.
    if (cam_ok) {
        uint32_t want[kWatch];
        float    wpos[kWatch][3];
        int      nw = 0;
        for (int i = 0; i < n && nw < kWatch; ++i) {
            if (local[i].mind < 0.0f || local[i].mind >= 10.0f) continue;
            want[nw] = local[i].key;
            wpos[nw][0] = local[i].pos[0];
            wpos[nw][1] = local[i].pos[1];
            wpos[nw][2] = local[i].pos[2];
            ++nw;
        }
        for (int i = 0; i < kWatch; ++i) {
            if (!g_watch[i].on) continue;
            bool keep = false;
            for (int j = 0; j < nw; ++j)
                if (want[j] == g_watch[i].key) keep = true;
            if (!keep) InterlockedExchange(&g_watch[i].on, 0);
        }
        for (int j = 0; j < nw; ++j) {
            bool have = false;
            for (int i = 0; i < kWatch; ++i)
                if (g_watch[i].on && g_watch[i].key == want[j]) have = true;
            if (have) continue;
            for (int i = 0; i < kWatch; ++i) {
                if (g_watch[i].on) continue;
                g_watch[i].key    = want[j];
                g_watch[i].pos[0] = wpos[j][0];
                g_watch[i].pos[1] = wpos[j][1];
                g_watch[i].pos[2] = wpos[j][2];
                InterlockedExchange(&g_watch[i].hits, 0);
                InterlockedExchange(&g_watch[i].on, 1);
                break;
            }
        }
        // The legend the whole build exists for: colour -> handle, once per
        // second. The user names the colour sitting ON the gun.
        static const char* kCol[kWatch] = {"RED",     "GREEN", "YELLOW",
                                           "MAGENTA", "CYAN",  "WHITE"};
        char leg[512];
        int  loff = _snprintf_s(leg, sizeof(leg), _TRUNCATE, "wpm:");
        int  live = 0;
        for (int i = 0; i < kWatch; ++i) {
            if (!g_watch[i].on || loff <= 0 || loff >= (int)sizeof(leg) - 1)
                continue;
            ++live;
            loff += _snprintf_s(leg + loff, sizeof(leg) - loff, _TRUNCATE,
                                " %s=h%06X/t%02X u=%ld",
                                kCol[i], g_watch[i].key & 0xFFFFFF,
                                g_watch[i].key >> 24,
                                (long)InterlockedExchange(&g_watch[i].hits, 0));
        }
        if (live)
            LOG_INFO("%s  (which colour sits ON the gun?)", leg);
    }

    // Build 47: write status, once per second while armed. writes=0 with a
    // live target means the setter never carried that handle this second,
    // which is a measurement (the engine stopped placing it), not silence.
    {
        static const char* kCol[kWatch] = {"RED",     "GREEN", "YELLOW",
                                           "MAGENTA", "CYAN",  "WHITE"};
        const LONG wc = InterlockedExchange(&g_w_count, 0);
        const LONG wh = g_w_handle;
        const LONG ws = g_w_slot;
        if (wh) {
            LOG_INFO("wpw: WRITING h%06X (by handle) mode=%ld ctrl=%s "
                     "writes=%ld this second",
                     (unsigned)wh, (long)g_w_mode,
                     g_w_ctrl_ok ? "ok" : "NONE", (long)wc);
        } else if (ws >= 0 && ws < kWatch) {
            LOG_INFO("wpw: WRITING %s (h%06X/t%02X) mode=%ld ctrl=%s "
                     "writes=%ld this second",
                     kCol[ws],
                     g_watch[ws].on ? (g_watch[ws].key & 0xFFFFFF) : 0,
                     g_watch[ws].on ? (g_watch[ws].key >> 24) : 0,
                     (long)g_w_mode, g_w_ctrl_ok ? "ok" : "NONE",
                     (long)wc);
        }
    }

    if (!cam_ok) {
        static bool said = false;
        if (!said) {
            said = true;
            LOG_WARN("wp: no camera position available, so the d= distances "
                     "are unknown and CLOSEST is not reported. Placement "
                     "counts above are still real. This resolves itself once "
                     "the mode-0 gameplay camera has been seen once.");
        }
    }
}

void set_write(uint32_t handle, int slot, int mode, float up,
               const float ctrl_world[3], bool ctrl_ok) {
    if (ctrl_ok) {
        g_w_ctrl[0] = ctrl_world[0];
        g_w_ctrl[1] = ctrl_world[1];
        g_w_ctrl[2] = ctrl_world[2];
    }
    InterlockedExchange(&g_w_ctrl_ok, ctrl_ok ? 1 : 0);
    g_w_up   = up;
    g_w_tick = GetTickCount64();
    InterlockedExchange(&g_w_mode, (mode == 2) ? 2 : 1);
    InterlockedExchange(&g_w_handle, (LONG)(handle & 0xFFFFFF));
    InterlockedExchange(&g_w_slot,
                        (slot >= 0 && slot < kWatch) ? slot : -1);
}

bool marker(int slot, float out_pos[3]) {
    if (slot < 0 || slot >= kWatch) return false;
    if (!g_watch[slot].on) return false;
    out_pos[0] = g_watch[slot].pos[0];
    out_pos[1] = g_watch[slot].pos[1];
    out_pos[2] = g_watch[slot].pos[2];
    return true;
}

}  // namespace wp
}  // namespace grwxr
