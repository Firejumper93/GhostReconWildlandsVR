#include "VRMirror.h"
#include "HeadPose.h"
#include "Log.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <vector>
#include <cmath>

namespace grwxr {
namespace vr {
namespace {

XrInstance   g_instance   = XR_NULL_HANDLE;
XrSystemId   g_system     = XR_NULL_SYSTEM_ID;
XrSession    g_session    = XR_NULL_HANDLE;
XrSpace      g_space      = XR_NULL_HANDLE;
XrSpace      g_view_space = XR_NULL_HANDLE;

// BUILD 10a: one swapchain per eye. Each presented frame lands in one eye
// (alternating); the other eye re-submits its previous image.
XrSwapchain g_swapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
std::vector<XrSwapchainImageD3D11KHR> g_images[2];
uint32_t g_sc_w = 0, g_sc_h = 0;

// Per-eye submission state, render thread only. The stale eye's view must
// carry the pose and fov its image was RENDERED with, not the current ones;
// the compositor reprojects it to display time from there. valid goes false
// at start and after a session re-begin, and the next frame bootstraps both
// eyes from the same image.
struct EyeSub { XrPosef pose; XrFovf fov; bool valid; };
EyeSub g_eye[2] = {};

// BUILD 10i: private per-eye copy of each eye's last frame. The stale eye is
// re-copied from here into a FRESHLY ACQUIRED swapchain image every present,
// so both swapchains go through acquire/copy/release every frame. Before 10i
// the stale eye re-referenced its last released image without re-acquiring;
// the 10h merge measurement (character merges at ipd_scale ~ -11, i.e. the
// two copies sit ~15 deg apart, exactly the Quest 3 per-eye fov asymmetry)
// says the compositor displaces one of the two alternating submissions by
// the recommended-fov offset, and the re-reference path is the one thing
// that distinguishes them. Created at init on the game's device; null if
// creation failed, which falls back to the old re-reference behaviour.
ID3D11Texture2D* g_last[2] = {nullptr, nullptr};

// BUILD 10L: the canvas. The Quest Link runtime IGNORES the submitted
// per-view fov (proven by build 10k: submitting 99 instead of 45 degrees
// vertical changed nothing on screen) and stretches each eye's image across
// that eye's recommended asymmetric frustum. The frustums are mirrored, so
// each eye's content was displaced ~7 deg toward its temple: the constant
// binocular divergence (diplopia) behind every "doubled" report since build
// 9, and the stretch was the fisheye. Fix: allocate the swapchains as
// per-eye CANVASES shaped like the recommended frustum, place the game image
// angle-correct inside (per-eye integer offsets from the frustum tangents,
// black elsewhere), submit the recommended fov, which is then the truth.
// Created lazily on the first present with valid views, because the
// tangents come from xrLocateViews, which needs a frame time. Known
// limitation, deliberate: content rendered far from the pi/4 reference fov
// (scoped zoom) still shows at the wrong apparent size; that needs a
// scaling blit or the projection override, later.
bool     g_canvas_ready = false;
uint32_t g_content_w = 0, g_content_h = 0;   // game backbuffer size
int64_t  g_sc_format  = 0;
XrFovf   g_eyefov[2]  = {};
int      g_dst_x[2] = {0, 0}, g_dst_y[2] = {0, 0};
std::vector<ID3D11RenderTargetView*> g_rtv[2];

XrSessionState g_state_xr = XR_SESSION_STATE_UNKNOWN;
std::atomic<bool> g_active{false};
std::atomic<bool> g_failed{false};
// Render-thread only. Set when the session is unrecoverable (EXITING,
// LOSS_PENDING, or a failed re-begin); on_present becomes a no-op then.
bool g_dead = false;

// Deferred diagnostics: the render thread fills these, our own thread logs them.
std::atomic<bool> g_want_log{false};
char g_deferred[2048] = {};

// A staging copy of the backbuffer. The backbuffer may be multisampled or in a
// format the runtime will not accept directly, so we always go through our own
// texture rather than assuming we can CopyResource straight in.
ID3D11Texture2D* g_staging = nullptr;

// Build 10m: the ipd_scale the run started with (cfg value, or 1.0 with no
// cfg). Numpad * resets to this rather than a hardcoded 1.0, so reset means
// "back to my tuned value", not "back to untuned".
float g_ipd_default = 1.0f;

void note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_deferred, sizeof(g_deferred), fmt, ap);
    va_end(ap);
    g_want_log.store(true, std::memory_order_release);
}

// BUILD 10c: read the one user knob from GRWVR\grwxr.cfg at init. Missing
// file or key leaves the default. Lines are `key=value`; `#` starts a comment.
void load_config() {
    const std::wstring path = log::data_dir() + L"\\grwxr.cfg";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || !f) {
        LOG_INFO("VR: no grwxr.cfg, ipd_scale = 1.00 (Numpad 9/- adjusts, * resets)");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        float v = 0.0f;
        if (sscanf_s(line, " ipd_scale = %f", &v) == 1) {
            if (v < -2.0f) v = -2.0f;
            if (v >  2.0f) v =  2.0f;
            headpose::set_ipd_scale(v);
        }
    }
    fclose(f);
    g_ipd_default = headpose::ipd_scale();
    LOG_INFO("VR: grwxr.cfg read, ipd_scale = %.2f (Numpad 9/- adjusts, * resets to it)",
             headpose::ipd_scale());
}

