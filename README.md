# Ghost Recon Wildlands VR (GRW-XR)

> [!WARNING]
> **THIS IS NOT A COMPLETE VR EXPERIENCE.** This mod is in the EARLY STAGES of
> development and testing. Stereo depth, a fullscreen 4K view, motion-controller
> aiming and an anchored first-person camera all work, but first person does not yet
> follow the head bone, your character's head is not hidden, there are no IK arms or
> hands, and comfort issues remain. It has been tested on a single hardware
> configuration. Try it as an experiment and a preview, not as a finished way to play
> the game. Development is active and every release changes things.

A native OpenXR VR mod for Tom Clancy's Ghost Recon Wildlands (AnvilNext 2.0, DirectX 11).
Head-tracked stereoscopic 3D rendered by the game's own engine, injected through a
`dxgi.dll` proxy. No game files are modified, ever.

**Status: experimental alpha, in ongoing development.** Stereo fusion with real depth
was achieved on 2026-07-29, the fullscreen view on 2026-07-30, and 4K internal
rendering, motion-controller aiming and anchored first person on 2026-08-01/02. This is
a development snapshot, not a finished mod. Expect rough edges. Performance numbers here come from one
test system; different hardware, headsets, and settings may perform noticeably worse.
The mod is being actively optimized and improved, so expect frequent changes.

See [CHANGELOG.md](CHANGELOG.md) for what changed between versions.

## What works

- Native OpenXR session on the game's own D3D11 device, paced by the headset (72 Hz)
- Full head tracking driving the actual game camera, with the render-time pose submitted
  to the compositor so rotation stays smooth under alternate-eye rendering
- Stereoscopic depth via alternate-eye rendering: the engine renders one eye per frame,
  alternating, with per-eye swapchains and correct per-eye frustum placement
- **Fullscreen view**: the mod overrides the game's rendered field of view (default
  1.92 rad, about 110 degrees) so the image fills the headset instead of appearing as
  a window. On by default, toggleable and tunable live.
- **4K internal rendering, no desktop changes required.** The mod sizes the game's
  swapchain and reports a 4K client area to the engine, so the whole pipeline renders
  at 3840x2160 and the capture is sharp from an ordinary 1080p desktop. The render size
  is a config key, so it doubles as the quality-versus-frame-rate knob.
- **Motion-controller aiming and firing.** A partial right-trigger squeeze aims down
  sights, a full squeeze fires (hold for automatic), and pointing the controller away
  from head center turns the game's own aim, so ballistics, HUD and crosshair stay true.
- **Anchored first person.** A toggle moves the viewpoint onto the player character
  itself. The player is identified through the engine's own player component, so the
  camera attaches to your body rather than to a nearby NPC.
- **Scoped aiming**: while scoped, the mod steps aside so the scope renders exactly as
  the flat game and bullets land on the crosshair. Magnified optics are displayed
  across a comfortable window so they actually magnify instead of shrinking to their
  true angular size.
- Recenter on the Home key, live-adjustable eye separation, field of view and
  first-person placement, config file persistence
- A cropped, non-alternating desktop mirror suitable for recording
- Stable at 72 fps on the test system through extended open-world play

## Known limitations (honest list)

- **First person does not track the head bone yet.** The viewpoint is anchored to the
  character's origin plus a configurable eye height, so it can sit slightly behind or
  above the real head position, it does not follow idle animations, and it does not
  compensate for crouch or prone. Head-bone tracking is the current development focus.
- **Your character's head is not hidden** in first person. You are inside the model and
  rely on backface culling; expect to see hair or helmet geometry at some angles.
- **No IK arms or hands.** Aiming is steered through the game's own aim path; the weapon
  is not held by your controllers.
- The camera can occasionally attach to the wrong body after a respawn or fast travel.
  Toggling first person off and on while facing your character re-acquires it.
- Vehicles in first person are unfinished.
- Wide-angle rendering can look warped or "off" toward the edges; the projection
  geometry is under active tuning.
- Sky and cloud registration at wide field of view is still imperfect.
- Frame rate dips below 72 in dense towns on the test system.
- The desktop mirror shows a cropped single eye; judge the image only in the headset.
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
The sections below are for building from source.

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

All hotkeys are on the numeric keypad and are read live while the game has focus.

| Key | Action |
|---|---|
| Home | Recenter (look where you want forward to be, then press) |
| Numpad 9 / Numpad - | Eye separation scale + / - (0.05 steps) |
| Numpad * | Reset eye separation scale to its startup value |
| Numpad 1 | Fullscreen field-of-view override on / off |
| Numpad + / Numpad 2 | Fullscreen field of view wider / narrower (0.10 rad steps) |
| Numpad 8 | First person on / off |
| Numpad 7 / Numpad 4 | First person: eye height up / down (0.05 m) while anchored to the character, otherwise camera forward / back (0.10 m) |
| Numpad 6 / Numpad 5 | First person: viewpoint right / left |
| Numpad 3 / Numpad 0 | First person: viewpoint up / down (unanchored fallback only) |
| Numpad / | Desktop recording view on / off |

Every change is logged with the exact `grwxr.cfg` line needed to persist it. Hotkey
changes last only for the session; the config file is the permanent home.

### Right Touch controller

| Input | Action |
|---|---|
| Trigger, partial squeeze | Aim down sights |
| Trigger, full squeeze | Fire (hold for automatic fire) |
| Point away from head center | Turn the game's aim |

Keep your thumb off the gamepad's right stick while steering with the controller.

## Configuration

`GRWVR\grwxr.cfg` in the game folder holds the persistent settings and documents every
key in comments. The ones most worth knowing:

| Key | Meaning |
|---|---|
| `ipd_scale` | Eye separation multiplier (default 0.50) |
| `fullscreen_fov` | Rendered field of view in radians (default 1.92) |
| `upsize_width` / `upsize_height` | Internal render size (default 3840x2160). Lower it, for example 3200x1800, to trade sharpness for frame rate |
| `fp_eye` | First-person eye height above the character origin, meters |
| `fp_anchor_side` | First-person lateral centering, meters |
| `aim_steer`, `aim_ads`, `aim_fire` | Motion-control features, set any to `0` to disable |
| `desktop_fov` | Crop of the desktop recording view, `0` disables |

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
