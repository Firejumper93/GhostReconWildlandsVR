// cfg_gui.cpp: standalone Win32 settings editor for the GRW-XR mod config
// (GRWVR\grwxr.cfg). Single translation unit, Win32 API only, Unicode.
//
// The mod hot-reloads grwxr.cfg about once per second, so this tool saves the
// file 300 ms after any control change: moving a slider IS the live tuning.
// Writes preserve every comment and unknown line in the file: only the lines
// whose keys the user actually changed are rewritten, and keys that were not
// present are appended at the end under a "# added by cfg_gui" marker.
// Writes are atomic (temp file in the same directory, then MoveFileExW with
// replace).
//
// Key list, clamp ranges, and defaults mirror load_config() in
// src\VRMirror.cpp and the defaults in src\HeadPose.cpp. If a key is added
// to the mod, add it here too.
//
// Build: tools\cfg_gui\build.bat

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------
// Model

struct SliderDef {
    const wchar_t* key;     // cfg key
    const wchar_t* label;
    double mn, mx, step, def;
    int dec;                // decimals shown and written
    bool zeroOff;           // show "off" when the value is 0
    // runtime
    HWND hLabel = nullptr, hTrack = nullptr, hVal = nullptr;
    double val = 0.0;
    bool dirty = false;
};

// Order matters only for the enum below; layout is explicit.
static SliderDef g_sl[] = {
    // key                  label                                  mn      mx     step   def     dec zeroOff
    { L"ipd_scale",         L"IPD scale",                          -2.00,  2.00,  0.05,  1.00,   2, false },
    { L"fullscreen_fov",    L"Fullscreen fov (rad)",                0.60,  2.50,  0.02,  1.92,   2, false },
    { L"desktop_fov",       L"Desktop recording view crop (rad)",   0.00,  2.00,  0.02,  0.90,   2, true  },
    { L"fp_head_eye",       L"Head joint to eye (m)",              -0.50,  1.00,  0.02,  0.10,   2, false },
    { L"fp_anchor_side",    L"Anchor side centering (m)",          -1.00,  1.00,  0.05,  0.00,   2, false },
    { L"fp_eye",            L"Eye height, origin anchor (m)",       0.00,  2.50,  0.05,  0.85,   2, false },
    { L"fp_forward",        L"Forward push, fallback (m)",          0.00,  4.00,  0.05,  2.20,   2, false },
    { L"fp_side",           L"Side offset, fallback (m)",          -2.00,  2.00,  0.05, -0.40,   2, false },
    { L"fp_up",             L"Up offset, fallback (m)",            -2.00,  2.00,  0.05,  0.00,   2, false },
    { L"mono_scope_fov",    L"Mono scope threshold (rad)",          0.00,  1.50,  0.01,  0.30,   2, true  },
    { L"scope_display_fov", L"Scope display window (rad)",          0.00,  1.20,  0.01,  0.52,   2, true  },
    { L"aim_deadzone_deg",  L"Deadzone (deg)",                      0.00, 30.00,  0.50,  2.00,   1, false },
    { L"aim_gain",          L"Gain ((deg/s) per deg)",              0.00, 50.00,  0.50,  8.00,   1, false },
    { L"aim_max_rate",      L"Max turn rate (deg/s)",               0.00, 720.0,  5.00,  180.0,  0, false },
    { L"aim_mouse_per_deg", L"Mouse counts per degree",             0.00, 200.0,  1.00,  10.0,   0, false },
};
enum {
    SL_IPD, SL_FSFOV, SL_DESK, SL_HEADEYE, SL_ASIDE, SL_EYE, SL_FWD,
    SL_SIDE, SL_UP, SL_MONO, SL_SCOPE, SL_DEAD, SL_GAIN, SL_RATE, SL_MPD,
    SL_COUNT
};

struct CheckDef {
    const wchar_t* key;     // nullptr: no key of its own (the fullscreen enable)
    const wchar_t* label;
    bool defOn;
    int onVal, offVal;      // written as integers
    // runtime
    HWND h = nullptr;
    bool on = false;
    bool dirty = false;
};

