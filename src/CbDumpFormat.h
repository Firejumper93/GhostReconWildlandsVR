// CbDumpFormat.h - BUILD 246: the on-disk and in-memory record layouts of the
// constant-buffer CONTENT capture (CbDump.h). Plain C++ with no Windows or D3D
// types, so the in-process analysis (CbAnalysis.h) can be compiled and tested
// offline against synthetic data.
//
// FILE LAYOUT of GRWVR\cbdump-<pid>.bin (all little endian, all offsets in bytes
// from the start of the file, every section starts 16-byte aligned):
//
//   FileHeader                 512 bytes, at 0
//   GtRow[gt_rows]             at off_gt     (gt_row_bytes each)   ground truth, one per Present
//   BufMetaRec[n_bufs]         at off_buf    (buf_rec_bytes each)  one per constant buffer seen
//   arena I records            at off_arena[0], n_slots[0] slots of slot_bytes[0]
//   arena D records            at off_arena[1], n_slots[1] slots of slot_bytes[1]
//   arena M records            at off_arena[2], n_slots[2] slots of slot_bytes[2]
//
// Arena I = immediate-context writes to buffers of 1008 or 320 bytes, every write.
// Arena D = deferred-context writes to buffers of 320 or 144 bytes, a sample.
// Arena M = everything else, a small sample per buffer per row.
// A slot is a RecHdr (64 bytes) followed by payload[arena] bytes; only data_len of
// them are meaningful. A slot whose magic is 0 was reserved but never completed.

#pragma once

#include <cstdint>

