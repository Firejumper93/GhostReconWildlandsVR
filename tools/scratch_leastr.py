#!/usr/bin/env python3
"""Read-only: scan an RVA range for `lea r64,[rip+d32]` whose target is ASCII text."""
import sys, struct
import pefile
path = sys.argv[1]; lo = int(sys.argv[2],0); hi = int(sys.argv[3],0)
pe = pefile.PE(path, fast_load=True)
data = open(path,"rb").read()
secs = [(s.VirtualAddress, s.VirtualAddress+max(s.Misc_VirtualSize,s.SizeOfRawData), s.PointerToRawData) for s in pe.sections]
def off(r):
    for a,b,p in secs:
        if a<=r<b: return p+(r-a)
    return None
def stra(r):
    o=off(r)
    if o is None: return None
    s=b""
    for k in range(o, min(o+200,len(data))):
        c=data[k]
        if 32<=c<127: s+=bytes([c])
        elif c==0: break
        else: return None
    return s.decode() if len(s)>=6 else None
o_lo = off(lo); o_hi = off(hi)
seen=set()
i=o_lo
while i < o_hi-7:
    b0=data[i]
    if b0 in (0x48,0x4C) and data[i+1]==0x8D and (data[i+2]&0xC7)==0x05:
        d=struct.unpack_from("<i",data,i+3)[0]
        rva = lo + (i-o_lo) + 7 + d
        s=stra(rva)
        if s and s not in seen:
            seen.add(s)
            print(f"0x{lo+(i-o_lo):08X} -> 0x{rva:08X}  \"{s}\"")
    i+=1
