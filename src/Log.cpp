#include "Log.h"

#include <bcrypt.h>
#include "Perf.h"   // BUILD 215: the log counts its own lines and bytes
#include <atomic>
#include <cstdarg>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace grwxr {
namespace log {
namespace {

FILE* g_file = nullptr;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
std::wstring g_game_dir;
std::wstring g_data_dir;
std::string g_log_path;
bool g_fallback_used = false;

// 0 unresolved, 1 resolving, 2 ready. The data directory has to be resolvable
// from ANY thread at ANY time, because the factory stubs read the cfg and can
// run before the init thread has done anything (FactoryHook.cpp's own note:
// "These functions can run before the init thread has called log::init", and
// dllmain's: "the factory stubs may already have fired"). Publishing g_data_dir
// through this guard makes both the ordering and the visibility well defined.
std::atomic<int> g_dir_state{0};
bool g_no_writable_dir = false;

// SHA256 of a file, via CNG so we do not depend on any external library.
// Returns an empty string on failure rather than throwing.
std::string sha256_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    std::vector<unsigned char> obj;
    unsigned char digest[32] = {};
    DWORD cb = 0, objLen = 0;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0) != 0) goto done;
    obj.resize(objLen);
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) != 0) goto done;

    {
        std::vector<unsigned char> buf(1 << 20);
        DWORD read = 0;
        while (ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr) && read > 0) {
            if (BCryptHashData(hash, buf.data(), read, 0) != 0) goto done;
        }
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) goto done;

    {
        static const char* hex = "0123456789abcdef";
        out.reserve(64);
        for (unsigned char b : digest) { out += hex[b >> 4]; out += hex[b & 0xF]; }
    }

done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return out;
}

// ------------------------------------------------------------- the data dir --
//
// Where we write, and what happens when the game folder refuses us.
//
// Until 2026-09-04 there was no answer to that question. init() created
// <game>\GRWVR, IGNORED the result, tried to open the log twice in that SAME
// directory, and on failure returned silently. write() begins with
// `if (!g_file) return;`, so from that moment every LOG_ call in the mod became
// a no-op for the life of the process: the mod ran, hooked and armed with no
// log at all, and the user had nothing to report with.
//
// The one tester we have who installed under Program Files (x86) DID get a log
// written there. That tells us only that THAT user's process could write there.
// It says nothing about the next user, who may not be running elevated, and a
// diagnostic that vanishes exactly when the install is unusual is missing where
// it is needed most.
//
// So: probe every candidate with a real write before choosing it, fall back in
// a fixed order, and record every attempt with its GetLastError so the chosen
// directory is never a mystery.
struct DirAttempt {
    std::wstring path;
    DWORD        err;   // 0 = usable
    const char*  what;  // the stage that failed
};
std::vector<DirAttempt> g_dir_attempts;

// What the settings carry-over did. Recorded rather than logged because it
// happens BEFORE the log file exists, and a carry-over that quietly did nothing
// would leave the tester editing a grwxr.cfg the mod no longer reads.
enum SeedCfg { kSeedNoSource, kSeedCopied, kSeedKeptExisting, kSeedFailed };
SeedCfg g_seed_cfg = kSeedNoSource;
bool    g_seed_stale = false;   // kept copy predates the installed one
DWORD   g_seed_cfg_err = 0;
int     g_seed_presets = 0;
DWORD   g_seed_presets_err = 0;

// Create if needed, then prove we can actually WRITE. An existing directory we
// may not write into is precisely the protected-folder case, so the create
// succeeding (or reporting ERROR_ALREADY_EXISTS) proves nothing on its own.
bool dir_is_usable(const std::wstring& dir, DWORD* err, const char** what) {
    *err = 0;
    *what = "";
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        const DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            *err = e;
            *what = "CreateDirectory";
            return false;
        }
    }
    // Deliberately NOT FILE_FLAG_DELETE_ON_CLOSE: that flag makes the kernel
    // add DELETE to the requested access, so a directory that grants write but
    // not delete would be judged unusable and a user who was working fine
    // would be moved onto the fallback for no reason. Ask for exactly the right
    // we actually need, then clean up.
    //
    // Built with wstring, never swprintf_s into a fixed buffer: swprintf_s does
    // not truncate, it calls the invalid parameter handler, and the release CRT
    // default for that is to terminate the process. This runs on every launch.
    wchar_t pid[32];
    swprintf_s(pid, L"%lu", GetCurrentProcessId());
    const std::wstring probe = dir + L"\\grwxr-write-test-" + pid + L".tmp";

    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        *what = "write probe";
        return false;
    }
    CloseHandle(h);
    DeleteFileW(probe.c_str());   // best effort; the file is tiny and pid-named
    return true;
}

