#!/usr/bin/env python3
"""
MSVC RTTI recovery: find every RTTITypeDescriptor in the image, demangle the
decorated name, and report class name + RVA.

An MSVC x64 RTTITypeDescriptor looks like:
    +0x00  void*  pVFTable   -> type_info vftable
    +0x08  void*  spare
    +0x10  char   name[]     -> ".?AVCameraNode@@", ".?AUSomeStruct@@", ...

We locate the ".?AV" / ".?AU" / ".?AW" / ".?AT" name blobs in data sections and
back up 0x10 bytes to get the descriptor base. Demangling uses the Windows
DbgHelp UnDecorateSymbolName via ctypes, which is exact for MSVC names; if that
fails we fall back to a crude structural parse.

Session 1 (GRW-XR). Read-only.

Usage:
    python tools/rtti_scan.py <pe-file> --outdir docs/RAW
"""
import argparse
import ctypes
import os
import re
import sys
from collections import Counter

import pefile

# Classes worth flagging for a VR camera / skeleton / weapon port.
FLAG_PATTERNS = {
    "camera": r"camera|view|frustum|projection|lens|fov",
    "skeleton_anim": r"skeleton|bone|joint|anim|rig|pose|skin|ragdoll|humanik|\bhik|effector",
    "weapon_aim": r"weapon|gun|rifle|firearm|ballist|bullet|projectile|aim|recoil|muzzle|scope|sight",
    "render": r"render|graphic|gfx|shader|d3d|dx11|swapchain|rendertarget|postfx|postprocess|texture|material",
    "player_char": r"player|character|avatar|pawn|actor|human|npc|agent",
    "input": r"input|gamepad|controller|keyboard|mouse|binding|action",
    "hud_ui": r"\bhud\b|\bui\b|menu|widget|hudelement|scaleform",
}
FLAG_COMPILED = {k: re.compile(v, re.I) for k, v in FLAG_PATTERNS.items()}

UNDNAME_COMPLETE = 0x0000

# dbghelp exports the ANSI entry point as "UnDecorateSymbolName" (no A suffix);
# only the wide version carries a suffix. Try both so this keeps working if a
# future SDK adds the A alias.
_UnDecorateSymbolName = None
for _name in ("UnDecorateSymbolName", "UnDecorateSymbolNameA"):
    try:
        _fn = getattr(ctypes.WinDLL("dbghelp.dll"), _name)
        _fn.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                        ctypes.c_ulong, ctypes.c_ulong]
        _fn.restype = ctypes.c_ulong
        _UnDecorateSymbolName = _fn
        break
    except Exception:
        continue
if _UnDecorateSymbolName is None:  # pragma: no cover
    print("warning: dbghelp UnDecorateSymbolName unavailable; using fallback "
          "demangler (template names will be approximate)", file=sys.stderr)


def demangle(decorated: str) -> str:
    """
    Decorated RTTI names look like '.?AVCameraNode@@' or
    '.?AV?$Handle@VEntity@@@@'. UnDecorateSymbolName wants a leading '?',
    so we swap the leading '.' for '?' and prefix a fake symbol form.
    """
    if _UnDecorateSymbolName is None:
        return fallback_demangle(decorated)
    # ".?AVFoo@@"  ->  "?Foo@@" is not right; the standard trick is to feed
    # "??_R0" style names as-is is unsupported, so we transform the type name
    # into a data-symbol form that dbghelp understands: "?x@@3" + <type> + "A".
    body = decorated[4:]  # strip ".?AV" / ".?AU" etc -> "Foo@@"
    kind = decorated[3] if len(decorated) > 3 else "V"
    sym = f"?x@@3{kind}{body}A"
    buf = ctypes.create_string_buffer(4096)
    n = _UnDecorateSymbolName(sym.encode("latin-1"), buf, 4096, UNDNAME_COMPLETE)
    if n:
        out = buf.value.decode("latin-1", "replace")
        # dbghelp yields e.g. "class Foo x" -> strip the fake symbol name.
        out = re.sub(r"\s+x$", "", out).strip()
        if out and "?" not in out:
            return out
    return fallback_demangle(decorated)


