#!/usr/bin/env python3
"""
Find the enclosing function for an RVA using the PE exception directory (.pdata).

On x64 Windows every non-leaf function must have a RUNTIME_FUNCTION entry
(BeginAddress, EndAddress, UnwindData) so the OS can unwind through it. The
entries are sorted by BeginAddress, so a binary search gives the exact function
boundaries containing any code RVA. This is authoritative in a way that
backward prologue-scanning is not, and it works on GRW.exe because Denuvo must
keep unwind data valid for exceptions to work.

Handles chained unwind info (UNW_FLAG_CHAININFO): a chained entry describes a
non-contiguous fragment of a parent function, and this tool follows the chain
to the primary entry so it reports the true function start.

Read-only.

Usage:
    python tools/pdata_lookup.py <pe-file> 0x0C510DF8 [more RVAs...]
"""
import argparse
import bisect
import struct

import pefile

UNW_FLAG_CHAININFO = 0x4


def rva_to_off(pe, rva):
    for s in pe.sections:
        start = s.VirtualAddress
        end = start + max(s.Misc_VirtualSize, s.SizeOfRawData)
        if start <= rva < end:
            return s.PointerToRawData + (rva - start)
    return None


def load_pdata(pe, data):
    d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY[
        "IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
    off = rva_to_off(pe, d.VirtualAddress)
    raw = data[off:off + d.Size]
    n = len(raw) // 12
    entries = [struct.unpack_from("<III", raw, i * 12) for i in range(n)]
    # entries may have zero-padding at the tail
    entries = [e for e in entries if e[0] != 0]
    return entries


def unwind_flags(pe, data, unwind_rva):
    off = rva_to_off(pe, unwind_rva)
    return data[off] >> 3  # low 3 bits are version, high 5 are flags


def chained_parent(pe, data, unwind_rva):
    """For UNW_FLAG_CHAININFO, the parent RUNTIME_FUNCTION follows the
    unwind-code array, aligned to 2 slots."""
    off = rva_to_off(pe, unwind_rva)
    count_codes = data[off + 2]
    parent_off = off + 4 + ((count_codes + 1) & ~1) * 2
    return struct.unpack_from("<III", data, parent_off)


def lookup(pe, data, entries, begins, rva):
    i = bisect.bisect_right(begins, rva) - 1
    if i < 0:
        return None
    e = entries[i]
    if not (e[0] <= rva < e[1]):
        return None
    hops = 0
    while unwind_flags(pe, data, e[2]) & UNW_FLAG_CHAININFO and hops < 32:
        e = chained_parent(pe, data, e[2])
        hops += 1
    return e, hops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pefile")
    ap.add_argument("rvas", nargs="+")
    a = ap.parse_args()

    pe = pefile.PE(a.pefile, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    with open(a.pefile, "rb") as fh:
        data = fh.read()

    entries = load_pdata(pe, data)
    begins = [e[0] for e in entries]
    print(f"exception directory: {len(entries)} RUNTIME_FUNCTION entries")
    print()

    for r in a.rvas:
        rva = int(r, 0)
        hit = lookup(pe, data, entries, begins, rva)
        if hit is None:
            print(f"RVA 0x{rva:08X}: NOT inside any RUNTIME_FUNCTION "
                  f"(leaf function, or pdata gap)")
            continue
        e, hops = hit
        chain = f"  (via {hops} chained fragment(s))" if hops else ""
        print(f"RVA 0x{rva:08X}: function 0x{e[0]:08X} .. 0x{e[1]:08X}"
              f"  size 0x{e[1] - e[0]:X}"
              f"  VA 0x{base + e[0]:016X}{chain}")


if __name__ == "__main__":
    main()
