#!/usr/bin/env python3
"""
Exhaustive reference finder for a data RVA in a PE image.

WHY THIS EXISTS (session 19). Two reference scanners in this tree produce FALSE
NEGATIVES, and both had already been used to declare things "not referenced":

  * tools/find_dataxrefs.py only recognises `lea r64,[rip+d32]` and
    `mov r64,[rip+d32]`. It MISSES `call/jmp qword ptr [rip+d32]` entirely, so
    any function pointer reached through a table or an indirect call looks
    unreferenced.
  * extracted/atk_decomp/scratch/scan_disp.py (and dispscan2.py) decode
    backwards from the displacement bytes and accept the first decode that
    covers them, which reports 64-bit loads as 32-bit. Its empty results are
    meaningless. Ground truth: a masked AOB scan for one field found 144 real
    qword loads where that scanner reported zero.

An "X is never referenced" claim is expensive when it is wrong: it closes a
line of enquiry that was actually open. So this tool does not try to understand
instructions at all. It scans every executable section for ANY four-byte
displacement that resolves to the target, whatever the opcode:

    for every byte offset i:  if rva(i) + 4 + int32(bytes[i:i+4]) == target

That is the defining property of every x64 rip-relative reference, so the scan
cannot miss one by failing to know an encoding. It over-reports instead of
under-reporting, which is the correct direction for a negative result: each hit
is then disassembled backwards so real instructions can be told from noise.

It also scans the whole file for absolute 8-byte pointers to the target, since
ASLR is off and the image loads at its preferred base, so a stored pointer to
RVA X is literally the qword 0x140000000 + X.

ALWAYS RUN A POSITIVE CONTROL. Pass --control with an RVA known to be
referenced; if the control comes back empty the scan is broken and its negative
for the real target means nothing.

Read-only.

Usage:
    python tools/xref_all.py <pe-file> 0x04AC1E30
    python tools/xref_all.py <pe-file> 0x04AC1E30 --control 0x04409780
    python tools/xref_all.py <pe-file> 0x04AC1E30 --context 24
"""
import argparse
import struct

import numpy as np
import pefile

try:
    from capstone import CS_ARCH_X86, CS_MODE_64, Cs
    HAVE_CS = True
except ImportError:
    HAVE_CS = False

IMAGE_SCN_MEM_EXECUTE = 0x20000000


def sections(pe):
    out = []
    for s in pe.sections:
        out.append({
            "name": s.Name.rstrip(b"\x00").decode("latin-1", "replace"),
            "va": s.VirtualAddress,
            "vsize": max(s.Misc_VirtualSize, s.SizeOfRawData),
            "raw": s.PointerToRawData,
            "rawsize": s.SizeOfRawData,
            "exec": bool(s.Characteristics & IMAGE_SCN_MEM_EXECUTE),
        })
    return out


def rip_refs(data, secs, target):
    """Every 4-byte displacement in an executable section resolving to target.

    Vectorised: the naive per-byte Python loop is about 250 million iterations
    per section per tail, which does not finish in useful time on a 369 MB
    image. For each of the four byte alignments, view the section as an int32
    array (positions p = k + 4j are exactly the ones that array covers), and
    solve `va + p + disp == base` as `disp + 4j == base - va - k` with one
    vector compare per alignment.
    """
    hits = []
    for s in secs:
        if not s["exec"]:
            continue
        blob = data[s["raw"]:s["raw"] + s["rawsize"]]
        n = len(blob)
        if n < 8:
            continue
        for tail in (4, 5, 6, 8):
            base = target - tail
            for k in range(4):
                usable = (n - k) // 4 * 4
                if usable < 4:
                    continue
                arr = np.frombuffer(blob[k:k + usable], dtype="<i4").astype(np.int64)
                want = base - s["va"] - k
                lhs = arr + 4 * np.arange(arr.size, dtype=np.int64)
                for j in np.nonzero(lhs == want)[0]:
                    p = k + 4 * int(j)
                    hits.append((s["va"] + p, s["name"], int(arr[j]), tail))
    hits.sort()
    return hits


def abs_ptrs(data, secs, target, image_base):
    """Stored absolute pointers to the target, anywhere in the file."""
    needle = struct.pack("<Q", image_base + target)
    out = []
    start = 0
    while True:
        j = data.find(needle, start)
        if j < 0:
            break
        rva = None
        for s in secs:
            if s["raw"] <= j < s["raw"] + s["rawsize"]:
                rva = s["va"] + (j - s["raw"])
                name = s["name"]
                break
        out.append((j, rva, name if rva is not None else "?"))
        start = j + 1
    return out


def disasm_back(data, secs, rva, context):
    """Disassemble a window ending at rva so a hit can be judged by eye."""
    if not HAVE_CS:
        return []
    for s in secs:
        if s["va"] <= rva < s["va"] + s["vsize"]:
            off = s["raw"] + (rva - s["va"])
            start = max(0, off - context)
            blob = data[start:off + 8]
            md = Cs(CS_ARCH_X86, CS_MODE_64)
            base = rva - (off - start)
            return [(i.address, i.mnemonic, i.op_str)
                    for i in md.disasm(blob, base)]
    return []


def run(pe, data, secs, target, image_base, context, label):
    print("=== %s RVA 0x%08X ===" % (label, target))
    rips = rip_refs(data, secs, target)
    absp = abs_ptrs(data, secs, target, image_base)
    print("  rip-relative displacement hits: %d" % len(rips))
    for rva, sec, d, tail in rips[:40]:
        print("    0x%08X  [%s]  disp=0x%08X tail=%d" % (rva, sec, d & 0xFFFFFFFF, tail))
        for a, m, o in disasm_back(data, secs, rva, context):
            mark = "  <<<" if a <= rva < a + 16 else ""
            print("        0x%08X  %-8s %s%s" % (a, m, o, mark))
    if len(rips) > 40:
        print("    ... %d more" % (len(rips) - 40))
    print("  absolute stored pointers: %d" % len(absp))
    for off, rva, sec in absp[:40]:
        print("    file 0x%08X  rva %s  [%s]"
              % (off, ("0x%08X" % rva) if rva is not None else "?", sec))
    if len(absp) > 40:
        print("    ... %d more" % (len(absp) - 40))
    print()
    return len(rips), len(absp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pefile")
    ap.add_argument("target")
    ap.add_argument("--control", default=None,
                    help="an RVA KNOWN to be referenced, to prove the scan works")
    ap.add_argument("--context", type=int, default=16,
                    help="bytes of backward disassembly context per hit")
    a = ap.parse_args()

    pe = pefile.PE(a.pefile, fast_load=True)
    data = pe.__data__
    secs = sections(pe)
    image_base = pe.OPTIONAL_HEADER.ImageBase

    if a.control:
        cr, ca = run(pe, data, secs, int(a.control, 0), image_base,
                     a.context, "POSITIVE CONTROL")
        if cr == 0 and ca == 0:
            print("!! CONTROL FOUND NOTHING. The scan is broken; any negative "
                  "below is meaningless.\n")

    run(pe, data, secs, int(a.target, 0), image_base, a.context, "TARGET")


if __name__ == "__main__":
    main()
