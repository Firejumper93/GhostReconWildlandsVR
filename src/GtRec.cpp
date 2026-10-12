// GtRec.cpp - BUILD 270, see GtRec.h. LOG ONLY.
//
// Layout of this file: the rings and the per-thread state (hot path, rule 8), the record writers (B, W, G, C, P),
// then the init thread's state machine and drain (the only file I/O). Every engine read is an SEH-guarded copy.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "GtRec.h"
#include "Anatomy.h"
#include "Cam2.h"
#include "CameraProbe.h"
#include "D3D11Hook.h"
#include "HeadPose.h"
#include "Log.h"

namespace grwxr {
namespace gtrec {

std::atomic<int> g_live{0};
PRow g_ps;
bool g_ps_open = false;

namespace {

constexpr uint32_t kModBuild = 270;
constexpr uint32_t kCbBytes  = 1008;                 // the per-view constant buffer (F-254)
// The two main recompute passes, as CameraProbe.cpp kSiteA / kSiteB identify them (B rows carry the classification
// itself; these two are copied into the file header for the reader only).
constexpr uint32_t kSiteARva = 0x0D6454AAu, kSiteBRva = 0x0DA2534Eu;
constexpr size_t   kStageBytes = 2u << 20;           // the drain's staging buffer (one chunk at a time)

// ---------------------------------------------------------------------------------------------------------------------
// The ring: preallocated slots, multi-producer by a 64-bit reservation counter, a per-slot publish word (n + 1 once
// slot n is complete, 0 while it is being written). Writers: one fetch_add, one store, one fence, the copy, one
// release store. The drain (init thread only) copies a slot whose word says it is complete and checks the word again
// after the copy (a changed word means a writer lapped it mid-copy: the record is counted lost, never half-written).
// ---------------------------------------------------------------------------------------------------------------------
template <uint32_t kBytes, uint32_t kCap>
struct Ring {
    static_assert((kCap & (kCap - 1)) == 0, "the ring capacity must be a power of two");
    static constexpr uint32_t bytes = kBytes;
    static constexpr uint32_t cap   = kCap;
    uint8_t*              base = nullptr;            // set by the init thread at arm, before g_live; never freed
    std::atomic<uint64_t> pub[kCap];
    std::atomic<uint64_t> head{0};                   // reservations, any thread
    std::atomic<uint64_t> lost{0};                   // counted by the drain: lapped, torn, stuck
    uint64_t tail = 0;                               // the drain cursor (init thread only, as are the fields below)
    uint32_t stuck_ticks = 0;
    uint64_t lost_reported = 0;
    uint64_t written = 0;
    uint64_t written_last = 0;

