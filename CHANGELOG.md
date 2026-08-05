# Changelog

All notable user-facing changes to the mod. Internal build IDs (10m, 11a, 12c and
so on) are the development ledger's numbering and appear here so bug reports can
name an exact build. Entries are verified in the headset on the test system unless
marked otherwise.

## v0.5.0-alpha (builds 20-38, 2026-08-04, PACKAGED RELEASE)

The first packaged release since v0.1.1-alpha. If your installed version still shows
the game as a flat screen floating in the headset, you were on that old build:
everything below (and the v0.2 through v0.4 source-drop features: real stereo depth,
fullscreen view, 4K internal rendering, true first person) is new to you. Delete the
old `dxgi.dll` and install this one, and read the new "Baseline graphics settings"
section in the README before judging performance: in particular, **never use
temporal anti-aliasing (TAA)** with this mod.

### Added

- **Touch controllers as an EMULATED GAMEPAD, so no physical gamepad is needed**
  (builds 22-22.2). The mod translates sticks, triggers, grips, A/B/X/Y and menu into
  ordinary gamepad input. New key `xinput_touch` (default 1). This works reliably.
  **It is gamepad emulation, NOT motion control**, and should not be described as
  motion controls: no hands, no gestures, no weapon manipulation.
- **Controller-driven hip-fire aim with a dot reticle** (builds 23-24). Hip fire
  follows the right controller's ray. New keys `aim_source` (default 1 = controller),
  `aim_ctrl_smooth` (default 0.35), `aim_reticle` (default 1). **Set expectations
  before you try it: see "Controller support, honestly" in the README.** Summary: there
  is no visible gun in your hands, aim CHASES your controller through the game's own
  aim system rather than tracking it 1:1, and hip fire uses the game's wide vanilla
  spread cone so pointing precisely does not make shots land precisely.
- **Aim-down-sights look-to-aim split** (build 25). Holding the left trigger switches
  aim BACK to the head and hides the dot, because the game draws its sight picture at
  view center; leaving aim on the controller made the picture and the impacts
  disagree. The cost is that you play with two different aiming models depending on
  the trigger. Transitional until the weapon itself rides the controller.
- **Config GUI and hot reload** (build 21). `grwxr.cfg` is re-read about one second
  after any save; `tools\cfg_gui\cfg_gui.exe` is a standalone slider editor. All
  numpad tuning keys were REMOVED: only Home (recenter), Numpad 8 (first person) and
  Numpad Decimal (head aim) remain.
- **Resilient VR startup** (build 38). Launching with the headset asleep no longer
  kills VR for the run; the mod arms itself the moment the headset wakes.

### Fixed

- **The first-person close-range body blur is GONE** (build 35). Chest, arms and
  weapon no longer smear in first person. This was the headline known issue of the
  last release.
- **Head hiding is instant on the toggle** (build 34). No more waiting for the next
  aim transition (the first toggle of a session still engages after your first aim).
- **A vertical-aim runaway** under controller aim when the controller pointed nearly
  straight down (builds 20, 23.1): pitch accounting is now bounded and near-vertical
  rays no longer drag yaw.

### Known rough edges in this release

- **There are no real motion controls.** Touch is emulated as a gamepad; the only
  motion-tracked input beyond head tracking is aim DIRECTION from where the right
  controller points, and that is the weakest part of the mod: no gun visibly in your
  hands, aim chases rather than tracks, hip fire is inaccurate by the game's design,
  ADS abandons controller aim, and there are no hands, gestures or weapon
  manipulation. The README section "Controller support, honestly" spells out each
  one. Fixing the first of these (the weapon model riding your controller) is the
  current focus.
- Hip-fire spread defeat under VR aim is designed but NOT in this release.

### Performance

- Verified on the test system: 72 fps sustained in open-world play at the baseline
  settings, including with Supersampling 0.90 and SMAA.
- A save carrying TAA settings measured a sustained drop into the low 60s. TAA also
  ghosts under alternate-eye rendering. Use SMAA or no AA, never TAA.

## v0.4.0-alpha (build 19, 2026-08-03, source drop; a packaged release follows with the beta)

The three oldest gaps in first person closed in one night: the camera now rides the
character's actual head bone, the head is hidden from the inside, and an optional 1:1
head-aim mode makes bullets follow your gaze. Headline known issue: your own chest and
hands blur at close range in first person (the world stays sharp); removing that blur
is the focus of the next update.

### Added

- **Head-bone first person** (build 16a). The first-person viewpoint anchors to the
  player's animated head bone instead of the character origin: eye height tracks
  standing, crouch and prone automatically, the camera follows animation without
  jumping, and idle head motion is real. New keys `fp_head_anchor` (default 1) and
  `fp_head_eye` (offset above the bone, default 0.10 m); Numpad 7/4 tune the offset
  in 0.02 m steps while anchored.
