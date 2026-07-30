// Sig.h - AOB signature scanning over the live GRW.exe image.
//
// project rule 7: locate engine code with unique AOB signatures, never ship a
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

}  // namespace sig
}  // namespace grwxr
