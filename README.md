# Ghost Recon Wildlands VR (GRW-XR)

**English** | [Deutsch](README.de.md)

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
> **THE WEAPON FOLLOWS YOUR CONTROLLER, as of v0.8.0.** Position and rotation,
> one to one, confirmed in the headset. It is the game's own weapon, not an
> overlay: point your hand and the gun points there, move your hand and it goes
> with you.
>
> **BULLETS DO NOT FOLLOW THE WEAPON YET.** Rounds still go where you are
> LOOKING, so you can aim the gun at one thing and hit another. That is the
> known state of this release, not a fault on your machine, and it is the next
> thing being worked on. Everything else about the controls is unchanged:
> sticks, buttons, triggers and grips are still read as an ordinary gamepad, so
> no physical controller is needed.
>
> Read "Motion controls: exactly where this is" below before you decide whether
> this release is what you are after. It says plainly what works, what does
> not, and what is left.
>
> **WORKS WITH THE AUGUST 2026 "LAST RITES" TITLE UPDATE, as of v0.7.0.** The update
> replaced the game executable; this release carries a full verified address table
> for it, and full stereo on the updated game is confirmed in the headset. Steam and
> Ubisoft Connect now ship the IDENTICAL executable, so both stores are covered by
> the same table. One known casualty until it is re-derived: **head hiding in first
> person is temporarily NOT working on the updated game** (you will see hair or
> helmet from inside). Details in "The 2026-08 game update" below.

A native OpenXR VR mod for Tom Clancy's Ghost Recon Wildlands (AnvilNext 2.0, DirectX 11).
Head-tracked stereoscopic 3D rendered by the game's own engine, injected through a
`dxgi.dll` proxy. No game files are modified, ever.

**Status: experimental alpha, in ongoing development.** Stereo fusion with real depth
was achieved on 2026-07-29, the fullscreen view on 2026-07-30, 4K internal rendering,
controller-pointing aim and anchored first person on 2026-08-01/02, head-bone
first person, head hiding, and continuous 1:1 head aim on 2026-08-03, and Touch
controllers as an emulated gamepad, controller-pointing hip-fire aim with a reticle,
and removal of the first-person close-range body blur on 2026-08-03/04. The port to
the August 2026 "Last Rites" game update was headset-verified on 2026-08-08, and on
2026-08-10 the weapon itself began tracking the controller in position and rotation.
This is a development snapshot, not a finished mod. Expect rough edges. Performance numbers here come from one test
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

## The 2026-08 game update ("Last Rites"): honest status

Ubisoft shipped a ~31 GB title update in August 2026 that replaced `GRW.exe`. The mod
locates engine code inside that executable, so a new executable means every address
must be re-derived and re-verified. That work is what v0.7.0 is. The current state,
feature by feature:

**Verified working on the updated game (in the headset, 2026-08-08):**

- Build identification: the mod recognizes the new executable and logs it as the
  "2026-08-update binary". Steam and Ubisoft Connect now ship the byte-identical
  executable, so one table covers both stores.
- Full stereoscopic rendering, head tracking, the fullscreen view, and 4K internal
  rendering: all camera and projection hooks re-derived and confirmed live.
- Head aim, the Touch emulated gamepad, and the no-blur patch: re-derived and
  installed (their hooks report success in the log).
- The new executable enables ASLR (randomized load addresses); the mod handles it.

**Known NOT working on the updated game, to be restored in a coming release:**

- **Head hiding in first person.** The engine function that hides the head could not
  be matched in the new executable (it was recompiled, not just moved), and this mod
  never guesses addresses: rather than risk your game, the feature disables itself.
  First person still works; you will see hair or helmet geometry from inside until
  this is re-derived. It is the top restoration priority.

**Not yet re-verified on the updated game (worked before, expected to work, but the
update's gameplay changes touch them and they have not been re-confirmed in the
headset yet):**

- The head-bone first-person anchor (eye height tracking crouch and prone).
- The hand markers and the research instruments around weapon identification. The
  update changed weapon handling (reloads, a two-primary loadout), so the internal
  weapon bookkeeping the research side reads is due a re-check.

If any of these misbehave for you on the new game version, that is why; please
report it with your `GRWVR\grwxr-<pid>.log`. Nothing silently guesses: every feature
that could not be re-verified either disabled itself or is listed here.

**Recommended settings for the new game version:** the update added FSR upscaling;
**keep FSR off** while using the mod (it sits inside the render path the mod
manages, untested and likely to blur the eyes). The update's new native immersion
toggles (reduced highlight glow, throwable sightline preview off, hidden-UI sounds)
work fine and are recommended for VR.

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
- **THE WEAPON FOLLOWS YOUR CONTROLLER**, position and rotation, one to one. The
  gun is the game's own weapon, placed by writing the bone the engine mounts it
  on, at the instant the engine reads that bone. Bullets do not follow it yet.
  See "Motion controls: exactly where this is".
- **Hand markers**: two coloured dots drawn where your controllers actually are,
  with real stereo depth.