static CheckDef g_ck[] = {
    { nullptr,           L"Fullscreen view (off writes fullscreen_fov = 0)", true,   0,  0 },
    { L"fp_head_anchor", L"Anchor to head bone",                             true,   1,  0 },
    { L"aim_steer",      L"Controller aim steer (hold right trigger)",       true,   1,  0 },
    { L"aim_ads",        L"Trigger half pull aims (ADS)",                    true,   1,  0 },
    { L"aim_fire",       L"Trigger full pull fires",                         true,   1,  0 },
    { L"aim_yaw_sign",   L"Invert yaw direction (writes -1)",                false, -1,  1 },
    { L"aim_pitch_sign", L"Invert pitch direction (writes -1)",              false, -1,  1 },
};
enum { CK_FS, CK_HEAD, CK_STEER, CK_ADS, CK_FIRE, CK_YAW, CK_PITCH, CK_COUNT };

static const wchar_t* AIM_NOTE =
    L"Current game build ships yaw -1 (inverted) and pitch +1. A wrong pitch "
    L"sign drives the engine into its pitch clamp and pins the camera near "
    L"vertical: leave pitch unchecked unless calibration says otherwise.";

// ---------------------------------------------------------------------------
// Globals

static HINSTANCE g_hInst;
static HWND g_hWnd, g_hStatus, g_hReload, g_hNote;
static HWND g_hGroup[5];
static HFONT g_hFont;
static std::wstring g_cfgPath;
static std::wstring g_iniPath;
static bool g_timerLive = false;
static wchar_t g_savedAt[32] = L"not saved yet";

static const wchar_t* DEFAULT_CFG =
    L"C:\\Steam\\steamapps\\common\\Wildlands\\GRWVR\\grwxr.cfg";

enum { IDT_SAVE = 1, IDC_RELOAD = 3000, IDC_CHECK0 = 2000 };

// Layout metrics (system DPI is handled by SetProcessDPIAware; values are
// plain pixels at 96 dpi).
enum {
    MARGIN = 10, GROUP_PAD = 10, GROUP_TOP = 20, ROW_H = 30,
    LABEL_W = 180, VAL_W = 56, CHK_H = 22, NOTE_H = 64, BTN_H = 28
};

// ---------------------------------------------------------------------------
// Small helpers

static int SliderTicks(const SliderDef& s) {
    return (int)lround((s.mx - s.mn) / s.step);
}

static double SnapValue(const SliderDef& s, double v) {
    int pos = (int)lround((v - s.mn) / s.step);
    if (pos < 0) pos = 0;
    int n = SliderTicks(s);
    if (pos > n) pos = n;
    return s.mn + pos * s.step;
}

static void FormatValue(const SliderDef& s, wchar_t* buf, size_t n) {
    if (s.zeroOff && fabs(s.val) < 1e-9)
        swprintf(buf, n, L"off");
    else
        swprintf(buf, n, L"%.*f", s.dec, s.val);
}

static void UpdateValLabel(int i) {
    wchar_t buf[32];
    FormatValue(g_sl[i], buf, 32);
    SetWindowTextW(g_sl[i].hVal, buf);
}

static void SetSliderValue(int i, double v) {
    SliderDef& s = g_sl[i];
    s.val = SnapValue(s, v);
    int pos = (int)lround((s.val - s.mn) / s.step);
    SendMessageW(s.hTrack, TBM_SETPOS, TRUE, pos);
    UpdateValLabel(i);
}

static void SetCheckValue(int i, bool on) {
    g_ck[i].on = on;
    SendMessageW(g_ck[i].h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void UpdateStatus() {
    wchar_t right[64];
    swprintf(right, 64, L"%s", g_savedAt);
    SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)g_cfgPath.c_str());
    SendMessageW(g_hStatus, SB_SETTEXTW, 1, (LPARAM)right);
}

// ---------------------------------------------------------------------------
// Cfg file I/O

static bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.clear();
    if (sz.QuadPart > 0 && sz.QuadPart < 4 * 1024 * 1024) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!::ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr)) {
            CloseHandle(h);
            return false;
        }
        out.resize(got);
    }
    CloseHandle(h);
    return true;
}

