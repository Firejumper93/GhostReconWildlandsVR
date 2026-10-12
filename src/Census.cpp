// Census.cpp - BUILD 242, see Census.h. Observation only.
//
// Layout: the pins (the only TU25-specific numbers), the guarded memory reads,
// the preallocated tables, the recorder (hot path, rule 8), the installer, the
// state machine (init thread), the dump thread (file I/O, off every hot path).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Census.h"
#include "D3D11Hook.h"
#include "GameBuild.h"
#include "Log.h"

// ---------------------------------------------------------------------------
// The symbols the assembly stubs (ProbeStub.asm, grwxr_census_*) bind to.
// ---------------------------------------------------------------------------
struct CensusSaved {   // mirrors the block the stub writes at rsp+40h
    uint64_t rcx, rdx, r8, r9;
    float    xmm0[4], xmm1[4], xmm2[4], xmm3[4];
};

extern "C" {
volatile uint32_t grwxr_census_on = 0;          // 1 only while the window is open
volatile uint32_t grwxr_cam2_on = 0;            // BUILD 243: 1 while the second-camera experiment wants entry 0
uint64_t          grwxr_census_orig[4] = {0, 0, 0, 0};
void grwxr_census_entry_0();
void grwxr_census_entry_1();
void grwxr_census_entry_2();
void grwxr_census_entry_3();
void grwxr_census_record(uint32_t id, const CensusSaved* a, uint64_t ret);
}

