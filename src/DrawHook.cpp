// DrawHook.cpp - see DrawHook.h for why code detours replaced vtable patching.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

#include "Anatomy.h"
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
constexpr int kSlotOMSetRenderTargets   = 33;   // BUILD 216
constexpr int kSlotOMSetRTsAndUAVs      = 34;   // BUILD 216
// BUILD 241 (slots counted from d3d11.h and d3d11_1.h: Map 14, Unmap 15,
// UpdateSubresource 48, ExecuteCommandList 58, FinishCommandList 114, and
// ID3D11DeviceContext1::UpdateSubresource1 116; the immediate context carries the
// 11.1 vtable, verified by QueryInterface on this machine).
constexpr int kSlotMap                  = 14;
constexpr int kSlotUnmap                = 15;
constexpr int kSlotUpdateSubresource    = 48;
constexpr int kSlotExecuteCommandList   = 58;
constexpr int kSlotFinishCommandList    = 114;
constexpr int kSlotUpdateSubresource1   = 116;

constexpr size_t kMinSteal   = 8;    // one aligned atomic store
constexpr size_t kMaxSteal   = 24;
constexpr int    kMaxTargets = 64;   // 6 draw kinds + 2 render-target binds (BUILD 216) + 6 anatomy functions (BUILD 241), x 2 context classes

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
    // BUILD 241: the two spills ExecuteCommandList and FinishCommandList start
    // with (probed offline 2026-10-05): mov [rsp+disp8], r8d ; mov [rsp+disp8], edx.
    {5, 4, {0x44, 0x89, 0x44, 0x24}}, {4, 3, {0x89, 0x54, 0x24}},
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
bool   g_draw_done  = false;   // install() ran (whatever it detoured)
int    g_draw_count = 0;
bool   g_rt_done    = false;   // BUILD 216: install_rt() ran
int    g_rt_count   = 0;

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

FnDrawIndexed g_t_di[5]    = {};
FnDraw        g_t_d[5]     = {};
FnDII         g_t_dii[5]   = {};
FnDInst       g_t_dinst[5] = {};
FnDisp        g_t_disp[5]  = {};
FnDispInd     g_t_dispi[5] = {};

// BUILD 216: the render-target binds. Same two-class shape as the draws.
using FnOMRT  = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                         ID3D11RenderTargetView* const*,
                                         ID3D11DepthStencilView*);
using FnOMRTU = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                         ID3D11RenderTargetView* const*,
                                         ID3D11DepthStencilView*, UINT, UINT,
                                         ID3D11UnorderedAccessView* const*,
                                         const UINT*);
FnOMRT     g_t_omrt[5]  = {};
FnOMRTU    g_t_omrtu[5] = {};
RtRecorder g_rt_rec     = nullptr;

inline void tally(Kind k) {
    g_counts[(int)k].fetch_add(1, std::memory_order_relaxed);
}

