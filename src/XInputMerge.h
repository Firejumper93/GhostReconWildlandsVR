// XInputMerge.h - merge Quest Touch state into the game's gamepad reads.
//
// Build 22. The game resolves XInputGetState at runtime into a shadow-IAT
// slot (RE-notes 2026-08-03: slot RVA 0x03851120 [VERIFIED], decoded against
// the real XInput1_3 export table; the DLL loads from the game directory).
// Instead of shipping a replacement XINPUT1_3.dll, which would overwrite a
// file the game ships, we redirect that one pointer in memory from inside
// dxgi.dll: every XInputGetState call returns the physical pad state with
// the Touch controllers' sticks, triggers and buttons merged in. With no
// physical pad connected the detour fabricates a connected pad from Touch
// alone, which is what makes gamepad-free play work.

#pragma once

namespace grwxr {
namespace xin {

// Called at 1 Hz from the init thread until it succeeds. The slot is only
// patched after verifying it currently points into the loaded XINPUT1_3.dll
// module (identity check, rule 7); an unresolved or foreign value logs once
// and leaves the game untouched.
void poll_install();

// Telemetry on the init thread, same cadence as the other drains.
void drain();

// cfg key xinput_touch. Disabled = the detour passes through untouched.
void set_enabled(bool on);

// cfg key stick_pitch (build 49). Off = the right stick's Y axis is zeroed
// in every pad state the game sees, so the head is the only pitch source.
void set_stick_pitch(bool on);

// True when the detour is installed, enabled, and Touch state is live.
// VRMirror uses it to suppress the older mouse-synthesis paths (steer, ADS,
// fire) so the game's input scheme does not flap between mouse and pad.
bool merging_live();

}  // namespace xin
}  // namespace grwxr
