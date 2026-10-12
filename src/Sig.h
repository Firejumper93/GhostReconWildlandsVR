// Sig.h - AOB signature scanning over the live GRW.exe image.
//
// CLAUDE.md rule 7: locate engine code with unique AOB signatures, never ship a
// guessed hardcoded address, and fail loudly on a miss while leaving the game
// running normally. CURRENT-STATE.md standing rules 1 to 3 add three more:
//
//   1. Never fall back to a hardcoded address from another build. The reference
//      implementation's FuncRelocation returns a stale fallback on a miss and
//      the caller hooks it unchecked. Ours returns std::optional and the caller
//      must handle a miss.
//   2. Always compile the scanner in. The reference gates it behind a
//      SIGNATURE_SCAN macro and compiles it out of Release, which silently ships
//      a hardcoded-address build.
//   3. Never filter by section name. Wildlands' section table is scrambled and
//      the real code lives in a section called .sbss (docs/RE-notes.md).
//
// Scanning 369 MB takes a moment, so this runs on the init thread, never on a
// render thread.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <optional>

namespace grwxr {
namespace sig {

// Byte range of the main module, discovered from its PE headers at runtime.
struct Image {
    uint8_t*  base = nullptr;
    size_t    size = 0;
};

// Reads the headers of GRW.exe as loaded. Fails if the module cannot be read.
std::optional<Image> main_image();

// Scan for a pattern written as "48 89 E0 53 ?? 81 EC", where ?? (or ?) is a
// wildcard byte. Returns the address of the FIRST match only if the pattern
// matches exactly once in the image; ambiguity is treated as a miss, because a
// signature that matches twice is not a signature.
//
// `matches_out` receives the true match count so the caller can log whether it
// missed (0) or was ambiguous (>1). That distinction matters: 0 usually means
// the game updated, >1 means our pattern is too short.
std::optional<uint8_t*> find_unique(const Image& img, const char* pattern,
                                    size_t* matches_out = nullptr);

// Find the single `E9 <rel32>` thunk whose jump resolves to `target`.
//
// WHY THIS EXISTS. Every other hook here takes its thunk RVA from the GameBuild
// pin table, and HANDOFF-2026-08-28 method rule 6.3 measured that vtable RVAs
// agree in 0 of 97 cases across any pair of the four pinned builds, including
// two builds of the SAME game version from different stores. A pinned thunk RVA
// is therefore wrong per store, not merely per patch. Deriving the thunk from
// the body at runtime removes the pin from this hook entirely.
//
// Method rule 6.2 is what makes it tractable: every function reaches its callers
// through exactly ONE jmp thunk (271,346 thunks against 316,202 .pdata records).
// So body -> thunk is a well-posed question with one answer, and uniqueness is
// the correctness check rather than a hopeful assumption. More than one match
// means that rule does not hold for this function on this binary, so we return
// nothing rather than pick one, exactly as find_unique does.
//
// The slot must also be safe for ThunkHook: 5 bytes of E9 rel32 followed by at
// least 9 bytes of CC padding, which is what lets a 14-byte absolute indirect
// jump fit without touching anything outside the slot. An unpadded thunk is not
// returned, because overwriting it would run past the slot into whatever
// follows.
//
// `matches_out` receives the true count, so a caller can tell "the game was
// updated" (0) from "rule 6.2 does not hold here" (>1).
std::optional<uint8_t*> find_thunk_to(const Image& img, const uint8_t* target,
                                      size_t* matches_out = nullptr);

}  // namespace sig
}  // namespace grwxr
