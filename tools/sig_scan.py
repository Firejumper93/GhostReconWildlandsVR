#!/usr/bin/env python3
"""
AOB signature scanner for GRW.exe, offline against the file on disk.

Session 1 established that GRW.exe's code is plaintext on disk despite Denuvo
(tools/disasm_at.py), so signatures can be derived and tested WITHOUT running
the game. That is a large advantage: no launch cycle, no crash risk, no
anti-tamper exposure.

Step 1 of the Origins porting method (docs/REFERENCE-ANALYSIS.md A14) is
"attempt the Odyssey signatures first". This runs all of them, plus the
Valhalla and Mirage variants, and reports hits with RVA and VA.

Expectation, stated up front so a miss is not a surprise: docs/PORT-MAP.md 2.3
predicts ZERO direct hits, because not one byte pattern survived between
Odyssey and Valhalla, and Wildlands is older than both with a different internal
namespace. Running them anyway is cheap and a hit would be worth a lot.

Usage:
    python tools/sig_scan.py <pe-file>
    python tools/sig_scan.py <pe-file> --pattern "48 8B C4 53 ? ? ?"
    python tools/sig_scan.py <pe-file> --shape          # shape-based heuristics
"""
import argparse
import re
import struct
import sys

import pefile

# From docs/REFERENCE-ANALYSIS.md A3, verbatim.
REFERENCE_SIGS = [
    ("Odyssey  calc_projection",       "48 89 E0 53 48 81 EC 90 00 00 00 0F 29 70 E8 48 89 CB F3"),
    ("Odyssey  on_calc_mvp",           "48 89 E0 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 C8"),
    ("Odyssey  on_task",               "40 53 48 83 EC 20 48 8B D9 48 81 C1 00 01 00 00 48 83 39 00 75 07"),
    ("Odyssey  camera_get_forward",    "0F 28 59 70 48 89"),
    ("Odyssey  calc_ui_viewport",      "55 48 8D 6C 24 A9 48 81 EC E0 00 00 00 48 8B 05 ? ? ? ? 48 31 E0"),
    ("Valhalla calc_projection",       "48 8B C4 53 48 81 EC 90 00 00 00 0F 29 70 E8 48 8B D9 F3"),
    ("Valhalla update_views",          "48 8B C4 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC D8 01"),
    ("Valhalla camera_get_forward",    "0F 10 51 70 48 8B C2 0F 28 1D"),
    ("Valhalla begin_render_frame",    "40 53 48 83 EC 20 48 8B D9 48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B 0D"),
    ("Valhalla disable_taa2",          "85 DB 74 41 83 EB 01"),
    ("Valhalla gfx_context_copy",      "0F 10 01 0F 10 49 10 0F 11 81 D0 06 00 00"),
]

# Shape-based probes. These do not come from the reference; they target the
# ARITHMETIC a perspective projection builder must perform, which is far more
# stable across builds than a prologue. See docs/REFERENCE-ANALYSIS.md B2.3.
SHAPE_SIGS = [
    # movss/mulss/divss clusters writing a 4x4 through xmm, common tail of a
    # projection builder: unpcklps pairs then four 16-byte stores.
    ("shape: unpcklps x2 then store",  "0F 14 ? 0F 14 ? 0F 29"),
    ("shape: 4 consecutive movaps stores to [rcx+0/16/32/48]",
                                       "0F 29 01 0F 29 49 10 0F 29 41 20 0F 29 49 30"),
    ("shape: movaps stores to [rax+...]",
                                       "0F 29 00 0F 29 48 10 0F 29 40 20 0F 29 48 30"),
    # 2.0f and 1.0f immediates adjacent, typical of 2/(r-l) style setup
    ("shape: rcpss/divss then unpcklps", "F3 0F 5E ? 0F 14"),
]


def parse_pattern(p):
    """'48 8B ? ?? C4' -> (bytes, mask) where mask 1 = must match."""
    toks = p.replace("??", "?").split()
    b, m = bytearray(), bytearray()
    for t in toks:
        if t == "?":
            b.append(0)
            m.append(0)
        else:
            b.append(int(t, 16))
            m.append(1)
    return bytes(b), bytes(m)


def find_all(data, pat, mask, limit=32):
    """Scan with a fast first-anchor skip, then verify the masked remainder."""
    hits = []
    # Anchor on the first concrete byte to let bytes.find do the heavy lifting.
    try:
        anchor = mask.index(1)
    except ValueError:
        return hits
    a = pat[anchor]
    n = len(pat)
    start = 0
    while True:
        i = data.find(bytes([a]), start)
        if i < 0:
            break
        s = i - anchor
        start = i + 1
        if s < 0 or s + n > len(data):
            continue
        ok = True
        for k in range(n):
            if mask[k] and data[s + k] != pat[k]:
                ok = False
                break
        if ok:
            hits.append(s)
            if len(hits) >= limit:
                break
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pefile")
    ap.add_argument("--pattern", default=None)
    ap.add_argument("--shape", action="store_true")
    ap.add_argument("--limit", type=int, default=32)
    a = ap.parse_args()

    pe = pefile.PE(a.pefile, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = []
    for s in pe.sections:
        if s.SizeOfRawData:
            secs.append((s.Name.rstrip(b"\x00").decode("latin-1"),
                         s.PointerToRawData, s.SizeOfRawData, s.VirtualAddress))
    pe.close()

    with open(a.pefile, "rb") as fh:
        data = fh.read()

    def off_to_rva(off):
        for nm, praw, sz, rva in secs:
            if praw <= off < praw + sz:
                return rva + (off - praw), nm
        return None, "?"

    if a.pattern:
        sigs = [("custom", a.pattern)]
    elif a.shape:
        sigs = SHAPE_SIGS
    else:
        sigs = REFERENCE_SIGS

    print(f"scanning {a.pefile} ({len(data):,} bytes), image base 0x{base:X}\n")
    total = 0
    for name, pstr in sigs:
        pat, mask = parse_pattern(pstr)
        hits = find_all(data, pat, mask, a.limit)
        total += len(hits)
        if not hits:
            print(f"  MISS  {name}")
            continue
        print(f"  HIT   {name}   ({len(hits)} match{'es' if len(hits) != 1 else ''})")
        for h in hits[: a.limit]:
            rva, sec = off_to_rva(h)
            if rva is None:
                print(f"          file 0x{h:X}  (outside any section)")
            else:
                print(f"          RVA 0x{rva:08X}  VA 0x{base + rva:012X}  [{sec}]")
    print(f"\n{total} total matches across {len(sigs)} patterns")
    if not total and not a.pattern:
        print("\nAll reference signatures missed, which is exactly what")
        print("docs/PORT-MAP.md 2.3 predicted. Fall back to function shape and")
        print("string cross-references: try --shape, and see B2.3.")


if __name__ == "__main__":
    main()