// Split into lines, dropping the line terminators. crlf reports the file's
// dominant terminator so the rewrite keeps it.
static void SplitLines(const std::string& text, std::vector<std::string>& lines,
                       bool& crlf, bool& endedWithNewline) {
    crlf = text.find("\r\n") != std::string::npos;
    endedWithNewline = !text.empty() && text.back() == '\n';
    lines.clear();
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    if (!cur.empty()) lines.push_back(cur);
}

// Extract "key" from a "key = value" cfg line. Empty result: comment, blank,
// or no '='.
static std::string LineKey(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= line.size() || line[i] == '#') return std::string();
    size_t eq = line.find('=', i);
    if (eq == std::string::npos) return std::string();
    size_t end = eq;
    while (end > i && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
    return line.substr(i, end - i);
}

static std::string LineValue(const std::string& line) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return std::string();
    size_t i = eq + 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t end = line.size();
    while (end > i && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                       line[end - 1] == '\r')) end--;
    return line.substr(i, end - i);
}

static std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((char)c);   // keys and values are ASCII
    return s;
}

// Reset every control to the mod defaults, then overlay whatever the file
// defines. Clears all dirty flags.
static void LoadIntoControls() {
    for (int i = 0; i < SL_COUNT; i++) {
        g_sl[i].dirty = false;
        SetSliderValue(i, g_sl[i].def);
    }
    for (int i = 0; i < CK_COUNT; i++) {
        g_ck[i].dirty = false;
        SetCheckValue(i, g_ck[i].defOn);
    }

    std::string text;
    bool haveFile = ReadFileBytes(g_cfgPath, text);
    if (haveFile) {
        std::vector<std::string> lines;
        bool crlf, nl;
        SplitLines(text, lines, crlf, nl);
        for (const std::string& line : lines) {
            std::string key = LineKey(line);
            if (key.empty()) continue;
            double v = atof(LineValue(line).c_str());
            if (key == "fullscreen_fov") {
                // 0 disables the override; the slider keeps its last value so
                // re-checking the box restores it.
                SetCheckValue(CK_FS, v > 0.0);
                if (v > 0.0) SetSliderValue(SL_FSFOV, v);
                continue;
            }
            bool matched = false;
            for (int i = 0; i < SL_COUNT && !matched; i++) {
                if (i != SL_FSFOV && key == Narrow(g_sl[i].key)) {
                    SetSliderValue(i, v);
                    matched = true;
                }
            }
            for (int i = 0; i < CK_COUNT && !matched; i++) {
                if (g_ck[i].key && key == Narrow(g_ck[i].key)) {
                    // Sign keys: checked means -1. Boolean keys: nonzero is on.
                    bool on = (g_ck[i].onVal == -1) ? (v < 0.0) : (v != 0.0);
                    SetCheckValue(i, on);
                    matched = true;
                }
            }
            // Unknown keys (upsize_width etc.) are left alone.
        }
    }
    EnableWindow(g_sl[SL_FSFOV].hTrack, g_ck[CK_FS].on);
    if (!haveFile)
        swprintf(g_savedAt, 32, L"file not found, will create");
    UpdateStatus();
}

struct PendingWrite { std::string key, value; };

// Collect the keys the user has touched, as key/value strings ready to write.
static void CollectDirty(std::vector<PendingWrite>& out) {
    out.clear();
    char buf[64];
    for (int i = 0; i < SL_COUNT; i++) {
        if (i == SL_FSFOV) continue;
        if (!g_sl[i].dirty) continue;
        snprintf(buf, sizeof(buf), "%.*f", g_sl[i].dec, g_sl[i].val);
        out.push_back({ Narrow(g_sl[i].key), buf });
    }
    if (g_sl[SL_FSFOV].dirty || g_ck[CK_FS].dirty) {
        if (g_ck[CK_FS].on)
            snprintf(buf, sizeof(buf), "%.*f", g_sl[SL_FSFOV].dec, g_sl[SL_FSFOV].val);
        else
            snprintf(buf, sizeof(buf), "0");
        out.push_back({ "fullscreen_fov", buf });
    }
    for (int i = 0; i < CK_COUNT; i++) {
        if (i == CK_FS || !g_ck[i].dirty || !g_ck[i].key) continue;
        snprintf(buf, sizeof(buf), "%d", g_ck[i].on ? g_ck[i].onVal : g_ck[i].offVal);
        out.push_back({ Narrow(g_ck[i].key), buf });
    }
}

