// CbAnalysis.h - BUILD 246: the in-process analysis of a constant-buffer content
// capture (CbDump.h). Pure C++ over plain memory (CbDumpFormat.h), no Windows
// and no D3D, so the offline harness compiles the very same code against
// synthetic captures. It runs on the DUMP thread after the window closed, never
// in a hook. It allocates freely; the hooks never call it.
//
// WHAT IT ANSWERS, in the order the report prints it:
//   1. what was captured and what was dropped (budgets);
//   2. the ground truth the mod read from the camera object at every Present;
//   3. for every 4x4 float block (16-byte aligned, which includes every 64-byte
//      aligned one) in every captured write: how often it repeats, in how many
//      buffers, and whether it is (A) a rigid transform, (B) a perspective
//      projection, (C) the camera pose or its inverse or transpose, (D) a
//      view*projection or projection*view built from the ground truth, (G) one of
//      the matrices the engine itself stores in the camera object (Camera+0x420..)
//      or the proj builder's output, (O) something else, (I) the identity;
//   4. a field map (which 16-byte rows change within a frame, between frames, or
//      never) for the immediate-context buffers of 1008 and 320 bytes and the
//      deferred ones of 320 and 144;
//   5. the precomputed-WVP test on the per-object buffers (B == W*S, and the
//      derived-world variant W = B * inverse(S) being a rigid transform);
//   6. a plain-language VERDICT.
//
// LIMITS (also printed): matching is by float comparison with tolerances, so a
// matrix that went through a different arithmetic path (jitter, a different
// handedness, a reduced precision copy) is reported as NOT matched; a "NO" for
// the WVP test only covers the sampled writes and the shared candidates that were
// captured; the ground truth is read at Present time from an unsynchronised
// object, so a frame whose camera moved between the read and the draw has a lag
// the table reports instead of hiding.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "CbDumpFormat.h"

namespace grwxr {
namespace cbdump {
namespace an {

struct Text {
    std::string s;
    void f(const char* fmt, ...) {
        char b[4096];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if ((size_t)n >= sizeof(b)) n = (int)sizeof(b) - 1;
        s.append(b, (size_t)n);
    }
};

struct Arena {
    const uint8_t* base = nullptr;
    uint32_t slot_bytes = 0, payload = 0, n = 0;
};

struct Input {
    const FileHeader*  hdr = nullptr;
    const GtRow*       gt = nullptr;
    int                gt_rows = 0;
    const BufMetaRec*  bufs = nullptr;
    int                n_bufs = 0;
    Arena              arena[3];
};

// ---------------------------------------------------------------------------
// small matrix algebra, row-major, doubles
// ---------------------------------------------------------------------------
struct Mat { double v[16]; };

inline Mat from_f(const float* f) { Mat m; for (int i = 0; i < 16; ++i) m.v[i] = f[i]; return m; }

inline Mat mtr(const Mat& a) {
    Mat r;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r.v[i * 4 + j] = a.v[j * 4 + i];
    return r;
}

// r = a*b, ab = sum |a||b| (the rounding scale of every element of r)
inline void mmul2(const Mat& a, const Mat& b, Mat& r, Mat& ab) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0, t = 0;
            for (int k = 0; k < 4; ++k) {
                const double x = a.v[i * 4 + k], y = b.v[k * 4 + j];
                s += x * y;
                t += std::fabs(x) * std::fabs(y);
            }
            r.v[i * 4 + j] = s;
            ab.v[i * 4 + j] = t;
        }
}
inline Mat mmul(const Mat& a, const Mat& b) { Mat r, t; mmul2(a, b, r, t); return r; }

inline bool minv(const Mat& a, Mat& out) {
    double m[4][8];
    double scale = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            m[i][j] = a.v[i * 4 + j];
            m[i][4 + j] = (i == j) ? 1.0 : 0.0;
            scale = std::max(scale, std::fabs(a.v[i * 4 + j]));
        }
    if (!(scale > 0)) return false;
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (std::fabs(m[p][c]) < 1e-12 * scale) return false;
        if (p != c) for (int j = 0; j < 8; ++j) std::swap(m[p][j], m[c][j]);
        const double d = m[c][c];
        for (int j = 0; j < 8; ++j) m[c][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = m[r][c];
            if (f == 0) continue;
            for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j];
        }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out.v[i * 4 + j] = m[i][4 + j];
    for (int i = 0; i < 16; ++i) if (!std::isfinite(out.v[i])) return false;
    return true;
}

inline bool eq_rel(const float* a, const float* b, double rel) {
    for (int i = 0; i < 16; ++i) {
        const double x = a[i], y = b[i];
        const double m = std::max(1.0, std::max(std::fabs(x), std::fabs(y)));
        if (std::fabs(x - y) > rel * m) return false;
    }
    return true;
}

// p (optionally transposed) against the stored block b, with the rounding scale ap.
inline bool eq_prod(const Mat& p, const Mat& ap, const float* b, bool transposed) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            const int pi = transposed ? j * 4 + i : i * 4 + j;
            const double tol = std::min(0.02, 2e-6 * ap.v[pi]) + 1e-5;
            if (std::fabs(p.v[pi] - (double)b[i * 4 + j]) > tol) return false;
        }
    return true;
}

// ---------------------------------------------------------------------------
// shape tests
// ---------------------------------------------------------------------------
inline bool ortho3(const double* m, double tol, bool unit) {
    double n[3];
    for (int i = 0; i < 3; ++i)
        n[i] = std::sqrt(m[4 * i] * m[4 * i] + m[4 * i + 1] * m[4 * i + 1] + m[4 * i + 2] * m[4 * i + 2]);
    if (unit) {
        for (int i = 0; i < 3; ++i) if (!(std::fabs(n[i] - 1.0) < tol)) return false;
    } else {
        if (!(n[0] > 1e-3 && n[0] < 1e3)) return false;
        for (int i = 1; i < 3; ++i) if (!(std::fabs(n[i] / n[0] - 1.0) < tol)) return false;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j) {
            const double d = m[4 * i] * m[4 * j] + m[4 * i + 1] * m[4 * j + 1] + m[4 * i + 2] * m[4 * j + 2];
            if (!(std::fabs(d) <= tol * n[i] * n[j])) return false;
        }
    return true;
}
inline bool last_col_h(const double* m, double tol) {
    return std::fabs(m[3]) < tol && std::fabs(m[7]) < tol && std::fabs(m[11]) < tol && std::fabs(m[15] - 1.0) < tol;
}
inline bool last_row_h(const double* m, double tol) {
    return std::fabs(m[12]) < tol && std::fabs(m[13]) < tol && std::fabs(m[14]) < tol && std::fabs(m[15] - 1.0) < tol;
}
// (a) orthonormal 3x3 and last row or column (0,0,0,1)
inline bool is_rigid(const double* m, double tol = 1e-3) {
    return ortho3(m, tol, true) && (last_col_h(m, tol) || last_row_h(m, tol));
}
// (b) zeros pattern with (0,0,+-1,0) in the last column (row vectors) or the last row (column vectors)
inline bool is_persp(const double* m, double tol = 1e-3) {
    if (!(std::fabs(m[0]) > 1e-6 && std::fabs(m[5]) > 1e-6)) return false;
    if (!(std::fabs(m[1]) < tol && std::fabs(m[4]) < tol)) return false;
    const bool col = std::fabs(m[3]) < tol && std::fabs(m[7]) < tol && std::fabs(std::fabs(m[11]) - 1.0) < tol && std::fabs(m[15]) < tol;
    const bool row = std::fabs(m[12]) < tol && std::fabs(m[13]) < tol && std::fabs(std::fabs(m[14]) - 1.0) < tol && std::fabs(m[15]) < tol;
    return col || row;
}
inline bool is_identity(const double* m, double tol = 1e-4) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j)
        if (!(std::fabs(m[i * 4 + j] - (i == j ? 1.0 : 0.0)) < tol)) return false;
    return true;
}
// a block worth looking at: finite, bounded, not all zero
inline bool block_usable(const float* f) {
    bool nz = false;
    for (int i = 0; i < 16; ++i) {
        const float x = f[i];
        if (!(std::fabs(x) < 1e7f)) return false;   // also rejects NaN and inf
        if (std::fabs(x) > 1e-9f) nz = true;
    }
    return nz;
}
// a block with at most 4 near-zero entries: a real view*projection or any composed matrix is dense, a block that
// straddles two fields (an identity row next to a matrix row, say) is not
inline bool dense_block(const float* f) {
    int z = 0;
    for (int i = 0; i < 16; ++i) if (std::fabs(f[i]) < 1e-5f) ++z;
    return z <= 4;
}
inline uint64_t hash64(const void* p) {
    uint64_t w[8];
    std::memcpy(w, p, 64);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 8; ++i) { h ^= w[i]; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 32; }
    return h;
}
inline uint64_t hash16(const void* p) {
    uint64_t w[2];
    std::memcpy(w, p, 16);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 2; ++i) { h ^= w[i]; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 32; }
    return h;
}

enum : uint8_t { kA = 1, kB = 2, kC = 4, kD = 8, kG = 16, kO = 32, kI = 64 };
inline std::string mark_str(uint8_t mk) {
    std::string s;
    if (mk & kA) s += "A";
    if (mk & kB) s += "B";
    if (mk & kC) s += "C";
    if (mk & kD) s += "D";
    if (mk & kG) s += "G";
    if (mk & kI) s += "I";
    if (mk & kO) s += "O";
    return s.empty() ? "-" : s;
}
inline int mark_bit(int i) { return 1 << i; }   // cell arrays are indexed 0..6 for A B C D G O I

// ---------------------------------------------------------------------------
// the ground truth set
// ---------------------------------------------------------------------------
struct GtEnt { float m[16]; int name; int row; };

