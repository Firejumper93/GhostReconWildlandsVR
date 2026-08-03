// FactoryHook.cpp - build 15a: swapchain upsize at the DXGI factory seam.
//
// WHY THIS EXISTS. Session 13 proved the fullscreen blur is capture
// resolution: the mod captures the backbuffer, and a 3840x2160 backbuffer
// fixed the blur, but only by setting the desktop to 4K via DSR before
// launch, which is not a shippable user requirement. This build reproduces
// the 4K-backbuffer condition from inside the process: intercept the game's
// swapchain creation and rewrite the requested size. The window stays at
// desktop size; DXGI stretches the backbuffer into it at Present, and the VR
// capture reads the full-size pixels.
//
// THE SEAM. GRW.exe imports neither dxgi.dll nor d3d11.dll statically
// (docs/RAW/pe-inventory-GRW.txt): everything resolves at runtime against the
// loaded module named dxgi.dll, which is this proxy. Until this build the
// CreateDXGIFactory* exports were pure linker forwards to dxgi_real.dll, so
// factory creation never touched our code. proxy_exports.inc now aliases
// those three exports (same names, same ordinals) to the grwxr_* functions at
// the bottom of this file. Whether the game calls them directly or through
// d3d11.dll, the call lands here, and the factory we hand back has its
// CreateSwapChain slot patched before the game ever sees it.
//
// KNOWN OPEN QUESTION THIS BUILD ANSWERS. Does the engine size its internal
// render targets from the actual backbuffer (full-resolution render, blur
// fixed) or from its own validated display resolution (a 1080p viewport in a
// corner of the 4K buffer)? The headset test discriminates immediately. If
// the corner outcome happens, the fallback design is spoofing the monitor
// size APIs the game imports (GetMonitorInfoA, GetSystemMetrics,
// MonitorFromWindow) so its own validation accepts 4K.
//
// SAFETY. Every hook here carries a thread_local re-entrancy guard and passes
// straight through when re-entered: DXGI re-enters hooked vtable slots from
// inside its own implementation (the documented 2026-07-28 ResizeBuffers
// crash, which was an UNGUARDED hook). If an upsized creation or resize
// fails, the hook retries at the requested size, so the worst case is the
// old 1080p behaviour. If the stubs never fire, nothing was patched and the
// game runs unmodified; report_startup() says so loudly.
//
// BUILD 15b: ResizeBuffers is hooked after all. The 15a run proved the need:
// the game's swapchain creation WAS upsized (log: 1920x1061 -> 3840x2160),
// then the game's own post-create ResizeBuffers put the buffers back at
// window size before the first Present (captured backbuffer 1920x1080).
// The do-not-hook entry for ResizeBuffers is retired with evidence in
// docs/RE-notes.md: the 07-28 crash signature (~1400 identical log lines in
// one millisecond) is unguarded self-re-entry, and the guard that fixes it
// has run in the Present hook since the same day. The resize hook rewrites
// sizes exactly like the creation hook, including the (0, 0) fit-the-window
// form, and its slot is patched from inside the creation hook, so it is
// armed before the game's first post-create resize can run.
//
// These functions can run before the init thread has called log::init, so
// nothing here writes the log directly: events go into a fixed ring drained
// by the heartbeat loop. File I/O (the cfg read) happens once, at creation
// time, never per frame (rule 8 concerns Present and per-draw paths).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "FactoryHook.h"
#include "Log.h"

