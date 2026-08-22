// Presets.h - whole-config hot swap, one numpad key per config.
//
// BUILD 129 (owner directive, 2026-08-22): "now we can hot reload whole CFG's
// and map them to 10 keys via the numpad. We can test over 100 configs in a
// single session now."
//
// This is the third rung of the same argument that produced build 97's per
// setting hotkeys and build 99's tuner. Build 99's note says it plainly: one
// key per setting does not scale, so the tuner made one key select WHICH
// setting is live. That still tunes one value at a time, and a feel judgement
// (does the gun sit right, does the world fuse) is a judgement about a whole
// combination of values, not about one of them. So: one key loads a whole
// config.
//
// THE MECHANISM ALREADY EXISTED AND IS NOT NEW HERE. Build 21 added
// poll_config(), which stats GRWVR\grwxr.cfg once a second and re-runs
// load_config() when the write time moves, and load_config() is idempotent
// (every key clamps and lands in an atomic or a seqlock publish). Loading a
// preset is therefore: copy the preset file over grwxr.cfg, then call
// poll_config(). No new parser, no second source of truth, and the invariant
// the whole tree already assumes holds unchanged:
//
//     WHAT IS LIVE IS WHAT grwxr.cfg SAYS.
//
// The panel's Apply, the hot reload, and every log dump already depend on
// that. A preset system that kept its values somewhere else would break it.
//
// THE TRAP, AND IT IS THE REASON FOR THE COMPLETENESS WARNING BELOW.
// load_config() is ADDITIVE, not authoritative: it sets the keys it finds and
// leaves every other value at whatever the last load left it. So loading
// preset B after preset A does NOT give you B, it gives you B over the
// remains of A for any key B omits. That is exactly the class of silent wrong
// state that cost builds 56, 59, 60 and 82. Presets must therefore be WHOLE
// configs, and load_slot() logs every known key the incoming file does not
// mention so a partial preset announces itself instead of producing a quietly
// false test result.
//
// IT SPEAKS, for the NUMPAD 5 reason (2026-08-15): a key that silently does
// nothing, or silently does something else, costs a whole session to diagnose
// because the tester is wearing a headset and cannot read the log or the
// screen. Every load says the preset's name out loud. An empty slot says so.
//
// THREADING, and it is why load_slot() only queues. This module does file I/O
// by design, and the F1 panel runs inside Present, where rule 8 forbids it. So
// every entry point below is a cheap atomic store that anyone may call, and
// pump() on the init thread does the actual work. One path for the keys and
// the panel both, rather than two paths and a rule 8 violation on one of them.

#pragma once

namespace grwxr {
namespace presets {

// Scan GRWVR\presets for *.cfg and log the inventory. Safe to call when the
// directory does not exist: it logs how to create one and disables the keys.
// Init thread only, at startup.
void init();

// Perform any queued load or rescan. Init thread, once per key-poll tick.
void pump();

// Load the preset on numpad key `slot` (0..9) of the current bank. Out of
// range, or an empty slot, logs and SAYS so rather than doing nothing.
// Queues; pump() does it. Safe from Present.
void load_slot(int slot);

// Page the ten keys over a longer list, so 100 configs need 10 keys and not
// 100. dir is +1 or -1; the bank wraps.
void bank_step(int dir);

// Panel read-outs. name_of_slot returns nullptr for an empty slot.
int         count();          // presets found on disk
int         bank();           // current bank, 0 based
int         bank_count();     // how many banks the inventory fills
const char* name_of_slot(int slot);   // 0..9, within the current bank
const char* current();        // last preset loaded, or nullptr

// Re-scan the directory without a relaunch, so presets can be added while the
// game runs (he authors them from a second machine or an editor on the side).
void rescan();

}  // namespace presets
}  // namespace grwxr
