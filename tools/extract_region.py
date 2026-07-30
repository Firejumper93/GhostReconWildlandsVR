#!/usr/bin/env python3
"""
Dump every string in a contiguous RVA window of a PE image.

Used in session 1 to lift the whole Autodesk HumanIK effector / property name
table out of GRW.exe as one durable artifact, rather than relying on the
themed-string heuristics to have caught every entry.

Session 1 (GRW-XR). Read-only.

Usage:
    python tools/extract_region.py <pe-file> 0x03C7F000 0x03C88000 --out docs/RAW/humanik-strings.txt
"""
import argparse
import os
import re

import pefile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pefile")
    ap.add_argument("start", help="start RVA, e.g. 0x03C7F000")
    ap.add_argument("end", help="end RVA (exclusive)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--min", type=int, default=4)
    a = ap.parse_args()

    start = int(a.start, 0)
    end = int(a.end, 0)

    pe = pefile.PE(a.pefile, fast_load=True)
    rows = []
    for s in pe.sections:
        if s.SizeOfRawData:
            rows.append((s.VirtualAddress,
                         s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData),
                         s.PointerToRawData,
                         s.Name.rstrip(b"\x00").decode("latin-1")))
    pe.close()

    lines = []
    with open(a.pefile, "rb") as fh:
        for vstart, vend, praw, name in rows:
            lo = max(start, vstart)
            hi = min(end, vend)
            if lo >= hi:
                continue
            fh.seek(praw + (lo - vstart))
            blob = fh.read(hi - lo)
            pat = re.compile(rb"[\x20-\x7e]{%d,}" % a.min)
            for m in pat.finditer(blob):
                rva = lo + m.start()
                lines.append(f"0x{rva:08X}\t{name}\t{m.group().decode('latin-1')}")

    header = (f"# region dump of {a.pefile}\n"
              f"# RVA window [0x{start:08X}, 0x{end:08X})  min length {a.min}\n"
              f"# {len(lines)} strings\n")
    text = header + "\n".join(lines) + "\n"
    if a.out:
        os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
        with open(a.out, "w", encoding="utf-8", errors="replace") as out:
            out.write(text)
        print(f"wrote {a.out} ({len(lines)} strings)")
    else:
        print(text)


if __name__ == "__main__":
    main()
