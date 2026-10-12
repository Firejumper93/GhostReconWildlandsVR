// Anatomy.h - BUILD 241: the "frame anatomy" capture. OBSERVATION ONLY.
//
// THE QUESTION. The mod alternates eyes (one fresh eye per present). Rendering
// both eyes every frame needs the engine's per-frame work split by eye, and the
// engine draws its scene on D3D11 DEFERRED contexts (command lists). Before
// deciding where a second eye could be injected, the shape of one frame has to
// be measured: which threads draw, how many command lists are finished and
// executed per present, which render-target sizes the deferred and the
// immediate contexts bind, and which constant buffers receive matrix-looking
// writes, how often, from which kind of context.
//
// WHAT IT DOES. A bounded capture of ONE window of 240 presents, starting 30 s
// after the first stereo layer was submitted, then it stops for good (one
// window per run). The hooks are the same E9 detours as DrawHook.cpp (installed
// 20 s after the first stereo frame, only when every target decodes cleanly),
// they write into PREALLOCATED static tables and a fixed-size event buffer with
// relaxed atomics, and a background thread writes GRWVR\anatomy-<pid>.txt once
// the window closes. Nothing in the engine's data is changed and the alternate
// eye behaviour is untouched.
//
// RULE 8. The taps log nothing, take no lock and allocate nothing. The ONE
// deviation, called out on purpose: on the FIRST sight of a buffer, a render
// target view or a context the tap makes a handful of plain COM getter calls
// (GetType, GetDesc, GetResource+Release) to learn its bind flags or size, and
// caches the answer in a lock-free table keyed by pointer. A recycled pointer
// can therefore be classified from a stale cache entry (risk accepted: the
// constant-buffer read is size-bounded and SEH-guarded).
//
// cfg: anatomy_capture = 0 disables it. The code default is ON.
//
// BUILD 246: THE HOOK SET IS SHARED WITH THE CONSTANT-BUFFER CONTENT CAPTURE
// (CbDump.h, cfg cbdump_capture, code default OFF). The hooks are installed when
// EITHER anatomy_capture OR cbdump_capture is on; with both off nothing is
// installed and nothing is read. The anatomy WINDOW opens only when
// anatomy_capture is on; the cbdump window is its own state machine. The taps are
// gated by g_gate, one relaxed load: bit 1 = the anatomy window is open, bit 2 =
// the cbdump window is open. A tap serves whichever windows are open.

#pragma once

#include <atomic>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

namespace grwxr {
namespace anatomy {

// 2 = capturing. The anatomy window's own state (0 idle, 1 hooked, 2 capturing, ...).
extern std::atomic<int> g_state;

// BUILD 246: bit 1 = the anatomy window is open, bit 2 = the cbdump window is open.
// BUILD 270: bit 4 = gtrec is recording (GtRec.h; its Map/Unmap/UpdateSubresource taps).
// The hook set is installed for gtrec as well when cfg gtrec and gtrec_cb are on.
// Read by every hook before it does anything else; a tap does work only when its
// bit is set.
extern std::atomic<int> g_gate;
inline bool capturing() { return g_gate.load(std::memory_order_relaxed) != 0; }
void gate_set(int bits);
void gate_clear(int bits);

// BUILD 246, for CbDump.cpp. hooks_status: 0 = not installed yet, 1 = installed, -1 = the
// install was refused (nothing was written). first_stereo_ms: GetTickCount64 at the first
// stereo layer, 0 before it. context_kind: 0 immediate, 1 deferred, 2 unknown, 3 an
// immediate-class call made by the runtime while it replays a command list.
// constant_buffer_bytes: the ByteWidth if the resource is a buffer whose bind flags are
// EXACTLY D3D11_BIND_CONSTANT_BUFFER, else 0 (cached per pointer; the first sight of a
// resource makes the same plain COM getter calls the anatomy taps always made).
int      hooks_status();
uint64_t first_stereo_ms();
int      context_kind(ID3D11DeviceContext* ctx);
uint32_t constant_buffer_bytes(ID3D11Resource* res);
void     table_pressure(uint32_t* ctx_full, uint32_t* cache_full);   // the shared lookup tables' overflow counters

void set_enabled(bool on);   // cfg anatomy_capture, any thread
bool enabled();

// Render thread, after xrEndFrame submitted a projection layer. One relaxed load
// per frame, and one store the first time.
void on_stereo_submit();

// Bracket the REAL ExecuteCommandList on the calling thread (only when capturing()
// was true at the start of the call), so immediate-class calls the runtime makes
// while replaying a list are labelled "rpl" instead of passing for the game's own.
void exec_enter();
void exec_leave();

// Render thread, from the Present hook, after the frame counter was bumped.
// Records the Present boundary while a window is open and closes the window.
void on_present(unsigned long long frames);

// Init thread, once a second. Installs the hooks, opens the window, and hands
// the finished window to the dump thread.
void tick();

// The taps. Called from the detours in DrawHook.cpp on whatever thread the
// engine calls the D3D function from. All are gated by capturing().
void tap_draw(ID3D11DeviceContext* ctx);
void tap_rt(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
            ID3D11DepthStencilView* dsv);
void tap_finish(ID3D11DeviceContext* ctx, BOOL restore, ID3D11CommandList* cl,
                HRESULT hr, uint64_t t0);
void tap_execute(ID3D11DeviceContext* ctx, ID3D11CommandList* cl, BOOL restore,
                 uint64_t t0);
void tap_map(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub,
             D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE* ms, HRESULT hr);
void tap_unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub);
void tap_update(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub,
                const D3D11_BOX* box, const void* src);

}  // namespace anatomy
}  // namespace grwxr