- **Head hiding in first person** (build 18). The character's head is hidden through
  the engine's own head-visibility mechanism whenever first person is on, and restored
  when it is off. The engine applies visibility on aim and camera transitions, so the
  hide or un-hide lands at the next such transition (tapping aim once is enough); this
  is expected.
- **Continuous 1:1 head aim** (build 19, Numpad Decimal, default off). Head yaw and
  pitch feed the game's own aim path as the engine consumes them, so view, reticle and
  bullets follow the gaze exactly while the right stick still turns underneath.
  Aiming down sights pauses the injection so scopes stay true. New config keys
  `aim_yaw_sign` / `aim_pitch_sign` calibrate the engine's aim directions (defaults
  -1 / +1 for the current game build; the axes genuinely differ). If a future game
  patch reverses an axis, flip that sign.

### Known issues

- Close-range blur on your own chest and hands in first person; the world is sharp.
  Fix in progress (see the README roadmap).
- No IK arms or hands yet; the weapon is not held by your controllers.
- The camera can attach to the wrong body after a respawn or fast travel; toggle first
  person off and on while facing your character to re-acquire.
- Aerial vehicle interiors are not yet wired up (ground vehicles are playable).
- Frame rate dips below 72 in dense towns on the test system.

## v0.3.0-alpha (build 15L, 2026-08-02)

The blur is gone, controller-pointing aim arrived, and first person now attaches to your
actual character. Headline known issue: first person is anchored to the character's
origin rather than the head bone, so the viewpoint can sit slightly off, does not
follow idle animation, and does not compensate for crouch or prone. Head-bone
tracking is the focus of the next update.

### Added

- **4K internal rendering with no desktop changes** (builds 15a, 15b, 15c). The mod
  hooks the DXGI factory to create the game's swapchain at `upsize_width` x
  `upsize_height` (default 3840x2160), keeps that size across the engine's own
  `ResizeBuffers` call, and reports the upsized client area to the game so its entire
  render pipeline runs at that resolution. The v0.2.0 blur is fixed at the source, and
  no display or driver setting is required. Lower the two keys (for example
  3200x1800) to trade sharpness for frame rate.
- **Controller-pointing aim** (build 14d). Pointing the right Touch controller away
  from head center turns the game's own aim, injected as relative mouse motion, so
  ballistics, HUD and crosshair stay true. Tunable via `aim_deadzone_deg`, `aim_gain`,
  `aim_max_rate`; `aim_steer=0` disables.
- **Aim down sights and fire on the right trigger** (builds 14f, 14h). A partial
  squeeze engages the game's own ADS; a full squeeze fires and holds for automatic
  weapons, releasing back into the ADS band so you can stop shooting while staying
  aimed. `aim_ads=0` and `aim_fire=0` disable them independently.
- **Anchored first person** (builds 15e, 15L). The first-person toggle now places the
  viewpoint on the player character itself rather than pushing it forward from the
  third-person camera, so it no longer slides when the chase camera pitches or orbits.
  The player is identified through the engine's own player component, so the camera
  attaches to your body and not to a nearby NPC. New keys `fp_eye` (eye height above
  the character origin, default 0.85 m) and `fp_anchor_side` (lateral centering);
  Numpad 7/4 and 6/5 tune them live while anchored.
- **Cropped desktop recording view** (build 12c, verified this cycle). Numpad /
  toggles it; `desktop_fov` sets the crop.

### Fixed

- **Rotation stutter** (build 13a). The layer submitted to the compositor now carries
  the exact head orientation the frame was rendered with, instead of the pose sampled
  at present time. Under alternate-eye rendering the engine's pipeline depth varies
  frame to frame, which made that mismatch oscillate and read as a shimmer during head
  turns.
- **Camera jumping to the barrel of the gun while aiming** (build 15e.2). Player
  identification is frozen whenever the rendered field of view is inside the aim band,
  where the weapon's own rig sits dead center in front of the camera.

### Changed

- **SMAA is now the recommended anti-aliasing mode.** At 4K the temporal modes blend
  the two alternating eye viewpoints into a shimmering halo on static edges; the blur
  of the old 1080p capture was hiding it.
- Recommended settings are Supersampling 0.90 with a 72 fps limit. At 4K with
  Supersampling 1.0 the test system cannot hold 72.

### Known issues

- First person does not track the head bone (see above); your character's head is not
  hidden, so you may see hair or helmet geometry at some angles.
- No IK arms or hands; the weapon is not held by your controllers.
- The camera can attach to the wrong body after a respawn or fast travel. Toggle first
  person off and on while facing your character to re-acquire.
