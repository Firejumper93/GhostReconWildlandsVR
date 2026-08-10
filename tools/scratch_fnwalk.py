#!/usr/bin/env python3
"""Read-only: find the .pdata FRAGMENT containing an RVA, linearly disassemble it,
and verify an instruction starts exactly at the RVA. Prints the fragment listing."""
import sys, bisect, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = sys.argv[1]
pe = pefile.PE(path, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(path, "rb").read()
secs = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData),
         s.PointerToRawData, s.Name.rstrip(b"\x00").decode("latin-1")) for s in pe.sections]

def off(rva):
    for a, b, p, n in secs:
        if a <= rva < b:
            return p + (rva - a), n
    return None, None

d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
po, _ = off(d.VirtualAddress)
raw = data[po:po + d.Size]
n = len(raw) // 12
ents = [struct.unpack_from("<III", raw, i*12) for i in range(n)]
ents = [e for e in ents if e[0]]
starts = [e[0] for e in ents]

md = Cs(CS_ARCH_X86, CS_MODE_64)

mode = sys.argv[2] if len(sys.argv) > 2 else "walk"
args = sys.argv[3:]

def frag(rva):
    i = bisect.bisect_right(starts, rva) - 1
    if i < 0: return None
    b, e, u = ents[i]
    if b <= rva < e: return (b, e)
    return None

for spec in args:
    rva = int(spec, 0)
    f = frag(rva)
    print(f"\n===== site 0x{rva:08X} =====")
    if not f:
        print("  no pdata fragment")
        continue
    b, e = f
    print(f"  fragment 0x{b:08X}..0x{e:08X}")
    o, _ = off(b)
    ok = False
    lines = []
    for ins in md.disasm(data[o:o + (e - b)], base + b):
        r = ins.address - base
        mark = " <<<" if r == rva else ""
        if r == rva: ok = True
        lines.append(f"0x{r:08X}  {ins.mnemonic:<9} {ins.op_str}{mark}")
    print("  aligned:", ok)
    for l in lines:
        print("   ", l)
