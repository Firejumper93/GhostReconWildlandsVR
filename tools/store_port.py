#!/usr/bin/env python3
"""
Store-build port scanner (ISSUE-foreign-store-builds.md).

Derives, against the tester's Ubisoft/Epic GRW.exe, every game RVA the mod
hardcodes for the pinned Steam binary. Nothing is transferred numerically:

  bodies  a curated AOB where one exists (the four RE-notes signatures);
          otherwise a FINGERPRINT generated from the Steam bytes at the
          documented RVA: capstone walk, wildcarding rel32 call/jmp targets,
          rip-relative disp32s and image-range imm64s, grown one instruction
          at a time until it hits exactly once in BOTH binaries (and at the
          documented RVA in Steam, the positive control).
  thunks  never searched for by pattern: mechanical scan for a 5-byte
          `E9 rel32` resolving to the found body, with the int3-slot
          context reported (the build-1 hook family invariant).
  stubs   SetYaw/SetPitch virtual-dispatch stubs, exact 10-byte sequence
          inside an int3 slot.
  data    the head-hide method table, found by scanning for the absolute VA
          of the found setter thunk stored at slot +0x1F0; Ansel IAT slots
          from the import directory.

Every row prints its hit count. Anything that is not exactly-one is a MISS
to resolve by hand, never a guess (project RE rule 2).

Usage:
    python tools/store_port.py <steam-exe> <store-exe>
"""
import re
import struct
import sys

import pefile
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

BASE = 0x140000000

# The 11 kTargets rows (CameraProbe.cpp) plus the weapon setter and the
# head-hide setter: (name, steam_thunk_rva or None, steam_fn_rva).
TARGETS = [
    ("proj[0] anchor",      0x01347280, 0x0C50C0E0),
    ("proj[1]",             0x01347460, 0x0C50C2E0),
    ("proj[2] gameplay",    0x01347530, 0x0C50C420),
    ("proj[3] skew",        0x01347840, 0x0C50C7E0),
    ("proj[4]",             0x01345720, 0x0C5094D0),
    ("proj[5]",             0x01345800, 0x0C509720),
    ("on_calc_mvp",         0x0135F720, 0x0C5E47E0),
    ("selector",            0x01349DF0, 0x0C510B20),
    ("SkeletonPostUpdate",  0x01865A10, 0x0DA1A990),
    ("HIK datablock rdr",   0x018BE500, 0x0DC4F9B0),
    ("OnInit callee",       0x02713160, 0x114A6DE0),
    ("weapon setterA",      0x030AC6A0, 0x13E5EA30),
    ("weapon setterB",      None,       0x13E5FDE0),
    ("bone gather consumer", None,      0x0DDD12D0),
    ("head-hide slot fn",   None,       0x124E15A0),
    ("head-hide setter",    0x029DC7D0, 0x12582AC0),
    # Aim family (GameBuild.cpp Steam row, extension owed by the 2026-08-06
    # update triage): fingerprint rows, thunks by the mechanical E9 scan.
    ("wfire",               0x029AB510, 0x124B8360),
    ("hknp castRay",        0x030B08E0, 0x13E68070),
    ("GetAimOrientation",   0x029A8E80, 0x124B0770),
    ("proj spawn",          0x02986B20, 0x12458BD0),
    ("TtCastRay",           0x030C9990, 0x13EA3550),
    # Mid-function anchors: the getter stubs (int3-slot dispatch stubs) and
    # the per-shot aim read sites (return addresses inside wfire). AimTrace
    # re-verifies the E8 in front of each site at runtime before trusting.
    ("getyaw stub",         None,       0x006764B0),
    ("getpitch stub",       None,       0x00677600),
    ("shot yaw site",       None,       0x124B855D),
    ("shot pitch site",     None,       0x124B85A1),
]

