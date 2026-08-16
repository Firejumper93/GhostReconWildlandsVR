#include "Menu.h"
#include "D3D11Hook.h"
#include "GameBuild.h"   // build 86: the pin, shown first on the Status page
#include "Log.h"
#include "VRMirror.h"    // build 86: live barrel-aim state for the panel

#include "imgui.h"
#include "imgui_impl_dx11.h"

#include <atomic>
#include <cfloat>    // FLT_MAX, parking the cursor off the panel
#include <cstdarg>   // note()
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace grwxr {
namespace menu {
namespace {

// ---------------------------------------------------------------------------
// Device objects. All created in init(), on the init thread.
// ---------------------------------------------------------------------------
ID3D11Device*           g_dev = nullptr;
ID3D11DeviceContext*    g_ctx = nullptr;
ID3D11Texture2D*        g_tex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;

std::atomic<bool> g_ready{false};
std::atomic<bool> g_open{false};

// Pointer, written by the VR side each frame, read by render().
std::atomic<bool>  g_ptr_hit{false};
std::atomic<float> g_ptr_u{0.5f};
std::atomic<float> g_ptr_v{0.5f};
std::atomic<bool>  g_ptr_down{false};

// ---------------------------------------------------------------------------
// Probes.
// ---------------------------------------------------------------------------
struct Probe {
    const char* label;
    const char* help;
    std::atomic<int>       state{kIdle};
    std::atomic<long long> count{0};
    std::atomic<bool>      fire{false};
    std::atomic<const char*> unavailable{nullptr};
};

// The table is fixed at compile time, so the panel can never show a probe the
// build does not contain, which is half of "no activation".
Probe g_probe[kProbeCount];

void init_probe_table() {
    g_probe[kProbeWeaponDraw].label = "Weapon draw census";
    g_probe[kProbeWeaponDraw].help =
        "Walks the weapon-visible / weapon-gone / dump sequence. Stand with a "
        "weapon drawn, fire it once, holster, fire again, then dump. Log only.";
    g_probe[kProbePalette].label = "Skinning palette capture";
    g_probe[kProbePalette].help =
        "Opens a one-shot capture window over the next frames and dumps the "
        "shader resource bindings it sees. Log only.";
    g_probe[kProbeFirstPerson].label = "First person toggle";
    g_probe[kProbeFirstPerson].help =
        "Moves the camera to the head bone and hides the head mesh. Needs a "
        "live player pin and a readable head, which is why it is gated here: "
        "firing it in the pre-game lobby froze the render thread on 2026-08-05.";
    g_probe[kProbeRecenter].label = "Recenter view";
    g_probe[kProbeRecenter].help =
        "Re-zeroes the head pose to where you are looking now. Always safe.";
}

// ---------------------------------------------------------------------------
// Settings.
//
// One row per grwxr.cfg key the panel exposes. The value shown is whatever the
// file currently holds, or `def` when the key is absent, so the panel and the
// file can never disagree about what the game is running.
// ---------------------------------------------------------------------------
struct Setting {
    const char* key;
    const char* label;
    const char* help;
    float def, lo, hi;
    bool  is_toggle;
    float value;      // live edit target, seeded from the file
    bool  dirty;
};

Setting g_set[] = {
    {"ipd_scale", "Stereo strength (IPD scale)",
     "How far apart the two eye viewpoints sit. 1.00 is your measured IPD. "
     "Lower it if depth feels exaggerated or uncomfortable.",
     1.00f, -2.0f, 2.0f, false, 1.00f, false},

    {"fullscreen_fov", "World field of view (radians)",
     "Widens the image the engine renders so it fills the headset. 0 turns the "
     "override off and gives you the game's own framing.",
     0.0f, 0.0f, 2.5f, false, 0.0f, false},

    {"desktop_fov", "Desktop mirror crop (radians)",
     "What the monitor shows while you are in the headset. 0 disables the "
     "separate desktop view. Useful when recording.",
     0.0f, 0.0f, 2.0f, false, 0.0f, false},

    {"stick_pitch", "Right stick can pitch the view",
     "OFF means only your head looks up and down, which is what stops the view "
     "flipping over. Turn it on for aircraft.",
     0.0f, 0.0f, 1.0f, true, 0.0f, false},

    {"hand_markers", "Show hand markers",
     "Blue and orange dots on your tracked controller positions.",
     1.0f, 0.0f, 1.0f, true, 1.0f, false},

    {"wp_markers", "Show weapon-candidate markers",
     "Research aid. Coloured dots on nearby engine objects, with a legend in "
     "the log once a second. Leave off for normal play.",
     0.0f, 0.0f, 1.0f, true, 0.0f, false},

    {"max_frame_latency", "Frames in flight",
     "How many frames Windows lets the game queue ahead of the display. 0 "
     "leaves the game's own setting alone. 1 pairs the two eyes more tightly "
     "in time; put it back to 0 if it costs frame rate.",
     0.0f, 0.0f, 4.0f, false, 0.0f, false},

    // Build 86: the weapon and aim rows. These are the settings a headset test
    // actually needs to change, and until now every one of them required
    // taking the headset off and editing grwxr.cfg in a text editor. That is
    // a large part of why the two-handed hold and the barrel aim have both
    // sat built-but-untested across several sessions.
    {"aim_barrel", "Bullets follow the barrel",
     "0 off. 1 steers only while you hold the fire trigger. 2 keeps the gun's "
     "aim live all the time, which is the one that behaves like a real weapon, "
     "because a real gun is already pointing where it points before you pull. "
     "Needs the motion-controlled weapon switched on.",
     0.0f, 0.0f, 2.0f, false, 0.0f, false},

    {"wgun_twohand", "Two-handed hold",
     "Your rear hand sets where the gun is, your front hand sets where it "
     "points, like a real rifle. Off means the rear hand alone aims it.",
     1.0f, 0.0f, 1.0f, true, 1.0f, false},

    {"wgun_pos_clamp", "Gun reach (metres)",
     "How far the gun may travel from its resting anchor before it stops "
     "following your hand. Raise it if the gun feels tethered and will not "
     "come up to your eye; lower it if it wanders too far.",
     0.60f, 0.10f, 2.00f, false, 0.60f, false},

    {"wgun_pos_scale", "Gun travel scale",
     "Multiplies how far the gun moves for a given hand movement. 1.0 is "
     "one-to-one with your real hand. Below 1 damps it, above 1 exaggerates.",
     1.0f, 0.25f, 2.0f, false, 1.0f, false},

    {"wgun_roll_deg", "Gun roll trim (degrees)",
     "Rotates the weapon about its own barrel. Use this if the gun is canted "
     "or upside down; the usual corrections are 90 or 180.",
     0.0f, -180.0f, 180.0f, false, 0.0f, false},

    {"aim_ads", "Trigger also aims down sights",
     "OFF gives you real hip fire, with aiming moved to the left trigger where "
     "the game normally puts it. ON is the older behaviour where a half pull "
     "raises the sights, which means every shot goes through ADS.",
     0.0f, 0.0f, 1.0f, true, 0.0f, false},
};
constexpr int kSettingCount = (int)(sizeof(g_set) / sizeof(g_set[0]));

// The cfg file, held verbatim so a save preserves every comment and every key
// the panel does not know about.
std::vector<std::string> g_cfg_lines;
bool g_cfg_loaded = false;

std::atomic<bool> g_want_save{false};
std::atomic<bool> g_want_reload{false};

// Deferred log line, drained by poll(), so render() never touches the log.
char g_note[256] = {};
std::atomic<bool> g_have_note{false};

void note(const char* fmt, ...) {
    if (g_have_note.load(std::memory_order_acquire)) return;   // do not stack
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_note, sizeof(g_note), fmt, ap);
    va_end(ap);
    g_have_note.store(true, std::memory_order_release);
}

std::wstring cfg_path() { return log::data_dir() + L"\\grwxr.cfg"; }

// Trim, then test whether `line` assigns `key`. Returns the value when it does.
bool line_is_key(const std::string& line, const char* key, float* out) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] == '#') return false;
    const size_t klen = strlen(key);
    if (line.compare(i, klen, key) != 0) return false;
    size_t j = line.find_first_not_of(" \t", i + klen);
    if (j == std::string::npos || line[j] != '=') return false;
    if (out) *out = (float)atof(line.c_str() + j + 1);
    return true;
}