template <int N>
void STDMETHODCALLTYPE hk_di(ID3D11DeviceContext* c, UINT i, UINT s, INT b) {
    tally(Kind::DrawIndexed);
    if (anatomy::capturing()) anatomy::tap_draw(c);   // BUILD 241
    if (g_rec) g_rec(c, Kind::DrawIndexed, i);
    g_t_di[N](c, i, s, b);
}
template <int N>
void STDMETHODCALLTYPE hk_d(ID3D11DeviceContext* c, UINT v, UINT s) {
    tally(Kind::Draw);
    if (anatomy::capturing()) anatomy::tap_draw(c);   // BUILD 241
    if (g_rec) g_rec(c, Kind::Draw, v);
    g_t_d[N](c, v, s);
}
template <int N>
void STDMETHODCALLTYPE hk_dii(ID3D11DeviceContext* c, UINT i, UINT n, UINT s,
                              INT b, UINT si) {
    tally(Kind::DrawIndexedInstanced);
    if (anatomy::capturing()) anatomy::tap_draw(c);   // BUILD 241
    if (g_rec) g_rec(c, Kind::DrawIndexedInstanced, i);
    g_t_dii[N](c, i, n, s, b, si);
}
template <int N>
void STDMETHODCALLTYPE hk_dinst(ID3D11DeviceContext* c, UINT v, UINT n, UINT s,
                                UINT si) {
    tally(Kind::DrawInstanced);
    if (anatomy::capturing()) anatomy::tap_draw(c);   // BUILD 241
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

// BUILD 216: the recorder runs AFTER the real bind, so the state it acts on
// (the shading rate for the target just bound) is set against the target
// the draws that follow will use.
template <int N>
void STDMETHODCALLTYPE hk_omrt(ID3D11DeviceContext* c, UINT n,
                               ID3D11RenderTargetView* const* r,
                               ID3D11DepthStencilView* d) {
    g_t_omrt[N](c, n, r, d);
    if (anatomy::capturing()) anatomy::tap_rt(c, n, r, d);   // BUILD 241
    if (g_rt_rec) g_rt_rec(c, n, r);
}
template <int N>
void STDMETHODCALLTYPE hk_omrtu(ID3D11DeviceContext* c, UINT n,
                                ID3D11RenderTargetView* const* r,
                                ID3D11DepthStencilView* d, UINT us, UINT nu,
                                ID3D11UnorderedAccessView* const* u,
                                const UINT* ic) {
    g_t_omrtu[N](c, n, r, d, us, nu, u, ic);
    if (anatomy::capturing()) anatomy::tap_rt(c, n, r, d);   // BUILD 241
    if (g_rt_rec) g_rt_rec(c, n, r);
}

// BUILD 241: the frame-anatomy hooks (Anatomy.h). Same two-class shape. The taps
// are gated by anatomy::capturing(), one relaxed load, so outside the window
// each hook costs a load and a branch on top of the trampoline.
using FnMap    = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                             D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using FnUnmap  = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using FnUpdate = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                          const D3D11_BOX*, const void*, UINT, UINT);
using FnUpdate1 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                           const D3D11_BOX*, const void*, UINT, UINT, UINT);
using FnExec   = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using FnFinish = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
FnMap     g_t_map[5]     = {};
FnUnmap   g_t_unmap[5]   = {};
FnUpdate  g_t_upd[5]     = {};
FnUpdate1 g_t_upd1[5]    = {};
FnExec    g_t_exec[5]    = {};
FnFinish  g_t_finish[5]  = {};

template <int N>
HRESULT STDMETHODCALLTYPE hk_map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub,
                                 D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* ms) {
    const HRESULT hr = g_t_map[N](c, r, sub, type, flags, ms);
    if (anatomy::capturing()) anatomy::tap_map(c, r, sub, type, ms, hr);
    return hr;
}
template <int N>
void STDMETHODCALLTYPE hk_unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (anatomy::capturing()) anatomy::tap_unmap(c, r, sub);   // BEFORE: the mapped data is still valid
    g_t_unmap[N](c, r, sub);
}
template <int N>
void STDMETHODCALLTYPE hk_upd(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub,
                              const D3D11_BOX* box, const void* src, UINT rp, UINT dp) {
    if (anatomy::capturing()) anatomy::tap_update(c, r, sub, box, src);
    g_t_upd[N](c, r, sub, box, src, rp, dp);
}
template <int N>
void STDMETHODCALLTYPE hk_upd1(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub,
                               const D3D11_BOX* box, const void* src, UINT rp, UINT dp, UINT cf) {
    if (anatomy::capturing()) anatomy::tap_update(c, r, sub, box, src);
    g_t_upd1[N](c, r, sub, box, src, rp, dp, cf);
}
template <int N>
void STDMETHODCALLTYPE hk_exec(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore) {
    const bool cap = anatomy::capturing();
    const uint64_t t0 = cap ? __rdtsc() : 0;
    if (cap) anatomy::exec_enter();
    g_t_exec[N](c, cl, restore);
    if (cap) { anatomy::exec_leave(); anatomy::tap_execute(c, cl, restore, t0); }
}
template <int N>
HRESULT STDMETHODCALLTYPE hk_finish(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** pp) {
    const bool cap = anatomy::capturing();
    const uint64_t t0 = cap ? __rdtsc() : 0;
    const HRESULT hr = g_t_finish[N](c, restore, pp);
    if (cap) anatomy::tap_finish(c, restore, (SUCCEEDED(hr) && pp) ? *pp : nullptr, hr, t0);
    return hr;
}

