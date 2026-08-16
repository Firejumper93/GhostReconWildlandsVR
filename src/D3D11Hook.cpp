#include "D3D11Hook.h"
#include "Log.h"

#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

namespace grwxr {
namespace d3d11 {
namespace {

// IDXGISwapChain vtable slots.
//   IUnknown            0 QueryInterface, 1 AddRef, 2 Release
//   IDXGIObject         3 SetPrivateData .. 6 GetParent
//   IDXGIDeviceSubObj   7 GetDevice
//   IDXGISwapChain      8 Present, 9 GetBuffer, 10 SetFullscreenState,
//                       11 GetFullscreenState, 12 GetDesc, 13 ResizeBuffers, ...
constexpr int kPresentSlot       = 8;
constexpr int kResizeBuffersSlot = 13;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_orig_present = nullptr;
ResizeFn  g_orig_resize  = nullptr;
void**    g_vtable       = nullptr;

State g_state;
std::atomic<PresentCallback> g_callback{nullptr};
std::atomic<bool> g_installed{false};

// Frames in flight (see D3D11Hook.h). The request is written by the cfg reader
// on the init thread and read on the init thread, but it is atomic so a future
// caller on another thread cannot tear it. The other three are touched only by
// poll_max_frame_latency, which is init-thread only, so they are plain.
std::atomic<int> g_flat_request{0};       // cfg value, 0 = leave the engine alone
int              g_flat_applied = -1;     // -1 = nothing decided yet
UINT             g_flat_original = 0;     // the engine's own value, for restore
bool             g_flat_have_original = false;

// Write one pointer into a read-only vtable.
bool patch_slot(void** vtable, int slot, void* fn, void** out_original) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (out_original) *out_original = vtable[slot];
    vtable[slot] = fn;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return true;
}

DXGI_SWAP_CHAIN_DESC g_desc{};   // stashed for the init thread to log

// Fill in the state struct. Called from Present, so it writes NO log lines.
void capture_quiet(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;

    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);

    g_desc            = desc;
    g_state.device    = dev;
    g_state.context   = ctx;
    g_state.swapchain = sc;
    g_state.width     = desc.BufferDesc.Width;
    g_state.height    = desc.BufferDesc.Height;
    g_state.format    = desc.BufferDesc.Format;
    g_state.hwnd      = desc.OutputWindow;
    g_state.ready     = true;
}

// Re-entrancy guard. DXGI can re-enter a hooked vtable slot from inside its own
// implementation, which turns a naive hook into unbounded recursion and a stack
// overflow. This is not theoretical: it is exactly what crashed the first
// version of this file (see docs/RE-notes.md, do-not-hook list).
thread_local int g_in_hook = 0;

struct ReentryGuard {
    bool ok;
    ReentryGuard() : ok(g_in_hook == 0) { ++g_in_hook; }
    ~ReentryGuard() { --g_in_hook; }
};

// Set by Present, drained and logged by the init thread. Present itself must not
// touch the log: project rule 8 forbids file I/O on a per-frame hook, and a
// flush under a critical section on the render thread is a real deadlock risk.
std::atomic<bool> g_capture_pending{false};

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    ReentryGuard guard;
    if (!guard.ok) {
        // Re-entered from inside DXGI. Pass straight through, do nothing.
        return g_orig_present(sc, sync, flags);
    }

    // Hot path. No logging, no allocation, no COM creation. Rule 8.
    if (!g_state.ready) {
        capture_quiet(sc);                 // fills the struct, writes no log
        g_capture_pending.store(true, std::memory_order_release);
    }
    g_state.frames++;

    if (auto cb = g_callback.load(std::memory_order_relaxed)) {
        cb(g_state);
    }

    return g_orig_present(sc, sync, flags);
}

// ResizeBuffers is not hooked HERE. This module still relies on Present
// re-reading the swapchain description when the cached size looks stale.
// Build 15b hooks ResizeBuffers in FactoryHook.cpp for the swapchain upsize,
// with the re-entrancy guard whose absence caused the 2026-07-28 recursion
// crash (docs/RE-notes.md, do-not-hook entry retired with evidence).

