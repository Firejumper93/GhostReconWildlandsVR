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

// Build 38: lets the init thread arm the callback exactly once without
// tracking that state itself.
bool has_present_callback();

// Present captures the device but writes no log lines (rule 8). The init thread
// calls this to emit the capture details off the render thread.
void drain_capture_log();

unsigned long long frame_count();

// FRAMES IN FLIGHT. Prior art: "Making 6DOF Mods 3D, Rev 4" section 15.8, which
// records that Luke Ross's R.E.A.L. caps D3D max frame latency to 1 under
// alternate-frame stereo, for the same reason we might want to. Fewer frames
// queued means the eye a frame was BUILT with reaches Present sooner, so the two
// AER eyes are paired more tightly in time. Our own build-10b logs measured the
// build-to-present depth flapping between 0 and 1, which is exactly the quantity
// this bounds, and that flap was the mechanism behind the session-13 rotation
// stutter.
//
// grwxr.cfg `max_frame_latency`:
//   0     (default) leave the engine's own value completely alone. Nothing is
//         written, so a DLL carrying this code behaves exactly as one without it
//         until the key is set.
//   1..16 request that queue depth.
// Setting the key back to 0 restores the value the device had when we captured
// it, so the experiment is fully reversible live, through the cfg hot reload,
// with no relaunch and no second build.
void request_max_frame_latency(int frames);

// Init thread only. Applies a pending request once the device exists, logs the
// engine's own value, what was asked for and what the device reads back, and
// returns immediately when nothing has changed. Never called from Present:
// this does COM work and logs, both banned on the render thread (rule 8).
void poll_max_frame_latency();

}  // namespace d3d11
}  // namespace grwxr