struct GtSet {
    std::vector<std::string> names;
    std::vector<uint8_t> cat;           // kC, kD or kG
    std::vector<GtEnt> ents;            // sorted by m[0] after finish()
    int name(const std::string& n, uint8_t c) {
        for (size_t i = 0; i < names.size(); ++i) if (names[i] == n) return (int)i;
        names.push_back(n);
        cat.push_back(c);
        return (int)names.size() - 1;
    }
    void add(const Mat& m, int nm, int row) {
        GtEnt e;
        for (int i = 0; i < 16; ++i) {
            const float f = (float)m.v[i];
            if (!std::isfinite(f) || std::fabs(f) > 1e7f) return;
            e.m[i] = f;
        }
        e.name = nm;
        e.row = row;
        ents.push_back(e);
    }
    void add_t(const Mat& m, const std::string& n, uint8_t c, int row) {   // the matrix and its transpose
        add(m, name(n, c), row);
        add(mtr(m), name(n + "^T", c), row);
    }
    void finish() {
        std::sort(ents.begin(), ents.end(), [](const GtEnt& a, const GtEnt& b) { return a.m[0] < b.m[0]; });
    }
    int match(const float* f, const GtEnt** out, int maxn) const {
        const double w = 2.1e-4 * std::max(1.0, std::fabs((double)f[0])) + 1e-9;
        size_t lo = 0, hi = ents.size();
        const double key = (double)f[0] - w;
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if ((double)ents[mid].m[0] < key) lo = mid + 1; else hi = mid;
        }
        int n = 0;
        for (size_t i = lo; i < ents.size() && (double)ents[i].m[0] <= (double)f[0] + w; ++i)
            if (eq_rel(f, ents[i].m, 1e-4)) {
                out[n++] = &ents[i];
                if (n == maxn) break;
            }
        return n;
    }
};

inline Mat synth_proj(double fovy, double nr, double fr, double asp, bool lh) {
    Mat m;
    for (int i = 0; i < 16; ++i) m.v[i] = 0;
    const double t = std::tan(fovy * 0.5);
    m.v[0] = 1.0 / (asp * t);
    m.v[5] = 1.0 / t;
    if (!lh) {
        m.v[10] = fr / (nr - fr);
        m.v[11] = -1.0;
        m.v[14] = nr * fr / (nr - fr);
    } else {
        m.v[10] = fr / (fr - nr);
        m.v[11] = 1.0;
        m.v[14] = -nr * fr / (fr - nr);
    }
    return m;
}

inline bool finite_block16(const float* f) {
    for (int i = 0; i < 16; ++i) if (!(std::fabs(f[i]) < 1e7f)) return false;
    return true;
}

inline void build_gt(const Input& in, GtSet& gt, std::vector<int>* slot_persp_rows) {
    for (int r = 0; r < in.gt_rows; ++r) {
        const GtRow& g = in.gt[r];
        if (g.magic != kGtMagic) continue;
        const int row = (int)g.row;
        Mat V;
        bool have_v = false;
        if ((g.flags & kGtPose) && finite_block16(g.pose) && block_usable(g.pose)) {
            const Mat P = from_f(g.pose);
            gt.add_t(P, "pose", kC, row);
            if (minv(P, V)) {
                have_v = true;
                gt.add_t(V, "pose^-1", kC, row);
            }
        }
        struct PC { Mat m; std::string nm; };
        std::vector<PC> pers;
        if (g.flags & kGtSlots) {
            for (int k = 0; k < kNumSlots; ++k) {
                if (!finite_block16(g.slots[k]) || !block_usable(g.slots[k])) continue;
                if (is_identity(from_f(g.slots[k]).v)) continue;   // an unused slot says nothing and would match every identity block
                char nm[40];
                std::snprintf(nm, sizeof(nm), "cam+0x%X", (unsigned)(kSlotBase + 0x40u * (unsigned)k));
                const Mat S = from_f(g.slots[k]);
                gt.add_t(S, nm, kG, row);
                if (is_persp(S.v)) {
                    pers.push_back({S, nm});
                    if (slot_persp_rows) (*slot_persp_rows)[k]++;
                }
            }
        }
        double asp_guess = 0;
        for (int i = 0; i < kProjCalls; ++i) {
            const ProjCall& pc = g.pc[i];
            if (pc.valid >= 2 && finite_block16(pc.readback) && block_usable(pc.readback) && !is_identity(from_f(pc.readback).v)) {
                const Mat R = from_f(pc.readback);
                gt.add_t(R, "projcall.readback", kG, row);
                if (is_persp(R.v)) pers.push_back({R, "projcall.readback"});
            }
        }
        for (const PC& p : pers)
            if (p.m.v[0] != 0 && std::fabs(p.m.v[5] / p.m.v[0]) > 0.1 && std::fabs(p.m.v[5] / p.m.v[0]) < 10) {
                asp_guess = std::fabs(p.m.v[5] / p.m.v[0]);
                break;
            }
        for (int i = 0; i < kProjCalls; ++i) {
            const ProjCall& pc = g.pc[i];
            if (!pc.valid) continue;
            const double fovy = pc.fovy, nr = pc.near_, fr = pc.far_;
            double asp = pc.aspect;
            if (!(asp > 0.1 && asp < 10)) asp = asp_guess;
            if (!(fovy > 0.01 && fovy < 3.1 && nr > 0 && fr > 0 && nr != fr && asp > 0.1 && asp < 10)) continue;
            for (int lh = 0; lh < 2; ++lh) {
                const Mat S = synth_proj(fovy, nr, fr, asp, lh != 0);
                const std::string nm = lh ? "synthP(LH)" : "synthP(RH)";
                gt.add_t(S, nm, kG, row);
                pers.push_back({S, nm});
            }
            break;   // the newest valid call is enough
        }
        if (have_v) {
            for (const PC& p : pers) {
                gt.add_t(mmul(V, p.m), "V*" + p.nm, kD, row);
                gt.add_t(mmul(p.m, V), p.nm + "*V", kD, row);
            }
        }
    }
    gt.finish();
}

// ---------------------------------------------------------------------------
// the analyzer
// ---------------------------------------------------------------------------
struct RecV { const RecHdr* h; const uint8_t* d; uint16_t cls; uint16_t buf; uint8_t arena; };
struct Cls { uint8_t kind = 0; uint32_t size = 0; uint32_t nrec = 0; uint32_t nbuf = 0; uint64_t seen = 0; };
struct Obs { uint64_t h; uint32_t rec; uint16_t off; uint8_t row; uint8_t cls; };
struct Cell {
    uint32_t writes = 0, distinct = 0, maxshare = 0;
    uint32_t markw[7] = {0, 0, 0, 0, 0, 0, 0};
    uint32_t markd[7] = {0, 0, 0, 0, 0, 0, 0};
    int16_t gtn[3] = {-1, -1, -1};
};
struct Loc { uint8_t cls; uint16_t off; uint32_t cnt; };
struct Group {
    uint64_t h; uint32_t n; uint32_t rec0; uint16_t off0; uint8_t mark; uint8_t al64;
    uint16_t nbuf, nrows, maxrow; int16_t gtn[2]; int16_t gtrow[2]; int16_t lag; uint8_t nloc; Loc loc[4];
};
struct SEnt { const float* p; Mat m; Mat inv; bool invok; uint32_t cnt; };

inline const char* kind_name(int k) { return k == 0 ? "imm" : (k == 1 ? "def" : (k == 3 ? "rpl" : "unk")); }

class Analyzer {
public:
    Analyzer(const Input& in, Text& t) : in_(in), t_(t) {}
    void run();

private:
    const Input& in_;
    Text& t_;
    GtSet gt_;
    std::vector<RecV> recs_;
    std::vector<Cls> cls_;
    std::unordered_map<uint64_t, int> cls_idx_;
    std::vector<uint64_t> bufptr_;
    std::unordered_map<uint64_t, int> buf_idx_;
    std::unordered_map<uint64_t, int> meta_idx_;
    std::vector<Obs> obs_;
    bool obs_truncated_ = false;
    uint64_t bad_recs_ = 0, skipped_misaligned_ = 0;
    int rows_ = 0;                           // rows that have ground truth or data
    std::unordered_map<uint32_t, Cell> cells_;
    std::vector<Group> keep_;
    uint64_t n_groups_ = 0, n_uniq_ = 0, n_rep_ = 0, n_obs_al64_ = 0, n_groups_al64_rep_ = 0;
    uint64_t lag_hist_[3][9] = {};
    std::vector<SEnt> S_[kGtRows];
    std::vector<int> slot_persp_rows_;
    int nrows_gt_ = 0, pose_ok_ = 0, slots_ok_ = 0;

    // outcome of the WVP tests, for the verdict
    struct Wvp {
        uint64_t tested = 0, no_s = 0, no_b = 0, hit_w = 0, hit_d = 0, hit_any = 0, decoy_w = 0, decoy_d = 0, cand_w = 0;
        uint64_t var[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        std::map<std::string, uint64_t> pairs;
    };
    Wvp wvp_total_;
    struct ClassWvp { int cls; Wvp w; };
    std::vector<ClassWvp> wvp_cls_;

    std::string cls_name(int c) const {
        char b[48];
        std::snprintf(b, sizeof(b), "%s/%u", kind_name(cls_[c].kind), cls_[c].size);
        return b;
    }
    int class_of(uint8_t kind, uint32_t size) {
        const uint64_t key = ((uint64_t)kind << 32) | size;
        auto it = cls_idx_.find(key);
        if (it != cls_idx_.end()) return it->second;
        Cls c;
        c.kind = kind;
        c.size = size;
        cls_.push_back(c);
        cls_idx_[key] = (int)cls_.size() - 1;
        return (int)cls_.size() - 1;
    }

