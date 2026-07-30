#!/usr/bin/env python3
"""lea_map.py: resolve rip-relative lea targets in a disasm_at.py dump.

For each `lea reg, [rip +/- disp]` line, compute the target VA, read the
bytes at that RVA from the PE file, and classify the target as STRING
(printable ASCII run >= 4) or CODE/DATA (with the section name). Output is
one line per lea, in address order, so registration sequences read
top-to-bottom. Read-only on the PE file.

Usage: python tools/lea_map.py <pefile> <disasm.txt> [--calls]
  --calls  also list `call 0x...` lines interleaved, to show which
           register call each lea feeds.
"""
import re
import sys

IMAGE_BASE = 0x140000000


def load_sections(pefile):
    import pefile as pf
    pe = pf.PE(pefile, fast_load=True)
    secs = []
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode(errors="replace")
        secs.append((s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData, s.SizeOfRawData, name))
    data = open(pefile, "rb").read()
    return secs, data


def rva_to_off(secs, rva):
    for va, va_end, raw, raw_sz, name in secs:
        if va <= rva < va_end:
            off = rva - va
            if off < raw_sz:
                return raw + off, name
            return None, name
    return None, None


def classify(secs, data, rva):
    off, sec = rva_to_off(secs, rva)
    if off is None:
        return sec or "?", None
    chunk = data[off:off + 96]
    run = b""
    for b in chunk:
        if 0x20 <= b < 0x7F:
            run += bytes([b])
        else:
            break
    if len(run) >= 4:
        return sec, run.decode()
    return sec, None


LINE = re.compile(r"^0x([0-9A-Fa-f]{16})\s+((?:[0-9a-f]{2} )+)\s*(.+)$")
LEA = re.compile(r"lea (\w+), \[rip ([+-]) 0x([0-9a-f]+)\]")
CALL = re.compile(r"call 0x([0-9a-f]+)$")


def main():
    pefile, dump = sys.argv[1], sys.argv[2]
    show_calls = "--calls" in sys.argv
    secs, data = load_sections(pefile)
    for line in open(dump):
        m = LINE.match(line)
        if not m:
            continue
        va = int(m.group(1), 16)
        nbytes = len(m.group(2).split())
        asm = m.group(3).strip()
        lm = LEA.search(asm)
        if lm:
            disp = int(lm.group(3), 16)
            tgt = va + nbytes + (disp if lm.group(2) == "+" else -disp)
            rva = tgt - IMAGE_BASE
            sec, s = classify(secs, data, rva)
            kind = f'STRING "{s}"' if s else f"({sec})"
            print(f"0x{va - IMAGE_BASE:08X}  lea {lm.group(1):3s} -> 0x{rva:08X} {kind}")
        elif show_calls:
            cm = CALL.search(asm)
            if cm:
                tgt = int(cm.group(1), 16) - IMAGE_BASE
                print(f"0x{va - IMAGE_BASE:08X}    call -> 0x{tgt:08X}")


if __name__ == "__main__":
    main()
