// Presets.cpp - see Presets.h for why this exists and why it only queues.

#include "Presets.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "Log.h"
#include "VRMirror.h"
#include "Voice.h"

namespace grwxr {
namespace presets {
namespace {

constexpr int kSlots      = 10;    // the numpad digit row, 0..9
constexpr int kMaxPresets = 200;
constexpr int kNameLen    = 96;

// The inventory the PANEL reads. Rule 8 forbids locks in Present, and the
// panel runs in Present, so the panel never touches the vectors below: the
// init thread builds an immutable Snapshot, publishes the pointer with a
// release store, and the panel acquire-loads it and reads. The old snapshot
// is deliberately never freed. A rescan is a manual act, each snapshot is
// ~19 KB, and a use-after-free in Present costs more than the memory does.
struct Snapshot {
    int  n = 0;
    char name[kMaxPresets][kNameLen] = {};
};
std::atomic<Snapshot*> g_snap{nullptr};

// Init-thread-only state. Nothing in Present may read these.
std::vector<std::wstring> g_files;   // file names, sorted, no directory part
std::vector<std::string>  g_names;   // the same, display form, utf8

std::atomic<int>  g_bank{0};
std::atomic<int>  g_req{-1};         // queued slot, -1 = nothing queued
std::atomic<bool> g_req_rescan{false};
std::atomic<bool> g_backed_up{false};

char g_current[kNameLen] = {};       // last loaded, utf8, for the panel

std::wstring dir_path() { return log::data_dir() + L"\\presets"; }
std::wstring live_cfg() { return log::data_dir() + L"\\grwxr.cfg"; }

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n,
                        nullptr, nullptr);
    return s;
}

// "03-wide-fov.cfg" -> "03 wide fov". The voice reads this, so separators
// become spaces: "zero three dash wide" is not a name anyone recognises.
std::string display_name(const std::wstring& file) {
    std::wstring s = file;
    const size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos) s.erase(dot);
    for (wchar_t& c : s)
        if (c == L'_' || c == L'-') c = L' ';
    return to_utf8(s);
}

// Parse a cfg into key -> value TEXT. Deliberately the same shape as
// load_config()'s reader (`#` comments, then `key = value`), but it keeps the
// value as text and never clamps: this is for reporting the diff, not for
// applying anything. load_config() stays the only thing that applies a value.
std::map<std::string, std::string> parse_cfg(const std::wstring& path) {
    std::map<std::string, std::string> kv;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || !f) return kv;
    char line[512];
    auto trim = [](std::string& s) {
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) { s.clear(); return; }
        const size_t e = s.find_last_not_of(" \t\r\n");
        s = s.substr(b, e - b + 1);
    };
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r') continue;
        const char* eq = strchr(p, '=');
        if (!eq) continue;
        std::string k(p, (size_t)(eq - p));
        std::string v(eq + 1);
        const size_t hash = v.find('#');
        if (hash != std::string::npos) v.erase(hash);
        trim(k);
        trim(v);
        if (!k.empty()) kv[k] = v;
    }
    fclose(f);
    return kv;
}

void publish_snapshot() {
    Snapshot* s = new (std::nothrow) Snapshot();
    if (!s) return;                       // keep the old one, do not crash
    s->n = (int)g_names.size();
    if (s->n > kMaxPresets) s->n = kMaxPresets;
    for (int i = 0; i < s->n; ++i)
        strncpy_s(s->name[i], g_names[(size_t)i].c_str(), _TRUNCATE);
    g_snap.store(s, std::memory_order_release);
}

