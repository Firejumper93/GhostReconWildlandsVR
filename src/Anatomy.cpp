// Anatomy.cpp - BUILD 241, see Anatomy.h. Observation only.
//
// Layout of this file: the preallocated tables, the taps (hot path, rule 8),
// the state machine (init thread), the dump thread (file I/O, off every hot
// path). Everything the taps touch is static storage written with relaxed
// atomics; there is no lock, no allocation and no logging in a tap.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Anatomy.h"
#include "CbDump.h"       // BUILD 246: the constant-buffer content capture shares these taps
#include "D3D11Hook.h"
#include "DrawHook.h"
#include "GtRec.h"        // BUILD 270: the ground-truth stereo recorder rides these taps too (gate bit 4)
#include "Log.h"

namespace grwxr {
namespace anatomy {

std::atomic<int> g_state{0};   // 0 idle, 1 hooked, 2 capturing, 3 stopped, 4 dumping, 5 done, 6 failed
std::atomic<int> g_gate{0};    // BUILD 246: bit 1 anatomy window open, bit 2 cbdump window open; BUILD 270: bit 4 gtrec recording

void gate_set(int bits)   { g_gate.fetch_or(bits, std::memory_order_release); }
void gate_clear(int bits) { g_gate.fetch_and(~bits, std::memory_order_release); }

namespace {

std::atomic<int> g_hooks{0};   // BUILD 246: 0 not installed, 1 installed, -1 refused

constexpr int      kFrames   = 240;          // the window, in presents
constexpr uint64_t kInstallMs = 20000;       // hooks go in this long after the first stereo frame
constexpr uint64_t kArmMs     = 30000;       // the window opens this long after it
constexpr uint64_t kGiveUpMs  = 150000;      // a window that never fills is closed anyway
constexpr uint32_t kEvCap    = 1u << 18;     // 262144 events x 56 bytes
constexpr int      kThrCap   = 64;
constexpr size_t   kCtxCap   = 256;
constexpr size_t   kBufCap   = 8192;
constexpr size_t   kViewCap  = 2048;
constexpr size_t   kBsCap    = 1024;
constexpr size_t   kRtCap    = 128;

constexpr uint64_t kValid = 1ull << 63;
constexpr uint64_t kIsCB  = 1ull << 62;

enum : uint16_t { kEvPresent = 1, kEvFinish = 2, kEvExecute = 3, kEvCbWrite = 4, kEvRtBind = 5, kEvTypes = 6 };
// kRepl: an IMMEDIATE-class function entered while this thread is inside the real
// ExecuteCommandList, i.e. the runtime replaying a command list through the same
// entry points the game calls. Without this split those calls would be counted as
// the game's own immediate-context work.
enum : int { kImm = 0, kDef = 1, kUnk = 2, kRepl = 3, kKinds = 4 };

std::atomic<bool>     g_enabled{true};
std::atomic<uint64_t> g_first_stereo_ms{0};
std::atomic<uint64_t> g_start_frame{0};
uint64_t g_arm_ms = 0;
uint64_t g_tsc0 = 0, g_qpc0 = 0;
uint64_t g_pts[kFrames + 1] = {};     // TSC at the entry of Present #(start + i)
std::atomic<uint32_t> g_install_n{0};
// The Present-time check that the immediate context's vtable only points at
// detoured entries (see DrawHook.cpp install_anatomy for why it can flip).
ID3D11DeviceContext*  g_watch_ctx = nullptr;
std::atomic<uint32_t> g_watch_n{0}, g_unhooked_n{0};
std::atomic<int>      g_bad_slot{-1};
std::atomic<uintptr_t> g_bad_fn{0};

// --- the event buffer -------------------------------------------------------
struct Ev {
    std::atomic<uint64_t> ts;   // rdtsc; written LAST, 0 = not yet published
    uint32_t tid;
    uint32_t frame;             // d3d11::frame_count() when recorded
    uint16_t type;
    uint16_t kind;              // 0 immediate, 1 deferred, 2 unknown
    uint32_t aux;
    uint64_t p[4];
};
Ev g_ev[kEvCap];
std::atomic<uint32_t> g_head{0};
std::atomic<uint32_t> g_tn[kEvTypes] = {};
std::atomic<uint32_t> g_tdrop[kEvTypes] = {};
std::atomic<uint32_t> g_full{0};
const uint32_t g_tcap[kEvTypes] = {0, 1000, 60000, 60000, 100000, 60000};

inline void push(uint16_t type, uint16_t kind, uint32_t aux, uint64_t a, uint64_t b,
                 uint64_t c, uint64_t d, uint64_t ts = 0) {
    if (g_tn[type].fetch_add(1, std::memory_order_relaxed) >= g_tcap[type]) {
        g_tdrop[type].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t i = g_head.fetch_add(1, std::memory_order_relaxed);
    if (i >= kEvCap) { g_full.fetch_add(1, std::memory_order_relaxed); return; }
    Ev& e = g_ev[i];
    e.tid   = GetCurrentThreadId();
    e.frame = (uint32_t)d3d11::frame_count();
    e.type  = type;
    e.kind  = kind;
    e.aux   = aux;
    e.p[0] = a; e.p[1] = b; e.p[2] = c; e.p[3] = d;
    e.ts.store(ts ? ts : __rdtsc(), std::memory_order_release);
}

// --- aggregates (exact even when the event buffer is full) ------------------
std::atomic<uint32_t> g_draws[kKinds][kFrames] = {};
std::atomic<uint32_t> g_fin[kFrames] = {};
std::atomic<uint32_t> g_exe[kFrames] = {};
std::atomic<uint32_t> g_rtb[kKinds][kFrames] = {};
std::atomic<uint32_t> g_cbw[kKinds][kFrames] = {};      // every constant-buffer write
std::atomic<uint32_t> g_cbm[kKinds][kFrames] = {};      // the matrix-looking ones
std::atomic<uint32_t> g_cb_fault{0}, g_cache_full{0}, g_bs_full{0}, g_ctx_full{0}, g_thr_full{0}, g_rt_full{0};

inline uint32_t hp(uintptr_t p, uint32_t mask) {
    uint64_t x = (uint64_t)(p >> 4) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(x >> 40) & mask;
}

// Lock-free pointer-keyed slot table. Find or claim; -1 if the probe run is full.
template <size_t N>
struct PtrMap {
    std::atomic<uintptr_t> key[N];
    std::atomic<uint64_t>  val[N];
    int slot(uintptr_t p) {
        const uint32_t h = hp(p, (uint32_t)(N - 1));
        for (uint32_t i = 0; i < 32; ++i) {
            const uint32_t s = (h + i) & (uint32_t)(N - 1);
            const uintptr_t k = key[s].load(std::memory_order_relaxed);
            if (k == p) return (int)s;
            if (k == 0) {
                uintptr_t exp = 0;
                if (key[s].compare_exchange_strong(exp, p, std::memory_order_relaxed) || exp == p)
                    return (int)s;
            }
        }
        return -1;
    }
};

PtrMap<kBufCap>  g_bufc;    // resource pointer -> kValid | kIsCB | ByteWidth
PtrMap<kViewCap> g_viewc;   // view pointer -> kValid | w | h<<16 | fmt<<32 | dsv<<40
PtrMap<kCtxCap>  g_ctxc;    // context pointer -> kind + 1
std::atomic<uint32_t> g_ctx_draws[kCtxCap];    // since the last FinishCommandList
std::atomic<uint64_t> g_ctx_total[kCtxCap];
std::atomic<uint32_t> g_ctx_fin[kCtxCap], g_ctx_exe[kCtxCap], g_ctx_tid[kCtxCap];

// --- per thread -------------------------------------------------------------
struct ThrEnt {
    std::atomic<uint32_t> tid, kmask;
    std::atomic<uint64_t> total;
    std::atomic<uint32_t> draws[kFrames];
};
ThrEnt g_thr[kThrCap];
thread_local int t_slot = -1;

ThrEnt* thr() {
    const uint32_t tid = GetCurrentThreadId();
    if (t_slot >= 0 && g_thr[t_slot].tid.load(std::memory_order_relaxed) == tid) return &g_thr[t_slot];
    for (int i = 0; i < kThrCap; ++i) {
        const uint32_t k = g_thr[i].tid.load(std::memory_order_relaxed);
        if (k == tid) { t_slot = i; return &g_thr[i]; }
        if (k == 0) {
            uint32_t exp = 0;
            if (g_thr[i].tid.compare_exchange_strong(exp, tid, std::memory_order_relaxed) || exp == tid) {
                t_slot = i;
                return &g_thr[i];
            }
        }
    }
    g_thr_full.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

// --- per constant buffer (matrix-looking writes only) -----------------------
struct BufStat {
    std::atomic<uint32_t> size, kmask, strict, loose, hashchg;
    std::atomic<uint32_t> tid[4];
    std::atomic<uint64_t> lasthash;
    std::atomic<uint16_t> wr[kKinds][kFrames];
};
PtrMap<kBsCap> g_bs;
BufStat g_bsv[kBsCap];

// --- render-target sizes: (kind, packed size) -> per-frame count ------------
struct RtAgg {
    std::atomic<uint64_t> key;
    std::atomic<uint32_t> n[kFrames];
};
RtAgg g_rt[kRtCap];

RtAgg* rt_agg(uint64_t key) {
    const uint32_t h = hp((uintptr_t)(key * 0x9E3779B97F4A7C15ull), (uint32_t)(kRtCap - 1));
    for (uint32_t i = 0; i < kRtCap; ++i) {
        RtAgg& a = g_rt[(h + i) & (kRtCap - 1)];
        const uint64_t k = a.key.load(std::memory_order_relaxed);
        if (k == key) return &a;
        if (k == 0) {
            uint64_t exp = 0;
            if (a.key.compare_exchange_strong(exp, key, std::memory_order_relaxed) || exp == key) return &a;
        }
    }
    g_rt_full.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

// --- helpers ----------------------------------------------------------------
inline int bucket() {
    const uint64_t f = d3d11::frame_count();
    const uint64_t s = g_start_frame.load(std::memory_order_relaxed);
    if (f < s) return -1;
    const uint64_t b = f - s;
    return b < (uint64_t)kFrames ? (int)b : -1;
}

thread_local int t_exec = 0;   // depth of the real ExecuteCommandList on this thread

// 0 immediate, 1 deferred, 2 unknown, 3 immediate-class replay inside
// ExecuteCommandList. The tap's own ctx slot is returned too.
int ctx_kind(ID3D11DeviceContext* c, int* slot_out) {
    const int s = g_ctxc.slot((uintptr_t)c);
    if (slot_out) *slot_out = s;
    if (s < 0) { g_ctx_full.fetch_add(1, std::memory_order_relaxed); return kUnk; }
    uint64_t v = g_ctxc.val[s].load(std::memory_order_relaxed);
    if (!v) {
        const UINT t = c->GetType();   // a plain getter; once per context pointer
        v = (t == D3D11_DEVICE_CONTEXT_IMMEDIATE) ? 1 : (t == D3D11_DEVICE_CONTEXT_DEFERRED ? 2 : 3);
        g_ctxc.val[s].store(v, std::memory_order_relaxed);
        g_ctx_tid[s].store(GetCurrentThreadId(), std::memory_order_relaxed);
    }
    const int k = (int)v - 1;
    return (k == kImm && t_exec > 0) ? kRepl : k;
}

uint64_t buf_info(ID3D11Resource* r) {
    const int s = g_bufc.slot((uintptr_t)r);
    if (s < 0) { g_cache_full.fetch_add(1, std::memory_order_relaxed); return 0; }
    uint64_t v = g_bufc.val[s].load(std::memory_order_relaxed);
    if (v) return v;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    v = kValid;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
        if (d.BindFlags == D3D11_BIND_CONSTANT_BUFFER) v |= kIsCB | (uint64_t)d.ByteWidth;
    }
    g_bufc.val[s].store(v, std::memory_order_relaxed);
    return v;
}

uint64_t view_size(ID3D11View* v, bool dsv) {
    const int s = g_viewc.slot((uintptr_t)v);
    if (s < 0) { g_cache_full.fetch_add(1, std::memory_order_relaxed); return kValid; }
    uint64_t r = g_viewc.val[s].load(std::memory_order_relaxed);
    if (r) return r;
    r = kValid;
    ID3D11Resource* res = nullptr;
    v->GetResource(&res);
    if (res) {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d{};
            static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
            UINT mip = 0;
            DXGI_FORMAT fmt = d.Format;
            if (dsv) {
                D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
                static_cast<ID3D11DepthStencilView*>(v)->GetDesc(&vd);
                if (vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) mip = vd.Texture2D.MipSlice;
                else if (vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) mip = vd.Texture2DArray.MipSlice;
                if (vd.Format != DXGI_FORMAT_UNKNOWN) fmt = vd.Format;
            } else {
                D3D11_RENDER_TARGET_VIEW_DESC vd{};
                static_cast<ID3D11RenderTargetView*>(v)->GetDesc(&vd);
                if (vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) mip = vd.Texture2D.MipSlice;
                else if (vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) mip = vd.Texture2DArray.MipSlice;
                if (vd.Format != DXGI_FORMAT_UNKNOWN) fmt = vd.Format;
            }
            const uint64_t w = std::max<UINT>(1u, d.Width >> mip) & 0xFFFF;
            const uint64_t h = std::max<UINT>(1u, d.Height >> mip) & 0xFFFF;
            r |= w | (h << 16) | ((uint64_t)((UINT)fmt & 0xFF) << 32) | (dsv ? (1ull << 40) : 0);
        }
        res->Release();
    }
    g_viewc.val[s].store(r, std::memory_order_relaxed);
    return r;
}

bool safe_copy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// mask bits: 1 bottom row (0,0,0,1)   2 right column (0,0,0,1)
//            4 bottom row (0,0,+-1,0) 8 right column (0,0,+-1,0)   (perspective)
//            16 LOOSE: any finite block with content in the 3x3 and a nonzero w
// Bits 1..8 are the strict view/world/projection shapes, bit 16 alone is the
// permissive reading of "a column with w nonzero and finite" and WILL include
// non-matrix data (colours, parameters). The dump keeps the two apart.
struct MatHit { uint8_t mask; uint8_t block; };

inline bool near_(float x, float t) { return std::fabs(x - t) < 1e-3f; }

MatHit matrix_scan(const float* f, size_t nf) {
    for (size_t s = 0; s + 16 <= nf; s += 4) {
        const float* m = f + s;
        bool ok = true;
        for (int i = 0; i < 16; ++i)
            if (!(std::fabs(m[i]) < 1e7f)) { ok = false; break; }   // also rejects NaN and inf
        if (!ok) continue;
        float rot = 0.0f;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) rot += std::fabs(m[r * 4 + c]);
        if (rot < 1e-3f) continue;
        uint8_t mask = 0;
        if (near_(m[12], 0) && near_(m[13], 0) && near_(m[14], 0) && near_(m[15], 1)) mask |= 1;
        if (near_(m[3], 0) && near_(m[7], 0) && near_(m[11], 0) && near_(m[15], 1)) mask |= 2;
        if (near_(m[12], 0) && near_(m[13], 0) && near_(std::fabs(m[14]), 1) && near_(m[15], 0)) mask |= 4;
        if (near_(m[3], 0) && near_(m[7], 0) && near_(std::fabs(m[11]), 1) && near_(m[15], 0)) mask |= 8;
        if (!mask && std::fabs(m[15]) > 1e-6f) mask |= 16;
        if (mask) return {mask, (uint8_t)(s / 4)};
    }
    return {0, 0};
}

uint64_t fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// One constant-buffer write, from an Unmap (src 1) or an UpdateSubresource (src 2).
void cb_write(ID3D11DeviceContext* c, ID3D11Resource* res, uint32_t size,
              const void* data, uint32_t src, uint32_t aux) {
    const int b = bucket();
    if (b < 0 || !data || !size) return;
    const int k = ctx_kind(c, nullptr);
    g_cbw[k][b].fetch_add(1, std::memory_order_relaxed);

    alignas(16) uint8_t buf[192];
    const size_t n = std::min<size_t>(size, sizeof(buf)) & ~size_t(3);
    if (!n) return;
    if (!safe_copy(buf, data, n)) { g_cb_fault.fetch_add(1, std::memory_order_relaxed); return; }
    const uint64_t hash = fnv64(buf, std::min<size_t>(n, 64));
    const MatHit hit = matrix_scan((const float*)buf, n / 4);
    if (!hit.mask) return;
    g_cbm[k][b].fetch_add(1, std::memory_order_relaxed);

    const int s = g_bs.slot((uintptr_t)res);
    bool raw = true;
    if (s < 0) {
        g_bs_full.fetch_add(1, std::memory_order_relaxed);
    } else {
        BufStat& bs = g_bsv[s];
        bs.size.store(size, std::memory_order_relaxed);
        bs.kmask.fetch_or(1u << k, std::memory_order_relaxed);
        const uint32_t tid = GetCurrentThreadId();
        for (int i = 0; i < 4; ++i) {
            uint32_t t = bs.tid[i].load(std::memory_order_relaxed);
            if (t == tid) break;
            if (t == 0) {
                uint32_t exp = 0;
                if (bs.tid[i].compare_exchange_strong(exp, tid, std::memory_order_relaxed) || exp == tid) break;
            }
        }
        uint32_t loose_seen = 0;
        if (hit.mask & 0xF) bs.strict.fetch_add(1, std::memory_order_relaxed);
        else loose_seen = bs.loose.fetch_add(1, std::memory_order_relaxed);
        const uint64_t prev = bs.lasthash.exchange(hash, std::memory_order_relaxed);
        if (prev && prev != hash) bs.hashchg.fetch_add(1, std::memory_order_relaxed);
        bs.wr[k][b].fetch_add(1, std::memory_order_relaxed);
        if (!(hit.mask & 0xF) && loose_seen >= 64) raw = false;   // keep the raw list for real matrices
    }
    if (raw)
        push(kEvCbWrite, (uint16_t)k, aux, (uint64_t)(uintptr_t)res,
             (uint64_t)size | ((uint64_t)src << 32) | ((uint64_t)k << 40) |
                 ((uint64_t)hit.mask << 48) | ((uint64_t)hit.block << 56),
             hash, (uint64_t)(uintptr_t)c);
}

struct Pend {
    ID3D11DeviceContext* c;
    ID3D11Resource*      r;
    UINT                 sub;
    UINT                 type;
    const void*          data;
    uint32_t             size;
};
thread_local Pend t_pend[8];
thread_local int  t_np = 0;

}  // namespace

// ---------------------------------------------------------------------------
// the taps
// ---------------------------------------------------------------------------
void tap_draw(ID3D11DeviceContext* c) {
    if (!(g_gate.load(std::memory_order_relaxed) & 1)) return;   // BUILD 246: only the anatomy window wants draws
    const int b = bucket();
    if (b < 0) return;
    int slot = -1;
    const int k = ctx_kind(c, &slot);
    if (slot >= 0) {
        g_ctx_draws[slot].fetch_add(1, std::memory_order_relaxed);
        g_ctx_total[slot].fetch_add(1, std::memory_order_relaxed);
    }
    g_draws[k][b].fetch_add(1, std::memory_order_relaxed);
    if (ThrEnt* t = thr()) {
        t->draws[b].fetch_add(1, std::memory_order_relaxed);
        t->total.fetch_add(1, std::memory_order_relaxed);
        t->kmask.fetch_or(1u << k, std::memory_order_relaxed);
    }
}

void tap_rt(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
            ID3D11DepthStencilView* dsv) {
    if (!(g_gate.load(std::memory_order_relaxed) & 1)) return;      // BUILD 246
    if (n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;   // UAV-only change
    const int b = bucket();
    if (b < 0) return;
    const int k = ctx_kind(c, nullptr);
    ID3D11View* view = nullptr;
    bool is_dsv = false;
    UINT nn = 0;
    if (rtvs) {
        for (UINT i = 0; i < n && i < 8; ++i)
            if (rtvs[i]) { if (!view) view = rtvs[i]; ++nn; }
    }
    if (!view && dsv) { view = dsv; is_dsv = true; }
    const uint64_t sz = view ? view_size(view, is_dsv) : kValid;   // 0x0 = nothing bound
    g_rtb[k][b].fetch_add(1, std::memory_order_relaxed);
    if (RtAgg* a = rt_agg(sz | ((uint64_t)(k + 1) << 48)))
        a->n[b].fetch_add(1, std::memory_order_relaxed);
    push(kEvRtBind, (uint16_t)k, n | (nn << 8) | ((dsv ? 1u : 0u) << 16),
         (uint64_t)(uintptr_t)c, sz, (uint64_t)(uintptr_t)view, 0);
}

void tap_finish(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList* cl, HRESULT hr, uint64_t t0) {
    if (!(g_gate.load(std::memory_order_relaxed) & 1)) return;      // BUILD 246
    const int b = bucket();
    int slot = -1;
    const int k = ctx_kind(c, &slot);
    uint32_t draws = 0;
    if (slot >= 0) {
        draws = g_ctx_draws[slot].exchange(0, std::memory_order_relaxed);
        g_ctx_fin[slot].fetch_add(1, std::memory_order_relaxed);
    }
    if (b >= 0) g_fin[b].fetch_add(1, std::memory_order_relaxed);
    const uint64_t dur = __rdtsc() - t0;
    push(kEvFinish, (uint16_t)k, (uint32_t)(dur > 0xFFFFFFFFull ? 0xFFFFFFFFull : dur),
         (uint64_t)(uintptr_t)c, (uint64_t)(uintptr_t)cl, draws,
         (uint64_t)(restore ? 1 : 0) | ((uint64_t)(uint32_t)hr << 32), t0);
}

void tap_execute(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore, uint64_t t0) {
    if (!(g_gate.load(std::memory_order_relaxed) & 1)) return;      // BUILD 246
    const int b = bucket();
    int slot = -1;
    const int k = ctx_kind(c, &slot);
    if (slot >= 0) g_ctx_exe[slot].fetch_add(1, std::memory_order_relaxed);
    if (b >= 0) g_exe[b].fetch_add(1, std::memory_order_relaxed);
    const uint64_t dur = __rdtsc() - t0;
    push(kEvExecute, (uint16_t)k, (uint32_t)(dur > 0xFFFFFFFFull ? 0xFFFFFFFFull : dur),
         (uint64_t)(uintptr_t)c, (uint64_t)(uintptr_t)cl, restore ? 1 : 0, 0, t0);
}

void tap_map(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, D3D11_MAP type,
             const D3D11_MAPPED_SUBRESOURCE* ms, HRESULT hr) {
    const int gate = g_gate.load(std::memory_order_relaxed);        // BUILD 246
    if (gate & 2) cbdump::tap_map(c, res, sub, type, ms, hr);
    if ((gate & 4) && SUCCEEDED(hr) && ms && ms->pData)              // BUILD 270: gtrec, the 1008-byte per-view buffer
        gtrec::tap_map(c, res, (uint32_t)sub, (uint32_t)type, ms->pData);
    if (!(gate & 1)) return;
    if (FAILED(hr) || !ms || !ms->pData || !res || type == D3D11_MAP_READ) return;
    if (bucket() < 0) return;
    const uint64_t info = buf_info(res);
    if (!(info & kIsCB)) return;
    if (t_np == 8) { memmove(&t_pend[0], &t_pend[1], 7 * sizeof(Pend)); t_np = 7; }
    t_pend[t_np++] = {c, res, sub, (UINT)type, ms->pData, (uint32_t)(info & 0xFFFFFFFFull)};
}

void tap_unmap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub) {
    const int gate = g_gate.load(std::memory_order_relaxed);        // BUILD 246
    if (gate & 2) cbdump::tap_unmap(c, res, sub);
    if (gate & 4) gtrec::tap_unmap(c, res, (uint32_t)sub);            // BUILD 270
    if (!(gate & 1)) return;
    for (int i = t_np - 1; i >= 0; --i) {
        if (t_pend[i].c == c && t_pend[i].r == res && t_pend[i].sub == sub) {
            const Pend p = t_pend[i];
            for (int j = i; j + 1 < t_np; ++j) t_pend[j] = t_pend[j + 1];
            --t_np;
            cb_write(c, res, p.size, p.data, 1, p.type);
            return;
        }
    }
}

void tap_update(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, const D3D11_BOX* box,
                const void* src) {
    (void)sub;
    const int gate = g_gate.load(std::memory_order_relaxed);        // BUILD 246
    if (gate & 2) cbdump::tap_update(c, res, sub, box, src);
    if (gate & 4) gtrec::tap_update(c, res, (uint32_t)sub, box, src); // BUILD 270
    if (!(gate & 1)) return;
    if (!src || !res || bucket() < 0) return;
    const uint64_t info = buf_info(res);
    if (!(info & kIsCB)) return;
    uint32_t size = (uint32_t)(info & 0xFFFFFFFFull);
    if (box) size = box->right > box->left ? std::min<uint32_t>(size, box->right - box->left) : 0;
    cb_write(c, res, size, src, 2, 0);
}

// ---------------------------------------------------------------------------
// state, cfg, the Present boundary
// ---------------------------------------------------------------------------
void exec_enter() { ++t_exec; }
void exec_leave() { if (t_exec > 0) --t_exec; }

void set_enabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void on_stereo_submit() {
    if (g_first_stereo_ms.load(std::memory_order_relaxed)) return;
    g_first_stereo_ms.store(GetTickCount64(), std::memory_order_relaxed);
}

void on_present(unsigned long long frames) {
    if (g_state.load(std::memory_order_relaxed) != 2) return;
    const uint64_t s = g_start_frame.load(std::memory_order_relaxed);
    if (frames < s) return;
    const uint64_t b = frames - s;
    if (b <= (uint64_t)kFrames) {
        const uint64_t ts = __rdtsc();
        g_pts[b] = ts;
        if (g_watch_ctx) {
            void* bad = nullptr;
            const int slot = drawhook::anatomy_vtable_unhooked(g_watch_ctx, &bad);
            g_watch_n.fetch_add(1, std::memory_order_relaxed);
            if (slot >= 0) {
                g_unhooked_n.fetch_add(1, std::memory_order_relaxed);
                g_bad_slot.store(slot, std::memory_order_relaxed);
                g_bad_fn.store((uintptr_t)bad, std::memory_order_relaxed);
            }
        }
        push(kEvPresent, kImm, (uint32_t)b, frames, GetCurrentThreadId(), 0, 0, ts);
    }
    if (b >= (uint64_t)kFrames) {
        int exp = 2;
        if (g_state.compare_exchange_strong(exp, 3, std::memory_order_release)) gate_clear(1);   // BUILD 246
    }
}

namespace {

// ---------------------------------------------------------------------------
// the dump (its own thread; file I/O is allowed here and nowhere in the taps)
// ---------------------------------------------------------------------------
double g_tsc_hz = 0.0;

double ms_since_arm(uint64_t ts) {
    return ts >= g_tsc0 ? (double)(ts - g_tsc0) * 1000.0 / g_tsc_hz : -(double)(g_tsc0 - ts) * 1000.0 / g_tsc_hz;
}

const char* kind_name(int k) { return k == kImm ? "imm" : (k == kDef ? "def" : (k == kRepl ? "rpl" : "???")); }

std::string kmask_str(uint32_t m) {
    std::string s;
    if (m & 1) s += "imm";
    if (m & 2) s += s.empty() ? "def" : "+def";
    if (m & 4) s += s.empty() ? "???" : "+???";
    if (m & 8) s += s.empty() ? "rpl" : "+rpl";
    return s.empty() ? "-" : s;
}

std::string size_str(uint64_t packed) {
    const unsigned w = (unsigned)(packed & 0xFFFF), h = (unsigned)((packed >> 16) & 0xFFFF);
    const unsigned fmt = (unsigned)((packed >> 32) & 0xFF);
    char b[64];
    if (!w && !h) return "none";
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%ux%u%s/f%u", w, h, ((packed >> 40) & 1) ? "(depth)" : "", fmt);
    return b;
}

std::string rt_top(int kind, int b) {   // kind: kImm or kDef; replay binds are folded into the imm list
    struct E { uint64_t sz; uint32_t n; };
    std::vector<E> v;
    for (size_t i = 0; i < kRtCap; ++i) {
        const uint64_t key = g_rt[i].key.load(std::memory_order_relaxed);
        if (!key) continue;
        const int kk = (int)((key >> 48) & 0x7FFF) - 1;
        if (kk != kind && !(kind == kImm && kk == kRepl)) continue;   // replay binds shown with imm, marked in the table below
        const uint32_t n = g_rt[i].n[b].load(std::memory_order_relaxed);
        if (n) v.push_back({key & 0xFFFFFFFFFFFFull, n});
    }
    std::sort(v.begin(), v.end(), [](const E& a, const E& c) { return a.n > c.n; });
    std::string s;
    for (size_t i = 0; i < v.size() && i < 3; ++i) {
        char t[96];
        _snprintf_s(t, sizeof(t), _TRUNCATE, "%s%s:%u", i ? " " : "", size_str(v[i].sz).c_str(), v[i].n);
        s += t;
    }
    if (v.size() > 3) s += " +" + std::to_string(v.size() - 3);
    return s.empty() ? "-" : s;
}

DWORD WINAPI dump_thread(LPVOID) {
    Sleep(1000);   // let any tap that passed the gate before the stop finish writing

    LARGE_INTEGER q1, qf;
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    const uint64_t t1 = __rdtsc();
    const double secs = (double)((uint64_t)q1.QuadPart - g_qpc0) / (double)qf.QuadPart;
    g_tsc_hz = secs > 0.1 ? (double)(t1 - g_tsc0) / secs : 3.0e9;

    const std::wstring path = log::data_dir() + L"\\anatomy-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        LOG_ERROR("anatomy: could not open the dump file (err %lu).", GetLastError());
        g_state.store(5, std::memory_order_release);
        return 0;
    }
    static char fbuf[1 << 16];
    setvbuf(f, fbuf, _IOFBF, sizeof(fbuf));

