// AnselProbe.cpp - PHASE 4 STEP 1: read the engine's coordinate conventions
// straight out of the NVIDIA Ansel configuration struct.
//
// WHY THIS IS WORTH DOING FIRST
//
// Handedness, up axis and forward basis vector are the number one source of
// bugs in a VR injection port, and the normal way to establish them is a
// six-step nudge-and-observe procedure (docs/QUESTIONS.md Q3) that takes hours
// and where two wrong answers can cancel out and look right.
//
// But GRW.exe statically imports ansel::setConfiguration, and the Ansel SDK
// requires a title to DECLARE its coordinate convention in that struct:
// ansel::Configuration begins with three Vec3 basis vectors, right/up/forward,
// followed by speeds and a metersInWorldUnit scale. If we can see that struct
// we get the conventions as DATA, plus the world unit scale, which we would
// otherwise have to calibrate by hand.
//
// HOW
//
// We hook the Import Address Table slot, not the function. The IAT entry for
// setConfiguration is at RVA 0x162AD098 (docs/RE-notes.md), and since ASLR is
// off and the base is confirmed 0x140000000 at runtime, the slot address is
// exact. An IAT hook writes one pointer into a data section and touches no
// code at all, which matters on a Denuvo target.
//
// WE DELIBERATELY DO NOT ASSUME THE STRUCT LAYOUT. Ansel SDK 1.0.937 field
// order varies between revisions, so instead of casting to a struct we dump the
// leading bytes as floats, ints and hex and identify the basis vectors by
// inspection: they will be unit vectors, i.e. nine floats that are all 0.0 or
// +/-1.0. Guessing a layout is exactly the kind of plausible-but-wrong step
// this project cannot afford.
//
// TIMING CAVEAT: setConfiguration is typically called once during graphics
// init. If the game calls it before our DLL is loaded we will miss it, and the
// log will say so rather than silently reporting nothing. In that case the
// fallback is static: find the xref to this IAT slot in Ghidra and read the
// float constants the game stores into the struct before the call.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "Log.h"

#include <atomic>