    void gather();
    void print_overview();
    void print_ground_truth();
    void build_obs_and_groups();
    void print_blocks();
    void field_maps();
    void chunk_table(const std::vector<int>& idx, uint32_t len, bool contiguous, const char* title, bool dump_first);
    void wvp_tests();
    void verdict();
    const float* blk(const Obs& o) const {
        const RecV& r = recs_[o.rec];
        return (const float*)(r.d + (size_t)o.off * 16 - r.h->dst_offset);
    }
    uint32_t group_count(uint64_t h) const {
        Obs key;
        key.h = h; key.rec = 0; key.off = 0; key.row = 0; key.cls = 0;
        auto cmp = [](const Obs& a, const Obs& b) { return a.h < b.h; };
        auto r = std::equal_range(obs_.begin(), obs_.end(), key, cmp);
        return (uint32_t)(r.second - r.first);
    }
};

// ---------------------------------------------------------------------------
inline void Analyzer::gather() {
    // classes that were written but never kept (the budget dropped every one) still get a row
    for (int i = 0; i < in_.n_bufs; ++i) {
        const BufMetaRec& b = in_.bufs[i];
        meta_idx_[b.ptr] = i;
        for (int k = 0; k < 4; ++k)
            if (b.seen_kind[k]) class_of((uint8_t)k, b.size);
    }
    for (int a = 0; a < 3; ++a) {
        const Arena& ar = in_.arena[a];
        for (uint32_t s = 0; s < ar.n; ++s) {
            const uint8_t* p = ar.base + (size_t)s * ar.slot_bytes;
            const RecHdr* h = (const RecHdr*)p;
            if (h->magic != kRecMagic || h->row >= (uint32_t)kGtRows || h->kind > 3 ||
                h->data_len > ar.payload || (h->data_len & 3)) {
                ++bad_recs_;
                continue;
            }
            RecV r;
            r.h = h;
            r.d = p + kHdrBytes;
            r.cls = (uint16_t)class_of(h->kind, h->byte_size);
            auto bi = buf_idx_.find(h->buffer);
            if (bi == buf_idx_.end()) {
                bufptr_.push_back(h->buffer);
                bi = buf_idx_.emplace(h->buffer, (int)bufptr_.size() - 1).first;
            }
            r.buf = (uint16_t)std::min<int>(bi->second, 65535);
            r.arena = (uint8_t)a;
            recs_.push_back(r);
            cls_[r.cls].nrec++;
            rows_ = std::max(rows_, (int)h->row + 1);
        }
    }
    // distinct buffers per class
    {
        std::vector<std::vector<uint16_t>> per(cls_.size());
        for (const RecV& r : recs_) per[r.cls].push_back(r.buf);
        for (size_t c = 0; c < cls_.size(); ++c) {
            std::sort(per[c].begin(), per[c].end());
            cls_[c].nbuf = (uint32_t)(std::unique(per[c].begin(), per[c].end()) - per[c].begin());
        }
    }
    for (size_t c = 0; c < cls_.size(); ++c) {
        uint64_t seen = 0;
        for (int i = 0; i < in_.n_bufs; ++i)
            if (in_.bufs[i].size == cls_[c].size) seen += in_.bufs[i].seen_kind[cls_[c].kind];
        cls_[c].seen = seen;
    }
}

inline void Analyzer::print_overview() {
    const FileHeader& h = *in_.hdr;
    t_.f("GRW-VR CONSTANT-BUFFER CONTENT CAPTURE, build %u, observation only. pid %u\n", h.build, h.pid);
    t_.f("window: Present #%llu, %u rows requested, %d rows with data; tsc %.3f GHz. Row b = the writes made while frame_count() == start+b. "
         "Row 0 is partial. A deferred-context write is filed under the row current when the worker made the call, which can lag the render thread.\n",
         (unsigned long long)h.start_present, h.frames, rows_, h.tsc_hz / 1e9);
    static const char* an[3] = {"I (immediate 1008/320, every write)", "D (deferred 320/144, first 8 + every 64th per buffer per row)",
                                "M (everything else, first 2 + every 128th per buffer per row)"};
    t_.f("\nBUDGET AND DROPS (the numbers that say how much of the traffic is in the file)\n");
    t_.f("%-62s %10s %10s %10s %10s %10s %9s\n", "arena", "selected", "stored", "budget", "row-quota", "rule-skip", "capacity");
    for (int a = 0; a < 3; ++a)
        t_.f("%-62s %10llu %10llu %10llu %10llu %10llu %9u  (%.1f MB written)\n", an[a], (unsigned long long)h.seen[a],
             (unsigned long long)h.stored[a], (unsigned long long)h.dropped_budget[a], (unsigned long long)h.dropped_quota[a],
             (unsigned long long)h.skipped_rule[a], h.cap_slots[a], (double)h.n_slots[a] * h.slot_bytes[a] / 1048576.0);
    t_.f("read faults %llu, buffer table full %llu, context table full %llu, cache full %llu, writes outside the window rows %llu, "
         "incomplete slots at dump %llu, unreadable records skipped in analysis %llu\n",
         (unsigned long long)h.faults, (unsigned long long)h.buf_full, (unsigned long long)h.ctx_full, (unsigned long long)h.cache_full,
         (unsigned long long)h.outside_window, (unsigned long long)h.incomplete_slots, (unsigned long long)bad_recs_);
    if (h.dropped_budget[0] || h.dropped_quota[0])
        t_.f("NOTE: arena I dropped writes, so the immediate-context counts below are a lower bound for the rows after the drop.\n");

    t_.f("\nCLASSES (context kind / buffer ByteWidth). seen = every write inside the window rows, kept = records in the file.\n");
    t_.f("%-12s %7s %8s %9s %9s %12s\n", "class", "buffers", "kept", "seen", "kept%", "writes/row");
    std::vector<int> ord;
    for (size_t c = 0; c < cls_.size(); ++c) ord.push_back((int)c);
    std::sort(ord.begin(), ord.end(), [&](int a, int b) { return cls_[a].seen != cls_[b].seen ? cls_[a].seen > cls_[b].seen : cls_[a].nrec > cls_[b].nrec; });
    int shown = 0;
    for (int c : ord) {
        if (++shown > 40) { t_.f("(%zu more classes not shown)\n", ord.size() - 40); break; }
        const Cls& k = cls_[c];
        t_.f("%-12s %7u %8u %9llu %8.1f%% %12.1f\n", cls_name(c).c_str(), k.nbuf, k.nrec, (unsigned long long)k.seen,
             k.seen ? 100.0 * k.nrec / (double)k.seen : 0.0, rows_ > 1 ? (double)k.seen / (rows_ - 1) : 0.0);
    }

    t_.f("\nBUFFERS (constant buffers written in the window; per-row counts are EVERY write, not the sample)\n");
    t_.f("%-18s %6s %-12s %10s %9s  %s\n", "buffer", "bytes", "kinds", "writes", "kept", "writes/row min/median/max (rows 1..)");
    struct B { int i; uint64_t tot; };
    std::vector<B> bo;
    for (int i = 0; i < in_.n_bufs; ++i) {
        uint64_t tot = 0;
        for (int k = 0; k < 4; ++k) tot += in_.bufs[i].seen_kind[k];
        bo.push_back({i, tot});
    }
    std::sort(bo.begin(), bo.end(), [](const B& a, const B& b) { return a.tot > b.tot; });
    int nb = 0;
    for (const B& b : bo) {
        if (++nb > 60) { t_.f("(%zu more buffers not shown)\n", bo.size() - 60); break; }
        const BufMetaRec& m = in_.bufs[b.i];
        std::string km;
        for (int k = 0; k < 4; ++k) if (m.kmask & (1u << k)) { if (!km.empty()) km += "+"; km += kind_name(k); }
        std::vector<uint32_t> per;
        for (int r = 1; r < rows_ && r < kGtRows; ++r) if (m.seen_row[r]) per.push_back(m.seen_row[r]);
        std::sort(per.begin(), per.end());
        char rng[64] = "-";
        if (!per.empty()) std::snprintf(rng, sizeof(rng), "%u/%u/%u", per.front(), per[per.size() / 2], per.back());
        char nm[32];
        std::snprintf(nm, sizeof(nm), "0x%llX", (unsigned long long)m.ptr);
        t_.f("%-18s %6u %-12s %10llu %9u  %s\n", nm, m.size, km.c_str(), (unsigned long long)b.tot, m.kept, rng);
    }
}

inline void Analyzer::print_ground_truth() {
    slot_persp_rows_.assign(kNumSlots, 0);
    build_gt(in_, gt_, &slot_persp_rows_);
    int filled = 0, pose_ok = 0, slots_ok = 0, scal_ok = 0, cam_null = 0, pub_ok = 0;
    std::vector<uint64_t> cams;
    int proj_rows = 0;
    for (int r = 0; r < in_.gt_rows; ++r) {
        const GtRow& g = in_.gt[r];
        if (g.magic != kGtMagic) continue;
        ++filled;
        if (g.flags & kGtPose) ++pose_ok;
        if (g.flags & kGtSlots) ++slots_ok;
        if (g.flags & kGtScal) ++scal_ok;
        if (g.flags & kGtCamNull) ++cam_null;
        if (g.pub_valid & 3) ++pub_ok;
        if (g.cam && std::find(cams.begin(), cams.end(), g.cam) == cams.end() && cams.size() < 8) cams.push_back(g.cam);
        if (g.pc[0].valid) ++proj_rows;
    }
    nrows_gt_ = filled; pose_ok_ = pose_ok; slots_ok_ = slots_ok;
    t_.f("\n=== GROUND TRUTH, read from the camera object at every Present ===\n");
    t_.f("rows filled %d of %d; pose (Camera+0x000) read ok %d; engine matrices (Camera+0x420..+0x660) read ok %d; scalars ok %d; "
         "camera pointer null %d; published fov/clip present %d; rows with a proj[2] call recorded %d; distinct camera pointers %zu\n",
         filled, in_.gt_rows, pose_ok, slots_ok, scal_ok, cam_null, pub_ok, proj_rows, cams.size());
    for (size_t i = 0; i < cams.size(); ++i) t_.f("  camera 0x%llX\n", (unsigned long long)cams[i]);
    t_.f("ground-truth matrices in the match set: %zu entries over %zu distinct names (each with its transpose; V = inverse(pose); "
         "derived = V*P and P*V for every perspective-shaped P among the engine matrices, the proj builder read-back and a synthesised one)\n",
         gt_.ents.size(), gt_.names.size());
    t_.f("engine matrix slots that were perspective-shaped in how many rows:");
    for (int k = 0; k < kNumSlots; ++k)
        if (slot_persp_rows_[k]) t_.f(" cam+0x%X:%d", (unsigned)(kSlotBase + 0x40u * (unsigned)k), slot_persp_rows_[k]);
    t_.f("%s\n", std::all_of(slot_persp_rows_.begin(), slot_persp_rows_.end(), [](int x) { return x == 0; }) ? " (none)" : "");

    // how the pose moves, and a few sample rows
    int pose_changes = 0;
    const GtRow* prev = nullptr;
    for (int r = 0; r < in_.gt_rows; ++r) {
        const GtRow& g = in_.gt[r];
        if (g.magic != kGtMagic || !(g.flags & kGtPose)) continue;
        if (prev && std::memcmp(prev->pose, g.pose, sizeof(g.pose)) != 0) ++pose_changes;
        prev = &g;
    }
    t_.f("the pose matrix differed from the previous row's in %d of %d row pairs (the mod alternates eyes, so a change every Present is expected)\n",
         pose_changes, std::max(0, pose_ok - 1));
    int printed = 0;
    for (int r = 0; r < in_.gt_rows && printed < 3; r += std::max(1, in_.gt_rows / 3)) {
        const GtRow& g = in_.gt[r];
        if (g.magic != kGtMagic) continue;
        ++printed;
        t_.f("row %d present #%llu cam 0x%llX flags 0x%X proj-calls-so-far %u pub fov %.4f near %.4f far %.1f\n", g.row,
             (unsigned long long)g.present_no, (unsigned long long)g.cam, g.flags, g.proj_calls_total, g.pub_fov, g.pub_near, g.pub_far);
        t_.f("  scalars +0x2B0 fov %.5f  +0x2B4 near %.4f  +0x2B8 far %.1f  +0x2BC aspmult %.4f  +0x2C0 res %.3f  skew %.4f %.4f  +0x7A4 near-override %.4f\n",
             g.scal[0], g.scal[1], g.scal[2], g.scal[3], g.scal[4], g.scal[5], g.scal[6], g.scal[7]);
        for (int i = 0; i < 4; ++i) {
            t_.f("  pose row %d: % .5f % .5f % .5f % .5f\n", i, g.pose[i * 4], g.pose[i * 4 + 1], g.pose[i * 4 + 2], g.pose[i * 4 + 3]);
        }
        for (int i = 0; i < kProjCalls; ++i) {
            const ProjCall& p = g.pc[i];
            if (!p.valid) continue;
            const long long d = (long long)p.out_proj - (long long)g.cam;
            t_.f("  proj[2] call %u: out 0x%llX (%s) fovy %.5f near %.4f far %.1f aspect %.4f readback %s\n", p.seq,
                 (unsigned long long)p.out_proj, (g.cam && d >= 0 && d < 0x900) ? "INSIDE the camera object" : "outside the camera object", p.fovy,
                 p.near_, p.far_, p.aspect, p.valid >= 2 ? (is_persp(from_f(p.readback).v) ? "perspective-shaped" : "not perspective") : "unreadable");
            if (g.cam && d >= 0 && d < 0x900) t_.f("    out_proj = camera + 0x%llX\n", (unsigned long long)d);
        }
    }
}

inline void Analyzer::build_obs_and_groups() {
    constexpr size_t kObsCap = 5000000;
    size_t est = 0;
    for (const RecV& r : recs_) est += r.h->data_len >= 64 ? (r.h->data_len - 64) / 16 + 1 : 0;
    obs_.reserve(std::min(est, kObsCap));
    for (size_t ri = 0; ri < recs_.size(); ++ri) {
        const RecV& r = recs_[ri];
        if (r.h->dst_offset & 15) { ++skipped_misaligned_; continue; }
        const uint32_t len = r.h->data_len;
        for (uint32_t o = 0; o + 64 <= len; o += 16) {
            const float* f = (const float*)(r.d + o);
            if (!block_usable(f)) continue;
            if (obs_.size() >= kObsCap) { obs_truncated_ = true; break; }
            Obs ob;
            ob.h = hash64(f);
            ob.rec = (uint32_t)ri;
            ob.off = (uint16_t)(r.h->dst_offset / 16 + o / 16);
            ob.row = (uint8_t)std::min<uint32_t>(r.h->row, 255);
            ob.cls = (uint8_t)std::min<int>(r.cls, 254);
            obs_.push_back(ob);
        }
        if (obs_truncated_) break;
    }
    std::sort(obs_.begin(), obs_.end(), [](const Obs& a, const Obs& b) {
        if (a.h != b.h) return a.h < b.h;
        if (a.cls != b.cls) return a.cls < b.cls;
        return a.off < b.off;
    });

    std::vector<uint16_t> bufs_tmp;
    std::vector<std::vector<SEnt>> sraw(kGtRows);
    size_t i = 0;
    while (i < obs_.size()) {
        size_t j = i + 1;
        while (j < obs_.size() && obs_[j].h == obs_[i].h) ++j;
        const uint32_t n = (uint32_t)(j - i);
        const Obs& o0 = obs_[i];
        const float* b0 = blk(o0);

        const GtEnt* mt[8];
        int nmt = 0;
        uint8_t mk = 0;
        {
            const Mat m = from_f(b0);
            if (is_identity(m.v)) mk |= kI;
            else {
                if (is_rigid(m.v)) mk |= kA;
                if (is_persp(m.v)) mk |= kB;
            }
            nmt = gt_.match(b0, mt, 8);
            for (int k = 0; k < nmt; ++k) mk |= gt_.cat[mt[k]->name];
            if (!(mk & (kA | kB | kC | kD | kG | kI))) mk |= kO;
        }
        ++n_groups_;
        if (n == 1) ++n_uniq_; else ++n_rep_;

        bool al64 = false;
        size_t a = i;
        Loc locs[4];
        int nloc = 0;
        while (a < j) {
            size_t b = a + 1;
            while (b < j && obs_[b].cls == obs_[a].cls && obs_[b].off == obs_[a].off) ++b;
            const uint32_t cnt = (uint32_t)(b - a);
            Cell& c = cells_[((uint32_t)obs_[a].cls << 16) | obs_[a].off];
            c.writes += cnt;
            c.distinct++;
            c.maxshare = std::max(c.maxshare, cnt);
            for (int bit = 0; bit < 7; ++bit)
                if (mk & (1 << bit)) { c.markw[bit] += cnt; c.markd[bit]++; }
            for (int k = 0; k < nmt; ++k) {
                const int16_t nm = (int16_t)mt[k]->name;
                bool have = false;
                for (int q = 0; q < 3; ++q) if (c.gtn[q] == nm) have = true;
                if (!have) for (int q = 0; q < 3; ++q) if (c.gtn[q] < 0) { c.gtn[q] = nm; break; }
            }
            if (obs_[a].off % 4 == 0) { al64 = true; n_obs_al64_ += cnt; }
            if (nloc < 4) locs[nloc++] = {obs_[a].cls, obs_[a].off, cnt};
            a = b;
        }

        uint16_t nbuf = 1, nrows = 1, maxrow = 1;
        if (n >= 2) {
            uint16_t rc[kGtRows + 1];
            std::memset(rc, 0, sizeof(rc));
            bufs_tmp.clear();
            for (size_t k = i; k < j; ++k) {
                if (obs_[k].row <= kGtRows) rc[obs_[k].row]++;
                if (bufs_tmp.size() < 4096) bufs_tmp.push_back(recs_[obs_[k].rec].buf);
            }
            std::sort(bufs_tmp.begin(), bufs_tmp.end());
            nbuf = (uint16_t)(std::unique(bufs_tmp.begin(), bufs_tmp.end()) - bufs_tmp.begin());
            nrows = 0; maxrow = 0;
            for (int r = 0; r <= kGtRows; ++r) {
                if (!rc[r]) continue;
                ++nrows;
                maxrow = std::max<uint16_t>(maxrow, rc[r]);
                // shared candidates: not rigid, not identity, and either matched to the ground truth or projection-shaped (2 or
                // more writes in the row), or dense and repeated in 4 or more writes of the row. The threshold of 4 keeps an
                // object's own matrix out of the set: a per-object write can show up twice (the deferred write and the
                // runtime's replay of it) but not four times.
                if (r < kGtRows && !(mk & (kA | kI)) &&
                    (((mk & (kB | kC | kD | kG)) && rc[r] >= 2) || (dense_block(b0) && rc[r] >= 4))) {
                    SEnt e;
                    e.p = b0; e.m = from_f(b0); e.invok = false; e.cnt = rc[r];
                    sraw[r].push_back(e);
                }
            }
            if (al64 && n >= 2) ++n_groups_al64_rep_;
        }
        if (nmt > 0) {
            int best = 0, bestd = 1 << 30;
            for (int k = 0; k < nmt; ++k) {
                const int d = std::abs((int)o0.row - mt[k]->row);
                if (d < bestd) { bestd = d; best = k; }
            }
            const int lag = (int)o0.row - mt[best]->row;
            const uint8_t cat = gt_.cat[mt[best]->name];
            const int ci = cat == kC ? 0 : (cat == kD ? 1 : 2);
            lag_hist_[ci][std::max(-4, std::min(4, lag)) + 4]++;
        }
        if (n >= 4 || (mk & (kB | kC | kD | kG)) || ((mk & kA) && n >= 2)) {
            Group g;
            g.h = o0.h; g.n = n; g.rec0 = o0.rec; g.off0 = o0.off; g.mark = mk; g.al64 = al64;
            g.nbuf = nbuf; g.nrows = nrows; g.maxrow = maxrow;
            g.gtn[0] = g.gtn[1] = -1; g.gtrow[0] = g.gtrow[1] = -1; g.lag = 0;
            for (int k = 0; k < nmt && k < 2; ++k) { g.gtn[k] = (int16_t)mt[k]->name; g.gtrow[k] = (int16_t)mt[k]->row; }
            if (nmt) g.lag = (int16_t)((int)o0.row - mt[0]->row);
            g.nloc = (uint8_t)nloc;
            for (int k = 0; k < nloc; ++k) g.loc[k] = locs[k];
            keep_.push_back(g);
        }
        i = j;
    }

    // the shared-candidate sets, per row: most repeated first, then the ground-truth derived ones
    for (int r = 0; r < kGtRows; ++r) {
        std::vector<SEnt>& v = sraw[r];
        std::sort(v.begin(), v.end(), [](const SEnt& a, const SEnt& b) { return a.cnt > b.cnt; });
        if (v.size() > 64) v.resize(64);
    }
    for (const GtEnt& e : gt_.ents) {
        const uint8_t c = gt_.cat[e.name];
        if (!(c & (kD | kG)) || e.row < 0 || e.row >= kGtRows) continue;
        const Mat m = from_f(e.m);
        if (is_rigid(m.v) || is_identity(m.v)) continue;
        SEnt s;
        s.p = e.m; s.m = m; s.invok = false; s.cnt = 1000000;
        sraw[e.row].push_back(s);
    }
    for (int r = 0; r < kGtRows; ++r) {
        for (SEnt& s : sraw[r]) s.invok = minv(s.m, s.inv);
        S_[r] = std::move(sraw[r]);
    }
}

inline void Analyzer::print_blocks() {
    t_.f("\n=== 4x4 FLOAT BLOCKS IN THE CAPTURED WRITES ===\n");
    t_.f("blocks examined: %zu (every 16-byte aligned offset with 64 readable bytes, finite and not all zero%s); distinct block values %llu, "
         "of which %llu repeat (seen in 2 or more writes) and %llu appear exactly once; %llu observations sit at 64-byte aligned offsets, %llu "
         "repeating values have at least one 64-byte aligned occurrence; writes with a destination offset that is not 16-byte aligned: %llu\n",
         obs_.size(), obs_truncated_ ? ", TRUNCATED at the analysis cap" : "", (unsigned long long)n_groups_,
         (unsigned long long)n_rep_, (unsigned long long)n_uniq_, (unsigned long long)n_obs_al64_,
         (unsigned long long)n_groups_al64_rep_, (unsigned long long)skipped_misaligned_);
    t_.f("marks: A rigid (orthonormal 3x3 to 1e-3 with last row or column (0,0,0,1)); B perspective projection (zeros pattern with (0,0,+-1,0) "
         "in the last column or row); C equal to the camera pose, its inverse or a transpose (to 1e-4 relative); D equal to V*P or P*V from the "
         "ground truth, either convention, with transposes; G equal to a matrix the engine stores in the camera object (Camera+0x420..) or the "
         "proj builder's output, with transpose; I identity; O finite and non-zero but none of those.\n");
    t_.f("lag of the ground truth that matched (capture row minus ground-truth row, first observation of each distinct value; -4/+4 are clamps):\n");
    static const char* ln[3] = {"  pose family (C)", "  derived V*P (D)  ", "  engine slots (G) "};
    for (int c = 0; c < 3; ++c) {
        t_.f("%s", ln[c]);
        for (int l = 0; l < 9; ++l) t_.f(" %+d:%llu", l - 4, (unsigned long long)lag_hist_[c][l]);
        t_.f("\n");
    }

    // per (class, offset) table for the interesting classes
    std::vector<int> focus;
    auto add_focus = [&](uint8_t kind, uint32_t size) {
        auto it = cls_idx_.find(((uint64_t)kind << 32) | size);
        if (it != cls_idx_.end() && std::find(focus.begin(), focus.end(), it->second) == focus.end() && cls_[it->second].nrec) focus.push_back(it->second);
    };
    add_focus(0, 1008); add_focus(0, 320); add_focus(1, 320); add_focus(1, 144);
    {   // plus any other class that carries a marked block
        std::vector<std::pair<uint64_t, int>> extra;
        for (size_t c = 0; c < cls_.size(); ++c) {
            if (std::find(focus.begin(), focus.end(), (int)c) != focus.end() || !cls_[c].nrec) continue;
            uint64_t sc = 0;
            for (const auto& kv : cells_)
                if ((int)(kv.first >> 16) == (int)c) sc += kv.second.markw[2] + kv.second.markw[3] + kv.second.markw[4] + kv.second.markw[1] * 4;
            if (sc) extra.push_back({sc, (int)c});
        }
        std::sort(extra.begin(), extra.end(), [](const std::pair<uint64_t, int>& a, const std::pair<uint64_t, int>& b) { return a.first > b.first; });
        for (size_t k = 0; k < extra.size() && focus.size() < 14; ++k) focus.push_back(extra[k].second);
    }
    t_.f("\nPER-OFFSET BLOCK MAP. One line per (class, buffer offset) where a 4x4 block starting there was a rigid transform, a perspective projection, "
         "or matched the ground truth. '*' = 64-byte aligned. writes = writes in which the block was finite and non-zero; distinct = distinct "
         "values; top = share of writes carrying the single most common value; the counts after the marks are writes by mark. gt = ground truth matched.\n");
    t_.f("%-12s %-8s %8s %8s %7s  %s\n", "class", "offset", "writes", "distinct", "top%", "marks (writes)  gt");
    for (int c : focus) {
        std::vector<std::pair<uint32_t, const Cell*>> v;
        uint32_t other_only = 0;
        for (const auto& kv : cells_) {
            if ((int)(kv.first >> 16) != c) continue;
            const Cell& cl = kv.second;
            if (cl.markw[0] || cl.markw[1] || cl.markw[2] || cl.markw[3] || cl.markw[4]) v.push_back({kv.first & 0xFFFF, &cl});
            else ++other_only;
        }
        std::sort(v.begin(), v.end(), [](const std::pair<uint32_t, const Cell*>& a, const std::pair<uint32_t, const Cell*>& b) { return a.first < b.first; });
        int lines = 0;
        for (const auto& pr : v) {
            if (++lines > 70) { t_.f("%-12s (more offsets not shown)\n", cls_name(c).c_str()); break; }
            const Cell& cl = *pr.second;
            char off[24];
            std::snprintf(off, sizeof(off), "+0x%03X%s", pr.first * 16, pr.first % 4 == 0 ? "*" : " ");
            std::string mk;
            static const char nm[7] = {'A', 'B', 'C', 'D', 'G', 'O', 'I'};
            for (int b = 0; b < 7; ++b)
                if (cl.markw[b]) { char tmp[40]; std::snprintf(tmp, sizeof(tmp), "%s%c:%u", mk.empty() ? "" : " ", nm[b], cl.markw[b]); mk += tmp; }
            std::string g;
            for (int q = 0; q < 3; ++q) if (cl.gtn[q] >= 0) g += (g.empty() ? "" : ",") + gt_.names[cl.gtn[q]];
            t_.f("%-12s %-8s %8u %8u %6.1f%%  %s  %s\n", cls_name(c).c_str(), off, cl.writes, cl.distinct,
                 cl.writes ? 100.0 * cl.maxshare / cl.writes : 0.0, mk.c_str(), g.c_str());
        }
        t_.f("%-12s (%u other offsets carried finite non-zero blocks with none of the marks above)\n", cls_name(c).c_str(), other_only);
    }

    auto floats = [&](const Group& g) {
        const RecV& r = recs_[g.rec0];
        const float* f = (const float*)(r.d + (size_t)g.off0 * 16 - r.h->dst_offset);
        char b[400];
        std::snprintf(b, sizeof(b), "[% .5g % .5g % .5g % .5g | % .5g % .5g % .5g % .5g | % .5g % .5g % .5g % .5g | % .5g % .5g % .5g % .5g]",
                      f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        return std::string(b);
    };
    auto line = [&](const Group& g) {
        std::string locs;
        for (int k = 0; k < g.nloc; ++k) {
            char b[64];
            std::snprintf(b, sizeof(b), "%s%s@+0x%X%s x%u", k ? ", " : "", cls_name(g.loc[k].cls).c_str(), g.loc[k].off * 16u, g.loc[k].off % 4 == 0 ? "*" : "", g.loc[k].cnt);
            locs += b;
        }
        std::string gtn;
        for (int k = 0; k < 2; ++k) if (g.gtn[k] >= 0) { char b[96]; std::snprintf(b, sizeof(b), "%s%s@row%d", k ? "," : "", gt_.names[g.gtn[k]].c_str(), (int)g.gtrow[k]); gtn += b; }
        t_.f("  n=%-6u bufs=%-4u rows=%-4u max/row=%-4u mark=%-4s loc=%s%s%s\n         %s\n", g.n, g.nbuf, g.nrows, g.maxrow, mark_str(g.mark).c_str(),
             locs.c_str(), gtn.empty() ? "" : "  gt=", gtn.c_str(), floats(g).c_str());
    };
    std::vector<const Group*> sel;
    auto top = [&](const char* title, auto pred, size_t cap) {
        sel.clear();
        for (const Group& g : keep_) if (pred(g)) sel.push_back(&g);
        std::sort(sel.begin(), sel.end(), [](const Group* a, const Group* b) { return a->n > b->n; });
        t_.f("\n%s (%zu matching values; showing %zu)\n", title, sel.size(), std::min(sel.size(), cap));
        for (size_t k = 0; k < sel.size() && k < cap; ++k) line(*sel[k]);
    };
    top("BLOCKS EQUAL TO THE GROUND TRUTH (marks C, D, G)", [](const Group& g) { return (g.mark & (kC | kD | kG)) != 0; }, 50);
    top("REPEATING PERSPECTIVE-SHAPED BLOCKS that did not match the ground truth (mark B only)", [](const Group& g) { return (g.mark & kB) && !(g.mark & (kC | kD | kG)) && g.n >= 2; }, 20);
    top("SHARED BLOCKS that repeat across many writes, 64-byte aligned (n >= 8, any mark except identity)", [](const Group& g) { return g.al64 && g.n >= 8 && !(g.mark & kI); }, 30);
    top("SHARED BLOCKS that repeat across many writes, only at 16-byte (not 64-byte) aligned offsets (n >= 8)", [](const Group& g) { return !g.al64 && g.n >= 8 && !(g.mark & kI); }, 15);
    top("REPEATING RIGID TRANSFORMS (mark A, n >= 2)", [](const Group& g) { return (g.mark & kA) && g.n >= 2; }, 15);
    t_.f("\nUNIQUE-PER-WRITE blocks (values seen exactly once) per class: see the 'distinct' versus 'writes' columns of the per-offset table; "
         "a (class, offset) with distinct close to writes is per-object data, one with top near 100%% is a shared constant.\n");
}

// -------- field maps ---------------------------------------------------------
inline void Analyzer::chunk_table(const std::vector<int>& idx_in, uint32_t len, bool contiguous, const char* title, bool dump_first) {
    std::vector<int> idx = idx_in;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        const RecHdr& x = *recs_[a].h;
        const RecHdr& y = *recs_[b].h;
        if (x.buffer != y.buffer) return x.buffer < y.buffer;
        if (x.row != y.row) return x.row < y.row;
        return x.ordinal < y.ordinal;
    });
    const uint32_t nch = len / 16;
    if (!nch || idx.empty()) return;
    struct St { uint32_t wp = 0, wc = 0, fp = 0, fc = 0; bool allzero = true; };
    std::vector<St> st(nch);
    std::vector<std::vector<uint64_t>> vals(nch);
    const RecV* prev = nullptr;
    const RecV* row_first = nullptr;       // first record of the current row of this buffer
    const RecV* prev_row_first = nullptr;  // first record of the previous row
    const RecV* prev_row_last = nullptr;   // last record of the previous row
    for (int ii : idx) {
        const RecV& r = recs_[ii];
        const uint32_t l = std::min(len, r.h->data_len) / 16 * 16;
        if (prev && prev->h->buffer != r.h->buffer) { prev = row_first = prev_row_first = prev_row_last = nullptr; }
        for (uint32_t q = 0; q < l / 16; ++q) {
            const uint8_t* c = r.d + q * 16;
            vals[q].push_back(hash16(c));
            for (int k = 0; k < 16; ++k) if (c[k]) { st[q].allzero = false; break; }
        }
        if (prev && prev->h->row == r.h->row) {
            if (!contiguous || r.h->ordinal == prev->h->ordinal + 1) {
                const uint32_t pl = std::min(l, std::min(len, prev->h->data_len) / 16 * 16);
                for (uint32_t q = 0; q < pl / 16; ++q) {
                    st[q].wp++;
                    if (std::memcmp(prev->d + q * 16, r.d + q * 16, 16) != 0) st[q].wc++;
                }
            }
        } else if (prev) {
            // a new row for this buffer
            prev_row_last = prev;
            prev_row_first = row_first;
            row_first = &r;
            if (prev->h->row + 1 == r.h->row) {
                const RecV* ref = contiguous ? prev_row_last : prev_row_first;
                if (ref) {
                    const uint32_t pl = std::min(l, std::min(len, ref->h->data_len) / 16 * 16);
                    for (uint32_t q = 0; q < pl / 16; ++q) {
                        st[q].fp++;
                        if (std::memcmp(ref->d + q * 16, r.d + q * 16, 16) != 0) st[q].fc++;
                    }
                }
            }
        } else {
            row_first = &r;
        }
        prev = &r;
    }
    t_.f("\n%s  (%zu records, 16-byte rows 0..%u)\n", title, idx.size(), nch - 1);
    t_.f("  class letters: '.' never changed; 'F' changed only between frames; 'W' changed only inside a frame; 'B' both; 'z' always zero; '?' not enough pairs.\n");
    t_.f("  %-8s %-3s %12s %12s %9s\n", "offset", "cls", "within-frame", "between", "distinct");
    std::string runs;
    char last = 0;
    uint32_t run_start = 0;
    std::vector<char> letter(nch);
    for (uint32_t q = 0; q < nch; ++q) {
        auto& v = vals[q];
        std::sort(v.begin(), v.end());
        const uint32_t distinct = (uint32_t)(std::unique(v.begin(), v.end()) - v.begin());
        char c;
        if (st[q].allzero) c = 'z';
        else if (st[q].wp + st[q].fp == 0) c = '?';
        else if (st[q].wc && st[q].fc) c = 'B';
        else if (st[q].wc) c = 'W';
        else if (st[q].fc) c = 'F';
        else c = '.';
        letter[q] = c;
        t_.f("  +0x%03X   %c   %5u/%-6u %5u/%-6u %9u\n", q * 16, c, st[q].wc, st[q].wp, st[q].fc, st[q].fp, distinct);
    }
    for (uint32_t q = 0; q <= nch; ++q) {
        const char c = q < nch ? letter[q] : 0;
        if (q == 0) { last = c; run_start = 0; continue; }
        if (c != last) {
            char b[64];
            std::snprintf(b, sizeof(b), "%s+0x%03X..+0x%03X %c", runs.empty() ? "" : "; ", run_start * 16, q * 16 - 1, last);
            runs += b;
            run_start = q;
            last = c;
        }
    }
    t_.f("  runs: %s\n", runs.c_str());
    if (dump_first) {
        const RecV& r = recs_[idx[0]];
        t_.f("  first write (row %u, ordinal %u), as floats:\n", r.h->row, r.h->ordinal);
        for (uint32_t q = 0; q < r.h->data_len / 16; ++q) {
            const float* f = (const float*)(r.d + q * 16);
            t_.f("   +0x%03X % .6g % .6g % .6g % .6g\n", q * 16, f[0], f[1], f[2], f[3]);
        }
    }
}