    uint8_t* reserve(uint64_t* n) {
        uint8_t* b = base;
        if (!b) return nullptr;
        const uint64_t i = head.fetch_add(1, std::memory_order_relaxed);
        *n = i;
        pub[i & (kCap - 1)].store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        return b + (size_t)(i & (kCap - 1)) * kBytes;
    }
    void publish(uint64_t n) { pub[n & (kCap - 1)].store(n + 1, std::memory_order_release); }
};

// Slot counts (design section 6.1): about 14 s of headroom at 72 presents a second, 9 s for C.
Ring<sizeof(BRow), 4096>  g_b;
Ring<sizeof(WRow), 4096>  g_w;
Ring<sizeof(GRow), 1024>  g_g;
Ring<sizeof(CRow), 32768> g_c;
Ring<sizeof(PRow), 1024>  g_p;

// cfg
std::atomic<bool>     g_enabled{false};
std::atomic<bool>     g_cb{true};
std::atomic<int>      g_max_mb{1024};
std::atomic<int>      g_seconds{0};
std::atomic<uint32_t> g_cfg_bits[16] = {};

// shared counters
std::atomic<uint64_t> g_rc{0};                       // recompute entries recorded (rc_id = the value after the add)
std::atomic<uint32_t> g_graph{0};                    // graph-builder entries recorded
std::atomic<uint64_t> g_last_compose{0};             // seq + 1 of the last kWCompose W row, 0 = none yet
std::atomic<uint32_t> g_faults{0};                   // guarded reads that faulted

// per thread
thread_local uint64_t t_rc = 0;                      // the B row open on this thread (between b_begin and b_end), 0 none
thread_local uint64_t t_wseq = 0;                    // seq + 1 of the last W row this thread emitted
thread_local uint64_t t_wseq_mark = 0;               // t_wseq when the open B row began
thread_local uint32_t t_cb_ord = 0;                  // this thread's C rows
struct Pend {
    ID3D11DeviceContext* c;
    ID3D11Resource*      r;
    uint32_t             sub;
    uint32_t             type;
    const void*          data;
};
thread_local Pend t_pend[4];                         // Maps of the 1008-byte buffer waiting for their Unmap (the CbDump shape)
thread_local int  t_np = 0;

// ---------------------------------------------------------------------------------------------------------------------
// guarded reads
// ---------------------------------------------------------------------------------------------------------------------
bool rd_seh(void* dst, uint64_t src, size_t n) {
    if (src < 0x10000ull || src > 0x00007FFFFFFE0000ull) {
        memset(dst, 0, n);
        return false;
    }
    __try {
        memcpy(dst, (const void*)(uintptr_t)src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(dst, 0, n);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

inline bool cam_ok(uint64_t cam) { return cam != 0 && (cam & 15) == 0; }

inline void fill_hdr(Hdr* h, uint16_t type, uint16_t bytes, uint64_t seq) {
    h->type  = type;
    h->bytes = bytes;
    h->tid   = GetCurrentThreadId();
    h->seq   = seq;
    h->qpc   = qpc_now();
    h->frame = d3d11::frame_count();
}

void emit_c(ID3D11DeviceContext* c, ID3D11Resource* res, uint8_t src, uint8_t mt, const uint8_t* d, uint32_t len,
            uint32_t off) {
    uint64_t n = 0;
    CRow* r = (CRow*)g_c.reserve(&n);
    if (!r) return;
    fill_hdr(&r->h, kTC, (uint16_t)sizeof(CRow), n);
    r->buffer   = (uint64_t)(uintptr_t)res;
    r->ctx      = (uint64_t)(uintptr_t)c;
    r->kind     = (uint8_t)anatomy::context_kind(c);
    r->src      = src;
    r->map_type = mt;
    r->ord      = ++t_cb_ord;
    r->dst_off  = off;
    r->len      = len;
    uint8_t fl = 0;
    // A block is copied only when the write covers it whole: buffer byte X sits at d + (X - off).
    if (d && off <= 0x20u && off + len >= 0x60u) {
        if (rd_seh(r->m020, (uint64_t)(uintptr_t)(d + (0x20u - off)), 64)) fl |= 1u; else fl |= 4u;
    } else {
        memset(r->m020, 0, sizeof(r->m020));
    }
    if (d && off <= 0x270u && off + len >= 0x2B0u) {
        if (rd_seh(r->m270, (uint64_t)(uintptr_t)(d + (0x270u - off)), 64)) fl |= 2u; else fl |= 4u;
    } else {
        memset(r->m270, 0, sizeof(r->m270));
    }
    r->flags = fl;
    g_c.publish(n);
}

// ---------------------------------------------------------------------------------------------------------------------
// the init thread's state
// ---------------------------------------------------------------------------------------------------------------------
enum : int { kStOff = 0, kStWait = 1, kStLive = 2, kStDone = 3, kStFailed = 4 };
int               g_st = kStOff;
unsigned          g_wait_ticks = 0;
FILE*             g_file = nullptr;
std::wstring      g_path;
uint8_t*          g_stage = nullptr;
uint64_t          g_bytes = 0;
uint64_t          g_live_ms = 0;
uint64_t          g_frame_live = 0;
bool              g_cb_on = false;
bool              g_io_err = false;
std::atomic<int>  g_drain_busy{0};                   // the drain and the close are never run twice at once (init thread vs detach)

template <class R>
void write_chunk(R& r, uint16_t type, uint32_t cnt) {
    const uint64_t lost_now = r.lost.load(std::memory_order_relaxed);
    const uint32_t ld = (uint32_t)std::min<uint64_t>(lost_now - r.lost_reported, 0xFFFFFFFFull);
    if (!cnt && !ld) return;
    ChunkHdr ch{kChunkMagic, type, (uint16_t)R::bytes, cnt, ld};
    if (fwrite(&ch, sizeof(ch), 1, g_file) != 1) g_io_err = true;
    if (cnt && fwrite(g_stage, R::bytes, cnt, g_file) != cnt) g_io_err = true;
    r.lost_reported = lost_now;
    r.written += cnt;
    g_bytes += sizeof(ch) + (uint64_t)cnt * R::bytes;
}

template <class R>
void drain_ring(R& r, uint16_t type, bool final_drain) {
    const uint64_t h = r.head.load(std::memory_order_acquire);
    if (h - r.tail > (uint64_t)R::cap - 64) {          // lapped since the last drain: the oldest slots are gone
        const uint64_t nt = h - ((uint64_t)R::cap - 64);
        r.lost.fetch_add(nt - r.tail, std::memory_order_relaxed);
        r.tail = nt;
        r.stuck_ticks = 0;
    }
    const uint32_t per = (uint32_t)(kStageBytes / R::bytes);
    uint32_t cnt = 0;
    while (r.tail < h) {
        const uint32_t slot = (uint32_t)(r.tail & (R::cap - 1));
        const uint64_t p1 = r.pub[slot].load(std::memory_order_acquire);
        if (p1 == r.tail + 1) {
            memcpy(g_stage + (size_t)cnt * R::bytes, r.base + (size_t)slot * R::bytes, R::bytes);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (r.pub[slot].load(std::memory_order_relaxed) == p1) ++cnt;
            else r.lost.fetch_add(1, std::memory_order_relaxed);   // torn: a writer lapped it during the copy
            ++r.tail;
            r.stuck_ticks = 0;
        } else if (p1 > r.tail + 1) {
            r.lost.fetch_add(1, std::memory_order_relaxed);         // lapped: overwritten by a newer record
            ++r.tail;
            r.stuck_ticks = 0;
        } else {
            // reserved and not yet published: wait for it, at most two drains, then step over it
            if (final_drain || r.stuck_ticks >= 2) {
                r.lost.fetch_add(1, std::memory_order_relaxed);
                ++r.tail;
                r.stuck_ticks = 0;
                continue;
            }
            ++r.stuck_ticks;
            break;
        }
        if (cnt == per) {
            write_chunk(r, type, cnt);
            cnt = 0;
        }
    }
    write_chunk(r, type, cnt);
}

// false when another drain is running (the detach path while the init thread is inside one)
bool drain_all(bool final_drain) {
    if (g_drain_busy.exchange(1, std::memory_order_acquire) != 0) return false;
    if (g_file) {
        drain_ring(g_b, kTB, final_drain);
        drain_ring(g_w, kTW, final_drain);
        drain_ring(g_g, kTG, final_drain);
        drain_ring(g_c, kTC, final_drain);
        drain_ring(g_p, kTP, final_drain);
        if (fflush(g_file) != 0) g_io_err = true;
    }
    g_drain_busy.store(0, std::memory_order_release);
    return true;
}

float cfg_at(int i) {
    const uint32_t b = g_cfg_bits[i].load(std::memory_order_relaxed);
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}

void arm(bool cb, bool hooks_refused) {
    const size_t sb = (size_t)g_b.bytes * g_b.cap, sw = (size_t)g_w.bytes * g_w.cap, sg = (size_t)g_g.bytes * g_g.cap,
                 sc = (size_t)g_c.bytes * g_c.cap, sp = (size_t)g_p.bytes * g_p.cap;
    const size_t total = sb + sw + sg + sc + sp + kStageBytes;
    uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        g_st = kStFailed;
        LOG_ERROR("gtrec: could not allocate the %.1f MB of rings (error %lu). Nothing is recorded this run.",
                  (double)total / 1048576.0, GetLastError());
        return;
    }
    g_path = log::data_dir() + L"\\gtrec-" + std::to_wstring(GetCurrentProcessId()) + L".bin";
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_path.c_str(), L"wb") != 0 || !f) {
        VirtualFree(mem, 0, MEM_RELEASE);
        g_st = kStFailed;
        LOG_ERROR("gtrec: could not open %ls for writing. Nothing is recorded this run.", g_path.c_str());
        return;
    }
    setvbuf(f, nullptr, _IOFBF, 4u << 20);

    FileHdr hd;
    memset(&hd, 0, sizeof(hd));
    memcpy(hd.magic, "GRWGTR01", 8);
    hd.version   = kVersion;
    hd.hdr_bytes = (uint32_t)sizeof(FileHdr);
    hd.build     = kModBuild;
    hd.pid       = GetCurrentProcessId();
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    hd.qpc_freq   = (uint64_t)qf.QuadPart;
    hd.qpc0       = qpc_now();
    hd.frame0     = d3d11::frame_count();
    hd.image_base = (uint64_t)(uintptr_t)GetModuleHandleW(nullptr);
    hd.cam0       = cam2::first_camera();
    hd.cam2       = cam2::second_camera();
    hd.rec_bytes[kTB] = (uint32_t)sizeof(BRow);
    hd.rec_bytes[kTW] = (uint32_t)sizeof(WRow);
    hd.rec_bytes[kTG] = (uint32_t)sizeof(GRow);
    hd.rec_bytes[kTC] = (uint32_t)sizeof(CRow);
    hd.rec_bytes[kTP] = (uint32_t)sizeof(PRow);
    for (int i = 0; i < 16; ++i) hd.cfg[i] = cfg_at(i);
    hd.cfg[10] = headpose::fs_fov();                 // what the scene override carries now (stereo borrows it)
    hd.cfg[11] = cam2::stereo_target_fovy();         // set by the present thread from the headset's fovs
    hd.cfg[15] = cb ? 1.0f : 0.0f;
    hd.site_a_rva = kSiteARva;
    hd.site_b_rva = kSiteBRva;
    hd.flags      = (cb ? 1u : 0u) | (hooks_refused ? 2u : 0u);
    hd.cb_bytes   = kCbBytes;
    if (fwrite(&hd, sizeof(hd), 1, f) != 1) g_io_err = true;
    g_bytes = sizeof(hd);

    uint8_t* p = mem;
    g_b.base = p; p += sb;
    g_w.base = p; p += sw;
    g_g.base = p; p += sg;
    g_c.base = p; p += sc;
    g_p.base = p; p += sp;
    g_stage = p;
    g_file  = f;
    g_cb_on = cb;
    if (cb) anatomy::gate_set(4);
    g_live_ms = GetTickCount64();
    g_frame_live = d3d11::frame_count();
    g_live.store(1, std::memory_order_release);
    g_st = kStLive;
    LOG_INFO("gtrec: LIVE at Present #%llu, recording to %ls (rings %.1f MB, cap %d MB, %s). Constant-buffer taps: %s.",
             (unsigned long long)g_frame_live, g_path.c_str(), (double)total / 1048576.0,
             g_max_mb.load(std::memory_order_relaxed),
             g_seconds.load(std::memory_order_relaxed) > 0 ? "time limit set by gtrec_seconds" : "no time limit",
             cb ? "ON (Anatomy gate bit 4)"
                : (hooks_refused ? "OFF: the D3D11 hook set was refused, so no C rows (the picture cannot be resolved)"
                                 : "OFF (cfg gtrec_cb = 0): no C rows, so the picture of a frame cannot be resolved"));
}

void stop(const char* why) {
    g_live.store(0, std::memory_order_release);
    if (g_cb_on) anatomy::gate_clear(4);
    Sleep(100);   // init thread: a writer that passed on() just before the store finishes its slot
    drain_all(true);
    if (g_drain_busy.exchange(1, std::memory_order_acquire) == 0) {
        if (g_file) fclose(g_file);
        g_file = nullptr;
        g_drain_busy.store(0, std::memory_order_release);
    }
    g_st = kStDone;
    LOG_INFO("gtrec: STOPPED (%s). %.1f MB in %ls: rows B %llu W %llu G %llu C %llu P %llu, lost B %llu W %llu G %llu "
             "C %llu P %llu, guarded-read faults %u. One recording per run. Analyse with tools\\gtrec_analyze.py.",
             why, (double)g_bytes / 1048576.0, g_path.c_str(), (unsigned long long)g_b.written,
             (unsigned long long)g_w.written, (unsigned long long)g_g.written, (unsigned long long)g_c.written,
             (unsigned long long)g_p.written, (unsigned long long)g_b.lost.load(), (unsigned long long)g_w.lost.load(),
             (unsigned long long)g_g.lost.load(), (unsigned long long)g_c.lost.load(),
             (unsigned long long)g_p.lost.load(), g_faults.load(std::memory_order_relaxed));
}

template <class R>
uint64_t step_of(R& r) {
    const uint64_t d = r.written - r.written_last;
    r.written_last = r.written;
    return d;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// cfg
// ---------------------------------------------------------------------------------------------------------------------
void set_enabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
void set_cb(bool on) { g_cb.store(on, std::memory_order_relaxed); }
void set_max_mb(int mb) { g_max_mb.store(mb < 16 ? 16 : (mb > 20000 ? 20000 : mb), std::memory_order_relaxed); }
void set_seconds(int s) { g_seconds.store(s < 0 ? 0 : (s > 86400 ? 86400 : s), std::memory_order_relaxed); }
void set_cfg_snapshot(const float v[16]) {
    for (int i = 0; i < 16; ++i) {
        uint32_t b;
        memcpy(&b, &v[i], sizeof(b));
        g_cfg_bits[i].store(b, std::memory_order_relaxed);
    }
}
bool cb_wanted() { return g_enabled.load(std::memory_order_relaxed) && g_cb.load(std::memory_order_relaxed); }

uint64_t qpc_now() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (uint64_t)q.QuadPart;
}

bool copy64(float dst[16], uint64_t src) {
    if (!cam_ok(src)) {
        memset(dst, 0, 64);
        return false;
    }
    return rd_seh(dst, src, 64);
}

// ---------------------------------------------------------------------------------------------------------------------
// B
// ---------------------------------------------------------------------------------------------------------------------
BRow* b_begin(uint64_t cam, uint64_t other, int role, uint32_t build_seq, bool first, uint32_t site, uint32_t ret_rva,
              uint32_t seenA, uint32_t ctx_flags) {
    if (!cam_ok(cam)) return nullptr;
    uint64_t n = 0;
    BRow* r = (BRow*)g_b.reserve(&n);
    if (!r) return nullptr;
    fill_hdr(&r->h, kTB, (uint16_t)sizeof(BRow), n);
    r->cam            = cam;
    r->rc_id          = g_rc.fetch_add(1, std::memory_order_relaxed) + 1;
    t_rc              = r->rc_id;
    t_wseq_mark       = t_wseq;
    r->role           = (uint32_t)role;
    r->build_seq      = build_seq;
    r->first_in_build = first ? 1u : 0u;
    r->graph_seq      = g_graph.load(std::memory_order_relaxed);
    r->action         = kActNone;
    r->w_seq          = 0xFFFFFFFFu;
    r->pad0           = 0;
    r->site           = site;
    r->ret_rva        = ret_rva;
    r->seenA          = seenA;
    r->pad1           = 0;
    memset(r->reserved, 0, sizeof(r->reserved));
    memset(r->in1, 0, sizeof(r->in1));
    uint32_t fl = ctx_flags & 0x1E0u;                // bits 5..8 come from the caller
    bool fault = false;
    if (rd_seh(r->in0, cam + 0x000, 64)) fl |= 1u; else fault = true;
    if (rd_seh(r->d4A0, cam + 0x4A0, 64)) fl |= 4u; else fault = true;
    if (rd_seh(r->d5A0, cam + 0x5A0, 64)) fl |= 8u; else fault = true;
    if (cam_ok(other)) {
        if (rd_seh(r->o4A0, other + 0x4A0, 64)) fl |= 16u; else fault = true;
    } else {
        memset(r->o4A0, 0, sizeof(r->o4A0));
    }
    uint32_t mode = 0;
    uint64_t ovr = 0;
    if (!rd_seh(&mode, cam + 0x290, sizeof(mode))) fault = true;
    if (!rd_seh(&ovr, cam + 0x2A0, sizeof(ovr))) fault = true;
    if (!rd_seh(r->scal, cam + 0x2B0, 12)) fault = true;
    if (!rd_seh(&r->scal[3], cam + 0x7A4, 4)) fault = true;
    r->mode = mode;
    r->override_ptr = ovr;
    if (fault) fl |= 1u << 31;
    r->flags = fl;
    return r;   // published by b_end
}

void b_end_impl(BRow* r, uint32_t action) {
    r->action = action;
    if (t_wseq != t_wseq_mark) r->w_seq = (uint32_t)(t_wseq - 1);
    if (rd_seh(r->in1, r->cam, 64)) r->flags |= 2u;
    else                            r->flags |= 1u << 30;
    t_rc = 0;
    g_b.publish(r->h.seq);
}

// ---------------------------------------------------------------------------------------------------------------------
// W
// ---------------------------------------------------------------------------------------------------------------------
void w_emit(WRow& w) {
    uint64_t n = 0;
    uint8_t* p = g_w.reserve(&n);
    if (!p) return;
    fill_hdr(&w.h, kTW, (uint16_t)sizeof(WRow), n);
    w.rc_id = t_rc;
    if (w.kind == kWCompose) {
        w.ref_wseq = ~0ull;
        g_last_compose.store(n + 1, std::memory_order_relaxed);
    } else {
        const uint64_t lc = g_last_compose.load(std::memory_order_relaxed);
        w.ref_wseq = lc ? lc - 1 : ~0ull;
    }
    memcpy(p, &w, sizeof(WRow));
    g_w.publish(n);
    t_wseq = n + 1;
}

void w_mark(uint32_t kind, uint64_t cam0, uint64_t cam2) {
    WRow w;
    memset(&w, 0, sizeof(w));
    w.kind      = kind;
    w.build_seq = camera::build_seq_now();
    w.tag_eye   = -1;
    w.cam0      = cam0;
    w.cam2      = cam2;
    if (cam0) copy64(w.m0, cam0);
    if (cam2) copy64(w.m2, cam2);
    w_emit(w);
}

// ---------------------------------------------------------------------------------------------------------------------
// G
// ---------------------------------------------------------------------------------------------------------------------
void g_emit(uint64_t cam0, uint64_t cam2, uint32_t build_seq, int state) {
    uint64_t n = 0;
    GRow* r = (GRow*)g_g.reserve(&n);
    if (!r) return;
    fill_hdr(&r->h, kTG, (uint16_t)sizeof(GRow), n);
    r->cam0      = cam0;
    r->cam2      = cam2;
    r->graph_seq = g_graph.fetch_add(1, std::memory_order_relaxed) + 1;
    r->build_seq = build_seq;
    r->state     = (uint32_t)state;
    uint32_t fl = 0;
    if (cam_ok(cam0) && rd_seh(r->a0, cam0 + 0x4A0, 64)) fl |= 1u; else memset(r->a0, 0, sizeof(r->a0));
    if (cam_ok(cam2) && rd_seh(r->a2, cam2 + 0x4A0, 64)) fl |= 2u; else memset(r->a2, 0, sizeof(r->a2));
    r->flags = fl;
    g_g.publish(n);
}

// ---------------------------------------------------------------------------------------------------------------------
// C (Anatomy.cpp calls these only while its gate bit 4 is set)
// ---------------------------------------------------------------------------------------------------------------------
void tap_map(ID3D11DeviceContext* c, ID3D11Resource* res, uint32_t sub, uint32_t map_type, const void* data) {
    if (!data || !res || map_type == (uint32_t)D3D11_MAP_READ) return;
    if (anatomy::constant_buffer_bytes(res) != kCbBytes) return;
    if (t_np == 4) {
        memmove(&t_pend[0], &t_pend[1], 3 * sizeof(Pend));
        t_np = 3;
    }
    t_pend[t_np++] = {c, res, sub, map_type, data};
}

void tap_unmap(ID3D11DeviceContext* c, ID3D11Resource* res, uint32_t sub) {
    for (int i = t_np - 1; i >= 0; --i) {
        if (t_pend[i].c == c && t_pend[i].r == res && t_pend[i].sub == sub) {
            const Pend p = t_pend[i];
            for (int j = i; j + 1 < t_np; ++j) t_pend[j] = t_pend[j + 1];
            --t_np;
            emit_c(c, res, 1, (uint8_t)p.type, (const uint8_t*)p.data, kCbBytes, 0);
            return;
        }
    }
}

void tap_update(ID3D11DeviceContext* c, ID3D11Resource* res, uint32_t sub, const D3D11_BOX* box, const void* src) {
    (void)sub;
    if (!src || !res) return;
    if (anatomy::constant_buffer_bytes(res) != kCbBytes) return;
    uint32_t off = 0, len = kCbBytes;
    if (box) {
        if (box->right > box->left && box->left < kCbBytes) {
            off = box->left;
            len = std::min<uint32_t>(kCbBytes - off, box->right - box->left);
        } else {
            len = 0;
        }
    }
    emit_c(c, res, 2, 0, (const uint8_t*)src, len, off);
}

// ---------------------------------------------------------------------------------------------------------------------
// P (render thread only)
// ---------------------------------------------------------------------------------------------------------------------
void p_begin_impl(uint64_t frame, int built_eye, const float q[4], bool q_ok, uint32_t built_flags) {
    memset(&g_ps, 0, sizeof(g_ps));
    g_ps.h.type  = kTP;
    g_ps.h.bytes = (uint16_t)sizeof(PRow);
    g_ps.h.tid   = GetCurrentThreadId();
    g_ps.h.qpc   = qpc_now();
    g_ps.h.frame = frame;
    g_ps.rc_id     = g_rc.load(std::memory_order_relaxed);
    g_ps.build_seq = camera::build_seq_now();
    g_ps.graph_seq = g_graph.load(std::memory_order_relaxed);
    g_ps.built_eye   = built_eye;
    g_ps.built_flags = built_flags;
    if (q) memcpy(g_ps.built_q, q, sizeof(g_ps.built_q));
    if (q_ok) g_ps.fl |= 1u;
    g_ps.label_from[0] = g_ps.label_from[1] = 0xFFFFFFFFu;
    const uint64_t c0 = cam2::first_camera(), c2 = cam2::second_camera();
    if (cam_ok(c0)) {
        rd_seh(g_ps.c0_4A0, c0 + 0x4A0, 64);
        rd_seh(g_ps.c0_000, c0 + 0x000, 64);
    }
    if (cam_ok(c2)) {
        rd_seh(g_ps.c2_4A0, c2 + 0x4A0, 64);
        rd_seh(g_ps.c2_000, c2 + 0x000, 64);
    }
    g_ps_open = true;
}

void p_commit() {
    g_ps_open = false;
    uint64_t n = 0;
    uint8_t* p = g_p.reserve(&n);
    if (!p) return;
    g_ps.h.seq = n;
    memcpy(p, &g_ps, sizeof(PRow));
    g_p.publish(n);
}

// ---------------------------------------------------------------------------------------------------------------------
// init thread
// ---------------------------------------------------------------------------------------------------------------------
void tick() {
    const bool en = g_enabled.load(std::memory_order_relaxed);
    if (g_st == kStOff) {
        if (!en) return;
        g_st = kStWait;
        g_wait_ticks = 0;
        LOG_INFO("gtrec: ENABLED (cfg gtrec = 1; build 270, the ground-truth stereo recorder, log only). It goes live once "
                 "true stereo is live%s, and records to GRWVR\\gtrec-%lu.bin.",
                 g_cb.load(std::memory_order_relaxed) ? " and the D3D11 hook set exists" : "", GetCurrentProcessId());
    }
    if (g_st == kStWait) {
        if (!en) {
            g_st = kStOff;
            LOG_INFO("gtrec: disabled (cfg gtrec = 0) before it went live; nothing was recorded.");
            return;
        }
        ++g_wait_ticks;
        if (!cam2::stereo_live()) {
            if ((g_wait_ticks % 10) == 1)
                LOG_INFO("gtrec: waiting for live true stereo (cam2_experiment = 1, cam2_stereo = 1, camera 2 RUNNING).");
            return;
        }
        const bool want_cb = g_cb.load(std::memory_order_relaxed);
        const int  hs = anatomy::hooks_status();
        if (want_cb && hs == 0) {
            if ((g_wait_ticks % 5) == 1)
                LOG_INFO("gtrec: waiting for the D3D11 hook set (Anatomy installs it 20 s after the first stereo layer), so "
                         "that every recorded frame has its constant-buffer rows.");
            return;
        }
        arm(want_cb && hs == 1, want_cb && hs < 0);
        return;
    }
    if (g_st != kStLive) return;

    drain_all(false);
    uint32_t ctx_full = 0, cache_full = 0;
    anatomy::table_pressure(&ctx_full, &cache_full);
    const int max_mb = g_max_mb.load(std::memory_order_relaxed);
    LOG_INFO("gtrec: rows B/W/G/C/P +%llu/+%llu/+%llu/+%llu/+%llu | lost %llu/%llu/%llu/%llu/%llu | %.1f of %d MB | "
             "cb gate %s, hooks %d | anatomy tables ctx_full %u cache_full %u | read faults %u",
             (unsigned long long)step_of(g_b), (unsigned long long)step_of(g_w), (unsigned long long)step_of(g_g),
             (unsigned long long)step_of(g_c), (unsigned long long)step_of(g_p),
             (unsigned long long)g_b.lost.load(), (unsigned long long)g_w.lost.load(),
             (unsigned long long)g_g.lost.load(), (unsigned long long)g_c.lost.load(),
             (unsigned long long)g_p.lost.load(), (double)g_bytes / 1048576.0, max_mb,
             (anatomy::g_gate.load(std::memory_order_relaxed) & 4) ? "on" : "off", anatomy::hooks_status(), ctx_full,
             cache_full, g_faults.load(std::memory_order_relaxed));

    const int secs = g_seconds.load(std::memory_order_relaxed);
    if (!en)                                                   stop("cfg gtrec = 0");
    else if (g_io_err)                                         stop("a write to the file failed (disk full?)");
    else if (g_bytes >= ((uint64_t)max_mb << 20))             stop("the size cap, gtrec_max_mb");
    else if (secs > 0 && GetTickCount64() - g_live_ms >= (uint64_t)secs * 1000ull) stop("the time limit, gtrec_seconds");
}

void on_stall() {
    if (g_st == kStLive) drain_all(false);
}

void on_exit() {
    if (g_st != kStLive) return;
    g_live.store(0, std::memory_order_release);
    // DLL detach: the other threads are gone, so nothing is mid-write that will ever finish. A drain that is somehow
    // still marked busy is left alone (the CRT flushes the stream at its own termination).
    if (!drain_all(true)) return;
    if (g_drain_busy.exchange(1, std::memory_order_acquire) == 0) {
        if (g_file) fclose(g_file);
        g_file = nullptr;
        g_drain_busy.store(0, std::memory_order_release);
    }
    g_st = kStDone;
}

}  // namespace gtrec
}  // namespace grwxr
