// BUILD 248: EYE RECORDER (see EyeRec.h). Diagnostic only; off by default; never ships.
#include "EyeRec.h"

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Log.h"

namespace grwxr {
namespace eyerec {
namespace {

std::atomic<bool> g_on{false};
std::atomic<int>  g_width{640};
std::atomic<int>  g_every{1};
std::atomic<int>  g_max_mb{3000};

constexpr int kRing = 6;       // staging slots (each holds both eyes)
constexpr int kLag = 2;        // a slot is harvested when it is at least this many recordings old
constexpr int kMaxQueue = 12;  // frames waiting for the writer; more are dropped
constexpr int kGapPx = 4;      // black strip between the eyes

// ---- Present-thread state (only record() touches these) ----
bool     g_init_done = false, g_init_failed = false;
uint32_t g_rw = 0, g_rh = 0;
ID3D11Texture2D*          g_rt[2]  = {nullptr, nullptr};
ID3D11RenderTargetView*   g_rtv[2] = {nullptr, nullptr};
ID3D11Texture2D*          g_stg[kRing][2] = {};
uint64_t g_slot_frame[kRing] = {};
char     g_slot_meta[kRing][480] = {};
uint64_t g_n = 0;              // recordings issued
uint64_t g_tail = 0;           // next recording to harvest
uint64_t g_seen = 0;           // trusted frames seen (for every-N)
uint64_t g_dropped_ring = 0, g_dropped_queue = 0, g_busy_skips = 0;

// ---- writer ----
struct Job {
    std::vector<uint8_t> rgba;   // BGRA rows of the side-by-side image, tightly packed
    uint32_t w = 0, h = 0;
    uint64_t frame = 0;
    char meta[480] = {};
};
std::mutex              g_mu;
std::condition_variable g_cv;
std::deque<Job*>        g_q;
std::vector<Job*>       g_pool;
std::atomic<bool>       g_stop{false};     // the cap was reached or init failed: nothing more is recorded
std::atomic<bool>       g_quit{false};
std::atomic<uint64_t>   g_bytes{0}, g_written{0}, g_failed{0};
std::thread*            g_thread = nullptr;
std::wstring            g_dir;

void release_all() {
    for (int i = 0; i < 2; ++i) {
        if (g_rtv[i]) { g_rtv[i]->Release(); g_rtv[i] = nullptr; }
        if (g_rt[i])  { g_rt[i]->Release();  g_rt[i]  = nullptr; }
        for (int s = 0; s < kRing; ++s)
            if (g_stg[s][i]) { g_stg[s][i]->Release(); g_stg[s][i] = nullptr; }
    }
}

bool write_jpeg(IWICImagingFactory* f, const wchar_t* path, const uint8_t* bgr, UINT w, UINT h, UINT stride) {
    IWICStream* s = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* pb = nullptr;
    bool ok = false;
    do {
        if (FAILED(f->CreateStream(&s))) break;
        if (FAILED(s->InitializeFromFilename(path, GENERIC_WRITE))) break;
        if (FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc))) break;
        if (FAILED(enc->Initialize(s, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&fr, &pb))) break;
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        memset(&v, 0, sizeof(v));
        v.vt = VT_R4;
        v.fltVal = 0.85f;
        pb->Write(1, &opt, &v);
        if (FAILED(fr->Initialize(pb))) break;
        if (FAILED(fr->SetSize(w, h))) break;
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        if (FAILED(fr->SetPixelFormat(&pf))) break;
        if (FAILED(fr->WritePixels(h, stride, stride * h, const_cast<BYTE*>(bgr)))) break;
        if (FAILED(fr->Commit())) break;
        if (FAILED(enc->Commit())) break;
        ok = true;
    } while (false);
    if (pb)  pb->Release();
    if (fr)  fr->Release();
    if (enc) enc->Release();
    if (s)   s->Release();
    return ok;
}

void writer_main() {
    const HRESULT hrco = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic) {
        LOG_ERROR("eyerec: the WIC imaging factory could not be created; recording stops.");
        g_stop.store(true);
        if (SUCCEEDED(hrco)) CoUninitialize();
        return;
    }
    FILE* csv = nullptr;
    _wfopen_s(&csv, (g_dir + L"\\frames.csv").c_str(), L"ab");
    if (csv && ftell(csv) == 0) fprintf(csv, "frame,file_bytes,meta\n");
    std::vector<uint8_t> bgr;
    ULONGLONG last_log = GetTickCount64();
    for (;;) {
        Job* j = nullptr;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_for(lk, std::chrono::milliseconds(500), [] { return !g_q.empty() || g_quit.load(); });
            if (!g_q.empty()) { j = g_q.front(); g_q.pop_front(); }
            else if (g_quit.load()) break;
        }
        if (j) {
            const UINT w = j->w, h = j->h, stride = (w * 3u + 3u) & ~3u;
            bgr.assign((size_t)stride * h, 0);
            for (UINT y = 0; y < h; ++y) {
                const uint8_t* s = j->rgba.data() + (size_t)y * w * 4u;
                uint8_t* d = bgr.data() + (size_t)y * stride;
                for (UINT x = 0; x < w; ++x) { d[3 * x] = s[4 * x]; d[3 * x + 1] = s[4 * x + 1]; d[3 * x + 2] = s[4 * x + 2]; }
            }
            wchar_t name[96];
            swprintf_s(name, L"\\f%08llu.jpg", (unsigned long long)j->frame);
            const std::wstring path = g_dir + name;
            if (write_jpeg(wic, path.c_str(), bgr.data(), w, h, stride)) {
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                uint64_t sz = 0;
                if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) sz = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                g_bytes.fetch_add(sz);
                g_written.fetch_add(1);
                if (csv) fprintf(csv, "%llu,%llu,%s\n", (unsigned long long)j->frame, (unsigned long long)sz, j->meta);
            } else {
                g_failed.fetch_add(1);
            }
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_pool.push_back(j);
            }
            const uint64_t cap = (uint64_t)g_max_mb.load(std::memory_order_relaxed) * 1024ull * 1024ull;
            if (g_bytes.load() >= cap && !g_stop.exchange(true))
                LOG_INFO("eyerec: the %d MB disk cap was reached after %llu frames; recording stopped.",
                         g_max_mb.load(), (unsigned long long)g_written.load());
        }
        const ULONGLONG now = GetTickCount64();
        if (now - last_log >= 5000) {
            last_log = now;
            if (csv) fflush(csv);
            LOG_INFO("eyerec: written=%llu failed=%llu %llu MB | dropped ring=%llu queue=%llu busy-skips=%llu",
                     (unsigned long long)g_written.load(), (unsigned long long)g_failed.load(),
                     (unsigned long long)(g_bytes.load() >> 20), (unsigned long long)g_dropped_ring,
                     (unsigned long long)g_dropped_queue, (unsigned long long)g_busy_skips);
        }
    }
    if (csv) fclose(csv);
    wic->Release();
    if (SUCCEEDED(hrco)) CoUninitialize();
}

