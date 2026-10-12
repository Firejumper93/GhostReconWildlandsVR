// Log.h - minimal, allocation-light logging for GRW-XR.
//
// Rules this file exists to enforce (CLAUDE.md):
//   Rule 5: the FIRST line of grwxr.log must carry the deployed DLL's own
//           SHA256 and timestamp, so a test result can be matched to the exact
//           binary that produced it. We hash our own module at startup.
//   Rule 8: keep logging, file I/O, locks, COM and allocation out of Present
//           and any per-draw hook. Hence LOG_ONCE and LOG_EVERY_N, and hence
//           the explicit warning below.
//
// The log file is opened once and held open. Writes are buffered by the CRT and
// flushed on each line, which is fine for startup and init but is NOT safe to
// call per frame. Use the throttling macros if you must log from a hot path,
// and prefer not to at all.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>

namespace grwxr {
namespace log {

// Open the log next to the DLL, in <game>\GRWVR\grwxr.log.
// Writes the identity banner required by CLAUDE.md rule 5.
void init(HMODULE self);

void shutdown();

// printf-style. Thread safe via a critical section.
void write(const char* level, const char* fmt, ...);

// BUILD 227: cfg log_diag. 0 (the shipped default) drops the once-a-second
// instrument lines; WARN, ERROR, startup, hooks, features and the heartbeat
// are always written. 1 writes everything, as every build before 227 did.
void set_diag(int on);
int  diag();

// Absolute path of the directory holding our DLL (the game root).
const std::wstring& game_dir();

// Absolute path of our GRWVR data directory. This is the directory the mod
// actually WROTE to, which is not always the one next to the game: see the
// resolver in Log.cpp. Everything that reads or writes grwxr.cfg, presets or
// any other file of ours must go through this, never rebuild the path itself.
const std::wstring& data_dir();

// The log file we actually opened, UTF-8, for the panel to show. Empty until
// init() has opened a file, and empty for good if nothing was writable.
const std::string& log_path();

// True when the game folder refused a write and data_dir() is the fallback.
bool data_dir_is_fallback();

}  // namespace log
}  // namespace grwxr

#define LOG_INFO(...)  ::grwxr::log::write("INFO ", __VA_ARGS__)
#define LOG_WARN(...)  ::grwxr::log::write("WARN ", __VA_ARGS__)
#define LOG_ERROR(...) ::grwxr::log::write("ERROR", __VA_ARGS__)

// For hot paths: log the first time only.
#define LOG_ONCE(...)                                                          \
    do {                                                                       \
        static bool _once = false;                                             \
        if (!_once) { _once = true; LOG_INFO(__VA_ARGS__); }                    \
    } while (0)

// For hot paths: log every Nth call.
#define LOG_EVERY_N(n, ...)                                                    \
    do {                                                                       \
        static unsigned long long _c = 0;                                      \
        if ((_c++ % (n)) == 0) { LOG_INFO(__VA_ARGS__); }                       \
    } while (0)