- Vehicles in first person are unfinished.
- Sky and cloud registration at wide field of view is still imperfect.
- Frame rate dips below 72 in dense towns on the test system.

## v0.2.0-alpha (build 12c, 2026-07-30)

Headline known issue: the fullscreen image is blurry in this version (the
capture source is the game's 1080p backbuffer). This is known and is the
focus of the next update.

### Added

- **Fullscreen view** (builds 12a, 12a.1). The projection hook overrides the
  game's rendered field of view (`fullscreen_fov`, default 1.92 rad, on by
  default) so the image fills the headset instead of appearing as a 45 degree
  window. The override applies only inside a band (0.60 to 1.35 rad) so scopes,
  menus, and the engine's sky and reflection cubemap captures keep their real
  projections. Numpad 1 toggles it, Numpad + and Numpad 2 adjust it.
- **First-person demo mode** (builds 11c, 11f). Numpad 8 pushes the camera from
  the chase position onto the character (`fp_forward` 2.20 m, `fp_side` -0.40 m,
  `fp_up` 0 m, all tunable live and in the config). Demo quality: culling pop at
  screen edges, visible hair, and no aim integration yet. Vehicle cabins are not
  reachable yet (the forward clamp stops short of the vehicle chase camera
  distance; a fix is queued).
- **Flat scope with true ballistics** (build 11d). Below `mono_scope_fov`
  (default 0.30 rad) the mod stops writing the camera entirely, so scoped frames
  render exactly as the flat game: the scope overlay stays aligned and bullets
  land on the crosshair. Head tracking resumes the moment you leave the scope.
- **Magnified scope display window** (build 11e). Zoomed optics are drawn across
  `scope_display_fov` (default 0.5236 rad) instead of their true tiny angle, so
  a 6x scope actually magnifies instead of shrinking to a distant dot.
- **Live field-of-view placement** (build 11a family). Each eye's image is drawn
  at the exact angular size the engine rendered that frame, per eye, instead of
  assuming a fixed 45 degrees. This removed the residual "off-ness" on objects
  in motion and the slight scale disagreement between the eyes.
- **Desktop recording view** (build 12c, **not yet headset-verified**). Numpad /
  redraws the desktop window from the left eye's capture cropped to
  `desktop_fov` (default 0.90 rad), so flat-screen recordings show a normal
  stable view instead of the wide-angle warp and eye alternation. The headset
  path is untouched. This build is compiled but has not completed a verified
  play session yet.
- New config keys: `fullscreen_fov`, `mono_scope_fov`, `scope_display_fov`,
  `fp_forward`, `fp_side`, `fp_up`, `desktop_fov`. All optional; the log prints
  the exact line to persist whenever a tuning key changes a value.

### Fixed

- **Black headset view with live tracking** (build 11a.2). The Oculus runtime
  creates its swapchain textures typeless; render target views made with a null
  descriptor silently fail on those, which blacked out the first draw-based
  submission path. Views are now created with an explicit typed descriptor, and
  if that ever fails the mod falls back automatically to the older copy path
  instead of showing black.
- The blit now also clears GPU predication state around its draws (build 11a.1);
  the engine's occlusion-culling predicate could silently skip them.

### Changed

- Temporal anti-aliasing is no longer discouraged: SMAA and TAA were both tested
  under the stereo setup and both look and run fine. AA mode is user preference.
- Foreign OpenXR API layers (OBS Mirror, Virtual Desktop compatibility) are
  suppressed for the game's process at startup to remove confounders.

### Known issues in this snapshot

- The fullscreen image is soft: the capture source is the 1080p backbuffer.
  Raising capture resolution is the current focus.
- Wide-angle rendering can look warped toward the edges; under investigation.
- Sky and cloud registration fix (12a.1 band limit) is deployed but its final
  verification is pending.
- Aim outside of scopes still follows the game's own aim rather than your view;
  aim integration belongs to the planned IK phase.

## v0.1.1-alpha (build 10m, 2026-07-29)

- Corrected the eye-offset sign: positive `ipd_scale` is now the correct-depth
  direction.
- Tuned depth baseline shipped: `grwxr.cfg` with `ipd_scale=0.50`.
- Removed the coarse merge-hunt keys that could silently de-tune depth (an Enter
  press in a menu was stepping the value); fine step moved to Numpad 9,
  Numpad * resets to the config value, clamp tightened.

## v0.1.0-alpha (2026-07-29)

- Initial public release. Native OpenXR session on the game's own D3D11 device,
  full head tracking driving the real game camera, stereoscopic depth via
  alternate-eye rendering with per-eye canvases placed angle-correct in each
  eye's true display frustum, Home-key recenter, live eye-separation tuning,
  72 Hz pacing.
