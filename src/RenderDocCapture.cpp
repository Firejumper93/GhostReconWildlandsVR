// RenderDocCapture.cpp - see RenderDocCapture.h for why this exists.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>

#include "RenderDocCapture.h"
#include "Log.h"
#include "renderdoc_app.h"

namespace grwxr {
namespace rdoc {
namespace {

RENDERDOC_API_1_4_1* g_api = nullptr;
HMODULE g_dll = nullptr;
bool g_on = false;
bool g_read = false;
unsigned g_last_count = 0;

// Where to look for renderdoc.dll. The cfg key wins; this is the default the
// stock Windows installer uses.
std::wstring g_dll_path = L"C:\\Program Files\\RenderDoc\\renderdoc.dll";

// Deferred log line, same pattern as VRMirror: poll() fills it, drain() emits.
// Single slot, so a burst overwrites; that is fine for a 1 Hz hotkey.
char g_deferred[1024] = {};
bool g_want_log = false;

void note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_deferred, sizeof(g_deferred), fmt, ap);
    va_end(ap);
    g_want_log = true;
}

// Read only the two keys this module owns, independently of VRMirror's
// load_config(). That runs at VR init, which is far too late: renderdoc.dll
// has to be loaded before the game creates its device.
void read_cfg() {
    if (g_read) return;
    g_read = true;

    const std::wstring path = log::data_dir() + L"\\grwxr.cfg";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || !f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        int v = 0;
        if (sscanf_s(line, " renderdoc_capture = %d", &v) == 1) g_on = (v != 0);

        char buf[MAX_PATH] = {};
        if (sscanf_s(line, " renderdoc_dll = %259[^\r\n]", buf, (unsigned)sizeof(buf)) == 1) {
            // Trim trailing blanks so a stray space in the cfg is harmless.
            size_t n = strlen(buf);
            while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = '\0';
            if (n > 0) {
                wchar_t w[MAX_PATH] = {};
                MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, MAX_PATH);
                g_dll_path = w;
            }
        }
    }
    fclose(f);
}

}  // namespace

bool enabled() {
    read_cfg();
    return g_on;
}

bool install() {
    if (!enabled()) return false;

    char p[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_dll_path.c_str(), -1, p, sizeof(p), nullptr, nullptr);

    // Already present? Then something else got here first and we must not
    // load a second copy.
    g_dll = GetModuleHandleW(L"renderdoc.dll");
    if (g_dll) {
        LOG_INFO("rdoc: renderdoc.dll already loaded at 0x%p, reusing it", (void*)g_dll);
    } else {
        g_dll = LoadLibraryW(g_dll_path.c_str());
        if (!g_dll) {
            // Second chance on the bare name, in case it is on PATH.
            g_dll = LoadLibraryW(L"renderdoc.dll");
        }
        if (!g_dll) {
            LOG_ERROR("rdoc: could NOT load renderdoc.dll (tried '%s', err %lu). "
                      "Set renderdoc_dll= in grwxr.cfg to the right path. "
                      "Game continues unmodified.", p, GetLastError());
            return false;
        }
        LOG_INFO("rdoc: loaded renderdoc.dll from %s", p);
    }

    auto get_api = (pRENDERDOC_GetAPI)GetProcAddress(g_dll, "RENDERDOC_GetAPI");
    if (!get_api) {
        LOG_ERROR("rdoc: RENDERDOC_GetAPI not exported. Not a RenderDoc DLL?");
        g_dll = nullptr;
        return false;
    }

    if (get_api(eRENDERDOC_API_Version_1_4_1, (void**)&g_api) != 1 || !g_api) {
        LOG_ERROR("rdoc: RENDERDOC_GetAPI(1.4.1) refused. Installed RenderDoc is "
                  "older than the API we ask for.");
        g_api = nullptr;
        return false;
    }

    int maj = 0, min = 0, pat = 0;
    g_api->GetAPIVersion(&maj, &min, &pat);

    // Put captures somewhere obvious rather than %TEMP%, so they are easy to
    // find and easy to tell me about. RenderDoc appends _frameN.rdc itself,
    // but it will NOT create the directory, so we do.
    const std::wstring dir = log::data_dir() + L"\\capture";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring tmpl = dir + L"\\grwxr";
    char t[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, tmpl.c_str(), -1, t, sizeof(t), nullptr, nullptr);
    g_api->SetCaptureFilePathTemplate(t);

    g_last_count = g_api->GetNumCaptures();

    LOG_INFO("rdoc: In-Application API %d.%d.%d bound. Captures go to %s_frameN.rdc",
             maj, min, pat, t);
    LOG_INFO("rdoc: press F12 (RenderDoc's own key) or NUMPAD MINUS (ours) to "
             "capture a frame. VR is STOOD DOWN while renderdoc_capture=1.");
    return true;
}

void poll() {
    if (!g_api) return;

    // Our own capture key, edge triggered. Numpad Minus is free (build 21
    // removed every tuning key). RenderDoc's own F12 also works; this exists
    // because F12 is Steam's screenshot key by default and may be eaten.
    //
    // NOTE this poll runs at 1 Hz with the rest of the drain loop, so a quick
    // tap can be missed. HOLD the key for about a second. Same behaviour as
    // the Home / Numpad 8 / Numpad Decimal play toggles.
    {
        static bool was_down = false;
        const bool down = (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
        if (down && !was_down) {
            g_api->TriggerCapture();
            note("rdoc: capture triggered, next presented frame will be saved");
        }
        was_down = down;
    }

    // Report anything that landed, so the log proves whether a capture
    // happened. The user should not have to remember seeing an overlay.
    const unsigned n = g_api->GetNumCaptures();
    if (n != g_last_count) {
        char path[MAX_PATH] = {};
        uint32_t len = (uint32_t)sizeof(path);
        uint64_t ts = 0;
        if (n > 0 && g_api->GetCapture(n - 1, path, &len, &ts)) {
            note("rdoc: CAPTURE %u SAVED -> %s", n, path);
        } else {
            note("rdoc: capture count is now %u", n);
        }
        g_last_count = n;
    }
}

void drain() {
    if (!g_want_log) return;
    g_want_log = false;
    LOG_INFO("%s", g_deferred);
}

}  // namespace rdoc
}  // namespace grwxr
