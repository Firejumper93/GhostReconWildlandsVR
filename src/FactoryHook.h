// FactoryHook.h - build 15a: swapchain upsize at the DXGI factory seam.
//
// The three CreateDXGIFactory* exports of this proxy are real functions
// (defined in FactoryHook.cpp, exported via proxy_exports.inc aliases) instead
// of linker forwards. They call dxgi_real.dll, then patch the returned
// factory's CreateSwapChain vtable slot so the game's windowed swapchain can
// be created at a larger size than the window.
//
// Everything here runs at game startup, before the init thread may have
// initialized the log, so findings are buffered and drained from the
// heartbeat loop like the other hook modules.

#pragma once

namespace grwxr {
namespace factory {

// Log the cfg state and whether the stubs have fired yet. Called once from the
// init thread after the log banner.
void report_startup();

// Build 15c: patch GetClientRect in GRW.exe's IAT so the engine sizes its
// renderer at the upsize target instead of the real window client size.
// Called once from the init thread, before the game sizes its renderer.
void install_render_size_spoof();

// Flush buffered creation events into the log. Called from the heartbeat loop.
void drain();

}  // namespace factory
}  // namespace grwxr