namespace grwxr {
namespace ansel {
namespace {

// docs/RE-notes.md: IAT slot RVAs, from tools/pe_inventory.py.
constexpr uintptr_t kIatSetConfiguration = 0x162AD098;
constexpr uintptr_t kIatUpdateCamera     = 0x162AD0A0;
constexpr uintptr_t kIatIsAnselAvailable = 0x162AD0A8;

using SetConfigFn = int(__cdecl*)(const void* cfg);
SetConfigFn g_orig_setconfig = nullptr;
void** g_slot = nullptr;

std::atomic<bool> g_captured{false};
alignas(16) unsigned char g_snapshot[256] = {};

// Dump a blob three ways. The float view is what identifies the basis vectors.
void dump(const unsigned char* p, size_t n) {
    LOG_INFO("--- ansel::Configuration, first %zu bytes ---", n);
    LOG_INFO("  off   hex                                       float        int");
    for (size_t off = 0; off + 4 <= n; off += 4) {
        float f;
        int   i;
        memcpy(&f, p + off, 4);
        memcpy(&i, p + off, 4);
        // Only print the float if it is a plausible finite value; garbage
        // floats printed as %f are noise that hides the signal.
        char fbuf[32];
        if (f == f && f > -1e9f && f < 1e9f) snprintf(fbuf, sizeof(fbuf), "%12.6f", f);
        else                                 snprintf(fbuf, sizeof(fbuf), "%12s", "-");
        LOG_INFO("  +0x%03zX  %02X %02X %02X %02X                               %s  %11d",
                 off, p[off], p[off + 1], p[off + 2], p[off + 3], fbuf, i);
    }
    LOG_INFO("--- end ---");
}

// Interpret the leading 9 floats as three basis vectors and say what convention
// they imply. This is the whole point of the exercise.
void interpret(const unsigned char* p) {
    float v[9];
    memcpy(v, p, sizeof(v));

    auto unitish = [](float a) { return fabsf(fabsf(a) - 1.0f) < 1e-3f || fabsf(a) < 1e-3f; };
    bool looks_like_basis = true;
    for (float f : v) if (!unitish(f)) looks_like_basis = false;

    LOG_INFO("");
    LOG_INFO("=== COORDINATE CONVENTION, read from Ansel ===");
    LOG_INFO("  right   = (%+.4f, %+.4f, %+.4f)", v[0], v[1], v[2]);
    LOG_INFO("  up      = (%+.4f, %+.4f, %+.4f)", v[3], v[4], v[5]);
    LOG_INFO("  forward = (%+.4f, %+.4f, %+.4f)", v[6], v[7], v[8]);

    if (!looks_like_basis) {
        LOG_WARN("  These are NOT unit vectors, so the struct layout is not what we");
        LOG_WARN("  assumed. Do NOT trust the interpretation above. Read the hex dump");
        LOG_WARN("  and find the nine consecutive 0/+-1 floats by eye.");
        return;
    }

    // Handedness: right x up should equal forward for a left-handed system,
    // and -forward for a right-handed one.
    const float cx = v[1] * v[5] - v[2] * v[4];
    const float cy = v[2] * v[3] - v[0] * v[5];
    const float cz = v[0] * v[4] - v[1] * v[3];
    const float dot = cx * v[6] + cy * v[7] + cz * v[8];
    LOG_INFO("  (right x up) . forward = %+.4f", dot);
    LOG_INFO("  => %s-HANDED", dot > 0.5f ? "LEFT" : (dot < -0.5f ? "RIGHT" : "INDETERMINATE"));

    const char* axis[3] = { "X", "Y", "Z" };
    auto name_axis = [&](const float* a) -> const char* {
        for (int i = 0; i < 3; ++i) if (fabsf(a[i]) > 0.5f) return axis[i];
        return "?";
    };
    auto sign_of = [&](const float* a) -> const char* {
        for (int i = 0; i < 3; ++i) if (fabsf(a[i]) > 0.5f) return a[i] > 0 ? "+" : "-";
        return "";
    };
    LOG_INFO("  up axis      = %s%s", sign_of(v + 3), name_axis(v + 3));
    LOG_INFO("  forward axis = %s%s", sign_of(v + 6), name_axis(v + 6));
    LOG_INFO("");
    LOG_INFO("  Compare against the reference implementation (Odyssey/Valhalla):");
    LOG_INFO("    left-handed, +Z up, +Y forward.");
    LOG_INFO("  If Wildlands matches, Y_UP_TO_Z_UP_BASIS transfers unchanged.");
    LOG_INFO("=== end ===");
}

int __cdecl hooked_setconfig(const void* cfg) {
    if (cfg && !g_captured.exchange(true)) {
        memcpy(g_snapshot, cfg, sizeof(g_snapshot));
    }
    return g_orig_setconfig ? g_orig_setconfig(cfg) : 0;
}

// --- ansel::updateCamera -----------------------------------------------------
//
// WHY THIS IS THE MOST INTERESTING HOOK IN THE PROJECT SO FAR
//
// The Ansel SDK's contract for updateCamera is that the GAME fills in a Camera
// struct with its current pose and calls into Ansel, and ANSEL WRITES BACK the
// pose the photo-mode user has flown to. The game then applies it. That is not
// a read-only notification, it is an engine-native camera override with a
// documented interface, and GRW.exe imports it (docs/RE-notes.md).
//
// So this hook can answer, with evidence rather than argument, the question the
// whole port rests on: will Wildlands accept an externally supplied camera pose
// and render the world from it? Photo mode existing at all means the answer is
// probably yes. Watching the bytes change proves it and shows us the exact
// field layout, units and conventions.
//
// The technique is to snapshot the struct BEFORE calling the original and again
// AFTER. Whatever Ansel changed is the override. Fields that never change are
// the game's own inputs. This identifies the layout by observation rather than
// by assuming an SDK revision's header, the same discipline used for
// setConfiguration.
//
// CAVEAT, so this is not oversold: photo mode almost certainly pauses gameplay,
// so this is very unlikely to be the shipping mechanism for phase 5. Its value
// is as a research instrument and as proof that the engine has a working
// camera-override path at all.

using UpdateCameraFn = void(__cdecl*)(void* cam);
UpdateCameraFn g_orig_updatecam = nullptr;
void**         g_slot_updatecam = nullptr;

constexpr size_t kCamBytes = 96;

std::atomic<unsigned long long> g_cam_calls{0};
std::atomic<bool>               g_cam_dirty{false};
alignas(16) unsigned char       g_cam_before[kCamBytes] = {};
alignas(16) unsigned char       g_cam_after[kCamBytes]  = {};

// Called once per frame while a photo session is live. No logging, no locks,
// no allocation (project rule 8): two small memcpys and a counter.
void __cdecl hooked_updatecam(void* cam) {
    if (!cam) {
        if (g_orig_updatecam) g_orig_updatecam(cam);
        return;
    }
    unsigned char before[kCamBytes];
    memcpy(before, cam, kCamBytes);

    if (g_orig_updatecam) g_orig_updatecam(cam);

    memcpy(g_cam_before, before, kCamBytes);
    memcpy(g_cam_after, cam, kCamBytes);
    g_cam_calls.fetch_add(1, std::memory_order_relaxed);
    g_cam_dirty.store(true, std::memory_order_release);
}

// Print the two snapshots side by side. A field Ansel overrode shows a delta.
void dump_camera() {
    LOG_INFO("");
    LOG_INFO("######## ansel::updateCamera, %llu calls so far ########",
             (unsigned long long)g_cam_calls.load(std::memory_order_relaxed));
    LOG_INFO("  A field that CHANGES is one Ansel is overriding, i.e. one the");
    LOG_INFO("  engine will accept from outside. That is the whole point.");
    LOG_INFO("  off    before          after           delta      raw(after)");
    for (size_t off = 0; off + 4 <= kCamBytes; off += 4) {
        float b, a;
        memcpy(&b, g_cam_before + off, 4);
        memcpy(&a, g_cam_after + off, 4);
        const bool changed = memcmp(g_cam_before + off, g_cam_after + off, 4) != 0;
        char bs[24], as[24], ds[24];
        auto fmt = [](char* out, size_t n, float v) {
            if (v == v && v > -1e9f && v < 1e9f) snprintf(out, n, "%14.6f", v);
            else                                 snprintf(out, n, "%14s", "-");
        };
        fmt(bs, sizeof(bs), b);
        fmt(as, sizeof(as), a);
        fmt(ds, sizeof(ds), a - b);
        unsigned int raw = 0;
        memcpy(&raw, g_cam_after + off, 4);
        LOG_INFO("  %s+0x%02zX %s %s %s   0x%08X",
                 changed ? "*" : " ", off, bs, as, changed ? ds : "              ", raw);
    }
    LOG_INFO("######## end ########");
}

}  // namespace

bool install() {
    HMODULE base = GetModuleHandleW(nullptr);
    if (!base) return false;
    if ((uintptr_t)base != 0x140000000ull) {
        LOG_WARN("ansel: module base is 0x%p, not the expected 0x140000000.", (void*)base);
        LOG_WARN("ansel: IAT RVAs may be wrong. Skipping to avoid a bad write.");
        return false;
    }

    g_slot = (void**)((uintptr_t)base + kIatSetConfiguration);

    // Sanity: the slot should currently point into anselsdk64.dll.
    HMODULE owner = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)*g_slot, &owner);
    char nm[MAX_PATH] = "(none)";
    if (owner) GetModuleFileNameA(owner, nm, MAX_PATH);
    LOG_INFO("ansel: IAT slot 0x%p currently -> 0x%p (%s)",
             (void*)g_slot, *g_slot, nm);