// The machinery for one detour: whitelist the prologue, build the trampoline
// (stolen bytes plus a jump back) and the bridge (an absolute jump to our
// hook), store the trampoline where the hook reads it BEFORE the entry is
// patched (the hook may run at once on another thread), then patch the entry
// atomically. Returns true if fn is now detoured, or already was: the two
// context classes may share an implementation, in which case the first hook
// serves both and tramp_slot for the second is left untouched.
bool detour_at(uint8_t* fn, void* hook, void** tramp_slot, const char* tag) {
    if (!fn) return false;
    for (int i = 0; i < g_ntargets; ++i)
        if (g_targets[i].fn == fn) return true;   // shared implementation
    if (g_ntargets >= kMaxTargets) {
        LOG_ERROR("draw: target table full, refusing %s.", tag);
        return false;
    }

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

    *tramp_slot = (void*)tramp;   // before the patch: the hook may run at once
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

// The draw kinds: the hook and the trampoline slot for this kind and context
// class (0 immediate, 1 deferred), then the machinery above.
bool detour(uint8_t* fn, Kind kind, int set, const char* tag) {
    void*  hook = nullptr;
    void** slot = nullptr;
    switch (kind) {
        case Kind::DrawIndexed:
            slot = (void**)&g_t_di[set];
            hook = set ? (void*)&hk_di<1> : (void*)&hk_di<0>;
            break;
        case Kind::Draw:
            slot = (void**)&g_t_d[set];
            hook = set ? (void*)&hk_d<1> : (void*)&hk_d<0>;
            break;
        case Kind::DrawIndexedInstanced:
            slot = (void**)&g_t_dii[set];
            hook = set ? (void*)&hk_dii<1> : (void*)&hk_dii<0>;
            break;
        case Kind::DrawInstanced:
            slot = (void**)&g_t_dinst[set];
            hook = set ? (void*)&hk_dinst<1> : (void*)&hk_dinst<0>;
            break;
        case Kind::Dispatch:
            slot = (void**)&g_t_disp[set];
            hook = set ? (void*)&hk_disp<1> : (void*)&hk_disp<0>;
            break;
        case Kind::DispatchIndirect:
            slot = (void**)&g_t_dispi[set];
            hook = set ? (void*)&hk_dispi<1> : (void*)&hk_dispi<0>;
            break;
        default:
            return false;
    }
    return detour_at(fn, hook, slot, tag);
}

uint8_t* slot_fn(void* obj, int slot) {
    if (!obj) return nullptr;
    void** vt = *(void***)obj;
    return vt ? (uint8_t*)vt[slot] : nullptr;
}

}  // namespace

int install(ID3D11Device* dev, ID3D11DeviceContext* ctx, Recorder rec) {
    if (rec) g_rec = rec;   // BUILD 241: a later caller still gets its recorder when install_anatomy ran first
    if (g_draw_done || !ctx) return g_draw_count;
    g_draw_done = true;
    const int before = g_ntargets;

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

    if (!g_arena) g_arena = alloc_near(anchor);   // shared with install_rt (BUILD 216)
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

    g_draw_count = g_ntargets - before;
    LOG_INFO("draw: %d function(s) detoured in d3d11.dll. This catches EVERY "
             "context object, whenever it was created.", g_draw_count);
    return g_draw_count;
}

