# Ghost Recon Wildlands VR (GRW-XR)

> [!WARNING]
> **SINGLEPLAYER ONLY. Solo campaign, never co-op, never PvP, never matchmaking.**
> The game ships Easy Anti-Cheat for multiplayer and this mod must never run in
> that context. Playing offline (Steam offline mode or Ubisoft Connect offline) is
> recommended while testing.
>
> **THIS IS NOT A COMPLETE VR EXPERIENCE.** This mod is in the EARLY STAGES of
> development and testing. The rendering side is genuinely good: stereo depth, a
> fullscreen 4K view, and a real first-person camera anchored to your character's
> head bone (head hidden, close-range body blur removed).
>
> **THERE ARE NO REAL MOTION CONTROLS. Your Touch controllers are read as an
> EMULATED GAMEPAD.** Sticks, buttons, triggers and grips are translated into
> ordinary gamepad input, which is not motion control and does not feel like a
> native VR shooter. The only motion-tracked thing on top of that is aim
> DIRECTION from the right controller's pointing angle, and even that chases
> rather than tracks. There is no gun in your hands, no hands, no gestures, and
> no weapon manipulation. See "Controller support, honestly" below and read it
> before you decide whether this build is worth your time.
>
> Tested on a single hardware configuration. Try it as an experiment and a
> preview, not as a finished way to play the game. Development is active and
> every release changes things.

A native OpenXR VR mod for Tom Clancy's Ghost Recon Wildlands (AnvilNext 2.0, DirectX 11).
Head-tracked stereoscopic 3D rendered by the game's own engine, injected through a
`dxgi.dll` proxy. No game files are modified, ever.

**Status: experimental alpha, in ongoing development.** Stereo fusion with real depth
was achieved on 2026-07-29, the fullscreen view on 2026-07-30, 4K internal rendering,
controller-pointing aim and anchored first person on 2026-08-01/02, head-bone
first person, head hiding, and continuous 1:1 head aim on 2026-08-03, and Touch
controllers as an emulated gamepad, controller-pointing hip-fire aim with a reticle,
and removal of the first-person close-range body blur on 2026-08-03/04. This is a development snapshot,
not a finished mod. Expect rough edges. Performance numbers here come from one test
system; different hardware, headsets, and settings may perform noticeably worse. The
mod is being actively optimized and improved, so expect frequent changes.

**If you tested an earlier release** (v0.1.x through v0.3.x): the "flat screen
floating in space" is long gone. Current builds render a fullscreen, head-tracked,
stereoscopic view with real depth, let you play on the Touch controllers (emulated
as a gamepad, not true motion controls), aim by pointing the right controller, and
support a true first-person mode. If your install still shows a floating window, you are on the old release:
delete the old `dxgi.dll` and install this one.

