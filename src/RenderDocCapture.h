// RenderDocCapture.h - load RenderDoc into the game from inside the game.
//
// WHY THIS EXISTS (session 22, 2026-08-03)
// ----------------------------------------
// RenderDoc cannot inject into Wildlands on this machine. Both of its routes
// were tried and both fail for reasons that are not our fault and not fixable:
//
//   * Launching GRW.exe from RenderDoc reaches a process that then hands off
//     to a Uplay broker. The game that actually runs is NOT a descendant of
//     anything RenderDoc hooked, so "Capture Child Processes" cannot reach it
//     at any depth.
//   * The global hook uses AppInit_DLLs, and Windows disables AppInit_DLLs
//     outright when Secure Boot is on. It is on. The registry state is
//     correct and Windows ignores it.
//
// Full evidence, with process IDs, in docs/RENDERDOC-GUIDE.md section 3b.
//
// But WE are already inside. The proxy loads into the real game process every
// time by DLL search-order hijack, and it loads early, before the D3D11 device
// exists. That is exactly the timing RenderDoc needs, so the mod can simply
// load renderdoc.dll itself and let it hook normally. No injection, no
// launcher problem, no admin, no Secure Boot involvement.
//
// Loading renderdoc.dll arms RenderDoc fully: it installs its own D3D11 and
// DXGI hooks, draws its overlay, and answers its own capture key. The
// In-Application API is used on top of that only to point captures somewhere
// findable and to report what was taken into our log, so a session can be
// verified from the log alone rather than from the user's memory of an overlay.
//
// Off by default. `renderdoc_capture=1` in grwxr.cfg turns it on, and when it
// is on the VR path stands down entirely (see dllmain.cpp): RenderDoc and our
// Present hook must not fight over the same swapchain.

#pragma once

namespace grwxr {
namespace rdoc {

// True if grwxr.cfg has renderdoc_capture=1. Cached after the first call.
// Safe to call before anything else is initialised.
bool enabled();

// Load renderdoc.dll and bind the In-Application API.
//
// MUST be called before the game creates its D3D11 device, which means early
// on the init thread. Returns false and logs loudly on any failure; a failure
// here leaves the game completely unmodified (project rule 7).
bool install();

// 1 Hz poll from the init thread's drain loop. Watches the capture hotkey and
// reports any new captures. Never called from Present (project rule 8).
void poll();

// Emit anything poll() deferred. Init thread only.
void drain();

}  // namespace rdoc
}  // namespace grwxr