// Rewrite the cfg: replace the value on every line whose key is pending,
// append the rest under a marker comment, write atomically.
static bool SaveCfg() {
    std::vector<PendingWrite> pending;
    CollectDirty(pending);
    if (pending.empty()) return true;

    std::string text;
    ReadFileBytes(g_cfgPath, text);     // missing file: start from empty
    std::vector<std::string> lines;
    bool crlf = false, endNl = true;
    SplitLines(text, lines, crlf, endNl);
    if (text.empty()) crlf = false;

    std::vector<bool> found(pending.size(), false);
    for (std::string& line : lines) {
        std::string key = LineKey(line);
        if (key.empty()) continue;
        for (size_t p = 0; p < pending.size(); p++) {
            if (key == pending[p].key) {
                line = pending[p].key + " = " + pending[p].value;
                found[p] = true;
            }
        }
    }
    bool anyNew = false;
    for (size_t p = 0; p < pending.size(); p++) {
        if (found[p]) continue;
        if (!anyNew) {
            if (!lines.empty() && !lines.back().empty()) lines.push_back("");
            lines.push_back("# added by cfg_gui");
            anyNew = true;
        }
        lines.push_back(pending[p].key + " = " + pending[p].value);
    }

    const char* eol = crlf ? "\r\n" : "\n";
    std::string out;
    out.reserve(text.size() + 256);
    for (const std::string& line : lines) { out += line; out += eol; }

    std::wstring tmp = g_cfgPath + L".cfg_gui.tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &wrote, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || wrote != out.size()) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), g_cfgPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

static void SaveNow() {
    if (g_timerLive) { KillTimer(g_hWnd, IDT_SAVE); g_timerLive = false; }
    if (SaveCfg()) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        swprintf(g_savedAt, 32, L"saved %02u:%02u:%02u",
                 st.wHour, st.wMinute, st.wSecond);
    } else {
        swprintf(g_savedAt, 32, L"SAVE FAILED");
    }
    UpdateStatus();
}

static void ScheduleSave() {
    SetTimer(g_hWnd, IDT_SAVE, 300, nullptr);
    g_timerLive = true;
}

// ---------------------------------------------------------------------------
// Path resolution (cfg_gui.ini next to the exe remembers a chosen path)

static void ResolveIniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    size_t slash = p.find_last_of(L'\\');
    if (slash != std::wstring::npos) p.resize(slash);
    g_iniPath = p + L"\\cfg_gui.ini";
}

static bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void ResolveCfgPath() {
    ResolveIniPath();
    wchar_t saved[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"cfg_gui", L"path", L"", saved, MAX_PATH,
                             g_iniPath.c_str());
    if (saved[0] && FileExists(saved)) { g_cfgPath = saved; return; }
    if (FileExists(DEFAULT_CFG)) { g_cfgPath = DEFAULT_CFG; return; }

    // Neither the saved path nor the default exists: ask.
    wchar_t file[MAX_PATH] = L"grwxr.cfg";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Config files (*.cfg)\0*.cfg\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Locate grwxr.cfg";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        g_cfgPath = file;
        WritePrivateProfileStringW(L"cfg_gui", L"path", file, g_iniPath.c_str());
    } else {
        // No file chosen: fall back to the default path; the first save
        // creates it.
        g_cfgPath = DEFAULT_CFG;
    }
}

// ---------------------------------------------------------------------------
// UI construction and layout

