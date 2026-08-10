// GameBuild.h - build 46: the fail-closed module identity pin.
//
// Two known GRW.exe builds ship the same engine with every function at a
// different RVA: the Steam binary (pinned since session 1) and the
// Ubisoft/Epic store binary (sibling build six days older, derived offline
// 2026-08-05 with tools/store_port.py: every body, thunk, stub and data
// target hit exactly once, with the Steam value reproduced as a positive
// control on every row; full table in the ISSUE-foreign-store-builds doc).
//
// detect() reads TimeDateStamp + SizeOfImage from the loaded module's own PE
// header, the Halo MCC pattern, and returns the matching table, or null for
// a binary we have never analysed, in which case every consumer logs loudly
// and installs NOTHING (rule 7).
//
// The pin SELECTS values; it never TRUSTS them. Every install site keeps its
// own runtime verification against the selected value: signature scans with
// expected-RVA cross-checks, thunk byte verification before any write, and
// exact byte reads. A wrong table can therefore refuse to install, but it
// cannot patch the wrong bytes.

#pragma once

#include <cstdint>

namespace grwxr {
namespace gamebuild {

struct Build {
    const char* name;
    uint32_t    pe_timestamp;        // IMAGE_FILE_HEADER.TimeDateStamp
    uint32_t    pe_size_of_image;    // IMAGE_OPTIONAL_HEADER.SizeOfImage

    // CameraProbe thunk table rows. RVAs only: the per-row metadata
    // (rcx_is_camera, name) is build-independent and stays in CameraProbe.
    struct CamTarget { uintptr_t thunk, fn; };
    CamTarget cam[11];

    // Aim setter virtual-dispatch stubs (the 10 expected bytes, including
    // the 0x570/0x5D0 dispatch offsets, are identical in both builds:
    // the store scan matched the full byte sequence exactly once).
    uintptr_t setyaw_slot;
    uintptr_t setpitch_slot;

    // Build 50: the matching GETTER stubs (vtable +0x5B0 yaw, +0x5F0 pitch),
    // for the aim-reader census (AimTrace.h). Derived 2026-08-06 with
    // tools/sig_scan.py: the 13-byte sequence hits exactly once in each
    // binary, and the Steam control lands on the RVA RE-notes documents.
    uintptr_t getyaw_slot;
    uintptr_t getpitch_slot;

    // Build 51: the PER-SHOT aim reader, found by build 50's census on
    // 2026-08-06 (ten calls for ten rounds fired, then flat). These are
    // RETURN addresses: the instruction after `call <getter stub>` inside
    // the function at 0x124B8360, which computes
    //     [r15+0xAC] = wrap(perAxisOffset + currentYaw)
    //     [r15+0xB0] = wrap(perAxisOffset + currentPitch)
    // 0 = not derived for this binary, and the override refuses to arm.
    // AimTrace verifies the E8 rel32 in front of each site really resolves
    // to the matching getter stub before trusting either (rule 7).
    uintptr_t shot_yaw_site;
    uintptr_t shot_pitch_site;

    // Build 53: the per-shot weapon routine that owns those two reads, and
    // its E9 thunk. Found by walking up from the read sites: the function is
    // virtual (no direct callers, one vtable slot at 0x04A673D0), so the only
    // way to learn who invokes it is to hook it and let it name its caller.
    // 0 = not derived for this binary.
    uintptr_t wfire_thunk;
    uintptr_t wfire_impl;

    // Build 54: hknpWorld::castRay, the Havok world raycast, and its E9
    // thunk. Found 2026-08-06 by cross-referencing Havok's own monitor-timer
    // string literal "TtWorldCastRay" (0x03C737A8), which HK_TIMER_BEGIN
    // writes into the profiling stream from INSIDE the function it names.
    // Signature: castRay(hknpWorld* rcx, RayInput* rdx, Collector* r8).
    // Only EIGHT call sites exist in the image; the shot trace is one.
    uintptr_t raycast_thunk;
    uintptr_t raycast_impl;

    // Build 55: GetAimOrientation, the function that turns the aim SCALARS
    // into a DIRECTION. Verified body: rcx = out quaternion, rdx = aim
    // sub-object (parent = rdx-0x48), r8 = int* mode; when *mode == -1 and
    // the dirty byte [parent+0x4B0] is clear it returns the cached quaternion
    // at [parent+0x420], otherwise it rebuilds base * yaw * pitch * roll.
    // The recoil node sets that dirty flag, which is the causal link.
    uintptr_t aimquat_thunk;
    uintptr_t aimquat_impl;

