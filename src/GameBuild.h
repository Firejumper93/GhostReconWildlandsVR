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