namespace grwxr {
namespace cbdump {

constexpr int      kFrames       = 120;                // presents in the window
constexpr int      kGtRows       = kFrames + 1;        // the Present entry that opens row b, b = 0..kFrames
constexpr uint32_t kBuild        = 246;
constexpr uint32_t kRecMagic     = 0xCBD0CB01u;
constexpr uint32_t kGtMagic      = 0x31525447u;        // "GTR1"
constexpr int      kHdrBytes     = 64;
constexpr int      kPayload[3]   = {1024, 320, 1024};
constexpr int      kSlotBytes[3] = {kHdrBytes + 1024, kHdrBytes + 320, kHdrBytes + 1024};
constexpr int      kProjCalls    = 4;                  // proj[2] calls kept per Present
constexpr int      kNumSlots     = 9;                  // Camera+0x420 .. +0x660, nine 4x4 floats
constexpr uint32_t kSlotBase     = 0x420;

// context kinds, the same numbering as Anatomy.cpp
enum : uint8_t { kKindImm = 0, kKindDef = 1, kKindUnk = 2, kKindRepl = 3 };

struct RecHdr {                    // 64 bytes
    uint32_t magic;                // kRecMagic, written LAST (release); 0 = slot not completed
    uint32_t slot;                 // slot index inside its arena
    uint32_t row;                  // window row b = frame_count() - start_present at the call
    uint32_t tid;
    uint64_t buffer;               // ID3D11Resource* of the constant buffer
    uint64_t ctx;                  // ID3D11DeviceContext*
    uint64_t tsc;                  // rdtsc at the capture
    uint32_t byte_size;            // the buffer's ByteWidth
    uint32_t ordinal;              // write ordinal of this buffer within this row, counting EVERY write, kept or not
    uint32_t dst_offset;           // byte offset of the written data inside the buffer (UpdateSubresource box), else 0
    uint32_t data_len;             // bytes captured
    uint8_t  kind;                 // kKind*
    uint8_t  src;                  // 1 = Map/Unmap, 2 = UpdateSubresource(1)
    uint8_t  map_type;             // D3D11_MAP for src 1, 0 for src 2
    uint8_t  flags;                // bit0 the read faulted (data_len 0), bit1 truncated (written size > data_len)
    uint32_t reserved;
};
static_assert(sizeof(RecHdr) == 64, "RecHdr layout");

struct ProjCall {                  // 104 bytes: one proj[2] (persp2) call seen by the camera probe
    uint64_t out_proj;             // persp2 arg 1, where the engine writes the projection
    uint64_t out_inv;              // persp2 arg 2
    float    near_, far_, fovy, aspect;   // aspect is stack arg 6, 0 if implausible
    uint32_t seq;                  // call number since the window opened
    uint32_t valid;                // 1 = the entry was complete, 2 = and the read-back worked
    float    readback[16];         // 64 bytes read from out_proj at Present time (stale or recycled memory is possible)
};
static_assert(sizeof(ProjCall) == 104, "ProjCall layout");

constexpr uint32_t kGtPose = 1, kGtSlots = 2, kGtScal = 4, kGtPub = 8, kGtCamNull = 16;

struct GtRow {                     // 1144 bytes, sampled at the entry of Present #(start + row)
    uint32_t magic;                // kGtMagic once filled
    uint32_t row;
    uint64_t present_no;
    uint64_t tsc;
    uint64_t cam;                  // g_player_cam, 0 if none
    uint32_t flags;                // kGt*
    uint32_t proj_calls_total;     // proj[2] calls noted since the window opened
    float    pub_fov, pub_near, pub_far;   // headpose::read_fov / read_clip
    uint32_t pub_valid;            // bit0 fov was published, bit1 clip was published
    float    pose[16];             // Camera+0x000, the pose matrix (row vectors, translation in row 3)
    float    slots[kNumSlots][16]; // Camera+0x420 + 0x40*i
    float    scal[8];              // Camera+0x2B0 fov, +0x2B4 near, +0x2B8 far, +0x2BC aspect mult, +0x2C0 res scale,
                                   // +0x2C4 skew x, +0x2C8 skew y, +0x7A4 near override
    ProjCall pc[kProjCalls];       // newest first
};
static_assert(sizeof(GtRow) == 1144, "GtRow layout");

struct BufMetaRec {                // 288 bytes
    uint64_t ptr;
    uint32_t size;
    uint32_t kmask;                // bit k set = a write from context kind k was seen
    uint32_t seen_kind[4];         // every write, per kind, inside the window rows
    uint32_t kept;
    uint32_t pad0;
    uint16_t seen_row[kGtRows];    // every write per row (saturating)
    uint16_t pad1[3];
};
static_assert(sizeof(BufMetaRec) == 288, "BufMetaRec layout");

struct FileHeader {                // 512 bytes
    char     magic[8];             // "GRWCBD01"
    uint32_t version;              // 1
    uint32_t header_bytes;         // 512
    uint32_t build;                // 246
    uint32_t pid;
    uint32_t frames;               // kFrames
    uint32_t gt_rows;              // kGtRows
    uint32_t n_bufs;
    uint32_t gt_row_bytes;
    uint32_t buf_rec_bytes;
    uint32_t rec_hdr_bytes;        // 64
    uint32_t pad0, pad1;
    uint64_t start_present;
    double   tsc_hz;
    uint32_t slot_bytes[3];
    uint32_t payload[3];
    uint32_t n_slots[3];           // slots written to the file (<= cap)
    uint32_t cap_slots[3];
    uint64_t off_gt, off_buf, off_arena[3];
    uint64_t seen[3];              // writes that selected this arena
    uint64_t stored[3];
    uint64_t dropped_budget[3];    // selected but the arena was full
    uint64_t dropped_quota[3];     // selected but the row's quota was used up
    uint64_t skipped_rule[3];      // not selected: the sampling rule (first 8 + every 64th, ...) left it out
    uint64_t faults, buf_full, ctx_full, cache_full, outside_window, incomplete_slots;
    uint8_t  reserved[512 - 328];
};
static_assert(sizeof(FileHeader) == 512, "FileHeader layout");

}  // namespace cbdump
}  // namespace grwxr