# Curated AOBs from src/ and docs/RE-notes.md, all verified unique in Steam.
AOBS = {
    "proj[0] anchor":
        "48 89 E0 53 48 81 EC 90 00 00 00 0F 29 70 E8 48 89 CB F3",
    "weapon setterA":
        "48 89 5C 24 18 89 54 24 10 55 56 57 48 83 EC 40 F3 0F 10 05 "
        "?? ?? ?? ?? 89 D0 4C 89 C7 F3 0F 11 44 24 78 25 FF FF FF 00",
    "weapon setterB":
        "48 89 6C 24 18 48 89 74 24 20 89 54 24 10 57 48 83 EC 40 89 D0 "
        "4C 89 CD 25 FF FF FF 00 4C 89 C6 4C 69 D0 B0 00 00 00",
    "bone gather consumer":
        "48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 "
        "EC 90 00 00 00 48 8B 41 10 48 89 CE 49 89 D4 4C 8B 40 60 4D 8B "
        "B0 08 0D 00 00",
    "head-hide slot fn":
        "48 89 5C 24 08 57 48 83 EC 20 48 83 7A 20 00 48 89 D3 48 89 CF "
        "74 ?? 49 89 D0 31 D2 E8",
    "head-hide setter":
        "48 83 EC 08 44 0F B6 DA 49 89 C9 38 51 68 74 ?? 44 0F B7 51 4A",
}

# Standalone byte-pattern rows: (name, steam_expected_rva, pattern).
PATTERNS = [
    ("SetYaw stub",    0x006777C0, "48 8B 01 48 FF A0 70 05 00 00"),
    ("SetPitch stub",  0x005FA190, "48 8B 01 48 FF A0 D0 05 00 00"),
    ("no-blur match",  0x124DE4CC, "45 89 E6 40 B6 01 E8 ?? ?? ?? ?? 3D F3 46 68 82"),
    ("accuracy cmp",   0x123161ED, "80 BB F4 00 00 00 00 C6 83 E4 00 00 00 01"),
]

HEAD_TABLE_STEAM = 0x04A66410   # slot +0x1F0 holds the setter thunk VA
HEAD_TABLE_SLOT  = 0x1F0


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.data = open(path, "rb").read()
        self.secs = [(s.PointerToRawData, s.PointerToRawData + s.SizeOfRawData,
                      s.VirtualAddress) for s in self.pe.sections]

    def off2rva(self, o):
        for lo, hi, va in self.secs:
            if lo <= o < hi:
                return va + (o - lo)
        return None

    def rva2off(self, r):
        for lo, hi, va in self.secs:
            if va <= r < va + (hi - lo):
                return lo + (r - va)
        return None


def pat_to_re(p):
    out = b""
    for tok in p.split():
        out += b"." if tok in ("??", "?") else re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.DOTALL)


def scan(img, pattern):
    return [img.off2rva(m.start()) for m in pat_to_re(pattern).finditer(img.data)]


MD = Cs(CS_ARCH_X86, CS_MODE_64)
MD.detail = True


def fingerprint(img, rva, n_insns):
    """Pattern for the first n_insns at rva, wildcarding relocatable bytes."""
    off = img.rva2off(rva)
    code = img.data[off:off + 16 * n_insns]
    toks = []
    count = 0
    for insn in MD.disasm(code, BASE + rva):
        wild = set()
        # rel32/rel8 call and jump targets move between builds.
        if insn.group(1) or insn.group(7):     # CS_GRP_JUMP=1, CS_GRP_CALL=7
            for op in insn.operands:
                if op.type == X86_OP_IMM and insn.imm_size:
                    wild.update(range(insn.imm_offset,
                                      insn.imm_offset + insn.imm_size))
        for op in insn.operands:
            # rip-relative displacements move between builds.
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and insn.disp_size:
                wild.update(range(insn.disp_offset,
                                  insn.disp_offset + insn.disp_size))
            # absolute image addresses in immediates move between builds.
            if (op.type == X86_OP_IMM and insn.imm_size >= 4 and
                    BASE <= op.imm < BASE + 0x20000000):
                wild.update(range(insn.imm_offset,
                                  insn.imm_offset + insn.imm_size))
        for i, b in enumerate(insn.bytes):
            toks.append("??" if i in wild else "%02X" % b)
        count += 1
        if count >= n_insns:
            break
    return " ".join(toks)


def find_body(steam, store, name, steam_rva):
    """AOB if curated, else fingerprint grown until unique in both images."""
    if name in AOBS:
        pat, how = AOBS[name], "aob"
        s_hits = scan(steam, pat)
        t_hits = scan(store, pat)
        return pat, how, s_hits, t_hits
    for n in (12, 18, 26, 40):
        pat = fingerprint(steam, steam_rva, n)
        s_hits = scan(steam, pat)
        if len(s_hits) != 1 or s_hits[0] != steam_rva:
            continue
        t_hits = scan(store, pat)
        if len(t_hits) == 1:
            return pat, "fp%d" % n, s_hits, t_hits
    return pat, "fp-max", s_hits, scan(store, pat)