bool init(ID3D11DeviceContext* ctx, uint32_t sc_w, uint32_t sc_h) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    uint32_t rw = (uint32_t)std::clamp(g_width.load(std::memory_order_relaxed), 160, 1280) & ~1u;
    uint32_t rh = (uint32_t)((double)rw * (double)sc_h / (double)sc_w + 0.5) & ~1u;
    if (rh < 2) rh = 2;
    bool ok = true;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = rw; td.Height = rh; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    for (int e = 0; e < 2 && ok; ++e) {
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET; td.CPUAccessFlags = 0;
        ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_rt[e])) && SUCCEEDED(dev->CreateRenderTargetView(g_rt[e], nullptr, &g_rtv[e]));
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int s = 0; s < kRing && ok; ++s) ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_stg[s][e]));
    }
    dev->Release();
    if (!ok) { release_all(); return false; }
    g_rw = rw; g_rh = rh;

    g_dir = log::data_dir() + L"\\eyerec-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_thread = new std::thread(writer_main);
    LOG_INFO("eyerec: recording started: %ux%u per eye (side by side %ux%u), every %d trusted frame(s), cap %d MB, into %ls",
             rw, rh, 2 * rw + kGapPx, rh, g_every.load(), g_max_mb.load(), g_dir.c_str());
    return true;
}

