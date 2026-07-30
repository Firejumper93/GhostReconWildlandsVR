#!/usr/bin/env python3
"""
Find references to a data RVA in a PE image, two ways:

1. RIP-relative lea/mov sites in executable sections (the way code loads a
   string or table address on x64): scans for `lea r64, [rip+disp32]`
   (REX + 8D + modrm rip) and `mov r64, [rip+disp32]` (REX + 8B + modrm rip)
   whose displacement resolves to the target RVA.
2. Absolute 8-byte pointers anywhere in the file (name tables are often
   pointer arrays in data): ASLR is off and the image loads at its preferred
   base 0x140000000 (docs/RE-notes.md), so a stored pointer to RVA X is the
   literal little-endian qword 0x140000000+X. Pointer-slot hits can then be
   fed back into mode 1 to find the code that walks the table.

A hit can be a false positive if the matched bytes are data or the tail of
another instruction; verify with tools/disasm_at.py and locate the enclosing
function with tools/pdata_lookup.py.

Read-only. Used first in session 11 for the Q11 HumanIK effector hunt.

Usage:
    python tools/find_dataxrefs.py <pe-file> 0x03C7FE30 [more RVAs...]
"""
import argparse

import pefile

EXEC = 0x20000000  # IMAGE_SCN_MEM_EXECUTE
IMAGE_BASE = 0x140000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pefile")
    ap.add_argument("rvas", nargs="+")
    a = ap.parse_args()

    pe = pefile.PE(a.pefile, fast_load=True)
    secs = [(s.VirtualAddress, s.PointerToRawData,
             min(s.Misc_VirtualSize, s.SizeOfRawData),
             s.Name.rstrip(b"\x00").decode("latin-1"),
             bool(s.Characteristics & EXEC))
            for s in pe.sections]
    with open(a.pefile, "rb") as fh:
        data = fh.read()

    for target in [int(r, 0) for r in a.rvas]:
        rip_hits = []
        for va, raw, size, name, is_exec in secs:
            if not is_exec:
                continue
            buf = data[raw:raw + size]
            for op, kind in ((0x8D, "lea"), (0x8B, "mov")):
                i = buf.find(bytes([op]))
                while i != -1:
                    # REX prefix before, rip-relative modrm after, disp32.
                    if 1 <= i and i + 6 <= size:
                        rex = buf[i - 1]
                        modrm = buf[i + 1]
                        if 0x40 <= rex <= 0x4F and (modrm & 0xC7) == 0x05:
                            disp = int.from_bytes(buf[i + 2:i + 6], "little",
                                                  signed=True)
                            # Instruction starts at i-1, length 7.
                            if va + (i - 1) + 7 + disp == target:
                                rip_hits.append((va + i - 1, name, kind))
                    i = buf.find(bytes([op]), i + 1)

        needle = (IMAGE_BASE + target).to_bytes(8, "little")
        ptr_hits = []
        for va, raw, size, name, _ in secs:
            buf = data[raw:raw + size]
            i = buf.find(needle)
            while i != -1:
                ptr_hits.append((va + i, name))
                i = buf.find(needle, i + 1)

        print(f"target 0x{target:08X}:")
        print(f"  {len(rip_hits)} rip-relative site(s)")
        for rva, name, kind in sorted(rip_hits):
            print(f"    {kind} at 0x{rva:08X}  ({name})")
        print(f"  {len(ptr_hits)} absolute-pointer slot(s)")
        for rva, name in sorted(ptr_hits):
            print(f"    qword at 0x{rva:08X}  ({name})")


if __name__ == "__main__":
    main()