// Init thread. Reads the file into memory and seeds every slider from it.
void load_cfg_file() {
    g_cfg_lines.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, cfg_path().c_str(), L"rt") == 0 && f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            g_cfg_lines.push_back(s);
        }
        fclose(f);
    }
    for (int i = 0; i < kSettingCount; ++i) {
        g_set[i].value = g_set[i].def;
        g_set[i].dirty = false;
        for (const std::string& l : g_cfg_lines) {
            float v = 0.0f;
            if (line_is_key(l, g_set[i].key, &v)) { g_set[i].value = v; break; }
        }
    }
    g_cfg_loaded = true;
}

// Init thread. Rewrites only the lines the panel changed, appends keys that were
// not in the file, and leaves everything else byte for byte as the tester wrote
// it. A settings menu that eats your comments is a settings menu you stop using.
void save_cfg_file() {
    if (!g_cfg_loaded) return;

    int changed = 0, added = 0;
    for (int i = 0; i < kSettingCount; ++i) {
        if (!g_set[i].dirty) continue;
        char repl[160];
        snprintf(repl, sizeof(repl), "%s = %g", g_set[i].key, (double)g_set[i].value);
        bool found = false;
        for (std::string& l : g_cfg_lines) {
            if (line_is_key(l, g_set[i].key, nullptr)) { l = repl; found = true; ++changed; break; }
        }
        if (!found) { g_cfg_lines.push_back(repl); ++added; }
        g_set[i].dirty = false;
    }
    if (!changed && !added) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, cfg_path().c_str(), L"wt") != 0 || !f) {
        LOG_WARN("menu: could not write grwxr.cfg, settings NOT saved");
        return;
    }
    for (const std::string& l : g_cfg_lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    LOG_INFO("menu: grwxr.cfg written (%d changed, %d added). The hot reload "
             "applies it within a second.", changed, added);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(420.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void draw_settings_page() {
    ImGui::TextWrapped(
        "These write GRWVR\\grwxr.cfg. Nothing changes until you press Apply, "
        "and the game picks it up about a second later.");
    ImGui::Separator();

    bool any_dirty = false;
    for (int i = 0; i < kSettingCount; ++i) {
        Setting& s = g_set[i];
        ImGui::PushID(i);
        if (s.is_toggle) {
            bool on = s.value != 0.0f;
            if (ImGui::Checkbox(s.label, &on)) { s.value = on ? 1.0f : 0.0f; s.dirty = true; }
        } else {
            if (ImGui::SliderFloat(s.label, &s.value, s.lo, s.hi, "%.2f")) s.dirty = true;
        }
        help_marker(s.help);
        if (s.dirty) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "*");
            any_dirty = true;
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    if (!any_dirty) ImGui::BeginDisabled();
    if (ImGui::Button("Apply and save", ImVec2(200, 0))) g_want_save.store(true);
    if (!any_dirty) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload from file", ImVec2(200, 0))) g_want_reload.store(true);
    ImGui::SameLine();
    ImGui::TextDisabled(any_dirty ? "unsaved changes" : "in sync with the file");
}

void draw_captures_page() {
    ImGui::TextWrapped(
        "Each capture says whether it can run right now, and what it is doing "
        "once it does. If a button is greyed out, the reason is beside it: that "
        "is the state that would previously have crashed or silently done "
        "nothing.");
    ImGui::Separator();

    static const char* kStateName[] = { "idle", "ARMED", "RUNNING", "done", "FAILED" };
    static const ImVec4 kStateCol[] = {
        ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
        ImVec4(1.00f, 0.80f, 0.20f, 1.0f),
        ImVec4(0.30f, 0.90f, 0.40f, 1.0f),
        ImVec4(0.55f, 0.80f, 1.00f, 1.0f),
        ImVec4(1.00f, 0.35f, 0.35f, 1.0f),
    };

    for (int i = 0; i < kProbeCount; ++i) {
        Probe& p = g_probe[i];
        const char* why = p.unavailable.load(std::memory_order_relaxed);
        const int   st  = p.state.load(std::memory_order_relaxed);

        ImGui::PushID(i);
        ImGui::Separator();
        ImGui::TextUnformatted(p.label);
        help_marker(p.help);

        if (why) ImGui::BeginDisabled();
        if (ImGui::Button("Run", ImVec2(120, 0))) {
            p.fire.store(true, std::memory_order_release);
            p.state.store(kArmed, std::memory_order_relaxed);
            note("menu: %s armed from the panel", p.label);
        }
        if (why) ImGui::EndDisabled();

        ImGui::SameLine();
        const int sidx = (st >= 0 && st < 5) ? st : 0;
        ImGui::TextColored(kStateCol[sidx], "%s", kStateName[sidx]);
        const long long c = p.count.load(std::memory_order_relaxed);
        if (c) { ImGui::SameLine(); ImGui::TextDisabled("(%lld)", c); }

        if (why) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "unavailable: %s", why);
        }
        ImGui::PopID();
    }
}

