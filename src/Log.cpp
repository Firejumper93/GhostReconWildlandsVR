#include "Log.h"

#include <bcrypt.h>
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

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

const std::wstring& game_dir() { return g_game_dir; }
const std::wstring& data_dir() { return g_data_dir; }

void init(HMODULE self) {
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = true; }

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dll_path = path;

    size_t slash = dll_path.find_last_of(L"\\/");
    g_game_dir = (slash == std::wstring::npos) ? L"." : dll_path.substr(0, slash);
    g_data_dir = g_game_dir + L"\\GRWVR";
    CreateDirectoryW(g_data_dir.c_str(), nullptr);

    // Open with _SH_DENYWR so the log can be READ while the game is running.
    // Plain fopen denies all sharing, which makes the log unreadable until the
    // game exits. That is useless for live debugging.
    //
    // GRW.exe can appear more than once (the launcher path re-spawns it), so if
    // the primary name is already held by another instance, fall back to a
    // pid-suffixed file rather than silently losing the log.
    std::wstring log_path = g_data_dir + L"\\grwxr.log";
    g_file = _wfsopen(log_path.c_str(), L"w", _SH_DENYWR);
    if (!g_file) {
        wchar_t alt[MAX_PATH];
        swprintf_s(alt, L"%s\\grwxr-%lu.log", g_data_dir.c_str(), GetCurrentProcessId());
        log_path = alt;
        g_file = _wfsopen(log_path.c_str(), L"w", _SH_DENYWR);
    }
    if (!g_file) return;

    // --- project rule 5: identity banner must be the first line. ---
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
    write("INFO ", "log file     : %s", narrow(log_path).c_str());

    // Module base matters: docs/TARGET-INVENTORY.md says ASLR is OFF and the
    // preferred base is 0x140000000. If the runtime base differs, every
    // absolute address in our notes is wrong and we need to know immediately.
    HMODULE base = GetModuleHandleW(nullptr);
    write("INFO ", "module base  : 0x%p  %s", (void*)base,
          ((uintptr_t)base == 0x140000000ull)
              ? "(matches the no-ASLR preferred base, as expected)"
              : "*** NOT the preferred base 0x140000000: ASLR is in play ***");
    write("INFO ", "data dir     : %s", narrow(g_data_dir).c_str());
}

void shutdown() {
    if (g_lock_ready) EnterCriticalSection(&g_lock);
    if (g_file) { fflush(g_file); fclose(g_file); g_file = nullptr; }
    if (g_lock_ready) LeaveCriticalSection(&g_lock);
}

void write(const char* level, const char* fmt, ...) {
    if (!g_file || !g_lock_ready) return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    EnterCriticalSection(&g_lock);
    fprintf(g_file, "[%02u:%02u:%02u.%03u] [%lu] %s %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId(), level, msg);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

}  // namespace log
}  // namespace grwxr
