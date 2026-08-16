// PaletteProbe.cpp - see PaletteProbe.h for why this exists.
//
// Rule 8 discipline: when DISARMED each hooked draw costs one relaxed atomic
// increment, one relaxed load and a branch, nothing else. The COM getter
// calls happen only inside the armed window (a few thousand draws,
// user-triggered, in a menu scene), which is this probe's designed and
// announced exception to the no-COM rule; the dump with its logging and its
// GetDesc calls runs on the init thread afterward, never in a hook.
//
// BUILD 33.1: the first run armed and never completed. 26 seconds at 72 fps
// put fewer than 4096 indexed draws through the immediate context's
// DrawIndexed/DrawIndexedInstanced slots, so the scene is drawn elsewhere.
// Two blind spots fixed:
//   1. DEFERRED CONTEXTS. In this D3D11 runtime the deferred context class
//      has its OWN vtable (the build-33 assumption that it shares the
//      immediate one was wrong). AnvilNext records scene draws on deferred
//      contexts on worker threads, so we create one deferred context from the
//      game's own device and patch ITS class vtable too, which covers every
//      deferred context the game ever creates. If the runtime does share the
//      vtable, that is detected and logged instead of double-patching.
//   2. ALL FOUR DRAW CALLS. Draw and DrawInstanced are now hooked alongside
//      the two indexed forms, with a lifetime counter per slot, so whichever
//      path the engine uses shows up in numbers instead of in silence.
// The capture window also force-closes after ~12 s and dumps whatever it has,
// with the counters, so a sparse scene can never again end in a log that
// proves nothing.
//
// The capture table is LOCK-FREE (CAS insert into a fixed array): rule 8 bans
// locks in per-draw hooks, and a mutex here could deadlock a render worker
// against the init thread.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>

#include "PaletteProbe.h"
#include "WeaponDraw.h"
#include "DrawHook.h"
#include "Log.h"

