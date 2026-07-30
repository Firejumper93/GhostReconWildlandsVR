#!/usr/bin/env python3
"""
Find direct call/jmp rel32 sites targeting a given RVA in a PE image.

Scans every executable section for E8 (call rel32) and E9 (jmp rel32) whose
displacement resolves to the target RVA. Used in session 2 to walk up the
camera call graph: the projection function had zero direct callers and one
jump thunk, and this automates that discovery for each new function.

A rel32 hit can be a false positive if the E8/E9 byte is actually data or the
tail of another instruction, so verify hits by disassembling around them.

Read-only.

Usage:
    python tools/find_callers.py <pe-file> 0x0C510B20 [more RVAs...]
"""
import argparse

import pefile

EXEC = 0x20000000  # IMAGE_SCN_MEM_EXECUTE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pefile")
    ap.add_argument("rvas", nargs="+")
    a = ap.parse_args()

    pe = pefile.PE(a.pefile, fast_load=True)
    secs = [(s.VirtualAddress, s.PointerToRawData,
             min(s.Misc_VirtualSize, s.SizeOfRawData),
             s.Name.rstrip(b"\x00").decode("latin-1"))
            for s in pe.sections if s.Characteristics & EXEC]
    with open(a.pefile, "rb") as fh:
        data = fh.read()

    targets = [int(r, 0) for r in a.rvas]
    for target in targets:
        hits = []
        for va, raw, size, name in secs:
            buf = data[raw:raw + size]
            i = buf.find(b"\xe8")
            while i != -1:
                if i + 5 <= size:
                    rel = int.from_bytes(buf[i + 1:i + 5], "little", signed=True)
                    if va + i + 5 + rel == target:
                        hits.append((va + i, name, "call"))
                i = buf.find(b"\xe8", i + 1)
            i = buf.find(b"\xe9")
            while i != -1:
                if i + 5 <= size:
                    rel = int.from_bytes(buf[i + 1:i + 5], "little", signed=True)
                    if va + i + 5 + rel == target:
                        hits.append((va + i, name, "jmp"))
                i = buf.find(b"\xe9", i + 1)
        print(f"target 0x{target:08X}: {len(hits)} rel32 site(s)")
        for rva, name, kind in sorted(hits):
            print(f"  {kind:<4s} at 0x{rva:08X}  ({name})")
        print()


if __name__ == "__main__":
    main()
