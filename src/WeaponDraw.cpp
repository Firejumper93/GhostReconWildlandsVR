// WeaponDraw.cpp - see WeaponDraw.h for why this exists.

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "WeaponDraw.h"
#include "Log.h"

namespace grwxr {
namespace weapondraw {
namespace {

// Open-addressed, fixed size, allocated with the module. Power of two so the
// hash reduces with a mask.
constexpr uint32_t kSize  = 4096;
constexpr uint32_t kProbe = 8;      // linear probes before a sample is dropped

// One recording is kSeconds long and each second is one bit of a u32 mask, so
// the whole timeline of a draw is a single atomic word.
constexpr uint32_t kSeconds = 32;

struct Slot {
    std::atomic<uint32_t> key;      // index count; 0 = empty
    std::atomic<uint32_t> mask;     // bit s set = drawn during second s
    std::atomic<uint32_t> hits;     // total draws, for volume ranking
};
Slot g_tab[kSize];

// 0 = idle and the per-draw path returns immediately.
std::atomic<uint32_t> g_recording{0};
std::atomic<uint32_t> g_second{0};
std::atomic<uint32_t> g_dropped{0};

inline uint32_t hash(uint32_t k) {
    return (k * 2654435761u) & (kSize - 1);
}

void reset_table() {
    for (uint32_t i = 0; i < kSize; ++i) {
        g_tab[i].key.store(0, std::memory_order_relaxed);
        g_tab[i].mask.store(0, std::memory_order_relaxed);
        g_tab[i].hits.store(0, std::memory_order_relaxed);
    }
    g_dropped.store(0, std::memory_order_relaxed);
}

inline uint32_t popcount(uint32_t v) {
    uint32_t n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}

inline int first_bit(uint32_t v) {
    for (uint32_t i = 0; i < kSeconds; ++i) if (v & (1u << i)) return (int)i;
    return -1;
}

inline int last_bit(uint32_t v) {
    for (int i = (int)kSeconds - 1; i >= 0; --i) if (v & (1u << i)) return i;
    return -1;
}

// Poll thread only. Reports the draws whose TIMELINE CHANGED during the run,
// and the second at which most of them changed, which is the weapon swap.
void analyse() {
    constexpr uint32_t kFull = 0xFFFFFFFFu;   // seen in all 32 seconds
    constexpr int kReport = 60;

    LOG_INFO("wdraw: RECORDING DONE (%u s). Draws present for the WHOLE run are "
             "scenery and are not listed. Everything below changed state during "
             "the run; the weapon swap is the second most of them agree on.",
             kSeconds);

    // Vote on the transition second, so the swap moment is identified by the
    // data instead of by the user's timing.
    uint32_t vote_off[kSeconds] = {};   // stopped being drawn after second s
    uint32_t vote_on[kSeconds]  = {};   // started being drawn at second s

    for (uint32_t i = 0; i < kSize; ++i) {
        if (!g_tab[i].key.load(std::memory_order_relaxed)) continue;
        const uint32_t m = g_tab[i].mask.load(std::memory_order_relaxed);
        if (m == 0 || m == kFull) continue;
        if (g_tab[i].hits.load(std::memory_order_relaxed) < 200) continue;
        const int f = first_bit(m), l = last_bit(m);
        if (f <= 1 && l >= 0 && l < (int)kSeconds - 2) ++vote_off[l];
        if (f >= 2 && l >= (int)kSeconds - 2)          ++vote_on[f];
    }
    int best_s = -1; uint32_t best_v = 0;
    for (uint32_t s = 0; s < kSeconds; ++s) {
        const uint32_t v = vote_off[s] + (s ? vote_on[s] : 0);
        if (v > best_v) { best_v = v; best_s = (int)s; }
    }
    if (best_s >= 0)
        LOG_INFO("wdraw: the swap looks like second %d (%u draw families change "
                 "there). OFF at that second = the weapon you started with.",
                 best_s, best_v);
    else
        LOG_INFO("wdraw: no clean transition found. If the weapon never got "
                 "swapped during the run, nothing here is a weapon.");

    // Report the changers, biggest first.
    uint32_t printed = 0, ceiling = 0xFFFFFFFFu;
    while (printed < kReport) {
        uint32_t best = 0; int best_i = -1;
        for (uint32_t i = 0; i < kSize; ++i) {
            if (!g_tab[i].key.load(std::memory_order_relaxed)) continue;
            const uint32_t m = g_tab[i].mask.load(std::memory_order_relaxed);
            if (m == 0 || m == kFull) continue;
            const uint32_t h = g_tab[i].hits.load(std::memory_order_relaxed);
            if (h < 200 || h >= ceiling) continue;
            if (h > best) { best = h; best_i = (int)i; }
        }
        if (best_i < 0) break;
        for (uint32_t i = 0; i < kSize && printed < kReport; ++i) {
            if (!g_tab[i].key.load(std::memory_order_relaxed)) continue;
            const uint32_t m = g_tab[i].mask.load(std::memory_order_relaxed);
            if (m == 0 || m == kFull) continue;
            if (g_tab[i].hits.load(std::memory_order_relaxed) != best) continue;
            const int f = first_bit(m), l = last_bit(m);
            const bool off = (f <= 1 && l < (int)kSeconds - 2);
            const bool on  = (f >= 2 && l >= (int)kSeconds - 2);
            LOG_INFO("wdraw:   indices=%-6u hits=%-8u secs %2d..%-2d (%u of %u)%s",
                     g_tab[i].key.load(std::memory_order_relaxed), best, f, l,
                     popcount(m), kSeconds,
                     off ? "   <<< WENT AWAY (weapon 1 candidate)"
                         : (on ? "   <<< APPEARED (weapon 2 candidate)" : ""));
            ++printed;
        }
        ceiling = best;
    }
    LOG_INFO("wdraw: %u changing draw families listed, %u sample(s) dropped. "
             "The suppression build gates on the WENT AWAY index counts.",
             printed, g_dropped.load(std::memory_order_relaxed));
    reset_table();
}

}  // namespace

void on_draw(drawhook::Kind kind, uint32_t index_count) {
    if (!g_recording.load(std::memory_order_relaxed)) return;   // normal state
    if (kind != drawhook::Kind::DrawIndexed &&
        kind != drawhook::Kind::DrawIndexedInstanced) return;
    if (!index_count) return;                  // 0 would collide with "empty"

    const uint32_t bit = 1u << (g_second.load(std::memory_order_relaxed) & 31);

    uint32_t i = hash(index_count);
    for (uint32_t p = 0; p < kProbe; ++p, i = (i + 1) & (kSize - 1)) {
        uint32_t k = g_tab[i].key.load(std::memory_order_acquire);
        if (k == 0) {
            uint32_t expect = 0;
            if (!g_tab[i].key.compare_exchange_strong(
                    expect, index_count, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                k = expect;
            } else {
                k = index_count;
            }
        }
        if (k != index_count) continue;        // collision, keep probing
        g_tab[i].mask.fetch_or(bit, std::memory_order_relaxed);
        g_tab[i].hits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_dropped.fetch_add(1, std::memory_order_relaxed);
}

void poll() {
    // ONE keypress, then the probe runs itself. Three runs were lost to a
    // multi-press protocol (2026-08-09, logs grwxr-9440, -23144, -26664): the
    // user is in a headset and cannot see the log, so every extra press was a
    // chance to lose the whole run. Now the recorder times itself and finds the
    // weapon swap in the data.
    //
    // Bit 0x8000 alone would miss a tap between two 1 Hz polls, so bit 0x0001
    // (pressed since the previous call for this key) is the real edge. Nothing
    // else in the tree polls VK_NUMPAD7.
    static bool was_down = false;
    const SHORT st   = GetAsyncKeyState(VK_NUMPAD7);
    const bool  down = (st & 0x8000) != 0;
    const bool  edge = ((st & 0x0001) != 0) || (down && !was_down);
    was_down = down;

    if (!g_recording.load(std::memory_order_relaxed)) {
        if (!edge) return;
        reset_table();
        g_second.store(0, std::memory_order_release);
        g_recording.store(1, std::memory_order_release);
        LOG_INFO("wdraw: RECORDING for %u seconds. Stand still and look one "
                 "way. SWAP YOUR WEAPON once, whenever you like during the "
                 "run: the exact moment does not matter and there is NOTHING "
                 "else to press. The result writes itself at the end.",
                 kSeconds);
        return;
    }

    const uint32_t s = g_second.load(std::memory_order_relaxed) + 1;
    if (s >= kSeconds) {
        g_recording.store(0, std::memory_order_release);
        analyse();
        return;
    }
    g_second.store(s, std::memory_order_release);

    uint32_t distinct = 0;
    for (uint32_t i = 0; i < kSize; ++i)
        if (g_tab[i].key.load(std::memory_order_relaxed)) ++distinct;
    LOG_INFO("wdraw: recording %u/%u s, %u distinct index counts%s",
             s, kSeconds, distinct,
             s == kSeconds / 2 ? "   <-- swap your weapon around now if you "
                                 "have not yet" : "");
}

}  // namespace weapondraw
}  // namespace grwxr
