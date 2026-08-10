#!/usr/bin/env python3
"""Read-only: dump a full function (primary + all chained .pdata fragments) for an RVA,
annotating rip-relative data references with any ASCII string found there."""
import sys, bisect, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = sys.argv[1]
pe = pefile.PE(path, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(path, "rb").read()
secs = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData),
         s.PointerToRawData, s.Name.rstrip(b"\x00").decode("latin-1"),
         bool(s.Characteristics & 0x20000000)) for s in pe.sections]

def off(rva):
    for a,b,p,n,x in secs:
        if a <= rva < b: return p + (rva - a)
    return None
def sect(rva):
    for a,b,p,n,x in secs:
        if a <= rva < b: return n, x
    return None, False

d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
raw = data[off(d.VirtualAddress): off(d.VirtualAddress)+d.Size]
ents = [struct.unpack_from("<III", raw, i*12) for i in range(len(raw)//12)]
ents = [e for e in ents if e[0]]
starts = [e[0] for e in ents]
UNW_CHAIN = 0x4

def unw_primary(i):
    seen = 0
    b,e,u = ents[i]
    while seen < 8:
        uo = off(u)
        if uo is None: return b,e
        ver_flags = data[uo]
        flags = ver_flags >> 3
        if not (flags & UNW_CHAIN): return b,e
        cnt = data[uo+2]
        pos = uo + 4 + ((cnt+1)//2)*4
        cb, ce, cu = struct.unpack_from("<III", data, pos)
        b,e,u = cb,ce,cu
        seen += 1
    return b,e

def frag_index(rva):
    i = bisect.bisect_right(starts, rva)-1
    if i>=0 and ents[i][0] <= rva < ents[i][1]: return i
    return None

def primary_of(rva):
    i = frag_index(rva)
    if i is None: return None
    return unw_primary(i)[0]

def all_frags(prim):
    out = []
    for j,(b,e,u) in enumerate(ents):
        if b == prim:
            out.append((b,e)); continue
        pb,pe_ = unw_primary(j)
        if pb == prim: out.append((b,e))
    return sorted(set(out))

def strat(rva):
    o = off(rva)
    if o is None: return None
    s = b""
    for k in range(o, min(o+120, len(data))):
        c = data[k]
        if 32 <= c < 127: s += bytes([c])
        elif c == 0: break
        else: return None
    if len(s) >= 4: return s.decode()
    return None

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

for spec in sys.argv[2:]:
    rva = int(spec, 0)
    prim = primary_of(rva)
    if prim is None:
        print(f"\n##### 0x{rva:08X}: no pdata"); continue
    frags = all_frags(prim)
    print(f"\n##### function 0x{prim:08X}  ({len(frags)} fragment(s)) for site 0x{rva:08X}")
    for b,e in frags:
        print(f"  --- frag 0x{b:08X}..0x{e:08X}")
        o = off(b)
        for ins in md.disasm(data[o:o+(e-b)], base+b):
            r = ins.address - base
            line = f"0x{r:08X}  {ins.mnemonic:<9} {ins.op_str}"
            ann = ""
            # rip-relative annotation
            if "rip" in ins.op_str:
                for op in ins.operands:
                    if op.type == 3 and op.mem.base == 41:  # X86_REG_RIP
                        t = ins.address + ins.size + op.mem.disp - base
                        nm, ex = sect(t)
                        ann += f"   ; ->0x{t:08X} ({nm})"
                        st = strat(t)
                        if st: ann += f" \"{st}\""
                        elif not ex:
                            oo = off(t)
                            if oo is not None:
                                q = struct.unpack_from("<Q", data, oo)[0]
                                ann += f" q=0x{q:X}"
            if ins.mnemonic in ("call","jmp") and ins.op_str.startswith("0x"):
                ann += "   ; " + f"fn 0x{int(ins.op_str,16)-base:08X}"
            if r == rva: ann += "   <<<<<< SETTER"
            print("   ", line + ann)