// BUILD 216: the render-target binds, for the foveation mask (Foveate.cpp).
// The same machinery as the draws, on the two implementations (immediate and
// deferred class) of OMSetRenderTargets and
// OMSetRenderTargetsAndUnorderedAccessViews, read from a live context and a
// throwaway deferred context. Their prologues were probed offline on this
// machine's d3d11.dll (2026-09-07: push runs, 40 53 55 56 57 41 54 41 55 and
// 40 55 53 56 57 41 54 41 55, nine bytes stolen each) and are on the
// whitelist; anything else is refused and logged, nothing patched (rule 7).
int install_rt(ID3D11Device* dev, ID3D11DeviceContext* ctx, RtRecorder rec) {
    if (rec) g_rt_rec = rec;   // BUILD 241: see install()
    if (g_rt_done || !ctx) return g_rt_count;
    g_rt_done = true;

    uint8_t* anchor = slot_fn(ctx, kSlotOMSetRenderTargets);
    if (!anchor) {
        LOG_ERROR("draw: could not read OMSetRenderTargets from the context vtable.");
        return 0;
    }
    if (!g_arena) g_arena = alloc_near(anchor);
    if (!g_arena) {
        LOG_ERROR("draw: no free page within 2 GB of d3d11.dll for the "
                  "bridges. Nothing patched.");
        return 0;
    }

    // Where the implementation lives, as install() reports it: with a
    // context-wrapping layer (ReShade) the slot can resolve outside d3d11.dll,
    // and then the whitelist is the only guard.
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    LOG_INFO("draw: d3d11.dll base 0x%p, OMSetRenderTargets impl 0x%p (RVA 0x%zX)",
             (void*)d3d11, (void*)anchor,
             d3d11 ? (size_t)(anchor - (uint8_t*)d3d11) : (size_t)0);

    // All four or nothing: a partial set would miss the binds through the
    // unhooked function (a stale rate carried into a pass, or the scene's
    // deferred binds never masked) and the verdict would read wrong for the
    // wrong reason. On any refusal the count is -1; what was patched passes
    // through untouched while the recorder's owner stays inactive.
    const int before = g_ntargets;
    bool ok = true;
    if (!detour_at(slot_fn(ctx, kSlotOMSetRenderTargets), (void*)&hk_omrt<0>,
                   (void**)&g_t_omrt[0], "immediate OMSetRenderTargets")) ok = false;
    if (!detour_at(slot_fn(ctx, kSlotOMSetRTsAndUAVs), (void*)&hk_omrtu<0>,
                   (void**)&g_t_omrtu[0], "immediate OMSetRTsAndUAVs")) ok = false;
    if (dev) {
        ID3D11DeviceContext* dc = nullptr;
        const HRESULT hr = dev->CreateDeferredContext(0, &dc);
        if (SUCCEEDED(hr) && dc) {
            if (!detour_at(slot_fn(dc, kSlotOMSetRenderTargets), (void*)&hk_omrt<1>,
                           (void**)&g_t_omrt[1], "deferred  OMSetRenderTargets")) ok = false;
            if (!detour_at(slot_fn(dc, kSlotOMSetRTsAndUAVs), (void*)&hk_omrtu<1>,
                           (void**)&g_t_omrtu[1], "deferred  OMSetRTsAndUAVs")) ok = false;
            dc->Release();
        } else {
            LOG_WARN("draw: CreateDeferredContext failed (0x%08X); the deferred-class "
                     "bind functions cannot be resolved, so the set is incomplete.", (unsigned)hr);
            ok = false;
        }
    } else {
        ok = false;
    }
    g_rt_count = ok ? g_ntargets - before : -1;
    LOG_INFO("draw: %d render-target bind function(s) detoured in d3d11.dll "
             "(build 216: the foveation mask rides these; -1 = incomplete, the owner stays off).", g_rt_count);
    return g_rt_count;
}

// BUILD 241: the frame-anatomy hook set. See DrawHook.h.
//
// THE FACT THIS IS BUILT AROUND (measured 2026-10-05 with variants.exe, on both a
// hardware and a WARP device): the IMMEDIATE context does not have one set of
// entry points. ID3D11DeviceContext::Flush rewrites the context's own heap vtable
// so that DrawIndexed, Draw, DrawIndexedInstanced, DrawInstanced, UpdateSubresource
// and UpdateSubresource1 point at a SECOND set of functions in d3d11.dll, and the
// next ExecuteCommandList puts the first set back. A detour on whichever set the
// vtable shows at install time therefore misses every immediate draw made in the
// other state (the first harness run saw dI = 0 for exactly this reason). So this
// installer samples the live vtable for a while and detours EVERY distinct entry
// it sees per slot, in both context classes, and Anatomy.cpp watches at Present
// that the vtable still only points at hooked entries.
//
// ALL OR NOTHING: every distinct target is decoded against the whitelist BEFORE
// the first byte is written; any refusal returns -1 with d3d11.dll untouched.
namespace {
struct AnRow {
    int         slot;
    const char* name;
    void*       hook[5];     // by trampoline index N
    void**      tramp[5];
};
#define AN_ROW(S, NAME, H, A)                                                                  \
    { S, NAME, { (void*)&H<0>, (void*)&H<1>, (void*)&H<2>, (void*)&H<3>, (void*)&H<4> },      \
      { (void**)&A[0], (void**)&A[1], (void**)&A[2], (void**)&A[3], (void**)&A[4] } }
const AnRow kAnRows[] = {
    AN_ROW(kSlotDrawIndexed,          "DrawIndexed",          hk_di,     g_t_di),
    AN_ROW(kSlotDraw,                 "Draw",                 hk_d,      g_t_d),
    AN_ROW(kSlotDrawIndexedInstanced, "DrawIndexedInstanced", hk_dii,    g_t_dii),
    AN_ROW(kSlotDrawInstanced,        "DrawInstanced",        hk_dinst,  g_t_dinst),
    AN_ROW(kSlotOMSetRenderTargets,   "OMSetRenderTargets",   hk_omrt,   g_t_omrt),
    AN_ROW(kSlotOMSetRTsAndUAVs,      "OMSetRTsAndUAVs",      hk_omrtu,  g_t_omrtu),
    AN_ROW(kSlotMap,                  "Map",                  hk_map,    g_t_map),
    AN_ROW(kSlotUnmap,                "Unmap",                hk_unmap,  g_t_unmap),
    AN_ROW(kSlotUpdateSubresource,    "UpdateSubresource",    hk_upd,    g_t_upd),
    AN_ROW(kSlotUpdateSubresource1,   "UpdateSubresource1",   hk_upd1,   g_t_upd1),
    AN_ROW(kSlotExecuteCommandList,   "ExecuteCommandList",   hk_exec,   g_t_exec),
    AN_ROW(kSlotFinishCommandList,    "FinishCommandList",    hk_finish, g_t_finish),
};
#undef AN_ROW
constexpr int kAnRowCount = (int)(sizeof(kAnRows) / sizeof(kAnRows[0]));
// Trampoline index per variant: immediate class 0, 2, 3; deferred class 1, 4.
constexpr int kImmN[3] = {0, 2, 3};
constexpr int kDefN[2] = {1, 4};
constexpr DWORD kSampleMs = 1500;

bool an_note(uint8_t** list, int& n, int cap, uint8_t* fn) {   // false when the list overflowed
    if (!fn) return true;
    for (int i = 0; i < n; ++i)
        if (list[i] == fn) return true;
    if (n >= cap) return false;
    list[n++] = fn;
    return true;
}
}  // namespace

