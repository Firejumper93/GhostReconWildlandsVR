// ThunkHook.h - redirect an engine call by rewriting a jump thunk, not a
// function prologue.
//
// WHY THIS EXISTS, AND WHY IT IS THE SAFEST HOOK AVAILABLE ON THIS TARGET
//
// docs/RE-notes.md established that GRW.exe does not call its camera maths
// functions directly. Every call goes through a 5-byte relative jump thunk in
// the .edata section:
//
//     caller  ->  E9 <rel32>  (thunk, .edata)  ->  real function (.sbss)
//
// and each thunk sits alone in a 16-byte slot padded with int3:
//
//     E9 5B 4E 1C 0B CC CC CC CC CC CC CC CC CC CC CC
//
// That padding is the opportunity. A 14-byte absolute indirect jump
//
//     FF 25 00 00 00 00        jmp qword ptr [rip+0]
//     <8-byte absolute target>
//
// fits entirely inside the thunk's own slot, so redirecting a call costs 14
// bytes written into a table of jump stubs. Compare that with the usual
// alternative, a detour on the function prologue, which on this target would be
// materially worse:
//
//   1. It writes into real engine code rather than a stub table.
//   2. Both camera functions we care about begin with rsp-relative instructions
//      (`mov rax, rsp` / `mov [rsp+8], rbx`), so a copied-prologue trampoline
//      would execute them at the wrong stack depth and corrupt the frame. It
//      would need a hand-written assembly stub to be correct.
//   3. It needs a length disassembler to find a safe instruction boundary.
//
// The thunk rewrite needs none of that. Our replacement function is entered by
// a jump from the caller's `call`, exactly as the real function would be, so it
// is an ordinary function with the identical signature: same arguments in the
// same registers and stack slots, same return address on the stack. No
// trampoline, no stolen instructions, no assembly.
//
// HAZARD, HONESTLY STATED. This is still a write into GRW.exe's memory image,
// and docs/HANDOFF.md records that nothing had written into the image up to this
// point and that no anti-tamper trigger had fired. This is therefore the first
// build that could plausibly trip one, and per the standing rule the check for
// any anomaly is to rename dxgi.dll and see whether the symptom persists.
// It is not a write to any file: the game install is untouched on disk.
//
// Restore() puts the original 16 bytes back, so the change is exactly
// reversible within the process.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>

namespace grwxr {
namespace hook {

// One redirected thunk slot.
class ThunkHook {
public:
    // `thunk` must point at a 5-byte E9 rel32 followed by at least 9 bytes of
    // CC padding, and its jump must resolve to `expected_target`. Both are
    // verified before anything is written; a mismatch installs nothing and
    // returns false, leaving the game running normally (project rule 7).
    bool install(uint8_t* thunk, void* expected_target, void* replacement,
                 const char* name);

    void restore();

    bool installed() const { return installed_; }

    // The real function, for the replacement to call through to.
    void* original() const { return original_; }

private:
    uint8_t*    thunk_        = nullptr;
    void*       original_     = nullptr;
    uint8_t     saved_[16]    = {};
    bool        installed_    = false;
    const char* name_         = "";
};

}  // namespace hook
}  // namespace grwxr
