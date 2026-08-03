// xr_probe.cpp - standalone OpenXR + D3D11 probe for the GRW-XR project.
//
// This is NOT mod code. It never touches Ghost Recon Wildlands, never injects,
// never hooks. It is a diagnostic harness that exercises exactly the OpenXR
// path the mod will later need from inside the game process, but standalone,
// where it can be debugged sanely.
//
// Why it exists:
//   Phase 3 of the plan (OpenXR session, D3D11 binding, STAGE space, swapchain,
//   frame loop) is 6 to 10 days of work that must be REIMPLEMENTED, because the
//   reference mod's core is a private submodule we cannot read. Getting that
//   skeleton right standalone is far cheaper than debugging it inside an
//   injected DLL in a Denuvo-protected process whose module list we cannot even
//   read. Every problem found here is one not found later in the worst possible
//   debugging environment.
//
// What it proves, in order:
//   1. The Meta Horizon OpenXR runtime loads and reports its extensions.
//   2. XR_KHR_D3D11_enable is available (the mod depends on it; Wildlands is D3D11).
//   3. We can create a D3D11 device on the adapter the runtime demands.
//   4. A session reaches FOCUSED state.
//   5. STAGE reference space works (the reference core uses STAGE).
//   6. Swapchains create, and we learn which formats are offered.
//   7. The frame loop runs and frames are accepted by the compositor.
//   8. Head tracking is live.
//
// What it captures that is directly reusable:
//   - The per-eye frustum tangents (XrFovf angleLeft/Right/Up/Down). These are
//     exactly the inputs to the projection matrix in docs/PORT-MAP.md 2.6.
//     Real numbers from the real headset beat assumptions.
//   - Whether the two eyes' frustums are symmetric mirrors. The reference mod
//     uses frustums[0] for BOTH eyes; if this headset is asymmetric that
//     shortcut is a bug we must not inherit.
//   - IPD, as the distance between the two eye poses.
//   - Predicted display period, i.e. the real refresh rate, and how stable
//     xrWaitFrame's predicted display time is. Groundwork for phase 8 timing.
//
// Rendering is deliberately ClearRenderTargetView only: no shaders, no vertex
// buffers, no pipeline state. That removes every avoidable failure mode from a
// first run. You should see a solid colour per eye that CHANGES AS YOU TURN
// YOUR HEAD, and each eye is tinted differently so closing one eye at a time
// proves the eyes really are separate images.
//
// Build:  tools\xr_probe\build.bat
// Run:    tools\xr_probe\xr_probe.exe [seconds]
// Log:    docs\RAW\xr-probe.log

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ---------------------------------------------------------------- logging --

static FILE* g_log = nullptr;