int install_anatomy(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!dev || !ctx) {
        LOG_ERROR("anatomy: no device or context, nothing patched.");
        return -1;
    }
    uint8_t* anchor = slot_fn(ctx, kSlotDrawIndexed);
    if (!anchor) {
        LOG_ERROR("anatomy: could not read DrawIndexed from the context vtable, nothing patched.");
        return -1;
    }
    ID3D11DeviceContext* dc = nullptr;
    const HRESULT hr = dev->CreateDeferredContext(0, &dc);
    if (FAILED(hr) || !dc) {
        LOG_ERROR("anatomy: CreateDeferredContext failed (0x%08X); the deferred class cannot be resolved, "
                  "nothing patched.", (unsigned)hr);
        return -1;
    }
    if (!g_arena) g_arena = alloc_near(anchor);
    if (!g_arena) {
        LOG_ERROR("anatomy: no free page within 2 GB of d3d11.dll for the bridges. Nothing patched.");
        dc->Release();
        return -1;
    }

    // Phase 0: sample the live vtables (read only) and collect every distinct entry
    // per slot. The immediate context flips between sets on Flush and
    // ExecuteCommandList, so one read is not enough.
    uint8_t* imm[kAnRowCount][4] = {};
    uint8_t* def[kAnRowCount][4] = {};
    int nimm[kAnRowCount] = {};
    int ndef[kAnRowCount] = {};
    bool overflow = false;
    const ULONGLONG t_end = GetTickCount64() + kSampleMs;
    unsigned long long samples = 0;
    do {
        for (int r = 0; r < kAnRowCount; ++r) {
            overflow |= !an_note(imm[r], nimm[r], 3, slot_fn(ctx, kAnRows[r].slot));
            overflow |= !an_note(def[r], ndef[r], 2, slot_fn(dc, kAnRows[r].slot));
        }
        ++samples;
        for (int i = 0; i < 40; ++i) YieldProcessor();
    } while (GetTickCount64() < t_end);
    LOG_INFO("anatomy: sampled the context vtables %llu times over %lu ms.", samples, (unsigned long)kSampleMs);
    for (int r = 0; r < kAnRowCount; ++r)
        LOG_INFO("anatomy:   %-22s slot %3d: %d immediate entry point(s), %d deferred%s", kAnRows[r].name,
                 kAnRows[r].slot, nimm[r], ndef[r], nimm[r] > 1 ? "   <-- the immediate context flipped sets" : "");
    if (overflow) {
        LOG_ERROR("anatomy: more distinct entry points than the hook table holds (immediate 3, deferred 2). Nothing patched.");
        dc->Release();
        return -1;
    }

    // Phase 1: decode everything, write nothing.
    bool ok = true;
    for (int r = 0; r < kAnRowCount; ++r) {
        for (int cls = 0; cls < 2; ++cls) {
            const int n = cls ? ndef[r] : nimm[r];
            if (n == 0) {
                LOG_ERROR("anatomy: %s %s did not resolve.", cls ? "deferred" : "immediate", kAnRows[r].name);
                ok = false;
            }
            for (int k = 0; k < n; ++k) {
                uint8_t* fn = cls ? def[r][k] : imm[r][k];
                bool have = false;
                for (int i = 0; i < g_ntargets; ++i)
                    if (g_targets[i].fn == fn) have = true;
                if (have) continue;   // already detoured (shared, or installed by another caller)
                if (((uintptr_t)fn & 7) != 0) {
                    LOG_ERROR("anatomy: %s %s entry 0x%p is not 8-byte aligned.", cls ? "deferred" : "immediate",
                              kAnRows[r].name, (void*)fn);
                    ok = false;
                } else if (!steal_len(fn)) {
                    LOG_ERROR("anatomy: %s %s prologue not on the whitelist. First 16 bytes: %02X %02X %02X %02X %02X "
                              "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                              cls ? "deferred" : "immediate", kAnRows[r].name, fn[0], fn[1], fn[2], fn[3], fn[4],
                              fn[5], fn[6], fn[7], fn[8], fn[9], fn[10], fn[11], fn[12], fn[13], fn[14], fn[15]);
                    ok = false;
                }
            }
        }
    }
    if (!ok) {
        LOG_ERROR("anatomy: at least one target was refused, so NOTHING was written (rule 7).");
        dc->Release();
        return -1;
    }

    // Phase 2: patch.
    const int before = g_ntargets;
    char tag[96];
    bool all = true;
    for (int r = 0; r < kAnRowCount; ++r) {
        const AnRow& row = kAnRows[r];
        for (int k = 0; k < nimm[r]; ++k) {
            sprintf_s(tag, "anatomy immediate %s v%d", row.name, k);
            all &= detour_at(imm[r][k], row.hook[kImmN[k]], row.tramp[kImmN[k]], tag);
        }
        for (int k = 0; k < ndef[r]; ++k) {
            sprintf_s(tag, "anatomy deferred  %s v%d", row.name, k);
            all &= detour_at(def[r][k], row.hook[kDefN[k]], row.tramp[kDefN[k]], tag);
        }
    }
    dc->Release();
    const int added = g_ntargets - before;
    if (!all) {
        // Phase 1 passed, so this is a VirtualProtect or arena failure. What was
        // patched is a complete detour with a live trampoline and passes through.
        LOG_ERROR("anatomy: a detour failed AFTER every target decoded (see above); %d function(s) were patched and "
                  "pass through. No capture will run.", added);
        return -1;
    }
    LOG_INFO("anatomy: %d function(s) newly detoured in d3d11.dll (every distinct entry point seen in both context "
             "classes: draws, render-target binds, Map/Unmap/UpdateSubresource(1), Execute/FinishCommandList). The taps "
             "are inert until the window opens.", added);
    return added;
}

// BUILD 241: the Present-time check that the immediate context's vtable still
// points only at hooked entries. Plain reads, no COM, render thread safe.
// Returns the first slot whose entry is NOT detoured (and the entry in *bad), or -1.
int anatomy_vtable_unhooked(ID3D11DeviceContext* ctx, void** bad) {
    if (!ctx) return -1;
    for (int r = 0; r < kAnRowCount; ++r) {
        uint8_t* fn = slot_fn(ctx, kAnRows[r].slot);
        bool have = false;
        for (int i = 0; i < g_ntargets; ++i)
            if (g_targets[i].fn == fn) { have = true; break; }
        if (!have) {
            if (bad) *bad = fn;
            return kAnRows[r].slot;
        }
    }
    return -1;
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
    g_rt_rec = nullptr;
    g_draw_done = g_rt_done = false;
    g_draw_count = g_rt_count = 0;
}

void counts(uint64_t out[(int)Kind::kCount]) {
    for (int i = 0; i < (int)Kind::kCount; ++i)
        out[i] = g_counts[i].load(std::memory_order_relaxed);
}

}  // namespace drawhook
}  // namespace grwxr