static HWND MakeCtl(const wchar_t* cls, const wchar_t* text, DWORD style,
                    DWORD exStyle, HMENU id) {
    HWND h = CreateWindowExW(exStyle, cls, text,
                             WS_CHILD | WS_VISIBLE | style,
                             0, 0, 10, 10, g_hWnd, id, g_hInst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return h;
}

static void CreateControls() {
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);

    static const wchar_t* titles[5] = {
        L"Depth and view", L"First person", L"Scopes",
        L"Motion controls", L"Head aim calibration"
    };
    for (int i = 0; i < 5; i++)
        g_hGroup[i] = MakeCtl(L"BUTTON", titles[i],
                              BS_GROUPBOX | WS_CLIPSIBLINGS, 0, nullptr);

    for (int i = 0; i < SL_COUNT; i++) {
        SliderDef& s = g_sl[i];
        s.hLabel = MakeCtl(L"STATIC", s.label, 0, 0, nullptr);
        s.hTrack = MakeCtl(TRACKBAR_CLASSW, L"",
                           TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 0, nullptr);
        s.hVal = MakeCtl(L"STATIC", L"", SS_RIGHT, 0, nullptr);
        SendMessageW(s.hTrack, TBM_SETRANGE, FALSE,
                     MAKELPARAM(0, SliderTicks(s)));
        SendMessageW(s.hTrack, TBM_SETPAGESIZE, 0, 4);
    }
    for (int i = 0; i < CK_COUNT; i++)
        g_ck[i].h = MakeCtl(L"BUTTON", g_ck[i].label,
                            BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                            (HMENU)(INT_PTR)(IDC_CHECK0 + i));
    g_hNote = MakeCtl(L"STATIC", AIM_NOTE, 0, 0, nullptr);
    g_hReload = MakeCtl(L"BUTTON", L"Reload from file",
                        BS_PUSHBUTTON | WS_TABSTOP, 0,
                        (HMENU)(INT_PTR)IDC_RELOAD);
    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                0, 0, 0, 0, g_hWnd, nullptr, g_hInst, nullptr);
    SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);
}

static int PlaceSlider(int i, int x, int y, int w) {
    SliderDef& s = g_sl[i];
    int trackW = w - LABEL_W - VAL_W - 8;
    MoveWindow(s.hLabel, x, y + 5, LABEL_W, 18, TRUE);
    MoveWindow(s.hTrack, x + LABEL_W, y, trackW, ROW_H - 4, TRUE);
    MoveWindow(s.hVal, x + w - VAL_W, y + 5, VAL_W, 18, TRUE);
    return y + ROW_H;
}

static int PlaceCheck(int i, int x, int y, int w) {
    MoveWindow(g_ck[i].h, x, y + 2, w, CHK_H, TRUE);
    return y + ROW_H - 4;
}

