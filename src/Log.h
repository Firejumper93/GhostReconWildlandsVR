// Log.h - minimal, allocation-light logging for GRW-XR.
//
// Rules this file exists to enforce (project rules):
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
// Writes the identity banner required by project rule 5.
void init(HMODULE self);

void shutdown();

// printf-style. Thread safe via a critical section.
void write(const char* level, const char* fmt, ...);

// Absolute path of the directory holding our DLL (the game root).
const std::wstring& game_dir();

// Absolute path of our GRWVR data directory.
const std::wstring& data_dir();

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