// Moves the oldest due recording from the staging ring to the writer. Returns false when it is not ready.
bool harvest_one(ID3D11DeviceContext* ctx) {
    const int s = (int)(g_tail % kRing);
    D3D11_MAPPED_SUBRESOURCE m[2] = {};
    HRESULT h0 = ctx->Map(g_stg[s][0], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[0]);
    if (FAILED(h0)) { ++g_busy_skips; return false; }
    HRESULT h1 = ctx->Map(g_stg[s][1], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[1]);
    if (FAILED(h1)) { ctx->Unmap(g_stg[s][0], 0); ++g_busy_skips; return false; }

    Job* j = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_q.size() < (size_t)kMaxQueue) {
            if (!g_pool.empty()) { j = g_pool.back(); g_pool.pop_back(); }
            else j = new Job();
        }
    }
    if (j) {
        const uint32_t ow = 2 * g_rw + kGapPx;
        j->w = ow; j->h = g_rh; j->frame = g_slot_frame[s];
        memcpy(j->meta, g_slot_meta[s], sizeof(j->meta));
        j->rgba.assign((size_t)ow * g_rh * 4u, 0);
        for (uint32_t y = 0; y < g_rh; ++y) {
            uint8_t* d = j->rgba.data() + (size_t)y * ow * 4u;
            memcpy(d, (const uint8_t*)m[0].pData + (size_t)y * m[0].RowPitch, (size_t)g_rw * 4u);
            memcpy(d + (size_t)(g_rw + kGapPx) * 4u, (const uint8_t*)m[1].pData + (size_t)y * m[1].RowPitch, (size_t)g_rw * 4u);
        }
    } else {
        ++g_dropped_queue;
    }
    ctx->Unmap(g_stg[s][1], 0);
    ctx->Unmap(g_stg[s][0], 0);
    if (j) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_q.push_back(j);
        }
        g_cv.notify_one();
    }
    return true;
}

}  // namespace

void set_enabled(bool on) { g_on.store(on, std::memory_order_relaxed); }
void set_width(int px)    { g_width.store(std::clamp(px, 160, 1280), std::memory_order_relaxed); }
void set_every(int n)     { g_every.store(std::clamp(n, 1, 60), std::memory_order_relaxed); }
void set_max_mb(int mb)   { g_max_mb.store(std::clamp(mb, 16, 20000), std::memory_order_relaxed); }
bool enabled()            { return g_on.load(std::memory_order_relaxed) && !g_stop.load(std::memory_order_relaxed); }

void record(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const src[2],
            const float tan_h[2], const float tan_v[2],
            const float cx[2], const float cy[2], const float sx[2], const float sy[2],
            uint32_t sc_w, uint32_t sc_h, uint64_t frame, const char* meta) {
    if (!enabled() || !ctx || !src[0] || !src[1] || sc_w == 0 || sc_h == 0) return;
    if (++g_seen % (uint64_t)g_every.load(std::memory_order_relaxed) != 0) return;
    if (!g_init_done) {
        g_init_done = true;
        if (!init(ctx, sc_w, sc_h)) {
            g_init_failed = true;
            g_stop.store(true);
            LOG_ERROR("eyerec: the recording targets could not be created; recording is off for this run.");
            return;
        }
    }
    if (g_init_failed) return;

    // The ring is full of unharvested frames: the oldest is lost (the writer or the GPU fell behind).
    if (g_n - g_tail >= (uint64_t)kRing) { ++g_dropped_ring; ++g_tail; }

    const float k = (float)g_rw / (float)sc_w;
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int e = 0; e < 2; ++e) {
        ctx->ClearRenderTargetView(g_rtv[e], black);
        D3D11_VIEWPORT vp;
        vp.TopLeftX = k * (cx[e] - tan_h[e] * sx[e]);
        vp.TopLeftY = k * (cy[e] - tan_v[e] * sy[e]);
        vp.Width    = k * 2.0f * tan_h[e] * sx[e];
        vp.Height   = k * 2.0f * tan_v[e] * sy[e];
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* s = src[e];
        ctx->PSSetShaderResources(0, 1, &s);
        ctx->OMSetRenderTargets(1, &g_rtv[e], nullptr);
        ctx->Draw(3, 0);
    }
    const int slot = (int)(g_n % kRing);
    ctx->CopyResource(g_stg[slot][0], g_rt[0]);
    ctx->CopyResource(g_stg[slot][1], g_rt[1]);
    g_slot_frame[slot] = frame;
    strncpy_s(g_slot_meta[slot], meta ? meta : "", _TRUNCATE);
    ++g_n;

    // Harvest what is old enough that the GPU has finished it; stop at the first one that is not ready.
    while (g_tail + kLag <= g_n && g_tail < g_n) {
        if (!harvest_one(ctx)) break;
        ++g_tail;
    }
}

}  // namespace eyerec
}  // namespace grwxr