inline void Analyzer::field_maps() {
    t_.f("\n=== FIELD MAPS ===\n");
    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t size = pass == 0 ? 1008 : 320;
        auto it = cls_idx_.find(((uint64_t)0 << 32) | size);
        if (it == cls_idx_.end() || !cls_[it->second].nrec) { t_.f("\nno immediate-context writes to a %u-byte buffer were captured\n", size); continue; }
        // one table per buffer, the three busiest
        std::map<uint64_t, std::vector<int>> per;
        for (size_t i = 0; i < recs_.size(); ++i) if (recs_[i].cls == it->second) per[recs_[i].h->buffer].push_back((int)i);
        std::vector<std::pair<size_t, uint64_t>> order;
        for (auto& kv : per) order.push_back({kv.second.size(), kv.first});
        std::sort(order.begin(), order.end(), [](const std::pair<size_t, uint64_t>& a, const std::pair<size_t, uint64_t>& b) { return a.first > b.first; });
        for (size_t k = 0; k < order.size() && k < 3; ++k) {
            char title[160];
            std::snprintf(title, sizeof(title), "IMMEDIATE buffer 0x%llX, %u bytes (all writes kept; consecutive writes compared)",
                          (unsigned long long)order[k].second, size);
            uint32_t len = 0;
            for (int ii : per[order[k].second]) len = std::max(len, recs_[ii].h->data_len);
            chunk_table(per[order[k].second], len, true, title, true);
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t size = pass == 0 ? 320 : 144;
        auto it = cls_idx_.find(((uint64_t)1 << 32) | size);
        if (it == cls_idx_.end() || !cls_[it->second].nrec) { t_.f("\nno deferred-context writes to a %u-byte buffer were captured\n", size); continue; }
        std::vector<int> all;
        for (size_t i = 0; i < recs_.size(); ++i) if (recs_[i].cls == it->second) all.push_back((int)i);
        char title[160];
        std::snprintf(title, sizeof(title), "DEFERRED class def/%u, all buffers together (sampled writes: first 8 + every 64th per buffer per row)", size);
        chunk_table(all, size, false, title, false);
    }
}