static void logf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (g_log) {
        fputs(buf, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

// XrResult to string. The loader gives us xrResultToString once an instance
// exists; before that, fall back to the numeric code.
static XrInstance g_instance = XR_NULL_HANDLE;

static std::string xrStr(XrResult r) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (g_instance != XR_NULL_HANDLE &&
        XR_SUCCEEDED(xrResultToString(g_instance, r, buf))) {
        return buf;
    }
    char n[32];
    snprintf(n, sizeof(n), "XrResult(%d)", (int)r);
    return n;
}

// Check macro: log and bail out of main on failure.
#define XRC(expr)                                                              \
    do {                                                                       \
        XrResult _r = (expr);                                                  \
        if (XR_FAILED(_r)) {                                                   \
            logf("FAIL  %s\n        -> %s", #expr, xrStr(_r).c_str());         \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#define HRC(expr)                                                              \
    do {                                                                       \
        HRESULT _h = (expr);                                                    \
        if (FAILED(_h)) {                                                      \
            logf("FAIL  %s\n        -> HRESULT 0x%08lX", #expr, (unsigned long)_h); \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static void rule(const char* title) {
    logf("");
    logf("======================================================================");
    logf("  %s", title);
    logf("======================================================================");
}

// ------------------------------------------------------------------- main --

int main(int argc, char** argv) {
    const double runSeconds = (argc > 1) ? atof(argv[1]) : 20.0;

    CreateDirectoryA("docs", nullptr);
    CreateDirectoryA("docs\\RAW", nullptr);
    fopen_s(&g_log, "docs\\RAW\\xr-probe.log", "w");

    logf("GRW-XR OpenXR probe");
    logf("standalone diagnostic, does not touch the game");
    logf("run duration: %.1f s", runSeconds);

    // -- 1. API layers and extensions -------------------------------------
    rule("1. RUNTIME CAPABILITIES");

    uint32_t n = 0;
    XRC(xrEnumerateApiLayerProperties(0, &n, nullptr));
    std::vector<XrApiLayerProperties> layers(n, {XR_TYPE_API_LAYER_PROPERTIES});
    if (n) XRC(xrEnumerateApiLayerProperties(n, &n, layers.data()));
    logf("API layers: %u", n);
    for (auto& l : layers) logf("    %s  (spec %u.%u.%u)", l.layerName,
                                XR_VERSION_MAJOR(l.specVersion),
                                XR_VERSION_MINOR(l.specVersion),
                                XR_VERSION_PATCH(l.specVersion));

    XRC(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    XRC(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data()));
    logf("instance extensions: %u", n);
    bool hasD3D11 = false, hasDepth = false, hasVisMask = false;
    for (auto& e : exts) {
        logf("    %s (v%u)", e.extensionName, e.extensionVersion);
        if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) hasD3D11 = true;
        if (!strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)) hasDepth = true;
        if (!strcmp(e.extensionName, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME)) hasVisMask = true;
    }
    logf("");
    logf("  XR_KHR_D3D11_enable            : %s   <-- REQUIRED, Wildlands is D3D11",
         hasD3D11 ? "PRESENT" : "*** ABSENT ***");
    logf("  XR_KHR_composition_layer_depth : %s", hasDepth ? "present" : "absent");
    logf("  XR_KHR_visibility_mask         : %s", hasVisMask ? "present" : "absent");
    if (!hasD3D11) {
        logf("");
        logf("STOP: the runtime does not expose XR_KHR_D3D11_enable. The whole");
        logf("      approach depends on it. Check that Meta Quest Link is the");
        logf("      active OpenXR runtime, not SteamVR.");
        return 1;
    }

    // -- 2. instance -------------------------------------------------------
    rule("2. INSTANCE");

    const char* enabled[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    strcpy_s(ici.applicationInfo.applicationName, "GRW-XR probe");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "none");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XRC(xrCreateInstance(&ici, &g_instance));

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    XRC(xrGetInstanceProperties(g_instance, &ip));
    logf("runtime      : %s", ip.runtimeName);
    logf("runtime ver  : %u.%u.%u",
         XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion),
         XR_VERSION_PATCH(ip.runtimeVersion));

    // -- 3. system ---------------------------------------------------------
    rule("3. SYSTEM");

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sysId = XR_NULL_SYSTEM_ID;
    {
        XrResult r = xrGetSystem(g_instance, &sgi, &sysId);
        if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
            logf("");
            logf("NO HEADSET AVAILABLE (XR_ERROR_FORM_FACTOR_UNAVAILABLE).");
            logf("");
            logf("This is not a bug in the probe. The runtime loaded fine, it just");
            logf("has no HMD to give us. Checklist:");
            logf("  1. Quest 3 plugged in over the Link cable.");
            logf("  2. Meta Quest Link enabled and CONNECTED (put the headset on and");
            logf("     accept the Link prompt, or start Link from the headset menu).");
            logf("  3. The headset must be awake and on your head, not idle.");
            logf("  4. Meta Horizon / Oculus app running on the PC.");
            logf("");
            logf("Then re-run: tools\\xr_probe\\xr_probe.exe 20");
            return 2;
        }
        if (XR_FAILED(r)) {
            logf("FAIL  xrGetSystem -> %s", xrStr(r).c_str());
            return 1;
        }
    }

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XRC(xrGetSystemProperties(g_instance, sysId, &sp));
    logf("system name         : %s", sp.systemName);
    logf("vendor id           : %u", sp.vendorId);
    logf("max swapchain       : %u x %u",
         sp.graphicsProperties.maxSwapchainImageWidth,
         sp.graphicsProperties.maxSwapchainImageHeight);
    logf("max composition layers: %u", sp.graphicsProperties.maxLayerCount);
    logf("orientation tracking: %s", sp.trackingProperties.orientationTracking ? "yes" : "no");
    logf("position tracking   : %s", sp.trackingProperties.positionTracking ? "yes" : "no");

    uint32_t viewCount = 0;
    XRC(xrEnumerateViewConfigurationViews(
        g_instance, sysId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));
    std::vector<XrViewConfigurationView> vcv(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XRC(xrEnumerateViewConfigurationViews(
        g_instance, sysId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, vcv.data()));
    logf("stereo views        : %u", viewCount);
    for (uint32_t i = 0; i < viewCount; i++) {
        logf("  view %u recommended : %u x %u  (%u samples)   max: %u x %u",
             i, vcv[i].recommendedImageRectWidth, vcv[i].recommendedImageRectHeight,
             vcv[i].recommendedSwapchainSampleCount,
             vcv[i].maxImageRectWidth, vcv[i].maxImageRectHeight);
    }
    logf("");
    logf("  NOTE: recommended per-eye size is the render target the mod must");
    logf("        make Wildlands draw into. Compare against your desktop res to");
    logf("        gauge the GPU cost before phase 8.");

    // -- 4. D3D11 device on the runtime's adapter --------------------------
    rule("4. D3D11 DEVICE");

    PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetReq = nullptr;
    XRC(xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                              (PFN_xrVoidFunction*)&pfnGetReq));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XRC(pfnGetReq(g_instance, sysId, &req));
    logf("required adapter LUID : %08lX:%08lX",
         (unsigned long)req.adapterLuid.HighPart, (unsigned long)req.adapterLuid.LowPart);
    logf("min feature level     : 0x%04X", (unsigned)req.minFeatureLevel);
    logf("");
    logf("  NOTE: the runtime dictates WHICH adapter. Creating the device on the");
    logf("        wrong one is a classic silent failure. Inside the game we do");
    logf("        not choose the device at all, so if the game picks a different");
    logf("        adapter than this LUID we have a real problem to solve.");

    IDXGIFactory1* factory = nullptr;
    HRC(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory));
    IDXGIAdapter1* adapter = nullptr;
    IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
        bool match = (d.AdapterLuid.LowPart == req.adapterLuid.LowPart &&
                      d.AdapterLuid.HighPart == req.adapterLuid.HighPart);
        logf("  adapter %u: %-40s LUID %08lX:%08lX %s", i, name,
             (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart,
             match ? " <-- MATCH" : "");
        if (match) { chosen = adapter; } else { adapter->Release(); }
    }
    if (!chosen) { logf("FAIL: no adapter matches the runtime's LUID"); return 1; }

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL got{};
    HRC(D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                          levels, 2, D3D11_SDK_VERSION, &dev, &got, &ctx));
    logf("D3D11 device created, feature level 0x%04X", (unsigned)got);

    // -- 5. session --------------------------------------------------------
    rule("5. SESSION AND REFERENCE SPACE");

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = sysId;
    XrSession session = XR_NULL_HANDLE;
    XRC(xrCreateSession(g_instance, &sci, &session));
    logf("session created");

    uint32_t spaceCount = 0;
    XRC(xrEnumerateReferenceSpaces(session, 0, &spaceCount, nullptr));
    std::vector<XrReferenceSpaceType> spaces(spaceCount);
    XRC(xrEnumerateReferenceSpaces(session, spaceCount, &spaceCount, spaces.data()));
    bool hasStage = false, hasLocal = false;
    logf("reference spaces offered: %u", spaceCount);
    for (auto s : spaces) {
        const char* nm = s == XR_REFERENCE_SPACE_TYPE_VIEW  ? "VIEW"
                       : s == XR_REFERENCE_SPACE_TYPE_LOCAL ? "LOCAL"
                       : s == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "other";
        logf("    %s", nm);
        if (s == XR_REFERENCE_SPACE_TYPE_STAGE) hasStage = true;
        if (s == XR_REFERENCE_SPACE_TYPE_LOCAL) hasLocal = true;
    }
    logf("");
    logf("  NOTE: the reference mod's core uses STAGE. STAGE %s here.",
         hasStage ? "IS available" : "is NOT available, we would need LOCAL");

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = hasStage ? XR_REFERENCE_SPACE_TYPE_STAGE
                            : (hasLocal ? XR_REFERENCE_SPACE_TYPE_LOCAL
                                        : XR_REFERENCE_SPACE_TYPE_VIEW);
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace appSpace = XR_NULL_HANDLE;
    XRC(xrCreateReferenceSpace(session, &rsci, &appSpace));
    logf("using space: %s", hasStage ? "STAGE" : (hasLocal ? "LOCAL" : "VIEW"));

    // -- 6. swapchains -----------------------------------------------------
    rule("6. SWAPCHAINS");

    uint32_t fmtCount = 0;
    XRC(xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr));
    std::vector<int64_t> formats(fmtCount);
    XRC(xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, formats.data()));
    logf("swapchain formats offered: %u", fmtCount);
    for (auto f : formats) {
        const char* nm = "";
        switch ((DXGI_FORMAT)f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: nm = "R8G8B8A8_UNORM_SRGB"; break;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: nm = "B8G8R8A8_UNORM_SRGB"; break;
            case DXGI_FORMAT_R8G8B8A8_UNORM:      nm = "R8G8B8A8_UNORM"; break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:      nm = "B8G8R8A8_UNORM"; break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:  nm = "R16G16B16A16_FLOAT"; break;
            default: nm = "(other)"; break;
        }
        logf("    %lld  %s", (long long)f, nm);
    }
    // Prefer a plain UNORM so ClearRenderTargetView colours are predictable.
    int64_t chosenFmt = formats.empty() ? 0 : formats[0];
    for (auto f : formats) if ((DXGI_FORMAT)f == DXGI_FORMAT_R8G8B8A8_UNORM) { chosenFmt = f; break; }
    logf("chosen format: %lld", (long long)chosenFmt);

    struct EyeChain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t w = 0, h = 0;
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<ID3D11RenderTargetView*> rtvs;
    };
    std::vector<EyeChain> chains(viewCount);

    for (uint32_t i = 0; i < viewCount; i++) {
        XrSwapchainCreateInfo scci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        scci.format = chosenFmt;
        scci.sampleCount = 1;
        scci.width = vcv[i].recommendedImageRectWidth;
        scci.height = vcv[i].recommendedImageRectHeight;
        scci.faceCount = 1;
        scci.arraySize = 1;
        scci.mipCount = 1;
        XRC(xrCreateSwapchain(session, &scci, &chains[i].handle));
        chains[i].w = scci.width;
        chains[i].h = scci.height;

        uint32_t imgCount = 0;
        XRC(xrEnumerateSwapchainImages(chains[i].handle, 0, &imgCount, nullptr));
        chains[i].images.assign(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XRC(xrEnumerateSwapchainImages(chains[i].handle, imgCount, &imgCount,
                                       (XrSwapchainImageBaseHeader*)chains[i].images.data()));
        chains[i].rtvs.resize(imgCount, nullptr);
        for (uint32_t k = 0; k < imgCount; k++) {
            D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
            rtvd.Format = (DXGI_FORMAT)chosenFmt;
            rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            HRC(dev->CreateRenderTargetView(chains[i].images[k].texture, &rtvd, &chains[i].rtvs[k]));
        }
        logf("eye %u swapchain: %u x %u, %u images", i, scci.width, scci.height, imgCount);
    }

    // -- 7. frame loop -----------------------------------------------------
    rule("7. FRAME LOOP");
    logf("PUT THE HEADSET ON NOW.");
    logf("Expect: a solid colour per eye that CHANGES AS YOU TURN YOUR HEAD.");
    logf("        Left eye is tinted blue-ish, right eye red-ish.");
    logf("        Close one eye at a time to confirm they really are separate.");
    logf("");

    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, quit = false;
    LARGE_INTEGER freq, t0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    uint64_t frames = 0;
    bool loggedFov = false;
    XrTime lastDisplayTime = 0;
    double periodSum = 0.0;
    uint64_t periodCount = 0;

    while (!quit) {
        // events
        for (;;) {
            XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
            XrResult r = xrPollEvent(g_instance, &ev);
            if (r == XR_EVENT_UNAVAILABLE) break;
            if (XR_FAILED(r)) { logf("xrPollEvent failed: %s", xrStr(r).c_str()); quit = true; break; }
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* sc = (XrEventDataSessionStateChanged*)&ev;
                state = sc->state;
                const char* nm =
                    state == XR_SESSION_STATE_IDLE ? "IDLE" :
                    state == XR_SESSION_STATE_READY ? "READY" :
                    state == XR_SESSION_STATE_SYNCHRONIZED ? "SYNCHRONIZED" :
                    state == XR_SESSION_STATE_VISIBLE ? "VISIBLE" :
                    state == XR_SESSION_STATE_FOCUSED ? "FOCUSED" :
                    state == XR_SESSION_STATE_STOPPING ? "STOPPING" :
                    state == XR_SESSION_STATE_LOSS_PENDING ? "LOSS_PENDING" :
                    state == XR_SESSION_STATE_EXITING ? "EXITING" : "?";
                logf("session state -> %s", nm);
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
                    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XRC(xrBeginSession(session, &sbi));
                    running = true;
                    logf("session begun");
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    running = false;
                    xrEndSession(session);
                } else if (state == XR_SESSION_STATE_EXITING ||
                           state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                logf("instance loss pending");
                quit = true;
            }
        }

        if (!running) { Sleep(10); goto checkTime; }

        {
            XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState fs{XR_TYPE_FRAME_STATE};
            XRC(xrWaitFrame(session, &fwi, &fs));

            if (lastDisplayTime != 0 && fs.predictedDisplayTime > lastDisplayTime) {
                periodSum += (double)(fs.predictedDisplayTime - lastDisplayTime) / 1e6;
                periodCount++;
            }
            lastDisplayTime = fs.predictedDisplayTime;

            XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
            XRC(xrBeginFrame(session, &fbi));

            std::vector<XrCompositionLayerProjectionView> projViews(viewCount);
            XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            bool haveLayer = false;

            if (fs.shouldRender) {
                XrViewState vs{XR_TYPE_VIEW_STATE};
                XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
                vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                vli.displayTime = fs.predictedDisplayTime;
                vli.space = appSpace;
                uint32_t got2 = 0;
                std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
                XrResult lr = xrLocateViews(session, &vli, &vs, viewCount, &got2, views.data());

                if (XR_SUCCEEDED(lr) &&
                    (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {

                    if (!loggedFov) {
                        loggedFov = true;
                        logf("");
                        logf("---- PER-EYE FRUSTUM TANGENTS (radians) ----");
                        logf("     these are the inputs to the projection matrix in PORT-MAP 2.6");
                        for (uint32_t i = 0; i < viewCount; i++) {
                            const XrFovf& f = views[i].fov;
                            logf("  eye %u  angleLeft=%+.6f  angleRight=%+.6f  angleUp=%+.6f  angleDown=%+.6f",
                                 i, f.angleLeft, f.angleRight, f.angleUp, f.angleDown);
                            logf("         tan:      L=%+.6f      R=%+.6f      U=%+.6f      D=%+.6f",
                                 tanf(f.angleLeft), tanf(f.angleRight),
                                 tanf(f.angleUp), tanf(f.angleDown));
                            logf("         horiz FOV=%.2f deg  vert FOV=%.2f deg",
                                 (f.angleRight - f.angleLeft) * 57.2957795f,
                                 (f.angleUp - f.angleDown) * 57.2957795f);
                        }
                        if (viewCount >= 2) {
                            const XrFovf& a = views[0].fov;
                            const XrFovf& b = views[1].fov;
                            bool mirror = fabsf(a.angleLeft + b.angleRight) < 1e-4f &&
                                          fabsf(a.angleRight + b.angleLeft) < 1e-4f;
                            logf("");
                            logf("  eyes are symmetric mirrors of each other: %s", mirror ? "YES" : "NO");
                            logf("    -> the reference mod uses frustums[0] for BOTH eyes.");
                            logf("       That shortcut is %s on this headset.",
                                 mirror ? "harmless" : "*** A BUG we must not inherit ***");
                            float dx = views[0].pose.position.x - views[1].pose.position.x;
                            float dy = views[0].pose.position.y - views[1].pose.position.y;
                            float dz = views[0].pose.position.z - views[1].pose.position.z;
                            logf("  measured IPD: %.2f mm", sqrtf(dx*dx + dy*dy + dz*dz) * 1000.0f);
                        }
                        logf("");
                    }

                    // Head yaw drives the colour so tracking is visibly proven.
                    const XrQuaternionf& q = views[0].pose.orientation;
                    float yaw = atan2f(2.0f * (q.w * q.y + q.x * q.z),
                                       1.0f - 2.0f * (q.y * q.y + q.x * q.x));
                    float t = (yaw + 3.14159265f) / (2.0f * 3.14159265f); // 0..1

                    for (uint32_t i = 0; i < viewCount; i++) {
                        uint32_t idx = 0;
                        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        XRC(xrAcquireSwapchainImage(chains[i].handle, &ai, &idx));
                        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        wi.timeout = XR_INFINITE_DURATION;
                        XRC(xrWaitSwapchainImage(chains[i].handle, &wi));

                        // Left eye leans blue, right eye leans red. Both shift with yaw.
                        float col[4];
                        col[0] = (i == 1) ? 0.25f + 0.55f * t : 0.10f + 0.20f * t; // R
                        col[1] = 0.15f + 0.35f * (1.0f - t);                       // G
                        col[2] = (i == 0) ? 0.25f + 0.55f * t : 0.10f + 0.20f * t; // B
                        col[3] = 1.0f;
                        ctx->ClearRenderTargetView(chains[i].rtvs[idx], col);

                        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        XRC(xrReleaseSwapchainImage(chains[i].handle, &ri));

                        projViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                        projViews[i].pose = views[i].pose;
                        projViews[i].fov  = views[i].fov;
                        projViews[i].subImage.swapchain = chains[i].handle;
                        projViews[i].subImage.imageRect.offset = {0, 0};
                        projViews[i].subImage.imageRect.extent = {(int32_t)chains[i].w, (int32_t)chains[i].h};
                        projViews[i].subImage.imageArrayIndex = 0;
                    }

                    layer.space = appSpace;
                    layer.viewCount = viewCount;
                    layer.views = projViews.data();
                    haveLayer = true;
                }
            }

            const XrCompositionLayerBaseHeader* layers2[] = { (XrCompositionLayerBaseHeader*)&layer };
            XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
            fei.displayTime = fs.predictedDisplayTime;
            fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            fei.layerCount = haveLayer ? 1 : 0;
            fei.layers = haveLayer ? layers2 : nullptr;
            XRC(xrEndFrame(session, &fei));
            frames++;

            if (frames % 200 == 0) {
                logf("  ... %llu frames submitted", (unsigned long long)frames);
            }
        }

    checkTime:
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (elapsed > runSeconds) quit = true;
    }

    // -- 8. results --------------------------------------------------------
    rule("8. RESULTS");
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    logf("frames submitted    : %llu in %.2f s", (unsigned long long)frames, elapsed);
    if (elapsed > 0) logf("submitted frame rate: %.2f fps", frames / elapsed);
    if (periodCount) {
        double avgMs = periodSum / (double)periodCount;
        logf("mean predicted display period: %.4f ms  (%.2f Hz)", avgMs, 1000.0 / avgMs);
        logf("");
        logf("  NOTE: the project rules targets 72 Hz. Under the reference mod's AFR with");
        logf("        NO reprojection, the GAME would have to render at twice this");
        logf("        rate, %.0f fps, which is why stale-eye reprojection is a", 2.0 * (1000.0 / avgMs));
        logf("        required feature rather than an optimisation.");
    }

    logf("");
    logf("PROBE COMPLETE. Full log: docs\\RAW\\xr-probe.log");

    // cleanup
    for (auto& c : chains) {
        for (auto* r : c.rtvs) if (r) r->Release();
        if (c.handle) xrDestroySwapchain(c.handle);
    }
    if (appSpace) xrDestroySpace(appSpace);
    if (session) xrDestroySession(session);
    if (g_instance) xrDestroyInstance(g_instance);
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (chosen) chosen->Release();
    if (factory) factory->Release();
    if (g_log) fclose(g_log);
    return 0;
}
