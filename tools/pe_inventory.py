#!/usr/bin/env python3
"""
PE inventory: headers, sections, characteristics, imports, exports, debug dir,
rich header, and packing heuristics for any PE file.

Session 1 (GRW-XR). Read-only. Never writes to the game install.

Usage:
    python tools/pe_inventory.py <pe-file> [--out docs/RAW/pe-inventory.txt]

Everything reported here is derived directly from the file on disk, so every
line of output qualifies as [VERIFIED] evidence.
"""
import argparse
import hashlib
import math
import os
import sys
from collections import Counter

import pefile

# DllCharacteristics bits we care about for injection / hooking feasibility.
DLL_CHARACTERISTICS = [
    (0x0020, "HIGH_ENTROPY_VA", "64-bit ASLR entropy"),
    (0x0040, "DYNAMIC_BASE", "ASLR enabled: image base is randomised"),
    (0x0080, "FORCE_INTEGRITY", "code integrity checks enforced"),
    (0x0100, "NX_COMPAT", "DEP enabled"),
    (0x0200, "NO_ISOLATION", ""),
    (0x0400, "NO_SEH", "no structured exception handling"),
    (0x0800, "NO_BIND", ""),
    (0x1000, "APPCONTAINER", ""),
    (0x2000, "WDM_DRIVER", ""),
    (0x4000, "GUARD_CF", "Control Flow Guard: indirect calls validated"),
    (0x8000, "TERMINAL_SERVER_AWARE", ""),
]

SECTION_CHARACTERISTICS = [
    (0x00000020, "CNT_CODE"),
    (0x00000040, "CNT_INITIALIZED_DATA"),
    (0x00000080, "CNT_UNINITIALIZED_DATA"),
    (0x02000000, "MEM_DISCARDABLE"),
    (0x04000000, "MEM_NOT_CACHED"),
    (0x08000000, "MEM_NOT_PAGED"),
    (0x10000000, "MEM_SHARED"),
    (0x20000000, "MEM_EXECUTE"),
    (0x40000000, "MEM_READ"),
    (0x80000000, "MEM_WRITE"),
]

MACHINE = {0x8664: "x86-64 (AMD64)", 0x014C: "x86 (i386)", 0xAA64: "ARM64"}


def entropy(data: bytes) -> float:
    """Shannon entropy in bits/byte. >7.2 on a code section suggests packing."""
    if not data:
        return 0.0
    counts = Counter(data)
    n = len(data)
    return -sum((c / n) * math.log2(c / n) for c in counts.values())


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def flags(value: int, table) -> str:
    return ", ".join(name for bit, name, *_ in table if value & bit) or "(none)"