// -------- the WVP tests ------------------------------------------------------
inline void Analyzer::wvp_tests() {
    t_.f("\n=== IS A WORLD*VIEW*PROJECTION PRECOMPUTED IN THE PER-OBJECT BUFFERS? ===\n");
    t_.f("Test 1 (in-write product): for a sampled write, W = every world-looking block in it (a 4x4 with a similarity 3x3 and last row or "
         "column (0,0,0,1); or three consecutive 16-byte rows whose xyz form a similarity 3x3, completed with the next row's xyz as translation, "
         "with the w lanes as translation, or with none), S = a shared non-rigid block of the same or an adjacent row (a value that repeated within a "
         "row, or a ground-truth view*projection or engine matrix). HIT when another block B of the same write equals one of W*S, W*S^T, W^T*S, "
         "W^T*S^T, S*W, S*W^T, S^T*W, S^T*W^T (the transposes of these are in the set), to float rounding. Test 2 (derived world): for a block B that "
         "occurs in exactly one write and is not itself rigid, HIT when B*inv(S), inv(S)*B, or the same with B transposed or inv(S) transposed is a "
         "world-looking similarity transform. DECOY: both tests repeated with S taken from a row 10 away; hits there mean S barely changes from frame "
         "to frame, so the pairing in time is not tested.\n");
    std::vector<int> classes;
    for (size_t c = 0; c < cls_.size(); ++c) {
        const Cls& k = cls_[c];
        if (!k.nrec) continue;
        if ((k.kind == 1 && (k.size == 320 || k.size == 144)) || (k.kind == 0 && k.size == 320)) classes.push_back((int)c);
    }
    if (classes.empty()) t_.f("no per-object class (deferred 320/144 or immediate 320) was captured; the test could not run.\n");

    static const char* vname[8] = {"W*S", "W*S^T", "W^T*S", "W^T*S^T", "S*W", "S*W^T", "S^T*W", "S^T*W^T"};
    for (int c : classes) {
        std::vector<int> all;
        for (size_t i = 0; i < recs_.size(); ++i) if (recs_[i].cls == c) all.push_back((int)i);
        std::sort(all.begin(), all.end(), [&](int a, int b) {
            if (recs_[a].h->row != recs_[b].h->row) return recs_[a].h->row < recs_[b].h->row;
            return recs_[a].h->slot < recs_[b].h->slot;
        });
        const size_t want = 120;
        std::vector<int> sample;
        if (all.size() <= want) sample = all;
        else for (size_t k = 0; k < want; ++k) sample.push_back(all[k * all.size() / want]);

        Wvp w;
        for (int pass = 0; pass < 2; ++pass) {          // pass 1 = the decoy
            for (int ii : sample) {
                const RecV& r = recs_[ii];
                const int row = (int)r.h->row;
                int drow = row;
                if (pass == 1) drow = row + 10 < kGtRows ? row + 10 : row - 10;
                if (drow < 0 || drow >= kGtRows) continue;
                const uint32_t nq = r.h->data_len / 16;
                const uint32_t nb = nq >= 4 ? nq - 3 : 0;
                std::vector<Mat> B(nb);
                std::vector<uint8_t> okb(nb, 0);       // 1 = usable per-object candidate (finite, non-zero, not rigid, not identity)
                std::vector<uint8_t> uniq(nb, 0);
                for (uint32_t q = 0; q < nb; ++q) {
                    const float* f = (const float*)(r.d + q * 16);
                    if (!block_usable(f)) continue;
                    B[q] = from_f(f);
                    if (is_rigid(B[q].v) || is_identity(B[q].v)) continue;
                    okb[q] = 1;
                    uniq[q] = group_count(hash64(f)) <= 2;   // a write and its replay may both be in the capture
                }
                // world-looking candidates
                struct Wc { Mat m; uint32_t q; int kind; };
                std::vector<Wc> W;
                for (uint32_t q = 0; q + 4 <= nq && W.size() < 14; ++q) {
                    const float* f = (const float*)(r.d + q * 16);
                    if (block_usable(f)) {
                        const Mat m = from_f(f);
                        if (!is_identity(m.v) && ortho3(m.v, 2e-3, false) && (last_col_h(m.v, 2e-3) || last_row_h(m.v, 2e-3))) { W.push_back({m, q, 0}); continue; }
                    }
                    // three rows, xyz only
                    Mat m;
                    for (int i = 0; i < 16; ++i) m.v[i] = 0;
                    bool fin = true;
                    for (int r3 = 0; r3 < 3; ++r3) {
                        const float* rr = (const float*)(r.d + (q + (uint32_t)r3) * 16);
                        for (int k = 0; k < 3; ++k) { m.v[r3 * 4 + k] = rr[k]; if (!(std::fabs(rr[k]) < 1e6f)) fin = false; }
                    }
                    m.v[15] = 1.0;
                    if (!fin || !ortho3(m.v, 2e-3, false)) continue;
                    if (std::sqrt(m.v[0] * m.v[0] + m.v[1] * m.v[1] + m.v[2] * m.v[2]) < 1e-2) continue;
                    W.push_back({m, q, 2});                           // no translation
                    {   // the w lanes as the translation: as a column ([R|t], last row (0,0,0,1)) and as a row ([R,0],[t,1])
                        Mat m3 = m, m5 = m;
                        bool ok3 = true;
                        for (int r3 = 0; r3 < 3; ++r3) {
                            const float wv = ((const float*)(r.d + (q + (uint32_t)r3) * 16))[3];
                            if (!(std::fabs(wv) < 1e6f)) ok3 = false;
                            m3.v[r3 * 4 + 3] = wv;
                            m5.v[12 + r3] = wv;
                        }
                        if (ok3) { W.push_back({m3, q, 3}); W.push_back({m5, q, 4}); }
                    }
                    {   // the next row's xyz as the translation row
                        const float* tr = (const float*)(r.d + (q + 3) * 16);
                        if (std::fabs(tr[0]) < 1e6f && std::fabs(tr[1]) < 1e6f && std::fabs(tr[2]) < 1e6f) {
                            Mat m1 = m;
                            m1.v[12] = tr[0]; m1.v[13] = tr[1]; m1.v[14] = tr[2];
                            W.push_back({m1, q, 1});
                        }
                    }
                }
                bool any_b = false;
                for (uint32_t q = 0; q < nb; ++q) if (okb[q]) any_b = true;
                if (pass == 0) {
                    if (!W.empty()) w.cand_w++;
                    if (!any_b) { w.no_b++; continue; }
                } else if (!any_b) {
                    continue;
                }
                bool any_s = false;
                bool hit_w = false, hit_d = false;
                std::vector<Mat> Wt(W.size());
                for (size_t k = 0; k < W.size(); ++k) Wt[k] = mtr(W[k].m);
                for (int dr = -1; dr <= 1; ++dr) {
                    const int sr = drow + dr;
                    if (sr < 0 || sr >= kGtRows) continue;
                    for (const SEnt& S : S_[sr]) {
                        any_s = true;
                        const Mat St = mtr(S.m);
                        // test 1
                        for (size_t k = 0; k < W.size(); ++k) {
                            const Wc& wc = W[k];
                            Mat p[8], a[8];
                            mmul2(wc.m, S.m, p[0], a[0]);
                            mmul2(wc.m, St, p[1], a[1]);
                            mmul2(Wt[k], S.m, p[2], a[2]);
                            mmul2(Wt[k], St, p[3], a[3]);
                            mmul2(S.m, wc.m, p[4], a[4]);
                            mmul2(S.m, Wt[k], p[5], a[5]);
                            mmul2(St, wc.m, p[6], a[6]);
                            mmul2(St, Wt[k], p[7], a[7]);
                            for (uint32_t q = 0; q < nb; ++q) {
                                if (!okb[q]) continue;
                                if (q + 4 > wc.q && q < wc.q + 4) continue;        // overlaps W's own rows
                                const float* bf = (const float*)(r.d + q * 16);
                                int vi = -1;
                                for (int v = 0; v < 8; ++v) if (eq_prod(p[v], a[v], bf, false)) { vi = v; break; }
                                if (vi < 0) continue;
                                bool trivial = true;   // B equal to S itself is not a product
                                for (int e = 0; e < 16; ++e)
                                    if (std::fabs(B[q].v[e] - S.m.v[e]) > 1e-4 * std::max(1.0, std::fabs(S.m.v[e]))) { trivial = false; break; }
                                if (trivial) continue;
                                hit_w = true;
                                if (pass == 0) {
                                    w.var[vi]++;
                                    char key[96];
                                    std::snprintf(key, sizeof(key), "W@+0x%03X(%s) -> B@+0x%03X via %s", wc.q * 16,
                                                  wc.kind == 0 ? "4x4" : (wc.kind == 1 ? "3x3+row-t" : (wc.kind == 2 ? "3x3" : (wc.kind == 3 ? "3x4 w-lane t, col" : "3x3 w-lane t, row"))), q * 16, vname[vi]);
                                    w.pairs[key]++;
                                }
                            }
                        }
                        // test 2
                        if (S.invok) {
                            Mat Y[2];
                            Y[0] = S.inv;
                            Y[1] = mtr(S.inv);
                            for (uint32_t q = 0; q < nb; ++q) {
                                if (!okb[q] || !uniq[q]) continue;
                                const Mat Bt = mtr(B[q]);
                                bool hit_q = false;
                                for (int yi = 0; yi < 2 && !hit_q; ++yi) {
                                    Mat x[4], ax[4];
                                    mmul2(B[q], Y[yi], x[0], ax[0]);
                                    mmul2(Y[yi], B[q], x[1], ax[1]);
                                    mmul2(Bt, Y[yi], x[2], ax[2]);
                                    mmul2(Y[yi], Bt, x[3], ax[3]);
                                    for (int v = 0; v < 4; ++v) {
                                        // per-element rounding bound: about 4e-7 of the sum of the absolute products, which is
                                        // a few float ulps; the translation row of a big-coordinate world makes it large there
                                        // and nowhere else, so each entry gets its own slack
                                        auto err = [&](int e) { return std::min(0.05, 4e-7 * ax[v].v[e] + 1e-6); };
                                        double s3 = 0;
                                        for (int i3 = 0; i3 < 3; ++i3) for (int j3 = 0; j3 < 3; ++j3) s3 = std::max(s3, err(i3 * 4 + j3));
                                        const double tol = 3e-3 + 4 * s3;
                                        const double* m = x[v].v;
                                        if (is_identity(m, 1e-3)) continue;
                                        if (!ortho3(m, tol, false)) continue;
                                        bool hc = std::fabs(m[15] - 1.0) < 3e-3 + 4 * err(15);
                                        bool hr = hc;
                                        for (int r3 = 0; r3 < 3; ++r3) {
                                            if (!(std::fabs(m[r3 * 4 + 3]) < 3e-3 + 4 * err(r3 * 4 + 3))) hc = false;
                                            if (!(std::fabs(m[12 + r3]) < 3e-3 + 4 * err(12 + r3))) hr = false;
                                        }
                                        if (!(hc || hr)) continue;
                                        const double n0 = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                                        if (!(n0 > 0.05 && n0 < 50)) continue;
                                        hit_q = true;
                                        break;
                                    }
                                }
                                if (hit_q) {
                                    hit_d = true;
                                    if (pass == 0) {
                                        char key[64];
                                        std::snprintf(key, sizeof(key), "derived world from B@+0x%03X", q * 16);
                                        w.pairs[key]++;
                                    }
                                }
                            }
                        }
                    }
                }
                if (pass == 0) {
                    if (!any_s) { w.no_s++; continue; }
                    w.tested++;
                    if (hit_w) w.hit_w++;
                    if (hit_d) w.hit_d++;
                    if (hit_w || hit_d) w.hit_any++;
                } else if (any_s) {
                    if (hit_w) w.decoy_w++;
                    if (hit_d) w.decoy_d++;
                }
            }
        }
        wvp_cls_.push_back({c, w});
        wvp_total_.tested += w.tested; wvp_total_.hit_any += w.hit_any; wvp_total_.hit_w += w.hit_w; wvp_total_.hit_d += w.hit_d;
        wvp_total_.no_s += w.no_s; wvp_total_.no_b += w.no_b; wvp_total_.cand_w += w.cand_w; wvp_total_.decoy_w += w.decoy_w; wvp_total_.decoy_d += w.decoy_d;
        t_.f("\nclass %s: %zu records, %zu sampled; testable writes %llu (no per-object matrix-like block: %llu; no shared candidate: %llu); "
             "writes with a world-looking block: %llu\n", cls_name(c).c_str(), all.size(), sample.size(), (unsigned long long)w.tested,
             (unsigned long long)w.no_b, (unsigned long long)w.no_s, (unsigned long long)w.cand_w);
        uint64_t vsum = 0;
        for (int v = 0; v < 8; ++v) vsum += w.var[v];
        t_.f("  Test 1 in-write product hit in %llu writes (%llu block hits;", (unsigned long long)w.hit_w, (unsigned long long)vsum);
        for (int v = 0; v < 8; ++v) if (w.var[v]) t_.f(" %s %llu", vname[v], (unsigned long long)w.var[v]);
        t_.f("); Test 2 derived-world hit in %llu writes; either: %llu of %llu testable. DECOY (S from another row): test 1 hit in %llu writes, test 2 in %llu "
             "(test 2 cannot tell rows apart: B*inv(S) stays a world transform for any S that differs from the right one by a rigid view change)\n",
             (unsigned long long)w.hit_d, (unsigned long long)w.hit_any, (unsigned long long)w.tested, (unsigned long long)w.decoy_w, (unsigned long long)w.decoy_d);
        std::vector<std::pair<uint64_t, std::string>> pv;
        for (auto& kv : w.pairs) pv.push_back({kv.second, kv.first});
        std::sort(pv.begin(), pv.end(), [](const std::pair<uint64_t, std::string>& a, const std::pair<uint64_t, std::string>& b) { return a.first > b.first; });
        for (size_t k = 0; k < pv.size() && k < 6; ++k) t_.f("    %llu x %s\n", (unsigned long long)pv[k].first, pv[k].second.c_str());
    }
}

