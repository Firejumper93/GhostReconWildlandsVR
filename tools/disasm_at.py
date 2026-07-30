#!/usr/bin/env python3
"""
Disassemble N instructions at a given RVA of a PE image.

Session 1 (GRW-XR) used this to answer one specific question: is GRW.exe's code
plaintext on disk, or is it encrypted/packed by Denuvo? If a known export's RVA
disassembles into a sane x64 function prologue, then static AOB signature
scanning against the on-disk file is valid and session 2 can derive signatures
offline instead of dumping a running process.

Read-only.

Usage:
    python tools/disasm_at.py <pe-file> 0x0A8959D0 [--count 40]
    python tools/disasm_at.py <pe-file> --export "??0GraphicLibFacade@scimitar@@QEAA@XZ"
"""
import argparse

import pefile
from capstone import CS_ARCH_X86, CS_MODE_64, Cs


def rva_to_off(pe, rva):
    for s in pe.sections:
        start = s.VirtualAddress
        end = start + max(s.Misc_VirtualSize, s.SizeOfRawData)
        if start <= rva < end:
            return s.PointerToRawData + (rva - start), \
                   s.Name.rstrip(b"\x00").decode("latin-1")
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pefile")
    ap.add_argument("rva", nargs="?", default=None)
    ap.add_argument("--export", default=None)
    ap.add_argument("--count", type=int, default=40)
    a = ap.parse_args()

    pe = pefile.PE(a.pefile, fast_load=False)
    base = pe.OPTIONAL_HEADER.ImageBase

    if a.export:
        rva = None
        for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if exp.name and exp.name.decode("latin-1") == a.export:
                rva = exp.address
                break
        if rva is None:
            raise SystemExit(f"export not found: {a.export}")
    else:
        rva = int(a.rva, 0)

    off, sec = rva_to_off(pe, rva)
    if off is None:
        raise SystemExit(f"RVA 0x{rva:X} is not inside any section")
    pe.close()

    with open(a.pefile, "rb") as fh:
        fh.seek(off)
        code = fh.read(16 * a.count)

    print(f"file      : {a.pefile}")
    print(f"RVA       : 0x{rva:08X}   VA 0x{base + rva:016X}")
    print(f"section   : {sec}   file offset 0x{off:X}")
    print(f"raw bytes : {code[:32].hex(' ')}")
    print()

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    n = 0
    bad = 0
    for ins in md.disasm(code, base + rva):
        print(f"0x{ins.address:016X}  {ins.bytes.hex(' '):<28s} {ins.mnemonic} {ins.op_str}")
        n += 1
        if ins.mnemonic in ("(bad)", ".byte"):
            bad += 1
        if n >= a.count:
            break
    print()
    print(f"decoded {n} instructions, {bad} invalid")
    if n < a.count // 2:
        print("VERDICT: decoding stalled early. Bytes are probably NOT plaintext code")
        print("         (encrypted/packed), or the RVA does not point at code.")
    else:
        print("VERDICT: bytes decode cleanly as x64. Code appears to be plaintext")
        print("         on disk, so static AOB signature scanning is valid.")


if __name__ == "__main__":
    main()