const char* state_name(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE:         return "IDLE";
        case XR_SESSION_STATE_READY:        return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
        case XR_SESSION_STATE_STOPPING:     return "STOPPING";
        case XR_SESSION_STATE_EXITING:      return "EXITING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        default:                            return "?";
    }
}

bool fail(const char* what, XrResult r) {
    LOG_ERROR("VR: %s failed (XrResult %d). VR disabled, game unaffected.", what, (int)r);
    g_failed.store(true);
    return false;
}

// --- BUILD 8: head rotation -> camera (docs/HANDOFF.md "BUILD 8") -----------
//
// Each presented frame we locate the headset (VIEW space in LOCAL space at the
// predicted display time), take its orientation relative to a yaw-only
// reference captured on the first tracked pose, convert that rotation to the
// game's basis, and publish it through headpose:: for the camera hook to
// compose onto Camera+0x000. All of the XR-side maths lives here so the camera
// side stays engine-pure.

struct Quat { float x, y, z, w; };

// Hamilton product, the convention where R(a*b) = R(a) * R(b) in column form.
Quat qmul(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// Row-vector rotation matrix (the transpose of the usual column form): rows
// are the images of the basis vectors, composing as v * M, matching the
// engine's convention (docs/RE-notes.md, build 5).
void quat_to_rows(const Quat& q, float R[9]) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    R[0] = 1.0f - 2.0f * (yy + zz); R[1] = 2.0f * (xy + wz);        R[2] = 2.0f * (xz - wy);
    R[3] = 2.0f * (xy - wz);        R[4] = 1.0f - 2.0f * (xx + zz); R[5] = 2.0f * (yz + wx);
    R[6] = 2.0f * (xz + wy);        R[7] = 2.0f * (yz - wx);        R[8] = 1.0f - 2.0f * (xx + yy);
}

bool g_have_ref = false;
Quat g_ref_inv{0.0f, 0.0f, 0.0f, 1.0f};   // conjugate of the yaw-only reference