    // Build 56: THE PROJECTILE SPAWN. 0x12458BD0 allocates a 0x180-byte
    // cBallisticProjectileComponent and fills it. Verified byte for byte:
    //   movaps xmm1,[owner+0x150] -> [proj+0x50]   m_vBulletShootOrigin
    //   movaps xmm0,[owner+0x140] -> [proj+0x100]  m_vBulletSimulationDirection
    //   mov    [proj+0x20], owner                  back-pointer
    // So the shot direction is NOT computed here: it is read from
    // [owner+0x140]. Class name and field names came from CRC32-cracking the
    // Anvil reflection tables, so they are the engine's own names.
    uintptr_t spawn_thunk;
    uintptr_t spawn_impl;

    // Head hide. The method table RVA is identical in both builds (data
    // layout survived the recompile); kept per-build anyway so an unknown
    // future build cannot inherit it silently.
    uintptr_t head_table;
    uintptr_t head_setter_thunk;
    uintptr_t head_setter_impl;

    // No-camera-blur match site (patch byte at +5).
    uintptr_t noblur_match;

    // Pooled placement SetTransform (WeaponProbe).
    uintptr_t wp_setter_thunk;
    uintptr_t wp_setter_impl;

    // Ansel IAT slots.
    uintptr_t ansel_setconfig;
    uintptr_t ansel_updatecam;

    // Build 58: TtCastRay (HK_TIMER literals "CastRay" 0x03C742E0 and
    // "TtCastRay" 0x03C742E8 name it from inside), the raycast body the
    // hknpWorld::castRay wrapper reaches through a runtime-built virtual
    // table. Three call sites reach it DIRECTLY through this thunk,
    // bypassing the wrapper, which is why the build 57 shot window saw
    // only ambient traffic: the wrapper hook was blind to them.
    uintptr_t raycast2_thunk;
    uintptr_t raycast2_impl;

    // XInputGetState shadow-IAT slot (XInputMerge). The game resolves XInput by
    // hand (GetModuleHandle + GetProcAddress) into a fixed data global and calls
    // through it, bypassing the real IAT, so this data slot is the only pointer
    // to patch. Its RVA moves on every recompile: the 2023 value 0x03851120
    // (identical Steam/store) became 0x038A4138 on the 2026-08 update. Pinned
    // per build so an unknown binary installs nothing (rule 7). Re-derived via
    // the resolver-anchor AOB, Steam positive control passing.
    uintptr_t xinput_slot;

    // Head-hide SetHidden body signature. Per build: the 2026-08 recompile
    // re-registered the body (mov r10 vs r9 at byte 10), so the 2023 bytes
    // get zero hits there and the install refuses (rule 7). The campaign doc
    // (UPDATE-CAMPAIGN-2026-08.md section 1) derived both and directed
    // "keep both signatures build-specific". The verify contract in
    // CameraProbe is unchanged: the signature must hit uniquely AND at
    // head_setter_impl, or head hide stays off.
    const char* head_setter_sig;

    // Skeleton::PublishAttachments. The character's skeleton update composes
    // each attached object's world transform here, from its own pose bone
    // buffer, and then places the object. So this is the LAST INSTANT at which
    // the gun-root bone can be changed and still be the value the engine uses
    // to place the weapon: writing earlier is overwritten by the animation
    // solver, writing later is too late. 0 = not derived, and the consumer
    // installs nothing (rule 7). Chain and evidence: docs/RE-notes.md
    // "THE HELD-WEAPON ATTACHMENT CHAIN".
    uintptr_t publish_thunk;
    uintptr_t publish_impl;

    // TransformNode::SetWorldTransform. THE write target for making the game's
    // own weapon follow the controller. Signature
    // __fastcall(TransformNode* rcx, const float4x4* rdx, bool, bool); rows
    // +0x00/+0x10/+0x20 are the rotation basis, translation is the FOURTH ROW
    // at +0x30. Substituting rdx here is not a race with the engine, it IS the
    // engine's own commit, which is why every precedent (FRIK abandoning its
    // renderer hooks, REFramework hooking named pipeline stages, UEVR's
    // "Permanent Change") converges on being the last writer. 9 rel32 sites,
    // so gating on rcx is MANDATORY. 0 = not derived, installs nothing.
    uintptr_t setworld_thunk;
    uintptr_t setworld_impl;
};

// The table for the binary this process actually loaded, detected once from
// the PE header and cached. nullptr = a binary we have never analysed;
// callers must log loudly and install nothing. Never returns a partially
// filled table.
const Build* get();

// Logs the detected identity (or the mismatch, with both known pins) once.
// Called from dllmain before any consumer installs.
void log_identity();

}  // namespace gamebuild
}  // namespace grwxr