namespace grwxr {
namespace factory {
namespace {

// ---------------------------------------------------------------------------
// Buffered event log. Writers may run before log::init; the drain side runs
// on the init thread's heartbeat. Fixed slots, overflow counted, never lost
// silently.

struct Event {
    std::atomic<int> ready{0};
    char text[224] = {};
};

Event                 g_events[24];
std::atomic<unsigned> g_widx{0};
unsigned              g_ridx = 0;   // drain thread only
std::atomic<unsigned> g_dropped{0};

void note(const char* fmt, ...) {
    const unsigned i = g_widx.fetch_add(1, std::memory_order_acq_rel);
    if (i >= _countof(g_events)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_events[i].text, sizeof(g_events[i].text), fmt, ap);
    va_end(ap);
    g_events[i].ready.store(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Config. upsize_width / upsize_height in GRWVR\grwxr.cfg; either 0 disables.
// Defaults ON at 3840x2160, the session-13 proven-good capture size. Read
// once, lazily, at the first factory call; log::data_dir() may not exist yet,
// so the path is derived from this module's own location.

std::atomic<int> g_cfg_state{0};   // 0 unread, 1 reading, 2 ready
UINT g_up_w = 3840;
UINT g_up_h = 2160;

void load_cfg_once() {
    int expect = 0;
    if (!g_cfg_state.compare_exchange_strong(expect, 1, std::memory_order_acq_rel)) {
        while (g_cfg_state.load(std::memory_order_acquire) != 2) Sleep(0);
        return;
    }

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&g_widx, &self);
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(self, p, MAX_PATH);
    std::wstring dir = p;
    size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\GRWVR\\grwxr.cfg").c_str(), L"rt") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') continue;
            unsigned v = 0;
            if (sscanf_s(line, " upsize_width = %u", &v) == 1) {
                if (v != 0 && v < 1280) v = 1280;
                if (v > 7680) v = 7680;
                g_up_w = v;
            }
            if (sscanf_s(line, " upsize_height = %u", &v) == 1) {
                if (v != 0 && v < 720) v = 720;
                if (v > 4320) v = 4320;
                g_up_h = v;
            }
        }
        fclose(f);
    }
    g_cfg_state.store(2, std::memory_order_release);
}

bool upsize_enabled() { return g_up_w != 0 && g_up_h != 0; }

// Only the game process gets the override and the vtable patch. The proxy is
// also picked up by launcher and helper processes; they forward untouched.
bool host_is_game() {
    static int cached = -1;
    if (cached < 0) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring s = exe;
        for (auto& c : s) c = (wchar_t)towlower(c);
        cached = (s.find(L"grw.exe") != std::wstring::npos) ? 1 : 0;
    }
    return cached == 1;
}

// ---------------------------------------------------------------------------
// The real dxgi. deploy.bat places a copy of System32\dxgi.dll as
// dxgi_real.dll beside the game executable, next to this proxy, so the plain
// module name resolves there first.

FARPROC real_export(const char* name) {
    static HMODULE real = LoadLibraryW(L"dxgi_real.dll");
    return real ? GetProcAddress(real, name) : nullptr;
}

// ---------------------------------------------------------------------------
// The CreateSwapChain vtable patch.
//
// IDXGIFactory vtable: IUnknown 0-2, IDXGIObject 3-6, 7 EnumAdapters,
// 8 MakeWindowAssociation, 9 GetWindowAssociation, 10 CreateSwapChain,
// 11 CreateSoftwareAdapter.
constexpr int kCreateSwapChainSlot = 10;

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
                                                      DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);

// The three factory exports may return objects with distinct vtables (old and
// new factory interfaces). Each patched vtable keeps its own original.
struct Patched {
    void** vt   = nullptr;
    void*  orig = nullptr;
};
Patched          g_patched[4];
std::atomic<int> g_npatched{0};
SRWLOCK          g_patch_lock = SRWLOCK_INIT;

std::atomic<unsigned> g_stub_calls{0};      // CreateDXGIFactory* invocations
std::atomic<unsigned> g_create_calls{0};    // CreateSwapChain invocations seen

CreateSwapChainFn orig_for(void** vt) {
    const int n = g_npatched.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_patched[i].vt == vt) return (CreateSwapChainFn)g_patched[i].orig;
    return nullptr;
}

// Re-entrancy guard, same lesson as the Present hook: DXGI re-enters hooked
// slots from inside its own implementation.
thread_local int g_in_hook = 0;
struct ReentryGuard {
    bool ok;
    ReentryGuard() : ok(g_in_hook == 0) { ++g_in_hook; }
    ~ReentryGuard() { --g_in_hook; }
};

// The presentation-swapchain filter. The game requests its real backbuffer
// explicitly sized (1904x1071, 1920x1061, 1920x1080 in past logs) or lets
// DXGI derive it from the window (0x0, also the common ResizeBuffers form).
// Small utility swapchains, like our own 8x8 vtable dummy in D3D11Hook, must
// pass through untouched.
bool size_is_presentation(UINT w) { return w == 0 || w >= 1280; }

bool should_upsize(const DXGI_SWAP_CHAIN_DESC& d) {
    if (!upsize_enabled() || !host_is_game()) return false;
    if (!d.Windowed) return false;   // exclusive fullscreen sizes are modes, not ours to invent
    if (!size_is_presentation(d.BufferDesc.Width)) return false;
    if (d.BufferDesc.Width == g_up_w && d.BufferDesc.Height == g_up_h) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Build 15b: the ResizeBuffers hook. IDXGISwapChain vtable: 8 Present,
// 9 GetBuffer, 10 SetFullscreenState, 11 GetFullscreenState, 12 GetDesc,
// 13 ResizeBuffers.
constexpr int kResizeBuffersSlot = 13;

using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);

