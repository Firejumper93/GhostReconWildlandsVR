// Foveate.h - BUILD 216/217: fixed foveated rendering by Variable Rate Shading.
//
// WHY THIS EXISTS. The game renders one 3840x2160 image per eye at the wide
// field the clouds fix set (110 degrees vertical), and the headset shows about
// 45 per cent of it: each eye's frustum covers roughly the middle half of the
// width and the lower 84 per cent of the height, and the rest is copied into
// a canvas the lenses never reach. Every frame shades all of it at full rate.
// Build 215 showed the mod's own CPU is about 0.11 ms per present and flat,
// so the headroom is the GPU's, and the owner's route after the instrument
// was foveated rendering (2026-09-07 02:58).
//
// WHAT IT DOES. NVIDIA Variable Rate Shading through NVAPI, the same
// mechanism as fholger's vrperfkit (MIT): a small R8_UINT texture with one
// texel per 16x16 tile of the game's render target names a shading rate per
// tile; it is bound whenever the game binds a render target of the content's
// size, and cleared on every other bind. The pattern: full rate inside
// ffr_inner, half rate to ffr_mid, quarter to ffr_outer, one sixteenth beyond
// that and everywhere outside the union of the two eyes' visible rectangles.
// Radii are fractions of the visible half-extent from the image centre, so
// vrperfkit's numbers mean the same thing here. The pattern is the SAME for
// both eyes: the game's image is centred on the view axis for either eye and
// only its placement in the canvas differs, so no eye identity is needed.
//
// WHERE IT RUNS. Init thread: NVAPI init, the capability check, the two
// render-target detours (DrawHook::install_rt), the mask build, the 1 Hz line
// and the GPU utilization read for the perf line. Present thread: publishes
// the geometry the mask depends on and clears the rate on the immediate
// context before anything of ours draws. Render threads (the game's, through
// the detours): the bind hook, which is counters and driver calls only
// (rule 8), plus the one documented exception of build 218: the driver's
// rate calls are made under a process-wide spinlock because they are not
// thread-safe across contexts. Nothing logs outside the init thread.
//
// BUILD 217. Build 216 matched the content's exact size and reached two binds
// a frame, the final passes into the 3840x2160 backbuffer, never the scene:
// the scene renders at the game's own Supersampling (0.9 in the owner's
// GRW.ini, about 3456x1944). Now every render-target size the game binds is
// counted each second (the rtcensus: line), a mask is built for every size
// that looks like the scene (the content's aspect within one per cent, at
// least half its width, at most twice it), and the hook matches a bind by its
// exact size against the published table of masks. Same rings, same keys.
//
// BUILD 218. Two facts measured offline on 2026-09-07 (nvdeferred.cpp in the
// scratch folder) after the 217 run died on NVAPI status -14 within seconds:
// the driver's rate calls are not thread-safe across contexts (eight threads
// crashed it; a spinlock gave 16,000 calls with zero errors), so every NVAPI
// context call here is under one process-wide spinlock; and the shading-rate
// image is ignored until the first Flush after a bind, so the first mask is
// bound and flushed once on the present thread. The ffr: line carries
// `unlocked=` and `contended=`.
//
// WHO IT IS FOR. NVIDIA RTX and GTX 16xx. On AMD, Intel, or without the
// NVIDIA driver NvAPI_Initialize fails, one line says so, nothing is hooked,
// and the game runs exactly as build 215. The library is a static stub that
// loads nvapi64.dll at run time, so the proxy gains no import (see
// src\nvapi\README-VENDORED.md).

#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace grwxr {
namespace ffr {

struct Params {
    int   on         = 0;      // cfg ffr
    float inner      = 0.6f;   // cfg ffr_inner: full rate inside, in visible half-extents
    float mid        = 0.8f;   // cfg ffr_mid: half rate to here
    float outer      = 1.0f;   // cfg ffr_outer: quarter rate to here, one sixteenth beyond
    int   horizontal = 1;      // cfg ffr_horizontal: 1 = the half-rate ring is 2x1, 0 = 1x2
    int   cull       = 0;      // cfg ffr_cull: 1 = tiles beyond the visible edge are culled
    float margin     = 0.05f;  // cfg ffr_margin: tangent units of slack beyond the visible edge
};

// cfg (init thread; hot reload). The first call decides whether the hooks are
// installed at all: ffr = 0 at start leaves d3d11.dll untouched for the run.
void set_params(const Params& p);

// F7: flip the run-time switch for an A/B; the 1 Hz line counts it.
void toggle();

// BUILD 247: true-stereo mode (two half-width cameras on one full-size target). The mask is one pattern
// built for a full-frame optical centre, so on a target holding two eyes side by side it would put the sharp
// zone on the seam. While suspended every bind gets an explicit full rate (the same path as ffr off); the
// cfg and the F7 switch are untouched, and clearing the flag brings the mask back. Any thread.
void set_suspended(bool on);

// Present thread, every frame, before anything of ours draws: publish the
// geometry the mask is built from (the content size, the wide field, the two
// eyes' frustum tangents: L, R, U, D per eye) and clear the rate on the
// immediate context.
void present_tick(ID3D11DeviceContext* ctx, uint32_t content_w, uint32_t content_h,
                  const float eye_tan[2][4], bool eyes_valid, float wide_fov);

// RAII for the present thread: binds made inside our own Present are ours and
// the hook leaves them alone.
struct PresentGuard {
    PresentGuard();
    ~PresentGuard();
    PresentGuard(const PresentGuard&) = delete;
    PresentGuard& operator=(const PresentGuard&) = delete;
};

// The bind hook DrawHook::install_rt calls on every render-target bind, on the
// calling thread, after the real bind. Rule 8: counters and driver calls only.
void on_rt_bind(ID3D11DeviceContext* ctx, uint32_t n, ID3D11RenderTargetView* const* rtvs);

// Init thread, once a second: init on the first tick with a device, keep the
// mask current, print the ffr: line.
void tick_1hz();

// Init thread: the GPU's busy share in the last second, per cent, from NVAPI;
// -1 when unavailable. fb_out gets the memory controller's share the same way.
int gpu_util(int* fb_out);

}  // namespace ffr
}  // namespace grwxr