def fallback_demangle(decorated: str) -> str:
    """Structural parse: '.?AVA@B@C@@' -> 'class C::B::A' (no template support)."""
    kindmap = {"V": "class", "U": "struct", "W": "enum", "T": "union"}
    if len(decorated) < 5 or not decorated.startswith(".?A"):
        return decorated
    kind = kindmap.get(decorated[3], "type")
    body = decorated[4:]
    if body.endswith("@@"):
        body = body[:-2]
    parts = [p for p in body.split("@") if p]
    return f"{kind} " + "::".join(reversed(parts))


NAME_RE = re.compile(rb"\.\?A[VUWT][\x21-\x7e]{1,900}?\x00")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pefile")
    ap.add_argument("--outdir", default="docs/RAW")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)

    pe = pefile.PE(a.pefile, fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    sections = []
    for s in pe.sections:
        nm = s.Name.rstrip(b"\x00").decode("latin-1")
        if s.SizeOfRawData:
            sections.append((nm, s.PointerToRawData,
                             s.PointerToRawData + s.SizeOfRawData, s.VirtualAddress))
    pe.close()

    with open(a.pefile, "rb") as fh:
        data = fh.read()

    found = []
    for name, praw, pend, rva0 in sections:
        # RTTI type descriptors live in .data/.rdata; scan all data sections.
        blob = data[praw:pend]
        for m in NAME_RE.finditer(blob):
            raw = m.group()[:-1].decode("latin-1")
            name_off = praw + m.start()
            name_rva = rva0 + (name_off - praw)
            desc_rva = name_rva - 0x10  # RTTITypeDescriptor base on x64
            found.append((desc_rva, name_rva, name, raw))

    # De-duplicate on decorated name, keeping the first (lowest) RVA.
    seen = {}
    for desc_rva, name_rva, sec, raw in found:
        if raw not in seen:
            seen[raw] = (desc_rva, name_rva, sec)

    print(f"RTTI type descriptors found: {len(found)} raw, {len(seen)} unique",
          file=sys.stderr)

    rows = []
    for raw, (desc_rva, name_rva, sec) in seen.items():
        rows.append((desc_rva, name_rva, sec, raw, demangle(raw)))
    rows.sort()

    all_path = os.path.join(a.outdir, "rtti-all.txt")
    with open(all_path, "w", encoding="utf-8", errors="replace") as out:
        out.write(f"# {a.pefile}\n# ImageBase 0x{image_base:X}\n")
        out.write(f"# {len(rows)} unique RTTI type descriptors\n")
        out.write("# descRVA\tnameRVA\tsection\tdecorated\tdemangled\n")
        for d, n, sec, raw, dem in rows:
            out.write(f"0x{d:08X}\t0x{n:08X}\t{sec}\t{raw}\t{dem}\n")

    flagged = {k: [] for k in FLAG_PATTERNS}
    for d, n, sec, raw, dem in rows:
        for k, pat in FLAG_COMPILED.items():
            if pat.search(dem) or pat.search(raw):
                flagged[k].append((d, n, sec, raw, dem))

    flag_path = os.path.join(a.outdir, "rtti-flagged.txt")
    with open(flag_path, "w", encoding="utf-8", errors="replace") as out:
        out.write(f"# RTTI classes of interest in {a.pefile}\n")
        if not rows:
            out.write("\n*** NO RTTI TYPE DESCRIPTORS FOUND ***\n")
            out.write("RTTI is stripped or disabled (/GR-). Consequence: no class\n")
            out.write("names, no vftable-name mapping. All engine-structure work must\n")
            out.write("proceed by AOB signature and function shape only.\n")
        for k in FLAG_PATTERNS:
            out.write(f"\n{'=' * 74}\n## {k}  ({len(flagged[k])} hits)\n{'=' * 74}\n")
            for d, n, sec, raw, dem in flagged[k]:
                out.write(f"0x{d:08X}\t{sec}\t{dem}\t[{raw}]\n")

    stats_path = os.path.join(a.outdir, "rtti-stats.txt")
    with open(stats_path, "w", encoding="utf-8") as out:
        out.write(f"unique RTTI descriptors: {len(rows)}\n")
        out.write(f"RTTI present: {'YES' if rows else 'NO (stripped)'}\n\n")
        for k in FLAG_PATTERNS:
            out.write(f"  {k:16s} {len(flagged[k])}\n")
        out.write("\nper section:\n")
        for sec, n in Counter(r[2] for r in rows).most_common():
            out.write(f"  {sec:12s} {n}\n")

    print(f"wrote {all_path}\nwrote {flag_path}\nwrote {stats_path}")


if __name__ == "__main__":
    main()