    const uint32_t nev = std::min<uint32_t>(g_head.load(), kEvCap);
    const uint64_t start = g_start_frame.load();
    int last_b = kFrames - 1;
    for (int b = kFrames; b >= 1; --b) if (g_pts[b]) { last_b = b - 1; break; }
    const int rows = last_b + 1;
    const unsigned long rtid = d3d11::render_thread_id();

    fprintf(f, "GRW-VR FRAME ANATOMY, build 241, observation only. pid %lu, render thread (first Present) tid %lu\n",
            GetCurrentProcessId(), rtid);
    fprintf(f, "window: Present #%llu .. #%llu (%d rows), opened %.1f s after the first stereo layer; tsc %.3f GHz\n",
            (unsigned long long)start, (unsigned long long)(start + rows), rows,
            (double)(g_arm_ms - g_first_stereo_ms.load()) / 1000.0, g_tsc_hz / 1e9);
    fprintf(f, "ROW b = work recorded while frame_count() == start+b, i.e. between Present #(start+b) and Present #(start+b+1). "
               "Row 0 is partial (opened mid-frame). Deferred-context work recorded by a worker lands in the row current when "
               "the worker made the call, which can lag the render thread.\n");
    fprintf(f, "events: %u stored of cap %u; per type attempted/dropped: present %u/%u finish %u/%u execute %u/%u "
               "cbwrite %u/%u rtbind %u/%u; buffer-full drops %u\n",
            nev, kEvCap, g_tn[1].load(), g_tdrop[1].load(), g_tn[2].load(), g_tdrop[2].load(),
            g_tn[3].load(), g_tdrop[3].load(), g_tn[4].load(), g_tdrop[4].load(), g_tn[5].load(),
            g_tdrop[5].load(), g_full.load());
    fprintf(f, "hooks: %u function(s) newly detoured at install. vtable watch at Present: %u presents checked, %u of them had an "
               "immediate-context entry point that is NOT hooked (first bad slot %d, entry 0x%llX); nonzero means immediate draws "
               "or writes may be missing from the counts below.\n",
            g_install_n.load(), g_watch_n.load(), g_unhooked_n.load(), g_bad_slot.load(),
            (unsigned long long)g_bad_fn.load());
    fprintf(f, "table pressure (any nonzero means a count below is a LOWER bound): ctx-full %u thread-full %u "
               "cache-full %u bufstat-full %u rtagg-full %u; constant-buffer reads that faulted %u\n\n",
            g_ctx_full.load(), g_thr_full.load(), g_cache_full.load(), g_bs_full.load(), g_rt_full.load(),
            g_cb_fault.load());

