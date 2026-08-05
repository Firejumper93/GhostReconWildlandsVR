// DrawHook.cpp - see DrawHook.h for why code detours replaced vtable patching.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "DrawHook.h"
#include "Log.h"

namespace grwxr {
namespace drawhook {
namespace {

// ID3D11DeviceContext vtable slots (the vtable is only used to LOOK UP the
// function addresses; nothing is written into it).
constexpr int kSlotDrawIndexed          = 12;
constexpr int kSlotDraw                 = 13;
constexpr int kSlotDrawIndexedInstanced = 20;
constexpr int kSlotDrawInstanced        = 21;
constexpr int kSlotDispatch             = 41;
constexpr int kSlotDispatchIndirect     = 42;

constexpr size_t kMinSteal   = 8;    // one aligned atomic store
constexpr size_t kMaxSteal   = 24;
constexpr int    kMaxTargets = 12;   // 6 kinds x 2 context classes

Recorder g_rec = nullptr;
std::atomic<uint64_t> g_counts[(int)Kind::kCount] = {};

// --- the instruction whitelist ---------------------------------------------
// Only position-independent forms appear here: no rip-relative operands and no
// rel32 branches, because stolen bytes execute from a different address in the
// trampoline. A prologue containing anything else is refused rather than
// guessed at.
struct Shape { uint8_t len; uint8_t n; uint8_t b[4]; };
constexpr Shape kShapes[] = {
    // push/pop with and without a REX prefix
    {1, 1, {0x53}}, {1, 1, {0x55}}, {1, 1, {0x56}}, {1, 1, {0x57}},
    {2, 2, {0x40, 0x53}}, {2, 2, {0x40, 0x55}},
    {2, 2, {0x40, 0x56}}, {2, 2, {0x40, 0x57}},
    {2, 2, {0x41, 0x54}}, {2, 2, {0x41, 0x55}},
    {2, 2, {0x41, 0x56}}, {2, 2, {0x41, 0x57}},
    // sub rsp, imm8 / imm32
    {4, 3, {0x48, 0x83, 0xEC}}, {7, 3, {0x48, 0x81, 0xEC}},
    // add reg, imm32  (add rcx, -0xd8 in these prologues)
    {7, 3, {0x48, 0x81, 0xC1}}, {7, 3, {0x48, 0x81, 0xC2}},
    // lea r, [reg + disp32]
    {7, 3, {0x4C, 0x8D, 0x81}}, {7, 3, {0x4C, 0x8D, 0x89}},
    {7, 3, {0x4C, 0x8D, 0x91}}, {7, 3, {0x4C, 0x8D, 0x99}},
    {7, 3, {0x48, 0x8D, 0x81}}, {7, 3, {0x48, 0x8D, 0x91}},
    // mov r32, r32 (register moves seen shuffling the draw arguments)
    {3, 3, {0x44, 0x8B, 0xDA}}, {3, 3, {0x44, 0x8B, 0xD2}},
    {3, 3, {0x41, 0x8B, 0xD8}}, {3, 3, {0x41, 0x8B, 0xD9}},
    {3, 3, {0x44, 0x8B, 0xC2}}, {3, 3, {0x8B, 0xDA, 0x00}},
    // mov rax, rsp ; mov [rsp+8], reg
    {3, 3, {0x48, 0x8B, 0xC4}},
    // mov [rsp+disp8], r64 register spills. Build 37's run showed the
    // immediate DrawInstanced prologue is a run of these
    // (48 89 5C 24 08 / 48 89 6C 24 10 / 48 89 74 24 18 / 57), and the two
    // missing forms were the only reason 7 of 8 functions were detoured
    // instead of 8. Logged loudly rather than silently skipped, which is how
    // the gap was found.
    {5, 4, {0x48, 0x89, 0x5C, 0x24}}, {5, 4, {0x4C, 0x89, 0x4C, 0x24}},
    {5, 4, {0x48, 0x89, 0x4C, 0x24}}, {5, 4, {0x48, 0x89, 0x54, 0x24}},
    {5, 4, {0x48, 0x89, 0x6C, 0x24}}, {5, 4, {0x48, 0x89, 0x74, 0x24}},
    {5, 4, {0x48, 0x89, 0x7C, 0x24}}, {5, 4, {0x4C, 0x89, 0x44, 0x24}},
};

// Length of the instruction at p, or 0 if it is not on the whitelist.
size_t insn_len(const uint8_t* p) {
    for (const Shape& s : kShapes) {
        bool hit = true;
        for (int i = 0; i < s.n; ++i)
            if (p[i] != s.b[i]) { hit = false; break; }
        if (hit) return s.len;
    }
    return 0;
}

// Whole instructions covering at least kMinSteal bytes, or 0 if the prologue
// contains anything unrecognised.
size_t steal_len(const uint8_t* p) {
    size_t n = 0;
    while (n < kMinSteal) {
        const size_t l = insn_len(p + n);
        if (!l || n + l > kMaxSteal) return 0;
        n += l;
    }
    return n;
}

// --- near memory for the bridges -------------------------------------------
// The E9 at the entry can only reach +-2 GB, and our DLL may be far from
// d3d11.dll, so each detour jumps to a small bridge allocated near the target
// which then jumps absolutely to our hook.
uint8_t* g_arena     = nullptr;
size_t   g_arena_use = 0;
constexpr size_t kArenaSize = 4096;

uint8_t* alloc_near(uint8_t* anchor) {
    // Walk outward from the anchor in 64 KB steps (the allocation granularity)
    // until a free region takes the allocation, staying inside +-1.5 GB.
    constexpr uintptr_t kStep = 0x10000;
    constexpr uintptr_t kSpan = 0x60000000;
    for (uintptr_t d = kStep; d < kSpan; d += kStep) {
        for (int dir = 0; dir < 2; ++dir) {
            uintptr_t a = dir ? (uintptr_t)anchor + d : (uintptr_t)anchor - d;
            a &= ~(kStep - 1);
            if (a < 0x10000) continue;
            void* p = VirtualAlloc((void*)a, kArenaSize,
                                   MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return (uint8_t*)p;
        }
    }
    return nullptr;
}

uint8_t* arena_take(size_t n) {
    n = (n + 15) & ~size_t(15);
    if (!g_arena || g_arena_use + n > kArenaSize) return nullptr;
    uint8_t* p = g_arena + g_arena_use;
    g_arena_use += n;
    return p;
}

// --- one detour -------------------------------------------------------------
struct Target {
    uint8_t* fn      = nullptr;   // entry in d3d11.dll
    uint8_t  saved[8] = {};       // original first 8 bytes
    size_t   steal   = 0;
    void*    tramp   = nullptr;   // stolen bytes + jmp back
};
Target g_targets[kMaxTargets];
int    g_ntargets = 0;

// Write an absolute indirect jump: FF 25 00 00 00 00 <qword target>.
void write_abs_jmp(uint8_t* at, void* to) {
    at[0] = 0xFF; at[1] = 0x25;
    at[2] = at[3] = at[4] = at[5] = 0x00;
    memcpy(at + 6, &to, sizeof(to));
}

// Patch fn's first 8 bytes with `E9 rel32` to `bridge` plus NOP padding, in one
// aligned atomic store so no thread can see a half-written jump.
bool atomic_patch(uint8_t* fn, uint8_t* bridge, uint8_t saved_out[8]) {
    if (((uintptr_t)fn & 7) != 0) {
        LOG_ERROR("draw: entry 0x%p is not 8-byte aligned; refusing to patch "
                  "(an unaligned store is not atomic).", (void*)fn);
        return false;
    }
    const int64_t rel = (int64_t)(bridge - (fn + 5));
    if (rel < INT32_MIN || rel > INT32_MAX) {
        LOG_ERROR("draw: bridge 0x%p is out of E9 range of 0x%p.",
                  (void*)bridge, (void*)fn);
        return false;
    }
    uint8_t buf[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel32 = (int32_t)rel;
    memcpy(buf + 1, &rel32, sizeof(rel32));

    DWORD prot = 0;
    if (!VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &prot)) {
        LOG_ERROR("draw: VirtualProtect failed at 0x%p (err %lu).",
                  (void*)fn, GetLastError());
        return false;
    }
    memcpy(saved_out, fn, 8);
    int64_t next = 0;
    memcpy(&next, buf, sizeof(next));
    InterlockedExchange64((volatile LONG64*)fn, next);
    DWORD dummy = 0;
    VirtualProtect(fn, 8, prot, &dummy);
    FlushInstructionCache(GetCurrentProcess(), fn, 8);
    return true;
}

// --- the hooks --------------------------------------------------------------
// One pair per draw kind, because the immediate-context and deferred-context
// implementations are DIFFERENT functions in d3d11.dll and each needs its own
// trampoline. The recorder runs first, then the trampoline continues into the
// real function, which returns here and then back to the game.
using FnDrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using FnDraw        = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using FnDII         = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using FnDInst       = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using FnDisp        = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using FnDispInd     = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);

FnDrawIndexed g_t_di[2]    = {};
FnDraw        g_t_d[2]     = {};
FnDII         g_t_dii[2]   = {};
FnDInst       g_t_dinst[2] = {};
FnDisp        g_t_disp[2]  = {};
FnDispInd     g_t_dispi[2] = {};

inline void tally(Kind k) {
    g_counts[(int)k].fetch_add(1, std::memory_order_relaxed);
}

template <int N>
void STDMETHODCALLTYPE hk_di(ID3D11DeviceContext* c, UINT i, UINT s, INT b) {
    tally(Kind::DrawIndexed);
    if (g_rec) g_rec(c, Kind::DrawIndexed, i);
    g_t_di[N](c, i, s, b);
}
template <int N>
void STDMETHODCALLTYPE hk_d(ID3D11DeviceContext* c, UINT v, UINT s) {
    tally(Kind::Draw);
    if (g_rec) g_rec(c, Kind::Draw, v);
    g_t_d[N](c, v, s);
}
template <int N>
void STDMETHODCALLTYPE hk_dii(ID3D11DeviceContext* c, UINT i, UINT n, UINT s,
                              INT b, UINT si) {
    tally(Kind::DrawIndexedInstanced);
    if (g_rec) g_rec(c, Kind::DrawIndexedInstanced, i);
    g_t_dii[N](c, i, n, s, b, si);
}
template <int N>
void STDMETHODCALLTYPE hk_dinst(ID3D11DeviceContext* c, UINT v, UINT n, UINT s,
                                UINT si) {
    tally(Kind::DrawInstanced);
    if (g_rec) g_rec(c, Kind::DrawInstanced, v);
    g_t_dinst[N](c, v, n, s, si);
}
// Build 37: the compute path. The recorder is called BEFORE the dispatch, so
// the bindings it reads are the ones this dispatch will consume.
template <int N>
void STDMETHODCALLTYPE hk_disp(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    tally(Kind::Dispatch);
    if (g_rec) g_rec(c, Kind::Dispatch, x);
    g_t_disp[N](c, x, y, z);
}
template <int N>
void STDMETHODCALLTYPE hk_dispi(ID3D11DeviceContext* c, ID3D11Buffer* b,
                                UINT off) {
    tally(Kind::DispatchIndirect);
    if (g_rec) g_rec(c, Kind::DispatchIndirect, 0);
    g_t_dispi[N](c, b, off);
}

// Detour one function. `slot_index` is 0 for the immediate-context set and 1
// for the deferred-context set, which selects the trampoline storage.
bool detour(uint8_t* fn, Kind kind, int set, const char* tag) {
    if (!fn) return false;
    for (int i = 0; i < g_ntargets; ++i)
        if (g_targets[i].fn == fn) return true;   // shared implementation

    const size_t steal = steal_len(fn);
    if (!steal) {
        LOG_ERROR("draw: %s prologue not on the whitelist, refusing to patch. "
                  "First 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X "
                  "%02X %02X %02X %02X %02X %02X %02X %02X",
                  tag, fn[0], fn[1], fn[2], fn[3], fn[4], fn[5], fn[6], fn[7],
                  fn[8], fn[9], fn[10], fn[11], fn[12], fn[13], fn[14], fn[15]);
        return false;
    }

    uint8_t* tramp  = arena_take(steal + 14);
    uint8_t* bridge = arena_take(14);
    if (!tramp || !bridge) {
        LOG_ERROR("draw: out of near arena for %s.", tag);
        return false;
    }
    memcpy(tramp, fn, steal);
    write_abs_jmp(tramp + steal, fn + steal);

    void* hook = nullptr;
    switch (kind) {
        case Kind::DrawIndexed:
            g_t_di[set] = (FnDrawIndexed)tramp;
            hook = set ? (void*)&hk_di<1> : (void*)&hk_di<0>;
            break;
        case Kind::Draw:
            g_t_d[set] = (FnDraw)tramp;
            hook = set ? (void*)&hk_d<1> : (void*)&hk_d<0>;
            break;
        case Kind::DrawIndexedInstanced:
            g_t_dii[set] = (FnDII)tramp;
            hook = set ? (void*)&hk_dii<1> : (void*)&hk_dii<0>;
            break;
        case Kind::DrawInstanced:
            g_t_dinst[set] = (FnDInst)tramp;
            hook = set ? (void*)&hk_dinst<1> : (void*)&hk_dinst<0>;
            break;
        case Kind::Dispatch:
            g_t_disp[set] = (FnDisp)tramp;
            hook = set ? (void*)&hk_disp<1> : (void*)&hk_disp<0>;
            break;
        case Kind::DispatchIndirect:
            g_t_dispi[set] = (FnDispInd)tramp;
            hook = set ? (void*)&hk_dispi<1> : (void*)&hk_dispi<0>;
            break;
        default:
            return false;
    }
    write_abs_jmp(bridge, hook);
    FlushInstructionCache(GetCurrentProcess(), g_arena, kArenaSize);

    Target t;
    t.fn    = fn;
    t.steal = steal;
    t.tramp = tramp;
    if (!atomic_patch(fn, bridge, t.saved)) return false;
    g_targets[g_ntargets++] = t;
    LOG_INFO("draw: %-28s 0x%p detoured (stole %zu bytes)", tag, (void*)fn,
             steal);
    return true;
}

uint8_t* slot_fn(void* obj, int slot) {
    if (!obj) return nullptr;
    void** vt = *(void***)obj;
    return vt ? (uint8_t*)vt[slot] : nullptr;
}

}  // namespace

int install(ID3D11Device* dev, ID3D11DeviceContext* ctx, Recorder rec) {
    if (g_ntargets || !ctx) return g_ntargets;
    g_rec = rec;

    uint8_t* anchor = slot_fn(ctx, kSlotDrawIndexed);
    if (!anchor) {
        LOG_ERROR("draw: could not read DrawIndexed from the context vtable.");
        return 0;
    }
    // Report where the implementations actually live: this is the fact that
    // explained build 33.1's zero-draw result.
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    LOG_INFO("draw: d3d11.dll base 0x%p, DrawIndexed impl 0x%p (RVA 0x%zX); "
             "context vtable 0x%p is heap, which is why per-object vtable "
             "patching saw nothing.",
             (void*)d3d11, (void*)anchor,
             d3d11 ? (size_t)(anchor - (uint8_t*)d3d11) : (size_t)0,
             (void*)*(void***)ctx);

    g_arena = alloc_near(anchor);
    if (!g_arena) {
        LOG_ERROR("draw: no free page within 2 GB of d3d11.dll for the "
                  "bridges. Nothing patched.");
        return 0;
    }

    struct Job { int slot; Kind kind; const char* name; };
    constexpr Job kJobs[] = {
        {kSlotDrawIndexed,          Kind::DrawIndexed,          "DrawIndexed"},
        {kSlotDraw,                 Kind::Draw,                 "Draw"},
        {kSlotDrawIndexedInstanced, Kind::DrawIndexedInstanced, "DrawIndexedInstanced"},
        {kSlotDrawInstanced,        Kind::DrawInstanced,        "DrawInstanced"},
        {kSlotDispatch,             Kind::Dispatch,             "Dispatch"},
        {kSlotDispatchIndirect,     Kind::DispatchIndirect,     "DispatchIndirect"},
    };

    char tag[96];
    for (const Job& j : kJobs) {
        sprintf_s(tag, "immediate %s", j.name);
        detour(slot_fn(ctx, j.slot), j.kind, 0, tag);
    }

    // The deferred-context class is a different implementation in this runtime
    // (verified offline: immediate DrawIndexed and deferred DrawIndexed are
    // different RVAs), so resolve it from a throwaway deferred context.
    if (dev) {
        ID3D11DeviceContext* dc = nullptr;
        const HRESULT hr = dev->CreateDeferredContext(0, &dc);
        if (SUCCEEDED(hr) && dc) {
            for (const Job& j : kJobs) {
                sprintf_s(tag, "deferred  %s", j.name);
                detour(slot_fn(dc, j.slot), j.kind, 1, tag);
            }
            dc->Release();
        } else {
            LOG_WARN("draw: CreateDeferredContext failed (0x%08X); only the "
                     "immediate-class functions are detoured.", (unsigned)hr);
        }
    }

    LOG_INFO("draw: %d function(s) detoured in d3d11.dll. This catches EVERY "
             "context object, whenever it was created.", g_ntargets);
    return g_ntargets;
}

void restore() {
    for (int i = 0; i < g_ntargets; ++i) {
        Target& t = g_targets[i];
        DWORD prot = 0;
        if (VirtualProtect(t.fn, 8, PAGE_EXECUTE_READWRITE, &prot)) {
            int64_t orig = 0;
            memcpy(&orig, t.saved, sizeof(orig));
            InterlockedExchange64((volatile LONG64*)t.fn, orig);
            DWORD dummy = 0;
            VirtualProtect(t.fn, 8, prot, &dummy);
            FlushInstructionCache(GetCurrentProcess(), t.fn, 8);
        }
    }
    g_ntargets = 0;
    g_rec = nullptr;
}

void counts(uint64_t out[(int)Kind::kCount]) {
    for (int i = 0; i < (int)Kind::kCount; ++i)
        out[i] = g_counts[i].load(std::memory_order_relaxed);
}

}  // namespace drawhook
}  // namespace grwxr