    if (!owner || !strstr(nm, "ansel")) {
        LOG_WARN("ansel: slot does not point into anselsdk64.dll. Not hooking.");
        return false;
    }

    g_orig_setconfig = (SetConfigFn)*g_slot;
    DWORD old = 0;
    if (!VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG_ERROR("ansel: VirtualProtect on the IAT slot failed");
        return false;
    }
    *g_slot = (void*)&hooked_setconfig;
    VirtualProtect(g_slot, sizeof(void*), old, &old);

    LOG_INFO("ansel: setConfiguration IAT hook installed (original 0x%p)",
             (void*)g_orig_setconfig);
    LOG_INFO("ansel: waiting for the game to call it. If nothing appears, the");
    LOG_INFO("ansel: call happened before we loaded; use the Ghidra xref fallback.");

    // --- updateCamera, the camera-override observation hook -----------------
    g_slot_updatecam = (void**)((uintptr_t)base + kIatUpdateCamera);
    HMODULE owner2 = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)*g_slot_updatecam, &owner2);
    char nm2[MAX_PATH] = "(none)";
    if (owner2) GetModuleFileNameA(owner2, nm2, MAX_PATH);
    if (!owner2 || !strstr(nm2, "ansel")) {
        LOG_WARN("ansel: updateCamera slot does not point into anselsdk64.dll "
                 "(%s). Not hooking it.", nm2);
        return true;   // setConfiguration is still installed
    }

    g_orig_updatecam = (UpdateCameraFn)*g_slot_updatecam;
    DWORD old2 = 0;
    if (!VirtualProtect(g_slot_updatecam, sizeof(void*), PAGE_READWRITE, &old2)) {
        LOG_ERROR("ansel: VirtualProtect on the updateCamera IAT slot failed");
        return true;
    }
    *g_slot_updatecam = (void*)&hooked_updatecam;
    VirtualProtect(g_slot_updatecam, sizeof(void*), old2, &old2);

    LOG_INFO("ansel: updateCamera IAT hook installed (original 0x%p)",
             (void*)g_orig_updatecam);
    LOG_INFO("ansel: press F10 in game to open photo mode, then fly the camera.");
    LOG_INFO("ansel: every field Ansel overrides will be logged as a delta.");
    return true;
}

// Called from the init thread once per second; logs the snapshot when it lands.
void drain() {
    static bool done = false;
    if (!done && g_captured.load(std::memory_order_acquire)) {
        done = true;
        LOG_INFO("");
        LOG_INFO("############ ANSEL setConfiguration CALLED ############");
        dump(g_snapshot, 128);
        interpret(g_snapshot);
    }

    // Photo mode. Report the first call immediately, then every 5 seconds while
    // a session is live, so flying the camera around produces a trail of poses
    // rather than one frozen snapshot.
    static bool               cam_seen = false;
    static unsigned long long last_cam = 0;
    static int                cam_tick = 0;
    if (g_cam_dirty.load(std::memory_order_acquire)) {
        const unsigned long long now = g_cam_calls.load(std::memory_order_relaxed);
        const bool first = !cam_seen;
        if (first) {
            cam_seen = true;
            LOG_INFO("");
            LOG_INFO("!!!! PHOTO MODE IS LIVE. The engine is calling "
                     "ansel::updateCamera. !!!!");
        }
        if (first || (now != last_cam && (++cam_tick % 5) == 0)) {
            last_cam = now;
            dump_camera();
        }
    }
}

}  // namespace ansel
}  // namespace grwxr