- **True first person, anchored to your head bone.** A toggle moves the viewpoint onto
  the player character's actual animated head bone: eye height tracks standing, crouch
  and prone automatically, the camera stays glued to the head while moving, and the
  player is identified through the engine's own player component so it attaches to your
  body rather than to a nearby NPC.
- **Your character's head is hidden in first person**, using the engine's own
  head-visibility mechanism, so you no longer see hair or helmet geometry from inside.
  Hiding is instant on the toggle (the first toggle of a session engages after your
  first aim). **Temporarily NOT working on the 2026-08 game update** (see the update
  section above); it disables itself there rather than guess at a moved address.
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

## Motion controls: exactly where this is

This section is deliberately blunt in both directions, because motion controls are
the thing people care about most and the easiest thing to overstate.

### What now works, confirmed in the headset (2026-08-10)

**The weapon follows your controller, one to one, in position and rotation.** Point
your hand and the gun points there. Move your hand and the gun moves with it. You
can raise it, lower it, swing it across your body. It is not a floating overlay or
a cosmetic model: it is the game's own weapon, placed by the game's own systems,
being told where to go.

Getting there meant finding, and confirming in the headset one at a time: that the
weapon can be moved at all, which of the character's bones actually carries it,
which axis of that bone is the barrel, and then setting that barrel onto the
controller's ray directly rather than nudging it relative to where the game was
already aiming. Two of those four had previously been assumed, and both assumptions
turned out to be wrong, which is most of why this took as long as it did.

### What does not work yet, plainly

**Bullets do not follow the gun.** They still go where you are looking. You can aim
the weapon at one thing and shoot another, which is obviously not the finished
article, and it is the single remaining piece between this and real motion-controlled
shooting.

This is not for want of trying. Three separate mechanisms have been armed, verified
to actually execute, and shown to make no difference to where rounds land. Those are
useful results, not failures: each one removes a possibility with evidence rather
than leaving it suspected. The search is now narrowed to one strong candidate.

**Hip-fire spread is untouched**, so even a correctly pointed barrel scatters.
**Your character's arms do not follow the weapon**, so the gun can look detached
from the body. **There are still no hands, no gestures and no weapon manipulation**:
no grabbing, no gesture reloads, no physical mag changes, no two-handed grip. Reload,
swap and vehicle entry are ordinary button presses.

### Everything else about the controls

Beyond the weapon, your Touch controllers are still read as an **emulated gamepad**:
sticks, triggers, grips and face buttons become ordinary gamepad input, so no
physical controller is needed. Your head aims and looks at 1:1, and two hand-position
markers are drawn where your controllers are, with real stereo depth.

That part is gamepad emulation rather than motion control, and this README will not
call it anything else. Turning the weapon feature off (`wgun = 0` in the config)
gives you exactly the v0.7.0 behaviour back.

**Aiming down sights stays consistent** under head aim, because the game draws its
sight picture at view centre and that is exactly where your bullets go. Optics are
head-anchored rather than gun-anchored. Since bullets follow your gaze, aiming down
sights remains the accurate way to shoot in this release.

### Why this takes the time it takes

Wildlands is a closed 2017 AAA engine with no source, no SDK, no mod API and an
anti-tamper layer, and it is a third-person game with no first-person rig to borrow
from. Nothing about how it places a weapon, aims a shot or poses a skeleton is
documented anywhere. Every address the mod uses was recovered by reading the shipped
executable, and every one is confirmed in a headset before it is trusted, because a
plausible-looking wrong answer costs days to disprove.

That is also why the mod fails safe rather than guessing: if something cannot be
verified on your game version, it disables itself and says so in the log instead of
writing to an address it is not sure about.

Progress therefore arrives in discrete confirmed steps rather than continuously. The
upside is that what is listed as working here is genuinely working, and has been
watched working through a headset rather than inferred from a log.

## Known limitations (honest list)

- **Head hiding is temporarily out on the 2026-08 game update** (the current game
  version). You will see hair or helmet from inside in first person until the
  moved engine function is re-derived. See "The 2026-08 game update" above.
- **Bullets do not follow the weapon.** The gun tracks your controller; the rounds
  still go where you are looking. This is the last major piece and the current focus.
- **The gun may not sit exactly in your fist.** It is placed at the point the engine
  mounts it, which is near the receiver, so it can hang slightly off your hand. A
  grip offset is coming; `wgun_pos_scale` tunes reach in the meantime.
- **No hands, gestures or weapon manipulation.** No grabbing, gesture reloads,
  physical mag changes or two-handed grip.
