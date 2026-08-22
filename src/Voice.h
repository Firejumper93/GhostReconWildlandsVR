// Voice.h - build 107: the mod talks, because the tester cannot read.
//
// WHY THIS EXISTS. Every test this project runs is judged inside a headset,
// where the log, the cfg and the screen text are all unreachable. The standing
// workaround has been to agree a protocol beforehand and hope it is remembered
// through a ten minute run, which has cost real sessions: markers were judged
// by the wrong colour, a key was pressed in the wrong state, and a capture was
// taken before the thing it measured had happened.
//
// `[USER DIRECTIVE, 2026-08-16]` "would it be possible to get an audio cue or
// something, i could hit a hotkey or something once i am loaded in which would
// log the time and the audio could say switch to the next one and then instruct
// to leave the game."
//
// So: spoken instructions, driven by what the probe has ACTUALLY captured
// rather than by a stopwatch, so the tester is told to move on when the data
// is in and told to wait when it is not.
//
// SAFETY. Speech is COM plus a worker thread, which is banned everywhere near
// the render path (rule 8). say() therefore only copies a string into a small
// queue under a lock and returns; all COM lives on a private thread that this
// module owns. Never call say() from Present or from any per-draw or per-bone
// hook. The intended callers are the init thread's hotkey poll and the drain.
//
// If SAPI is unavailable for any reason, the module degrades to distinct Beep
// patterns and says so in the log once, rather than going silent and leaving
// the tester waiting for a cue that is never coming.

#pragma once

namespace grwxr {
namespace voice {

// Starts the worker thread. Safe to call more than once. Returns false if
// speech is unavailable, in which case cues fall back to beeps.
bool init();

// Queue one spoken line. Non-blocking, allocation-free, safe from any thread
// that is not the render thread. Lines are spoken in order.
void say(const char* text);

// Speak, then hold the queue silent for pause_ms before the next line.
// Used so an instruction that names a key is followed by enough quiet for
// the tester to actually find the key by feel with a headset on.
void say_wait(const char* text, int pause_ms);

// "Press <key>, <why>." then a pause long enough to locate it. This is the
// form every key-driven instruction should use: naming the key is what the
// tester needs, and the silence afterwards is what he needs more.
void say_key(const char* key, const char* why);

// A short non-speech cue, for "something happened" without a sentence.
// 1 = a single rising note, 2 = two notes, 3 = three.
void cue(int pattern);

// cfg voice = 0 silences everything without removing the module.
void set_enabled(bool on);
bool enabled();

// Stops the worker. Called from shutdown.
void shutdown();

}  // namespace voice
}  // namespace grwxr