def report(path: str, out):
    w = lambda s="": print(s, file=out)

    st = os.stat(path)
    w("=" * 78)
    w(f"PE INVENTORY: {path}")
    w("=" * 78)
    w(f"size            : {st.st_size} bytes ({st.st_size / (1 << 20):.2f} MiB)")
    w(f"sha256          : {sha256(path)}")
    w("")

    # fast_load=False so imports/exports/debug/resources are parsed.
    pe = pefile.PE(path, fast_load=False)

    fh = pe.FILE_HEADER
    oh = pe.OPTIONAL_HEADER
    w("--- FILE HEADER ---")
    w(f"machine         : 0x{fh.Machine:04X}  {MACHINE.get(fh.Machine, 'unknown')}")
    w(f"num sections    : {fh.NumberOfSections}")
    import datetime as _dt

    ts = _dt.datetime.fromtimestamp(fh.TimeDateStamp, _dt.timezone.utc)
    w(f"timestamp       : 0x{fh.TimeDateStamp:08X}  {ts.isoformat()}")
    w(f"characteristics : 0x{fh.Characteristics:04X}")
    w(f"symbol table    : ptr=0x{fh.PointerToSymbolTable:X} count={fh.NumberOfSymbols}")
    w("")

    w("--- OPTIONAL HEADER ---")
    w(f"magic           : 0x{oh.Magic:04X} ({'PE32+' if oh.Magic == 0x20B else 'PE32'})")
    w(f"image base      : 0x{oh.ImageBase:016X}")
    w(f"entry point RVA : 0x{oh.AddressOfEntryPoint:08X}")
    w(f"size of image   : 0x{oh.SizeOfImage:X} ({oh.SizeOfImage})")
    w(f"section align   : 0x{oh.SectionAlignment:X}   file align: 0x{oh.FileAlignment:X}")
    w(f"subsystem       : {oh.Subsystem}")
    w(f"linker version  : {oh.MajorLinkerVersion}.{oh.MinorLinkerVersion}")
    w(f"os version      : {oh.MajorOperatingSystemVersion}.{oh.MinorOperatingSystemVersion}")
    w(f"stack reserve   : 0x{oh.SizeOfStackReserve:X}")
    w(f"dll characteris.: 0x{oh.DllCharacteristics:04X}")
    for bit, name, note in DLL_CHARACTERISTICS:
        if oh.DllCharacteristics & bit:
            w(f"                  [x] {name:24s} {note}")
    w("")
    w("ASLR (DYNAMIC_BASE) : " + ("ON  -> all addresses must be module-base relative"
                                  if oh.DllCharacteristics & 0x0040 else
                                  "OFF -> absolute addresses are stable"))
    w("CFG  (GUARD_CF)     : " + ("ON  -> indirect-call targets validated; inline detours "
                                  "of *called-through-pointer* functions may trip the guard"
                                  if oh.DllCharacteristics & 0x4000 else
                                  "OFF -> no control-flow-guard constraints on hooking"))
    w("")

    w("--- SECTIONS ---")
    w(f"{'name':10s} {'VA':>10s} {'VSize':>10s} {'RawPtr':>10s} {'RawSize':>10s} "
      f"{'Entropy':>8s}  characteristics")
    total_code = 0
    for s in pe.sections:
        name = s.Name.rstrip(b"\x00").decode("latin-1")
        data = s.get_data()
        e = entropy(data[: 1 << 24])  # cap at 16 MiB for speed
        if s.Characteristics & 0x20000000:
            total_code += s.Misc_VirtualSize
        w(f"{name:10s} 0x{s.VirtualAddress:08X} 0x{s.Misc_VirtualSize:08X} "
          f"0x{s.PointerToRawData:08X} 0x{s.SizeOfRawData:08X} {e:8.3f}  "
          f"{flags(s.Characteristics, SECTION_CHARACTERISTICS)}")
    w("")
    w(f"total executable bytes: {total_code} ({total_code / (1 << 20):.2f} MiB)")
    w("")

    w("--- PACKING / OBFUSCATION HEURISTICS ---")
    names = [s.Name.rstrip(b"\x00").decode("latin-1") for s in pe.sections]
    standard = {".text", ".rdata", ".data", ".pdata", ".rsrc", ".reloc", ".tls",
                ".idata", ".edata", ".bss", ".didat", ".gfids", ".xdata", ".00cfg",
                ".voltbl", ".textbss", "_RDATA", ".CRT"}
    odd = [n for n in names if n not in standard]
    w(f"non-standard section names : {odd if odd else '(none)'}")
    for s in pe.sections:
        n = s.Name.rstrip(b"\x00").decode("latin-1")
        if s.Characteristics & 0x20000000:
            e = entropy(s.get_data()[: 1 << 24])
            verdict = "HIGH (possible packing)" if e > 7.2 else "normal for compiled code"
            w(f"code section {n:10s} entropy {e:.3f} -> {verdict}")
    w(f"raw size vs image size     : {st.st_size} / {oh.SizeOfImage} = "
      f"{st.st_size / max(oh.SizeOfImage, 1):.3f}")
    w("(a tiny raw file with a huge SizeOfImage is the classic packer signature)")
    w("")

    w("--- DATA DIRECTORIES (non-empty) ---")
    dd_names = ["EXPORT", "IMPORT", "RESOURCE", "EXCEPTION", "SECURITY", "BASERELOC",
                "DEBUG", "ARCHITECTURE", "GLOBALPTR", "TLS", "LOAD_CONFIG",
                "BOUND_IMPORT", "IAT", "DELAY_IMPORT", "CLR_HEADER", "RESERVED"]
    for i, d in enumerate(oh.DATA_DIRECTORY):
        if d.VirtualAddress or d.Size:
            label = dd_names[i] if i < len(dd_names) else str(i)
            w(f"{label:15s} rva=0x{d.VirtualAddress:08X} size=0x{d.Size:X}")
    w("")

    w("--- IMPORTS ---")
    dlls = []
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for entry in pe.DIRECTORY_ENTRY_IMPORT:
            dll = entry.dll.decode("latin-1")
            dlls.append(dll.lower())
            w(f"\n[{dll}]  ({len(entry.imports)} imports)  IAT rva=0x{entry.struct.FirstThunk:08X}")
            for imp in entry.imports:
                nm = imp.name.decode("latin-1") if imp.name else f"#ordinal {imp.ordinal}"
                w(f"    0x{imp.address:016X}  {nm}")
    else:
        w("(no import directory)")
    w("")

    if hasattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT"):
        w("--- DELAY IMPORTS ---")
        for entry in pe.DIRECTORY_ENTRY_DELAY_IMPORT:
            dll = entry.dll.decode("latin-1")
            dlls.append(dll.lower())
            w(f"\n[{dll}]  ({len(entry.imports)} imports)")
            for imp in entry.imports:
                nm = imp.name.decode("latin-1") if imp.name else f"#ordinal {imp.ordinal}"
                w(f"    {nm}")
        w("")

    w("--- IMPORT SUMMARY (hook-relevant) ---")
    interesting = ["d3d11.dll", "dxgi.dll", "d3d12.dll", "d3d9.dll", "xinput1_3.dll",
                   "xinput1_4.dll", "xinput9_1_0.dll", "dinput8.dll", "winmm.dll",
                   "user32.dll", "hid.dll", "setupapi.dll", "openvr_api.dll",
                   "steam_api64.dll", "opengl32.dll", "vulkan-1.dll"]
    for name in interesting:
        w(f"{'IMPORTED' if name in dlls else 'not imported':14s}  {name}")
    w("")

    w("--- EXPORTS ---")
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT") and pe.DIRECTORY_ENTRY_EXPORT.symbols:
        for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            nm = exp.name.decode("latin-1") if exp.name else "(noname)"
            w(f"    ord={exp.ordinal:<5d} rva=0x{exp.address:08X}  {nm}")
    else:
        w("(no exports)")
    w("")

    w("--- DEBUG DIRECTORY ---")
    if hasattr(pe, "DIRECTORY_ENTRY_DEBUG"):
        for dbg in pe.DIRECTORY_ENTRY_DEBUG:
            w(f"type={dbg.struct.Type} size={dbg.struct.SizeOfData} "
              f"rva=0x{dbg.struct.AddressOfRawData:08X}")
            ent = getattr(dbg, "entry", None)
            if ent is not None and hasattr(ent, "PdbFileName"):
                w(f"    PDB path: {ent.PdbFileName.rstrip(b(chr(0))).decode('latin-1', 'replace')}"
                  if False else
                  f"    PDB path: {ent.PdbFileName.rstrip(bytes([0])).decode('latin-1', 'replace')}")
    else:
        w("(no debug directory)")
    w("")

    w("--- VERSION RESOURCE ---")
    found = False
    if hasattr(pe, "FileInfo"):
        for fi_list in pe.FileInfo:
            for fi in fi_list:
                if getattr(fi, "Key", b"") == b"StringFileInfo":
                    for st_entry in fi.StringTable:
                        for k, v in st_entry.entries.items():
                            found = True
                            w(f"    {k.decode('latin-1')} = {v.decode('latin-1')}")
    if not found:
        w("(no StringFileInfo version resource present)")
    w("")

    w("--- LOAD CONFIG (CFG detail) ---")
    lc = getattr(pe, "DIRECTORY_ENTRY_LOAD_CONFIG", None)
    if lc is not None:
        s = lc.struct
        for f in ("GuardCFCheckFunctionPointer", "GuardCFFunctionTable",
                  "GuardCFFunctionCount", "GuardFlags", "SecurityCookie"):
            if hasattr(s, f):
                w(f"    {f} = 0x{getattr(s, f):X}")
    else:
        w("(no load config directory)")

    pe.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pefile")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    if a.out:
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
        with open(a.out, "w", encoding="utf-8") as fh:
            report(a.pefile, fh)
        print(f"wrote {a.out}")
    else:
        report(a.pefile, sys.stdout)


if __name__ == "__main__":
    main()
