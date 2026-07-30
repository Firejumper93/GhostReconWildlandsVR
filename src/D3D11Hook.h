// D3D11Hook.h - capture the game's D3D11 device, context and swapchain.
//
// APPROACH: vtable patching, not inline detours.
//
// The usual technique is an inline detour on IDXGISwapChain::Present, which
// needs an x64 length disassembler and a trampoline. We do not need any of
// that. COM objects of the same class share one vtable, so if we create a
// throwaway swapchain and patch the Present slot in ITS vtable, the game's
// swapchain, which shares that vtable, is hooked too.
//
// Why this is the right call here specifically:
//   - No instruction decoding, so no risk of mis-decoding and corrupting code.
//   - Fully reversible: restore one pointer.
//   - No writes into GRW.exe's own code at all, which matters on a Denuvo
//     target with 28 gameplay-sabotage anti-tamper triggers
//     (docs/TARGET-INVENTORY.md section 6). We only touch a vtable that lives
//     in dxgi.dll, not in the game image.
//   - Cooperates naturally with other overlays: we save whatever pointer was
//     there and call it, so Steam or Discord hooking the same slot still works.
//
// PROCESS SELECTION: GRW.exe spawns three processes on launch and only the last
// one renders (verified 2026-07-28: pids 24044, 8672, 26384). We therefore wait
// for this process to actually own a visible window before touching anything.
// Transient launcher processes never get that far, so they are never hooked.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

namespace grwxr {
namespace d3d11 {

// Install the Present/ResizeBuffers vtable hooks. Safe to call once.
// Returns false and logs loudly on failure; the game keeps running either way
// (project rule 7).
bool install();

// Undo the vtable patches.
void remove();

// Live state, populated on the first real Present. Null until then.
struct State {
    ID3D11Device*        device    = nullptr;
    ID3D11DeviceContext* context   = nullptr;
    IDXGISwapChain*      swapchain = nullptr;
    UINT                 width     = 0;
    UINT                 height    = 0;
    DXGI_FORMAT          format    = DXGI_FORMAT_UNKNOWN;
    HWND                 hwnd      = nullptr;
    unsigned long long   frames    = 0;
    bool                 ready     = false;
};

const State& state();

// Registered callback, invoked from inside Present AFTER the game has drawn but
// BEFORE the frame is presented. This is where phase 3 will copy the backbuffer
// into the OpenXR swapchain.
//
// project rule 8: no logging, file I/O, locks, COM creation or allocation in
// here. It runs on the render thread every single frame.
using PresentCallback = void (*)(const State&);
void set_present_callback(PresentCallback cb);

// Present captures the device but writes no log lines (rule 8). The init thread
// calls this to emit the capture details off the render thread.
void drain_capture_log();

unsigned long long frame_count();

}  // namespace d3d11
}  // namespace grwxr
