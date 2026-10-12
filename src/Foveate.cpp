// Foveate.cpp - BUILD 216/217: fixed foveated rendering by Variable Rate Shading.
// See Foveate.h for the why, the shape, and who it is for.
//
// The NVAPI calls follow fholger's vrperfkit (MIT, src/d3d11/
// d3d11_variable_rate_shading.cpp) exactly where they overlap: the capability
// check, the R8_UINT tile texture and its shading rate view, and the
// enable/disable through RSSetViewportsPixelShadingRates on two viewports.
// What differs is ours: the pattern is computed from the two eyes' frustums
// against the content's field (so the invisible margins are coarse), it is one
// pattern for both eyes, and the bind hook is a code detour in d3d11.dll that
// sees every context object, deferred ones included, because this engine
// records its scene on deferred contexts (DrawHook.h). NVAPI asks for
// NvAPI_D3D_RegisterDevice before any deferred-context call; it is made.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#pragma warning(push, 0)
#include "nvapi/nvapi.h"   // NVIDIA NVAPI SDK, MIT, vendored: src\nvapi\README-VENDORED.md
#pragma warning(pop)

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Foveate.h"
#include "D3D11Hook.h"
#include "DrawHook.h"
#include "HeadPose.h"
#include "Log.h"
#include "Perf.h"