// Does this process own a visible top-level window? Used to tell the real game
// apart from the two transient launcher processes GRW.exe spawns.
BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
        RECT r{};
        GetClientRect(hwnd, &r);
        if ((r.right - r.left) > 200 && (r.bottom - r.top) > 200) {
            *(HWND*)lp = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

HWND find_our_window() {
    HWND found = nullptr;
    EnumWindows(enum_proc, (LPARAM)&found);
    return found;
}

// A window of OUR OWN for the throwaway swapchain below.
//
// Until v0.8.4 that swapchain was created on GetDesktopWindow(). DXGI rejects a
// swapchain on an HWND the calling process does not own, and the desktop window
// is owned by another process, so this was always a gamble. It paid off on every
// rig we had, and then issue #3 (2026-08-11, Ubisoft Connect + SteamVR) logged
// the losing side of it:
//     ERROR dummy swapchain creation failed: 0x80070005      (E_ACCESSDENIED)
//     ERROR cannot read the vtable, so no hooks installed.
// No vtable means no Present hook, which means no VR at all on that machine.
// Owning the window is what every other D3D hook does, and it costs nothing.
const wchar_t kDummyClassName[] = L"GRWXR_DummySwapchainWnd";
HWND g_dummy_wnd   = nullptr;
ATOM g_dummy_class = 0;

// Never shown: no ShowWindow, so it cannot flash on the user's desktop or steal
// focus from the game. It exists only long enough for DXGI to accept it.
HWND make_dummy_window() {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = inst;
    wc.lpszClassName = kDummyClassName;

    g_dummy_class = RegisterClassExW(&wc);
    if (!g_dummy_class && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LOG_WARN("dummy window: RegisterClassEx failed (err %lu)", GetLastError());
        return nullptr;
    }

    HWND w = CreateWindowExW(0, kDummyClassName, L"GRW-XR", WS_OVERLAPPEDWINDOW,
                             0, 0, 64, 64, nullptr, nullptr, inst, nullptr);
    if (!w) LOG_WARN("dummy window: CreateWindowEx failed (err %lu)", GetLastError());
    return w;
}

// Called on the same thread that created it, which DestroyWindow requires.
void destroy_dummy_window() {
    if (g_dummy_wnd) {
        DestroyWindow(g_dummy_wnd);
        g_dummy_wnd = nullptr;
    }
    if (g_dummy_class) {
        UnregisterClassW(kDummyClassName, GetModuleHandleW(nullptr));
        g_dummy_class = 0;
    }
}

}  // namespace

const State& state() { return g_state; }
void set_present_callback(PresentCallback cb) { g_callback.store(cb, std::memory_order_relaxed); }
bool has_present_callback() { return g_callback.load(std::memory_order_relaxed) != nullptr; }

