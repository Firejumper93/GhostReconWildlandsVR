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

// The 2026-08-13 "Last Rites" update (Ubisoft also stripped EasyAntiCheat in
// it; Denuvo and all 32 anti-tamper trigger blobs are untouched, so nothing
// about hazard handling changes).
//
// HOW THESE WERE DERIVED, 2026-08-14. tools/store_port.py fingerprints each
// function in a SOURCE binary (wildcarding rel32 targets, rip-relative disps
// and image-range immediates), then re-finds it in the target. Every row below
// reproduced its source RVA as a positive control before its target hit was
// accepted, so a bad seed shows up as a control failure and never as a silent
// wrong address (project RE rule 2).
//
// Two passes were needed. Pass 1 seeded from the 2023 Steam pin and carried 16
// bodies. The nine misses were all bodies RECOMPILED since 2023, which no
// fingerprint taken from 2023 bytes can match at any length; pass 2 reseeded
// those from kUpdate2026 (nine days older, same recompile) and carried five
// more. tools/scratch_lastrites_port.py holds pass 2.
//
// Four values were derived independently from BOTH source binaries and agreed:
// noblur_match, getpitch_slot and the two Ansel slots. That is a check on the
// method itself, not just on those rows.
//
// FOUR ROWS ARE 0 AND THEIR CONSUMERS MUST INSTALL NOTHING (rule 7):
//   cam[6] on_calc_mvp, spawn_thunk/impl   bodies recompiled past a 40-insn
//                                          fingerprint from either source
//   head_table                             RETRACTED 2026-08-15. DERIVED, see
//                                          the correction directly below.
//
// RETRACTION, 2026-08-15. This block previously read "head_table NOT STATICALLY
// DERIVABLE: the vtable holds no absolute VAs on disk because Denuvo resolves
// them at runtime". THAT IS FALSE, and it cost a week of head hide.
//
// `[VERIFIED]` by direct read of the file on disk: the table is fully populated
// with absolute VAs, in all four archived binaries. On this build,
// `head_table + 0x1F0` reads `0x00000001429EBA70`, which is ImageBase plus RVA
// `0x029EBA70`, and the bytes there are `E9 3B C0 42 12`, a jump resolving to
// RVA `0x14E17AB0`. That is exactly the class slot function `kHeadSlotFnSig`
// finds, and it agrees with the live process: the 2026-08-15 log reports the
// signature at `0x00007FF7FAB77AB0` against module base `0x00007FF7E5D60000`,
// which is the same `0x14E17AB0`.
//
// What actually happened: `store_port.py`'s rule was "slot +0x1F0 holds the
// SETTER thunk VA", and +0x1F0 does not hold the SetHidden setter. It holds the
// class's own slot-0x0F member thunk, a different function. The rule searched
// for the wrong constant, returned zero, failed its own positive control, and
// the zero was explained away as Denuvo. The lesson is the one already written
// higher up this file and then not applied: a method that cannot find the
// answer in the binary where the answer IS known is not evidence about the
// binary where it is not.
//
// How the answer was found: 35 tables in the image share this class family's
// whole 17-entry {crc32(name), index} sequence, so the hash sequence alone does
// not discriminate. Exactly ONE of the 35 holds the slot-function thunk at
// +0x1F0, and that qword occurs exactly once in the whole 411 MB file.
// Corroborated by the same table's idx-0x0D slot, which calls the already
// derived SetHidden thunk at body offsets byte-identical to the verified 2017
// original. The three older binaries all reproduced their known head_table.
//
// A wrong value still cannot arm anything: the existing +0x1F0 verify in
// CameraProbe.cpp checks this exact relationship before the hook is installed.
constexpr Build kLastRites2026 = {
    "2026-08-lastrites", 0x6A75F2F4, 0x18B09000,
    {{0x01375620, 0x0D7BB4A0},    // proj[0] anchor
     {0x01375800, 0x0D7BB7C0},    // proj[1]
     {0x013758D0, 0x0D7BB920},    // proj[2] gameplay
     {0x01375BE0, 0x0D7BBCA0},    // proj[3] skew path
     {0x01373AC0, 0x0D7B92A0},    // proj[4]
     {0x01373BA0, 0x0D7B9430},    // proj[5]
     {0,          0},             // on_calc_mvp NOT DERIVED (recompiled)
     {0x013781B0, 0x0D7C0610},    // selector
     {0x01897DF0, 0x0F8160E0},    // SkeletonPostUpdate
     {0x018F1070, 0x0F947870},    // HIK datablock reader
     {0x02751DA0, 0x1403F0B0}},   // cPlayerComponent::OnInit callee
    0x003EA960, 0x00447580,       // setyaw, setpitch stubs
    // Getter stubs are a DIFFERENT SHAPE from the setters and this cost a
    // wrong answer once: setters are `mov rax,[rcx]; jmp [rax+disp]`, getters
    // are `mov rax,[rcx]; mov rdx,[rax+disp]; jmp rdx`, the 13-byte form. A
    // scan using the setter shape at the getter offsets returned confident
    // single hits on entirely different functions; only the positive control
    // caught it.
    0x003EACB0, 0x00446B30,       // getyaw (+0x5B0), getpitch (+0x5F0) stubs
    0x14E5BDBB, 0x14E5BDFC,       // per-shot aim read sites
    0x029FAF60, 0x14E5BBC0,       // weapon per-frame update thunk + impl
    0x030F9D30, 0x169B7630,       // hknpWorld::castRay thunk + impl
    0x029EF0F0, 0x14E276C0,       // GetAimOrientation thunk + impl
    0, 0,                         // projectile spawn NOT DERIVED (recompiled)
    // head_table DERIVED 2026-08-15, and the note that said it could not be
    // was WRONG. See below.
    0x04AF4550, 0x02A25600, 0x14ED3990,   // head_table, setter thunk + impl
    0x14E7625C,                   // no-blur match (agreed from both sources)
    0x030F5AF0, 0x169AF020,       // weapon setterA thunk + impl
    0x1880F098, 0x1880F0A0,       // ansel IAT slots (agreed from both sources)
    0x03112DE0, 0x16B16990,       // TtCastRay thunk + impl
    // XInputGetState shadow-IAT slot. Not findable by content: it is a data
    // slot Denuvo fills at runtime. Chained instead through the one code
    // reference to it (a `jmp [rip+disp]` import thunk), via a ported caller
    // whose three call sites landed at byte-identical relative offsets
    // (+0x7A, +0xED) onto the same descending 8-byte slot triple, with the
    // XInput slot first in both builds.
    0x0389D130,
    // Same recompiled-body AOB as kUpdate2026: re-verified against this
    // binary, one hit, landing exactly on head_setter_impl.
    "48 83 EC 08 44 0F B6 DA 49 89 CA 38 51 68 74 ? 0F B7 41 4A 85 C0 74 ?",
    0x0189CE30, 0x0F821260,       // Skeleton::PublishAttachments thunk + impl
    0x017DEA30, 0x0F47BDC0,       // TransformNode::SetWorldTransform thunk + impl
};

const Build* kKnown[] = {&kSteam, &kStore, &kUpdate2026, &kLastRites2026};

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
              "Ubisoft-Epic %08X/%08X, 2026-08-update %08X/%08X, "
              "2026-08-lastrites %08X/%08X. Nothing "
              "that depends on analysed addresses will install; the game "
              "runs unmodified. This usually means a game update or a store "
              "build we have not analysed yet: please report this log.",
              ts, soi, have ? "ok" : "FAILED",
              kSteam.pe_timestamp, kSteam.pe_size_of_image,
              kStore.pe_timestamp, kStore.pe_size_of_image,
              kUpdate2026.pe_timestamp, kUpdate2026.pe_size_of_image,
              kLastRites2026.pe_timestamp, kLastRites2026.pe_size_of_image);
}

}  // namespace gamebuild
}  // namespace grwxr
