# Ghost Recon Wildlands VR (GRW-XR)

> [!WARNING]
> **THIS IS NOT A COMPLETE VR EXPERIENCE.** This mod is in the EARLY STAGES of
> development and testing. Stereo depth and a fullscreen view work, but there are
> no motion controls, aiming still follows the flat game's own aim, and visual and
> comfort issues remain. It has been tested on a single hardware configuration.
> Try it as an experiment and a preview, not as a finished way to play the game.
> Development is active and every release changes things.

A native OpenXR VR mod for Tom Clancy's Ghost Recon Wildlands (AnvilNext 2.0, DirectX 11).
Head-tracked stereoscopic 3D rendered by the game's own engine, injected through a
`dxgi.dll` proxy. No game files are modified, ever.

**Status: experimental alpha, in ongoing development.** Stereo fusion with real depth
was achieved on 2026-07-29, and the fullscreen view (the engine rendering the headset's
field of view instead of a flat-game window) landed on 2026-07-30. This is a development
snapshot, not a finished mod. Expect rough edges. Performance numbers here come from one
test system; different hardware, headsets, and settings may perform noticeably worse.
The mod is being actively optimized and improved, so expect frequent changes.

Note: this README describes the current source. The packaged release on the Releases
page may lag behind it; build from source to get everything described here. See
[CHANGELOG.md](CHANGELOG.md) for what changed between versions.

## What works

- Native OpenXR session on the game's own D3D11 device, paced by the headset (72 Hz)
- Full head tracking (rotation) driving the actual game camera
- Stereoscopic depth via alternate-eye rendering: the engine renders one eye per frame,
  alternating, with per-eye swapchains and correct per-eye frustum placement
- **Fullscreen view**: the mod overrides the game's rendered field of view (default
  1.92 rad, about 110 degrees) so the image fills the headset instead of appearing as
  a window. On by default, toggleable and tunable live.
- **Scoped aiming**: while scoped, the mod steps aside so the scope renders exactly as
  the flat game and bullets land on the crosshair. Magnified optics are displayed
  across a comfortable window so they actually magnify instead of shrinking to their
  true angular size.
- **First-person demo mode**: a toggleable camera push that places the view at the
  character instead of behind them. Demo quality (see limitations), but playable.
- Recenter on the Home key, live-adjustable eye separation, field of view, and
  first-person placement, config file persistence
- Stable at 72 fps on the test system through extended open-world play

## Known limitations (honest list)

- **The fullscreen image is soft.** The mod captures the game's backbuffer, which is
  1080p stretched across a wide field of view. Raising the capture resolution is the
  current development focus.
- Wide-angle rendering can look warped or "off" toward the edges; the projection
  geometry is under active tuning.
- No motion controls. Aiming from the hip and in ADS follows the game's own aim, not
  your view; you will see the true ballistic aim point drift from your crosshair
  until the game's aim catches up. Proper aim integration is planned for the IK
  phase. While scoped, ballistics are exact (the mod disengages).
- First person is a demo: expect culling pop at the screen edges, visible hair and
  eyelashes, and vehicle cabins that the camera cannot reach yet.
- Third-person and first-person camera only, no first-person body rig yet.
- The new desktop recording view (Numpad /) is freshly built and not yet verified in
  a full play session.
- Sky and cloud registration at wide field of view has a deployed fix pending final
  verification.
- Tested on exactly one configuration (below). Other headsets and runtimes are untested.

## Requirements

- Ghost Recon Wildlands, Steam edition (tested against the 2023-09-14 Steam build)
- A PC VR headset with an OpenXR runtime. Tested only on Meta Quest 3 over Link cable
  with the Meta Quest Link runtime.
- **Asynchronous Spacewarp must be disabled** (Oculus Debug Tool, set ASW to Disabled).
  The mod manages the stale eye itself; ASW compounds artifacts on top of it.
- A GPU with headroom: the test system is an RTX 5060 Ti 16 GB with a Ryzen 7 9700X.
- To build: Visual Studio 2022 or newer with the C++ workload (MSVC x64, `ml64`).

## Quick install (no build needed)

