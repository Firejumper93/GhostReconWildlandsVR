#!/usr/bin/env python3
"""Offline verification: do kLastRites2026's RVAs transfer byte-for-byte to the
2026-08-19 update exe (TimeDateStamp 0x6A7C5143)?

Mirrors the FP workstream's F-072 method: for every RVA the mod uses, map
RVA -> file offset in BOTH binaries via their own section tables and compare a
window of raw bytes. Also re-runs the one curated AOB and requires exactly one
hit at the pinned impl RVA.

No guessing: a mismatch prints loudly and the port must not ship.
"""
import re
import struct
import sys

# OLD is your own archived copy of the previous GRW.exe; edit both paths to
# match your machine. No game binary ships with this mod.
OLD = r"D:\your-archive\LastRites-0x6A75F2F4\GRW.exe"
NEW = r"C:\Steam\steamapps\common\Wildlands\GRW.exe"

# Every nonzero RVA in kLastRites2026 (src/GameBuild.cpp), labelled.
# (label, rva, nbytes). Code sites use 64 bytes; data slots/tables sized to fit.
SITES = [
    ("proj0_thunk", 0x01375620, 64), ("proj0_body", 0x0D7BB4A0, 64),
    ("proj1_thunk", 0x01375800, 64), ("proj1_body", 0x0D7BB7C0, 64),
    ("proj2_thunk", 0x013758D0, 64), ("proj2_body", 0x0D7BB920, 64),
    ("proj3_thunk", 0x01375BE0, 64), ("proj3_body", 0x0D7BBCA0, 64),
    ("proj4_thunk", 0x01373AC0, 64), ("proj4_body", 0x0D7B92A0, 64),
    ("proj5_thunk", 0x01373BA0, 64), ("proj5_body", 0x0D7B9430, 64),
    ("selector_thunk", 0x013781B0, 64), ("selector_body", 0x0D7C0610, 64),
    ("skelpost_thunk", 0x01897DF0, 64), ("skelpost_body", 0x0F8160E0, 64),
    ("hikread_thunk", 0x018F1070, 64), ("hikread_body", 0x0F947870, 64),
    ("plyinit_thunk", 0x02751DA0, 64), ("plyinit_body", 0x1403F0B0, 64),
    ("setyaw_stub", 0x003EA960, 16), ("setpitch_stub", 0x00447580, 16),
    ("getyaw_stub", 0x003EACB0, 16), ("getpitch_stub", 0x00446B30, 16),
    ("aim_site_a", 0x14E5BDBB, 32), ("aim_site_b", 0x14E5BDFC, 32),
    ("wupdate_thunk", 0x029FAF60, 64), ("wupdate_body", 0x14E5BBC0, 64),
    ("castray_thunk", 0x030F9D30, 64), ("castray_body", 0x169B7630, 64),
    ("getaim_thunk", 0x029EF0F0, 64), ("getaim_body", 0x14E276C0, 64),
    ("head_table", 0x04AF4550, 0x220),
    ("headset_thunk", 0x02A25600, 64), ("headset_body", 0x14ED3990, 64),
    ("noblur_match", 0x14E7625C, 32),
    ("wsetterA_thunk", 0x030F5AF0, 64), ("wsetterA_body", 0x169AF020, 64),
    ("ansel_iat_a", 0x1880F098, 8), ("ansel_iat_b", 0x1880F0A0, 8),
    ("ttcast_thunk", 0x03112DE0, 64), ("ttcast_body", 0x16B16990, 64),
    ("xinput_slot", 0x0389D130, 8),
    ("pubattach_thunk", 0x0189CE30, 64), ("pubattach_body", 0x0F821260, 64),
    ("setworld_thunk", 0x017DEA30, 64), ("setworld_body", 0x0F47BDC0, 64),
    ("aimanchor_thunk", 0x02993820, 64), ("aimanchor_body", 0x14B366C0, 64),
    ("muzanchor_thunk", 0x02995340, 64), ("muzanchor_body", 0x14B3DEB0, 64),
]