namespace grwxr {
namespace census {

namespace {

// ---------------------------------------------------------------------------
// THE PINS. Every number here was read from the TU25 GRW.exe ON DISK
// (sha256 1284e4d5aad5f1ca5d2832b6c46707b1d043205e929b5810ffe1caf61cbdf751),
// read-only, 2026-10-05, by RVA -> file offset through its section table. The
// four thunks sit in .trace, 16-byte aligned, each `E9 rel32` plus 11 int3.
// RVAs never carry to another build: the table is keyed by PE timestamp and
// SizeOfImage and any other binary installs nothing.
// ---------------------------------------------------------------------------
enum : int { kGraph = 0, kCam = 1, kBuild = 2, kExec = 3, kHooks = 4 };

struct Pin {
    const char* name;
    uint32_t    thunk;        // RVA of the E9 slot
    uint32_t    body;         // RVA of the real body the slot jumps to
    uint8_t     thunk_bytes[16];
    uint8_t     body_bytes[16];
};

struct CensusBuild {
    uint32_t pe_timestamp;
    uint32_t pe_size_of_image;
    Pin      pin[kHooks];
    // Data globals (RVAs), read with guards, never written.
    uint32_t registry_global;    // holds the view registry pointer (view lists at +0x61/+0x6B and +0x6D/+0x77)
    uint32_t renderer_holder;    // [holder] + 0x10 = the Renderer singleton ("this" of the per-camera entry)
    uint32_t ctxpool_global;     // int: deferred wrapper count (context pool is this + 1 wrappers)
};

constexpr CensusBuild kCensusTU25 = {
    0x6A99768A, 0x185BA000,   // 2026-09-15 Title Update 25, the kTU25 row of GameBuild.cpp
    {
        {"graph-frame builder", 0x013626A0, 0x0D642F60,
         {0xE9, 0xBB, 0x08, 0x2E, 0x0C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC},
         {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54}},
        {"per-camera entry", 0x0154B900, 0x0E11ACA0,
         {0xE9, 0x9B, 0xF3, 0xBC, 0x0C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC},
         {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B}},
        {"BuildCmdList body", 0x0154EB50, 0x0E1212B0,
         {0xE9, 0x5B, 0x27, 0xBD, 0x0C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC},
         {0x40, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0xCE, 0x48, 0x8B, 0x49, 0x10, 0x48, 0x8B, 0x01}},
        {"ExecuteCmdList body", 0x0153F4A0, 0x0E0FD6A0,
         {0xE9, 0xFB, 0xE1, 0xBB, 0x0C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC},
         {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}},
    },
    0x04D5B0D8, 0x04D75898, 0x04D89504,
};

const CensusBuild* const kCensusTable[] = {&kCensusTU25};

// Struct offsets (static read, REPORT.md section 2/3, decompile of 0x0D642F60,
// 0x0D803CD0 and 0x0E11ACA0). None is written.
constexpr uint32_t kRegListA_Count = 0x77, kRegListA_Ptr = 0x6D;   // registry, first list
constexpr uint32_t kRegListB_Count = 0x6B, kRegListB_Ptr = 0x61;   // registry, second list
constexpr uint32_t kViewCamArr = 0x18, kViewCamCount = 0x22;       // view: camera array ptr, ushort count
constexpr uint32_t kCamBlock = 0x290, kCamSurfLink = 0x2A8, kCamSlotIdx = 0x2F4;
constexpr uint32_t kRendSlotArr = 0x2470, kRendSlotCount = 0x247A; // renderer: slot pointer array, ushort count
constexpr uint32_t kSlotCounter = 0xFCC;                           // slot: task counter
constexpr uint32_t kSurfType = 0x1C, kSurfD18 = 0xD18, kD18_358 = 0x358;

// ---------------------------------------------------------------------------
// constants and state
// ---------------------------------------------------------------------------
constexpr int      kFrames  = 240;           // the window, in presents
constexpr uint64_t kArmMs   = 50000;         // hooks go in and the window opens this long after the first stereo layer
constexpr uint64_t kGiveUpMs = 150000;       // a window that never fills is closed anyway
constexpr uint32_t kCamCap  = 4096;
constexpr uint32_t kGraphCap = 1024;
constexpr uint32_t kTaskCap = 24000;
constexpr int      kThrCap  = 64;
constexpr int      kViewsPerList = 6;
constexpr int      kCamsPerView  = 4;
constexpr int      kSlotsRead = 8;

std::atomic<int>       g_state{0};   // 0 idle, 2 capturing, 3 stopped, 4 dumping, 5 done, 6 failed
std::atomic<bool>      g_enabled{true};
std::atomic<uint64_t>  g_first_stereo_ms{0};
std::atomic<uint64_t>  g_start_frame{0};
uint64_t               g_arm_ms = 0;
uint64_t               g_tsc0 = 0, g_qpc0 = 0;
uint64_t               g_pts[kFrames + 1] = {};   // TSC at the entry of Present #(start + i)
const CensusBuild*     g_pins = nullptr;
uint64_t               g_base = 0, g_size = 0;
const char*            g_name = "";

struct Hooked {
    uint8_t*    slot = nullptr;
    uint64_t    old64 = 0, new64 = 0;
    bool        on = false;
    bool        pad_dirty = false;
    const char* status = "not attempted";
    uint8_t     found_thunk[16] = {};
    uint8_t     found_body[16] = {};
};
Hooked g_h[kHooks];

// BUILD 243: the second-camera experiment's share of hook 0 (the graph-frame
// builder). The callback runs on the builder's thread at every builder entry, before
// the body. g_keep_graph stops the census putting hook 0 back after its window.
std::atomic<void (*)()> g_graph_cb{nullptr};
std::atomic<bool>       g_keep_graph{false};

// BUILD 245: the second-camera diagnostics (Cam2.cpp) want to know which ProjShadow passes the
// BuildCmdList hook (hook 2) saw. The descriptors are an array of 0x38-byte records at RVA 0x45C79F0,
// ProjShadow0..7 (REPORT.md section 5 and the AV stacks of build 244: 0x45C79F0, 0x45C7A28, 0x45C7A60).
// Hook 2 is only in while the census window is open, so the mask is empty and `build_events_live()`
// false at every other time. The flag is off unless the experiment turns it on.
constexpr uint32_t kProjShadowRva = 0x045C79F0, kProjShadowStride = 0x38, kProjShadowCount = 8;
std::atomic<bool>     g_ps_watch{false};
std::atomic<uint32_t> g_ps_mask{0};

// ---------------------------------------------------------------------------
// guarded reads of engine memory
// ---------------------------------------------------------------------------
thread_local uint64_t t_lo = 0, t_hi = 0;   // the last region VirtualQuery blessed on this thread (one recorder call only)

bool readable(uint64_t a, size_t n) {
    if (a < 0x10000 || a + n < a || a + n > 0x00007FFFFFFE0000ull) return false;
    if (a >= t_lo && a + n <= t_hi) return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    const uint64_t lo = (uint64_t)(uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
    if (a + n > hi) return false;
    t_lo = lo;
    t_hi = hi;
    return true;
}

// VirtualQuery says it is mapped and readable; the SEH guard is the net under it.
bool rd(uint64_t a, void* out, size_t n) {
    if (!readable(a, n)) return false;
    __try {
        memcpy(out, (const void*)(uintptr_t)a, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_lo = t_hi = 0;
        return false;
    }
}
template <class T>
bool rdv(uint64_t a, T* out) { return rd(a, out, sizeof(T)); }

// ---------------------------------------------------------------------------
// the tables (all static, zero-initialised; the hot path never allocates)
// ---------------------------------------------------------------------------
struct EvCam {              // one call of the per-camera entry
    std::atomic<uint64_t> ts;   // rdtsc, written LAST, 0 = not yet published
    uint32_t tid;
    uint16_t bucket, pad;
    uint32_t caller;            // RVA of the return address
    uint32_t pad2;
    uint64_t w[14];
    // w0 renderer(rcx) w1 camera(rdx) w2 r8 w3 r9
    // w4 = slot index [cam+0x2F4] | okflags<<32 | renderer slot count<<48
    // w5 = [cam+0x2A8] (P, the surface handle)  w6 = [P]  w7 = [S+0x1C] | (int32)[P+0xC]<<32
    // w8 = [S+0xD18]  w9 = [[S+0xD18]+0x358]    w10,w11 = the 16 bytes at cam+0x290
    // w12 = slot pointer renderer[+0x2470][idx]  w13 = [slot+0xFCC]
};
struct EvTask {             // one BuildCmdList or ExecuteCmdList body call
    std::atomic<uint64_t> ts;
    uint32_t tid;
    uint16_t bucket, type;
    uint32_t caller;
    uint32_t okmask;
    uint64_t w[5];              // rcx, then [rcx+0], [rcx+8], [rcx+0x10], [rcx+0x18] (raw; meaning not established)
};
struct ViewRec {
    uint64_t view, vtbl, camarr;
    uint16_t ncam;              // [view+0x22]
    uint16_t ok;                // 1 view ptr, 2 vtbl, 4 ncam, 8 camarr
    uint32_t pad;
    uint64_t cam[kCamsPerView];
};
struct GraphRec {           // one call of the graph-frame builder
    std::atomic<uint64_t> ts;
    uint32_t tid;
    uint16_t bucket, ok;        // ok: 1 registry read, 2 pool read
    uint32_t caller;
    uint32_t pool;              // int at the context-pool global
    uint64_t rcx, rdx, reg;
    uint64_t arr[2];
    uint16_t n[2];              // view counts of list A / list B as the registry holds them
    uint16_t okl;               // 1 list A count, 2 list A ptr, 4 list B count, 8 list B ptr
    uint16_t pad;
    ViewRec  v[2][kViewsPerList];
};
struct SlotRow {            // read at the entry of Present #(start+b)
    uint64_t renderer;          // [[holder]+0x10]
    uint64_t renderer_call;     // rcx of the last per-camera call (cross-check)
    uint16_t count;             // [renderer+0x247A]
    uint16_t ok;                // 1 renderer via holder, 2 array ptr, 4 count
    uint64_t arr;
    uint64_t ptr[kSlotsRead];
    uint32_t ctr[kSlotsRead];
    uint8_t  okc[kSlotsRead];   // 1 ptr read, 2 counter read
};

EvCam     g_cam[kCamCap];
GraphRec  g_graph[kGraphCap];
EvTask    g_task[2][kTaskCap];      // [0] build, [1] exec
SlotRow   g_slot[kFrames + 1];
std::atomic<uint32_t> g_cam_head{0}, g_cam_drop{0}, g_graph_head{0}, g_graph_drop{0};
std::atomic<uint32_t> g_task_head[2] = {}, g_task_drop[2] = {};
std::atomic<uint32_t> g_cnt[kHooks][kFrames] = {};     // exact per-row call counts, never dropped
std::atomic<uint64_t> g_last_renderer{0};
std::atomic<uint32_t> g_thr_full{0}, g_rec_calls{0};

struct ThrEnt {
    std::atomic<uint32_t> tid;
    std::atomic<uint32_t> n[kHooks][kFrames];
};
ThrEnt g_thr[kThrCap];
thread_local int t_slot = -1;

ThrEnt* thr(uint32_t tid) {
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

inline int bucket() {
    const uint64_t f = d3d11::frame_count();
    const uint64_t s = g_start_frame.load(std::memory_order_relaxed);
    if (f < s) return -1;
    const uint64_t b = f - s;
    return b < (uint64_t)kFrames ? (int)b : -1;
}

inline uint32_t caller_rva(uint64_t ret) {
    return (ret >= g_base && ret < g_base + g_size) ? (uint32_t)(ret - g_base) : (uint32_t)ret;
}

// ---------------------------------------------------------------------------
// the recorder
// ---------------------------------------------------------------------------
void rec_cam(const CensusSaved* a, uint64_t ret, int b, uint32_t tid, uint64_t ts) {
    const uint32_t i = g_cam_head.fetch_add(1, std::memory_order_relaxed);
    if (i >= kCamCap) { g_cam_drop.fetch_add(1, std::memory_order_relaxed); return; }
    EvCam& e = g_cam[i];
    const uint64_t renderer = a->rcx, cam = a->rdx;
    g_last_renderer.store(renderer, std::memory_order_relaxed);

    uint32_t idx = 0xFFFFFFFFu;
    uint16_t flags = 0, cnt = 0;
    uint64_t P = 0, raw = 0, S = 0, d18 = 0, d358 = 0, slotp = 0, blk[2] = {0, 0};
    int32_t  m = 0;
    uint32_t stype = 0xFFFFFFFFu, slotc = 0xFFFFFFFFu;

    if (rdv(cam + kCamSlotIdx, &idx)) flags |= 0x001;
    if (rdv(cam + kCamSurfLink, &P)) {
        flags |= 0x002;
        if (rdv(P, &raw) && rdv(P + 0xC, &m)) {
            flags |= 0x004;
            S = (m < 0) ? raw : 0;   // the engine's own validity mask: ((int64)(int32)[P+0xC] << 32) >> 63 & [P]
            if (S) {
                if (rdv(S + kSurfType, &stype)) flags |= 0x008;
                if (rdv(S + kSurfD18, &d18)) {
                    flags |= 0x010;
                    if (d18 && rdv(d18 + kD18_358, &d358)) flags |= 0x020;
                }
            }
        }
    }
    if (rd(cam + kCamBlock, blk, 16)) flags |= 0x040;
    uint64_t arr = 0;
    if (rdv(renderer + kRendSlotCount, &cnt)) {
        flags |= 0x080;
        if (idx < cnt && rdv(renderer + kRendSlotArr, &arr) && rdv(arr + (uint64_t)idx * 8, &slotp)) {
            flags |= 0x100;
            if (slotp && rdv(slotp + kSlotCounter, &slotc)) flags |= 0x200;
        }
    }

    e.tid = tid;
    e.bucket = (uint16_t)b;
    e.pad = 0;
    e.caller = caller_rva(ret);
    e.pad2 = 0;
    e.w[0] = renderer; e.w[1] = cam; e.w[2] = a->r8; e.w[3] = a->r9;
    e.w[4] = (uint64_t)idx | ((uint64_t)flags << 32) | ((uint64_t)cnt << 48);
    e.w[5] = P; e.w[6] = raw;
    e.w[7] = (uint64_t)stype | ((uint64_t)(uint32_t)m << 32);
    e.w[8] = d18; e.w[9] = d358;
    e.w[10] = blk[0]; e.w[11] = blk[1];
    e.w[12] = slotp; e.w[13] = slotc;
    e.ts.store(ts, std::memory_order_release);
}

void rec_graph(const CensusSaved* a, uint64_t ret, int b, uint32_t tid, uint64_t ts) {
    const uint32_t i = g_graph_head.fetch_add(1, std::memory_order_relaxed);
    if (i >= kGraphCap) { g_graph_drop.fetch_add(1, std::memory_order_relaxed); return; }
    GraphRec& g = g_graph[i];
    g.tid = tid;
    g.bucket = (uint16_t)b;
    g.caller = caller_rva(ret);
    g.rcx = a->rcx; g.rdx = a->rdx;
    g.ok = 0; g.okl = 0; g.reg = 0; g.pool = 0xFFFFFFFFu;
    g.arr[0] = g.arr[1] = 0;
    g.n[0] = g.n[1] = 0;

    uint32_t pool = 0;
    if (rdv(g_base + g_pins->ctxpool_global, &pool)) { g.ok |= 2; g.pool = pool; }
    uint64_t reg = 0;
    if (rdv(g_base + g_pins->registry_global, &reg) && reg) {
        g.ok |= 1;
        g.reg = reg;
        const uint32_t cnt_off[2] = {kRegListA_Count, kRegListB_Count};
        const uint32_t ptr_off[2] = {kRegListA_Ptr, kRegListB_Ptr};
        for (int L = 0; L < 2; ++L) {
            uint16_t n = 0;
            uint64_t arr = 0;
            if (rdv(reg + cnt_off[L], &n)) g.okl |= (uint16_t)(1u << (2 * L));
            if (rdv(reg + ptr_off[L], &arr)) g.okl |= (uint16_t)(2u << (2 * L));
            g.n[L] = n;
            g.arr[L] = arr;
            for (int v = 0; v < kViewsPerList; ++v) {
                ViewRec& r = g.v[L][v];
                memset(&r, 0, sizeof(r));
                if (v >= n || !arr) continue;
                uint64_t view = 0;
                if (!rdv(arr + (uint64_t)v * 8, &view)) continue;
                r.view = view;
                r.ok |= 1;
                uint64_t vt = 0;
                if (rdv(view, &vt)) { r.vtbl = vt; r.ok |= 2; }
                uint16_t nc = 0;
                if (rdv(view + kViewCamCount, &nc)) { r.ncam = nc; r.ok |= 4; }
                uint64_t ca = 0;
                if (rdv(view + kViewCamArr, &ca)) {
                    r.camarr = ca;
                    r.ok |= 8;
                    for (int c = 0; c < kCamsPerView && c < nc; ++c) {
                        uint64_t cp = 0;
                        if (rdv(ca + (uint64_t)c * 8, &cp)) r.cam[c] = cp;
                    }
                }
            }
        }
    }
    g.ts.store(ts, std::memory_order_release);
}

void rec_task(int which, const CensusSaved* a, uint64_t ret, int b, uint32_t tid, uint64_t ts) {
    const uint32_t i = g_task_head[which].fetch_add(1, std::memory_order_relaxed);
    if (i >= kTaskCap) { g_task_drop[which].fetch_add(1, std::memory_order_relaxed); return; }
    EvTask& e = g_task[which][i];
    e.tid = tid;
    e.bucket = (uint16_t)b;
    e.type = (uint16_t)which;
    e.caller = caller_rva(ret);
    e.w[0] = a->rcx;
    uint64_t q[4] = {0, 0, 0, 0};
    e.okmask = rd(a->rcx, q, sizeof(q)) ? 1u : 0u;
    e.w[1] = q[0]; e.w[2] = q[1]; e.w[3] = q[2]; e.w[4] = q[3];
    e.ts.store(ts, std::memory_order_release);
    // BUILD 245: the second-camera diagnostics ask which ProjShadow pass descriptors went through
    // BuildCmdList. q[2] ([rcx+0x10]) is the pass descriptor's address. One relaxed load and, on a hit,
    // one relaxed fetch_or: nothing else, and nothing at all while the watch flag is off.
    if (which == 0 && g_ps_watch.load(std::memory_order_relaxed) && e.okmask != 0) {
        const uint64_t first = g_base + kProjShadowRva;
        if (q[2] >= first && q[2] < first + (uint64_t)kProjShadowCount * kProjShadowStride &&
            ((q[2] - first) % kProjShadowStride) == 0)
            g_ps_mask.fetch_or(1u << (uint32_t)((q[2] - first) / kProjShadowStride), std::memory_order_relaxed);
    }
}

void record_impl(uint32_t id, const CensusSaved* a, uint64_t ret) {
    // BUILD 243: the second-camera experiment, first, on the builder's own thread and
    // before the real body runs. It has its own gate (grwxr_cam2_on keeps the stub off the
    // fast path only for id 0) and never depends on the census window.
    if (id == (uint32_t)kGraph) {
        void (*const cb)() = g_graph_cb.load(std::memory_order_relaxed);
        if (cb) cb();
    }
    if (g_state.load(std::memory_order_relaxed) != 2 || id >= (uint32_t)kHooks) return;
    const int b = bucket();
    if (b < 0) return;
    t_lo = t_hi = 0;
    const uint32_t tid = __readgsdword(0x48);   // TEB.ClientId.UniqueThread
    const uint64_t ts = __rdtsc();
    g_rec_calls.fetch_add(1, std::memory_order_relaxed);
    g_cnt[id][b].fetch_add(1, std::memory_order_relaxed);
    if (ThrEnt* t = thr(tid)) t->n[id][b].fetch_add(1, std::memory_order_relaxed);
    switch (id) {
        case kGraph: rec_graph(a, ret, b, tid, ts); break;
        case kCam:   rec_cam(a, ret, b, tid, ts); break;
        default:     rec_task(id == kBuild ? 0 : 1, a, ret, b, tid, ts); break;
    }
}

// Present-boundary readout of the renderer's slot array (render thread, inside the window only).
void read_slots(SlotRow& r) {
    t_lo = t_hi = 0;
    memset(&r, 0, sizeof(r));
    r.renderer_call = g_last_renderer.load(std::memory_order_relaxed);
    uint64_t holder = 0, rend = 0;
    if (rdv(g_base + g_pins->renderer_holder, &holder) && holder && rdv(holder + 0x10, &rend) && rend) r.ok |= 1;
    else rend = r.renderer_call;
    r.renderer = rend;
    if (!rend) return;
    uint16_t cnt = 0;
    if (rdv(rend + kRendSlotCount, &cnt)) { r.ok |= 4; r.count = cnt; }
    uint64_t arr = 0;
    if (rdv(rend + kRendSlotArr, &arr) && arr) {
        r.ok |= 2;
        r.arr = arr;
        for (int i = 0; i < kSlotsRead && i < (int)cnt; ++i) {
            uint64_t p = 0;
            if (!rdv(arr + (uint64_t)i * 8, &p)) continue;
            r.ptr[i] = p;
            r.okc[i] |= 1;
            uint32_t c = 0;
            if (p && rdv(p + kSlotCounter, &c)) { r.ctr[i] = c; r.okc[i] |= 2; }
        }
    }
}

// ---------------------------------------------------------------------------
// the installer
// ---------------------------------------------------------------------------
bool install_one(int i) {
    Hooked& h = g_h[i];
    if (h.on) return true;   // BUILD 243: hook 0 may already be in, installed for the second-camera experiment
    const Pin& p = g_pins->pin[i];
    uint8_t* const slot = (uint8_t*)(g_base + p.thunk);
    uint8_t* const body = (uint8_t*)(g_base + p.body);

    if (!rd((uint64_t)(uintptr_t)slot, h.found_thunk, 16) || !rd((uint64_t)(uintptr_t)body, h.found_body, 16)) {
        h.status = "REFUSED: thunk slot or body not readable";
        return false;
    }
    if (((uintptr_t)slot & 15) != 0) { h.status = "REFUSED: thunk slot is not 16-byte aligned"; return false; }
    if (memcmp(h.found_thunk, p.thunk_bytes, 16) != 0) {
        h.status = "REFUSED: thunk slot bytes differ from the pinned 16 (E9 rel32 + 11 int3)";
        return false;
    }
    if (memcmp(h.found_body, p.body_bytes, 16) != 0) {
        h.status = "REFUSED: body prologue differs from the pinned 16 bytes";
        return false;
    }
    int32_t rel = 0;
    memcpy(&rel, slot + 1, sizeof(rel));
    if (slot + 5 + rel != body) {
        h.status = "REFUSED: the E9 does not resolve to the pinned body";
        return false;
    }

    // Final stub: jmp qword ptr [rip+0] ; <absolute stub address>. Its first 8 bytes
    // (FF 25 00 00 00 00 + two operand bytes) are what the one atomic store writes.
    void* const stub = (i == kGraph) ? (void*)&grwxr_census_entry_0 :
                       (i == kCam)   ? (void*)&grwxr_census_entry_1 :
                       (i == kBuild) ? (void*)&grwxr_census_entry_2 : (void*)&grwxr_census_entry_3;
    uint8_t patch[14] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const uint64_t dest = (uint64_t)(uintptr_t)stub;
    memcpy(patch + 6, &dest, sizeof(dest));
    uint64_t old64 = 0, new64 = 0;
    memcpy(&old64, h.found_thunk, 8);
    memcpy(&new64, patch, 8);

    grwxr_census_orig[i] = (uint64_t)(uintptr_t)body;   // before anything can reach the stub

    DWORD oldprot = 0;
    if (!VirtualProtect(slot, 16, PAGE_EXECUTE_READWRITE, &oldprot)) {
        h.status = "REFUSED: VirtualProtect failed";
        return false;
    }
    // 1. The tail of the operand goes into the dead padding. Nothing executes it
    //    while the E9 is still in place.
    memcpy(slot + 8, patch + 8, 6);
    // 2. ONE aligned 8-byte compare-exchange flips the slot from `jmp body` to
    //    `jmp [rip+0]` with a fully formed operand: no thread can see a torn slot.
    const long long prev = _InterlockedCompareExchange64((volatile long long*)slot, (long long)new64, (long long)old64);
    if ((uint64_t)prev != old64) {
        memset(slot + 8, 0xCC, 6);
        VirtualProtect(slot, 16, oldprot, &oldprot);
        h.status = "REFUSED: the slot head changed under us (compare-exchange failed); nothing left behind";
        return false;
    }
    DWORD tmp = 0;
    VirtualProtect(slot, 16, oldprot, &tmp);
    FlushInstructionCache(GetCurrentProcess(), slot, 16);
    h.slot = slot;
    h.old64 = old64;
    h.new64 = new64;
    h.on = true;
    h.status = "INSTALLED";
    return true;
}

// PUTTING THE THUNKS BACK, in two phases, because a plain "write the old bytes back"
// is NOT safe and was measured to crash (build 242 offline test, a fake thunk hammered
// by 6 threads while it was flipped 200000 times: every plain restore run died, every
// run of this scheme survived). A thread that already decoded `jmp [rip+0]` reads its
// 8-byte operand at execution time, so an operand that is blanked back to int3 under it
// sends it to 0xCCCCCCCCCCCCCCCC.
//   phase 1: ONE aligned 8-byte compare-exchange puts `E9 rel32` back but KEEPS the
//            two low operand bytes (6,7) in the slot head; operand bytes 8..13 are not
//            touched, so a thread still inside the old jump finds the whole operand.
//   phase 2: only after the caller has waited long enough that no thread can still be
//            inside that one instruction, the dead padding (bytes 5..13) is blanked.
void restore_heads() {
    for (int i = 0; i < kHooks; ++i) {
        Hooked& h = g_h[i];
        if (!h.on) continue;
        if (i == kGraph && g_keep_graph.load(std::memory_order_relaxed)) continue;   // BUILD 243: the experiment still needs it
        const uint64_t head2 = (h.old64 & 0x0000FFFFFFFFFFFFull) | (h.new64 & 0xFFFF000000000000ull);
        DWORD oldprot = 0;
        if (VirtualProtect(h.slot, 16, PAGE_EXECUTE_READWRITE, &oldprot)) {
            const long long prev = _InterlockedCompareExchange64((volatile long long*)h.slot, (long long)head2, (long long)h.new64);
            DWORD tmp = 0;
            VirtualProtect(h.slot, 16, oldprot, &tmp);
            FlushInstructionCache(GetCurrentProcess(), h.slot, 16);
            if ((uint64_t)prev == h.new64) {
                h.on = false;
                h.pad_dirty = true;
                h.status = "INSTALLED, then RESTORED after the window";
                LOG_INFO("census: thunk %08X (%s) restored (E9 back; padding cleaned after a grace period).",
                         g_pins->pin[i].thunk, g_pins->pin[i].name);
                continue;
            }
        }
        LOG_ERROR("census: could not restore thunk %08X (%s); the stub stays (harmless: window closed, fast path only).",
                  g_pins->pin[i].thunk, g_pins->pin[i].name);
    }
}

void clean_padding() {
    for (int i = 0; i < kHooks; ++i) {
        Hooked& h = g_h[i];
        if (!h.pad_dirty) continue;
        DWORD oldprot = 0;
        if (VirtualProtect(h.slot, 16, PAGE_EXECUTE_READWRITE, &oldprot)) {
            memset(h.slot + 5, 0xCC, 9);
            DWORD tmp = 0;
            VirtualProtect(h.slot, 16, oldprot, &tmp);
            FlushInstructionCache(GetCurrentProcess(), h.slot, 16);
            h.pad_dirty = false;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// state, cfg, the Present boundary
// ---------------------------------------------------------------------------
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
        g_pts[b] = __rdtsc();
        read_slots(g_slot[b]);
    }
    if (b >= (uint64_t)kFrames) {
        grwxr_census_on = 0;
        int exp = 2;
        g_state.compare_exchange_strong(exp, 3, std::memory_order_release);
    }
}

namespace {

// ---------------------------------------------------------------------------
// the dump (its own thread; file I/O is allowed here and nowhere in the recorder)
// ---------------------------------------------------------------------------
double g_tsc_hz = 0.0;

double ms_since_arm(uint64_t ts) {
    return ts >= g_tsc0 ? (double)(ts - g_tsc0) * 1000.0 / g_tsc_hz : -(double)(g_tsc0 - ts) * 1000.0 / g_tsc_hz;
}

std::string hx(uint64_t v) {
    char b[24];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "0x%llX", (unsigned long long)v);
    return b;
}

std::string f4(const uint64_t lo, const uint64_t hi) {
    float f[4];
    memcpy(&f[0], &lo, 8);
    memcpy(&f[2], &hi, 8);
    uint32_t d[4];
    memcpy(d, f, 16);
    char b[160];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%08X %08X %08X %08X = (%g, %g, %g, %g)", d[0], d[1], d[2], d[3],
                (double)f[0], (double)f[1], (double)f[2], (double)f[3]);
    return b;
}

struct Stat {
    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    uint64_t sum = 0;
    int n = 0;
    void add(uint32_t v) { mn = std::min(mn, v); mx = std::max(mx, v); sum += v; ++n; }
    std::string str() const {
        char b[96];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "min %u / mean %.2f / max %u", n ? mn : 0, n ? (double)sum / n : 0.0, mx);
        return b;
    }
};

std::string set_str(const std::set<uint64_t>& s, size_t lim = 6) {
    std::string r;
    size_t k = 0;
    for (uint64_t v : s) {
        if (k++ >= lim) { r += " +" + std::to_string(s.size() - lim); break; }
        r += (r.empty() ? "" : " ") + hx(v);
    }
    return r.empty() ? "-" : r;
}

struct CamAgg {
    uint32_t calls = 0;
    int first_b = 1 << 30, last_b = -1;
    std::set<int> rows;
    std::set<uint64_t> idx, tids, P, S, D18, D358, renderer, blk0, r8, r9, slotp, caller;
    uint32_t stype_set = 0;     // bit per surface type value seen (0..31), 31 = other
    std::map<int, int> per_row;
    uint64_t first_blk[2] = {0, 0};
    bool have_blk = false;
    std::set<std::pair<uint64_t, uint64_t>> blk;
};

DWORD WINAPI dump_thread(LPVOID) {
    Sleep(1000);   // let any recorder that passed the gate before the stop finish writing
    restore_heads();   // the engine's thunk slots go back to `E9 rel32` (phase 1, see restore_heads)

    LARGE_INTEGER q1, qf;
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    const uint64_t t1 = __rdtsc();
    const double secs = (double)((uint64_t)q1.QuadPart - g_qpc0) / (double)qf.QuadPart;
    g_tsc_hz = secs > 0.1 ? (double)(t1 - g_tsc0) / secs : 3.0e9;

    const std::wstring path = log::data_dir() + L"\\census-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        LOG_ERROR("census: could not open the dump file (err %lu).", GetLastError());
        g_state.store(5, std::memory_order_release);
        return 0;
    }
    static char fbuf[1 << 16];
    setvbuf(f, fbuf, _IOFBF, sizeof(fbuf));

    const uint64_t start = g_start_frame.load();
    int last_b = kFrames - 1;
    for (int b = kFrames; b >= 1; --b) if (g_pts[b]) { last_b = b - 1; break; }
    const int rows = last_b + 1;
    const uint32_t ncam = std::min<uint32_t>(g_cam_head.load(), kCamCap);
    const uint32_t ngr  = std::min<uint32_t>(g_graph_head.load(), kGraphCap);
    const uint32_t ntk[2] = {std::min<uint32_t>(g_task_head[0].load(), kTaskCap), std::min<uint32_t>(g_task_head[1].load(), kTaskCap)};
    const unsigned long rtid = d3d11::render_thread_id();

    fprintf(f, "GRW-VR CAMERA/VIEW CENSUS, build 242, observation only. pid %lu, render thread (first Present) tid %lu\n",
            GetCurrentProcessId(), rtid);
    fprintf(f, "game build row: %s (PE %08X / %08X). image base 0x%llX size 0x%llX. RVAs below are TU25 only.\n",
            g_name, g_pins->pe_timestamp, g_pins->pe_size_of_image, (unsigned long long)g_base, (unsigned long long)g_size);
    fprintf(f, "window: Present #%llu .. #%llu (%d rows), opened right after the hooks went in, %.1f s after the first stereo layer; "
               "tsc %.3f GHz\n",
            (unsigned long long)start, (unsigned long long)(start + rows), rows,
            (double)(g_arm_ms - g_first_stereo_ms.load()) / 1000.0, g_tsc_hz / 1e9);
    fprintf(f, "ROW b = work recorded while frame_count() == start+b, i.e. between Present #(start+b) and Present #(start+b+1). "
               "Row 0 is partial (opened mid-frame). A worker's call lands in the row current when it made the call, "
               "which can lag the render thread.\n\n");
    fprintf(f, "=== HOOKS (each verified against the TU25 exe bytes before anything was written) ===\n");
    for (int i = 0; i < kHooks; ++i) {
        const Pin& p = g_pins->pin[i];
        fprintf(f, "[%d] %-20s thunk RVA 0x%08X -> body RVA 0x%08X : %s\n", i, p.name, p.thunk, p.body, g_h[i].status);
        if (strncmp(g_h[i].status, "REFUSED", 7) == 0) {
            fprintf(f, "      expected thunk:");
            for (int k = 0; k < 16; ++k) fprintf(f, " %02X", p.thunk_bytes[k]);
            fprintf(f, "\n      found    thunk:");
            for (int k = 0; k < 16; ++k) fprintf(f, " %02X", g_h[i].found_thunk[k]);
            fprintf(f, "\n      expected body :");
            for (int k = 0; k < 16; ++k) fprintf(f, " %02X", p.body_bytes[k]);
            fprintf(f, "\n      found    body :");
            for (int k = 0; k < 16; ++k) fprintf(f, " %02X", g_h[i].found_body[k]);
            fprintf(f, "\n");
        }
    }
    fprintf(f, "recorder calls %u. events stored/cap/dropped: camera %u/%u/%u, graph %u/%u/%u, build %u/%u/%u, exec %u/%u/%u; "
               "thread-table-full %u. Exact per-row counts (the table below) never drop; a drop only thins the event lists.\n\n",
            g_rec_calls.load(), ncam, kCamCap, g_cam_drop.load(), ngr, kGraphCap, g_graph_drop.load(), ntk[0], kTaskCap,
            g_task_drop[0].load(), ntk[1], kTaskCap, g_task_drop[1].load(), g_thr_full.load());

    // ---- gather ----
    std::map<uint64_t, CamAgg> cams;
    std::vector<std::set<uint64_t>> row_cams(rows + 1);
    std::vector<std::vector<std::pair<uint32_t, uint64_t>>> row_cam_list(rows + 1);   // (slot idx, cam)
    for (uint32_t i = 0; i < ncam; ++i) {
        const EvCam& e = g_cam[i];
        if (!e.ts.load(std::memory_order_acquire)) continue;
        const int b = e.bucket;
        if (b >= rows) continue;
        const uint64_t cam = e.w[1];
        CamAgg& c = cams[cam];
        const uint32_t flags = (uint32_t)((e.w[4] >> 32) & 0xFFFF);
        ++c.calls;
        c.first_b = std::min(c.first_b, b);
        c.last_b = std::max(c.last_b, b);
        c.rows.insert(b);
        c.per_row[b]++;
        c.tids.insert(e.tid);
        c.renderer.insert(e.w[0]);
        c.r8.insert(e.w[2]);
        c.r9.insert(e.w[3]);
        c.caller.insert(e.caller);
        if (flags & 0x001) c.idx.insert((uint32_t)e.w[4]);
        if (flags & 0x002) c.P.insert(e.w[5]);
        if (flags & 0x004) {
            const int32_t m = (int32_t)(e.w[7] >> 32);
            c.S.insert(m < 0 ? e.w[6] : 0);
        }
        if (flags & 0x010) c.D18.insert(e.w[8]);
        if (flags & 0x020) c.D358.insert(e.w[9]);
        if (flags & 0x008) c.stype_set |= 1u << std::min<uint32_t>(31, (uint32_t)(e.w[7] & 0xFFFFFFFFu));
        if (flags & 0x040) {
            if (!c.have_blk) { c.first_blk[0] = e.w[10]; c.first_blk[1] = e.w[11]; c.have_blk = true; }
            c.blk.insert({e.w[10], e.w[11]});
            c.blk0.insert(e.w[10]);
        }
        if (flags & 0x100) c.slotp.insert(e.w[12]);
        row_cams[b].insert(cam);
        row_cam_list[b].push_back({(flags & 0x001) ? (uint32_t)e.w[4] : 0xFFFFFFFFu, cam});
    }

    // ---- SUMMARY ----
    fprintf(f, "=== SUMMARY ===\n");
    // per-row stats (rows 1..rows-1)
    Stat st[kHooks], dthr[2], ncamrow;
    for (int b = 1; b < rows; ++b) {
        for (int h = 0; h < kHooks; ++h) st[h].add(g_cnt[h][b].load());
        for (int w = 0; w < 2; ++w) {
            uint32_t nt = 0;
            for (int i = 0; i < kThrCap; ++i)
                if (g_thr[i].tid.load() && g_thr[i].n[kBuild + w][b].load()) ++nt;
            dthr[w].add(nt);
        }
        ncamrow.add((uint32_t)row_cams[b].size());
    }
    fprintf(f, "calls per full row (rows 1..%d), exact counts:\n", rows - 1);
    for (int h = 0; h < kHooks; ++h) {
        uint64_t tot = 0;
        for (int b = 0; b < rows; ++b) tot += g_cnt[h][b].load();
        fprintf(f, "  %-20s total %8llu   per row %s\n", g_pins->pin[h].name, (unsigned long long)tot, st[h].str().c_str());
    }
    fprintf(f, "  distinct threads per row that ran BuildCmdList: %s; ExecuteCmdList: %s\n", dthr[0].str().c_str(),
            dthr[1].str().c_str());
    {
        uint64_t cc = 0, bb = 0, ee = 0;
        for (int b = 1; b < rows; ++b) { cc += g_cnt[kCam][b].load(); bb += g_cnt[kBuild][b].load(); ee += g_cnt[kExec][b].load(); }
        if (cc) fprintf(f, "  BuildCmdList per per-camera call: %.2f; ExecuteCmdList per per-camera call: %.2f (rows 1..%d)\n",
                        (double)bb / cc, (double)ee / cc, rows - 1);
    }

    fprintf(f, "\n--- CAMERAS PER FRAME (distinct Camera pointers passed to the per-camera entry, from the stored events) ---\n");
    {
        std::map<uint32_t, int> hist;
        for (int b = 1; b < rows; ++b) hist[(uint32_t)row_cams[b].size()]++;
        fprintf(f, "cameras per full row: %s;  histogram (cameras: rows):", ncamrow.str().c_str());
        for (auto& kv : hist) fprintf(f, " %u:%d", kv.first, kv.second);
        fprintf(f, "\n");
        if (g_cam_drop.load()) fprintf(f, "NOTE: %u camera events were dropped (cap %u); distinct-pointer results are a lower bound.\n",
                                       g_cam_drop.load(), kCamCap);
    }
    fprintf(f, "distinct camera pointers seen: %zu\n", cams.size());
    uint64_t primary = 0;
    {
        uint32_t best = 0;
        for (auto& kv : cams) if (kv.second.calls > best) { best = kv.second.calls; primary = kv.first; }
    }
    fprintf(f, "%-16s %6s %5s %5s %5s %-14s %-18s %-18s %-18s %-18s\n", "camera", "calls", "rows", "first", "last", "slot idx [+2F4]",
            "P [+2A8]", "surface S", "[S+0xD18]", "[[S+D18]+358]");
    for (auto& kv : cams) {
        const CamAgg& c = kv.second;
        std::set<uint64_t> idxs(c.idx.begin(), c.idx.end());
        std::string is;
        for (uint64_t v : idxs) is += (is.empty() ? "" : ",") + std::to_string(v);
        fprintf(f, "0x%-14llX %6u %5zu %5d %5d %-14s %-18s %-18s %-18s %-18s%s\n", (unsigned long long)kv.first, c.calls, c.rows.size(),
                c.first_b, c.last_b, is.empty() ? "-" : is.c_str(), set_str(c.P, 3).c_str(), set_str(c.S, 3).c_str(),
                set_str(c.D18, 3).c_str(), set_str(c.D358, 3).c_str(), kv.first == primary ? "   <- most calls (primary)" : "");
    }
    for (auto& kv : cams) {
        const CamAgg& c = kv.second;
        int maxper = 0;
        for (auto& r : c.per_row) maxper = std::max(maxper, r.second);
        std::string stp;
        for (int k = 0; k < 32; ++k) if (c.stype_set & (1u << k)) stp += (stp.empty() ? "" : ",") + std::to_string(k);
        fprintf(f, "camera 0x%llX: max calls in one row %d; threads %s; renderer(rcx) %s; r8 %s; r9 %s; caller RVAs %s; "
                   "[S+0x1C] values %s; slot pointers %s\n",
                (unsigned long long)kv.first, maxper, set_str(c.tids, 4).c_str(), set_str(c.renderer, 2).c_str(),
                set_str(c.r8, 3).c_str(), set_str(c.r9, 3).c_str(), set_str(c.caller, 3).c_str(), stp.empty() ? "-" : stp.c_str(),
                set_str(c.slotp, 3).c_str());
        if (c.have_blk)
            fprintf(f, "   block at cam+0x290, first 16 bytes, first seen: %s;  distinct values over the window: %zu%s\n",
                    f4(c.first_blk[0], c.first_blk[1]).c_str(), c.blk.size(), c.blk.size() > 1 ? "  (CHANGES)" : "");
    }

    fprintf(f, "\n--- SECOND CAMERA / MINIMAP CADENCE ---\n");
    if (cams.size() < 2) {
        fprintf(f, "only %zu distinct camera pointer in the window, so no second camera appeared at all.\n", cams.size());
    } else {
        std::vector<int> second_rows;
        int multi = 0;
        for (int b = 1; b < rows; ++b) {
            if (row_cams[b].size() >= 2) ++multi;
            bool nonprim = false;
            for (uint64_t c : row_cams[b]) if (c != primary) nonprim = true;
            if (nonprim) second_rows.push_back(b);
        }
        fprintf(f, "rows with 2 or more distinct cameras: %d of %d full rows. rows where a NON-primary camera was called: %zu "
                   "(primary = 0x%llX).\n", multi, rows - 1, second_rows.size(), (unsigned long long)primary);
        if (second_rows.size() >= 2) {
            std::vector<int> gaps;
            for (size_t i = 1; i < second_rows.size(); ++i) gaps.push_back(second_rows[i] - second_rows[i - 1]);
            std::vector<int> sg = gaps;
            std::sort(sg.begin(), sg.end());
            fprintf(f, "gap between rows with a non-primary camera: min %d / median %d / max %d presents. rows:", sg.front(),
                    sg[sg.size() / 2], sg.back());
            for (size_t i = 0; i < second_rows.size() && i < 40; ++i) fprintf(f, " %d", second_rows[i]);
            if (second_rows.size() > 40) fprintf(f, " ...");
            fprintf(f, "\n");
        } else if (second_rows.size() == 1) {
            fprintf(f, "a non-primary camera appeared in exactly one row: %d\n", second_rows[0]);
        }
        for (auto& kv : cams) {
            if (kv.first == primary) continue;
            const CamAgg& c = kv.second;
            std::vector<int> r(c.rows.begin(), c.rows.end());
            std::string g;
            if (r.size() >= 2) {
                std::vector<int> gaps;
                for (size_t i = 1; i < r.size(); ++i) gaps.push_back(r[i] - r[i - 1]);
                std::sort(gaps.begin(), gaps.end());
                char b[96];
                _snprintf_s(b, sizeof(b), _TRUNCATE, "gaps min %d / median %d / max %d", gaps.front(), gaps[gaps.size() / 2], gaps.back());
                g = b;
            }
            fprintf(f, "camera 0x%llX: in %zu rows (%.1f%% of full rows), %u calls; %s\n", (unsigned long long)kv.first, c.rows.size(),
                    100.0 * (double)c.rows.size() / std::max(1, rows - 1), c.calls, g.empty() ? "no cadence (fewer than 2 rows)" : g.c_str());
        }
    }

    fprintf(f, "\n--- DOES THE SURFACE LINK DIFFER BETWEEN CAMERAS ---\n");
    {
        std::set<uint64_t> allP, allS, allD, allT;
        std::set<uint64_t> blk0;
        bool stable = true;
        for (auto& kv : cams) {
            allP.insert(kv.second.P.begin(), kv.second.P.end());
            allS.insert(kv.second.S.begin(), kv.second.S.end());
            allD.insert(kv.second.D18.begin(), kv.second.D18.end());
            allT.insert(kv.second.D358.begin(), kv.second.D358.end());
            if (kv.second.P.size() > 1 || kv.second.S.size() > 1) stable = false;
        }
        fprintf(f, "Camera+0x2A8 (P): %zu distinct value(s) across %zu camera(s): %s\n", allP.size(), cams.size(),
                cams.size() < 2 ? "only one camera, cannot compare" : (allP.size() > 1 ? "DIFFERS between cameras" : "SAME for all cameras"));
        fprintf(f, "resolved surface S: %zu distinct: %s;  [S+0xD18]: %zu distinct: %s;  [[S+0xD18]+0x358] (the graphic-frame object the builder uses): %zu distinct: %s\n",
                allS.size(), cams.size() < 2 ? "n/a" : (allS.size() > 1 ? "DIFFERS" : "SAME"), allD.size(),
                cams.size() < 2 ? "n/a" : (allD.size() > 1 ? "DIFFERS" : "SAME"), allT.size(),
                cams.size() < 2 ? "n/a" : (allT.size() > 1 ? "DIFFERS" : "SAME"));
        fprintf(f, "within one camera, did P or S change over the window: %s\n", stable ? "no" : "YES (see the camera table)");
    }

    fprintf(f, "\n--- GRAPH-FRAME BUILDER CALLS AND THE VIEW REGISTRY ---\n");
    {
        Stat gs, pool_distinct;
        for (int b = 1; b < rows; ++b) gs.add(g_cnt[kGraph][b].load());
        fprintf(f, "builder calls per full row: %s\n", gs.str().c_str());
        std::set<uint64_t> pools, regs, views, rcxs;
        std::set<uint64_t> reg_cams;
        struct VAgg { std::set<uint64_t> vtbl, cams, ncams; std::set<int> lists; int seen = 0; };
        std::map<uint64_t, VAgg> vagg;
        std::map<uint32_t, int> nviews_hist;
        int rec_n = 0;
        for (uint32_t i = 0; i < ngr; ++i) {
            const GraphRec& g = g_graph[i];
            if (!g.ts.load(std::memory_order_acquire)) continue;
            ++rec_n;
            if (g.ok & 2) pools.insert(g.pool);
            if (g.ok & 1) regs.insert(g.reg);
            rcxs.insert(g.rcx);
            nviews_hist[(uint32_t)g.n[0] + g.n[1]]++;
            for (int L = 0; L < 2; ++L)
                for (int v = 0; v < kViewsPerList; ++v) {
                    const ViewRec& r = g.v[L][v];
                    if (!r.view) continue;
                    VAgg& a = vagg[r.view];
                    ++a.seen;
                    a.lists.insert(L);
                    if (r.ok & 2) a.vtbl.insert(r.vtbl);
                    if (r.ok & 4) a.ncams.insert(r.ncam);
                    for (int c = 0; c < kCamsPerView; ++c) if (r.cam[c]) { a.cams.insert(r.cam[c]); reg_cams.insert(r.cam[c]); }
                }
        }
        fprintf(f, "graph records stored: %d. registry pointers seen: %s. frame-graph object (rcx) values: %s\n", rec_n,
                set_str(regs, 4).c_str(), set_str(rcxs, 4).c_str());
        fprintf(f, "registry view counts (list A + list B, as the registry holds them, active filter NOT applied) per call, histogram (views: calls):");
        for (auto& kv : nviews_hist) fprintf(f, " %u:%d", kv.first, kv.second);
        fprintf(f, "\n");
        fprintf(f, "distinct views seen: %zu\n", vagg.size());
        for (auto& kv : vagg) {
            std::string ls;
            for (int L : kv.second.lists) ls += (ls.empty() ? "" : "+") + std::string(L ? "B" : "A");
            std::string nc;
            for (uint64_t n : kv.second.ncams) nc += (nc.empty() ? "" : ",") + std::to_string(n);
            fprintf(f, "  view 0x%llX in list %s, seen in %d record(s), vtable %s, camera count [+0x22] {%s}, cameras %s\n",
                    (unsigned long long)kv.first, ls.c_str(), kv.second.seen, set_str(kv.second.vtbl, 2).c_str(), nc.c_str(),
                    set_str(kv.second.cams, 6).c_str());
        }
        std::string only_reg, only_call;
        for (uint64_t c : reg_cams) if (!cams.count(c)) only_reg += (only_reg.empty() ? "" : " ") + hx(c);
        for (auto& kv : cams) if (!reg_cams.count(kv.first)) only_call += (only_call.empty() ? "" : " ") + hx(kv.first);
        fprintf(f, "cameras in a registry view but never passed to the per-camera entry: %s\n", only_reg.empty() ? "none" : only_reg.c_str());
        fprintf(f, "cameras passed to the per-camera entry but in no registry view (first %d views per list, first %d cameras per view): %s\n",
                kViewsPerList, kCamsPerView, only_call.empty() ? "none" : only_call.c_str());
        fprintf(f, "context-pool global (int, deferred wrapper count; pool = value + 1): distinct values %s\n",
                pools.empty() ? "-" : set_str(pools, 6).c_str());
        (void)pool_distinct;
    }

    fprintf(f, "\n--- PER-CAMERA TASK SLOT (renderer+0x2470 array, slot+0xFCC counter), read at each Present entry ---\n");
    {
        fprintf(f, "node pool per slot is 0xA0 = 160 (REPORT.md); the counter is read raw, its exact meaning is [UNKNOWN]. rows read: %d\n", rows);
        std::set<uint64_t> rends, rcalls, counts;
        for (int b = 0; b <= rows && b <= kFrames; ++b) {
            if (g_slot[b].renderer) rends.insert(g_slot[b].renderer);
            if (g_slot[b].renderer_call) rcalls.insert(g_slot[b].renderer_call);
            if (g_slot[b].ok & 4) counts.insert(g_slot[b].count);
        }
        fprintf(f, "renderer via [[holder]+0x10]: %s;  renderer from the per-camera rcx: %s;  slot count [+0x247A] values: %s\n",
                set_str(rends, 3).c_str(), set_str(rcalls, 3).c_str(), set_str(counts, 6).c_str());
        for (int i = 0; i < kSlotsRead; ++i) {
            Stat s;
            std::set<uint64_t> ptrs;
            uint32_t last = 0;
            int n = 0;
            for (int b = 0; b <= rows && b <= kFrames; ++b) {
                if (g_slot[b].okc[i] & 1) ptrs.insert(g_slot[b].ptr[i]);
                if (g_slot[b].okc[i] & 2) { s.add(g_slot[b].ctr[i]); last = g_slot[b].ctr[i]; ++n; }
            }
            if (ptrs.empty()) continue;
            fprintf(f, "  slot %d: pointer(s) %s; counter %s over %d rows, last %u\n", i, set_str(ptrs, 3).c_str(), s.str().c_str(), n, last);
        }
    }

    // ---- per-row table ----
    fprintf(f, "\n=== PER ROW ===\n");
    fprintf(f, "gr = graph-frame builder calls, cam = per-camera entry calls, bld/exe = BuildCmdList/ExecuteCmdList calls, "
               "ncam = distinct cameras, tB/tE = distinct threads that ran build/exec, sc0..sc3 = slot counters read at the entry of "
               "the NEXT Present (end of this row), cameras = slotidx:pointer in call order\n");
    fprintf(f, "%4s %6s %7s | %3s %3s %5s %5s | %4s %3s %3s | %9s %9s %9s %9s | cameras\n", "b", "frame", "dt_ms", "gr", "cam", "bld", "exe",
            "ncam", "tB", "tE", "sc0", "sc1", "sc2", "sc3");
    for (int b = 0; b < rows; ++b) {
        const double dt = (g_pts[b + 1] && (b > 0 ? g_pts[b] : g_tsc0)) ?
            (double)(g_pts[b + 1] - (b > 0 ? g_pts[b] : g_tsc0)) * 1000.0 / g_tsc_hz : -1.0;
        uint32_t tb = 0, te = 0;
        for (int i = 0; i < kThrCap; ++i) {
            if (!g_thr[i].tid.load()) continue;
            if (g_thr[i].n[kBuild][b].load()) ++tb;
            if (g_thr[i].n[kExec][b].load()) ++te;
        }
        char sc[4][16];
        for (int i = 0; i < 4; ++i) {
            if (g_slot[b + 1].okc[i] & 2) _snprintf_s(sc[i], sizeof(sc[i]), _TRUNCATE, "%u", g_slot[b + 1].ctr[i]);
            else _snprintf_s(sc[i], sizeof(sc[i]), _TRUNCATE, "-");
        }
        std::string cl;
        for (auto& pr : row_cam_list[b]) {
            char t[48];
            if (pr.first == 0xFFFFFFFFu) _snprintf_s(t, sizeof(t), _TRUNCATE, " ?:0x%llX", (unsigned long long)pr.second);
            else _snprintf_s(t, sizeof(t), _TRUNCATE, " %u:0x%llX", pr.first, (unsigned long long)pr.second);
            cl += t;
        }
        fprintf(f, "%4d %6llu %7.2f | %3u %3u %5u %5u | %4zu %3u %3u | %9s %9s %9s %9s |%s\n", b, (unsigned long long)(start + b), dt,
                g_cnt[kGraph][b].load(), g_cnt[kCam][b].load(), g_cnt[kBuild][b].load(), g_cnt[kExec][b].load(), row_cams[b].size(), tb, te,
                sc[0], sc[1], sc[2], sc[3], cl.c_str());
    }

    // ---- threads ----
    fprintf(f, "\n=== THREADS THAT CALLED A HOOKED FUNCTION ===\n");
    fprintf(f, "%8s %-8s %9s %9s %9s %9s\n", "tid", "role", "graph", "camera", "build", "exec");
    for (int i = 0; i < kThrCap; ++i) {
        const uint32_t tid = g_thr[i].tid.load();
        if (!tid) continue;
        uint64_t t[kHooks] = {};
        for (int h = 0; h < kHooks; ++h) for (int b = 0; b < rows; ++b) t[h] += g_thr[i].n[h][b].load();
        fprintf(f, "%8u %-8s %9llu %9llu %9llu %9llu\n", tid, tid == rtid ? "render" : "worker", (unsigned long long)t[0],
                (unsigned long long)t[1], (unsigned long long)t[2], (unsigned long long)t[3]);
    }

    // ---- graph records (full) ----
    fprintf(f, "\n=== GRAPH-FRAME BUILDER CALLS (every stored record) ===\n");
    fprintf(f, "t = ms since the window opened. Lists are the registry at RVA 0x%08X: A = [+0x6D] x [+0x77], B = [+0x61] x [+0x6B]. "
               "Per view: pointer, vtable, camera count [+0x22], cameras [+0x18].\n", g_pins->registry_global);
    for (uint32_t i = 0; i < ngr; ++i) {
        const GraphRec& g = g_graph[i];
        const uint64_t ts = g.ts.load(std::memory_order_acquire);
        if (!ts) continue;
        fprintf(f, "%10.3f b=%-3u tid=%-6u caller=0x%08X rcx=0x%llX rdx=0x%llX registry=0x%llX pool=%d nA=%u nB=%u\n", ms_since_arm(ts),
                g.bucket, g.tid, g.caller, (unsigned long long)g.rcx, (unsigned long long)g.rdx, (unsigned long long)g.reg,
                (g.ok & 2) ? (int)g.pool : -1, g.n[0], g.n[1]);
        for (int L = 0; L < 2; ++L)
            for (int v = 0; v < kViewsPerList; ++v) {
                const ViewRec& r = g.v[L][v];
                if (!r.view) continue;
                fprintf(f, "             %c%d view=0x%llX vtbl=0x%llX ncam=%u cams:", L ? 'B' : 'A', v, (unsigned long long)r.view,
                        (unsigned long long)r.vtbl, r.ncam);
                for (int c = 0; c < kCamsPerView; ++c) if (r.cam[c]) fprintf(f, " 0x%llX", (unsigned long long)r.cam[c]);
                fprintf(f, "\n");
            }
    }

    // ---- raw events, merged by time ----
    fprintf(f, "\n=== EVENTS (time order, t = ms since the window opened) ===\n");
    fprintf(f, "CAMERA lines: slot idx, flags (1 idx 2 P 4 S 8 S+1C 10 D18 20 358 40 blk 80 slotcount 100 slotptr 200 slotctr), P, S, "
               "[S+0x1C], [S+0xD18], [[..]+0x358], block@+0x290, slot pointer, slot+0xFCC. BUILD/EXEC lines: rcx and the four qwords at rcx.\n");
    struct Item { uint64_t ts; int type; uint32_t i; };
    std::vector<Item> items;
    for (uint32_t i = 0; i < ncam; ++i) if (uint64_t t = g_cam[i].ts.load(std::memory_order_acquire)) items.push_back({t, 1, i});
    for (int w = 0; w < 2; ++w)
        for (uint32_t i = 0; i < ntk[w]; ++i)
            if (uint64_t t = g_task[w][i].ts.load(std::memory_order_acquire)) items.push_back({t, 2 + w, i});
    for (int b = 0; b <= rows && b <= kFrames; ++b) if (g_pts[b]) items.push_back({g_pts[b], 9, (uint32_t)b});
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& c) { return a.ts < c.ts; });
    for (const Item& it : items) {
        const double t = ms_since_arm(it.ts);
        if (it.type == 9) {
            fprintf(f, "%10.3f -------- PRESENT #%llu (row %u starts)\n", t, (unsigned long long)(start + it.i), it.i);
        } else if (it.type == 1) {
            const EvCam& e = g_cam[it.i];
            const uint32_t flags = (uint32_t)((e.w[4] >> 32) & 0xFFFF);
            const int32_t m = (int32_t)(e.w[7] >> 32);
            fprintf(f, "%10.3f b=%-3u tid=%-6u CAMERA cam=0x%llX idx=%d flags=%03X P=0x%llX S=0x%llX stype=%d D18=0x%llX D358=0x%llX "
                       "blk=[%s] slot=0x%llX ctr=%d renderer=0x%llX r8=0x%llX r9=0x%llX caller=0x%08X\n",
                    t, e.bucket, e.tid, (unsigned long long)e.w[1], (flags & 1) ? (int)(uint32_t)e.w[4] : -1, flags,
                    (unsigned long long)e.w[5], (unsigned long long)(m < 0 ? e.w[6] : 0), (flags & 8) ? (int)(uint32_t)e.w[7] : -1,
                    (unsigned long long)e.w[8], (unsigned long long)e.w[9], (flags & 0x40) ? f4(e.w[10], e.w[11]).c_str() : "-",
                    (unsigned long long)e.w[12], (flags & 0x200) ? (int)(uint32_t)e.w[13] : -1, (unsigned long long)e.w[0],
                    (unsigned long long)e.w[2], (unsigned long long)e.w[3], e.caller);
        } else {
            const EvTask& e = g_task[it.type - 2][it.i];
            fprintf(f, "%10.3f b=%-3u tid=%-6u %-5s rcx=0x%llX q=[0x%llX 0x%llX 0x%llX 0x%llX]%s caller=0x%08X\n", t, e.bucket, e.tid,
                    it.type == 2 ? "BUILD" : "EXEC", (unsigned long long)e.w[0], (unsigned long long)e.w[1], (unsigned long long)e.w[2],
                    (unsigned long long)e.w[3], (unsigned long long)e.w[4], e.okmask ? "" : " (unreadable)", e.caller);
        }
    }
    fclose(f);
    Sleep(1500);       // grace period: no thread can still be inside a pre-restore `jmp [rip+0]`
    clean_padding();   // phase 2: the slot is byte-identical to the original again
    LOG_INFO("census: wrote %ls (%u camera, %u graph, %u build, %u exec events; %d rows). Read the SUMMARY block at the top.",
             path.c_str(), ncam, ngr, ntk[0], ntk[1], rows);
    g_state.store(5, std::memory_order_release);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// the TU25 gate, shared by the census tick and the BUILD 243 second-camera hook.
// Idempotent. Init thread only. nullptr = ready; else "not-tu25" or "no-module".
// ---------------------------------------------------------------------------
namespace {
const char* init_pins() {
    if (g_pins) return nullptr;
    const gamebuild::Build* gb = gamebuild::get();
    const CensusBuild* pins = nullptr;
    if (gb)
        for (const CensusBuild* c : kCensusTable)
            if (c->pe_timestamp == gb->pe_timestamp && c->pe_size_of_image == gb->pe_size_of_image) pins = c;
    if (!pins) return "not-tu25";
    const HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) return "no-module";
    const auto* dos = (const IMAGE_DOS_HEADER*)mod;
    const auto* nt = (const IMAGE_NT_HEADERS64*)((const uint8_t*)mod + dos->e_lfanew);
    g_base = (uint64_t)(uintptr_t)mod;
    g_size = nt->OptionalHeader.SizeOfImage;
    g_name = gb->name;
    g_pins = pins;   // last: a non-null g_pins means base, size and name are set
    return nullptr;
}
}  // namespace

bool acquire_graph_hook(void (*cb)(), const char** why) {
    const char* r = init_pins();
    if (r) { *why = r; return false; }
    if (!install_one(kGraph)) { *why = g_h[kGraph].status; return false; }
    g_keep_graph.store(true, std::memory_order_relaxed);
    g_graph_cb.store(cb, std::memory_order_release);
    grwxr_cam2_on = 1;
    *why = g_h[kGraph].status;
    return true;
}

void release_graph_hook() {
    grwxr_cam2_on = 0;
    g_graph_cb.store(nullptr, std::memory_order_release);
}

void projshadow_watch(bool on) {
    if (!on) g_ps_mask.store(0, std::memory_order_relaxed);
    g_ps_watch.store(on, std::memory_order_relaxed);
}

uint32_t projshadow_take(bool* live) {
    if (live) *live = (g_state.load(std::memory_order_relaxed) == 2) && g_h[kBuild].on;
    return g_ps_mask.exchange(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// the state machine (init thread, once a second)
// ---------------------------------------------------------------------------
void tick() {
    int st = g_state.load(std::memory_order_acquire);
    if (st >= 4 || !g_enabled.load(std::memory_order_relaxed)) return;
    const uint64_t first = g_first_stereo_ms.load(std::memory_order_relaxed);
    if (!first) return;
    const uint64_t now = GetTickCount64();
    const uint64_t age = now - first;

    if (st == 0 && age >= kArmMs) {
        // Gate: only the TU25 row. Anything else installs nothing, once.
        const char* why = init_pins();
        if (why) {
            g_state.store(6, std::memory_order_release);
            if (strcmp(why, "not-tu25") == 0)
                LOG_INFO("census: this GRW.exe (%s) is not the TU25 build the census table is keyed to. Nothing installed.",
                         gamebuild::get() ? gamebuild::get()->name : "unknown");
            else
                LOG_ERROR("census: no main module. Nothing installed.");
            return;
        }
        const CensusBuild* pins = g_pins;

        int n = 0;
        for (int i = 0; i < kHooks; ++i) {
            if (install_one(i)) {
                ++n;
                LOG_INFO("census: hook %d (%s) installed: thunk RVA 0x%08X -> body RVA 0x%08X.", i, pins->pin[i].name, pins->pin[i].thunk,
                         pins->pin[i].body);
            } else {
                LOG_ERROR("census: hook %d (%s) NOT installed: %s. (thunk RVA 0x%08X; nothing written for it)", i, pins->pin[i].name,
                          g_h[i].status, pins->pin[i].thunk);
            }
        }
        if (n == 0) {
            g_state.store(6, std::memory_order_release);
            LOG_ERROR("census: no hook could be installed. No capture this run.");
            return;
        }
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_qpc0 = (uint64_t)q.QuadPart;
        g_tsc0 = __rdtsc();
        g_arm_ms = now;
        g_start_frame.store(d3d11::frame_count(), std::memory_order_relaxed);
        g_state.store(2, std::memory_order_release);
        grwxr_census_on = 1;
        LOG_INFO("census: %d of %d hooks installed; window OPEN at Present #%llu, capturing %d presents.", n, kHooks,
                 (unsigned long long)d3d11::frame_count(), kFrames);
        return;
    }
    if (st == 2 && now - g_arm_ms > kGiveUpMs) {
        grwxr_census_on = 0;
        int exp = 2;
        if (g_state.compare_exchange_strong(exp, 3))
            LOG_WARN("census: the window did not fill in %llu s; closing it with what it has.", (unsigned long long)(kGiveUpMs / 1000));
        st = g_state.load();
    }
    if (st == 3) {
        int exp = 3;
        if (g_state.compare_exchange_strong(exp, 4)) {
            LOG_INFO("census: window CLOSED, dumping and restoring the thunks.");
            if (HANDLE t = CreateThread(nullptr, 0, dump_thread, nullptr, 0, nullptr)) CloseHandle(t);
            else g_state.store(5, std::memory_order_release);
        }
    }
}

}  // namespace census
}  // namespace grwxr

// The recorder the assembly stubs call. Defined at global scope for the C linkage.
extern "C" void grwxr_census_record(uint32_t id, const CensusSaved* a, uint64_t ret) {
    ::grwxr::census::record_impl(id, a, ret);
}