void do_rescan() {
    g_files.clear();
    g_names.clear();

    WIN32_FIND_DATAW fd;
    const std::wstring pat = dir_path() + L"\\*.cfg";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            g_files.emplace_back(fd.cFileName);
            if ((int)g_files.size() >= kMaxPresets) break;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // Sorted by file name, so which digit a preset sits on is a property of
    // its name and not of the order the filesystem happened to return them.
    // Number them 01-, 02- and the numpad digit matches the prefix.
    std::sort(g_files.begin(), g_files.end());
    g_names.reserve(g_files.size());
    for (const auto& f : g_files) g_names.push_back(display_name(f));
    publish_snapshot();

    const int n     = (int)g_files.size();
    const int banks = n > 0 ? (n + kSlots - 1) / kSlots : 0;
    if (g_bank.load(std::memory_order_relaxed) >= banks)
        g_bank.store(banks > 0 ? banks - 1 : 0, std::memory_order_relaxed);

    if (n == 0) {
        LOG_INFO("preset: none found. Put WHOLE copies of grwxr.cfg in "
                 "GRWVR\\presets\\, named 01-something.cfg. They sort by name "
                 "and the numpad digits load them: 1 is the first, 0 is the "
                 "tenth.");
    } else {
        LOG_INFO("preset: %d preset(s) in GRWVR\\presets, %d bank(s) of %d. "
                 "Numpad 1..9 then 0 loads the current bank.", n, banks,
                 kSlots);
    }
}

// Key 1 is the first preset in the bank and key 0 is the tenth, because that
// is the order the digits read and the order a numbered file name implies.
int slot_to_index(int slot) {
    const int within = (slot == 0) ? 9 : (slot - 1);
    return g_bank.load(std::memory_order_relaxed) * kSlots + within;
}

void back_up_live_cfg_once() {
    if (g_backed_up.exchange(true)) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf_s(stamp, L".bak-before-presets-%04d%02d%02d-%02d%02d%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring dst = live_cfg() + stamp;
    if (CopyFileW(live_cfg().c_str(), dst.c_str(), TRUE)) {
        LOG_INFO("preset: grwxr.cfg copied to %s before the first preset load. "
                 "Presets overwrite grwxr.cfg by design (that is what keeps "
                 "the live state readable in one file); this is the way back.",
                 to_utf8(dst).c_str());
    } else {
        LOG_WARN("preset: could NOT back up grwxr.cfg (error %lu). The preset "
                 "still loads, but the cfg this session started with is not "
                 "saved anywhere.", GetLastError());
    }
}

// Bounded, so a 60-key config cannot produce a 60-key log line.
void append_bounded(std::string& out, const std::string& item, int& shown,
                    int max_shown) {
    if (shown >= max_shown) return;
    if (!out.empty()) out += ", ";
    out += item;
    ++shown;
}

void do_load(int slot) {
    const int idx = slot_to_index(slot);
    const bool have = idx >= 0 && idx < (int)g_files.size();

    // THE NUMPAD 5 LESSON (2026-08-15): an empty slot says so out loud. From
    // inside a headset a key that appears to do nothing is indistinguishable
    // from a key that did something wrong, and telling those two apart has
    // cost this project whole sessions.
    if (!have) {
        LOG_WARN("preset: numpad %d is empty (bank %d, %d preset(s) on disk)",
                 slot, g_bank.load(std::memory_order_relaxed) + 1,
                 (int)g_files.size());
        voice::say("No preset on that key.");
        return;
    }

    const std::wstring file = g_files[(size_t)idx];
    const std::string  name = g_names[(size_t)idx];
    const std::wstring src  = dir_path() + L"\\" + file;

    const auto before = parse_cfg(live_cfg());
    const auto after  = parse_cfg(src);

    if (after.empty()) {
        LOG_WARN("preset: %s parsed to zero keys, NOT loading it. An empty "
                 "file would leave every value exactly as it is and read as a "
                 "preset that did nothing.", to_utf8(file).c_str());
        voice::say("That preset is empty.");
        return;
    }

    back_up_live_cfg_once();

    if (!CopyFileW(src.c_str(), live_cfg().c_str(), FALSE)) {
        LOG_WARN("preset: could not copy %s over grwxr.cfg (error %lu). "
                 "Nothing changed.", to_utf8(file).c_str(), GetLastError());
        voice::say("Preset failed to load.");
        return;
    }

    // The copy alone would be picked up by build 21's 1 Hz watcher within a
    // second. Calling poll_config() applies it NOW instead, on this same
    // thread, through exactly that path: the write time moved, so it reloads
    // and logs. The next 1 Hz tick sees no further change and does nothing.
    vr::poll_config();

    // What actually changed, so "number 3 felt right" can be turned back into
    // values without guessing. This diff is the reason the load logs more than
    // a file name.
    std::string changed, added, missing;
    int nch = 0, nadd = 0, nmiss = 0, sch = 0, sadd = 0, smiss = 0;
    for (const auto& kv : after) {
        const auto it = before.find(kv.first);
        if (it == before.end()) {
            ++nadd;
            append_bounded(added, kv.first + " = " + kv.second, sadd, 8);
        } else if (it->second != kv.second) {
            ++nch;
            append_bounded(changed,
                           kv.first + " " + it->second + " -> " + kv.second,
                           sch, 12);
        }
    }
    for (const auto& kv : before) {
        if (after.find(kv.first) == after.end()) {
            ++nmiss;
            append_bounded(missing, kv.first, smiss, 12);
        }
    }

    LOG_INFO("preset: LOADED numpad %d = \"%s\" (%s), %d changed, %d added",
             slot, name.c_str(), to_utf8(file).c_str(), nch, nadd);
    if (nch)
        LOG_INFO("preset:   changed: %s%s", changed.c_str(),
                 nch > sch ? ", ..." : "");
    if (nadd)
        LOG_INFO("preset:   added: %s%s", added.c_str(),
                 nadd > sadd ? ", ..." : "");

    // The additive-load trap, announced. load_config() sets the keys it finds
    // and leaves the rest alone, so any key this preset omits is STILL AT THE
    // PREVIOUS PRESET'S VALUE and is not reset to a default. A test run on a
    // config that is half of one preset and half of another is the exact class
    // of silently wrong state this project has paid for most often.
    if (nmiss)
        LOG_WARN("preset:   INCOMPLETE. %d key(s) the previous config set are "
                 "absent here and KEEP THEIR CURRENT VALUE, they are not "
                 "reset: %s%s. A preset should be a whole copy of grwxr.cfg.",
                 nmiss, missing.c_str(), nmiss > smiss ? ", ..." : "");

    strncpy_s(g_current, name.c_str(), _TRUNCATE);
    voice::say(name.c_str());
}

}  // namespace