AOB = "48 83 EC 08 44 0F B6 DA 49 89 CA 38 51 68 74 ? 0F B7 41 4A 85 C0 74 ?"
AOB_EXPECT_RVA = 0x14ED3990  # head_setter_impl


class Pe:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        e = struct.unpack_from("<I", d, 0x3C)[0]
        self.ts, = struct.unpack_from("<I", d, e + 8)
        nsec, = struct.unpack_from("<H", d, e + 6)
        optsz, = struct.unpack_from("<H", d, e + 20)
        self.soi, = struct.unpack_from("<I", d, e + 0x50)
        secs = []
        off = e + 24 + optsz
        for i in range(nsec):
            name = d[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
            vsz, va, rsz, rp = struct.unpack_from("<IIII", d, off + 8)
            secs.append((name, va, vsz, rp, rsz))
            off += 40
        self.secs = secs

    def rva_off(self, rva):
        for name, va, vsz, rp, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz):
                fo = rp + (rva - va)
                if rva - va >= rsz:
                    return None, name  # virtual-only (bss-like)
                return fo, name
        return None, "?"


def main():
    old, new = Pe(OLD), Pe(NEW)
    print(f"old: ts=0x{old.ts:08X} soi=0x{old.soi:08X}")
    print(f"new: ts=0x{new.ts:08X} soi=0x{new.soi:08X}")
    if old.ts != 0x6A75F2F4 or new.ts != 0x6A7C5143:
        print("FATAL: unexpected binary identity, aborting")
        return 2

    print("\nsection tables:")
    same_secs = old.secs == new.secs
    print(f"  identical: {same_secs}")
    if not same_secs:
        for a, b in zip(old.secs, new.secs):
            if a != b:
                print(f"  OLD {a}\n  NEW {b}")

    fails = skips = 0
    for label, rva, n in SITES:
        fo_o, sec_o = old.rva_off(rva)
        fo_n, sec_n = new.rva_off(rva)
        if fo_o is None or fo_n is None:
            print(f"SKIP  {label:16s} rva=0x{rva:08X} virtual-only "
                  f"(old={sec_o}, new={sec_n})")
            skips += 1
            continue
        bo = old.data[fo_o:fo_o + n]
        bn = new.data[fo_n:fo_n + n]
        if bo == bn:
            print(f"OK    {label:16s} rva=0x{rva:08X} sec={sec_n} {n}B identical")
        else:
            diff = next(i for i in range(min(len(bo), len(bn)))
                        if bo[i] != bn[i])
            print(f"FAIL  {label:16s} rva=0x{rva:08X} first diff at +0x{diff:X}")
            print(f"      old {bo[:24].hex(' ')}")
            print(f"      new {bn[:24].hex(' ')}")
            fails += 1

    # AOB re-scan on the new binary
    toks = AOB.split()
    pat = b"".join(b"." if t == "?" else re.escape(bytes([int(t, 16)]))
                   for t in toks)
    hits = [m.start() for m in re.finditer(pat, new.data, re.DOTALL)]
    rvas = []
    for h in hits:
        for name, va, vsz, rp, rsz in new.secs:
            if rp <= h < rp + rsz:
                rvas.append(va + (h - rp))
                break
    print(f"\nAOB hits in new exe: {len(rvas)} at "
          f"{['0x%08X' % r for r in rvas]}")
    aob_ok = rvas == [AOB_EXPECT_RVA]
    print(f"AOB unique at head_setter_impl: {aob_ok}")

    print(f"\nRESULT: {fails} FAIL, {skips} SKIP (virtual-only), "
          f"{len(SITES) - fails - skips} OK, AOB {'OK' if aob_ok else 'FAIL'}")
    return 0 if fails == 0 and aob_ok else 1


if __name__ == "__main__":
    sys.exit(main())