namespace grwxr {
namespace ffr {
namespace {

enum State : int {
    kNotInit = 0,   // no device yet
    kNoNvapi,       // NvAPI_Initialize failed: not an NVIDIA machine, or no driver
    kNoVrs,         // NVIDIA, but the GPU has no variable rate shading (pre-Turing)
    kNotRequested,  // cfg ffr was 0 when the device appeared: hooks never installed
    kHooksFailed,   // DrawHook refused the render-target detours
    kActive,        // hooks in, VRS usable; ffr on/off is the run-time switch
    kDisabledErr,   // too many NVAPI errors: switched itself off for the run
};

const char* state_name(int s) {
    switch (s) {
        case kNotInit:      return "no-device-yet";
        case kNoNvapi:      return "no-nvapi";
        case kNoVrs:        return "no-vrs-on-this-gpu";
        case kNotRequested: return "not-requested-at-start";
        case kHooksFailed:  return "hooks-failed";
        case kActive:       return "active";
        case kDisabledErr:  return "disabled-after-errors";
        default:            return "?";
    }
}

// ---- cfg (init thread writes, hooks read atomics) --------------------------
Params            g_params{};          // init thread only
std::atomic<int>  g_on{0};
std::atomic<int>  g_suspended{0};      // BUILD 247: the true-stereo mode switches the mask off without touching g_on
std::atomic<int>  g_requested{-1};     // -1 until the first set_params
std::atomic<int>  g_horizontal{1};
std::atomic<int>  g_toggles{0};

// ---- state -----------------------------------------------------------------
std::atomic<int>  g_state{kNotInit};
std::atomic<int>  g_active{0};         // hooks installed and VRS usable; fail() can clear it
std::atomic<int>  g_hooked{0};         // the detours are in: the per-Present clear stays on for the run
std::atomic<int>  g_nvapi_up{0};
int               g_first_status    = 0;    // init thread
int               g_register_status = 0;    // init thread
unsigned          g_sm_major = 0, g_sm_minor = 0;

// ---- geometry, published by the present thread, double buffered ------------
struct Geometry {
    uint32_t w = 0, h = 0;
    float    wide = 0.0f;
    float    tan[2][4] = {};   // per eye: L, R, U, D
    int      valid = 0;
};
Geometry               g_geo[2];
std::atomic<Geometry*> g_geo_cur{nullptr};
int                    g_geo_write = 0;   // present thread only

// ---- the mask ----------------------------------------------------------------
struct Mask {
    ID3D11Texture2D*                 tex  = nullptr;
    ID3D11NvShadingRateResourceView* view = nullptr;
    uint32_t tw = 0, th = 0;
    uint32_t count[5] = {};   // tiles per rate index
    int      vis[4] = {};     // visible rectangle in content pixels: x0, x1, y0, y1
    float    hx = 0.0f, hy = 0.0f;
    Geometry geo;
    Params   par;
};
// BUILD 217: one mask per scene-sized render target. Build 216 matched the
// content's exact size and reached two binds a frame, the final passes into
// the 3840x2160 backbuffer, never the scene: the scene renders at the game's
// own Supersampling (0.9 in his GRW.ini, about 3456x1944). Now every distinct
// size the game binds is counted (the census below), and a mask is built for
// each size that looks like the scene: the content's aspect within one per
// cent, at least half its width, at most twice it. The masks are published as
// a table the hook matches by exact size; each Mask carries the size it was
// built for, so a bind can never pair a size with the wrong mask. Masks and
// tables live in static storage for the run, so a pointer once published is
// always valid.
constexpr int kMaxMasks = 64;      // a mask is at most about 130 KB; every mask ever built stays alive
Mask g_masks[kMaxMasks];           // init thread
int  g_nmasks = 0;
uint32_t g_rebuild_refused = 0;

struct MaskTable {
    int         n = 0;
    const Mask* m[kMaxMasks] = {};
};
// A table is published at most once per new mask, so with as many tables as
// masks the ring can never come round and a published table is never written
// again while a hook thread might still hold its pointer.
constexpr int kTables = kMaxMasks;
static_assert(kTables >= kMaxMasks, "at most one table publish per mask; the ring must never wrap");
MaskTable g_tables[kTables];
int       g_table_next = 0;        // init thread
std::atomic<const MaskTable*> g_table{nullptr};
std::atomic<uint32_t>         g_wide_bits{0};

// The census: every (width, height) bound, counted per second, and how many
// of those binds were on deferred contexts. Hook side: one 64-bit key per
// slot claimed by compare-exchange, so a slot is never half-written; counters
// are relaxed atomics. The init thread reads and zeroes the counts each second.
struct SizeSlot {
    std::atomic<uint64_t> key{0};   // (w << 32) | h; 0 = free
    std::atomic<uint64_t> n{0};
    std::atomic<uint64_t> nd{0};
};
// Slots are claimed for the run and never released; a renderer's distinct sizes
// in one session (the scene, its helpers, a downsample chain, probe cubemaps,
// the UI, video, load screens, then a whole new family after a Supersampling
// change) can reach thirty, so the table is sized well past that and the line
// prints its usage so exhaustion is visible before a new scene size is missed.
constexpr int kSizeSlots = 64;
SizeSlot g_sizes[kSizeSlots];
std::atomic<uint64_t> g_size_overflow{0};   // binds whose size found no free slot, for the run

inline void record_size(uint32_t w, uint32_t h, bool deferred) {
    const uint64_t key = ((uint64_t)w << 32) | (uint64_t)h;
    for (int i = 0; i < kSizeSlots; ++i) {
        SizeSlot& s = g_sizes[i];
        uint64_t k = s.key.load(std::memory_order_acquire);
        if (k == 0) {
            uint64_t zero = 0;
            k = s.key.compare_exchange_strong(zero, key, std::memory_order_acq_rel) ? key : zero;
        }
        if (k != key) continue;
        s.n.fetch_add(1, std::memory_order_relaxed);
        if (deferred) s.nd.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_size_overflow.fetch_add(1, std::memory_order_relaxed);
}

// Does a bound target look like the scene at this content size? The content's
// aspect within one per cent, from half its width (inclusive: at Supersampling
// 0.5 the scene ITSELF is half the backbuffer and must be in; the cost is that
// at Supersampling 1.0 the half-resolution helpers are in too) to twice it
// (Supersampling above 1). Size alone cannot tell those two cases apart; the
// rtcensus: line's * marks show which one a machine is in.
bool scene_sized(uint32_t w, uint32_t h, uint32_t cw, uint32_t ch) {
    if (!w || !h || !cw || !ch) return false;
    if (w < cw / 2 || w > cw * 2 + 8) return false;
    const double a = (double)w / (double)h, c = (double)cw / (double)ch;
    return fabs(a - c) / c <= 0.01;
}

// ---- counters ----------------------------------------------------------------
std::atomic<uint64_t> g_binds{0}, g_matched{0}, g_cleared{0}, g_skipped{0};
std::atomic<uint64_t> g_keep{0}, g_deferred{0}, g_gated{0}, g_present_clears{0};
std::atomic<uint64_t> g_nverr{0};
std::atomic<int>      g_nverr_first{0};
std::atomic<int>      g_nverr_deferred{0};

thread_local bool t_in_present = false;

inline float bits_to_f(uint32_t b) { float f; memcpy(&f, &b, sizeof(f)); return f; }
inline uint32_t f_to_bits(float f) { uint32_t b; memcpy(&b, &f, sizeof(b)); return b; }

// BUILD 218: the two NVAPI rate calls are NOT safe to make from several
// threads at once, even on different contexts. Measured 2026-09-07
// (nvdeferred.cpp): eight threads on their own deferred contexts crashed
// inside the driver without a lock and made 16,000 calls with zero errors
// under this spinlock; the 217 run's 64 errors (-14, NVAPI_INVALID_POINTER)
// on the game's render threads were the mild face of the same race. The hold
// is one or two driver calls, a few microseconds; the contended count is on
// the 1 Hz line. This is a deliberate exception to rule 8 (no locks in a
// per-bind hook), taken because the alternative is the driver's own race.
std::atomic<int>      g_nvlock{0};
std::atomic<uint64_t> g_lock_contended{0};
thread_local bool     t_in_nv = false;   // a re-entry guard: a thread inside the lock never waits on itself
struct NvLock {
    NvLock() {
        t_in_nv = true;
        int expected = 0;
        if (g_nvlock.compare_exchange_strong(expected, 1, std::memory_order_acquire)) return;
        g_lock_contended.fetch_add(1, std::memory_order_relaxed);
        // wait on a plain load (the line stays shared), a short pure spin,
        // then yield to the OS so a preempted holder gets its core back
        unsigned spins = 0;
        for (;;) {
            while (g_nvlock.load(std::memory_order_relaxed) != 0) {
                if (spins < 256) { ++spins; YieldProcessor(); }
                else SwitchToThread();
            }
            expected = 0;
            if (g_nvlock.compare_exchange_weak(expected, 1, std::memory_order_acquire)) break;
        }
    }
    ~NvLock() {
        g_nvlock.store(0, std::memory_order_release);
        t_in_nv = false;
    }
    NvLock(const NvLock&) = delete;
    NvLock& operator=(const NvLock&) = delete;
};

// BUILD 218: the shading-rate IMAGE takes effect only after the first command
// buffer submitted with it bound. Measured 2026-09-07 (nvdeferred.cpp): on a
// fresh device every image bind was ignored, in every order, until one Flush
// followed a bind on the immediate context; after that one flush every image
// bind on the device acted, deferred contexts included. Whether Present's own
// flush after an executed deferred bind would also unlock the device was not
// measured, so the explicit bind-flush-clear below is the mechanism, not a
// convenience. Tried once: 0 = not yet, 1 = done, -1 = the bind failed.
std::atomic<int> g_unlocked{0};

// The rate table is fixed: index 0 full, 1 half (2x1 or 1x2 by cfg), 2 quarter,
// 3 one sixteenth, 4 culled; every other index full, so an unexpected texel
// can only cost performance, never a hole.
NvAPI_Status set_rate(ID3D11DeviceContext* ctx, ID3D11NvShadingRateResourceView* view, bool on) {
    if (on) {
        const NvAPI_Status st = NvAPI_D3D11_RSSetShadingRateResourceView(ctx, view);
        if (st != NVAPI_OK) return st;
    }
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC v[2] = {};
    const int horiz = g_horizontal.load(std::memory_order_relaxed);
    for (int i = 0; i < 2; ++i) {
        v[i].enableVariablePixelShadingRate = on;
        for (int k = 0; k < NV_MAX_PIXEL_SHADING_RATES; ++k)
            v[i].shadingRateTable[k] = NV_PIXEL_X1_PER_RASTER_PIXEL;
        v[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
        v[i].shadingRateTable[1] = horiz ? NV_PIXEL_X1_PER_2X1_RASTER_PIXELS
                                         : NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
        v[i].shadingRateTable[2] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
        v[i].shadingRateTable[3] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
        v[i].shadingRateTable[4] = NV_PIXEL_X0_CULL_RASTER_PIXELS;
    }
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC d{};
    d.version      = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    d.numViewports = 2;
    d.pViewports   = v;
    return NvAPI_D3D11_RSSetViewportsPixelShadingRates(ctx, &d);
}

// An NVAPI error on a hot path: count it, keep the first status, and after 64
// switch the feature off for the run so a broken path cannot cost frames.
void fail(NvAPI_Status st, bool deferred) {
    const uint64_t n = g_nverr.fetch_add(1, std::memory_order_relaxed) + 1;
    int zero = 0;
    g_nverr_first.compare_exchange_strong(zero, (int)st, std::memory_order_relaxed);
    if (deferred) g_nverr_deferred.store(1, std::memory_order_relaxed);
    if (n >= 64) {
        g_active.store(0, std::memory_order_relaxed);
        g_state.store(kDisabledErr, std::memory_order_relaxed);
    }
}

// ---- the mask build (init thread) ----------------------------------------------
uint8_t ring_index(float r, const Params& p) {
    if (r < p.inner) return 0;
    if (r < p.mid)   return 1;
    if (r < p.outer) return 2;
    return 3;
}

bool build_mask(ID3D11Device* dev, const Geometry& g, const Params& p, Mask& m) {
    const uint32_t tw = (g.w + NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
    const uint32_t th = (g.h + NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;
    if (!tw || !th) return false;

    // the content's field: a symmetric perspective, tanV vertical, tanH by aspect
    const float tanV = tanf(0.5f * g.wide);
    const float tanH = tanV * (float)g.w / (float)g.h;
    if (!(tanV > 0.01f) || !(tanH > 0.01f)) return false;

    // the union of the two eyes' frustums, tangent units, about the view axis
    float minL = g.tan[0][0] < g.tan[1][0] ? g.tan[0][0] : g.tan[1][0];
    float maxR = g.tan[0][1] > g.tan[1][1] ? g.tan[0][1] : g.tan[1][1];
    float maxU = g.tan[0][2] > g.tan[1][2] ? g.tan[0][2] : g.tan[1][2];
    float minD = g.tan[0][3] < g.tan[1][3] ? g.tan[0][3] : g.tan[1][3];
    if (!(minL < -0.05f) || !(maxR > 0.05f) || !(maxU > 0.05f) || !(minD < -0.05f)) return false;
    const float hx = (-minL > maxR) ? -minL : maxR;
    const float hy = (maxU > -minD) ? maxU : -minD;

    // the visible rectangle in content pixels, row 0 at the top
    auto px_x = [&](float t) { float x = (t / tanH + 1.0f) * 0.5f * (float)g.w; return x < 0 ? 0 : (x > (float)g.w ? (int)g.w : (int)x); };
    auto px_y = [&](float t) { float y = (1.0f - t / tanV) * 0.5f * (float)g.h; return y < 0 ? 0 : (y > (float)g.h ? (int)g.h : (int)y); };
    m.vis[0] = px_x(minL); m.vis[1] = px_x(maxR);
    m.vis[2] = px_y(maxU); m.vis[3] = px_y(minD);

    std::vector<uint8_t> data((size_t)tw * th);
    uint32_t count[5] = {};
    for (uint32_t j = 0; j < th; ++j) {
        const float py = ((float)j + 0.5f) * (float)NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;
        const float ty = (1.0f - py / (float)g.h * 2.0f) * tanV;
        for (uint32_t i = 0; i < tw; ++i) {
            const float px = ((float)i + 0.5f) * (float)NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
            const float tx = (px / (float)g.w * 2.0f - 1.0f) * tanH;
            const bool outside = tx < minL - p.margin || tx > maxR + p.margin ||
                                 ty > maxU + p.margin || ty < minD - p.margin;
            uint8_t idx;
            if (outside) {
                idx = p.cull ? 4 : 3;
            } else {
                const float rx = tx / hx, ry = ty / hy;
                idx = ring_index(sqrtf(rx * rx + ry * ry), p);
            }
            data[(size_t)j * tw + i] = idx;
            ++count[idx];
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = tw;
    td.Height           = th;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem     = data.data();
    sd.SysMemPitch = tw;
    ID3D11Texture2D* tex = nullptr;
    const HRESULT hr = dev->CreateTexture2D(&td, &sd, &tex);
    if (FAILED(hr) || !tex) {
        LOG_ERROR("ffr: the %ux%u tile texture failed (0x%08lX); no mask, nothing bound.", tw, th, (unsigned long)hr);
        return false;
    }
    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
    vd.version            = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    vd.Format             = td.Format;
    vd.ViewDimension      = NV_SRRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipSlice = 0;
    ID3D11NvShadingRateResourceView* view = nullptr;
    NvAPI_Status st;
    {
        // the one device-level NVAPI call made while the render threads are
        // live: under the same lock, since the offline harness measured only
        // context against context and never a create against a context call
        NvLock lock;
        st = NvAPI_D3D11_CreateShadingRateResourceView(dev, tex, &vd, &view);
    }
    if (st != NVAPI_OK || !view) {
        LOG_ERROR("ffr: the shading rate view failed (NVAPI status %d); no mask, nothing bound.", (int)st);
        tex->Release();
        return false;
    }
    m.tex = tex; m.view = view; m.tw = tw; m.th = th;
    memcpy(m.count, count, sizeof(count));
    m.hx = hx; m.hy = hy; m.geo = g; m.par = p;
    return true;
}

// `on` is the run-time switch and `horizontal` is read at bind time; neither
// is baked into the texture, so a hot reload that flips them must not rebuild
// a byte-identical mask (the slots are finite).
bool same_par(const Params& a, const Params& b) {
    Params x = a, y = b;
    x.on = y.on = 0;
    x.horizontal = y.horizontal = 0;
    return memcmp(&x, &y, sizeof(Params)) == 0;
}

// The eye geometry and the field, without the size: two masks for different
// sizes share these, and a change in them makes every mask stale.
bool same_tan(const Geometry& a, const Geometry& b) {
    return a.wide == b.wide && memcmp(a.tan, b.tan, sizeof(a.tan)) == 0;
}

void log_mask(const Mask& m, const Geometry& g) {
    const double total = (double)m.tw * (double)m.th;
    const double visw  = (double)(m.vis[1] - m.vis[0]), vish = (double)(m.vis[3] - m.vis[2]);
    LOG_INFO("ffr: mask %d (%ux%u tiles for %ux%u content at fov %.3f rad): tiles full %u (%.0f%%) half %u (%.0f%%) "
             "quarter %u (%.0f%%) sixteenth %u (%.0f%%) culled %u (%.0f%%) | visible x %d..%d y %d..%d px, "
             "%.0f%% of the image | hx %.3f hy %.3f (the union of both eyes, tangent) | rings %.2f/%.2f/%.2f "
             "%s margin %.2f cull %d | eye L tan L/R/U/D %.3f/%.3f/%.3f/%.3f, R %.3f/%.3f/%.3f/%.3f",
             g_nmasks, m.tw, m.th, g.w, g.h, g.wide,
             m.count[0], 100.0 * m.count[0] / total, m.count[1], 100.0 * m.count[1] / total,
             m.count[2], 100.0 * m.count[2] / total, m.count[3], 100.0 * m.count[3] / total,
             m.count[4], 100.0 * m.count[4] / total,
             m.vis[0], m.vis[1], m.vis[2], m.vis[3],
             100.0 * visw * vish / ((double)g.w * (double)g.h),
             m.hx, m.hy, g_params.inner, g_params.mid, g_params.outer,
             g_params.horizontal ? "2x1" : "1x2", g_params.margin, g_params.cull,
             g.tan[0][0], g.tan[0][1], g.tan[0][2], g.tan[0][3],
             g.tan[1][0], g.tan[1][1], g.tan[1][2], g.tan[1][3]);
}

// BUILD 217: one mask per scene-sized size in the census, for the current
// rings and the current eye geometry; then a fresh table of every current mask.
void maintain_masks(ID3D11Device* dev) {
    Geometry* gp = g_geo_cur.load(std::memory_order_acquire);
    if (!gp) return;
    const Geometry base = *gp;   // a copy; the writer moves to the other buffer next frame
    if (!base.valid || !base.w || !base.h || !(base.wide > 0.0f)) return;

    // Which scene-sized sizes have no mask for the current rings and eyes.
    // All or nothing: if the cap cannot take every one of them, none is built
    // and the previous, consistent table stands, rather than one scene-sized
    // target keeping a mask while another falls to full rate by slot order.
    uint32_t need_w[kSizeSlots], need_h[kSizeSlots];
    int needed = 0;
    for (int i = 0; i < kSizeSlots; ++i) {
        const uint64_t k = g_sizes[i].key.load(std::memory_order_acquire);
        if (!k) continue;
        const uint32_t w = (uint32_t)(k >> 32), h = (uint32_t)(k & 0xFFFFFFFFu);
        if (!scene_sized(w, h, base.w, base.h)) continue;
        bool have = false;
        for (int j = 0; j < g_nmasks; ++j) {
            const Mask& x = g_masks[j];
            if (x.geo.w == w && x.geo.h == h && same_par(x.par, g_params) && same_tan(x.geo, base)) { have = true; break; }
        }
        if (have) continue;
        need_w[needed] = w;
        need_h[needed] = h;
        ++needed;
    }
    if (!needed) return;
    if (g_nmasks + needed > kMaxMasks) { ++g_rebuild_refused; return; }

    bool changed = false;
    for (int i = 0; i < needed; ++i) {
        const uint32_t w = need_w[i], h = need_h[i];
        Geometry g = base;
        g.w = w;
        g.h = h;
        Mask m;
        if (!build_mask(dev, g, g_params, m)) continue;
        g_masks[g_nmasks++] = m;
        log_mask(m, g);
        changed = true;
    }
    if (!changed) return;

    MaskTable& t = g_tables[g_table_next];
    g_table_next = (g_table_next + 1) % kTables;
    t.n = 0;
    for (int j = 0; j < g_nmasks; ++j) {
        const Mask& x = g_masks[j];
        if (same_par(x.par, g_params) && same_tan(x.geo, base)) t.m[t.n++] = &x;
    }
    g_table.store(&t, std::memory_order_release);
}

// ---- init (init thread, once a device exists) ---------------------------------
void init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    NvAPI_Status s = NvAPI_Initialize();
    if (s != NVAPI_OK) {
        g_first_status = (int)s;
        g_state.store(kNoNvapi, std::memory_order_relaxed);
        LOG_INFO("ffr: NVAPI is not available (NvAPI_Initialize status %d): no NVIDIA driver on this machine. "
                 "Foveated rendering is off, nothing is hooked, the game runs as build 215.", (int)s);
        return;
    }
    g_nvapi_up.store(1, std::memory_order_relaxed);
    NvAPI_ShortString ver = {};
    NvAPI_GetInterfaceVersionString(ver);

    // NVAPI: this must precede any deferred-context call, and this engine
    // records its scene on deferred contexts.
    g_register_status = (int)NvAPI_D3D_RegisterDevice(dev);

    NV_D3D1x_GRAPHICS_CAPS caps;
    memset(&caps, 0, sizeof(caps));
    s = NvAPI_D3D1x_GetGraphicsCapabilities(dev, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
    if (s != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
        g_first_status = (int)s;
        g_state.store(kNoVrs, std::memory_order_relaxed);
        LOG_INFO("ffr: NVAPI %s is up but variable rate shading is not supported on this GPU (caps status %d, "
                 "supported %u): RTX or GTX 16xx needed. Foveated rendering is off, nothing is hooked.",
                 ver, (int)s, (unsigned)caps.bVariablePixelRateShadingSupported);
        return;
    }
    g_sm_major = caps.majorSMVersion;
    g_sm_minor = caps.minorSMVersion;

    if (g_requested.load(std::memory_order_relaxed) <= 0) {
        g_state.store(kNotRequested, std::memory_order_relaxed);
        LOG_INFO("ffr: NVAPI %s, VRS supported (SM %u.%u), but cfg ffr was 0 when the device appeared: the "
                 "render-target hooks are NOT installed this run (d3d11.dll untouched). A later ffr = 1 needs a restart.",
                 ver, g_sm_major, g_sm_minor);
        return;
    }

    const int n = drawhook::install_rt(dev, ctx, &on_rt_bind);
    if (n <= 0) {
        g_state.store(kHooksFailed, std::memory_order_relaxed);
        LOG_ERROR("ffr: the render-target detours were refused or incomplete (%d; see the draw: lines above): a partial "
                  "set would miss binds and confound the verdict, so foveated rendering is off and whatever was patched "
                  "passes through (rule 7).", n);
        return;
    }
    g_hooked.store(1, std::memory_order_relaxed);
    g_active.store(1, std::memory_order_relaxed);
    g_state.store(kActive, std::memory_order_relaxed);
    LOG_INFO("ffr: ACTIVE. NVAPI %s, variable rate shading supported (SM %u.%u), device registered (status %d), "
             "%d render-target function(s) detoured in d3d11.dll. The mask follows on the next tick once the "
             "canvases exist; cfg ffr = %d now, F7 toggles it; the rate applies only while the wide field is in "
             "effect and only to scene-sized targets (the rtcensus: line names them).",
             ver, g_sm_major, g_sm_minor, g_register_status, n, g_on.load(std::memory_order_relaxed));
}

}  // namespace

// ---- public ------------------------------------------------------------------------

void set_params(const Params& in) {
    Params p = in;
    auto clampf = [](float v, float lo, float hi, float dflt) {
        if (!(v == v) || v < lo || v > hi) return dflt;   // NaN or out of range: the default
        return v;
    };
    p.inner  = clampf(p.inner,  0.05f, 3.0f, 0.6f);
    p.mid    = clampf(p.mid,    0.05f, 3.0f, 0.8f);
    p.outer  = clampf(p.outer,  0.05f, 3.0f, 1.0f);
    if (p.mid   < p.inner) p.mid   = p.inner;
    if (p.outer < p.mid)   p.outer = p.mid;
    p.margin = clampf(p.margin, 0.0f, 1.0f, 0.05f);
    p.horizontal = p.horizontal ? 1 : 0;
    p.cull       = p.cull ? 1 : 0;
    p.on         = p.on ? 1 : 0;
    g_params = p;
    g_horizontal.store(p.horizontal, std::memory_order_relaxed);
    g_on.store(p.on, std::memory_order_relaxed);
    int unset = -1;
    g_requested.compare_exchange_strong(unset, p.on, std::memory_order_relaxed);
}

void toggle() {
    if (!g_active.load(std::memory_order_relaxed)) return;
    g_on.fetch_xor(1, std::memory_order_relaxed);
    g_toggles.fetch_add(1, std::memory_order_relaxed);
}

void set_suspended(bool on) { g_suspended.store(on ? 1 : 0, std::memory_order_relaxed); }

void present_tick(ID3D11DeviceContext* ctx, uint32_t content_w, uint32_t content_h,
                  const float eye_tan[2][4], bool eyes_valid, float wide_fov) {
    Geometry& g = g_geo[g_geo_write];
    g.w = content_w; g.h = content_h; g.wide = wide_fov; g.valid = eyes_valid ? 1 : 0;
    memcpy(g.tan, eye_tan, sizeof(g.tan));
    g_geo_cur.store(&g, std::memory_order_release);
    g_geo_write ^= 1;
    g_wide_bits.store(f_to_bits(wide_fov), std::memory_order_relaxed);

    // our own draws, and the desktop mirror, at full rate: clear the immediate
    // context before any of them. The game's next matched bind sets it again.
    // Gated on the hooks being in, not on g_active, so a self-disable after
    // errors still leaves the immediate context clean every frame.
    if (g_hooked.load(std::memory_order_relaxed) && ctx) {
        // BUILD 218: the one-time unlock, the first time a mask exists: bind
        // it, flush, clear. On the present thread, the immediate context ours
        // to use here. Tried ONCE: a failure is recorded as -1 and counted
        // once, never retried, and never attempted after a self-disable.
        if (g_unlocked.load(std::memory_order_relaxed) == 0 && g_active.load(std::memory_order_relaxed)) {
            const MaskTable* t = g_table.load(std::memory_order_acquire);
            if (t && t->n > 0) {
                NvAPI_Status st;
                {
                    NvLock lock;
                    st = set_rate(ctx, t->m[0]->view, true);
                }
                if (st == NVAPI_OK) {
                    ctx->Flush();
                    g_unlocked.store(1, std::memory_order_relaxed);
                } else {
                    g_unlocked.store(-1, std::memory_order_relaxed);
                    fail(st, false);
                }
            }
        }
        NvAPI_Status st;
        {
            NvLock lock;
            st = set_rate(ctx, nullptr, false);
        }
        if (st != NVAPI_OK) fail(st, false);
        else g_present_clears.fetch_add(1, std::memory_order_relaxed);
    }
}

PresentGuard::PresentGuard()  { t_in_present = true; }
PresentGuard::~PresentGuard() { t_in_present = false; }

// The bind hook. Every context, every thread, after the real bind has run.
// Rule 8: counters, one resource query, one or two driver calls. No logging.
void on_rt_bind(ID3D11DeviceContext* ctx, uint32_t n, ID3D11RenderTargetView* const* rtvs) {
    if (!g_active.load(std::memory_order_relaxed) || !ctx) return;
    if (t_in_nv) return;   // a driver that re-entered the bind from inside a rate call would spin on itself: skip instead
    perf::Scope sc(perf::kFfr);
    g_binds.fetch_add(1, std::memory_order_relaxed);
    if (t_in_present) { g_skipped.fetch_add(1, std::memory_order_relaxed); return; }
    if (n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) { g_keep.fetch_add(1, std::memory_order_relaxed); return; }

    const bool deferred = ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
    if (deferred) g_deferred.fetch_add(1, std::memory_order_relaxed);

    // Off is not "do nothing": it is an explicit full rate on every bind, so
    // the two halves of the F7 A/B cost the same and no deferred context can
    // keep a stale mask through a command list that restores its state.
    const MaskTable* t = (g_on.load(std::memory_order_relaxed) && !g_suspended.load(std::memory_order_relaxed))
                             ? g_table.load(std::memory_order_acquire) : nullptr;   // BUILD 247: suspended = explicit full rate
    const Mask* m = nullptr;
    if (n && rtvs && rtvs[0]) {
        ID3D11Resource* res = nullptr;
        rtvs[0]->GetResource(&res);
        if (res) {
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            res->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC td;
                static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
                record_size(td.Width, td.Height, deferred);   // BUILD 217: the census, every bind
                if (t) {
                    // only while the wide field is what this frame renders (not ADS, menus, loading)
                    const float f    = headpose::read_fov(0.0f);
                    const float wide = bits_to_f(g_wide_bits.load(std::memory_order_relaxed));
                    if (wide > 0.0f && f >= 0.97f * wide) {
                        for (int i = 0; i < t->n; ++i) {
                            const Mask* c = t->m[i];
                            if (c->geo.w == td.Width && c->geo.h == td.Height) { m = c; break; }
                        }
                    } else {
                        g_gated.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            res->Release();
        }
    }
    NvAPI_Status st;
    {
        NvLock lock;   // BUILD 218: the driver's rate calls are not thread-safe across contexts
        st = set_rate(ctx, m ? m->view : nullptr, m != nullptr);
    }
    if (st != NVAPI_OK) fail(st, deferred);
    else if (m) g_matched.fetch_add(1, std::memory_order_relaxed);
    else g_cleared.fetch_add(1, std::memory_order_relaxed);
}

void tick_1hz() {
    ID3D11Device*        dev = d3d11::state().device;
    ID3D11DeviceContext* ctx = d3d11::state().context;
    if (g_state.load(std::memory_order_relaxed) == kNotInit) {
        if (!dev || !ctx) return;                                   // no device yet: nothing to say
        if (g_requested.load(std::memory_order_relaxed) < 0) return; // the cfg has not been applied yet
        init(dev, ctx);
    }
    if (g_active.load(std::memory_order_relaxed) && dev) maintain_masks(dev);

    // BUILD 217: the census line, the top eight sizes bound this second with
    // their counts and how many of those binds were on deferred contexts. The
    // scene's size is whatever the game is set to (Supersampling), and this is
    // how it gets on record instead of being assumed.
    {
        struct Row { uint32_t w, h; uint64_t n, nd; };
        Row rows[kSizeSlots];
        int nrows = 0;
        for (int i = 0; i < kSizeSlots; ++i) {
            const uint64_t k = g_sizes[i].key.load(std::memory_order_acquire);
            if (!k) continue;
            const uint64_t cnt = g_sizes[i].n.exchange(0, std::memory_order_relaxed);
            const uint64_t dc  = g_sizes[i].nd.exchange(0, std::memory_order_relaxed);
            if (!cnt) continue;
            rows[nrows++] = Row{(uint32_t)(k >> 32), (uint32_t)(k & 0xFFFFFFFFu), cnt, dc};
        }
        for (int i = 1; i < nrows; ++i) {            // insertion sort, descending by count
            Row r = rows[i];
            int j = i - 1;
            while (j >= 0 && rows[j].n < r.n) { rows[j + 1] = rows[j]; --j; }
            rows[j + 1] = r;
        }
        if (nrows) {
            int used = 0;
            for (int i = 0; i < kSizeSlots; ++i)
                if (g_sizes[i].key.load(std::memory_order_relaxed)) ++used;
            char line[512];
            int  p = 0;
            const MaskTable* t = g_table.load(std::memory_order_acquire);
            for (int i = 0; i < nrows && i < 8; ++i) {
                bool masked = false;
                if (t) for (int j = 0; j < t->n; ++j) if (t->m[j]->geo.w == rows[i].w && t->m[j]->geo.h == rows[i].h) masked = true;
                const int r = snprintf(line + p, sizeof(line) - (size_t)p, "%s%ux%u=%llu(d%llu)%s", i ? " " : "",
                                       rows[i].w, rows[i].h, (unsigned long long)rows[i].n, (unsigned long long)rows[i].nd,
                                       masked ? "*" : "");
                if (r < 0 || r >= (int)sizeof(line) - p) break;   // truncated: stop, never step past the buffer
                p += r;
            }
            LOG_INFO("rtcensus: slots=%d/%d distinct=%d overflow_total=%llu | %s (build 217: render-target sizes bound "
                     "this second, the count and in brackets the count on deferred contexts; * = a mask exists for that "
                     "size; slots = census slots claimed this run, overflow_total = binds that found no slot, for the run)",
                     used, kSizeSlots, nrows, (unsigned long long)g_size_overflow.load(std::memory_order_relaxed), line);
        }
    }

    const uint64_t binds    = g_binds.exchange(0, std::memory_order_relaxed);
    const uint64_t matched  = g_matched.exchange(0, std::memory_order_relaxed);
    const uint64_t cleared  = g_cleared.exchange(0, std::memory_order_relaxed);
    const uint64_t skipped  = g_skipped.exchange(0, std::memory_order_relaxed);
    const uint64_t keep     = g_keep.exchange(0, std::memory_order_relaxed);
    const uint64_t deferred = g_deferred.exchange(0, std::memory_order_relaxed);
    const uint64_t gated    = g_gated.exchange(0, std::memory_order_relaxed);
    const uint64_t pclears  = g_present_clears.exchange(0, std::memory_order_relaxed);
    const float f    = headpose::read_fov(0.0f);
    const float wide = bits_to_f(g_wide_bits.load(std::memory_order_relaxed));
    const int gate = (wide > 0.0f && f >= 0.97f * wide) ? 1 : 0;
    const MaskTable* tbl = g_table.load(std::memory_order_acquire);
    const uint64_t contended = g_lock_contended.exchange(0, std::memory_order_relaxed);
    LOG_INFO("ffr: %s on=%d gate=%d fov=%.3f | binds=%llu matched=%llu cleared=%llu skipped=%llu keep=%llu "
             "deferred=%llu gated=%llu pclears=%llu | nverr=%llu first=%d on_deferred=%d | table=%d masks=%d "
             "refused=%u | unlocked=%d contended=%llu | toggles=%d (build 218: state; on = the run-time switch; gate = "
             "the wide field is in effect this second; binds = render-target binds seen, matched = bound with a mask, "
             "cleared = rate cleared, skipped = our own Present's, keep = KEEP_RENDER_TARGETS, deferred = on deferred "
             "contexts; pclears = the per-Present clear; nverr = NVAPI errors, first = the first status; table = "
             "sizes with a current mask, masks = every mask built this run; unlocked = the one-time flush after the "
             "first mask bind has happened; contended = NVAPI lock acquisitions that had to wait, this second)",
             state_name(g_state.load(std::memory_order_relaxed)), g_on.load(std::memory_order_relaxed), gate, f,
             (unsigned long long)binds, (unsigned long long)matched, (unsigned long long)cleared,
             (unsigned long long)skipped, (unsigned long long)keep, (unsigned long long)deferred,
             (unsigned long long)gated, (unsigned long long)pclears,
             (unsigned long long)g_nverr.load(std::memory_order_relaxed),
             g_nverr_first.load(std::memory_order_relaxed), g_nverr_deferred.load(std::memory_order_relaxed),
             tbl ? tbl->n : 0, g_nmasks, g_rebuild_refused,
             g_unlocked.load(std::memory_order_relaxed), (unsigned long long)contended,
             g_toggles.load(std::memory_order_relaxed));
}

int gpu_util(int* fb_out) {
    if (fb_out) *fb_out = -1;
    if (!g_nvapi_up.load(std::memory_order_relaxed)) return -1;
    static NvPhysicalGpuHandle s_gpus[NVAPI_MAX_PHYSICAL_GPUS] = {};
    static NvU32 s_count = 0;
    static bool  s_tried = false;
    if (!s_tried) {
        s_tried = true;
        if (NvAPI_EnumPhysicalGPUs(s_gpus, &s_count) != NVAPI_OK) s_count = 0;
    }
    if (!s_count) return -1;
    NV_GPU_DYNAMIC_PSTATES_INFO_EX info;
    memset(&info, 0, sizeof(info));
    info.version = NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER;
    if (NvAPI_GPU_GetDynamicPstatesInfoEx(s_gpus[0], &info) != NVAPI_OK) return -1;
    if (fb_out && info.utilization[1].bIsPresent) *fb_out = (int)info.utilization[1].percentage;
    return info.utilization[0].bIsPresent ? (int)info.utilization[0].percentage : -1;
}

}  // namespace ffr
}  // namespace grwxr
