// CbDump.cpp - BUILD 246, see CbDump.h. Observation only.
//
// Layout of this file: the preallocated tables, the taps and the Present sampler
// (hot path, rule 8), the state machine (init thread), the dump thread (file I/O and
// the analysis, off every hot path). Everything the hot paths touch is either a static
// table or an arena VirtualAlloc'ed by the init thread before the window opens, and is
// written with relaxed atomics; there is no lock, no allocation and no logging in a tap.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "Anatomy.h"
#include "CameraProbe.h"
#include "CbAnalysis.h"
#include "CbDump.h"
#include "CbDumpFormat.h"
#include "D3D11Hook.h"
#include "HeadPose.h"
#include "Log.h"

namespace grwxr {
namespace cbdump {

std::atomic<int> g_state{0};   // 0 idle, 2 capturing, 3 stopped, 4 dumping, 5 done, 6 failed

namespace {

constexpr uint64_t kArmMs    = 30000;   // the window opens this long after the first stereo layer (same as the anatomy window)
constexpr uint64_t kGiveUpMs = 18000;   // a window that has not filled by now is closed, so it can never reach the census at 50 s
constexpr uint32_t kCap[3]   = {24000, 150000, 14000};   // slots: 26 MB, 58 MB, 15 MB
constexpr size_t   kBufCap   = 2048;

std::atomic<bool>     g_enabled{false};
std::atomic<uint64_t> g_start_frame{0};
uint64_t g_arm_ms = 0, g_tsc0 = 0, g_qpc0 = 0;

struct ArenaS {
    uint8_t* base = nullptr;
    uint32_t cap = 0, rowcap = 0;
    std::atomic<uint32_t> next{0};
    std::atomic<uint32_t> rowuse[kGtRows];
    std::atomic<uint64_t> seen{0}, stored{0}, drop_budget{0}, drop_quota{0}, skipped{0};
};
ArenaS g_ar[3];

// --- per constant buffer: pointer-keyed, lock-free -------------------------
struct Buf {
    std::atomic<uint32_t> size, kmask, kept;
    std::atomic<uint32_t> seen_kind[4];
};
std::atomic<uintptr_t> g_bkey[kBufCap];
Buf g_buf[kBufCap];
std::atomic<uint32_t> g_ord[kBufCap][kGtRows];   // writes per buffer per row, EVERY write

std::atomic<uint64_t> g_faults{0}, g_buf_full{0}, g_outside{0};

inline uint32_t hp(uintptr_t p) {
    const uint64_t x = (uint64_t)(p >> 4) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(x >> 40) & (uint32_t)(kBufCap - 1);
}

int bslot(uintptr_t p) {
    const uint32_t h = hp(p);
    for (uint32_t i = 0; i < 32; ++i) {
        const uint32_t s = (h + i) & (uint32_t)(kBufCap - 1);
        const uintptr_t k = g_bkey[s].load(std::memory_order_relaxed);
        if (k == p) return (int)s;
        if (k == 0) {
            uintptr_t exp = 0;
            if (g_bkey[s].compare_exchange_strong(exp, p, std::memory_order_relaxed) || exp == p) return (int)s;
        }
    }
    return -1;
}

// --- ground truth, filled at each Present -----------------------------------
GtRow g_gt[kGtRows];

struct PRing {
    std::atomic<uint64_t> out, inv;
    std::atomic<uint32_t> nr, fr, fv, asp, seq;
};
PRing g_pr[64];
std::atomic<uint32_t> g_pn{0};

inline uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float    bitsf(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

inline int bucket() {
    const uint64_t f = d3d11::frame_count();
    const uint64_t s = g_start_frame.load(std::memory_order_relaxed);
    if (f < s) return -1;
    const uint64_t b = f - s;
    return b < (uint64_t)kFrames ? (int)b : -1;
}

// The tap copy: SEH only. The source is memory the D3D runtime or the game handed to the
// call that is in progress, so it is valid unless the engine is broken, and a fault is
// counted and survived.
bool safe_copy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The Present-time copy of engine memory the mod does not own: VirtualQuery first (a
// PAGE_GUARD page must not be touched, the probe would consume the guard), then SEH.
bool guarded_read(void* dst, uint64_t src, size_t n) {
    if (!src || !n || src < 0x10000) return false;
    const uint64_t end = src + n - 1;
    uint64_t p = src;
    while (true) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void*)p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const uint64_t reg_end = (uint64_t)mbi.BaseAddress + mbi.RegionSize - 1;
        if (reg_end >= end) break;
        p = reg_end + 1;
    }
    return safe_copy(dst, (const void*)src, n);
}

// One constant-buffer write. Map/Unmap passes the mapped pointer at Unmap, UpdateSubresource
// the source pointer; dstoff is the byte offset inside the buffer (UpdateSubresource box).
void capture(ID3D11DeviceContext* c, ID3D11Resource* res, uint32_t bsize, const void* data, uint32_t avail,
             uint32_t src, uint32_t maptype, uint32_t dstoff) {
    const int b = bucket();
    if (b < 0) { g_outside.fetch_add(1, std::memory_order_relaxed); return; }
    if (!data || !avail || !bsize) return;
    const int k = anatomy::context_kind(c);

    uint32_t ord = 0;
    const int bs = bslot((uintptr_t)res);
    if (bs < 0) {
        g_buf_full.fetch_add(1, std::memory_order_relaxed);
    } else {
        Buf& B = g_buf[bs];
        B.size.store(bsize, std::memory_order_relaxed);
        B.kmask.fetch_or(1u << k, std::memory_order_relaxed);
        B.seen_kind[k].fetch_add(1, std::memory_order_relaxed);
        ord = g_ord[bs][b].fetch_add(1, std::memory_order_relaxed);
    }

    int a;
    bool keep;
    if (k == kKindImm && (bsize == 1008 || bsize == 320)) {
        a = 0; keep = true;
    } else if (k == kKindDef && (bsize == 320 || bsize == 144)) {
        a = 1; keep = ord < 8 || (ord & 63) == 0;
    } else {
        a = 2; keep = ord < 2 || (ord & 127) == 0;
    }
    ArenaS& A = g_ar[a];
    if (!A.base) return;
    A.seen.fetch_add(1, std::memory_order_relaxed);
    if (!keep) { A.skipped.fetch_add(1, std::memory_order_relaxed); return; }
    if (A.rowuse[b].fetch_add(1, std::memory_order_relaxed) >= A.rowcap) {
        A.drop_quota.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t slot = A.next.fetch_add(1, std::memory_order_relaxed);
    if (slot >= A.cap) {
        A.drop_budget.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint8_t* p = A.base + (size_t)slot * (size_t)kSlotBytes[a];
    RecHdr* h = (RecHdr*)p;
    uint32_t len = std::min<uint32_t>(avail, (uint32_t)kPayload[a]) & ~3u;
    uint8_t flags = avail > len ? 2 : 0;
    if (len && !safe_copy(p + kHdrBytes, data, len)) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        len = 0;
        flags |= 1;
    }
    h->slot = slot;
    h->row = (uint32_t)b;
    h->tid = GetCurrentThreadId();
    h->buffer = (uint64_t)(uintptr_t)res;
    h->ctx = (uint64_t)(uintptr_t)c;
    h->tsc = __rdtsc();
    h->byte_size = bsize;
    h->ordinal = ord;
    h->dst_offset = dstoff;
    h->data_len = len;
    h->kind = (uint8_t)k;
    h->src = (uint8_t)src;
    h->map_type = (uint8_t)maptype;
    h->flags = flags;
    h->reserved = 0;
    std::atomic_thread_fence(std::memory_order_release);
    *(volatile uint32_t*)&h->magic = kRecMagic;   // LAST: the dump skips a slot whose magic is 0
    A.stored.fetch_add(1, std::memory_order_relaxed);
    if (bs >= 0) g_buf[bs].kept.fetch_add(1, std::memory_order_relaxed);
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
void tap_map(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, D3D11_MAP type,
             const D3D11_MAPPED_SUBRESOURCE* ms, HRESULT hr) {
    if (FAILED(hr) || !ms || !ms->pData || !res || type == D3D11_MAP_READ) return;
    if (bucket() < 0) return;
    const uint32_t size = anatomy::constant_buffer_bytes(res);
    if (!size) return;
    if (t_np == 8) { std::memmove(&t_pend[0], &t_pend[1], 7 * sizeof(Pend)); t_np = 7; }
    t_pend[t_np++] = {c, res, sub, (UINT)type, ms->pData, size};
}

void tap_unmap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub) {
    for (int i = t_np - 1; i >= 0; --i) {
        if (t_pend[i].c == c && t_pend[i].r == res && t_pend[i].sub == sub) {
            const Pend p = t_pend[i];
            for (int j = i; j + 1 < t_np; ++j) t_pend[j] = t_pend[j + 1];
            --t_np;
            capture(c, res, p.size, p.data, p.size, 1, p.type, 0);
            return;
        }
    }
}

void tap_update(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* src) {
    (void)sub;
    if (!src || !res || bucket() < 0) return;
    const uint32_t size = anatomy::constant_buffer_bytes(res);
    if (!size) return;
    uint32_t len = size, off = 0;
    if (box) {
        if (box->right > box->left && box->left < size) {
            off = box->left;
            len = std::min<uint32_t>(size - off, box->right - box->left);
        } else {
            len = 0;
        }
    }
    capture(c, res, size, src, len, 2, 0, off);
}

void note_proj(uint64_t out_proj, uint64_t out_inv, float near_p, float far_p, float fovy, float aspect) {
    const uint32_t i = g_pn.fetch_add(1, std::memory_order_relaxed);
    PRing& e = g_pr[i & 63];
    e.seq.store(0, std::memory_order_relaxed);
    e.out.store(out_proj, std::memory_order_relaxed);
    e.inv.store(out_inv, std::memory_order_relaxed);
    e.nr.store(fbits(near_p), std::memory_order_relaxed);
    e.fr.store(fbits(far_p), std::memory_order_relaxed);
    e.fv.store(fbits(fovy), std::memory_order_relaxed);
    e.asp.store(fbits(aspect), std::memory_order_relaxed);
    e.seq.store(i + 1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// state, cfg, the Present boundary
// ---------------------------------------------------------------------------
void set_enabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

namespace {

// Render thread, at the entry of Present #(start + b). Reads the camera object, which the
// engine threads write without telling us, so a row can be torn; the analysis reports lag
// and mismatch instead of trusting a single row.
void sample_gt(int b, unsigned long long frames) {
    GtRow& g = g_gt[b];
    g.row = (uint32_t)b;
    g.present_no = frames;
    g.tsc = __rdtsc();
    const uint64_t cam = camera::player_camera();
    g.cam = cam;
    uint32_t fl = 0;
    if (!cam) {
        fl |= kGtCamNull;
    } else {
        if (guarded_read(g.pose, cam, sizeof(g.pose))) fl |= kGtPose;
        if (guarded_read(g.slots, cam + kSlotBase, sizeof(g.slots))) fl |= kGtSlots;
        float sc[7];
        float ov = 0;
        if (guarded_read(sc, cam + 0x2B0, sizeof(sc)) && guarded_read(&ov, cam + 0x7A4, sizeof(ov))) {
            std::memcpy(g.scal, sc, sizeof(sc));
            g.scal[7] = ov;
            fl |= kGtScal;
        }
    }
    g.flags = fl;
    const float fv = headpose::read_fov(-1.0f);
    if (fv > 0.0f) { g.pub_fov = fv; g.pub_valid |= 1; }
    float n = 0, f = 0;
    if (headpose::read_clip(&n, &f)) { g.pub_near = n; g.pub_far = f; g.pub_valid |= 2; }

    const uint32_t total = g_pn.load(std::memory_order_acquire);
    g.proj_calls_total = total;
    int k = 0;
    for (uint32_t j = total; j > 0 && k < kProjCalls && total - j < 64; --j) {
        PRing& e = g_pr[(j - 1) & 63];
        if (e.seq.load(std::memory_order_acquire) != j) continue;
        ProjCall& pc = g.pc[k];
        pc.out_proj = e.out.load(std::memory_order_relaxed);
        pc.out_inv = e.inv.load(std::memory_order_relaxed);
        pc.near_ = bitsf(e.nr.load(std::memory_order_relaxed));
        pc.far_ = bitsf(e.fr.load(std::memory_order_relaxed));
        pc.fovy = bitsf(e.fv.load(std::memory_order_relaxed));
        pc.aspect = bitsf(e.asp.load(std::memory_order_relaxed));
        pc.seq = j;
        pc.valid = 1;
        if (guarded_read(pc.readback, pc.out_proj, sizeof(pc.readback))) pc.valid = 2;
        ++k;
    }
    std::atomic_thread_fence(std::memory_order_release);
    *(volatile uint32_t*)&g.magic = kGtMagic;
}

}  // namespace

void on_present(unsigned long long frames) {
    if (g_state.load(std::memory_order_relaxed) != 2) return;
    const uint64_t s = g_start_frame.load(std::memory_order_relaxed);
    if (frames < s) return;
    const uint64_t b = frames - s;
    if (b <= (uint64_t)kFrames) sample_gt((int)b, frames);
    if (b >= (uint64_t)kFrames) {
        int exp = 2;
        if (g_state.compare_exchange_strong(exp, 3, std::memory_order_release)) anatomy::gate_clear(2);
    }
}

namespace {

// ---------------------------------------------------------------------------
// the dump (its own thread; file I/O and analysis are allowed here and nowhere in a tap)
// ---------------------------------------------------------------------------
void pad16(FILE* f, uint64_t& pos) {
    static const uint8_t z[16] = {};
    const size_t r = (size_t)(pos & 15);
    if (r) { fwrite(z, 1, 16 - r, f); pos += 16 - r; }
}

bool write_all(FILE* f, uint64_t& pos, const void* p, size_t n) {
    if (!n) return true;
    if (fwrite(p, 1, n, f) != n) return false;
    pos += n;
    return true;
}

// SEH shell around the analysis: the dump thread must never take the game down. No C++ objects
// with destructors live in this function.
bool analyze_guarded(const an::Input* in, an::Text* out) {
    __try {
        an::analyze(*in, *out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool analyze_cpp(const an::Input* in, an::Text* out, std::string* why) {
    try {
        return analyze_guarded(in, out);
    } catch (const std::exception& e) {
        *why = e.what();
        return false;
    } catch (...) {
        *why = "unknown C++ exception";
        return false;
    }
}

DWORD WINAPI dump_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Sleep(1000);   // let any tap that passed the gate before the stop finish writing

    LARGE_INTEGER q1, qf;
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    const uint64_t t1 = __rdtsc();
    const double secs = (double)((uint64_t)q1.QuadPart - g_qpc0) / (double)qf.QuadPart;
    const double tsc_hz = secs > 0.1 ? (double)(t1 - g_tsc0) / secs : 3.0e9;

    const DWORD pid = GetCurrentProcessId();
    const std::wstring base = log::data_dir() + L"\\cbdump-" + std::to_wstring(pid);

    std::vector<BufMetaRec> bufs;
    for (size_t i = 0; i < kBufCap; ++i) {
        const uintptr_t key = g_bkey[i].load(std::memory_order_relaxed);
        if (!key) continue;
        BufMetaRec m;
        std::memset(&m, 0, sizeof(m));
        m.ptr = (uint64_t)key;
        m.size = g_buf[i].size.load();
        m.kmask = g_buf[i].kmask.load();
        for (int k = 0; k < 4; ++k) m.seen_kind[k] = g_buf[i].seen_kind[k].load();
        m.kept = g_buf[i].kept.load();
        for (int r = 0; r < kGtRows; ++r) m.seen_row[r] = (uint16_t)std::min<uint32_t>(g_ord[i][r].load(), 65535u);
        bufs.push_back(m);
    }

    FileHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    std::memcpy(hdr.magic, "GRWCBD01", 8);
    hdr.version = 1;
    hdr.header_bytes = sizeof(FileHeader);
    hdr.build = kBuild;
    hdr.pid = (uint32_t)pid;
    hdr.frames = kFrames;
    hdr.gt_rows = kGtRows;
    hdr.n_bufs = (uint32_t)bufs.size();
    hdr.gt_row_bytes = sizeof(GtRow);
    hdr.buf_rec_bytes = sizeof(BufMetaRec);
    hdr.rec_hdr_bytes = kHdrBytes;
    hdr.start_present = g_start_frame.load();
    hdr.tsc_hz = tsc_hz;
    uint64_t incomplete = 0;
    for (int a = 0; a < 3; ++a) {
        hdr.slot_bytes[a] = (uint32_t)kSlotBytes[a];
        hdr.payload[a] = (uint32_t)kPayload[a];
        hdr.cap_slots[a] = g_ar[a].cap;
        hdr.n_slots[a] = std::min<uint32_t>(g_ar[a].next.load(), g_ar[a].cap);
        hdr.seen[a] = g_ar[a].seen.load();
        hdr.stored[a] = g_ar[a].stored.load();
        hdr.dropped_budget[a] = g_ar[a].drop_budget.load();
        hdr.dropped_quota[a] = g_ar[a].drop_quota.load();
        hdr.skipped_rule[a] = g_ar[a].skipped.load();
        for (uint32_t s = 0; s < hdr.n_slots[a]; ++s)
            if (((const RecHdr*)(g_ar[a].base + (size_t)s * kSlotBytes[a]))->magic != kRecMagic) ++incomplete;
    }
    hdr.faults = g_faults.load();
    hdr.buf_full = g_buf_full.load();
    hdr.outside_window = g_outside.load();
    hdr.incomplete_slots = incomplete;
    {
        uint32_t cf = 0, kf = 0;
        anatomy::table_pressure(&cf, &kf);
        hdr.ctx_full = cf;
        hdr.cache_full = kf;
    }

    // ---- the binary file ----
    const std::wstring binpath = base + L".bin";
    bool bin_ok = false;
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, binpath.c_str(), L"wb") == 0 && f) {
            static char fbuf[1 << 16];
            setvbuf(f, fbuf, _IOFBF, sizeof(fbuf));
            uint64_t pos = 0;
            bool ok = true;
            hdr.off_gt = sizeof(FileHeader);
            hdr.off_buf = (hdr.off_gt + (uint64_t)kGtRows * sizeof(GtRow) + 15) & ~15ull;
            uint64_t o = hdr.off_buf + (uint64_t)bufs.size() * sizeof(BufMetaRec);
            for (int a = 0; a < 3; ++a) {
                o = (o + 15) & ~15ull;
                hdr.off_arena[a] = o;
                o += (uint64_t)hdr.n_slots[a] * hdr.slot_bytes[a];
            }
            ok &= write_all(f, pos, &hdr, sizeof(hdr));
            ok &= write_all(f, pos, g_gt, sizeof(g_gt));
            pad16(f, pos);
            ok &= write_all(f, pos, bufs.data(), bufs.size() * sizeof(BufMetaRec));
            for (int a = 0; a < 3 && ok; ++a) {
                pad16(f, pos);
                ok &= write_all(f, pos, g_ar[a].base, (size_t)hdr.n_slots[a] * hdr.slot_bytes[a]);
            }
            fclose(f);
            bin_ok = ok;
        }
        if (!bin_ok) LOG_ERROR("cbdump: could not write %ls (err %lu).", binpath.c_str(), GetLastError());
    }

    // ---- the analysis ----
    an::Input in;
    in.hdr = &hdr;
    in.gt = g_gt;
    in.gt_rows = kGtRows;
    in.bufs = bufs.data();
    in.n_bufs = (int)bufs.size();
    for (int a = 0; a < 3; ++a) {
        in.arena[a].base = g_ar[a].base;
        in.arena[a].slot_bytes = hdr.slot_bytes[a];
        in.arena[a].payload = hdr.payload[a];
        in.arena[a].n = hdr.n_slots[a];
    }
    an::Text text;
    text.f("FILES: GRWVR\\cbdump-%lu.txt is this summary; GRWVR\\cbdump-%lu.bin holds the raw records (binary%s). Layout, little endian: "
           "512-byte FileHeader, %d ground-truth rows of %zu bytes, %zu buffer records of %zu bytes, then three arenas of fixed slots "
           "(arena I %u bytes per slot, D %u, M %u), each slot a 64-byte RecHdr then the captured bytes; every offset and size is in the "
           "FileHeader and the structs are in src\\CbDumpFormat.h.\n",
           (unsigned long)pid, (unsigned long)pid, bin_ok ? "" : ", NOT WRITTEN", kGtRows, sizeof(GtRow), bufs.size(), sizeof(BufMetaRec),
           hdr.slot_bytes[0], hdr.slot_bytes[1], hdr.slot_bytes[2]);
    text.f("READ ORDER: the VERDICT at the very end first; then BUDGET AND DROPS and GROUND TRUTH; then the PER-OFFSET BLOCK MAP and the field "
           "maps; the WVP test section explains its own method.\n\n");
    std::string why;
    const bool ok = analyze_cpp(&in, &text, &why);
    if (!ok) {
        text.f("\n*** THE IN-PROCESS ANALYSIS DID NOT FINISH (%s). Everything above this line is real; the VERDICT could not be produced. "
               "The .bin has the raw data for an offline analysis. ***\n", why.empty() ? "access violation or fault" : why.c_str());
        LOG_ERROR("cbdump: the analysis did not finish (%s). The .bin is complete.", why.empty() ? "fault" : why.c_str());
    }
    const std::wstring txtpath = base + L".txt";
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, txtpath.c_str(), L"wb") == 0 && f) {
            fwrite(text.s.data(), 1, text.s.size(), f);
            fclose(f);
        } else {
            LOG_ERROR("cbdump: could not write %ls (err %lu).", txtpath.c_str(), GetLastError());
        }
    }
    LOG_INFO("cbdump: wrote %ls and %ls (%u+%u+%u records stored, dropped for budget %llu, quota %llu). Read the VERDICT at the end of the .txt.",
             txtpath.c_str(), binpath.c_str(), hdr.n_slots[0], hdr.n_slots[1], hdr.n_slots[2],
             (unsigned long long)(hdr.dropped_budget[0] + hdr.dropped_budget[1] + hdr.dropped_budget[2]),
             (unsigned long long)(hdr.dropped_quota[0] + hdr.dropped_quota[1] + hdr.dropped_quota[2]));

    // The arenas stay committed (a tap that was preempted past the gate could still write into
    // them); the physical pages go back to the system.
    for (int a = 0; a < 3; ++a)
        if (g_ar[a].base) VirtualAlloc(g_ar[a].base, (size_t)g_ar[a].cap * kSlotBytes[a], MEM_RESET, PAGE_READWRITE);
    g_state.store(5, std::memory_order_release);
    return 0;
}

bool alloc_storage() {
    for (int a = 0; a < 3; ++a) {
        const size_t bytes = (size_t)kCap[a] * (size_t)kSlotBytes[a];
        void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            for (int b = 0; b < a; ++b) { VirtualFree(g_ar[b].base, 0, MEM_RELEASE); g_ar[b].base = nullptr; }
            return false;
        }
        g_ar[a].cap = kCap[a];
        g_ar[a].rowcap = (uint32_t)((uint64_t)kCap[a] * 3 / 2 / kFrames) + 1;
        g_ar[a].base = (uint8_t*)p;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// the state machine (init thread, once a second)
// ---------------------------------------------------------------------------
void tick() {
    int st = g_state.load(std::memory_order_acquire);
    if (st >= 4 || !enabled()) return;
    const uint64_t first = anatomy::first_stereo_ms();
    if (!first) return;
    const uint64_t now = GetTickCount64();
    const uint64_t age = now - first;

    if (st == 0) {
        const int hs = anatomy::hooks_status();
        if (hs < 0) {
            g_state.store(6, std::memory_order_release);
            LOG_ERROR("cbdump: the anatomy hook set was refused (nothing written, see the anatomy: lines). No capture this run.");
            return;
        }
        if (hs == 0 || age < kArmMs) return;
        if (!alloc_storage()) {
            g_state.store(6, std::memory_order_release);
            LOG_ERROR("cbdump: could not allocate the capture arenas (%u + %u + %u slots). No capture this run.", kCap[0], kCap[1], kCap[2]);
            return;
        }
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_qpc0 = (uint64_t)q.QuadPart;
        g_tsc0 = __rdtsc();
        g_arm_ms = now;
        g_pn.store(0, std::memory_order_relaxed);
        g_start_frame.store(d3d11::frame_count(), std::memory_order_relaxed);
        g_state.store(2, std::memory_order_release);
        anatomy::gate_set(2);
        LOG_INFO("cbdump: window OPEN at Present #%llu, capturing %d presents (%u + %u + %u slots).",
                 (unsigned long long)d3d11::frame_count(), kFrames, kCap[0], kCap[1], kCap[2]);
        return;
    }
    if (st == 2 && now - g_arm_ms > kGiveUpMs) {
        int exp = 2;
        if (g_state.compare_exchange_strong(exp, 3)) {
            anatomy::gate_clear(2);
            LOG_WARN("cbdump: the window did not fill in %llu s; closing it with what it has.", (unsigned long long)(kGiveUpMs / 1000));
        }
        st = g_state.load();
    }
    if (st == 3) {
        int exp = 3;
        if (g_state.compare_exchange_strong(exp, 4)) {
            LOG_INFO("cbdump: window CLOSED, dumping.");
            if (HANDLE t = CreateThread(nullptr, 0, dump_thread, nullptr, 0, nullptr)) CloseHandle(t);
            else g_state.store(5, std::memory_order_release);
        }
    }
}

}  // namespace cbdump
}  // namespace grwxr