void draw_status_page() {
    // Build 86: the game-build pin, first, because every other line on this
    // page is meaningless if the mod did not recognise the exe. After the
    // 2026-08-13 update this was the difference between "the feature does not
    // work" and "nothing installed at all", and that distinction previously
    // lived only in the log.
    const auto* gb = gamebuild::get();
    if (gb) {
        ImGui::Text("Game build       : %s  (recognised)", gb->name);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Game build       : NOT RECOGNISED");
        ImGui::TextWrapped(
            "The game has been updated to a binary this mod has not been "
            "taught yet, so nothing that depends on engine addresses has "
            "installed and the game is running unmodified. Nothing below "
            "will do anything until that is fixed.");
        ImGui::Separator();
    }

    // Anti-cheat, read from the process rather than assumed. Ubisoft removed
    // EasyAntiCheat in the 2026-08-13 update; this is the one place that can
    // confirm it from inside, which is stronger than any external check.
    ImGui::Text("EasyAntiCheat    : %s",
                GetModuleHandleW(L"EasyAntiCheat_x64.dll") ? "LOADED"
                                                           : "not loaded");
    ImGui::Separator();

    // Barrel aim. The three questions a test has to answer, in order: did the
    // loop run at all, did it use the real barrel or fall back to the raw
    // controller ray, and is the error converging toward zero.
    const vr::BarrelStatus b = vr::barrel_status();
    const char* mode = b.mode == 2 ? "2 (always on)"
                     : b.mode == 1 ? "1 (only while firing)"
                                   : "0 (off)";
    ImGui::Text("Bullets follow   : %s", mode);
    if (b.mode != 0) {
        const char* src = b.src == 1 ? "the real barrel"
                        : b.src == 2 ? "CONTROLLER RAY (fallback)"
                                     : "nothing yet";
        ImGui::Text("  steering from  : %s", src);
        ImGui::Text("  frames driven  : %u", b.frames);
        ImGui::Text("  last error     : yaw %+.2f  pitch %+.2f deg",
                    b.err_yaw_deg, b.err_pitch_deg);
        if (b.nodir || b.noview || b.overcap) {
            ImGui::Text("  skipped        : no-direction %u  no-view %u  "
                        "over-limit %u", b.nodir, b.noview, b.overcap);
        }
        if (b.src == 2) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                "The motion-controlled weapon is not publishing a barrel, so "
                "this is steering off the raw controller ray. Turn the weapon "
                "on, or this is not testing what you think it is.");
        }
        if (b.frames == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                "Armed, but it has never actually driven the aim. That is a "
                "different problem from it running and not helping.");
        }
    }
    ImGui::Separator();

    const auto& st = d3d11::state();
    ImGui::Text("Frames presented : %llu", st.frames);
    ImGui::Text("Backbuffer       : %u x %u  (format %d)", st.width, st.height, (int)st.format);
    ImGui::Text("Device           : %p", (void*)st.device);
    ImGui::Separator();
    ImGui::TextWrapped(
        "If you are reporting a problem, the log is GRWVR\\grwxr.log. Its first "
        "line carries the build hash. Quote that line, otherwise we cannot tell "
        "which build you actually ran.");
}

