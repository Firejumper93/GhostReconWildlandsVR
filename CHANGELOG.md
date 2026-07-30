# Changelog

All notable user-facing changes to the mod. Internal build IDs (10m, 11a, 12c and
so on) are the development ledger's numbering and appear here so bug reports can
name an exact build. Entries are verified in the headset on the test system unless
marked otherwise.

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