    // ---- SUMMARY, per frame ----
    fprintf(f, "=== SUMMARY, per frame ===\n");
    fprintf(f, "legend: thr = distinct threads that recorded draws; dI/dD/dR = draws on immediate/deferred contexts and "
               "immediate-class calls made by the runtime while replaying a command list inside ExecuteCommandList (R, rpl); "
               "fin/exe = FinishCommandList/ExecuteCommandList calls; rtI/rtD/rtR = render-target binds; "
               "cbI/cbD/cbR = constant-buffer writes (all), mxI/mxD/mxR = those whose first 192 bytes held a matrix-looking block "
               "(strict or loose, see the buffer table); dt = ms between the Present entries that bound the row\n");
    fprintf(f, "%4s %6s %7s | %3s %6s %6s %5s | %3s %3s | %4s %4s %4s | %5s %5s %5s | %4s %4s %4s | RT sizes imm | RT sizes def\n",
            "b", "frame", "dt_ms", "thr", "dI", "dD", "dR", "fin", "exe", "rtI", "rtD", "rtR", "cbI", "cbD", "cbR", "mxI", "mxD", "mxR");
    uint32_t thr_min = 0xFFFFFFFF, thr_max = 0;
    uint64_t thr_sum = 0, fin_tot = 0, exe_tot = 0;
    uint32_t fin_min = 0xFFFFFFFF, fin_max = 0;
    for (int b = 0; b < rows; ++b) {
        uint32_t nthr = 0;
        for (int i = 0; i < kThrCap; ++i)
            if (g_thr[i].tid.load() && g_thr[i].draws[b].load()) ++nthr;
        const double dt = (g_pts[b + 1] && (b > 0 ? g_pts[b] : g_tsc0)) ?
            (double)(g_pts[b + 1] - (b > 0 ? g_pts[b] : g_tsc0)) * 1000.0 / g_tsc_hz : -1.0;
        const uint32_t fin = g_fin[b].load(), exe = g_exe[b].load();
        fprintf(f, "%4d %6llu %7.2f | %3u %6u %6u %5u | %3u %3u | %4u %4u %4u | %5u %5u %5u | %4u %4u %4u | %s | %s\n",
                b, (unsigned long long)(start + b), dt, nthr, g_draws[kImm][b].load(), g_draws[kDef][b].load(),
                g_draws[kRepl][b].load(), fin, exe, g_rtb[kImm][b].load(), g_rtb[kDef][b].load(), g_rtb[kRepl][b].load(),
                g_cbw[kImm][b].load(), g_cbw[kDef][b].load(), g_cbw[kRepl][b].load(), g_cbm[kImm][b].load(),
                g_cbm[kDef][b].load(), g_cbm[kRepl][b].load(), rt_top(kImm, b).c_str(), rt_top(kDef, b).c_str());
        if (b > 0) {   // row 0 is partial
            thr_min = std::min(thr_min, nthr); thr_max = std::max(thr_max, nthr); thr_sum += nthr;
            fin_min = std::min(fin_min, fin); fin_max = std::max(fin_max, fin);
            fin_tot += fin; exe_tot += exe;
        }
    }
    const int full = std::max(1, rows - 1);
    fprintf(f, "\nper full row: draw threads min %u / mean %.2f / max %u; FinishCommandList min %u / mean %.2f / max %u; "
               "ExecuteCommandList mean %.2f\n",
            thr_min == 0xFFFFFFFF ? 0 : thr_min, (double)thr_sum / full, thr_max,
            fin_min == 0xFFFFFFFF ? 0 : fin_min, (double)fin_tot / full, fin_max, (double)exe_tot / full);