std::wstring env_dir(const wchar_t* var) {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
    return (n == 0 || n >= MAX_PATH) ? std::wstring() : std::wstring(buf);
}

// Carry the INSTALLED settings across to a fallback directory, so falling back
// changes where we write and not what the tester configured. Never overwrites a
// file already in the fallback: once that copy exists it is the live one, and
// the installer's copy must not silently undo the user's edits on next launch.
void seed_fallback(const std::wstring& from, const std::wstring& to) {
    const std::wstring cfg_from = from + L"\\grwxr.cfg";
    const std::wstring cfg_to   = to + L"\\grwxr.cfg";
    if (GetFileAttributesW(cfg_to.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_seed_cfg = kSeedKeptExisting;
        // A kept copy that predates the installed one is missing whatever keys
        // an update added. Cheap to notice, confusing to debug without it.
        WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
        if (GetFileAttributesExW(cfg_from.c_str(), GetFileExInfoStandard, &a) &&
            GetFileAttributesExW(cfg_to.c_str(), GetFileExInfoStandard, &b) &&
            CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) > 0) {
            g_seed_stale = true;
        }
    } else if (GetFileAttributesW(cfg_from.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_seed_cfg = kSeedNoSource;
    } else if (CopyFileW(cfg_from.c_str(), cfg_to.c_str(), TRUE)) {
        g_seed_cfg = kSeedCopied;
    } else {
        // A folder locked down hard enough can refuse the READ too, in which
        // case the mod runs on built-in defaults. That must be visible.
        g_seed_cfg = kSeedFailed;
        g_seed_cfg_err = GetLastError();
    }

    const std::wstring pre_from = from + L"\\presets";
    if (GetFileAttributesW(pre_from.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const std::wstring pre_to = to + L"\\presets";
    CreateDirectoryW(pre_to.c_str(), nullptr);
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW((pre_from + L"\\*.cfg").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) {
        g_seed_presets_err = GetLastError();
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (CopyFileW((pre_from + L"\\" + fd.cFileName).c_str(),
                      (pre_to + L"\\" + fd.cFileName).c_str(), TRUE)) {
            ++g_seed_presets;
        } else {
            g_seed_presets_err = GetLastError();
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// The directory this DLL sits in, derived from our OWN module rather than from
// a handle passed to init(), so resolution never depends on init() having run.
std::wstring own_module_dir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&own_module_dir), &self);
    wchar_t p[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring s = p;
    const size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : s.substr(0, slash);
}

// Choose the data directory exactly once, whichever thread asks first.
void resolve_data_dir_once() {
    int expect = 0;
    if (!g_dir_state.compare_exchange_strong(expect, 1, std::memory_order_acq_rel)) {
        while (g_dir_state.load(std::memory_order_acquire) != 2) Sleep(0);
        return;
    }

    g_game_dir = own_module_dir();

    // In order: where the installer put it, then per-user app data, then temp.
    // The first two are the real answer; temp exists so that "no log at all"
    // stops being a reachable state on any machine.
    const std::wstring installed = g_game_dir + L"\\GRWVR";
    std::vector<std::wstring> candidates;
    candidates.push_back(installed);
    const std::wstring lad = env_dir(L"LOCALAPPDATA");
    if (!lad.empty()) candidates.push_back(lad + L"\\GRWVR");
    const std::wstring tmp = env_dir(L"TEMP");
    if (!tmp.empty()) candidates.push_back(tmp + L"\\GRWVR");

    std::wstring chosen;
    for (const std::wstring& c : candidates) {
        DWORD err = 0;
        const char* what = "";
        if (dir_is_usable(c, &err, &what)) { chosen = c; break; }
        g_dir_attempts.push_back({c, err, what});
    }

    if (chosen.empty()) {
        // Nothing took a write. Keep the INSTALLED path rather than an empty
        // string: every consumer does data_dir() + L"\\something", and an empty
        // data dir turns all of those into writes to the root of the current
        // drive. Their writes will fail here, which is correct and contained.
        g_no_writable_dir = true;
        g_data_dir = installed;
    } else {
        g_data_dir = chosen;
        if (chosen != installed) {
            g_fallback_used = true;
            seed_fallback(installed, chosen);
        }
    }
    g_dir_state.store(2, std::memory_order_release);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

const std::wstring& game_dir() { resolve_data_dir_once(); return g_game_dir; }
const std::wstring& data_dir() { resolve_data_dir_once(); return g_data_dir; }
const std::string& log_path() { return g_log_path; }
bool data_dir_is_fallback() { return g_fallback_used; }

void init(HMODULE self) {
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = true; }

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dll_path = path;

    // May already have run, if a factory stub asked for the cfg before this
    // thread got here. Either way it happens exactly once and g_game_dir and
    // g_data_dir are published by it.
    resolve_data_dir_once();

    if (g_no_writable_dir) {
        // Nothing on this machine took a write. There is no log to say so in,
        // so say it where a tester can still be walked to it with DebugView.
        OutputDebugStringA("GRWXR: no writable data directory (the game folder, "
                           "LOCALAPPDATA and TEMP all refused a write test). "
                           "No log will be written this run.\n");
        return;
    }

    // Open with _SH_DENYWR so the log can be READ while the game is running.
    // Plain fopen denies all sharing, which makes the log unreadable until the
    // game exits. That is useless for live debugging.
    //
    // GRW.exe can appear more than once (the launcher path re-spawns it), so if
    // the primary name is already held by another instance, fall back to a
    // pid-suffixed file rather than silently losing the log.
    //
    // Rotate, do not truncate. Mode "w" destroys the previous session's log, and
    // that is precisely the file we need after a crash: a tester who crashes and
    // immediately relaunches to see whether it repeats has erased the only
    // evidence before anyone could collect it. Keep one generation back.
    //
    // If the rename fails because a live instance still holds grwxr.log open,
    // the _wfsopen below fails for the same reason and we fall through to the
    // pid-suffixed name exactly as before. Rotation adds no new failure path.
    std::wstring log_path_local = g_data_dir + L"\\grwxr.log";
    if (GetFileAttributesW(log_path_local.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring prev_path = g_data_dir + L"\\grwxr-prev.log";
        MoveFileExW(log_path_local.c_str(), prev_path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_file = _wfsopen(log_path_local.c_str(), L"w", _SH_DENYWR);
    if (!g_file) {
        wchar_t alt[MAX_PATH];
        swprintf_s(alt, L"%s\\grwxr-%lu.log", g_data_dir.c_str(), GetCurrentProcessId());
        log_path_local = alt;
        g_file = _wfsopen(log_path_local.c_str(), L"w", _SH_DENYWR);
    }
    if (!g_file) return;
    g_log_path = narrow(log_path_local);

    // --- CLAUDE.md rule 5: identity banner must be the first line. ---
    std::string hash = sha256_file(dll_path);

    FILETIME ft{};
    SYSTEMTIME st{};
    char stamp[64] = "unknown";
    HANDLE h = CreateFileW(dll_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        if (GetFileTime(h, nullptr, nullptr, &ft)) {
            FILETIME lft{};
            FileTimeToLocalFileTime(&ft, &lft);
            FileTimeToSystemTime(&lft, &st);
            snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        }
        CloseHandle(h);
    }

    fprintf(g_file, "GRWXR dll=%s sha256=%s mtime=%s\n",
            narrow(dll_path).c_str(),
            hash.empty() ? "(hash failed)" : hash.c_str(),
            stamp);
    fflush(g_file);

    // Host process identity, so a log can never be misattributed.
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    write("INFO ", "host process : %s", narrow(exe).c_str());
    write("INFO ", "pid          : %lu", GetCurrentProcessId());
    write("INFO ", "log file     : %s", narrow(log_path_local).c_str());

    // Module base matters: docs/TARGET-INVENTORY.md says ASLR is OFF and the
    // preferred base is 0x140000000. If the runtime base differs, every
    // absolute address in our notes is wrong and we need to know immediately.
    HMODULE base = GetModuleHandleW(nullptr);
    write("INFO ", "module base  : 0x%p  %s", (void*)base,
          ((uintptr_t)base == 0x140000000ull)
              ? "(matches the no-ASLR preferred base, as expected)"
              : "*** NOT the preferred base 0x140000000: ASLR is in play ***");
    for (const DirAttempt& a : g_dir_attempts) {
        write("WARN ", "data dir     : %s REFUSED (%s failed, error %lu)",
              narrow(a.path).c_str(), a.what, a.err);
    }
    write("INFO ", "data dir     : %s", narrow(g_data_dir).c_str());
    if (g_fallback_used) {
        write("WARN ", "data dir     : *** THE GAME FOLDER IS NOT WRITABLE. The "
                       "log, grwxr.cfg and presets are in the directory above, "
                       "NOT next to the game. Send THAT log, and edit THAT "
                       "grwxr.cfg: the one beside the game is no longer read. ***");
        switch (g_seed_cfg) {
        case kSeedCopied:
            write("INFO ", "data dir     : your installed grwxr.cfg was copied "
                           "across, with %d preset(s).", g_seed_presets);
            break;
        case kSeedKeptExisting:
            write("INFO ", "data dir     : a grwxr.cfg was already here from an "
                           "earlier run; that one is live and was left alone.");
            if (g_seed_stale)
                write("WARN ", "data dir     : and it is OLDER than the "
                               "grwxr.cfg beside the game, so an update's new "
                               "keys are not in it. Delete the one above to "
                               "take the installed copy again.");
            break;
        case kSeedNoSource:
            write("INFO ", "data dir     : no grwxr.cfg beside the game to copy; "
                           "built-in defaults are in use.");
            break;
        case kSeedFailed:
            write("WARN ", "data dir     : could NOT copy your installed "
                           "grwxr.cfg (error %lu). The mod is running on "
                           "BUILT-IN DEFAULTS. Any tuning you did beside the "
                           "game is not loaded.", g_seed_cfg_err);
            break;
        }
        if (g_seed_presets_err)
            write("WARN ", "data dir     : some presets did not copy (error %lu).",
                  g_seed_presets_err);
    }
}

void shutdown() {
    if (g_lock_ready) EnterCriticalSection(&g_lock);
    if (g_file) { fflush(g_file); fclose(g_file); g_file = nullptr; }
    if (g_lock_ready) LeaveCriticalSection(&g_lock);
}

// BUILD 227: THE QUIET LOG. The instruments print once a second each and a
// five minute run is 8,000 lines, most of them the same forty prefixes. A
// tester's report needs the startup block, the hook results, every WARN and
// ERROR, the feature lines and the heartbeat; it does not need the arm
// solver's per-second telemetry. With log_diag = 0 an INFO line whose text
// starts with one of these is not written. The table is the prefixes counted
// in the 2026-09-09 baseline run (grwxr-12348.log), by volume.
static volatile long g_diag = 0;

void set_diag(int on) { g_diag = on ? 1 : 0; }
int  diag() { return (int)g_diag; }

static bool is_instrument(const char* m) {
    static const char* const kPrefix[] = {
        "mount", "ohand", "wskel:", "stance:", "fovscene:", "roll", "lead:",
        "fovsrc:", "recenter:", "hpos", "fovroot:", "follow:", "perf:", "ffr:",
        "fov:", "input:", "aimref:", "rtcensus", "twoagree", "twistprobe",
        "survive:", "reachwrist:", "pole", "armanchor:", "animsurv", "rayprobe",
        "wnode:", "fp: ANCHOR", "fp: HEAD", "fp: pin(", "fp: diag", "wpose:",
        "aimpoint:", "aer", "wgun: ARMED", "aim: yaw", "hide: FORCING",
        "hide: idle", "grip:", "skel:", "wp:", "xin:", "=== ", "  probe",
        "settings:", "0:", "0.", "wpm", "wgun: census", "wskel census",
    };
    if (!m[0]) return true;                       // the blank spacer lines
    if (m[0] == ' ') {                            // table rows: spaces then a digit
        const char* p = m;
        while (*p == ' ') ++p;
        if (*p >= '0' && *p <= '9') return true;
    }
    for (const char* pre : kPrefix) {
        const char* a = m;
        const char* b = pre;
        while (*b && *a == *b) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

void write(const char* level, const char* fmt, ...) {
    if (!g_file || !g_lock_ready) return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (!g_diag && level[0] == 'I' && is_instrument(msg)) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    EnterCriticalSection(&g_lock);
    const int wrote = fprintf(g_file, "[%02u:%02u:%02u.%03u] [%lu] %s %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId(), level, msg);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
    // BUILD 215: the log's own volume is one of the accumulation candidates
    // (runs write up to 3.8 MB; every line is a flush under a lock).
    perf::g_log_lines.fetch_add(1, std::memory_order_relaxed);
    if (wrote > 0) perf::g_log_bytes.fetch_add((uint64_t)wrote, std::memory_order_relaxed);
}

}  // namespace log
}  // namespace grwxr
