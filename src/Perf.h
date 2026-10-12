// Perf.h - BUILD 215: the mod's own per-frame CPU time, by phase, once a second.
//
// WHY THIS EXISTS. The frame rate decays after about ninety good seconds on the
// same binary and the same cfg (CX-029, 2026-09-07), with every 6DOF feature and
// the clouds fix ruled out by controlled runs. A decay after a good stretch is
// the signature of something accumulating, and nothing the mod does per frame
// had ever been timed. This header is the instrument: a handful of lock-free
// accumulators, a scope timer, and the present cadence. It costs two
// QueryPerformanceCounter reads per timed call and nothing else. It never logs
// and never allocates (rule 8); the 1 Hz readout lives on the init thread in
// VRMirror.cpp (perf_log).
//
// A Scope must NOT be declared in a function that contains __try (C2712). Wrap
// such a function instead: keep its name on a thin timing wrapper and rename
// the body. grwxr_wgun_apply is that case.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>

namespace grwxr {
namespace perf {

enum Phase : int {
    kPresent = 0,   // the whole of on_present, entry to whichever return it takes
    kWait,          // acquire_frame_state: the runtime's xrWaitFrame, or our wait on its thread
    kBegin,         // xrBeginFrame
    kEnd,           // xrEndFrame, the submit
    kMenu,          // menu::render, the panel's ImGui frame (a no-op while closed)
    kRig,           // grwxr_wgun_apply: the gun root, both hands, both arms, per call
    kCam0,          // grwxr_probe_record by target index 0..10; slot 11 = out of range
    kNumCam = 12,
    kFfr = kCam0 + kNumCam,   // BUILD 216: the render-target bind hook, per bind, every context
    kNumPhases = kFfr + 1,
};

struct Acc {
    std::atomic<uint64_t> n{0};
    std::atomic<int64_t>  sum{0};   // performance-counter ticks
    std::atomic<int64_t>  max{0};   // ticks, the worst single call in the second
};

inline Acc g_acc[kNumPhases];

// the present cadence, from on_present entry to the next
inline std::atomic<int64_t>  g_last_present{0};
inline std::atomic<int64_t>  g_ft_max{0};       // worst present-to-present interval, ticks
inline std::atomic<uint64_t> g_ft_over20{0};    // intervals over 20 ms (a 72 Hz frame is 13.9)
inline std::atomic<uint64_t> g_ft_over28{0};    // intervals over 28 ms (two frames)

// the log's own volume, counted in log::write
inline std::atomic<uint64_t> g_log_lines{0};
inline std::atomic<uint64_t> g_log_bytes{0};

inline std::atomic<int64_t> g_freq{0};

inline int64_t freq() {
    int64_t f = g_freq.load(std::memory_order_relaxed);
    if (!f) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = q.QuadPart ? q.QuadPart : 1;
        g_freq.store(f, std::memory_order_relaxed);   // idempotent: every writer stores the same value
    }
    return f;
}

inline int64_t now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline void atomic_max(std::atomic<int64_t>& m, int64_t v) {
    int64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

inline void add(int phase, int64_t ticks) {
    if (phase < 0 || phase >= kNumPhases) return;
    Acc& a = g_acc[phase];
    a.n.fetch_add(1, std::memory_order_relaxed);
    a.sum.fetch_add(ticks, std::memory_order_relaxed);
    atomic_max(a.max, ticks);
}

// Times a scope, whichever path leaves it.
struct Scope {
    int     phase;
    int64_t t0;
    explicit Scope(int p) : phase(p), t0(now()) {}
    ~Scope() { add(phase, now() - t0); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// At on_present entry: the interval since the last entry.
inline void mark_present() {
    const int64_t t    = now();
    const int64_t last = g_last_present.exchange(t, std::memory_order_relaxed);
    if (!last) return;
    const int64_t dt = t - last;
    atomic_max(g_ft_max, dt);
    const int64_t f = freq();
    if (dt * 1000 > 20 * f) g_ft_over20.fetch_add(1, std::memory_order_relaxed);
    if (dt * 1000 > 28 * f) g_ft_over28.fetch_add(1, std::memory_order_relaxed);
}

// The 1 Hz drain: take one accumulator and reset it. Milliseconds.
struct Take {
    uint64_t n;
    double   sum_ms;
    double   max_ms;
};

inline Take take(int phase) {
    Take t{0, 0.0, 0.0};
    if (phase < 0 || phase >= kNumPhases) return t;
    Acc& a = g_acc[phase];
    const double k = 1000.0 / (double)freq();
    t.n      = a.n.exchange(0, std::memory_order_relaxed);
    t.sum_ms = (double)a.sum.exchange(0, std::memory_order_relaxed) * k;
    t.max_ms = (double)a.max.exchange(0, std::memory_order_relaxed) * k;
    return t;
}

}  // namespace perf
}  // namespace grwxr