void init() { do_rescan(); }

void pump() {
    if (g_req_rescan.exchange(false, std::memory_order_relaxed)) do_rescan();
    const int slot = g_req.exchange(-1, std::memory_order_relaxed);
    if (slot >= 0 && slot < kSlots) do_load(slot);
}

void load_slot(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    g_req.store(slot, std::memory_order_relaxed);
}

void rescan() { g_req_rescan.store(true, std::memory_order_relaxed); }

void bank_step(int dir) {
    const int banks = bank_count();
    if (banks <= 1) return;
    int b = g_bank.load(std::memory_order_relaxed) + (dir >= 0 ? 1 : -1);
    if (b < 0) b = banks - 1;
    if (b >= banks) b = 0;
    g_bank.store(b, std::memory_order_relaxed);
    LOG_INFO("preset: bank %d of %d", b + 1, banks);
}

int count() {
    const Snapshot* s = g_snap.load(std::memory_order_acquire);
    return s ? s->n : 0;
}

int bank() { return g_bank.load(std::memory_order_relaxed); }

int bank_count() {
    const int n = count();
    return n > 0 ? (n + kSlots - 1) / kSlots : 0;
}

const char* name_of_slot(int slot) {
    if (slot < 0 || slot >= kSlots) return nullptr;
    const Snapshot* s = g_snap.load(std::memory_order_acquire);
    if (!s) return nullptr;
    const int idx = slot_to_index(slot);
    if (idx < 0 || idx >= s->n) return nullptr;
    return s->name[idx];
}

const char* current() { return g_current[0] ? g_current : nullptr; }

}  // namespace presets
}  // namespace grwxr