    // ---- render-target sizes over the window ----
    fprintf(f, "\n=== RENDER-TARGET SIZES BOUND (whole window) ===\n");
    fprintf(f, "%-4s %-26s %8s %8s %9s\n", "ctx", "size/format(first RTV, else depth)", "binds", "frames", "per-frame");
    {
        struct E { int k; uint64_t sz; uint64_t n; int fr; };
        std::vector<E> v;
        for (size_t i = 0; i < kRtCap; ++i) {
            const uint64_t key = g_rt[i].key.load();
            if (!key) continue;
            uint64_t tot = 0; int fr = 0;
            for (int b = 0; b < rows; ++b) { const uint32_t n = g_rt[i].n[b].load(); tot += n; if (n) ++fr; }
            v.push_back({(int)((key >> 48) & 0x7FFF) - 1, key & 0xFFFFFFFFFFFFull, tot, fr});
        }
        std::sort(v.begin(), v.end(), [](const E& a, const E& c) { return a.k != c.k ? a.k < c.k : a.n > c.n; });
        for (const E& e : v)
            fprintf(f, "%-4s %-26s %8llu %8d %9.2f\n", kind_name(e.k), size_str(e.sz).c_str(),
                    (unsigned long long)e.n, e.fr, e.fr ? (double)e.n / e.fr : 0.0);
    }

