// BUILD 248: EYE RECORDER. A diagnostic that writes what the mod drew into each eye, as a JPEG sequence, so a
// session can be reviewed frame by frame: black borders, a wrong field of view, jumpiness and left/right
// disagreement are all visible in a sequence and none of them is in a log line.
//
//   Off by default (cfg eyerec = 0). It is never called outside the TRUE-STEREO path (a trusted halves frame,
//   which exists only while camera 2 is RUNNING, which in turn needs live gameplay: menus and loading frames
//   never reach it). Nothing here ships: the recorder taps every frame.
//
//   record() runs on the Present thread, after both eyes were placed. It redraws each eye's half image into a
//   small render target with the SAME placement math as the eye canvas (scaled), copies it to a staging ring,
//   and harvests frames from the ring two recordings later, so the GPU is never waited on. A writer thread
//   encodes the two eyes side by side into GRWVR\eyerec-<pid>\f<frame>.jpg and appends a row per frame to
//   frames.csv. The disk budget is a hard cap (eyerec_max_mb); at the cap the recorder stops itself.
//
//   What it shows: the picture's placement in each eye (the canvas), at reduced size. What it does not show:
//   the HUD and overlay quads (dot, hands, panels), which are drawn after the placement, and the lens.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace grwxr {
namespace eyerec {

// cfg, any thread.
void set_enabled(bool on);       // eyerec
void set_width(int px);          // eyerec_width: one eye's width in the recording, clamped to [160, 1280]
void set_every(int n);           // eyerec_every: record every Nth trusted frame, clamped to [1, 60]
void set_max_mb(int mb);         // eyerec_max_mb: stop at this many MB written, clamped to [16, 20000]
bool enabled();                  // one relaxed load

// Present thread, trusted stereo frames only. `src[e]` is the half-image SRV that eye e was drawn from.
// tan_h/tan_v are the half-extents (tangents) the eye placement used; cx/cy/sx/sy and sc_w/sc_h are the eye
// canvas centre and scale (pixels, pixels per unit tangent) and size. `meta` is a short text row (no commas)
// stored in frames.csv beside the frame number.
void record(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const src[2],
            const float tan_h[2], const float tan_v[2],
            const float cx[2], const float cy[2], const float sx[2], const float sy[2],
            uint32_t sc_w, uint32_t sc_h, uint64_t frame, const char* meta);

}  // namespace eyerec
}  // namespace grwxr
