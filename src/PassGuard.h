// PassGuard.h - BUILD 259: a fail-closed guard on the game's ProjShadow pass body.
//
// THE FAULT IT ANSWERS. Getting into a vehicle (or shooting one) with the second camera alive faults the game
// at GRW.exe RVA 0xE7C1812 (TU25): a null read, bad address 0x18, followed by a wedge. 2026-10-07 static work
// (work order WORK-ORDER-2026-10-07-vehicle-crash.md section 0b) traced it to the ProjShadow pass body
// FUN_14dfba720: it computes [renderer+0xB60]+0x18 (the projector-shadow atlas) and compares a 32-bit value
// there with a global, with no null check. The atlas was NULL on every logged row of every crash run, and
// cam2_noprojshadow (bit 4 of renderer+0x8C0 cleared at graph-builder entry) was ON and applied in all of
// them, so a ProjShadow0 pass still ran. Which of several causes lists the pass with no atlas is not yet known.
//
// WHAT THIS DOES. Redirects the pass body's own jump thunk (RVA 0x14DFF20, E9 + int3 padding, the only
// rel32 reference to the body) to a replacement that reads renderer+0xB60 first. Atlas present: the real
// body runs, untouched. Atlas NULL: the body is skipped for this call (the pass records nothing), a counter
// ticks, and the first few skips snapshot the state the cause analysis needs. Under EVERY candidate cause
// the pass would otherwise dereference null, so skipping it is fail-closed and cannot make a run worse.
//
// RULE 8. The replacement runs on the game's command-list worker threads: it logs nothing, takes no lock,
// allocates nothing. Counters are relaxed atomics, the two reads are SEH-guarded. All logging is the 1 Hz
// tick() on the init thread.
// RULE 7. TU25 only (PE timestamp and SizeOfImage), the thunk bytes, the body prologue and the atlas-read
// instruction are all verified before one byte is written. Anything else installs nothing.
//
// cfg projshadow_guard (default 1). 0 leaves the hook installed but passing straight through.

#pragma once

namespace grwxr {
namespace passguard {

void set_enabled(bool on);
void tick();   // init thread, once a second: install when due, log the counters

}  // namespace passguard
}  // namespace grwxr