void update_head(XrSpaceLocationFlags flags, const XrQuaternionf& o) {
    if (!(flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    const Quat q{o.x, o.y, o.z, o.w};

    if (!g_have_ref) {
        // Yaw-only reference: a full-pose reference would bake the headset's
        // tilt at capture time into the world. Require TRACKED, not just
        // VALID, so a runtime filling in a guess cannot become the reference.
        if (!(flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) return;
        // Head forward in LOCAL space is q rotating (0,0,-1).
        const float fx = -2.0f * (q.x * q.z + q.w * q.y);
        const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        // Nearly straight up or down: the horizontal projection is too small
        // to define a yaw. Wait for a usable pose.
        if (fx * fx + fz * fz < 0.04f) return;
        const float psi = atan2f(-fx, -fz);   // yaw about XR +Y
        g_ref_inv = Quat{0.0f, -sinf(psi * 0.5f), 0.0f, cosf(psi * 0.5f)};
        g_have_ref = true;
        note("VR: head reference captured, yaw %.1f deg. Head tracking live.",
             psi * 57.29578f);
    }

    // Head rotation relative to the reference, expressed in the REFERENCE
    // frame: H = ref^-1 * current. The camera side applies it in the camera's
    // own frame (rows = H * base), so the reference frame must be identified
    // with the camera frame. The other order, current * ref^-1, is the same
    // rotation expressed in LOCAL space; composed camera-locally it pitches
    // and rolls about axes yawed away from the camera's whenever the head is
    // turned away from the reference (the "world tilts sideways" trap in
    // docs/HANDOFF.md).
    const Quat rel = qmul(g_ref_inv, q);

    float Rr[9];
    quat_to_rows(rel, Rr);

    // Change of basis into the game's axes. XR view axes: x right, y up,
    // z backward. Game (RE-notes.md, Ansel): x right, y forward, z up.
    // H_game = A * Rr * A^T with A rows (1,0,0),(0,0,-1),(0,1,0); as a signed
    // permutation that is H[i][j] = S[i] * S[j] * Rr[P[i]][P[j]].
    static const int   P[3] = {0, 2, 1};
    static const float S[3] = {1.0f, -1.0f, 1.0f};
    float Hg[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            Hg[i * 3 + j] = S[i] * S[j] * Rr[P[i] * 3 + P[j]];

    headpose::publish(Hg);
}

// Acquire one image of the given eye's canvas swapchain, clear it black,
// place the content at the eye's angle-correct offset, release it. Render
// thread, no logging (rule 8). Returns false if the copy did not happen; the
// image is released either way so the swapchain cannot wedge on a leaked
// acquire.
bool copy_into(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g_swapchains[eye], &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    bool copied = false;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(g_swapchains[eye], &wi))) {
        if (idx < g_rtv[eye].size() && g_rtv[eye][idx]) {
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            ctx->ClearRenderTargetView(g_rtv[eye][idx], black);
        }
        D3D11_BOX box{0, 0, 0, g_content_w, g_content_h, 1};
        ctx->CopySubresourceRegion(g_images[eye][idx].texture, 0,
                                   (UINT)g_dst_x[eye], (UINT)g_dst_y[eye], 0,
                                   src, 0, &box);
        copied = true;
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_swapchains[eye], &ri);
    return copied;
}

// BUILD 10L: create the per-eye canvas swapchains, sized so canvas pixels
// per tangent unit equal the content's pixels per tangent unit at the
// reference fov (fovy pi/4, the game's stationary value). The content then
// copies in 1:1 with no scaling and spans its true angular size. Called once
// from the present path (one-time creation; rule 8 tolerates it as init).
bool create_canvases(const d3d11::State& st, const XrView views[2]) {
    const float half_v = 0.3926991f;  // pi/8
    const float aspect = (float)g_content_w / (float)g_content_h;
    const float ppt    = (float)g_content_w / (2.0f * tanf(half_v) * aspect);

    float tw = 0.0f, th = 0.0f;
    for (int e = 0; e < 2; ++e) {
        const float w = tanf(views[e].fov.angleRight) - tanf(views[e].fov.angleLeft);
        const float h = tanf(views[e].fov.angleUp) - tanf(views[e].fov.angleDown);
        if (w > tw) tw = w;
        if (h > th) th = h;
    }
    g_sc_w = ((uint32_t)(tw * ppt + 0.5f) + 1u) & ~1u;
    g_sc_h = ((uint32_t)(th * ppt + 0.5f) + 1u) & ~1u;

    for (int eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo scci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        scci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        scci.format      = g_sc_format;
        scci.sampleCount = 1;
        scci.width       = g_sc_w;
        scci.height      = g_sc_h;
        scci.faceCount   = 1;
        scci.arraySize   = 1;
        scci.mipCount    = 1;
        if (XR_FAILED(xrCreateSwapchain(g_session, &scci, &g_swapchains[eye])))
            return false;

        uint32_t ic = 0;
        xrEnumerateSwapchainImages(g_swapchains[eye], 0, &ic, nullptr);
        g_images[eye].assign(ic, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(xrEnumerateSwapchainImages(g_swapchains[eye], ic, &ic,
                                                 (XrSwapchainImageBaseHeader*)g_images[eye].data())))
            return false;
        g_rtv[eye].assign(ic, nullptr);
        for (uint32_t i = 0; i < ic; ++i)
            st.device->CreateRenderTargetView(g_images[eye][i].texture, nullptr, &g_rtv[eye][i]);

        g_eyefov[eye] = views[eye].fov;
        const float tanL = tanf(views[eye].fov.angleLeft);
        const float tanR = tanf(views[eye].fov.angleRight);
        const float tanU = tanf(views[eye].fov.angleUp);
        const float tanD = tanf(views[eye].fov.angleDown);
        // Canvas column/row where the content's optical center (angle 0)
        // belongs; content is centered on it. Row 0 is angleUp.
        const float cx = (float)g_sc_w * (0.0f - tanL) / (tanR - tanL);
        const float cy = (float)g_sc_h * tanU / (tanU - tanD);
        int dx = (int)(cx + 0.5f) - (int)g_content_w / 2;
        int dy = (int)(cy + 0.5f) - (int)g_content_h / 2;
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;
        if (dx > (int)(g_sc_w - g_content_w)) dx = (int)(g_sc_w - g_content_w);
        if (dy > (int)(g_sc_h - g_content_h)) dy = (int)(g_sc_h - g_content_h);
        g_dst_x[eye] = dx;
        g_dst_y[eye] = dy;
    }
    g_canvas_ready = true;
    note("VR: 10L canvas %ux%u, content %ux%u at L(%d,%d) R(%d,%d)",
         g_sc_w, g_sc_h, g_content_w, g_content_h,
         g_dst_x[0], g_dst_y[0], g_dst_x[1], g_dst_y[1]);
    return true;
}

}  // namespace

bool init(const d3d11::State& st) {
    if (g_session != XR_NULL_HANDLE || g_failed.load()) return g_session != XR_NULL_HANDLE;
    if (!st.device || !st.context) return false;

    LOG_INFO("VR: initialising OpenXR on the game's D3D11 device");
    load_config();

    // BUILD 10d: two foreign implicit API layers sit inside every call we
    // make (flagged by the session 1 probe, confirmed enabled in the
    // registry): OBS Mirror and Virtual Desktop's Oculus compatibility shim.
    // The mod needs neither, and both are suspects for the offset-independent
    // monocular doubling. Each honors a disable_environment variable that the
    // loader reads at instance creation, so setting them here suppresses the
    // layers for this process only.
    SetEnvironmentVariableA("DISABLE_XR_APILAYER_NOVENDOR_OBSMirror", "1");
    SetEnvironmentVariableA("DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY", "1");
    uint32_t lc = 0;
    xrEnumerateApiLayerProperties(0, &lc, nullptr);
    std::vector<XrApiLayerProperties> lps(lc, {XR_TYPE_API_LAYER_PROPERTIES});
    if (lc) xrEnumerateApiLayerProperties(lc, &lc, lps.data());
    LOG_INFO("VR: %u API layer(s) visible after suppression%s", lc,
             lc ? ":" : ". OBS Mirror and VD compatibility are out of the path.");
    for (uint32_t i = 0; i < lc; ++i) LOG_INFO("VR:   %s", lps[i].layerName);

    // --- extensions ---
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    if (n) xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    bool has_d3d11 = false;
    for (auto& e : exts) {
        if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) has_d3d11 = true;
    }
    if (!has_d3d11) {
        LOG_ERROR("VR: runtime does not expose XR_KHR_D3D11_enable. Cannot continue.");
        g_failed.store(true);
        return false;
    }

    const char* enabled[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    strcpy_s(ici.applicationInfo.applicationName, "GRW-XR");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "AnvilNext");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XrResult r = xrCreateInstance(&ici, &g_instance);
    if (XR_FAILED(r)) return fail("xrCreateInstance", r);

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &ip);
    LOG_INFO("VR: runtime = %s", ip.runtimeName);

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(g_instance, &sgi, &g_system);
    if (XR_FAILED(r)) {
        LOG_ERROR("VR: no headset available (XrResult %d). Is Quest Link connected?", (int)r);
        g_failed.store(true);
        return false;
    }

    // The runtime dictates which adapter. If the game's device is on a different
    // one we must know, because sharing textures across adapters will not work.
    PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&pfn);
    if (pfn) {
        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (XR_SUCCEEDED(pfn(g_instance, g_system, &req))) {
            LOG_INFO("VR: runtime wants adapter LUID %08lX:%08lX",
                     (unsigned long)req.adapterLuid.HighPart,
                     (unsigned long)req.adapterLuid.LowPart);
        }
    }

    // --- session on the GAME's device ---
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = st.device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    r = xrCreateSession(g_instance, &sci, &g_session);
    if (XR_FAILED(r)) return fail("xrCreateSession", r);
    LOG_INFO("VR: session created on the game's own D3D11 device");

    // LOCAL, not STAGE: for a fixed screen in front of the viewer we want a
    // seated origin, not a room-scale floor origin.
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    r = xrCreateReferenceSpace(g_session, &rsci, &g_space);
    if (XR_FAILED(r)) return fail("xrCreateReferenceSpace", r);

    // Build 8: a VIEW space handle so xrLocateSpace(VIEW, LOCAL, t) yields the
    // head pose directly, without pulling per-eye view state we do not need yet.
    XrReferenceSpaceCreateInfo vsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vsci.poseInReferenceSpace.orientation.w = 1.0f;
    r = xrCreateReferenceSpace(g_session, &vsci, &g_view_space);
    if (XR_FAILED(r)) return fail("xrCreateReferenceSpace(VIEW)", r);

    // --- swapchain sized to the game's backbuffer ---
    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &fc, nullptr);
    std::vector<int64_t> formats(fc);
    xrEnumerateSwapchainFormats(g_session, fc, &fc, formats.data());
    int64_t chosen = formats.empty() ? (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM : formats[0];
    for (auto f : formats) {
        if ((DXGI_FORMAT)f == st.format) { chosen = f; break; }
    }

    // BUILD 10L: swapchain creation is DEFERRED to the first present with
    // valid views. The canvas dimensions and per-eye placement offsets need
    // the frustum tangents, and xrLocateViews only yields those once frame
    // timing exists. Store what deferred creation needs.
    g_sc_format    = chosen;
    g_content_w    = st.width;
    g_content_h    = st.height;
    g_canvas_ready = false;
    LOG_INFO("VR: canvas swapchains deferred to the first frame (10L), format %lld",
             (long long)chosen);

    // BUILD 10i: the per-eye last-frame copies, same size/format as the
    // swapchain images. Plain default-usage textures, no bind flags needed
    // for CopyResource in either direction.
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width  = st.width;
        td.Height = st.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = st.format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        for (int eye = 0; eye < 2; ++eye) {
            if (FAILED(st.device->CreateTexture2D(&td, nullptr, &g_last[eye]))) {
                g_last[eye] = nullptr;
            }
        }
        if (g_last[0] && g_last[1]) {
            LOG_INFO("VR: 10i last-frame textures created; stale eye re-copied every present.");
        } else {
            LOG_INFO("VR: 10i last-frame texture creation FAILED; stale eye falls back to re-reference.");
        }
    }

    // --- begin session ---
    XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    // Session must reach READY before begin; poll briefly for the event.
    for (int i = 0; i < 400 && g_state_xr != XR_SESSION_STATE_READY; ++i) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                g_state_xr = ((XrEventDataSessionStateChanged*)&ev)->state;
                LOG_INFO("VR: session state -> %s", state_name(g_state_xr));
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (g_state_xr != XR_SESSION_STATE_READY) Sleep(25);
    }
    if (g_state_xr != XR_SESSION_STATE_READY) {
        LOG_ERROR("VR: session never reached READY. Is the headset awake and on?");
        g_failed.store(true);
        return false;
    }
    r = xrBeginSession(g_session, &sbi);
    if (XR_FAILED(r)) return fail("xrBeginSession", r);

    LOG_INFO("VR: session begun. Submitting the game frame to the headset.");
    LOG_INFO("VR: build 10L, angle-correct canvas placement per eye (fov-immutable fix).");
    LOG_INFO("VR: Home recenters. Numpad + and - step the eye separation by");
    LOG_INFO("VR: 0.05, Numpad * resets it to 1.00; each change is logged.");
    LOG_INFO("VR: Judge in the headset; the desktop mirror vibrates by design.");
    g_active.store(true);
    return true;
}

