#!/usr/bin/env python3
"""Masked AOB scan over executable sections. '??' is a wildcard byte. Read-only."""
import re
import sys
import pefile

EXEC = 0x20000000

pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(sys.argv[1], "rb").read()
secs = [(s.VirtualAddress, s.PointerToRawData,
         min(s.Misc_VirtualSize, s.SizeOfRawData),
         s.Name.rstrip(b"\x00").decode("latin-1"))
        for s in pe.sections if s.Characteristics & EXEC]

for pat in sys.argv[2:]:
    toks = pat.split()
    rx = b"".join(b"." if t in ("??", "?") else re.escape(bytes([int(t, 16)]))
                  for t in toks)
    print(f"\n=== {pat} ===")
    total = 0
    for va, po, sz, nm in secs:
        blob = data[po:po + sz]
        for m in re.finditer(rx, blob, re.DOTALL):
            print(f"  RVA 0x{va + m.start():08X}   VA 0x{base + va + m.start():X}  ({nm})")
            total += 1
            if total > 400:
                print("  ... truncated")
                sys.exit(0)
    print(f"  total {total}")