bool install() {
    if (g_installed.load()) return true;

    // Wait for this process to actually own a render window. The transient
    // launcher processes never do, so they never reach the code below.
    LOG_INFO("waiting for a render window (filters out the transient GRW.exe processes)");
    HWND game_wnd = nullptr;
    for (int i = 0; i < 1200 && !game_wnd; ++i) {   // up to 60 s
        game_wnd = find_our_window();
        if (!game_wnd) Sleep(50);
    }
    if (!game_wnd) {
        LOG_WARN("no render window after 60s. This is probably a launcher process. Standing down.");
        return false;
    }
    RECT r{};
    GetClientRect(game_wnd, &r);
    LOG_INFO("render window found: 0x%p  %ld x %ld", (void*)game_wnd,
             r.right - r.left, r.bottom - r.top);

    // Create a throwaway swapchain purely to read the shared vtable. It renders
    // nothing and is released a few lines below; only its vtable pointer matters,
    // because every DXGI swapchain in the process shares it, including the game's.
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount        = 1;
    sd.BufferDesc.Width   = 8;
    sd.BufferDesc.Height  = 8;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    IDXGISwapChain*      dummy_sc  = nullptr;
    ID3D11Device*        dummy_dev = nullptr;
    ID3D11DeviceContext* dummy_ctx = nullptr;
    D3D_FEATURE_LEVEL    got{};

    g_dummy_wnd = make_dummy_window();

    // Three routes, tried in order, each logging its own HRESULT so a tester's
    // log says exactly which ones the machine refused. The desktop window stays
    // as route 2 only because it is what shipped through v0.8.3 and worked for
    // most people. WARP is last: it is a software adapter, so it is the wrong
    // GPU for rendering, but the vtable it hands back is still DXGI's own and
    // reading a function pointer out of it is all we do here.
    struct Route {
        const char*     what;
        HWND            wnd;
        D3D_DRIVER_TYPE driver;
    };
    const Route routes[] = {
        { "our own hidden window, hardware device", g_dummy_wnd,        D3D_DRIVER_TYPE_HARDWARE },
        { "the desktop window, hardware device",    GetDesktopWindow(), D3D_DRIVER_TYPE_HARDWARE },
        { "our own hidden window, WARP device",     g_dummy_wnd,        D3D_DRIVER_TYPE_WARP     },
    };

    HRESULT hr = E_FAIL;
    for (const Route& route : routes) {
        if (!route.wnd) continue;           // window creation failed, skip its routes
        sd.OutputWindow = route.wnd;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, route.driver, nullptr, 0, want, 2,
            D3D11_SDK_VERSION, &sd, &dummy_sc, &dummy_dev, &got, &dummy_ctx);
        if (SUCCEEDED(hr) && dummy_sc) {
            LOG_INFO("dummy swapchain created on %s (device 0x%p)",
                     route.what, (void*)dummy_dev);
            break;
        }
        LOG_WARN("dummy swapchain on %s failed: 0x%08lX%s", route.what,
                 (unsigned long)hr,
                 hr == E_ACCESSDENIED ? " (E_ACCESSDENIED)" : "");
        if (dummy_ctx) { dummy_ctx->Release(); dummy_ctx = nullptr; }
        if (dummy_dev) { dummy_dev->Release(); dummy_dev = nullptr; }
        dummy_sc = nullptr;                 // never set on failure, but be explicit
    }

    if (!dummy_sc) {
        LOG_ERROR("dummy swapchain creation failed on every route (last hr 0x%08lX)",
                  (unsigned long)hr);
        LOG_ERROR("cannot read the vtable, so no hooks installed. Game unaffected.");
        destroy_dummy_window();
        return false;
    }

    g_vtable = *(void***)dummy_sc;
    LOG_INFO("IDXGISwapChain vtable at 0x%p", (void*)g_vtable);
    LOG_INFO("  slot %d Present       = 0x%p", kPresentSlot, g_vtable[kPresentSlot]);
    LOG_INFO("  slot %d ResizeBuffers = 0x%p", kResizeBuffersSlot, g_vtable[kResizeBuffersSlot]);

    // Pin the module that owns the vtable. We are about to drop every reference
    // we hold, and on a tester's rig ours was the FIRST code to load DXGI at all
    // (their log says "dxgi_real.dll present on disk, not yet loaded" where the
    // author's rig says "forwarding to"). Dropping the last reference could then
    // unmap the very memory we are about to patch. PIN is permanent for the
    // process, which is exactly the lifetime a patched vtable needs.
    HMODULE vtable_module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)g_vtable[kPresentSlot], &vtable_module)) {
        wchar_t mod_path[MAX_PATH] = {};
        GetModuleFileNameW(vtable_module, mod_path, MAX_PATH);
        LOG_INFO("  vtable module pinned: %ls", mod_path);
    } else {
        LOG_WARN("  could not pin the vtable's module (err %lu). Continuing.",
                 GetLastError());
    }

    // Drop the dummy objects NOW, BEFORE the Present slot is armed. Order is the
    // whole point of this block, so do not move it below patch_slot.
    //
    // Through v0.8.3 these three Release() calls ran at the BOTTOM of this
    // function, after the hook was live. On the author's rig they cost 127-140 ms
    // and nobody noticed. On a tester's rig (2026-08-11) the log stopped dead
    // between "ResizeBuffers not hooked by this module" and the next line, with
    // no crash record and no DLL_PROCESS_DETACH, and the tester reported a frozen
    // black screen he had to force-kill (which is why there is no detach line:
    // TerminateProcess does not run DllMain).
    //
    // That is the game's render thread stuck inside our freshly-armed
    // hooked_present, which calls GetDevice/GetImmediateContext/GetDesc (COM into
    // DXGI, capture_quiet above), while THIS thread sat inside DXGI destroying a
    // device. One process-wide DXGI lock, two threads, no timeout.
    //
    // Releasing first removes the race instead of narrowing it: after this point
    // there is no dummy object left for anything to serialise against, and the
    // only thing we still touch is the pinned vtable memory.
    LOG_INFO("releasing the dummy objects BEFORE arming the hook (v0.8.4 order)");
    dummy_sc->Release();
    dummy_sc = nullptr;
    if (dummy_ctx) { dummy_ctx->Release(); dummy_ctx = nullptr; }
    if (dummy_dev) { dummy_dev->Release(); dummy_dev = nullptr; }
    destroy_dummy_window();
    LOG_INFO("dummy objects released. Arming the Present hook.");

    // Only Present is hooked here. FactoryHook owns the ResizeBuffers hook (15b).
    if (!patch_slot(g_vtable, kPresentSlot, (void*)&hooked_present, (void**)&g_orig_present)) {
        LOG_ERROR("failed to patch Present slot");
        goto cleanup;
    }

    // Sanity check the captured original. If it somehow points back at our own
    // hook we would recurse forever, which is precisely the failure that took
    // the game down at 16:21. Refuse to arm the hook in that case.
    if ((void*)g_orig_present == (void*)&hooked_present) {
        LOG_ERROR("captured original Present == our hook. Refusing to arm (would recurse).");
        patch_slot(g_vtable, kPresentSlot, (void*)g_orig_present, nullptr);
        goto cleanup;
    }

    LOG_INFO("hook installed. original Present = 0x%p", (void*)g_orig_present);
    LOG_INFO("ResizeBuffers not hooked by this module (FactoryHook hooks it guarded, build 15b)");
    g_installed.store(true);

