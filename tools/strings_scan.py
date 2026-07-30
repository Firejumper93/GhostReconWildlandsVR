#!/usr/bin/env python3
"""
String survey of a PE image with true RVAs, plus themed grouping.

Extracts both ASCII and UTF-16LE strings, maps each file offset back to an RVA
through the section table, and buckets hits into themes that matter for a VR
injection port (camera, skeleton, weapon, stereo, IK, render).

Session 1 (GRW-XR). Read-only.

Usage:
    python tools/strings_scan.py <pe-file> --outdir docs/RAW [--min 5]

Outputs (in --outdir):
    strings-all.txt        every string, "RVA<TAB>section<TAB>enc<TAB>text"
    strings-themed.txt     only strings matching a theme, grouped by theme
    strings-stats.txt      counts per theme and per section

RVA caveat: a string's RVA is where the *bytes* live. The code that references
it is found by cross-referencing (see tools/xref_scan.py), not from this file.
"""
import argparse
import os
import re
import sys

import pefile

# Theme -> list of case-insensitive regex fragments.
# Kept deliberately broad; false positives are cheap, misses are expensive.
THEMES = {
    "humanik": [
        r"humanik", r"\bHIK\b", r"hik[A-Z_]", r"[a-z]HIK[A-Z]", r"Effector",
        r"CharacterState", r"ReachT", r"ReachR", r"Autodesk", r"FBIK", r"fullbodyik",
        r"IKEffector", r"SolveType", r"PropertyDefault",
    ],
    "ik_solver": [
        r"\bIK\b", r"IKChain", r"IKSolve", r"TwoBone", r"FootIK", r"HandIK",
        r"LookAt", r"AimIK", r"ikweight", r"ik_weight", r"PoleVector",
    ],
    "camera_projection": [
        r"camera", r"projection", r"viewmatrix", r"view_matrix", r"worldmatrix",
        r"frustum", r"\bfov\b", r"fieldofview", r"nearplane", r"farplane",
        r"nearclip", r"farclip", r"viewport", r"lookat", r"eyepos", r"CameraNode",
        r"CameraManager", r"ViewProj", r"jitter", r"reversez", r"reverse_z",
    ],
    "skeleton_animation": [
        r"skeleton", r"\bbone\b", r"bones", r"joint", r"skinning", r"bindpose",
        r"bind_pose", r"animgraph", r"animation", r"animset", r"rig\b", r"ragdoll",
        r"BoneIndex", r"BoneName", r"SkinnedMesh", r"PoseBuffer", r"blendtree",
        r"morphtarget", r"attachpoint", r"socket",
    ],
    "weapon_aim": [
        r"weapon", r"muzzle", r"barrel", r"recoil", r"crosshair", r"reticle",
        r"\baim\b", r"aiming", r"bullet", r"projectile", r"ballistic", r"tracer",
        r"scope", r"optic", r"sight", r"zeroing", r"firemode", r"holster",
        r"grip\b", r"\bads\b",
    ],
    "stereo_vr": [
        r"stereo", r"3dvision", r"3d vision", r"nvstereo", r"lefteye", r"righteye",
        r"left_eye", r"right_eye", r"\bhmd\b", r"oculus", r"openvr", r"openxr",
        r"steamvr", r"\bvr\b", r"interpupillary", r"\bipd\b", r"eyeoffset",
        r"instancedstereo", r"multiview",
    ],
    "render_post": [
        r"\btaa\b", r"temporalaa", r"temporal_aa", r"motionblur", r"motion_blur",
        r"depthoffield", r"depth_of_field", r"\bdof\b", r"bloom", r"vignette",
        r"chromatic", r"lensflare", r"\bfxaa\b", r"\bsmaa\b", r"sharpen",
        r"upscal", r"resolutionscale", r"renderscale", r"vsync", r"framecap",
        r"framerate", r"tonemap", r"\bhdr\b", r"letterbox", r"aspectratio",
    ],
    "hud_ui": [
        r"\bhud\b", r"minimap", r"\bui_", r"_ui\b", r"scaleform", r"widget",
        r"subtitle", r"safearea", r"safe_area", r"canvas", r"overlay",
    ],
    "d3d_gfx": [
        r"d3d11", r"d3d12", r"dxgi", r"swapchain", r"backbuffer", r"rendertarget",
        r"constantbuffer", r"cbuffer", r"scissor", r"drawindexed", r"deferredcontext",
        r"GfxContext", r"GfxDevice",
    ],
    "engine_anvil": [
        r"anvil", r"AnvilNext", r"scimitar", r"Ubisoft", r"forge", r"datapc",
        r"EntityGroup", r"WorldComponent", r"EngineSettings",
    ],
    "input": [
        r"xinput", r"dinput", r"directinput", r"gamepad", r"controller",
        r"deadzone", r"rumble", r"vibration", r"keybind", r"actionmap",
    ],
    "config_settings": [
        r"settings", r"\bcfg\b", r"config", r"\.ini\b", r"\.xml\b", r"GamerProfile",
        r"options", r"preset", r"quality",
    ],
}