def find_thunks(img, body_rva):
    """Every E9 rel32 in the image resolving to body_rva, with slot context."""
    target_hits = []
    for m in re.finditer(b"\xE9", img.data):
        o = m.start()
        rva = img.off2rva(o)
        if rva is None or o + 5 > len(img.data):
            continue
        rel = struct.unpack_from("<i", img.data, o + 1)[0]
        if rva + 5 + rel == body_rva:
            before = img.data[o - 1:o]
            after = img.data[o + 5:o + 6]
            slot = (before == b"\xCC" or rva % 16 == 0) and after == b"\xCC"
            target_hits.append((rva, slot))
    return target_hits


def main():
    steam = Image(sys.argv[1])
    store = Image(sys.argv[2])
    print("steam TimeDateStamp=%08X SizeOfImage=%08X" % (
        steam.pe.FILE_HEADER.TimeDateStamp, steam.pe.OPTIONAL_HEADER.SizeOfImage))
    print("store TimeDateStamp=%08X SizeOfImage=%08X" % (
        store.pe.FILE_HEADER.TimeDateStamp, store.pe.OPTIONAL_HEADER.SizeOfImage))

    found_bodies = {}
    print("\n== bodies ==")
    for name, steam_thunk, steam_fn in TARGETS:
        pat, how, s_hits, t_hits = find_body(steam, store, name, steam_fn)
        ok_s = len(s_hits) == 1 and s_hits[0] == steam_fn
        status = "OK" if (ok_s and len(t_hits) == 1) else "MISS"
        t_str = ", ".join("0x%08X" % h for h in t_hits) or "-"
        print("  %-20s %-6s steam=%s(%d hit) store=%s(%d hit)  %s" % (
            name, how, "0x%08X" % steam_fn, len(s_hits), t_str, len(t_hits),
            status))
        if status == "OK":
            found_bodies[name] = t_hits[0]

    print("\n== thunks (mechanical E9 scan in the store image) ==")
    for name, steam_thunk, steam_fn in TARGETS:
        if steam_thunk is None or name not in found_bodies:
            continue
        hits = find_thunks(store, found_bodies[name])
        ctrl = find_thunks(steam, steam_fn)
        ctrl_ok = any(r == steam_thunk for r, _ in ctrl)
        h_str = ", ".join("0x%08X%s" % (r, "" if s else "(!slot)")
                          for r, s in hits) or "NONE"
        print("  %-20s steam_thunk=0x%08X(ctrl %s, %d hits) store=%s" % (
            name, steam_thunk, "OK" if ctrl_ok else "FAIL", len(ctrl), h_str))

    print("\n== standalone patterns ==")
    for name, steam_rva, pat in PATTERNS:
        s_hits = scan(steam, pat)
        t_hits = scan(store, pat)
        ok_s = len(s_hits) == 1 and s_hits[0] == steam_rva
        print("  %-20s steam=%s(%d hit, ctrl %s) store=%s(%d hit)" % (
            name, "0x%08X" % steam_rva, len(s_hits), "OK" if ok_s else "FAIL",
            ", ".join("0x%08X" % h for h in t_hits) or "-", len(t_hits)))

    print("\n== head-hide method table (data scan for the setter thunk VA) ==")
    if "head-hide setter" in found_bodies:
        thunks = [r for r, _ in find_thunks(store, found_bodies["head-hide setter"])]
        for cand_rva in thunks + [found_bodies["head-hide setter"]]:
            needle = struct.pack("<Q", BASE + cand_rva)
            hits = [store.off2rva(m.start())
                    for m in re.finditer(re.escape(needle), store.data)]
            for h in hits:
                if h is None:
                    continue
                print("  VA-of 0x%08X stored at 0x%08X -> table 0x%08X "
                      "(steam table 0x%08X)" % (
                          cand_rva, h, h - HEAD_TABLE_SLOT, HEAD_TABLE_STEAM))

    print("\n== Ansel IAT (import directory) ==")
    for img, tag in ((steam, "steam"), (store, "store")):
        img.pe.parse_data_directories(
            directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
        for entry in getattr(img.pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = entry.dll.decode(errors="replace").lower()
            if "ansel" not in dll:
                continue
            for imp in entry.imports:
                nm = (imp.name or b"?").decode(errors="replace")
                print("  %s %-40s slot rva 0x%08X" % (
                    tag, dll + "!" + nm, imp.address - BASE))


if __name__ == "__main__":
    main()