static void DoLayout(int cw, int ch) {
    RECT sb;
    SendMessageW(g_hStatus, WM_SIZE, 0, 0);
    GetWindowRect(g_hStatus, &sb);
    int sbH = sb.bottom - sb.top;
    int parts[2] = { cw - 130, -1 };
    SendMessageW(g_hStatus, SB_SETPARTS, 2, (LPARAM)parts);

    int colW = (cw - 3 * MARGIN) / 2;
    int innerW = colW - 2 * GROUP_PAD;

    // Column A
    int xa = MARGIN, ia = xa + GROUP_PAD;
    int y = MARGIN, top;

    top = y; y += GROUP_TOP;
    y = PlaceSlider(SL_IPD, ia, y, innerW);
    y = PlaceCheck(CK_FS, ia, y, innerW);
    y = PlaceSlider(SL_FSFOV, ia, y, innerW);
    y = PlaceSlider(SL_DESK, ia, y, innerW);
    MoveWindow(g_hGroup[0], xa, top, colW, y - top + GROUP_PAD, TRUE);
    y += GROUP_PAD + MARGIN;

    top = y; y += GROUP_TOP;
    y = PlaceCheck(CK_HEAD, ia, y, innerW);
    y = PlaceSlider(SL_HEADEYE, ia, y, innerW);
    y = PlaceSlider(SL_ASIDE, ia, y, innerW);
    y = PlaceSlider(SL_EYE, ia, y, innerW);
    y = PlaceSlider(SL_FWD, ia, y, innerW);
    y = PlaceSlider(SL_SIDE, ia, y, innerW);
    y = PlaceSlider(SL_UP, ia, y, innerW);
    MoveWindow(g_hGroup[1], xa, top, colW, y - top + GROUP_PAD, TRUE);
    y += GROUP_PAD + MARGIN;

    top = y; y += GROUP_TOP;
    y = PlaceSlider(SL_MONO, ia, y, innerW);
    y = PlaceSlider(SL_SCOPE, ia, y, innerW);
    MoveWindow(g_hGroup[2], xa, top, colW, y - top + GROUP_PAD, TRUE);

    // Column B
    int xb = 2 * MARGIN + colW, ib = xb + GROUP_PAD;
    y = MARGIN;

    top = y; y += GROUP_TOP;
    y = PlaceCheck(CK_STEER, ib, y, innerW);
    y = PlaceCheck(CK_ADS, ib, y, innerW);
    y = PlaceCheck(CK_FIRE, ib, y, innerW);
    y = PlaceSlider(SL_DEAD, ib, y, innerW);
    y = PlaceSlider(SL_GAIN, ib, y, innerW);
    y = PlaceSlider(SL_RATE, ib, y, innerW);
    y = PlaceSlider(SL_MPD, ib, y, innerW);
    MoveWindow(g_hGroup[3], xb, top, colW, y - top + GROUP_PAD, TRUE);
    y += GROUP_PAD + MARGIN;

    top = y; y += GROUP_TOP;
    y = PlaceCheck(CK_YAW, ib, y, innerW);
    y = PlaceCheck(CK_PITCH, ib, y, innerW);
    MoveWindow(g_hNote, ib, y + 2, innerW, NOTE_H, TRUE);
    y += NOTE_H + 6;
    MoveWindow(g_hGroup[4], xb, top, colW, y - top + GROUP_PAD, TRUE);
    y += GROUP_PAD + MARGIN;

    MoveWindow(g_hReload, xb, y, 140, BTN_H, TRUE);

    (void)ch; (void)sbH;
}

// ---------------------------------------------------------------------------
// Window proc

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hWnd = hWnd;
        CreateControls();
        LoadIntoControls();
        return 0;

    case WM_SIZE:
        DoLayout(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 780;
        mmi->ptMinTrackSize.y = 560;
        return 0;
    }

    case WM_HSCROLL: {
        HWND h = (HWND)lp;
        for (int i = 0; i < SL_COUNT; i++) {
            if (g_sl[i].hTrack != h) continue;
            int pos = (int)SendMessageW(h, TBM_GETPOS, 0, 0);
            double v = g_sl[i].mn + pos * g_sl[i].step;
            if (fabs(v - g_sl[i].val) > 1e-9) {
                g_sl[i].val = v;
                g_sl[i].dirty = true;
                UpdateValLabel(i);
                ScheduleSave();
            }
            break;
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_RELOAD && HIWORD(wp) == BN_CLICKED) {
            if (g_timerLive) { KillTimer(hWnd, IDT_SAVE); g_timerLive = false; }
            LoadIntoControls();
            swprintf(g_savedAt, 32, L"reloaded");
            UpdateStatus();
            return 0;
        }
        if (id >= IDC_CHECK0 && id < IDC_CHECK0 + CK_COUNT &&
            HIWORD(wp) == BN_CLICKED) {
            int i = id - IDC_CHECK0;
            g_ck[i].on = SendMessageW(g_ck[i].h, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_ck[i].dirty = true;
            if (i == CK_FS)
                EnableWindow(g_sl[SL_FSFOV].hTrack, g_ck[CK_FS].on);
            ScheduleSave();
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wp == IDT_SAVE) { SaveNow(); return 0; }
        break;

    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_CLOSE:
        if (g_timerLive) SaveNow();     // flush a pending debounce
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    g_hInst = hInst;
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc{ sizeof(icc),
                              ICC_WIN95_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    ResolveCfgPath();

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"GRWVRCfgGui";
    RegisterClassW(&wc);

    RECT r{ 0, 0, 880, 590 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hWnd = CreateWindowExW(0, wc.lpszClassName, L"GRW-VR Settings",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return 1;
    ShowWindow(hWnd, nShow);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hFont) DeleteObject(g_hFont);
    return 0;
}
