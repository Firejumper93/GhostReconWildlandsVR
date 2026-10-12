// CbDump.h - BUILD 246: the constant-buffer CONTENT capture. OBSERVATION ONLY.
//
// THE QUESTION. Build 241's frame anatomy counted which D3D11 constant buffers are
// written (per-object buffers of 320 and 144 bytes on the deferred worker contexts,
// two buffers of 1008 and 320 bytes on the immediate context) but never looked at
// WHAT was written. Whether a second eye can be produced by replaying the engine's
// command lists with different view constants depends on two facts this build reads
// out of the data: where the camera view and projection matrices live, and whether
// the per-object buffers carry a precomputed world*view*projection (then replay with
// changed view constants cannot work) or only the world matrix (then it can).
//
// WHAT IT DOES. ONE window of 120 presents, opened 30 s after the first stereo layer
// (the same trigger as the anatomy window; the census opens at 50 s, and this window
// is force-closed 18 s after it opens so the two never overlap). In the window the
// Map/Unmap/UpdateSubresource taps of Anatomy.cpp also hand every write to a buffer
// whose bind flags are EXACTLY D3D11_BIND_CONSTANT_BUFFER to this module, which copies
// up to the first 1024 bytes of the written data into PREALLOCATED arenas:
//   arena I  immediate context, buffers of 1008 or 320 bytes, EVERY write
//   arena D  deferred contexts, buffers of 320 or 144 bytes, the first 8 writes per
//            buffer per row plus every 64th after that
//   arena M  everything else, the first 2 per buffer per row plus every 128th
// each with a hard slot budget and a per-row quota; what is not stored is counted and
// reported. At every Present the render thread also copies the mod's own ground truth
// (CbDumpFormat.h GtRow): the player camera's pose matrix (Camera+0x000), the engine's
// matrices at Camera+0x420..+0x660, the fov/near/far scalars, the published fov and
// clip, and the last four proj[2] (persp2) calls with a read-back of their output.
// A background thread then writes GRWVR\cbdump-<pid>.bin (the raw records, layout in
// CbDumpFormat.h) and GRWVR\cbdump-<pid>.txt (the analysis, CbAnalysis.h, ending in a
// VERDICT section).
//
// RULE 8. The taps and the Present sampler log nothing, take no lock and allocate
// nothing: the arenas are VirtualAlloc'ed once by the init thread before the window
// opens, every reservation is one relaxed fetch_add, every read of engine memory is
// SEH-guarded, and the dump thread does all file I/O and analysis. The ONE deviation
// is inherited from Anatomy.h: the first sight of a buffer or a context makes a few
// plain COM getter calls to learn its bind flags and kind.
//
// DEPENDENCY. The taps ride the Anatomy hook set (Anatomy.h). cbdump_capture = 1
// installs those hooks by itself; anatomy_capture may stay 0. With both off, nothing
// is installed.
//
// cfg: cbdump_capture = 1 turns it on for the run. The code default is OFF.

#pragma once

#include <atomic>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

namespace grwxr {
namespace cbdump {

void set_enabled(bool on);   // cfg cbdump_capture, any thread
bool enabled();

// 0 idle, 2 capturing, 3 stopped, 4 dumping, 5 done, 6 failed.
extern std::atomic<int> g_state;
inline bool capturing() { return g_state.load(std::memory_order_relaxed) == 2; }

// Init thread, once a second. Opens the window, closes it, hands it to the dump thread.
void tick();

// Render thread, from the Present hook, after the frame counter was bumped.
void on_present(unsigned long long frames);

// The taps. Called from Anatomy.cpp's taps on whatever thread the engine used, only
// while the cbdump window is open.
void tap_map(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, D3D11_MAP type,
             const D3D11_MAPPED_SUBRESOURCE* ms, HRESULT hr);
void tap_unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub);
void tap_update(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, const D3D11_BOX* box,
                const void* src);

// Camera probe (CameraProbe.cpp), engine thread, from the proj[2] (persp2) recorder:
// the output pointers and the arguments of one call. Called only while capturing().
void note_proj(uint64_t out_proj, uint64_t out_inv, float near_p, float far_p, float fovy, float aspect);

}  // namespace cbdump
}  // namespace grwxr
