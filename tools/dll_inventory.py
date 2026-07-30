#!/usr/bin/env python3
"""
Inventory every PE (dll/exe) under a game install tree: size, sha256, machine,
compile timestamp, version resource, export count, and a middleware guess.

Session 1 (GRW-XR). Read-only: opens files for reading only, writes nothing to
the install tree.

Usage:
    python tools/dll_inventory.py "C:\\Steam\\steamapps\\common\\Wildlands" \
        --out docs/RAW/dll-inventory.txt
"""
import argparse
import datetime as dt
import hashlib
import os

import pefile

# name fragment -> (vendor/product, what it implies for a VR port)
MIDDLEWARE = [
    ("amd_ags", "AMD AGS (GPU Services)", "AMD-specific D3D11 extensions; harmless"),
    ("anselsdk", "NVIDIA Ansel", "in-game photo mode; ALREADY takes camera control at "
                                "runtime, so an existing camera-override path exists in-engine"),
    ("bink2", "RAD Game Tools Bink 2 video", "prerendered video playback"),
    ("d3dcompiler", "D3D shader compiler", "runtime shader compilation is available"),
    ("d3dcsx", "D3DCSX compute", "DirectCompute helper"),
    ("d3dx11", "legacy D3DX11", "legacy D3D11 helper"),
    ("dbdata", "Ubisoft device database", "GPU/driver capability database"),
    ("dbghelp", "Microsoft DbgHelp", "crash dump generation"),
    ("gfsdk_ssao", "NVIDIA HBAO+", "screen-space AO; VR-relevant post effect"),
    ("gfsdk_turf", "NVIDIA TurfEffects", "GPU grass; performance lever"),
    ("gpudatabase", "Ubisoft GPU database", "auto-detected quality presets"),
    ("nvvolumetric", "NVIDIA Volumetric Lighting", "godrays; performance lever, "
                                                   "often stereo-inconsistent in VR"),
    ("shadercontainer", "Ubisoft shader container", "precompiled shader blob store"),
    ("steam_api", "Steamworks", "store DRM/API"),
    ("stream_engine", "Ubisoft streaming engine", "world streaming"),
    ("tobii", "Tobii EyeX eye tracking", "an existing gaze->camera input path in-engine"),
    ("uplay", "Ubisoft Connect", "store DRM"),
    ("xinput", "XInput", "gamepad input; PRIMARY INPUT HOOK TARGET"),
    ("easyanticheat", "Easy Anti-Cheat", "multiplayer anti-cheat; solo campaign only"),
    ("openvr", "OpenVR", "VR runtime"),
    ("openxr", "OpenXR", "VR runtime"),
    ("physx", "NVIDIA PhysX", "physics"),
    ("havok", "Havok", "physics/animation"),
    ("humanik", "Autodesk HumanIK", "runtime full-body IK solver"),
    ("scaleform", "Autodesk Scaleform", "flash-based UI"),
]

MACHINE = {0x8664: "x64", 0x014C: "x86", 0xAA64: "arm64"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def version_of(pe):
    try:
        if hasattr(pe, "FileInfo"):
            for lst in pe.FileInfo:
                for fi in lst:
                    if getattr(fi, "Key", b"") == b"StringFileInfo":
                        for st in fi.StringTable:
                            d = {k.decode("latin-1"): v.decode("latin-1")
                                 for k, v in st.entries.items()}
                            return (d.get("CompanyName", ""), d.get("ProductName", ""),
                                    d.get("FileVersion", ""))
    except Exception:
        pass
    return ("", "", "")


def guess(name):
    low = name.lower()
    for frag, product, implies in MIDDLEWARE:
        if frag in low:
            return product, implies
    return "", ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--out", default="docs/RAW/dll-inventory.txt")
    a = ap.parse_args()

    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    rows = []
    for dirpath, _dirs, files in os.walk(a.root):
        for fn in files:
            if not fn.lower().endswith((".dll", ".exe")):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, a.root)
            try:
                size = os.path.getsize(full)
                pe = pefile.PE(full, fast_load=False)
                mach = MACHINE.get(pe.FILE_HEADER.Machine, hex(pe.FILE_HEADER.Machine))
                ts = dt.datetime.fromtimestamp(pe.FILE_HEADER.TimeDateStamp,
                                               dt.timezone.utc).strftime("%Y-%m-%d")
                nexp = 0
                if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
                    nexp = len(pe.DIRECTORY_ENTRY_EXPORT.symbols or [])
                company, product, ver = version_of(pe)
                pe.close()
            except Exception as exc:
                rows.append((rel, size if 'size' in dir() else -1, "ERR", "", 0,
                             "", "", "", f"parse failed: {exc}", ""))
                continue
            mw, implies = guess(fn)
            rows.append((rel, size, mach, ts, nexp, company, product, ver, mw, implies))

    rows.sort(key=lambda r: r[0].lower())

    with open(a.out, "w", encoding="utf-8", errors="replace") as out:
        out.write(f"# PE inventory of {a.root}\n# {len(rows)} PE files\n\n")
        out.write("## TABLE\n")
        out.write(f"{'path':52s} {'size':>12s} {'arch':5s} {'built':10s} "
                  f"{'exp':>5s}  company / product / version\n")
        out.write("-" * 140 + "\n")
        for rel, size, mach, ts, nexp, company, product, ver, mw, implies in rows:
            meta = " / ".join(x for x in (company, product, ver) if x)
            out.write(f"{rel:52s} {size:12d} {mach:5s} {ts:10s} {nexp:5d}  {meta}\n")

        out.write("\n\n## SHA256\n")
        for rel, *_ in rows:
            full = os.path.join(a.root, rel)
            try:
                out.write(f"{sha256(full)}  {rel}\n")
            except Exception as exc:
                out.write(f"{'?' * 64}  {rel}  ({exc})\n")

        out.write("\n\n## MIDDLEWARE IDENTIFIED AND WHAT IT IMPLIES\n")
        seen = set()
        for rel, size, mach, ts, nexp, company, product, ver, mw, implies in rows:
            if mw and mw not in seen:
                seen.add(mw)
                out.write(f"\n{mw}\n  file    : {rel}\n  implies : {implies}\n")
        out.write("\n\n## NOT FOUND (checked for)\n")
        for frag, product, implies in MIDDLEWARE:
            if product not in seen:
                out.write(f"  absent: {product}\n")

    print(f"wrote {a.out}  ({len(rows)} PE files)")


if __name__ == "__main__":
    main()