COMPILED = {k: [re.compile(p, re.I) for p in v] for k, v in THEMES.items()}

ASCII_RE = None
UTF16_RE = None


def build_res(minlen: int):
    global ASCII_RE, UTF16_RE
    ASCII_RE = re.compile(rb"[\x20-\x7e]{%d,}" % minlen)
    UTF16_RE = re.compile(rb"(?:[\x20-\x7e]\x00){%d,}" % minlen)


class RvaMap:
    """Maps raw file offsets back to RVAs using the section table."""

    def __init__(self, pe):
        self.rows = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\x00").decode("latin-1")
            start = s.PointerToRawData
            size = s.SizeOfRawData
            if size:
                self.rows.append((start, start + size, s.VirtualAddress, name))
        self.rows.sort()

    def lookup(self, off):
        # Linear scan is fine: PE images have a handful of sections.
        for start, end, rva, name in self.rows:
            if start <= off < end:
                return rva + (off - start), name
        return None, "(headers/overlay)"


def themes_for(text: str):
    hits = []
    for theme, pats in COMPILED.items():
        if any(p.search(text) for p in pats):
            hits.append(theme)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pefile")
    ap.add_argument("--outdir", default="docs/RAW")
    ap.add_argument("--min", type=int, default=5, help="minimum string length")
    a = ap.parse_args()

    build_res(a.min)
    os.makedirs(a.outdir, exist_ok=True)

    pe = pefile.PE(a.pefile, fast_load=True)
    rmap = RvaMap(pe)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    pe.close()

    with open(a.pefile, "rb") as fh:
        data = fh.read()

    print(f"read {len(data)} bytes", file=sys.stderr)

    results = []  # (rva, section, enc, text)
    for m in ASCII_RE.finditer(data):
        rva, sec = rmap.lookup(m.start())
        results.append((rva if rva is not None else -1, sec, "A",
                        m.group().decode("latin-1")))
    print(f"ascii strings: {len(results)}", file=sys.stderr)

    n_ascii = len(results)
    for m in UTF16_RE.finditer(data):
        rva, sec = rmap.lookup(m.start())
        results.append((rva if rva is not None else -1, sec, "W",
                        m.group().decode("utf-16-le", "replace")))
    print(f"utf16 strings: {len(results) - n_ascii}", file=sys.stderr)

    results.sort(key=lambda r: (r[0], r[3]))

    all_path = os.path.join(a.outdir, "strings-all.txt")
    with open(all_path, "w", encoding="utf-8", errors="replace") as out:
        out.write(f"# {a.pefile}\n# ImageBase 0x{image_base:X}\n")
        out.write("# RVA\tsection\tenc\ttext   (enc A=ascii W=utf-16le)\n")
        for rva, sec, enc, text in results:
            out.write(f"0x{rva:08X}\t{sec}\t{enc}\t{text}\n")

    themed = {k: [] for k in THEMES}
    for rva, sec, enc, text in results:
        for t in themes_for(text):
            themed[t].append((rva, sec, enc, text))

    themed_path = os.path.join(a.outdir, "strings-themed.txt")
    with open(themed_path, "w", encoding="utf-8", errors="replace") as out:
        out.write(f"# themed string survey of {a.pefile}\n")
        out.write(f"# ImageBase 0x{image_base:X}; RVA is where the string BYTES live.\n\n")
        for t in THEMES:
            rows = themed[t]
            out.write(f"\n{'=' * 74}\n## THEME: {t}   ({len(rows)} hits)\n{'=' * 74}\n")
            for rva, sec, enc, text in rows:
                out.write(f"0x{rva:08X}\t{sec}\t{enc}\t{text}\n")

    stats_path = os.path.join(a.outdir, "strings-stats.txt")
    with open(stats_path, "w", encoding="utf-8") as out:
        out.write(f"# {a.pefile}\ntotal strings (len>={a.min}): {len(results)}\n\n")
        out.write("per theme:\n")
        for t in THEMES:
            out.write(f"  {t:20s} {len(themed[t])}\n")
        out.write("\nper section:\n")
        from collections import Counter
        for sec, n in Counter(r[1] for r in results).most_common():
            out.write(f"  {sec:20s} {n}\n")

    print(f"wrote {all_path}\nwrote {themed_path}\nwrote {stats_path}")


if __name__ == "__main__":
    main()