- **Your character's arms do not follow the weapon**, so the gun can look detached.
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
| The weapon rides your controller | **~90%, shipped in v0.8.0** | **Position and rotation both confirmed in the headset.** The gun is the game's own weapon, moved by writing the bone the engine mounts it on, at the moment the engine reads that bone. Remaining: a grip offset so it sits in your fist rather than beside it, and travel-scale tuning |
| Bullets go where the weapon points | ~40% | The last major piece. Three candidate mechanisms have been armed, verified to execute, and shown not to affect where rounds land, which eliminates them with evidence. One strong candidate remains and is the next thing worked on |
| Hip-fire accuracy at ADS grade under VR aim | ~70% | The exact engine flag is located and verified unique in every supported build; one write-route decision remains before it ships |
| Physical sighting (raise the gun, use the sights, no ADS mode) | ~35% | Follows directly from the two rows above. The gun already points where you point it; sights need the bullets fixed first, then an eye-aligned reticle |
| Performance pass for dense towns | ~30% | The engine's shadow-quality lever is located and writable live; a measurement run will decide what ships |
| Aerial vehicle interior camera | ~20% | Ground-vehicle first person already works in practice; helicopter and plane interiors need their camera behaviour characterised first |
| VR arms and hands (IK) | ~15% | The GPU skinning data format was recovered from the game's own shipped shaders. The engine's own IK system is confirmed present, which is the long-term route. Note the weapon does NOT depend on this: it is moved directly, so arms are a separate, harder problem |

Percentages are progress toward shipping, not promises or dates, and they move as
evidence comes in. A row only goes up when something has been watched working in a
headset.

Shipped in v0.8.0: the controller-tracked weapon, plus a fix for a config value that
could crash the game if it was mistyped.

**This release (v0.8.0)** carries the controller-tracked weapon. A public **beta**
follows once bullets track with it.

## Which version of the game do I need?

**The current, updated game (August 2026 "Last Rites" patch) is verified, on Steam,
in the headset.** Since that update, Steam and Ubisoft Connect ship the byte-identical
executable, so Ubisoft Connect installs are covered by the very same verified
address table (headset confirmation from a Ubisoft Connect user is still welcome).

The mod finds the engine's camera and projection code at specific addresses inside
`GRW.exe`, so every distinct build of that executable needs its own verified address
table. This release carries THREE: the pre-update Steam build (2023-09-14), the
pre-update Epic / Ubisoft Connect store build (2023-09-08, machine-verified offline,
never headset-confirmed by a store user), and the current 2026-08 update build that
both stores now ship (headset-verified). The mod identifies which one it is running
inside from the executable's own headers.

If the mod meets a `GRW.exe` it does not recognize (a future game patch, or a build
we have not analysed), it says so in its log, names the builds it knows, and
**installs nothing**: your game runs completely unmodified. The symptom of that state
is a small flat window in the headset that does not respond to head movement, with
controllers possibly still working. Check `GRWVR\grwxr-<pid>.log` for the
"build pin:" line to confirm.

## Requirements

- Ghost Recon Wildlands, current version (the August 2026 "Last Rites" update),
  Steam or Ubisoft Connect (identical executable since that update; see above).
  The two pre-update builds remain supported by their own address tables.
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
| Numpad 8 | First person on / off (head hiding follows it automatically; also recenters, so a stale reference cannot poison the view) |
| Numpad . (Decimal) | 1:1 head aim on / off (bullets follow your gaze; default off) |

### Touch controllers (emulated as a gamepad)

The mod merges the Touch controllers into the game as a gamepad, so the full normal
control scheme works and no physical gamepad is needed: sticks move and turn, face
buttons and grips act as their gamepad equivalents. To be clear, that is gamepad
emulation, not motion control. The one motion-tracked addition:

| Input | Action |
|---|---|
| Head | Aim: bullets follow your gaze (toggle with Numpad Decimal) |
| Both controllers | Hand markers: blue (left) and orange (right) dots drawn where your hands are |
| Right trigger, full squeeze | Fire (hold for automatic fire) |
| Left trigger (hold) | Aim down sights (the sight picture is at view center, which is where head aim points, so it stays true) |

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
| `aim_source` | `0` (default since v0.6.0) aim follows the head; `1` is the retired controller-aim experiment, which fights head-look |
| `aim_ctrl_smooth` | Controller-aim smoothing, 0 to 1 (default 0.35) |
| `aim_reticle` | `1` (default) draws the hip-fire dot reticle; `0` hides it |
| `xinput_touch` | `1` (default) merges Touch controllers into the gamepad; `0` passes through untouched |
| `hand_markers` | `1` (default) draws the two hand-position dots; `0` hides them |
| `wp_markers` | `0` (default in the shipped config) research aid: colored dots on the engine objects nearest the camera, used to identify the weapon's placement handle; turn on only if you are helping with that hunt |
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
- **SolKutTeR and [vrforum.de](https://vrforum.de)**, for the
  [thread about this mod](https://vrforum.de/threads/ghost-recon-wildlands-grw-xr.14507/)
  and for bringing German-speaking players to it. That thread is the reason this
  README now exists in German ([README.de.md](README.de.md)).
- Tom Clancy's Ghost Recon Wildlands is the property of Ubisoft. This project is not
  affiliated with, endorsed by, or supported by Ubisoft, and distributes none of
  their work.

## License

MIT, see [LICENSE](LICENSE). Portions derived from anvilengine2vr, Copyright (c)
2024 mutars, MIT.