Download the latest release zip from the
[Releases page](https://github.com/Firejumper93/GhostReconWildlandsVR/releases),
unzip it anywhere, run `install.bat`, and read the included `INSTALL.txt`.
The sections below are for building from source, which is currently ahead of the
packaged release.

## Building

1. Download the OpenXR SDK loader release from
   https://github.com/KhronosGroup/OpenXR-SDK/releases (tested with 1.1.61).
   Place it so these paths exist:
   - `tools/xr_probe/extern/include/openxr/openxr.h`
   - `tools/xr_probe/extern/lib/openxr_loader.lib`
   Keep the loader's `openxr_loader.dll` from the same release; you will copy it to
   the game folder in the install step.
2. If your Visual Studio is not at the default path, edit `VCVARS` at the top of
   `build.bat`.
3. Run `build.bat`. Output: `build\dxgi.dll` (the script prints its SHA256).

## Installing

1. Close the game.
2. If your game is not at `C:\Steam\steamapps\common\Wildlands`, edit `GAME` at the
   top of `deploy.bat`.
3. Run `deploy.bat auto`. It verifies the game is closed, builds, copies `dxgi.dll`
   into the game folder, creates `dxgi_real.dll` there (a local copy of your own
   `C:\Windows\System32\dxgi.dll`, used for export forwarding), and byte-compares
   the result.
4. Copy `openxr_loader.dll` (from the OpenXR SDK release in the build step) into the
   game folder next to `GRW.exe`.
5. Recommended in-game settings: motion blur Off, window mode fullscreen or borderless
   fullscreen (a bordered window locks the game to your monitor's refresh rate),
   resolution scaling to taste. Anti-aliasing is your preference; SMAA and TAA both
   work under the stereo setup.
6. Put the headset on so it is awake and tracking BEFORE launching the game
   (the VR session initializes once at startup), then launch through Steam.

The mod writes its runtime files to `GRWVR\` inside the game folder: a per-process
log (`grwxr-<pid>.log`) and an optional `grwxr.cfg`.

## In the headset

| Key | Action |
|---|---|
| Home | Recenter (look where you want forward to be, then press) |
| Numpad 9 / Numpad - | Eye separation scale + / - (0.05 steps) |
| Numpad * | Reset eye separation scale to its startup value |
| Numpad 1 | Fullscreen field-of-view override on / off |
| Numpad + / Numpad 2 | Fullscreen field of view wider / narrower (0.10 rad steps) |
| Numpad 8 | First-person demo mode on / off |
| Numpad 7 / Numpad 4 | First-person camera forward / back (0.10 m steps) |
| Numpad 6 / Numpad 5 | First-person camera right / left (0.10 m steps) |
| Numpad 3 / Numpad 0 | First-person camera up / down (0.10 m steps) |
| Numpad / | Desktop recording view on / off (experimental) |

Every tuning key prints the exact `grwxr.cfg` line to persist its current value in
the log. Settings the config file understands (all optional, defaults in parentheses):

| Key | Meaning |
|---|---|
| `ipd_scale` (1.0) | Eye separation multiplier. 0.50 is the tuned value on the test system. |
| `fullscreen_fov` (1.92) | The overridden vertical field of view in radians. |
| `mono_scope_fov` (0.30) | Below this rendered fov the mod steps aside (flat scope). 0 disables. |
| `scope_display_fov` (0.5236) | Display size of magnified scope content. 0 = angle-correct. |
| `fp_forward` (2.20) | First-person mode forward camera push in meters. |
| `fp_side` (-0.40) | First-person sideways offset in meters (cancels the over-shoulder camera). |
| `fp_up` (0) | First-person vertical offset in meters. |
| `desktop_fov` (0.90) | Field of view of the desktop recording view in radians. |

## Disabling and uninstalling

- Disable temporarily: rename `dxgi.dll` in the game folder (to `dxgi.dll.off`, for
  example). The game then runs completely unmodified.
- Uninstall: run `deploy.bat remove`, or delete `dxgi.dll`, `dxgi_real.dll`,
  `openxr_loader.dll`, and the `GRWVR` folder from the game directory.

## Rules of use

- **Solo campaign only. Never use this in co-op, PvP, or any matchmaking.** The game
  ships Easy Anti-Cheat for multiplayer; this mod must never run in that context.
- For now, playing in offline mode is recommended (Steam offline mode, or Ubisoft
  Connect set to offline). It keeps the session unambiguously single-player while
  the mod is under development.
- This repository ships source code only. It contains no game files, no Ubisoft
  binaries, and no anti-cheat components, and it never patches any file of your
  install; the proxy DLL sits beside the game and is loaded by normal Windows DLL
  search order.
- Use at your own risk. The game's anti-tamper occasionally reacts to hooks in
  unpredictable ways. If something looks broken, rename `dxgi.dll` and re-check
  before blaming the game or the mod.

## How it works, briefly

The mod is a `dxgi.dll` search-order proxy. It hooks `IDXGISwapChain::Present`,
creates an OpenXR session on the game's own D3D11 device, and locates the engine's
camera and projection functions through byte-signature scanning (never hardcoded
addresses; a failed scan logs loudly and leaves the game untouched). Each frame it
composes the live headset rotation onto the game camera's root transform and offsets
the camera position left or right of center, alternating each frame. The projection
hook also overrides the game's field-of-view argument (within a band that excludes
scopes, menus, and the engine's sky and reflection captures) so the engine renders
the headset's coverage. Each rendered frame is copied into that eye's swapchain,
drawn at its exact angular size inside a canvas shaped like the eye's real display
frustum, and submitted with its stored pose; the compositor reprojects both eyes to
display time. The engine therefore runs at 1x headset rate while both eyes stay
continuously fed.

## Credits

- **[mutars/anvilengine2vr](https://github.com/mutars/anvilengine2vr)** (MIT): the
  reference implementation this project is a port of. Its per-game adapter
  architecture, hook targets, and porting guides are the foundation of this work.
- **[elliotttate/vrframework](https://github.com/elliotttate/vrframework)**: the
  field guides that document the technique family for AnvilNext titles.
- **[dariulone/cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port)**
  (MIT): a sibling architecture for REDengine whose documented lessons on eye
  tagging, pose pairing, and runtime frustum correction directly informed the
  diagnosis of this mod's hardest bug.
- **[pancreations/Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR)** (MIT):
  whose notes on Quest runtime double-vision behavior corroborated that diagnosis.
- **[Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK)** (Apache 2.0):
  the OpenXR loader and headers.
- **Claude Code (Anthropic)**: development contributor; the reverse engineering,
  code, and diagnostics in this repository were built in collaboration with it.
- Tom Clancy's Ghost Recon Wildlands is the property of Ubisoft. This project is not
  affiliated with, endorsed by, or supported by Ubisoft, and distributes none of
  their work.

## License

MIT, see [LICENSE](LICENSE). Portions derived from anvilengine2vr, Copyright (c)
2024 mutars, MIT.
