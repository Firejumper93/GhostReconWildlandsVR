// GameBuild.cpp - build 46. See GameBuild.h for what this is and why.
//
// Every RVA below is [VERIFIED] against its binary:
//   Steam : docs/RE-notes.md, pinned since session 1, sha 25860653...
//   Store : tools/store_port.py 2026-08-05 against the tester's exe,
//           sha 3349b29b..., every row exactly one hit, Steam controls OK.
//           Full derivation table: docs/ISSUE-foreign-store-builds.md.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "GameBuild.h"
#include "Log.h"

namespace grwxr {
namespace gamebuild {
namespace {

constexpr Build kSteam = {
    "Steam", 0x6502AAB6, 0x1633A000,
    {{0x01347280, 0x0C50C0E0},    // proj[0] anchor
     {0x01347460, 0x0C50C2E0},    // proj[1]
     {0x01347530, 0x0C50C420},    // proj[2] gameplay
     {0x01347840, 0x0C50C7E0},    // proj[3] skew path
     {0x01345720, 0x0C5094D0},    // proj[4]
     {0x01345800, 0x0C509720},    // proj[5]
     {0x0135F720, 0x0C5E47E0},    // on_calc_mvp
     {0x01349DF0, 0x0C510B20},    // selector
     {0x01865A10, 0x0DA1A990},    // SkeletonPostUpdate
     {0x018BE500, 0x0DC4F9B0},    // HIK datablock reader
     {0x02713160, 0x114A6DE0}},   // cPlayerComponent::OnInit callee
    0x006777C0, 0x005FA190,       // setyaw, setpitch stubs
    0x006764B0, 0x00677600,       // getyaw, getpitch stubs
    0x124B855D, 0x124B85A1,       // per-shot aim read sites (build 50 census)
    0x029AB510, 0x124B8360,       // weapon per-frame update thunk + impl
    0x030B08E0, 0x13E68070,       // hknpWorld::castRay thunk + impl
    0x029A8E80, 0x124B0770,       // GetAimOrientation thunk + impl
    0x02986B20, 0x12458BD0,       // ballistic projectile spawn thunk + impl
    0x04A66410, 0x029DC7D0, 0x12582AC0,
    0x124DE4CC,
    0x030AC6A0, 0x13E5EA30,
    0x162AD098, 0x162AD0A0,
    0x030C9990, 0x13EA3550,       // TtCastRay thunk + impl (build 58)
    0x03851120,                   // XInputGetState shadow-IAT slot
    "48 83 EC 08 44 0F B6 DA 49 89 C9 38 51 68 74 ? 44 0F B7 51 4A",
    0, 0,                         // PublishAttachments NOT derived here
    0, 0,                         // SetWorldTransform NOT derived here
};

constexpr Build kStore = {
    "Ubisoft-Epic", 0x64FACA0E, 0x16F9C000,
    {{0x01346E30, 0x0DA9BE30},    // proj[0] anchor
     {0x01347010, 0x0DA9C130},    // proj[1]
     {0x013470E0, 0x0DA9C330},    // proj[2] gameplay
     {0x013473F0, 0x0DA9C760},    // proj[3] skew path
     {0x013452D0, 0x0DA99950},    // proj[4]
     {0x013453B0, 0x0DA99B30},    // proj[5]
     {0x0135F270, 0x0DAE6510},    // on_calc_mvp
     {0x013499A0, 0x0DAA1A70},    // selector
     {0x018655B0, 0x0F0FCD30},    // SkeletonPostUpdate
     {0x018BE060, 0x0F3B0DA0},    // HIK datablock reader
     {0x02712FC0, 0x1310A3A0}},   // cPlayerComponent::OnInit callee
    0x006772B0, 0x005F9E90,       // setyaw, setpitch stubs
    0x00675FA0, 0x006770F0,       // getyaw, getpitch stubs
    0, 0,                         // per-shot aim sites NOT derived here yet
    0, 0,                         // weapon update routine NOT derived here
    0, 0,                         // hknpWorld::castRay NOT derived here
    0, 0,                         // GetAimOrientation NOT derived here
    0, 0,                         // projectile spawn NOT derived here
    0x04A66410, 0x029DC5A0, 0x13EA7F80,
    0x13E464AC,
    0x030AC4F0, 0x152E3E70,
    0x16F0F098, 0x16F0F0A0,
    0, 0,                         // TtCastRay NOT derived here
    0x03851120,                   // XInputGetState shadow-IAT slot (identical to Steam)
    "48 83 EC 08 44 0F B6 DA 49 89 C9 38 51 68 74 ? 44 0F B7 51 4A",
    0, 0,                         // PublishAttachments NOT derived here
    0, 0,                         // SetWorldTransform NOT derived here
};

// The 2026-08 "Last Rites" update. Steam and Ubisoft Connect ship the SAME
// exe (sha 56791ff5..., byte-identical), so one row covers both stores.
// Derived 2026-08-07 with tools/store_port.py old-Steam -> new exe: every
// non-zero row hit exactly once with the Steam control OK (output archived
// at binaries/store_port-2026aug-output.txt). The rows store_port missed
// (bodies recompiled) were restored 2026-08-08 by the update research
// campaign: matched on function shape, string xrefs and caller intersection
// with Steam positive controls, each with a fresh single-hit raw-file AOB.
// Full evidence chains: docs/UPDATE-CAMPAIGN-2026-08.md.
constexpr Build kUpdate2026 = {
    "2026-08-update", 0x6A692948, 0x18502000,
    {{0x013786F0, 0x0DB26FA0},    // proj[0] anchor
     {0x013788D0, 0x0DB27220},    // proj[1]
     {0x013789A0, 0x0DB27300},    // proj[2] gameplay
     {0x01378CB0, 0x0DB27650},    // proj[3] skew path
     {0x01376B90, 0x0DB24880},    // proj[4]
     {0x01376C70, 0x0DB24BF0},    // proj[5]
     {0x01390570, 0x0DB6FF30},    // on_calc_mvp
     {0x0137B280, 0x0DB2BEC0},    // selector
     {0x0189B430, 0x0F085A50},    // SkeletonPostUpdate
     {0x018F4180, 0x0F1B82F0},    // HIK datablock reader
     {0x02759D40, 0x13435390}},   // cPlayerComponent::OnInit callee
    0x004487D0, 0x004477F0,       // setyaw, setpitch stubs
    0x00447400, 0x00447330,       // getyaw, getpitch stubs (13-byte stub AOB, 1 hit)
    0x1464DEDB, 0x1464DF1C,       // per-shot aim read sites (caller intersection)
    0x02A02DC0, 0x1464DCE0,       // weapon per-frame update thunk + impl
    0x03100590, 0x161AEDE0,       // hknpWorld::castRay thunk + impl
    0x029F6FB0, 0x14619440,       // GetAimOrientation thunk + impl
    0x029D7760, 0x145BB050,       // ballistic projectile spawn thunk + impl
    0x04AFB540, 0x02A2D550, 0x146D7630,  // head_table, setter thunk + impl
    0x146691CC,                   // no-blur match
    0x030FC350, 0x161A55E0,       // weapon setterA thunk + impl
    0x1820C098, 0x1820C0A0,       // ansel IAT slots
    0x03119640, 0x161E6D60,       // TtCastRay thunk + impl
    0x038A4138,                   // XInputGetState shadow-IAT slot (moved +0x53018)
    // Recompiled body: r9->r10 at byte 10 (campaign single-hit AOB, hit = impl).
    "48 83 EC 08 44 0F B6 DA 49 89 CA 38 51 68 74 ? 0F B7 41 4A 85 C0 74 ?",
    0x018A03B0, 0x0F090D00,       // Skeleton::PublishAttachments thunk + impl
    0x017E1770, 0x0EAB1C60,       // TransformNode::SetWorldTransform thunk + impl
};

const Build* kKnown[] = {&kSteam, &kStore, &kUpdate2026};

// Read once from the loaded module's own headers. No file I/O: the values
// come from the same mapped image every RVA is applied to.
bool read_identity(uint32_t* ts, uint32_t* soi) {
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    if (!base) return false;
    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *ts  = nt->FileHeader.TimeDateStamp;
    *soi = nt->OptionalHeader.SizeOfImage;
    return true;
}

const Build* detect() {
    uint32_t ts = 0, soi = 0;
    if (!read_identity(&ts, &soi)) return nullptr;
    for (const Build* b : kKnown)
        if (b->pe_timestamp == ts && b->pe_size_of_image == soi) return b;
    return nullptr;
}

}  // namespace

const Build* get() {
    static const Build* cached = detect();
    return cached;
}

void log_identity() {
    uint32_t ts = 0, soi = 0;
    const bool have = read_identity(&ts, &soi);
    const Build* b = get();
    if (b) {
        LOG_INFO("build pin: GRW.exe identified as the %s binary "
                 "(TimeDateStamp %08X, SizeOfImage %08X). Using its address "
                 "table; every install still verifies bytes before writing.",
                 b->name, b->pe_timestamp, b->pe_size_of_image);
        return;
    }
    LOG_ERROR("build pin: UNKNOWN GRW.exe binary (TimeDateStamp %08X, "
              "SizeOfImage %08X, header read %s). Known: Steam %08X/%08X, "
              "Ubisoft-Epic %08X/%08X, 2026-08-update %08X/%08X. Nothing "
              "that depends on analysed addresses will install; the game "
              "runs unmodified. This usually means a game update or a store "
              "build we have not analysed yet: please report this log.",
              ts, soi, have ? "ok" : "FAILED",
              kSteam.pe_timestamp, kSteam.pe_size_of_image,
              kStore.pe_timestamp, kStore.pe_size_of_image,
              kUpdate2026.pe_timestamp, kUpdate2026.pe_size_of_image);
}

}  // namespace gamebuild
}  // namespace grwxr