    // ---- matrix-looking constant buffers ----
    fprintf(f, "\n=== CONSTANT BUFFERS THAT RECEIVED MATRIX-LOOKING WRITES ===\n");
    fprintf(f, "strict = last row or last column was (0,0,0,1) or a perspective (0,0,+-1,0) in a 4x4 float block inside the "
               "first 192 bytes; loose = only 'finite block, content in the 3x3, w nonzero' (will include non-matrix data). "
               "hash = FNV-1a of the first 64 bytes; hashchg = writes whose hash differed from the previous write. "
               "wr/frame = min/median/max of (imm+def+rpl) matrix-looking writes over the rows that had any. kinds rpl = "
               "immediate-class calls made by the runtime while replaying a command list.\n");
    fprintf(f, "%-18s %6s %-9s %-26s %7s %7s %7s %5s %5s %9s %8s\n", "buffer", "bytes", "kinds", "writer tids (first 4)",
            "strict", "loose", "hashchg", "rowsI", "rowsD", "wr/frame", "writes");
    struct Row { int s; uint32_t total; };
    std::vector<Row> order;
    for (size_t i = 0; i < kBsCap; ++i)
        if (g_bs.key[i].load()) {
            uint32_t tot = 0;
            for (int k = 0; k < kKinds; ++k) for (int b = 0; b < rows; ++b) tot += g_bsv[i].wr[k][b].load();
            order.push_back({(int)i, tot});
        }
    std::sort(order.begin(), order.end(), [](const Row& a, const Row& c) { return a.total > c.total; });
    for (const Row& r : order) {
        BufStat& bs = g_bsv[r.s];
        std::string tids;
        for (int i = 0; i < 4; ++i)
            if (uint32_t t = bs.tid[i].load()) { tids += (tids.empty() ? "" : ","); tids += std::to_string(t); }
        std::vector<uint32_t> per;
        int rI = 0, rD = 0;
        for (int b = 0; b < rows; ++b) {
            const uint32_t i = bs.wr[kImm][b].load(), d = bs.wr[kDef][b].load(), u = bs.wr[kUnk][b].load(),
                           rp = bs.wr[kRepl][b].load();
            if (i) ++rI;
            if (d) ++rD;
            if (i + d + u + rp) per.push_back(i + d + u + rp);
        }
        std::sort(per.begin(), per.end());
        char rng[48] = "-";
        if (!per.empty()) _snprintf_s(rng, sizeof(rng), _TRUNCATE, "%u/%u/%u", per.front(), per[per.size() / 2], per.back());
        char name[32];
        _snprintf_s(name, sizeof(name), _TRUNCATE, "0x%p", (void*)g_bs.key[r.s].load());
        fprintf(f, "%-18s %6u %-9s %-26s %7u %7u %7u %5d %5d %9s %8u\n", name, bs.size.load(),
                kmask_str(bs.kmask.load()).c_str(), tids.c_str(), bs.strict.load(), bs.loose.load(), bs.hashchg.load(),
                rI, rD, rng, r.total);
    }
    fprintf(f, "(%zu buffer(s))\n", order.size());
    // per-frame detail for the busiest few
    for (size_t n = 0; n < order.size() && n < 12; ++n) {
        BufStat& bs = g_bsv[order[n].s];
        fprintf(f, "per-frame 0x%p imm:", (void*)g_bs.key[order[n].s].load());
        for (int b = 0; b < rows; ++b) fprintf(f, " %u", bs.wr[kImm][b].load());
        fprintf(f, "\nper-frame 0x%p def:", (void*)g_bs.key[order[n].s].load());
        for (int b = 0; b < rows; ++b) fprintf(f, " %u", bs.wr[kDef][b].load());
        fprintf(f, "\nper-frame 0x%p rpl:", (void*)g_bs.key[order[n].s].load());
        for (int b = 0; b < rows; ++b) fprintf(f, " %u", bs.wr[kRepl][b].load());
        fprintf(f, "\n");
    }