void on_present(const d3d11::State& st) {
    // Build 8.1: the session survives a doff. Taking the headset off sends
    // STOPPING; build 8 stopped polling events there, never called
    // xrEndSession, and so left the runtime showing a frozen frame with no way
    // back. Now STOPPING parks the session (xrEndSession, stop submitting) but
    // events keep draining every frame, and the READY that arrives when the
    // headset is put back on re-begins the session with a FRESH head
    // reference, since the headset has physically moved.
    if (g_session == XR_NULL_HANDLE || g_dead) return;
    if (!st.device || !st.context || !st.swapchain) return;

    // Drain session events cheaply, active or parked.
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            g_state_xr = ((XrEventDataSessionStateChanged*)&ev)->state;
            note("VR: session state -> %s", state_name(g_state_xr));
            if (g_state_xr == XR_SESSION_STATE_STOPPING) {
                headpose::disable();
                g_active.store(false);
                xrEndSession(g_session);
            } else if (g_state_xr == XR_SESSION_STATE_READY &&
                       !g_active.load(std::memory_order_relaxed)) {
                XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
                sbi.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_session, &sbi))) {
                    g_have_ref = false;
                    // Build 10a: the pre-doff eye images and poses are stale
                    // in every sense; bootstrap both eyes afresh.
                    g_eye[0].valid = false;
                    g_eye[1].valid = false;
                    g_active.store(true);
                } else {
                    g_dead = true;
                }
            } else if (g_state_xr == XR_SESSION_STATE_EXITING ||
                       g_state_xr == XR_SESSION_STATE_LOSS_PENDING) {
                headpose::disable();
                g_active.store(false);
                g_dead = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // Build 10a.1: the runtime recentred (user long-press recenter).
            // Both the yaw reference and the stored per-eye poses are in the
            // old space; recapture everything.
            g_have_ref = false;
            g_eye[0].valid = false;
            g_eye[1].valid = false;
            note("VR: runtime recenter. Head reference will re-capture.");
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    // Build 10a.1: manual recenter on the Home key, edge-triggered. The head
    // reference was captured from wherever the headset pointed at launch
    // (often the desk), which bakes that angle into the camera as a constant
    // offset. Home re-captures it at the current head pose: look where you
    // want "forward" to be and press it. Only the camera compose reference
    // resets; the layer poses are raw LOCAL space and unaffected.
    {
        static bool s_was_down = false;
        const bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (down && !s_was_down) {
            g_have_ref = false;
            note("VR: recenter requested (Home). Head reference will re-capture.");
        }
        s_was_down = down;
    }

    // Build 10c: live eye-separation tuning, same edge-triggered pattern.
    // Numpad 9 steps ipd_scale +0.05, Numpad - steps -0.05, Numpad * resets
    // to the startup value (cfg default).
    // Build 10m: the 10h coarse merge-hunt keys are REMOVED. The merge
    // measurement is done, and VK_RETURN is global, so any Enter press
    // (menus, chat) silently bumped depth by +0.5 (hazard 15; it fired in
    // the 20:44 run, kicking a tuned -0.50 back to 0.00 right before quit).
    // Increase moved from Numpad + to Numpad 9 (user request), and the
    // sweep range [-20, +20] tightened back to [-2, +2].
    {
        static bool s_add = false, s_sub = false, s_mul = false;
        const bool add = (GetAsyncKeyState(VK_NUMPAD9)  & 0x8000) != 0;
        const bool sub = (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
        const bool mul = (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) != 0;
        float s = headpose::ipd_scale();
        bool changed = false;
        if (add && !s_add) { s += 0.05f; changed = true; }
        if (sub && !s_sub) { s -= 0.05f; changed = true; }
        if (mul && !s_mul) { s = g_ipd_default; changed = true; }
        if (changed) {
            if (s < -2.0f) s = -2.0f;
            if (s >  2.0f) s =  2.0f;
            headpose::set_ipd_scale(s);
            note("VR: ipd scale = %.2f (persist it as ipd_scale=%.2f in grwxr.cfg)", s, s);
        }
        s_add = add; s_sub = sub; s_mul = mul;
    }
    if (g_dead || !g_active.load(std::memory_order_relaxed)) return;

    // Build 10b.1: exactly one tag pop per present, mirroring the one push
    // per built frame on the camera side. -1 means no camera write built this
    // frame (menus, loading, VR-inactive stretches): the image is mono.
    const int built_eye = headpose::pop_eye_tag();

    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g_session, &fwi, &fs))) return;

    // Build 8: head pose for this frame's predicted display time, published to
    // the camera hook. No logging here (rule 8); note() defers.
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    bool head_ori_ok = false;
    XrQuaternionf head_ori{0.0f, 0.0f, 0.0f, 1.0f};
    if (XR_SUCCEEDED(xrLocateSpace(g_view_space, g_space,
                                   fs.predictedDisplayTime, &loc))) {
        update_head(loc.locationFlags, loc.pose.orientation);
        // BUILD 10j: this is the orientation the frame's content is composed
        // with (both eyes share it); the submit path attaches it to both views.
        if (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
            head_ori = loc.pose.orientation;
            head_ori_ok = true;
        }
    }

    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g_session, &fbi))) return;

    // BUILD 10a (docs/HANDOFF.md "BUILD 10" step A): AER submission machinery.
    // Each presented frame the backbuffer is copied into ONE eye's swapchain,
    // alternating; that eye's view gets this frame's located pose and fov. The
    // other eye re-submits its previous image with the pose and fov stored
    // when that image was fresh, and the compositor reprojects both views to
    // display time. The camera is NOT offset yet: both eyes show the same
    // viewpoint, so the headset should look identical to build 9. That
    // sameness is this step's acceptance test.
    bool submitted = false;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pviews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    };

    if (fs.shouldRender) {
        // Per-eye poses for the layer. The xrLocateSpace(VIEW) head-pose path
        // above stays for the camera compose; this is the layer's anchor.
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime           = fs.predictedDisplayTime;
        vli.space                 = g_space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t vc = 0;
        const bool views_ok =
            XR_SUCCEEDED(xrLocateViews(g_session, &vli, &vs, 2, &vc, views)) &&
            vc == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);

        // BUILD 10b: the measured IPD feeds the camera-side eye offset.
        if (views_ok) {
            const float dx = views[1].pose.position.x - views[0].pose.position.x;
            const float dy = views[1].pose.position.y - views[0].pose.position.y;
            const float dz = views[1].pose.position.z - views[0].pose.position.z;
            headpose::publish_ipd(sqrtf(dx * dx + dy * dy + dz * dz));

            // BUILD 10j one-shot diagnostic: the runtime's per-eye view
            // orientations and the angle between them. Nonzero cant here is
            // the measured cause of the constant angular doubling.
            static bool s_cant_logged = false;
            if (!s_cant_logged) {
                s_cant_logged = true;
                const XrQuaternionf& a = views[0].pose.orientation;
                const XrQuaternionf& b = views[1].pose.orientation;
                float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
                if (dot < 0.0f) dot = -dot;
                if (dot > 1.0f) dot = 1.0f;
                const float cant_deg = 2.0f * acosf(dot) * 57.29578f;
                note("VR: 10j view ori L(%.4f,%.4f,%.4f,%.4f) R(%.4f,%.4f,%.4f,%.4f) cant %.3f deg",
                     a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, cant_deg);
            }
        }

        ID3D11Texture2D* back = nullptr;
        if (views_ok &&
            SUCCEEDED(st.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
            // BUILD 10b.1: the eye identity comes from the FIFO tag pushed by
            // the camera hook when this frame was BUILT, never from frame
            // parity: the build-to-present pipeline depth is unknown and
            // parity guessing swapped the eyes. A mono frame (no tag)
            // invalidates both eyes so the bootstrap below feeds the same
            // image to both with their current located poses.
            int fresh = built_eye;
            if (fresh < 0) {
                g_eye[0].valid = false;
                g_eye[1].valid = false;
                fresh = 0;
            }
            const int stale = fresh ^ 1;

            // BUILD 10L: one-time canvas creation, needs the live view fovs.
            if (!g_canvas_ready && !create_canvases(st, views)) {
                g_dead = true;
                note("VR: 10L canvas creation FAILED, VR submission disabled");
            }

            if (g_canvas_ready && copy_into(fresh, st.context, back)) {
                // BUILD 10j: both eyes' content is rendered with ONE shared
                // camera orientation (the game camera composed with the head
                // pose). Submitting each view with its own xrLocateViews
                // orientation asserts a per-eye cant the content does not
                // have; the compositor then shows the eyes diverged by the
                // cant difference: a constant angular doubling that ignores
                // the ipd knob (measured ~15 deg total, builds 10g/10h).
                // Attach the shared render orientation to both views;
                // positions stay per-eye. Zero runtime cant makes this a
                // no-op by construction.
                XrPosef fresh_pose = views[fresh].pose;
                XrPosef stale_boot = views[stale].pose;
                if (head_ori_ok) {
                    fresh_pose.orientation = head_ori;
                    stale_boot.orientation = head_ori;
                }
                // BUILD 10L: the recommended per-eye fov is now the TRUTH,
                // because the canvas is shaped to it and the content sits
                // angle-correct inside. (10k submitted this fov as a lie and
                // proved the runtime ignores the field either way.)
                g_eye[fresh] = {fresh_pose, views[fresh].fov, true};
                if (g_last[fresh]) st.context->CopyResource(g_last[fresh], back);

                // First frame after start or re-begin: the other eye has no
                // image yet, so feed it this same frame with its own located
                // pose. From the next frame on the eyes alternate.
                if (!g_eye[stale].valid) {
                    if (copy_into(stale, st.context, back)) {
                        g_eye[stale] = {stale_boot, views[stale].fov, true};
                        if (g_last[stale]) st.context->CopyResource(g_last[stale], back);
                    }
                } else if (g_last[stale]) {
                    // BUILD 10i: re-copy the stale eye's own last frame into a
                    // freshly acquired image so this submission is a normal
                    // release, not a re-reference. Pose and fov stay the stored
                    // ones (the frame's render pose). On copy failure the old
                    // re-reference behaviour still applies implicitly.
                    copy_into(stale, st.context, g_last[stale]);
                }

                if (g_eye[stale].valid) {
                    for (int i = 0; i < 2; ++i) {
                        pviews[i].pose = g_eye[i].pose;
                        pviews[i].fov  = g_eye[i].fov;
                        pviews[i].subImage.swapchain = g_swapchains[i];
                        pviews[i].subImage.imageRect.offset = {0, 0};
                        pviews[i].subImage.imageRect.extent = {(int32_t)g_sc_w, (int32_t)g_sc_h};
                        pviews[i].subImage.imageArrayIndex  = 0;
                    }
                    layer.layerFlags = 0;
                    layer.space      = g_space;
                    layer.viewCount  = 2;
                    layer.views      = pviews;
                    submitted = true;
                }
            }
        }
        if (back) back->Release();
    }

    const XrCompositionLayerBaseHeader* layers[] = { (XrCompositionLayerBaseHeader*)&layer };
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime          = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount           = submitted ? 1 : 0;
    fei.layers               = submitted ? layers : nullptr;
    xrEndFrame(g_session, &fei);
}