cleanup:
    // Normally a no-op: the dummy objects are already gone by the time the hook
    // is armed (see the release block above). This only fires on the two failure
    // paths below the vtable read, and it is written defensively so that a future
    // early return cannot leak. Swapchain before window, so DXGI never tears down
    // against a dead HWND.
    if (dummy_sc)  dummy_sc->Release();
    if (dummy_ctx) dummy_ctx->Release();
    if (dummy_dev) dummy_dev->Release();
    destroy_dummy_window();
    return g_installed.load();
}

void remove() {
    if (!g_installed.load() || !g_vtable) return;
    if (g_orig_present) patch_slot(g_vtable, kPresentSlot, (void*)g_orig_present, nullptr);
    g_installed.store(false);
    LOG_INFO("hook removed");
}

// Called by the init thread once Present has captured the device, so that the
// heavy logging happens off the render thread (project rule 8).
void drain_capture_log() {
    if (!g_capture_pending.exchange(false, std::memory_order_acq_rel)) return;

    LOG_INFO("=== D3D11 CAPTURED FROM LIVE PRESENT ===");
    LOG_INFO("  device       : 0x%p", (void*)g_state.device);
    LOG_INFO("  context      : 0x%p", (void*)g_state.context);
    LOG_INFO("  swapchain    : 0x%p", (void*)g_state.swapchain);
    LOG_INFO("  backbuffer   : %u x %u", g_state.width, g_state.height);
    LOG_INFO("  format       : %d", (int)g_state.format);
    LOG_INFO("  buffer count : %u", g_desc.BufferCount);
    LOG_INFO("  windowed     : %s", g_desc.Windowed ? "yes" : "NO (exclusive fullscreen)");
    LOG_INFO("  refresh      : %u/%u",
             g_desc.BufferDesc.RefreshRate.Numerator, g_desc.BufferDesc.RefreshRate.Denominator);
    LOG_INFO("  sample count : %u (quality %u)", g_desc.SampleDesc.Count, g_desc.SampleDesc.Quality);
    LOG_INFO("  hwnd         : 0x%p", (void*)g_state.hwnd);
    if (g_state.device) {
        LOG_INFO("  feature level: 0x%04X", (unsigned)g_state.device->GetFeatureLevel());
    }
    if (!g_desc.Windowed) {
        LOG_WARN("  exclusive fullscreen is active. VR needs WINDOWED. Set WindowMode in GRW.ini.");
    }
    LOG_INFO("=== end capture ===");
}

unsigned long long frame_count() { return g_state.frames; }

void request_max_frame_latency(int frames) {
    if (frames < 0)  frames = 0;
    if (frames > 16) frames = 16;   // DXGI's own ceiling
    g_flat_request.store(frames, std::memory_order_relaxed);
}

void poll_max_frame_latency() {
    const int want = g_flat_request.load(std::memory_order_relaxed);
    if (want == g_flat_applied) return;              // nothing changed
    if (!g_state.ready || !g_state.device) return;   // no device yet, retry next tick

    IDXGIDevice1* dev1 = nullptr;
    if (FAILED(g_state.device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dev1)) || !dev1) {
        LOG_WARN("frame latency: the game device has no IDXGIDevice1, max_frame_latency ignored");
        g_flat_applied = want;                       // do not retry every second
        return;
    }

    if (!g_flat_have_original) {
        UINT orig = 0;
        if (SUCCEEDED(dev1->GetMaximumFrameLatency(&orig))) {
            g_flat_original     = orig;
            g_flat_have_original = true;
        }
    }

    if (want == 0) {
        if (g_flat_applied < 0) {
            // Startup with the key absent or 0. Report the engine's own value,
            // which is free diagnostic information, and touch nothing.
            LOG_INFO("frame latency: engine value %u, max_frame_latency=0 so it is left alone",
                     g_flat_original);
        } else if (g_flat_have_original) {
            const HRESULT hr = dev1->SetMaximumFrameLatency(g_flat_original);
            LOG_INFO("frame latency: restored to the engine's %u (hr 0x%08lX)",
                     g_flat_original, (unsigned long)hr);
        }
    } else {
        const HRESULT hr = dev1->SetMaximumFrameLatency((UINT)want);
        UINT now = 0;
        dev1->GetMaximumFrameLatency(&now);
        LOG_INFO("frame latency: engine %u -> requested %d, device reads back %u (hr 0x%08lX)",
                 g_flat_original, want, now, (unsigned long)hr);
    }

    dev1->Release();
    g_flat_applied = want;
}

}  // namespace d3d11
}  // namespace grwxr