namespace grwxr {
namespace pal {
namespace {

// --- capture table: written by the hooks, read and reset by poll() ----------
struct Entry {
    std::atomic<void*>    ptr{nullptr};  // IUnknown*, the table holds ONE ref
    uint8_t               kind = 0;      // 1 = VS constant buffer, 2 = VS SRV
    uint8_t               slot = 0;
    std::atomic<uint32_t> draws{0};
    uint32_t              max_idx = 0;   // best-effort, unsynchronised
};
constexpr int kMaxEntries = 128;
Entry g_tab[kMaxEntries];

std::atomic<int>      g_remaining{0};    // > 0 means armed; one tick per draw
std::atomic<int>      g_dropped{0};
std::atomic<uint64_t> g_sampled{0};
std::atomic<bool>     g_dump_pending{false};

// Lifetime per-slot invocation counters, incremented armed or not. The
// disarmed cost of the whole probe is these plus the armed check.
std::atomic<uint64_t> g_n_di{0}, g_n_d{0}, g_n_dii{0}, g_n_dinst{0};

bool g_installed = false;

// p arrives holding one reference from the VSGet* call. The table keeps
// exactly one reference per unique pointer; every other one is released.
void add_unique(void* p, uint8_t kind, uint8_t slot, UINT index_count) {
    for (int i = 0; i < kMaxEntries; ++i) {
        void* cur = g_tab[i].ptr.load(std::memory_order_acquire);
        if (cur == p) {
            g_tab[i].draws.fetch_add(1, std::memory_order_relaxed);
            if (index_count > g_tab[i].max_idx) g_tab[i].max_idx = index_count;
            ((IUnknown*)p)->Release();
            return;
        }
        if (cur == nullptr) {
            void* expected = nullptr;
            if (g_tab[i].ptr.compare_exchange_strong(
                    expected, p, std::memory_order_acq_rel)) {
                g_tab[i].kind = kind;
                g_tab[i].slot = slot;
                g_tab[i].draws.store(1, std::memory_order_relaxed);
                g_tab[i].max_idx = index_count;
                return;  // the table now owns the getter's reference
            }
            if (expected == p) {  // another thread inserted this same pointer
                g_tab[i].draws.fetch_add(1, std::memory_order_relaxed);
                ((IUnknown*)p)->Release();
                return;
            }
            // Lost the slot to a different pointer: keep scanning.
        }
    }
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    ((IUnknown*)p)->Release();
}

void record(ID3D11DeviceContext* ctx, UINT index_count) {
    if (g_remaining.load(std::memory_order_relaxed) <= 0) return;
    if (g_remaining.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    g_sampled.fetch_add(1, std::memory_order_relaxed);

    ID3D11Buffer* cbs[8] = {};
    ctx->VSGetConstantBuffers(0, 8, cbs);
    for (UINT s = 0; s < 8; ++s)
        if (cbs[s]) add_unique(cbs[s], 1, (uint8_t)s, index_count);

    ID3D11ShaderResourceView* srvs[8] = {};
    ctx->VSGetShaderResources(0, 8, srvs);
    for (UINT s = 0; s < 8; ++s)
        if (srvs[s]) add_unique(srvs[s], 2, (uint8_t)s, index_count);
}

// Build 37: the compute path. RE-notes VERIFIED the shipped skinning compute
// shader binds its bone palette as a stride-48 structured SRV at t3, so a
// dispatch's CS bindings are where the palette shows up if characters are
// pre-skinned. Slot 3 is the one that matters but all 8 are recorded, because
// naming a slot we did not observe would be a guess.
void record_compute(ID3D11DeviceContext* ctx, UINT thread_groups) {
    if (g_remaining.load(std::memory_order_relaxed) <= 0) return;
    if (g_remaining.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    g_sampled.fetch_add(1, std::memory_order_relaxed);

    ID3D11ShaderResourceView* srvs[8] = {};
    ctx->CSGetShaderResources(0, 8, srvs);
    for (UINT s = 0; s < 8; ++s)
        if (srvs[s]) add_unique(srvs[s], 3, (uint8_t)s, thread_groups);

    ID3D11Buffer* cbs[8] = {};
    ctx->CSGetConstantBuffers(0, 8, cbs);
    for (UINT s = 0; s < 8; ++s)
        if (cbs[s]) add_unique(cbs[s], 4, (uint8_t)s, thread_groups);
}

// Build 36: the single recorder DrawHook calls for every detoured draw, on
// whatever thread the game draws from. Rule 8 discipline is unchanged.
void on_draw(ID3D11DeviceContext* ctx, drawhook::Kind kind, uint32_t count) {
    // 2026-08-09: the weapon draw census shares this one recorder (DrawHook
    // takes a single callback). Inert until its own hotkey arms a bucket, and
    // rule-8 clean either way: no COM, no lock, no allocation.
    weapondraw::on_draw(kind, count);

    switch (kind) {
        case drawhook::Kind::DrawIndexed:
            g_n_di.fetch_add(1, std::memory_order_relaxed); break;
        case drawhook::Kind::Draw:
            g_n_d.fetch_add(1, std::memory_order_relaxed); break;
        case drawhook::Kind::DrawIndexedInstanced:
            g_n_dii.fetch_add(1, std::memory_order_relaxed); break;
        case drawhook::Kind::DrawInstanced:
            g_n_dinst.fetch_add(1, std::memory_order_relaxed); break;
        case drawhook::Kind::Dispatch:
        case drawhook::Kind::DispatchIndirect:
            record_compute(ctx, count);
            return;
        default:
            return;
    }
    record(ctx, count);
}

// Init thread only. Names every captured binding, then empties the table.
void dump() {
    int unique = 0;
    for (int i = 0; i < kMaxEntries; ++i) {
        void* p = g_tab[i].ptr.load(std::memory_order_acquire);
        if (!p) continue;
        ++unique;
        const uint32_t draws = g_tab[i].draws.load(std::memory_order_relaxed);
        // kind: 1 = VS CB, 2 = VS SRV, 3 = CS SRV, 4 = CS CB.
        const char* st = g_tab[i].kind == 1 ? "VS" :
                         g_tab[i].kind == 2 ? "VS" :
                         g_tab[i].kind == 3 ? "CS" : "CS";
        if (g_tab[i].kind == 1 || g_tab[i].kind == 4) {
            D3D11_BUFFER_DESC d{};
            ((ID3D11Buffer*)p)->GetDesc(&d);
            LOG_INFO("pal: %s CB  b%u ptr=0x%p bytes=0x%05X usage=%d misc=0x%X "
                     "hits=%u maxIdx=%u",
                     st, g_tab[i].slot, p, d.ByteWidth, (int)d.Usage,
                     d.MiscFlags, draws, g_tab[i].max_idx);
        } else {
            auto* v = (ID3D11ShaderResourceView*)p;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            v->GetDesc(&sd);
            if (sd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ||
                sd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
                const UINT elems =
                    sd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER
                        ? sd.Buffer.NumElements
                        : sd.BufferEx.NumElements;
                UINT stride = 0, bytes = 0;
                ID3D11Resource* res = nullptr;
                v->GetResource(&res);
                if (res) {
                    ID3D11Buffer* rb = nullptr;
                    if (SUCCEEDED(res->QueryInterface(
                            __uuidof(ID3D11Buffer), (void**)&rb)) && rb) {
                        D3D11_BUFFER_DESC d{};
                        rb->GetDesc(&d);
                        stride = d.StructureByteStride;
                        bytes  = d.ByteWidth;
                        rb->Release();
                    }
                    res->Release();
                }
                // Stride 48 is the VERIFIED bone-matrix stride (RE-notes:
                // float3x4, 3 x float4 rows). Flag it in the log so the answer
                // does not depend on anyone scanning hex by eye.
                LOG_INFO("pal: %s SRV t%u ptr=0x%p BUFFER elems=%u stride=0x%X "
                         "bytes=0x%05X fmt=%d hits=%u maxIdx=%u%s",
                         st, g_tab[i].slot, p, elems, stride, bytes,
                         (int)sd.Format, draws, g_tab[i].max_idx,
                         stride == 48 ? "   <<< STRIDE 48: BONE PALETTE "
                                        "CANDIDATE" : "");
            } else {
                LOG_INFO("pal: %s SRV t%u ptr=0x%p dim=%d (not a buffer) "
                         "hits=%u", st, g_tab[i].slot, p,
                         (int)sd.ViewDimension, draws);
            }
        }
        ((IUnknown*)p)->Release();
        g_tab[i].ptr.store(nullptr, std::memory_order_release);
        g_tab[i].draws.store(0, std::memory_order_relaxed);
        g_tab[i].max_idx = 0;
    }
    uint64_t k[(int)drawhook::Kind::kCount] = {};
    drawhook::counts(k);
    LOG_INFO("pal: capture DONE: %llu calls sampled, %d unique bindings, "
             "%d dropped | lifetime: DrawIndexed=%llu Draw=%llu "
             "DrawIndexedInstanced=%llu DrawInstanced=%llu Dispatch=%llu "
             "DispatchIndirect=%llu",
             (unsigned long long)g_sampled.load(std::memory_order_relaxed),
             unique, g_dropped.load(std::memory_order_relaxed),
             (unsigned long long)k[0], (unsigned long long)k[1],
             (unsigned long long)k[2], (unsigned long long)k[3],
             (unsigned long long)k[4], (unsigned long long)k[5]);
    // Build 37: the criterion is no longer a guess. RE-notes VERIFIED from the
    // game's own shipped compute shader that the palette is a STRUCTURED SRV,
    // stride 48 (float3x4), and the player rig has 100 bones, so one
    // character's palette is 100 x 48 = 0x12C0 bytes. The old constant-buffer
    // range (0x3A80..0x4E00) was 3 to 4x too large and would have missed it.
    LOG_INFO("pal: palette = structured SRV, stride 0x30 (48), ~100 elements "
             "(0x12C0 bytes for one character). Verified at t3 in the COMPUTE "
             "skinning path, so check the CS lines first.");
    g_sampled.store(0, std::memory_order_relaxed);
    g_dropped.store(0, std::memory_order_relaxed);
}

}  // namespace

// The draw detours reroute EVERY DrawIndexed/Draw/... the game makes through
// our trampolines in d3d11.dll. They exist only for the palette and
// weapon-draw research captures, the VR mirror does not need them, and their
// per-draw cost has never been measured. They are the highest-risk thing the
// mod does to the game's own render thread, so from v0.8.2 they are OPT-IN:
// this reads the one cfg key directly (the cfg is not parsed this early), and
// with it off NOTHING is detoured. Default OFF. This is also the first thing
// to turn back on when chasing a per-frame render hang, since it is the only
// hook that touches the game's draw path unconditionally.
bool draw_probes_requested() {
    const std::wstring path = log::data_dir() + L"\\grwxr.cfg";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    char line[256];
    bool on = false;
    while (fgets(line, sizeof(line), f)) {
        float v = 0.0f;
        if (sscanf_s(line, " draw_probes = %f", &v) == 1) on = v > 0.0f;
    }
    fclose(f);
    return on;
}

bool install(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (g_installed || !ctx) return g_installed;

    if (!draw_probes_requested()) {
        LOG_INFO("pal: draw probes are OFF (draw_probes=0 or absent in "
                 "grwxr.cfg). The game's draw calls are NOT detoured; this is "
                 "the shipping default. Set draw_probes=1 only for palette or "
                 "weapon-draw research.");
        return false;
    }

    // Build 36: code detours in d3d11.dll, not vtable patches. Build 33.1
    // patched two context objects' HEAP vtables and counted zero draws in 81
    // seconds of rendering; see DrawHook.h for the full reasoning.
    if (drawhook::install(dev, ctx, &on_draw) <= 0) {
        LOG_ERROR("pal: no draw functions detoured; the probe is inert and "
                  "the game runs unmodified (rule 7).");
        return false;
    }

    g_installed = true;
    LOG_INFO("pal: installed. NUMPAD MINUS arms a 4096-draw capture; do it "
             "in the LOBBY MENU.");
    return true;
}

void poll() {
    if (!g_installed) return;

    static bool was_down = false;
    static int  grace = 0;
    static int  stall = 0;
    const bool down = (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
    if (down && !was_down &&
        g_remaining.load(std::memory_order_relaxed) <= 0 &&
        !g_dump_pending.load(std::memory_order_relaxed)) {
        grace = 0;
        stall = 0;
        g_dump_pending.store(true, std::memory_order_relaxed);
        g_remaining.store(4096, std::memory_order_release);
        LOG_INFO("pal: CAPTURE ARMED for the next 4096 draws");
    }
    was_down = down;

    if (!g_dump_pending.load(std::memory_order_relaxed)) return;

    const int rem = g_remaining.load(std::memory_order_acquire);
    if (rem > 0) {
        // Armed but not filling. Report at 5 s, force-close at 12 s so the
        // run always ends in a dump plus counters, never in silence
        // (build 33's failure mode).
        ++stall;
        if (stall == 5) {
            uint64_t k[(int)drawhook::Kind::kCount] = {};
            drawhook::counts(k);
            LOG_INFO("pal: window %d/4096 after 5 s | lifetime: di=%llu d=%llu "
                     "dii=%llu dinst=%llu disp=%llu dispi=%llu",
                     4096 - rem,
                     (unsigned long long)k[0], (unsigned long long)k[1],
                     (unsigned long long)k[2], (unsigned long long)k[3],
                     (unsigned long long)k[4], (unsigned long long)k[5]);
        }
        if (stall >= 12) {
            LOG_INFO("pal: window did not fill in 12 s (%d sampled); forcing "
                     "the dump of what was captured", 4096 - rem);
            g_remaining.store(0, std::memory_order_release);
        }
        return;
    }

    // Dump one poll tick AFTER the window closes, so any hook thread that
    // passed the armed check but is still mid-record finishes first.
    if (++grace >= 2) {
        grace = 0;
        stall = 0;
        g_dump_pending.store(false, std::memory_order_relaxed);
        dump();
    }
}

}  // namespace pal
}  // namespace grwxr