    // ---- threads ----
    fprintf(f, "\n=== THREADS THAT RECORDED DRAWS ===\n");
    fprintf(f, "%8s %-8s %-9s %10s %9s\n", "tid", "role", "ctx kinds", "draws", "rows>0");
    for (int i = 0; i < kThrCap; ++i) {
        const uint32_t tid = g_thr[i].tid.load();
        if (!tid) continue;
        int rr = 0;
        for (int b = 0; b < rows; ++b) if (g_thr[i].draws[b].load()) ++rr;
        fprintf(f, "%8u %-8s %-9s %10llu %9d\n", tid, tid == rtid ? "render" : "worker",
                kmask_str(g_thr[i].kmask.load()).c_str(), (unsigned long long)g_thr[i].total.load(), rr);
    }

    // ---- contexts ----
    fprintf(f, "\n=== CONTEXTS SEEN ===\n");
    fprintf(f, "%-18s %-5s %12s %8s %8s %9s\n", "context", "kind", "draws", "finish", "execute", "first tid");
    for (size_t i = 0; i < kCtxCap; ++i) {
        const uintptr_t key = g_ctxc.key[i].load();
        if (!key) continue;
        const uint64_t v = g_ctxc.val[i].load();
        fprintf(f, "0x%-16p %-5s %12llu %8u %8u %9u\n", (void*)key, v ? kind_name((int)v - 1) : "?",
                (unsigned long long)g_ctx_total[i].load(), g_ctx_fin[i].load(), g_ctx_exe[i].load(), g_ctx_tid[i].load());
    }

