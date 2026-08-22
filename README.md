# Ghost Recon Wildlands VR (GRW-XR)

**English** | [Deutsch](README.de.md) | [한국어](README.ko.md)

> [!IMPORTANT]
> **THE CURRENT GAME VERSION IS SUPPORTED, as of v0.9.1-test2.** If you have been
> sitting on "the mod does nothing since the update", this is the release for you.
>
> | Game update | `TimeDateStamp` | First release that supports it |
> |---|---|---|
> | late-July 2026 | `6A692948` | v0.7.0-alpha |
> | 2026-08-13 ("Last Rites") | `6A75F2F4` | v0.9.0-test1 (never announced as such, which is most of the confusion) |
> | **2026-08-19** | `6A7C5143` | **v0.9.1-test2, this one** |
>
> v0.8.5-alpha, which is what this page recommended until now, only ever knew the
> late-July executable. On anything newer it recognises nothing, installs nothing,
> and logs `build pin: UNKNOWN GRW.exe binary` while the game runs flat. That is
> the check working, not a crash.
>
> This is also the first source push in a while: the repository's `main` branch was
> still at v0.8.5-era code, so the newer work was only ever in release zips. It is
> all here now.
>
> **This is a test release, not a polished one.** It carries a lot of new work
> that has been run in a headset in pieces but never as a packaged release, and
> it has one known ugly bug in two-handed weapon handling (see
> [Known limitations](#known-limitations-honest-list)). If you want the calmest
> thing available and you are still on the old game version, v0.8.5-alpha is
> that. On the current game version, this is the only release that does
> anything at all.

> [!WARNING]
> **SINGLEPLAYER ONLY. Solo campaign, never co-op, never PvP, never matchmaking.**
> This is a hard rule of the project and it has not changed.
>
> Note that the 2026-08-13 title update **removed Easy Anti-Cheat** from the game.
> That does not loosen this rule: the mod is built and tested for the solo
> campaign only, and multiplayer remains out of scope. Playing offline (Steam
> offline mode or Ubisoft Connect offline) is still recommended while testing.
>
> **THIS IS NOT A COMPLETE VR EXPERIENCE.** This mod is in the EARLY STAGES of
> development and testing. The rendering side is genuinely good: stereo depth, a
> fullscreen 4K view, and a real first-person camera anchored to your character's
> head bone (head hidden, close-range body blur removed).
>
> **THE WEAPON FOLLOWS YOUR CONTROLLER, as of v0.8.0.** Position and rotation,
> one to one, confirmed in the headset. It is the game's own weapon, not an
> overlay: point your hand and the gun points there, move your hand and it goes
> with you. **Two-handed handling** (rear hand holds, front hand points, wrist
> rolls the gun) arrived in v0.9.0-test1 and is on by default here, with the
> caveat below.
>
> Bullets still follow your gaze in this release, so aiming down sights stays
> the accurate way to shoot. That is the last piece. Everything else is
> unchanged: sticks, buttons, triggers and grips are still read as an ordinary
> gamepad, so no physical controller is needed.
>
> **HEAD HIDING IS BACK on the current game version.** It was disabled from
> v0.7.0 through v0.8.5 because the engine function that hides the head had been
> recompiled and this mod never guesses an address. That address was re-derived
> on 2026-08-15, and the hook arms and reports `hide: armed` on both current
> executables.
>
> **YOU CAN NOW CHANGE SETTINGS WITHOUT TAKING THE HEADSET OFF.** `F1` opens a
> settings panel you drive with the controller, and the numpad digits load whole
> saved configs. Read the numpad warning in
> [In the headset](#in-the-headset) before you press one.

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
Two-handed weapon handling followed on 2026-08-16, head hiding was restored on
2026-08-15, the in-headset settings panel became driveable on 2026-08-22, and the
first-person head-bone anchor was re-confirmed in the headset the same day on the
2026-08-19 game build (72 fps median, 6400 head-bone reads, zero rejects).
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

## The 2026-08 game updates: honest status

Ubisoft shipped three executables in quick succession in August 2026. The mod locates
engine code at specific addresses inside `GRW.exe`, so a new executable means every
address must be re-derived and re-verified before anything is written. **This release
carries verified tables for all of them**, plus the two 2023 builds:

| Table | `TimeDateStamp` | How it was derived |
|---|---|---|
| Steam 2023-09-14 | (2017 lineage) | original derivation, headset-confirmed |
| Epic / Ubisoft Connect 2023-09-08 | (2017 lineage) | machine-verified offline, never headset-confirmed by a store user |
| late-July 2026 update | `6A692948` | full re-derivation, headset-confirmed 2026-08-08 (this is what v0.7.0 was) |
| 2026-08-13 "Last Rites" | `6A75F2F4` | full re-derivation, shipped in the v0.9.0-test1 zip, **new to this repository's source** |
| 2026-08-19 update | `6A7C5143` | **new in this release.** An offline byte comparison proved the patch re-stamped and re-wrapped the executable without moving anything the mod uses: all 51 pinned sites are byte-identical at the same addresses, and the head-setter signature still hits exactly once, on the same function |

The mod identifies which one it is running inside from the executable's own headers,
and every install still verifies the bytes at the address at runtime before writing.
A wrong assumption refuses to arm rather than patching the wrong thing.

**What was restored on the newer executables:**

- **Head hiding in first person.** Out since v0.7.0 because the engine's
  head-visibility function had been recompiled rather than moved. Re-derived
  2026-08-15 by constraining on a class method table with a slot-function
  fingerprint that occurs exactly once in the 411 MB executable, and corroborated
  against the verified 2017 original. It arms on both current executables.
- **The first-person head-bone anchor**, re-confirmed in the headset on the
  2026-08-19 build on 2026-08-22.

**What is still not derived on the newer executables**, and therefore stays off
rather than guessing: the `on_calc_mvp` camera entry point and the projectile-spawn
function. Both were recompiled, not merely moved. Nothing depends on them in a
shipped feature.

**Recommended settings for the current game version:** the August updates added FSR
upscaling; **keep FSR off** while using the mod (it sits inside the render path the
mod manages, untested and likely to blur the eyes). The native immersion toggles
(reduced highlight glow, throwable sightline preview off, hidden-UI sounds) work fine
and are recommended for VR.

If something misbehaves on your game version, please report it with your
`GRWVR\grwxr-<pid>.log`. Nothing silently guesses: every feature that could not be
verified either disabled itself or is listed here.

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
  on, at the instant the engine reads that bone. Bullets follow your gaze for now;
  see "Motion controls: exactly where this is".
- **Two-handed weapon handling.** Your rear hand sets where the weapon is, your
  front hand sets where it points, and twisting your wrist rolls it about its own
  barrel. Front-hand authority fades in with hand separation, so bringing your
  hands together degrades to a one-handed hold rather than to garbage. **This has
  a known bug in this release**, see Known limitations.
- **An in-headset settings panel (`F1`).** Driven with the controller, so
  settings that can only be judged by feel while moving can be changed without
  taking the headset off. The panel polls the controller itself rather than
  waiting on the game, which is what finally made it usable.
- **Whole-config presets on the numpad.** Drop complete copies of `grwxr.cfg`
  into `GRWVR\presets\` and the numpad digits load one each, ten keys per bank,
  with the panel paging between banks. Each load announces its name and logs a
  key-by-key diff of what changed.
- **Spoken feedback (optional, `voice = 1`).** The mod can say what it just did
  through Windows' own speech voice, so a key press confirms itself when you
  cannot see a log.
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
  first aim). It was disabled from v0.7.0 onward because the engine function had
  been recompiled; the address was re-derived on 2026-08-15 and it arms again on
  both current executables.
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
  editor. Whatever is live is whatever `grwxr.cfg` says, always: the panel, the
  hotkeys and the presets all work by changing that one file.
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

### Two-handed handling, and the bug in it (2026-08-16 onward)

A long gun can be held in two hands: the rear hand sets where it is, the front hand
sets where it points, and your wrist rolls it about the barrel. That part works and
is on by default.

**It currently flips 180 degrees at random and the gun appears to split or reverse.**
The mod decides which hand is in front by a sign test with no deadband and no
hysteresis, so hand jitter across that boundary flips the sign frame to frame. A
single instrumented 90-second run recorded **5466 flips**, 61% of the frames where
both hands were engaged. The cause is understood and the counter that proves it is in
the log; the fix is not in this release. Set `wgun_twohand = 0` to go back to a
one-handed hold if it bothers you.

### What is still coming

**Bullets follow your gaze rather than the gun**, so aiming down sights is still the
accurate way to shoot. This remains the last big piece. The previous approach shipped
disabled after being ruled out by construction rather than by tuning: it steered the
game's aim onto the barrel, but in this engine the aim *is* the camera, so both sides
of the error moved together and it could never settle. A comparison against six other
VR mods for closed engines, done 2026-08-22, has since identified which route the
mods that solved this used, and that is what the work is now on.

**Hip-fire spread is untouched**, so even a correctly pointed barrel scatters.
**Your character's arms do not follow the weapon**, so the gun can look detached
from the body. **There are still no hands and no gesture-based weapon manipulation**:
no grabbing, no gesture reloads, no physical mag changes. Reload, swap and vehicle
entry are ordinary button presses.

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

- **The two-handed hold flips 180 degrees at random**, so the gun appears to split
  and reform and reads as reversed every so often. Understood, measured (5466 flips
  in one 90-second run), not fixed in this release. Workaround:
  `wgun_twohand = 0`.
- **The numpad digits load whole configs now.** Before this release, most of them did
  nothing. Now every digit `1`..`9` and `0` replaces your entire `grwxr.cfg` with a
  preset file. If you have no `GRWVR\presets\` folder they do nothing and the log
  says so, but if you do, a stray press changes everything at once. Each load says
  its name out loud so you know it happened. Your live config is backed up once
  before the first load of each session.
- **Preset files must be WHOLE copies of `grwxr.cfg`.** Loading is additive: keys a
  preset leaves out keep whatever the previous preset set them to, they do not reset
  to defaults. The mod warns and names every missing key when it loads a partial one.
- **Bullets follow your gaze, not the gun**, so aim down sights to shoot accurately.
  This is still the last big piece.
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
| Two-handed hold | **~70%, shipped in v0.9.0-test1** | The hold itself works. The front-hand detection has no deadband, so it flips 180 degrees on jitter, which is the split-and-reverse artifact. Root cause found and measured; the fix is next |
| Bullets go where the weapon points | ~60% | Still the last big piece. Three candidate mechanisms were each tested, confirmed to run, and ruled out with evidence. A comparison against six other closed-engine VR mods identified the route the ones that solved it took |
| Settings you can change in the headset | **~85%, shipped here** | The panel opens, the controller drives it, and the numpad loads whole configs. Not every row has been exercised |
| Hip-fire accuracy at ADS grade under VR aim | ~70% | The exact engine flag is located and verified unique in every supported build; one write-route decision remains before it ships |
| Physical sighting (raise the gun, use the sights, no ADS mode) | ~35% | Follows directly from the two rows above. The gun already points where you point it; sights need the bullets fixed first, then an eye-aligned reticle |
| Performance pass for dense towns | ~30% | The engine's shadow-quality lever is located and writable live; a measurement run will decide what ships |
| Aerial vehicle interior camera | ~20% | Ground-vehicle first person already works in practice; helicopter and plane interiors need their camera behaviour characterised first |
| VR arms and hands (IK) | ~15% | The GPU skinning data format was recovered from the game's own shipped shaders. The engine's own IK system is confirmed present, which is the long-term route. Note the weapon does NOT depend on this: it is moved directly, so arms are a separate, harder problem |

Percentages are progress toward shipping, not promises or dates, and they move as
evidence comes in. A row only goes up when something has been watched working in a
headset.

**This release (v0.9.1-test2)** carries support for the current game executables,
head hiding restored, the in-headset settings panel, whole-config presets, and the
corrected eye sign. **The next release** is about the two-hand flip and the bullets
following the gun, after which a public **beta** follows.

## Which version of the game do I need?

**The current one is fine.** As of this release the mod carries verified address
tables for every `GRW.exe` shipped in 2026, including the 2026-08-19 update, which is
the build it was developed and run on. Since the late-July update, Steam and Ubisoft
Connect ship the byte-identical executable, so Ubisoft Connect installs are covered
by the very same tables (headset confirmation from a Ubisoft Connect user is still
welcome).

The mod finds the engine's camera and projection code at specific addresses inside
`GRW.exe`, so every distinct build of that executable needs its own verified address
table. This release carries FIVE; they are listed with their provenance in
["The 2026-08 game updates"](#the-2026-08-game-updates-honest-status) above. The mod
identifies which one it is running inside from the executable's own headers.

If the mod meets a `GRW.exe` it does not recognize (a future game patch, or a build
we have not analysed), it says so in its log, names the builds it knows, and
**installs nothing**: your game runs completely unmodified. The symptom of that state
is a small flat window in the headset that does not respond to head movement, with
controllers possibly still working. Check `GRWVR\grwxr-<pid>.log` for the
"build pin:" line to confirm.

## Requirements

- Ghost Recon Wildlands, any version up to and including the 2026-08-19 update,
  Steam or Ubisoft Connect (identical executable since the late-July update; see
  above). The two 2023 builds remain supported by their own address tables.
- A PC VR headset with an OpenXR runtime. Tested only on Meta Quest 3 over Link cable
  with the Meta Quest Link runtime.
- **Asynchronous Spacewarp must be disabled** (Oculus Debug Tool, set ASW to Disabled).
  The mod manages the stale eye itself; ASW compounds artifacts on top of it.
- A GPU with headroom: the test system is an RTX 5060 Ti 16 GB with a Ryzen 7 9700X.
- To build: MSVC x64 with the Visual Studio C++ workload (provides `cl` and
  `ml64`). See **Building** below for the exact toolchain each release used and
  how `build.bat` selects one.

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

### Build toolchain (which Visual Studio is used)

`build.bat` selects the toolchain in this order, using the first that exists:

1. `C:\Program Files\Microsoft Visual Studio\18\Community` (the project's primary
   build machine; **every release through v0.7.0, and v0.9.1-test2, was compiled
   with this**),
2. `C:\Program Files\Microsoft Visual Studio\2022\Community` (fallback).

**This matters right now.** The releases split by compiler:

| Releases | Machine | Compiler |
|---|---|---|
| through **v0.7.0** (worked on VD / Steam Link) | primary build machine | Visual Studio at `\18\` |
| **v0.8.0, v0.8.1, v0.8.2** (startup crash on VD / Steam Link) | second machine | **Visual Studio 2022 Community, MSVC 14.39.33519** |
| **v0.9.1-test2** | primary build machine | Visual Studio at `\18\` |

A startup crash in the v0.8.x builds (black screen, then the game closes) on
Virtual Desktop and Steam Link is under investigation as a probable toolchain
difference: the code paths involved are byte-identical to v0.7.0. The
`v0.7.0-vs2022-crashfix` pre-release pairs v0.7.0's known-good source with the
VS 2022 compiler to confirm it (see issue #2). Each release records the compiler
it was built with in its notes.

**v0.9.1-test2 is back on the `\18\` toolchain**, which is the one no crash was
ever reported against. That is a change of circumstance, not a fix: issues #2 and
#3 remain open and this release has not been tested against either report. If you
are one of those reporters, trying this one and saying what happens would genuinely
help.

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

Optional: to use the numpad preset loader, make a `GRWVR\presets\` folder and put
whole copies of `grwxr.cfg` in it, named so they sort the way you want them
(`01-baseline.cfg`, `02-wide-stereo.cfg`, and so on). Numpad `1` loads the first,
`0` loads the tenth. With no such folder the digit keys do nothing.

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

**The key map changed in this release.** The numpad digits used to be feature
toggles; they are preset loaders now, and the features they carried moved onto the
`F1` panel. The mod prints its own key list at startup, generated from what the keys
actually do, and that log line is the authority if this table ever drifts.

> [!WARNING]
> **Numpad `1`..`9` and `0` each replace your whole `grwxr.cfg`.** They read
> `GRWVR\presets\*.cfg` in file-name order. With no presets folder they do nothing.
> With one, a stray press changes every setting at once.

**Play keys:**

| Key | Action |
|---|---|
| F1 | Open / close the settings panel (drive it with the controller) |
| F2 | First person on / off (head hiding follows it automatically; also recenters, so a stale reference cannot poison the view) |
| Home | Recenter (look where you want forward to be, then press) |
| Space | Also recenters. Note it still vaults, because the mod polls the keyboard and does not intercept it |
| Numpad . (Decimal) | 1:1 head aim on / off (bullets follow your gaze; default off) |
| Numpad 1..9, 0 | Load a whole preset config from `GRWVR\presets\` (see the warning above) |

**Live tuning keys:**

| Key | Action |
|---|---|
| Insert | Cycle which setting the tuner is editing |
| Page Up / Page Down | Step that setting up / down |
| Delete | Reset that setting |
| Numpad / | World BIGGER (`ipd_scale` up) |
| Numpad * | World SMALLER (`ipd_scale` down) |
| Numpad + | Head roll into the camera (`cam_pose_rot`). Diagnostic: it also makes the camera fight yaw and pitch |
| Numpad - | Skinning palette capture (research aid) |
| End | Start a guided spoken test run for weapon identification (silenced by `voice = 0`) |

Every press logs the name, the value and what it changes. Saving `grwxr.cfg` restores
the file's values over anything a key changed.

**What moved onto the `F1` panel:** barrel aim, trigger ADS, the eye swap and the
camera pose write are Settings rows; the weapon-draw recorder and first person are
Captures rows.

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
| `ipd_scale` | Eye separation multiplier. `1.00` means the headset's measured IPD at 1 world unit = 1 metre, and it is the shipped value in this release |
| `ipd_swap` | **If the stereo looks wrong, try this first.** `1` (the shipped value) swaps which eye gets which offset. See the note below the table |
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
| `wgun` | The controller-tracked weapon. `0` gives you the v0.7.0 behaviour back |
| `wgun_twohand` | Two-handed hold, `1` by default. **Set to `0` if the 180-degree flip bothers you** |
| `wgun_grip_fwd` | Metres along the barrel: slides the gun forward or back in your fist |
| `wgun_grip_two` | Front-hand hold strength, `0` to `1` |
| `voice` | `1` speaks what the mod just did through Windows' own voice, `0` is silent |
| `desktop_fov` | Crop of the desktop recording view, `0` disables |

> [!NOTE]
> **About `ipd_swap` and the shipped stereo values.** A controlled A/B in the headset
> on 2026-08-22, toggling only this one key three times, came out in favour of
> `ipd_swap = 1` at `ipd_scale = 1.00`. That is what this release ships. The
> practical reading is that the eye sign was inverted, and that people running a very
> small `ipd_scale` because "everything looks huge" were compensating for it. **This
> is "better", tested once, and not declared correct**, which is exactly why the
> panel puts the swap on the first row. If it looks wrong to you, flip it and say so
> in an issue.

## Disabling and uninstalling

- Disable temporarily: rename `dxgi.dll` in the game folder (to `dxgi.dll.off`, for
  example). The game then runs completely unmodified.
- Uninstall: run `deploy.bat remove`, or delete `dxgi.dll`, `dxgi_real.dll`,
  `openxr_loader.dll`, and the `GRWVR` folder from the game directory.

## Rules of use

- **Solo campaign only. Never use this in co-op, PvP, or any matchmaking.** The
  2026-08-13 title update removed Easy Anti-Cheat from the game, and that changes
  nothing about this rule: the mod is built and tested for the solo campaign only,
  and it must never run in a competitive context.
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
- **아키아PhD**, for the
  [Naver Cafe post about this mod](https://cafe.naver.com/ca-fe/cafes/27902572/articles/252399)
  and for bringing Korean-speaking players to it. That post is the reason this README
  now exists in Korean ([README.ko.md](README.ko.md)).
- Tom Clancy's Ghost Recon Wildlands is the property of Ubisoft. This project is not
  affiliated with, endorsed by, or supported by Ubisoft, and distributes none of
  their work.

## Supporting the project

A few people have asked whether they could donate, so there is now a page for that:
[buymeacoffee.com/firejumper93](https://buymeacoffee.com/firejumper93). Entirely
optional and never required: the mod is free, nothing is gated behind donations,
and donating changes nothing about what you get. It just helps with tools and late
nights, and it is appreciated.

## License

MIT, see [LICENSE](LICENSE). Portions derived from anvilengine2vr, Copyright (c)
2024 mutars, MIT.
