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

// ResizeBuffers is deliberately NOT hooked. See docs/RE-notes.md do-not-hook
// list: hooking it by vtable patch caused unbounded recursion and a stack
// overflow before the game reached a single frame. We do not need it. Instead,
// Present re-reads the swapchain description whenever the cached size looks
// stale, which costs one GetDesc on a resize rather than a hook on a hot path.

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

}  // namespace

const State& state() { return g_state; }
void set_present_callback(PresentCallback cb) { g_callback.store(cb, std::memory_order_relaxed); }

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

    // Create a throwaway swapchain purely to read the shared vtable.
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount        = 1;
    sd.BufferDesc.Width   = 8;
    sd.BufferDesc.Height  = 8;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = GetDesktopWindow();
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    IDXGISwapChain*      dummy_sc  = nullptr;
    ID3D11Device*        dummy_dev = nullptr;
    ID3D11DeviceContext* dummy_ctx = nullptr;
    D3D_FEATURE_LEVEL    got{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2,
        D3D11_SDK_VERSION, &sd, &dummy_sc, &dummy_dev, &got, &dummy_ctx);

    if (FAILED(hr) || !dummy_sc) {
        LOG_ERROR("dummy swapchain creation failed: 0x%08lX", (unsigned long)hr);
        LOG_ERROR("cannot read the vtable, so no hooks installed. Game unaffected.");
        return false;
    }
    LOG_INFO("dummy swapchain created (device 0x%p)", (void*)dummy_dev);

    g_vtable = *(void***)dummy_sc;
    LOG_INFO("IDXGISwapChain vtable at 0x%p", (void*)g_vtable);
    LOG_INFO("  slot %d Present       = 0x%p", kPresentSlot, g_vtable[kPresentSlot]);
    LOG_INFO("  slot %d ResizeBuffers = 0x%p", kResizeBuffersSlot, g_vtable[kResizeBuffersSlot]);

    // Only Present is hooked. ResizeBuffers is on the do-not-hook list.
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
    LOG_INFO("ResizeBuffers deliberately NOT hooked (do-not-hook list: caused recursion)");
    g_installed.store(true);

cleanup:
    // The dummy objects have served their purpose. The vtable they pointed at is
    // shared and outlives them.
    if (dummy_sc)  dummy_sc->Release();
    if (dummy_ctx) dummy_ctx->Release();
    if (dummy_dev) dummy_dev->Release();
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

}  // namespace d3d11
}  // namespace grwxr