bool active() { return g_active.load(); }

void drain_log() {
    if (!g_want_log.exchange(false, std::memory_order_acq_rel)) return;
    LOG_INFO("%s", g_deferred);
}

void shutdown() {
    headpose::disable();
    g_active.store(false);
    if (g_staging)    { g_staging->Release(); g_staging = nullptr; }
    for (int eye = 0; eye < 2; ++eye) {
        if (g_last[eye]) { g_last[eye]->Release(); g_last[eye] = nullptr; }
    }
    for (int eye = 0; eye < 2; ++eye) {
        for (auto* r : g_rtv[eye]) {
            if (r) r->Release();
        }
        g_rtv[eye].clear();
        if (g_swapchains[eye]) {
            xrDestroySwapchain(g_swapchains[eye]);
            g_swapchains[eye] = XR_NULL_HANDLE;
        }
        g_canvas_ready = false;
    }
    if (g_view_space) { xrDestroySpace(g_view_space);    g_view_space = XR_NULL_HANDLE; }
    if (g_space)      { xrDestroySpace(g_space);         g_space = XR_NULL_HANDLE; }
    if (g_session)   { xrDestroySession(g_session);     g_session = XR_NULL_HANDLE; }
    if (g_instance)  { xrDestroyInstance(g_instance);   g_instance = XR_NULL_HANDLE; }
}

}  // namespace vr
}  // namespace grwxr