// -------- the verdict --------------------------------------------------------
inline void Analyzer::verdict() {
    t_.f("\n=== VERDICT ===\n");
    t_.f("(Read this with the limits in mind: it is the in-process reading of one %d-row window; the raw records are in the .bin for anything it got wrong.)\n\n", rows_);

    // 1. where the view and projection constants live
    struct Fnd { uint32_t key; const Cell* c; int best_bit; };
    std::vector<Fnd> f;
    for (const auto& kv : cells_) {
        const Cell& c = kv.second;
        int best = -1;
        uint32_t bw = 0;
        for (int b : {3, 4, 2}) if (c.markw[b] > bw) { bw = c.markw[b]; best = b; }
        if (best >= 0) f.push_back({kv.first, &c, best});
    }
    std::sort(f.begin(), f.end(), [](const Fnd& a, const Fnd& b) { return a.c->markw[a.best_bit] > b.c->markw[b.best_bit]; });
    t_.f("1. WHERE THE VIEW AND PROJECTION CONSTANTS LIVE\n");
    bool any_shared = false;
    bool found_vp_imm = false, found_vp_def = false, found_any = false;
    int printed = 0;
    for (const Fnd& x : f) {
        const int cls = (int)(x.key >> 16);
        const uint32_t off = (x.key & 0xFFFF) * 16;
        const Cell& c = *x.c;
        const char* what = x.best_bit == 3 ? "a view*projection built from the ground truth (D)" : (x.best_bit == 4 ? "a matrix the engine itself stores (G)" : "the camera pose, its inverse or transpose (C)");
        std::string g;
        for (int q = 0; q < 3; ++q) if (c.gtn[q] >= 0) g += (g.empty() ? "" : ",") + gt_.names[c.gtn[q]];
        const bool shared = c.distinct && (double)c.writes / c.distinct >= 3.0;
        if (printed < 8) {
            t_.f("   %s-context buffer of %u bytes, offset +0x%03X%s: %s, equal to %s in %u of %u writes; %u distinct values over %u writes "
                 "(about %.1f writes per value: %s)\n", kind_name(cls_[cls].kind), cls_[cls].size, off,
                 (off % 64 == 0) ? " (64-byte aligned)" : " (16-byte aligned only)", what, g.c_str(), c.markw[x.best_bit], c.writes,
                 c.distinct, c.writes, c.distinct ? (double)c.writes / c.distinct : 0.0,
                 shared ? "shared by many writes" : "different in nearly every write, so per-object data");
            ++printed;
        }
        found_any = true;
        if (x.best_bit != 2) {
            if (cls_[cls].kind == 0) found_vp_imm = true;
            if (cls_[cls].kind == 1) found_vp_def = true;
            if (shared) any_shared = true;
        }
    }
    if (!found_any) {
        t_.f("   NO captured 4x4 block equalled the camera pose, a view*projection from the ground truth, or an engine camera matrix. Either the "
             "view and projection reach the shaders by a path this capture did not see (a buffer that is not exactly D3D11_BIND_CONSTANT_BUFFER, "
             "a structured buffer, a GPU-side computation, a different arithmetic), or the ground-truth read did not work (see the GROUND TRUTH "
             "block: pose read ok in %d rows, engine matrices in %d rows).\n", pose_ok_, slots_ok_);
        // print the perspective-shaped candidates as a fallback
        int shown = 0;
        for (const Group& g : keep_) {
            if (!(g.mark & kB) || shown >= 4) continue;
            t_.f("   perspective-shaped block seen %u times at %s@+0x%X (not matched to the ground truth)\n", g.n, cls_name(g.loc[0].cls).c_str(), g.loc[0].off * 16u);
            ++shown;
        }
    }
    if (found_any) {
        t_.f("   Summary: ground-truth-equal view/projection data was found in %s%s%s context buffers. Their lag against the Present-time read is in the "
             "lag table above (0 means the same row).\n", found_vp_imm ? "IMMEDIATE" : "", (found_vp_imm && found_vp_def) ? " and " : "", found_vp_def ? "DEFERRED" : "");
    }

    // 2. WVP
    t_.f("\n2. DO THE PER-OBJECT BUFFERS CONTAIN A PRECOMPUTED WVP?\n");
    const Wvp& w = wvp_total_;
    const char* res;
    if (w.tested < 10) res = "UNKNOWN";
    else if (w.hit_any >= 5 && (double)w.hit_any >= 0.10 * (double)w.tested) res = "YES";
    else if (w.hit_w == 0 && (double)w.hit_d <= std::max(3.0, 0.03 * (double)w.tested) && w.tested >= 20 && found_any) res = "NO";
    else res = "UNKNOWN";
    t_.f("   RESULT: %s. Testable per-object writes %llu, writes with a hit %llu (in-write product %llu, derived world %llu), decoy hits of the in-write product %llu; "
         "writes with no per-object matrix-like block %llu, with no shared candidate %llu.\n", res, (unsigned long long)w.tested, (unsigned long long)w.hit_any,
         (unsigned long long)w.hit_w, (unsigned long long)w.hit_d, (unsigned long long)w.decoy_w, (unsigned long long)w.no_b, (unsigned long long)w.no_s);
    if (!std::strcmp(res, "NO")) {
        t_.f("   NO means: in the sampled per-object writes no block equalled world*S or S*world for any shared candidate captured in the same or "
             "an adjacent row, and %s. It does not rule out a WVP built from a shared matrix that was never captured, or one computed with "
             "different arithmetic.\n", w.hit_d ? "the few unique blocks whose quotient by a shared candidate looked like a world transform (derived-world hits, "
             "below 3 percent of the testable writes) are treated as chance matches in unstructured data" : "no unique block divided by a shared candidate gave a world transform");
    } else if (!std::strcmp(res, "UNKNOWN")) {
        t_.f("   UNKNOWN because %s.\n", w.tested < 10 ? "too few writes could be tested (see the no-block and no-candidate counts)" :
             (w.hit_w == 0 && w.hit_d <= std::max(3.0, 0.03 * (double)w.tested) ? (found_any ? "fewer than 20 writes could be tested" : "no shared view/projection block was identified, so the absence of products proves nothing")
                             : "only isolated hits, below the 10 percent and 5 write threshold for YES, and not zero for NO"));
    } else {
        t_.f("   YES means: per-object blocks equal a world-looking matrix times a shared one in at least 10 percent of the sampled writes. "
             "(Check the decoy count: if it is as high, the shared candidate is constant and the match is still genuine, only untimed.)\n");
    }

    // 3. implication
    t_.f("\n3. WHAT THAT MEANS FOR COMMAND-LIST REPLAY WITH CHANGED VIEW CONSTANTS\n");
    if (!std::strcmp(res, "YES")) {
        t_.f("   The per-object buffers carry object-to-clip transforms that already include the view and projection. Replaying a recorded command "
             "list with different view constants therefore cannot work by changing the shared view buffer alone: every per-object matrix would have "
             "to be rebuilt (or the lists re-recorded with the other eye's camera). Replay as a technique is blocked unless the per-object buffers "
             "are rewritten between replays.\n");
    } else if (!std::strcmp(res, "NO") && found_any) {
        t_.f("   The per-object writes show no precomputed world*view*projection, and the view/projection data was found in shared buffers (item 1). "
             "That is the layout command-list replay needs: re-upload the shared view constants between replays and the per-object data is "
             "reusable. UNVERIFIED: that the shaders really read those shared buffers and nothing else (a draw-time check of what is bound at "
             "each slot is the next question), and that the shared buffer can be rewritten between two replays of one list.\n");
    } else if (!std::strcmp(res, "NO")) {
        t_.f("   No precomputed WVP was found, but the view and projection constants were not located either, so replay with changed view "
             "constants cannot be judged from this capture: the location question (item 1) has to be answered first.\n");
    } else {
        t_.f("   Not decided. %s\n", found_any ? "The view/projection data was found, but the WVP test was inconclusive, so replay feasibility stays open." :
                                                 "Neither the view/projection location nor the WVP question was settled by this capture.");
    }
    (void)any_shared;
}

inline void Analyzer::run() {
    gather();
    print_overview();
    print_ground_truth();
    build_obs_and_groups();
    print_blocks();
    field_maps();
    wvp_tests();
    verdict();
}

inline void analyze(const Input& in, Text& out) {
    Analyzer a(in, out);
    a.run();
}

}  // namespace an
}  // namespace cbdump
}  // namespace grwxr
