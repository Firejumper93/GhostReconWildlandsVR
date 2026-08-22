// Voice.cpp - build 107. See Voice.h for why this exists.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <sapi.h>

#include <cstring>

#include "Voice.h"
#include "Log.h"

namespace grwxr {
namespace voice {
namespace {

constexpr int kQueue   = 16;
constexpr int kTextMax = 192;

CRITICAL_SECTION g_lock;
bool     g_lock_ready = false;
char     g_q[kQueue][kTextMax] = {};
int      g_qpause[kQueue] = {};        // ms of silence AFTER each line
volatile long g_w = 0;         // write index
volatile long g_r = 0;         // read index
HANDLE   g_thread  = nullptr;
HANDLE   g_wake    = nullptr;
volatile long g_stop = 0;
volatile long g_on   = 1;
bool     g_speech_ok = false;  // SAPI came up; false = beep fallback
volatile long g_started = 0;

// The beep fallback. Distinct patterns so three different cues cannot be
// confused with each other through a headset.
void beep_pattern(int pattern) {
    switch (pattern) {
        case 1: Beep(880, 120); break;
        case 2: Beep(660, 120); Sleep(60); Beep(880, 120); break;
        default: Beep(880, 110); Sleep(50); Beep(880, 110); Sleep(50);
                 Beep(660, 220); break;
    }
}

DWORD WINAPI worker(LPVOID) {
    // COM on our own thread, never on the game's. Apartment-threaded because
    // that is what SAPI's voice object expects.
    const HRESULT hrco = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ISpVoice* sp = nullptr;
    if (SUCCEEDED(hrco)) {
        const HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr,
                                            CLSCTX_ALL, IID_ISpVoice,
                                            (void**)&sp);
        if (SUCCEEDED(hr) && sp) {
            g_speech_ok = true;
        } else {
            LOG_WARN("voice: SAPI unavailable (0x%08lX). Cues fall back to "
                     "beep patterns; spoken instructions are OFF.",
                     (unsigned long)hr);
        }
    }

    while (!g_stop) {
        WaitForSingleObject(g_wake, 250);
        for (;;) {
            char line[kTextMax];
            int  pause = 0;
            bool have = false;
            EnterCriticalSection(&g_lock);
            if (g_r != g_w) {
                memcpy(line, g_q[g_r % kQueue], kTextMax);
                line[kTextMax - 1] = 0;
                pause = g_qpause[g_r % kQueue];
                g_r = g_r + 1;
                have = true;
            }
            LeaveCriticalSection(&g_lock);
            if (!have) break;
            if (!g_on) continue;

            if (sp) {
                wchar_t w[kTextMax];
                const int n = MultiByteToWideChar(CP_UTF8, 0, line, -1, w,
                                                  kTextMax);
                // Synchronous on purpose: the pause below has to mean silence
                // AFTER the sentence finishes, not 1.5 seconds that overlap it.
                if (n > 0) sp->Speak(w, SPF_DEFAULT, nullptr);
            } else {
                beep_pattern(2);
            }
            if (pause > 0) Sleep((DWORD)pause);
        }
    }

    if (sp) sp->Release();
    if (SUCCEEDED(hrco)) CoUninitialize();
    return 0;
}

}  // namespace

bool init() {
    if (InterlockedExchange(&g_started, 1) != 0) return g_speech_ok;
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!g_thread) {
        LOG_WARN("voice: could not start the speech thread. Cues are OFF.");
        return false;
    }
    // The worker sets g_speech_ok once COM is up; give it a moment so the
    // startup log line tells the truth rather than guessing.
    Sleep(200);
    LOG_INFO("voice: %s", g_speech_ok
                              ? "spoken test instructions are ON (SAPI). Set "
                                "voice = 0 in grwxr.cfg to silence them."
                              : "speech unavailable, falling back to beeps.");
    return g_speech_ok;
}

void say_wait(const char* text, int pause_ms) {
    if (!g_lock_ready || !text || !g_on) return;
    EnterCriticalSection(&g_lock);
    if (g_w - g_r < kQueue) {
        char* dst = g_q[g_w % kQueue];
        strncpy_s(dst, kTextMax, text, kTextMax - 1);
        g_qpause[g_w % kQueue] = pause_ms;
        g_w = g_w + 1;
    }
    LeaveCriticalSection(&g_lock);
    if (g_wake) SetEvent(g_wake);
    // The log carries every spoken line too, so a run can be reconstructed
    // afterwards from the log alone and the tester never has to remember what
    // he was told.
    LOG_INFO("voice: \"%s\"", text);
}

void say(const char* text) { say_wait(text, 0); }

// The standard form for anything key-driven. 1500 ms was the tester's own
// number: long enough to get a hand off a controller and onto a key by feel
// with a headset on, short enough not to feel like waiting.
void say_key(const char* key, const char* why) {
    char line[kTextMax];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "Press %s %s", key,
                why ? why : "");
    say_wait(line, 1500);
}

void cue(int pattern) {
    if (!g_on) return;
    beep_pattern(pattern);
}

void set_enabled(bool on) { g_on = on ? 1 : 0; }
bool enabled() { return g_on != 0; }

void shutdown() {
    if (!g_thread) return;
    g_stop = 1;
    if (g_wake) SetEvent(g_wake);
    WaitForSingleObject(g_thread, 1500);
    CloseHandle(g_thread);
    g_thread = nullptr;
    if (g_wake) { CloseHandle(g_wake); g_wake = nullptr; }
}

}  // namespace voice
}  // namespace grwxr