Patched          g_rb_patched[4];
std::atomic<int> g_rb_npatched{0};

ResizeBuffersFn rb_orig_for(void** vt) {
    const int n = g_rb_npatched.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_rb_patched[i].vt == vt) return (ResizeBuffersFn)g_rb_patched[i].orig;
    return nullptr;
}

HRESULT STDMETHODCALLTYPE hooked_resize_buffers(IDXGISwapChain* sc, UINT count, UINT w,
                                                UINT h, DXGI_FORMAT fmt, UINT flags) {
    ResizeBuffersFn orig = rb_orig_for(*(void***)sc);
    if (!orig) {
        auto fallback = (ResizeBuffersFn)g_rb_patched[0].orig;
        return fallback ? fallback(sc, count, w, h, fmt, flags) : E_FAIL;
    }

    ReentryGuard guard;
    if (!guard.ok) return orig(sc, count, w, h, fmt, flags);   // DXGI re-entered: pass through

    if (!upsize_enabled() || !host_is_game() || !size_is_presentation(w) ||
        (w == g_up_w && h == g_up_h)) {
        return orig(sc, count, w, h, fmt, flags);
    }

    HRESULT hr = orig(sc, count, g_up_w, g_up_h, fmt, flags);
    if (SUCCEEDED(hr)) {
        note("factory: ResizeBuffers %ux%u UPSIZED -> %ux%u (bufs=%u fmt=%d)",
             w, h, g_up_w, g_up_h, count, (int)fmt);
        return hr;
    }
    note("factory: ResizeBuffers upsize %ux%u -> %ux%u FAILED hr=0x%08lX, retrying as requested",
         w, h, g_up_w, g_up_h, (unsigned long)hr);
    hr = orig(sc, count, w, h, fmt, flags);
    note("factory: ResizeBuffers fallback hr=0x%08lX", (unsigned long)hr);
    return hr;
}

// ---------------------------------------------------------------------------
// Build 15c: the GetClientRect spoof. The 15b run proved the backbuffer can
// be held at 4K (log: creation AND ResizeBuffers upsized, captured backbuffer
// 3840x2160) and the headset showed the content in the TOP-LEFT QUARTER: the
// engine sizes its render viewport from its own stored resolution, not from
// the backbuffer. That stored resolution tracks the window CLIENT size (old
// logs: 1904x1071 client -> 3808x2142 internal targets at Supersampling 2.0;
// both swapchain requests were client-rect-shaped, 1904x1071 and 1920x1061).
// So the game's GetClientRect import is patched in GRW.exe's IAT: windows of
// this process at presentation size report upsize_width x upsize_height, and
// the engine sizes everything from the lie. Our own modules import user32
// through their own IATs and keep seeing real sizes.

using GetClientRectFn = BOOL(WINAPI*)(HWND, LPRECT);
GetClientRectFn       g_gcr_real = nullptr;
std::atomic<unsigned> g_gcr_spoofs{0};

BOOL WINAPI spoofed_get_client_rect(HWND hwnd, LPRECT rc) {
    BOOL ok = g_gcr_real(hwnd, rc);
    if (!ok || !rc || !upsize_enabled()) return ok;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return ok;
    if (rc->right >= 1280 && rc->bottom >= 720 &&
        (rc->right != (LONG)g_up_w || rc->bottom != (LONG)g_up_h)) {
        // Can run per frame once the engine is up: first few only into the ring.
        const unsigned n = g_gcr_spoofs.fetch_add(1, std::memory_order_relaxed);
        if (n < 3) {
            note("factory: GetClientRect(0x%p) real %ldx%ld -> reported %ux%u",
                 (void*)hwnd, rc->right, rc->bottom, g_up_w, g_up_h);
        }
        rc->right  = (LONG)g_up_w;
        rc->bottom = (LONG)g_up_h;
    }
    return ok;
}

// Find the IAT slot of a named import in a module. Matches by function name
// across every import descriptor, so api-set forwarding of user32 does not
// matter. Returns null if the name is not imported.
void** find_iat_slot(HMODULE mod, const char* func) {
    auto base = (BYTE*)mod;
    auto dos  = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
         desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        auto oft = (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk);
        auto ft  = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) == 0) return (void**)&ft->u1.Function;
        }
    }
    return nullptr;
}

