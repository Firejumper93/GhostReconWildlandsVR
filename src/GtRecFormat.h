// GtRecFormat.h - BUILD 270: the gtrec record layouts (GtRec.h). Plain C++ (no Windows types) so the analysis
// (tools\gtrec_analyze.py, whose numpy dtypes mirror these field for field and assert the same sizes) can be tested
// against synthetic data. Little endian, every field naturally aligned, every size static_asserted.
//
// FILE GRWVR\gtrec-<pid>.bin:
//   FileHdr (512 bytes) at 0
//   then chunks until EOF: ChunkHdr (16 bytes) + count * rec_bytes, one record type per chunk, written by the init
//   thread's drain in ring order. Records carry their own seq and qpc; the analysis sorts.
//
// Design: C:\Claude\scratch-recorder\design.md section 5, verbatim except where marked BUILD 270 (fields carved out of
// the reserved tails for the build 269 call sites; no record changed size, so a reader of the design layout still
// parses every record).
#pragma once
#include <cstdint>

namespace grwxr {
namespace gtrec {

constexpr uint32_t kVersion = 1;
constexpr uint32_t kChunkMagic = 0x4B435447u;   // "GTCK"
enum : uint16_t { kTB = 1, kTW = 2, kTG = 3, kTC = 4, kTP = 5 };

struct FileHdr {                 // 512 bytes
    char     magic[8];           // "GRWGTR01"
    uint32_t version;            // 1
    uint32_t hdr_bytes;          // 512
    uint32_t build;              // the mod build number
    uint32_t pid;
    uint64_t qpc_freq;
    uint64_t qpc0;               // at arm
    uint64_t frame0;             // frame_count at arm
    uint64_t image_base;         // GRW.exe base (record pointers minus this are not RVAs; kept for the log only)
    uint64_t cam0, cam2;         // at arm
    uint32_t rec_bytes[8];       // by type index
    float    cfg[16];            // ipd_scale, ipd_sign, layer_pose_lag, veh_layer_lag, veh_build_depth, cam_pose_rot,
                                 // head_roll_fix, mvp_drive, mvp_observe, swap_eyes, fs_fov, stereo_target_fovy,
                                 // cam2_ipd, veh_smooth, veh_camera, gtrec_cb
    // BUILD 270, from the reserved tail:
    uint32_t site_a_rva;         // 168 the return address RVA that identifies recompute pass A (CameraProbe.cpp kSiteA)
    uint32_t site_b_rva;         // 172 the same for pass B (kSiteB)
    uint32_t flags;              // 176 bit0 the constant-buffer taps were on at arm, bit1 the Anatomy hook set was refused
    uint32_t cb_bytes;           // 180 the ByteWidth the C rows are filtered on (1008)
    uint8_t  reserved[512 - 184];
};
static_assert(sizeof(FileHdr) == 512, "FileHdr");

struct ChunkHdr {                // 16 bytes
    uint32_t magic;              // kChunkMagic
    uint16_t type;               // kT*
    uint16_t rec_bytes;
    uint32_t count;
    uint32_t lost;               // records of this type lost since the previous chunk (lapped, torn, stuck)
};
static_assert(sizeof(ChunkHdr) == 16, "ChunkHdr");

struct Hdr {                     // 32 bytes, first in every record
    uint16_t type;               // kT*
    uint16_t bytes;              // sizeof(record)
    uint32_t tid;                // GetCurrentThreadId()
    uint64_t seq;                // the ring reservation index (a total order of reservations per type)
    uint64_t qpc;                // QueryPerformanceCounter at the record point
    uint64_t frame;              // d3d11::frame_count() at the record point (Present entries so far)
};
static_assert(sizeof(Hdr) == 32, "Hdr");

// B: one camera recompute entry (camera 0 or camera 2 only, at EVERY call site), engine thread. 512 bytes.
enum : uint32_t { kActNone = 0, kActCompose = 1, kActReassert = 2, kActNoHead = 3, kActWriteFail = 4 };
enum : uint32_t { kSiteA = 0, kSiteB = 1, kSiteOther = 2, kSiteUnread = 3 };
struct BRow {
    Hdr      h;                  //   0
    uint64_t cam;                //  32 camera pointer (rcx)
    uint64_t rc_id;              //  40 global recompute-entry id, gtrec's own atomic counter, starts at 1
    uint64_t override_ptr;       //  48 [cam+0x2A0] (worldMatrixOverride; non-zero means the body copies it into +0x000)
    uint32_t role;               //  56 0 camera 0, 1 camera 2
    uint32_t build_seq;          //  60 CameraProbe g_build_seq after this entry's segmentation (build 269: engine frames)
    uint32_t first_in_build;     //  64 1 when this entry opened the engine frame (build 269: the first pass A entry)
    uint32_t graph_seq;          //  68 gtrec graph-builder counter at entry
    uint32_t action;             //  72 kAct*
    uint32_t w_seq;              //  76 seq of the W row emitted on this thread during the action, 0xFFFFFFFF none
    uint32_t mode;               //  80 [cam+0x290]
    uint32_t flags;              //  84 bit0 in0 read, bit1 in1 read, bit2 d4A0, bit3 d5A0, bit4 o4A0, bit5 veh_on,
                                 //     bit6 mvp_drives, bit7 stereo_live, bit8 a composed engine-frame pose existed at
                                 //     entry (g_vf_valid, BUILD 270), bit30 in1 faulted, bit31 a read faulted
    uint64_t pad0;               //  88
    float    in0[16];            //  96 [cam+0x000] at entry, BEFORE the mod acted
    float    in1[16];            // 160 [cam+0x000] read again AFTER the mod acted (equal to in0 when action is kActNone,
                                 //     unless something else wrote the rows in the microseconds between)
    float    d4A0[16];           // 224 [cam+0x4A0] at entry = the output of this camera's previous recompute
    float    d5A0[16];           // 288 [cam+0x5A0] at entry (view-projection, for the per-object WVP check of U6)
    float    o4A0[16];           // 352 the OTHER camera's +0x4A0 at entry (cross-check only)
    float    scal[4];            // 416 [cam+0x2B0] fov, +0x2B4 near, +0x2B8 far, +0x7A4
    // BUILD 270, from the reserved tail:
    uint32_t site;               // 432 kSite*: the engine call site, from the recompute frame's return address
    uint32_t ret_rva;            // 436 that return address minus the image base (0xFFFFFFFF unread)
    uint32_t seenA;              // 440 CameraProbe g_mvp_seenA after this entry (bit 0 camera 0, bit 1 camera 2)
    uint32_t pad1;               // 444
    uint8_t  reserved[64];       // 448
};
static_assert(sizeof(BRow) == 512, "BRow");

// W: one mod write to a camera pose block, any thread that writes one. 384 bytes.
enum : uint32_t { kWCompose = 1, kWScopeMono = 2, kWReassert = 3, kWCam2Sel = 4, kWMirror = 5, kWPrewrite = 6 };
struct WRow {
    Hdr      h;                  //   0
    uint64_t cam0, cam2;         //  32 the pointers written (0 = not written by this row)
    uint64_t rc_id;              //  48 the B row open on this thread while the write was made (0 = not inside a recompute entry)
    uint32_t kind;               //  56 kW*
    uint32_t build_seq;          //  60
    uint32_t fl;                 //  64 bit0 by_build, bit1 first_of_frame, bit2 stereo, bit3 veh, bit4 rot,
                                 //     bit5 vb_frame_used, bit6 polluted, bit7 roll_level, bit8 have_aim, bit9 level,
                                 //     bit10 tag pushed by this call, bit11 fp_enabled, bit12 anchored, bit13 head_ok
    int32_t  tag_eye;            //  68 2 stereo, 0/1 alternate, -1 none pushed by this call
    uint32_t tag_flags;          //  72
    float    yaw0;               //  76 g_veh_yaw0
    float    aim_cy, aim_cp;     //  80 headpose::aim_cum at the compose
    float    s;                  //  88 the signed half offset given to camera 0 (camera 2 gets -s)
    uint32_t pad;                //  92
    float    Q[4];               //  96 the XR head quaternion of the compose (the one the tag carries), raw
    float    H[9];               // 112 g_frameH, the head matrix the compose used
    float    out[9];             // 148 the composed rotation rows (right, forward, up)
    float    B[9];               // 184 the base: g_veh_B in a vehicle, else the engine base or its rebuild
    float    pos[3];             // 220 the viewpoint before the eye offset
    uint64_t pad2;               // 232
    float    m0[16];             // 240 the 64 bytes now in cam0+0x000 (after the write; read back. A scope-mono row reads
                                 //     camera 0's own bytes here although it did not write them, so its cam0 is 0)
    float    m2[16];             // 304 the 64 bytes now in cam2+0x000 (after the write; read back)
    // BUILD 270, from the reserved tail:
    uint64_t ref_wseq;           // 368 rows that carry no compose (re-assert, mirror, prewrite): the seq of the last
                                 //     kWCompose row when this one was written (the pose it re-asserts), ~0 none
    uint8_t  reserved[8];        // 376
};
static_assert(sizeof(WRow) == 384, "WRow");

// G: one graph-frame builder entry, render thread (cross-check of order and of graph entries per Present). 192 bytes.
struct GRow {
    Hdr      h;                  //   0
    uint64_t cam0, cam2;         //  32
    uint32_t graph_seq;          //  48
    uint32_t build_seq;          //  52
    uint32_t state;              //  56 cam2 state
    uint32_t flags;              //  60 bit0 a0 read, bit1 a2 read
    float    a0[16];             //  64 cam0+0x4A0
    float    a2[16];             // 128 cam2+0x4A0
};
static_assert(sizeof(GRow) == 192, "GRow");

// C: one write of a per-view constant buffer (ByteWidth 1008, bind flags exactly CONSTANT_BUFFER), any D3D thread. 192 bytes.
struct CRow {
    Hdr      h;                  //   0 h.frame = K-1 for the picture shown at Present K
    uint64_t buffer;             //  32 ID3D11Resource*
    uint64_t ctx;                //  40 ID3D11DeviceContext*
    uint8_t  kind;               //  48 0 immediate, 1 deferred, 2 unknown, 3 replay inside ExecuteCommandList
    uint8_t  src;                //  49 1 Map/Unmap, 2 UpdateSubresource(1)
    uint8_t  map_type;           //  50
    uint8_t  flags;              //  51 bit0 m020 copied, bit1 m270 copied, bit2 the copy faulted
    uint32_t ord;                //  52 this thread's running count of such writes (the global order is h.seq)
    uint32_t dst_off;            //  56 UpdateSubresource box left, else 0
    uint32_t len;                //  60 bytes available from dst_off
    float    m020[16];           //  64 buffer bytes +0x020..+0x05F as stored: Camera+0x4A0 TRANSPOSED (F-254)
    float    m270[16];           // 128 buffer bytes +0x270..+0x2AF as stored: Camera+0x520 transposed (inverse view-projection)
};
static_assert(sizeof(CRow) == 192, "CRow");

// P: one Present, render thread, staged during on_present_body and committed once. 768 bytes.
enum : uint32_t { kLsLocatedView = 0, kLsHead = 1, kLsTag = 2, kLsBuildLabel = 3, kLsLagRing = 4, kLsStripped = 0x10 };
struct PRow {
    Hdr      h;                  //   0 h.frame = K (the eyerec file number)
    int64_t  pdt, pdp;           //  32 predictedDisplayTime, predictedDisplayPeriod (ns)
    uint64_t qpc_wait;           //  48 after xrWaitFrame returned
    uint64_t qpc_end;            //  56 after xrEndFrame
    uint64_t rc_id;              //  64 last rc_id at Present entry
    uint32_t build_seq;          //  72 at Present entry
    uint32_t graph_seq;          //  76 at Present entry
    int32_t  built_eye;          //  80 the popped tag: 2 stereo, 0/1, -1 none
    uint32_t built_flags;        //  84
    uint32_t fl;                 //  88 bit0 built_q_ok, bit1 head_ori_ok, bit2 views_ok, bit3 shouldRender,
                                 //     bit4 true_stereo, bit5 halves attempted, bit6 submitted, bit7 head_roll_fix,
                                 //     bit8 swap_eyes, bit9 veh_on, bit10 mvp_drives, bit11 eyerec enabled,
                                 //     bit12 own_cam eye 0, bit13 own_cam eye 1
    int32_t  halves_rc;          //  92 submit_halves' return (1 = both eyes drawn)
    float    built_q[4];         //  96
    float    head_q[4];          // 112 located at this Present = the quaternion published to the camera side
    float    ref_inv[4];         // 128 g_ref_inv
    uint32_t ref_epoch;          // 144
    uint32_t label_src[2];       // 148 kLs* per eye, | kLsStripped
    uint32_t label_from[2];      // 156 the Present count the label was taken at (lag want, fc - depth, K; 0xFFFFFFFF for
                                 //     the tag, whose push Present the tag does not carry: the W row with tag bit 10 has it)
    float    roll_removed;       // 164 degrees removed by the strip this Present (eye 0)
    float    label_q[2][4];      // 168 pviews[e].pose.orientation as submitted
    float    label_p[2][3];      // 200 pviews[e].pose.position as submitted
    float    label_fov[2][4];    // 224 pviews[e].fov
    float    view_q[2][4];       // 256 xrLocateViews orientation per eye
    float    view_p[2][3];       // 288
    float    th[2], tv[2];       // 312 the placement tangents used per eye
    float    eye_fovy[2];        // 328
    float    cfov;               // 336
    float    aim_armed[2];       // 340 yaw, pitch armed by aim_pump this Present (mode F, U7)
    float    aim_cum[2];         // 348 g_aim_cum after this Present
    uint32_t pad;                // 356
    float    c0_4A0[16];         // 360 Present-entry snapshots (the race of 2.3, re-measured in stereo)
    float    c2_4A0[16];         // 424
    float    c0_000[16];         // 488
    float    c2_000[16];         // 552
    uint8_t  reserved[152];      // 616
};
static_assert(sizeof(PRow) == 768, "PRow");

}  // namespace gtrec
}  // namespace grwxr