void draw_ui() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("GRW-XR", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextUnformatted("GRW-XR tester panel");
    ImGui::SameLine();
    ImGui::TextDisabled("| F1 closes");
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Settings")) { draw_settings_page(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Captures")) { draw_captures_page(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Status"))   { draw_status_page();   ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}  // namespace

// ---------------------------------------------------------------------------

bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (g_ready.load()) return true;
    if (!dev || !ctx) return false;
    g_dev = dev;
    g_ctx = ctx;

    init_probe_table();
    load_cfg_file();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width      = kMenuW;
    td.Height     = kMenuH;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_tex))) {
        LOG_ERROR("menu: offscreen texture creation failed, no panel");
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(g_tex, nullptr, &g_rtv)) ||
        FAILED(dev->CreateShaderResourceView(g_tex, nullptr, &g_srv))) {
        LOG_ERROR("menu: offscreen views failed, no panel");
        shutdown();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // NEVER let ImGui write its own ini. It would land beside the game exe,
    // and the project's default rule is that we place nothing in the install
    // except the proxy and the GRWVR folder.
    io.IniFilename  = nullptr;
    io.LogFilename  = nullptr;
    io.DisplaySize  = ImVec2((float)kMenuW, (float)kMenuH);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // The panel is read at arm's length through a headset lens, not on a
    // monitor at 60 cm. Everything is scaled up accordingly.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(1.4f);
    style.WindowRounding = 0.0f;
    io.FontGlobalScale = 1.6f;

    if (!ImGui_ImplDX11_Init(dev, ctx)) {
        LOG_ERROR("menu: ImGui DX11 backend init failed, no panel");
        ImGui::DestroyContext();
        shutdown();
        return false;
    }
    // Build the font atlas NOW, on this thread, so the first open does not
    // allocate and upload a texture from inside Present.
    ImGui_ImplDX11_CreateDeviceObjects();

    g_ready.store(true);
    LOG_INFO("menu: tester panel ready (%dx%d, ImGui %s). F1 opens it.",
             kMenuW, kMenuH, IMGUI_VERSION);
    return true;
}