See [CHANGELOG.md](CHANGELOG.md) for what changed between versions, and the
[Roadmap](#roadmap) below for what is coming and how close it is.

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
- **Touch controllers work as an EMULATED GAMEPAD, so no physical gamepad is
  needed.** Sticks, triggers, grips, A/B/X/Y and menu are translated into ordinary
  gamepad input. This is gamepad emulation, NOT motion control: it is reliable and
  convenient, and it is not what "VR motion controllers" normally means.
- **Aim direction from the right controller's pointing angle**, with a dot reticle
  and real caveats. This is the only motion-tracked input beyond head tracking.
  See "Controller support, honestly".
- **True first person, anchored to your head bone.** A toggle moves the viewpoint onto
  the player character's actual animated head bone: eye height tracks standing, crouch
  and prone automatically, the camera stays glued to the head while moving, and the
  player is identified through the engine's own player component so it attaches to your
  body rather than to a nearby NPC.
- **Your character's head is hidden in first person**, using the engine's own
  head-visibility mechanism, so you no longer see hair or helmet geometry from inside.
  Hiding is instant on the toggle (the first toggle of a session engages after your
  first aim).
- **The first-person close-range body blur is REMOVED.** Your chest, arms and weapon
  no longer smear when the camera sits at the character's head. This was the top
  complaint from earlier builds.
- **Resilient VR startup.** If the headset is asleep when the game launches, the mod
  now waits and arms itself the moment the headset wakes; no relaunch needed.
- **Continuous 1:1 head aim** (optional, Numpad Decimal). Your head's yaw and pitch feed
  the game's own aim path, so the view, reticle and bullets all follow your gaze
  exactly, while the right stick still turns underneath you. Aiming down sights pauses
  it so scopes stay true.
- **Scoped aiming**: while scoped, the mod steps aside so the scope renders exactly as
  the flat game and bullets land on the crosshair. Magnified optics are displayed
  across a comfortable window so they actually magnify instead of shrinking to their
  true angular size.
- **Config GUI and hot reload.** All tuning lives in `GRWVR\grwxr.cfg`, re-read about
  one second after any save, and `tools\cfg_gui\cfg_gui.exe` is a standalone slider
  editor. Only three hotkeys remain in play (recenter, first person, head aim).
- A cropped, non-alternating desktop mirror suitable for recording
- Stable at 72 fps on the test system through extended open-world play

## Controller support, honestly

**Start here: there are no real motion controls in this mod.** Your Touch
controllers are read as an *emulated gamepad*: the mod translates sticks, buttons,
triggers and grips into the ordinary gamepad input the game already understands.
That is convenient and it works reliably, but it is not motion control, and it
should not be described as such.

The only motion-tracked input layered on top of that is **aim direction** taken
from where the right controller points. That part is the weakest thing in the mod
and the most likely to disappoint you. It is being worked on and it will change.
What follows is what it actually is today, not what it is aiming to become.

**There is no gun in your hands.** Your bullets follow your controller, but the
weapon model does not move with it: the character still holds the rifle wherever
the third-person animation puts it. You are pointing an invisible line at things.
This is the single biggest gap and it is the current development focus.

**Aim chases your controller, it does not track it.** The mod cannot set the game's
aim directly, so it feeds turn input into the game's own aim system until the aim
catches up with where you are pointing. In practice that means a soft, slightly
laggy, "steering" feel rather than a 1:1 weapon in your hand, and fast flicks
overshoot or lag behind. Smoothing (`aim_ctrl_smooth`) trades one for the other;
neither setting makes it feel native.

**Hip fire is inaccurate, and that part is the real game.** Wildlands applies a wide
hip-fire spread cone. Pointing precisely does not help, because the game rolls the
shot inside that cone. It reads as "the mod is broken" and it is not: aiming down
sights is exact, which is how we know spread is vanilla behavior rather than
something the mod causes. Defeating hip-fire spread under VR aim is designed and
queued, but it is NOT in this release.

**Aiming down sights abandons controller aim.** Holding the left trigger switches
aim back to your head, because the game draws its sight picture at view center: if
we left aim on the controller, the sight picture and the impacts would disagree.
So you get two different aiming models depending on the trigger, which is
inconsistent to play. Optics are head-anchored, not gun-anchored, for the same
reason.

**There are no hands, no gestures, and no weapon manipulation.** No grabbing, no
gesture reloads, no physical mag changes, no two-handed grip. Reload, swap, vehicle
entry and everything else are ordinary button presses.

**The reticle is a flat dot at infinity.** It shows the ray direction. It does not
sit at target depth, so it will not converge on close targets the way a real red dot
does.

Where this is going: the weapon model riding your controller (so you physically
raise the gun to your eye), and ADS-grade accuracy under VR aim at all times. Both
are designed; neither ships here.

## Known limitations (honest list)

- **No real motion controls.** Touch is emulated as a gamepad; only aim direction is
  motion-tracked, and it is rough: no visible gun in your hands, aim chases rather
  than tracks, hip fire blooms, ADS switches back to head aim. Read "Controller
  support, honestly" above; it is the honest account, not a teaser.
- The camera can occasionally attach to the wrong body after a respawn or fast travel.
  Toggling first person off and on while facing your character re-acquires it.
- Vehicles in first person are unfinished. Ground vehicles are playable and fun in
  practice; aerial vehicle interiors are not yet wired up.
- Wide-angle rendering can look warped or "off" toward the edges; the projection
  geometry is under active tuning.
- Sky and cloud registration at wide field of view is still imperfect.
- Frame rate dips below 72 in dense towns on the test system.
- The desktop mirror shows a cropped single eye; judge the image only in the headset.
- Tested on exactly one configuration (below). Other headsets and runtimes are untested.
- If a game patch ever makes head aim turn the wrong way, flip `aim_yaw_sign` or
  `aim_pitch_sign` in the config (the engine's aim directions are calibrated per game
  build; the signs differ per axis on the current build).

## Roadmap

What is being worked on right now, with an honest estimate of how far along each item
is. Percentages are progress toward shipping, not promises or dates; they move as
evidence comes in.

| Feature | Progress | Where it stands |
|---|---|---|
| The gun visibly rides your controller (physical sighting) | ~40% | Bullets already follow the controller; the engine's object-placement path is fully mapped and a probe locating the weapon's placement handle is in testing |
| Hip-fire accuracy at ADS grade under VR aim | ~70% | The exact engine flag is located and verified unique in this build; one write route decision remains before it ships |
| Performance pass for dense towns | ~30% | The engine's shadow-quality lever is located and writable live; a measurement run will decide what ships |
| Aerial vehicle interior camera | ~20% | Ground-vehicle first person already works in practice; helicopter and plane interiors need their camera behavior characterized first |
| VR arms and hands (IK) | ~15% | The GPU skinning data format was recovered from the game's own shipped shaders; which render path draws the player is the remaining unknown. The engine's own IK system is confirmed present, which is the long-term route |

Shipped since the last release: Touch-as-emulated-gamepad support, controller-pointing
hip-fire aim with a reticle, first-person body blur removal, instant head hide, config GUI
with hot reload, resilient VR startup.

A public **beta** is planned once the top items land.

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
5. Set the baseline graphics settings below.
6. Launch through Steam. The headset no longer needs to be awake before launch: the
   mod waits and arms itself the moment the headset wakes.

## Baseline graphics settings (start here before judging anything)

This is the tested baseline. Test against it before reporting a graphics or
performance problem, because two of these settings (window mode and anti-aliasing)
can silently cost half the frame rate.

In-game video settings (these write to `GRW.ini` in
`Documents\My Games\Ghost Recon Wildlands`):

| Setting | Value | Why |
|---|---|---|
| Resolution / window | 1920x1080, Fullscreen (`WindowMode=1`) | The mod renders internally at 4K regardless of the desktop size. A BORDERED window locks the game to your monitor's refresh rate and caps VR at 60 |
| Frame rate limit | 72 (`FpsLimit=72`) | Matches the Quest 3 refresh the mod paces to |
| Supersampling | 0.90 (`Supersampling=0.90`) | 1.00 exceeds the tested GPU's budget at 4K; 0.90 holds 72 fps |
| Anti-aliasing | **SMAA or Off. NEVER any temporal (TAA) mode** (`AntiAliasingMode=3` is SMAA, `0` is off) | Temporal AA blends the alternating eye viewpoints into ghosting, and it is expensive: a TAA-enabled save measured a sustained drop from 72 to the low 60s |
| Motion blur | Off | Smears under head tracking |

Two traps worth knowing, both observed on the test system:

- **The game rewrites `GRW.ini` when you apply anything in its menus**, and it has
  silently wiped `FpsLimit` doing so. After any in-game menu apply, re-check the ini.
- **Old saves can carry old settings with them.** A save from before this baseline
  loaded with TAA enabled and read as "the mod got laggy". If performance suddenly
  looks wrong, check the anti-aliasing setting first, then the ini.

The mod writes its runtime files to `GRWVR\` inside the game folder: a per-process
log (`grwxr-<pid>.log`) and an optional `grwxr.cfg`.

## In the headset

Only THREE hotkeys remain; every tuning key from older releases was removed. All
tuning lives in `GRWVR\grwxr.cfg`, which hot-reloads about one second after any
save, or use the included slider GUI (`tools\cfg_gui\cfg_gui.exe`).

| Key | Action |
|---|---|
| Home | Recenter (look where you want forward to be, then press) |
| Numpad 8 | First person on / off (head hiding follows it automatically) |
| Numpad . (Decimal) | 1:1 head aim on / off (bullets follow your gaze; default off) |

### Touch controllers (emulated as a gamepad)

The mod merges the Touch controllers into the game as a gamepad, so the full normal
control scheme works and no physical gamepad is needed: sticks move and turn, face
buttons and grips act as their gamepad equivalents. To be clear, that is gamepad
emulation, not motion control. The one motion-tracked addition:

| Input | Action |
|---|---|
| Right controller, point | Hip-fire aim: the dot reticle and bullets follow the controller ray |
| Right trigger, full squeeze | Fire (hold for automatic fire) |
| Left trigger (hold) | Aim down sights: aim follows your head so the sight picture is true; the dot hides |

A physical gamepad still works if you prefer it (the Touch snapshot merges with it).

## Configuration

`GRWVR\grwxr.cfg` in the game folder holds the persistent settings and documents every
key in comments. The ones most worth knowing:

| Key | Meaning |
|---|---|
| `ipd_scale` | Eye separation multiplier (default 0.50) |
| `fullscreen_fov` | Rendered field of view in radians (default 1.92) |
| `upsize_width` / `upsize_height` | Internal render size (default 3840x2160). Lower it, for example 3200x1800, to trade sharpness for frame rate |
| `fp_head_anchor` | `1` (default) anchors first person to the character's head bone; `0` falls back to the origin anchor |
| `fp_head_eye` | Eye offset above the head bone, meters (default 0.10) |
| `fp_eye` | First-person eye height above the character origin, meters (fallback anchor only) |
| `fp_anchor_side` | First-person lateral centering, meters |
| `aim_yaw_sign`, `aim_pitch_sign` | Head-aim direction calibration (defaults -1 and +1 for the current game build; flip one only if head aim turns the wrong way on that axis) |
| `aim_source` | `1` (default) hip-fire aim follows the right controller; `0` follows the head |
| `aim_ctrl_smooth` | Controller-aim smoothing, 0 to 1 (default 0.35) |
| `aim_reticle` | `1` (default) draws the hip-fire dot reticle; `0` hides it |
| `xinput_touch` | `1` (default) merges Touch controllers into the gamepad; `0` passes through untouched |
| `aim_steer`, `aim_ads`, `aim_fire` | Controller-pointing aim, trigger ADS and trigger fire; set any to `0` to disable |
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
