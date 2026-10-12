// MvpHook.h - BUILD 267: the TU25 on_calc_mvp (camera recompute) entry hook the address table never had.
// The body (RVA 0x0138E5F0) has no E9 thunk, so ThunkHook cannot be used. The flat FP mod's technique (FpCamera.cpp
// synthesize_mvp_hook, confirmed in game 2026-09-25): put the 14 byte `jmp [rip+0]` into the int3 padding in front of the body
// FIRST, then repoint the single `call rel32` that reaches the body with one 4 byte store. Fail closed: nothing is written
// unless the build, the body bytes, the padding and the call site all verify.
#pragma once
#include <cstdint>
namespace grwxr {
namespace mvp {
// `entry` is the asm stub (grwxr_probe_entry_6) that saves the argument registers, calls the recorder and tail jumps to
// *original_out. *original_out is written BEFORE the call is repointed, so the stub never tail-jumps to null.
bool install(uint8_t* image_base, uint32_t pe_timestamp, uint32_t pe_size, void* entry, void** original_out);
void restore();
bool live();   // the call site is repointed and verified
}  // namespace mvp
}  // namespace grwxr