void shutdown() {
    if (g_ready.exchange(false)) {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
    }
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    g_dev = nullptr;
    g_ctx = nullptr;
}

bool ready()   { return g_ready.load(std::memory_order_acquire); }
bool is_open() { return g_open.load(std::memory_order_acquire); }

void set_open(bool on) {
    if (g_open.exchange(on) == on) return;
    if (on) g_want_reload.store(true);   // always show what the file really says
    note("menu: panel %s", on ? "opened" : "closed");
}

bool toggle() { const bool want = !is_open(); set_open(want); return want; }

void set_pointer(bool hit, float u, float v, bool pressed) {
    g_ptr_hit.store(hit, std::memory_order_relaxed);
    if (hit) {
        g_ptr_u.store(u, std::memory_order_relaxed);
        g_ptr_v.store(v, std::memory_order_relaxed);
    }
    g_ptr_down.store(hit && pressed, std::memory_order_relaxed);
}

ID3D11Texture2D* texture() { return g_tex; }

ID3D11ShaderResourceView* render() {
    if (!g_ready.load(std::memory_order_acquire)) return nullptr;
    if (!g_open.load(std::memory_order_acquire))  return nullptr;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)kMenuW, (float)kMenuH);
    // A fixed step. The panel has no animation that needs real time, and a
    // measured delta would mean a clock read on the render thread.
    io.DeltaTime = 1.0f / 72.0f;

    if (g_ptr_hit.load(std::memory_order_relaxed)) {
        io.AddMousePosEvent(g_ptr_u.load(std::memory_order_relaxed) * (float)kMenuW,
                            g_ptr_v.load(std::memory_order_relaxed) * (float)kMenuH);
    } else {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);   // park it, do not leave it hovering
    }
    io.AddMouseButtonEvent(0, g_ptr_down.load(std::memory_order_relaxed));

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();

    ID3D11RenderTargetView* prev_rtv = nullptr;
    ID3D11DepthStencilView* prev_dsv = nullptr;
    g_ctx->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);

    const float clear[4] = { 0.05f, 0.06f, 0.08f, 0.92f };
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    // The DX11 backend backs up and restores the whole device state around this
    // call, so it cannot leak into the game's next draw.
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_ctx->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
    if (prev_rtv) prev_rtv->Release();
    if (prev_dsv) prev_dsv->Release();

    return g_srv;
}

void poll() {
    if (g_want_reload.exchange(false)) load_cfg_file();
    if (g_want_save.exchange(false))   save_cfg_file();
    if (g_have_note.load(std::memory_order_acquire)) {
        LOG_INFO("%s", g_note);
        g_have_note.store(false, std::memory_order_release);
    }
}

void set_state(int id, int state, long long count) {
    if (id < 0 || id >= kProbeCount) return;
    g_probe[id].state.store(state, std::memory_order_relaxed);
    g_probe[id].count.store(count, std::memory_order_relaxed);
}

bool fire_pending(int id) {
    if (id < 0 || id >= kProbeCount) return false;
    return g_probe[id].fire.exchange(false, std::memory_order_acq_rel);
}

void set_unavailable(int id, const char* why) {
    if (id < 0 || id >= kProbeCount) return;
    g_probe[id].unavailable.store(why, std::memory_order_relaxed);
}

}  // namespace menu
}  // namespace grwxr