    // ---- raw events ----
    fprintf(f, "\n=== EVENTS (append order, t = ms since the window opened) ===\n");
    fprintf(f, "PRESENT b frame tid | FINISH ctx cmdlist draws-since-last-finish restore hr dur_us | EXECUTE ctx cmdlist "
               "restore dur_us | CBWRITE buffer bytes src(1 map,2 update) kind mask(1 row,2 col,4 persp row,8 persp col,16 loose) "
               "block hash ctx maptype | RTBIND ctx n nrtv depthbound size view\n");
    for (uint32_t i = 0; i < nev; ++i) {
        const Ev& e = g_ev[i];
        const uint64_t ts = e.ts.load(std::memory_order_acquire);
        if (!ts) continue;
        const double t = ms_since_arm(ts);
        const double us = (double)e.aux * 1e6 / g_tsc_hz;
        switch (e.type) {
            case kEvPresent:
                fprintf(f, "%10.3f tid=%-6u f=%-8u PRESENT   b=%u frame=%llu tid=%llu\n", t, e.tid, e.frame, e.aux,
                        (unsigned long long)e.p[0], (unsigned long long)e.p[1]);
                break;
            case kEvFinish:
                fprintf(f, "%10.3f tid=%-6u f=%-8u FINISH    %s ctx=0x%llX cl=0x%llX draws=%llu restore=%llu hr=0x%08X dur=%.1fus\n",
                        t, e.tid, e.frame, kind_name(e.kind), (unsigned long long)e.p[0], (unsigned long long)e.p[1],
                        (unsigned long long)e.p[2], (unsigned long long)(e.p[3] & 1), (unsigned)(e.p[3] >> 32), us);
                break;
            case kEvExecute:
                fprintf(f, "%10.3f tid=%-6u f=%-8u EXECUTE   %s ctx=0x%llX cl=0x%llX restore=%llu dur=%.1fus\n", t, e.tid,
                        e.frame, kind_name(e.kind), (unsigned long long)e.p[0], (unsigned long long)e.p[1],
                        (unsigned long long)e.p[2], us);
                break;
            case kEvCbWrite:
                fprintf(f, "%10.3f tid=%-6u f=%-8u CBWRITE   %s buf=0x%llX bytes=%u src=%u mask=%u blk=%u hash=%016llX ctx=0x%llX maptype=%u\n",
                        t, e.tid, e.frame, kind_name(e.kind), (unsigned long long)e.p[0], (unsigned)(e.p[1] & 0xFFFFFFFFu),
                        (unsigned)((e.p[1] >> 32) & 0xFF), (unsigned)((e.p[1] >> 48) & 0xFF), (unsigned)((e.p[1] >> 56) & 0xFF),
                        (unsigned long long)e.p[2], (unsigned long long)e.p[3], e.aux);
                break;
            case kEvRtBind:
                fprintf(f, "%10.3f tid=%-6u f=%-8u RTBIND    %s ctx=0x%llX n=%u nrtv=%u depth=%u size=%s view=0x%llX\n", t, e.tid,
                        e.frame, kind_name(e.kind), (unsigned long long)e.p[0], e.aux & 0xFF, (e.aux >> 8) & 0xFF,
                        (e.aux >> 16) & 1, size_str(e.p[1]).c_str(), (unsigned long long)e.p[2]);
                break;
            default: break;
        }
    }
    fclose(f);
    LOG_INFO("anatomy: wrote %ls (%u events, %d rows). Read the SUMMARY block at the top.", path.c_str(), nev, rows);
    g_state.store(5, std::memory_order_release);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// the state machine (init thread, once a second)
// ---------------------------------------------------------------------------
// BUILD 246: the hooks go in when anatomy_capture OR cbdump_capture is on. The anatomy WINDOW
// opens only when anatomy_capture is on; with only cbdump_capture on the state stays 1
// (hooked) for the rest of the run and CbDump.cpp runs its own window.
int hooks_status() { return g_hooks.load(std::memory_order_acquire); }
uint64_t first_stereo_ms() { return g_first_stereo_ms.load(std::memory_order_relaxed); }
int context_kind(ID3D11DeviceContext* c) { return ctx_kind(c, nullptr); }
void table_pressure(uint32_t* ctx_full, uint32_t* cache_full) {
    if (ctx_full) *ctx_full = g_ctx_full.load(std::memory_order_relaxed);
    if (cache_full) *cache_full = g_cache_full.load(std::memory_order_relaxed);
}
uint32_t constant_buffer_bytes(ID3D11Resource* r) {
    const uint64_t v = buf_info(r);
    return (v & kIsCB) ? (uint32_t)(v & 0xFFFFFFFFull) : 0u;
}

void tick() {
    int st = g_state.load(std::memory_order_acquire);
    const bool want_an = g_enabled.load(std::memory_order_relaxed);
    // BUILD 270: the hook set is installed for gtrec too (cfg gtrec and gtrec_cb); the anatomy window still opens only
    // with anatomy_capture, and gtrec sets and clears its own gate bit (4).
    if (st >= 4 || (!want_an && !cbdump::enabled() && !gtrec::cb_wanted())) return;
    const uint64_t first = g_first_stereo_ms.load(std::memory_order_relaxed);
    if (!first) return;
    const uint64_t now = GetTickCount64();
    const uint64_t age = now - first;

    if (st == 0 && age >= kInstallMs) {
        const auto& s = d3d11::state();
        if (!s.ready || !s.device || !s.context) return;   // retry next tick
        const int n = drawhook::install_anatomy(s.device, s.context);
        if (n < 0) {
            g_hooks.store(-1, std::memory_order_release);
            g_state.store(6, std::memory_order_release);
            LOG_ERROR("anatomy: the hook set was refused (nothing written, see the draw: lines). No capture this run.");
            return;
        }
        g_install_n.store((uint32_t)n);
        g_watch_ctx = s.context;
        g_hooks.store(1, std::memory_order_release);
        g_state.store(1, std::memory_order_release);
        LOG_INFO("anatomy: %d d3d11.dll function(s) newly detoured. The %s window opens in %llu s.", n,
                 want_an ? "anatomy" : "cbdump (anatomy_capture is off; these hooks serve it)",
                 (unsigned long long)(age >= kArmMs ? 0 : (kArmMs - age) / 1000));
        st = 1;
    }
    if (st == 1 && age >= kArmMs) {
        if (!want_an) return;   // BUILD 246: hooks only; the cbdump window is CbDump.cpp's
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_qpc0 = (uint64_t)q.QuadPart;
        g_tsc0 = __rdtsc();
        g_arm_ms = now;
        g_start_frame.store(d3d11::frame_count(), std::memory_order_relaxed);
        g_state.store(2, std::memory_order_release);
        gate_set(1);   // BUILD 246
        LOG_INFO("anatomy: window OPEN at Present #%llu, capturing %d presents.",
                 (unsigned long long)d3d11::frame_count(), kFrames);
        return;
    }
    if (st == 2 && now - g_arm_ms > kGiveUpMs) {
        int exp = 2;
        if (g_state.compare_exchange_strong(exp, 3)) {
            gate_clear(1);   // BUILD 246
            LOG_WARN("anatomy: the window did not fill in %llu s; closing it with what it has.",
                     (unsigned long long)(kGiveUpMs / 1000));
        }
        st = g_state.load();
    }
    if (st == 3) {
        int exp = 3;
        if (g_state.compare_exchange_strong(exp, 4)) {
            LOG_INFO("anatomy: window CLOSED, dumping.");
            if (HANDLE t = CreateThread(nullptr, 0, dump_thread, nullptr, 0, nullptr)) CloseHandle(t);
            else g_state.store(5, std::memory_order_release);
        }
    }
}

}  // namespace anatomy
}  // namespace grwxr
