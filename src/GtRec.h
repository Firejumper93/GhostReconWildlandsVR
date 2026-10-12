// GtRec.h - BUILD 270: gtrec, the GROUND-TRUTH STEREO RECORDER. LOG ONLY. Off by default (cfg gtrec = 0).
//
// THE QUESTION. For every presented true-stereo frame: which engine recompute drew each eye, which head sample and
// which composed rotation that recompute used, what rotation the engine actually built its view from, which pose the
// layer was labelled with per eye, the label error in degrees and the left-versus-right disagreement. Exactly, with no
// correlation guessing. Design: C:\Claude\scratch-recorder\design.md (sections 1 to 7); the analysis is
// tools\gtrec_analyze.py.
//
// WHAT IT RECORDS (GtRecFormat.h has every field):
//   B  every recompute entry of camera 0 or camera 2 (MvpHook.cpp, the on_calc_mvp branch of CameraProbe.cpp), at
//      every engine call site, classified by site (pass A, pass B, other): the pose rows before and after the mod
//      acted, the previous recompute's output (+0x4A0), the view-projection, the other camera's output, what the mod did.
//   W  every mod write of a camera pose block (write_pose_head, the camera 0 re-assert, the Cam2 latch writers): the
//      head quaternion, the head matrix, the composed rotation, the base, and the 64 bytes each camera now holds.
//   G  every graph-frame builder entry (Cam2.cpp on_graph_entry), both cameras' +0x4A0, before the per-frame update.
//   C  every write of the 1008-byte per-view constant buffer (Anatomy's Map/Unmap/UpdateSubresource hooks, gate bit
//      4): the +0x020 block (Camera+0x4A0 transposed, F-254) and the +0x270 block, with the context kind.
//   P  every Present: the tag, the predicted display time, the located head and views, the label per eye with its
//      source (located view, head, tag, build ring, lag ring, stripped), the placement, both cameras' blocks at entry.
//
// RULE 8. Nothing here logs, locks, allocates or does file I/O on a hook thread. Five rings are VirtualAlloc'ed ONCE
// by the init thread when the recorder arms; a writer reserves a slot with one fetch_add, fills it with SEH-guarded
// copies, and publishes it with one release store (a per-slot seqlock, the idiom of Cam2.cpp's pose latch made
// multi-producer). The init thread drains the rings to GRWVR\gtrec-<pid>.bin about once a second, and logs one
// "gtrec:" line per drain. The ONE deviation is inherited from Anatomy.h: the first sight of a buffer or a context
// makes a few plain COM getter calls to learn its bind flags and kind.
//
// WITH gtrec = 0: nothing is allocated, no file is opened, the Anatomy gate bit is never set, and every hook pays one
// relaxed-or-acquire atomic load (on()) and nothing else. On foot nothing is written in any mode (gtrec only reads).
//
// cfg (VRMirror.cpp load_config, beside veh_layer_lag; hot reload):
//   gtrec          0 (default) off; 1 arms the recorder. It goes live once true stereo is live (cam2::stereo_live) and,
//                  when gtrec_cb is on, once the Anatomy hook set exists (installed 20 s after the first stereo layer),
//                  so every recorded frame has its constant-buffer rows. One recording per run.
//   gtrec_cb       1 (default) taps the per-view constant buffer (C rows); 0 records B/W/G/P only.
//   gtrec_max_mb   1024 (default), clamped to [16, 20000]: the file stops growing at this size and the recorder stops.
//   gtrec_seconds  0 (default) = no time limit; else the recorder stops this many seconds after it went live
//                  (clamped to [0, 86400]).
#pragma once

#include <atomic>
#include <cstdint>

#include "GtRecFormat.h"

struct ID3D11DeviceContext;
struct ID3D11Resource;
struct D3D11_BOX;

namespace grwxr {
namespace gtrec {

// cfg, any thread (VRMirror.cpp load_config).
void set_enabled(bool on);
void set_cb(bool on);
void set_max_mb(int mb);
void set_seconds(int s);
void set_cfg_snapshot(const float v[16]);   // the 16 values of FileHdr::cfg, after each parse
bool cb_wanted();                           // gtrec && gtrec_cb: Anatomy installs its hook set for it

// 1 while recording. Every hook tests this first; acquire so the ring pointers set before it are visible (a plain
// load on x64).
extern std::atomic<int> g_live;
inline bool on() { return g_live.load(std::memory_order_acquire) != 0; }

uint64_t qpc_now();
// SEH-guarded 64-byte read of engine memory (false and zeros on a fault or a misaligned / null pointer).
bool copy64(float dst[16], uint64_t src);

// B: the recompute entry, engine thread. b_begin reserves and fills the row from the camera BEFORE the mod acts (call
// it only when on()); b_end reads the pose rows again AFTER the action and publishes. ctx_flags carries flags bits 5..8.
BRow* b_begin(uint64_t cam, uint64_t other, int role, uint32_t build_seq, bool first, uint32_t site, uint32_t ret_rva,
              uint32_t seenA, uint32_t ctx_flags);
void  b_end_impl(BRow* r, uint32_t action);
inline void b_end(BRow* r, uint32_t action) { if (r) b_end_impl(r, action); }

// W: a write of a camera pose block. w_emit takes a row the caller filled (kind, the compose fields, m0 / m2; it fills
// h, rc_id, ref_wseq and publishes). w_mark is the short form for a write that carries no compose (a re-assert, a
// mirror, a prewrite): it reads the 64 bytes each named camera now holds (0 = not written) and publishes.
void w_emit(WRow& w);
void w_mark(uint32_t kind, uint64_t cam0, uint64_t cam2);

// G: the graph-frame builder entry, render thread.
void g_emit(uint64_t cam0, uint64_t cam2, uint32_t build_seq, int state);

// C: Anatomy's taps, any D3D thread, only while Anatomy's gate bit 4 is set. map_type is the D3D11_MAP value.
void tap_map(ID3D11DeviceContext* c, ID3D11Resource* res, uint32_t sub, uint32_t map_type, const void* data);
void tap_unmap(ID3D11DeviceContext* c, ID3D11Resource* res, uint32_t sub);   // BEFORE the real Unmap
void tap_update(ID3D11DeviceContext* c, ID3D11Resource* res, uint32_t sub, const D3D11_BOX* box, const void* src);

// P: the Present row, render thread only. p_begin opens the static staging row (only when on()); p_stage() is the open
// row or nullptr, and the present path fills its fields in place; p_commit copies it into the ring once. PScope commits
// on every return of on_present_body (which has no __try, so a guard object is legal there).
extern PRow g_ps;
extern bool g_ps_open;
inline PRow* p_stage() { return g_ps_open ? &g_ps : nullptr; }
void p_begin_impl(uint64_t frame, int built_eye, const float q[4], bool q_ok, uint32_t built_flags);
inline void p_begin(uint64_t frame, int built_eye, const float q[4], bool q_ok, uint32_t built_flags) {
    if (on()) p_begin_impl(frame, built_eye, q, q_ok, built_flags);
}
void p_commit();
struct PScope {
    ~PScope() { if (g_ps_open) p_commit(); }
};

// Init thread: tick once a second (arms, drains, logs, stops at the cap). on_stall: one best-effort drain when the
// game stops presenting. on_exit: the final drain and close at DLL detach.
void tick();
void on_stall();
void on_exit();

}  // namespace gtrec
}  // namespace grwxr