// Patch a created swapchain's ResizeBuffers slot, once per distinct vtable.
// Runs inside the creation hook, so the game's first post-create resize
// already lands on the hook.
void patch_resize(IDXGISwapChain* sc) {
    if (!sc || !host_is_game()) return;
    void** vt = *(void***)sc;
    AcquireSRWLockExclusive(&g_patch_lock);
    if (!rb_orig_for(vt)) {
        const int n = g_rb_npatched.load(std::memory_order_relaxed);
        if (n < (int)_countof(g_rb_patched) &&
            vt[kResizeBuffersSlot] != (void*)&hooked_resize_buffers) {
            DWORD old = 0;
            if (VirtualProtect(&vt[kResizeBuffersSlot], sizeof(void*), PAGE_READWRITE, &old)) {
                g_rb_patched[n].vt   = vt;
                g_rb_patched[n].orig = vt[kResizeBuffersSlot];
                g_rb_npatched.store(n + 1, std::memory_order_release);
                vt[kResizeBuffersSlot] = (void*)&hooked_resize_buffers;
                VirtualProtect(&vt[kResizeBuffersSlot], sizeof(void*), old, &old);
                note("factory: swapchain vtable 0x%p ResizeBuffers hooked (orig 0x%p, guarded)",
                     (void*)vt, (void*)g_rb_patched[n].orig);
            } else {
                note("factory: VirtualProtect FAILED on swapchain vtable 0x%p, "
                     "ResizeBuffers not hooked", (void*)vt);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_patch_lock);
}

HRESULT STDMETHODCALLTYPE hooked_create_swapchain(IDXGIFactory* self, IUnknown* device,
                                                  DXGI_SWAP_CHAIN_DESC* desc,
                                                  IDXGISwapChain** out) {
    CreateSwapChainFn orig = orig_for(*(void***)self);
    if (!orig) {
        // Unpatched vtable reached the hook: impossible by construction, but
        // never leave the game without a path forward.
        auto fallback = (CreateSwapChainFn)g_patched[0].orig;
        return fallback ? fallback(self, device, desc, out) : E_FAIL;
    }

    ReentryGuard guard;
    if (!guard.ok || !desc) return orig(self, device, desc, out);

    g_create_calls.fetch_add(1, std::memory_order_relaxed);
    load_cfg_once();

    if (!should_upsize(*desc)) {
        note("factory: swapchain pass-through %ux%u fmt=%d bufs=%u windowed=%d fx=%d",
             desc->BufferDesc.Width, desc->BufferDesc.Height,
             (int)desc->BufferDesc.Format, desc->BufferCount,
             (int)desc->Windowed, (int)desc->SwapEffect);
        HRESULT hr = orig(self, device, desc, out);
        // 15b: even a pass-through creation (the D3D11Hook dummy) exposes the
        // shared swapchain vtable; arming the resize hook here means it is in
        // place before the game's own swapchain exists at all.
        if (SUCCEEDED(hr) && out && *out) patch_resize(*out);
        return hr;
    }

    DXGI_SWAP_CHAIN_DESC up = *desc;
    up.BufferDesc.Width  = g_up_w;
    up.BufferDesc.Height = g_up_h;
    HRESULT hr = orig(self, device, &up, out);
    if (SUCCEEDED(hr)) {
        note("factory: UPSIZED swapchain %ux%u -> %ux%u (fmt=%d bufs=%u fx=%d hwnd=0x%p)",
             desc->BufferDesc.Width, desc->BufferDesc.Height, g_up_w, g_up_h,
             (int)desc->BufferDesc.Format, desc->BufferCount,
             (int)desc->SwapEffect, (void*)desc->OutputWindow);
        patch_resize(*out);
        return hr;
    }
    note("factory: upsize %ux%u -> %ux%u FAILED hr=0x%08lX, retrying at requested size",
         desc->BufferDesc.Width, desc->BufferDesc.Height, g_up_w, g_up_h,
         (unsigned long)hr);
    hr = orig(self, device, desc, out);
    note("factory: fallback create at requested size hr=0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) patch_resize(*out);
    return hr;
}

// Patch the factory's CreateSwapChain slot, once per distinct vtable.
void on_factory(void* factory, const char* via) {
    if (!factory || !host_is_game()) return;

    void** vt = *(void***)factory;
    AcquireSRWLockExclusive(&g_patch_lock);
    if (!orig_for(vt)) {
        const int n = g_npatched.load(std::memory_order_relaxed);
        if (n < (int)_countof(g_patched) &&
            vt[kCreateSwapChainSlot] != (void*)&hooked_create_swapchain) {
            DWORD old = 0;
            if (VirtualProtect(&vt[kCreateSwapChainSlot], sizeof(void*), PAGE_READWRITE, &old)) {
                g_patched[n].vt   = vt;
                g_patched[n].orig = (CreateSwapChainFn)vt[kCreateSwapChainSlot];
                g_npatched.store(n + 1, std::memory_order_release);
                vt[kCreateSwapChainSlot] = (void*)&hooked_create_swapchain;
                VirtualProtect(&vt[kCreateSwapChainSlot], sizeof(void*), old, &old);
                note("factory: %s -> vtable 0x%p CreateSwapChain patched (orig 0x%p)",
                     via, (void*)vt, (void*)g_patched[n].orig);
            } else {
                note("factory: %s -> VirtualProtect FAILED on vtable 0x%p, not patched",
                     via, (void*)vt);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_patch_lock);
}

}  // namespace

void install_render_size_spoof() {
    if (!host_is_game()) return;
    load_cfg_once();
    if (!upsize_enabled()) {
        LOG_INFO("factory: GetClientRect spoof skipped (upsize disabled by cfg)");
        return;
    }
    void** slot = find_iat_slot(GetModuleHandleW(nullptr), "GetClientRect");
    if (!slot) {
        LOG_WARN("factory: GetClientRect not found in GRW.exe's import table; spoof NOT "
                 "installed, engine will keep rendering at window size (15b quarter view)");
        return;
    }
    // The real pointer must be in place before any thread can enter the spoof.
    g_gcr_real = (GetClientRectFn)*slot;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG_WARN("factory: VirtualProtect failed on the GetClientRect IAT slot; spoof NOT installed");
        return;
    }
    *slot = (void*)&spoofed_get_client_rect;
    VirtualProtect(slot, sizeof(void*), old, &old);
    LOG_INFO("factory: GetClientRect IAT spoof installed (slot 0x%p, real 0x%p): this "
             "process's windows at >= 1280x720 report %u x %u to the game",
             (void*)slot, (void*)g_gcr_real, g_up_w, g_up_h);
}

void report_startup() {
    load_cfg_once();
    if (upsize_enabled()) {
        LOG_INFO("factory: swapchain upsize ARMED, target %u x %u "
                 "(upsize_width/upsize_height in grwxr.cfg, 0 disables)",
                 g_up_w, g_up_h);
    } else {
        LOG_INFO("factory: swapchain upsize DISABLED by cfg");
    }
    LOG_INFO("factory: stub calls so far=%u, CreateSwapChain seen=%u "
             "(0 stub calls all run = the game never used our factory exports; "
             "game unmodified, seam dead, see FactoryHook.cpp fallback design)",
             g_stub_calls.load(), g_create_calls.load());
}

void drain() {
    const unsigned w = g_widx.load(std::memory_order_acquire);
    while (g_ridx < w && g_ridx < _countof(g_events)) {
        Event& e = g_events[g_ridx];
        if (!e.ready.load(std::memory_order_acquire)) break;   // writer mid-slot
        LOG_INFO("%s", e.text);
        ++g_ridx;
    }
    unsigned dropped = g_dropped.exchange(0, std::memory_order_relaxed);
    if (dropped) LOG_WARN("factory: %u event(s) dropped (ring full)", dropped);
}

// ---------------------------------------------------------------------------
// The exported stubs. proxy_exports.inc aliases the public names to these
// (same ordinals as the real dxgi.dll), so both a direct game call and
// d3d11.dll's own import of dxgi.dll land here. extern "C" linkage is
// unaffected by the enclosing namespace, so the link-time names stay
// grwxr_CreateDXGIFactory*.

extern "C" HRESULT WINAPI grwxr_CreateDXGIFactory(REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    g_stub_calls.fetch_add(1, std::memory_order_relaxed);
    auto fn = (Fn)real_export("CreateDXGIFactory");
    if (!fn) return E_NOINTERFACE;   // dxgi_real.dll missing: mis-deploy
    HRESULT hr = fn(riid, out);
    if (SUCCEEDED(hr) && out && *out) on_factory(*out, "CreateDXGIFactory");
    return hr;
}

extern "C" HRESULT WINAPI grwxr_CreateDXGIFactory1(REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    g_stub_calls.fetch_add(1, std::memory_order_relaxed);
    auto fn = (Fn)real_export("CreateDXGIFactory1");
    if (!fn) return E_NOINTERFACE;
    HRESULT hr = fn(riid, out);
    if (SUCCEEDED(hr) && out && *out) on_factory(*out, "CreateDXGIFactory1");
    return hr;
}

extern "C" HRESULT WINAPI grwxr_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    g_stub_calls.fetch_add(1, std::memory_order_relaxed);
    auto fn = (Fn)real_export("CreateDXGIFactory2");
    if (!fn) return E_NOINTERFACE;
    HRESULT hr = fn(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) on_factory(*out, "CreateDXGIFactory2");
    return hr;
}

}  // namespace factory
}  // namespace grwxr
