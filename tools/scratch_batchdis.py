#!/usr/bin/env python3
"""Batch disassembler: load the PE once, disassemble many RVAs. Read-only scratch tool."""
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.VirtualAddress,
         s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData),
         s.PointerToRawData,
         s.Name.rstrip(b"\x00").decode("latin-1")) for s in pe.sections]
data = open(sys.argv[1], "rb").read()
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False


def off(rva):
    for a, b, p, n in secs:
        if a <= rva < b:
            return p + (rva - a), n
    return None, None


for spec in sys.argv[2:]:
    if ":" in spec:
        r, c = spec.split(":")
        cnt = int(c)
    else:
        r, cnt = spec, 40
    rva = int(r, 0)
    o, n = off(rva)
    print(f"\n===== RVA 0x{rva:08X}  VA 0x{base+rva:X}  sect {n} =====")
    if o is None:
        print("  not mapped")
        continue
    for i in md.disasm(data[o:o + cnt * 15], base + rva):
        print(f"0x{i.address:012X}  {i.mnemonic:<10} {i.op_str}")
        cnt -= 1
        if cnt <= 0:
            break
