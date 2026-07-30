#!/usr/bin/env python3
"""
Dump every key from the Ghost Recon Wildlands user config directory and flag
the ones that are cheap VR levers.

The game keeps settings under %USERPROFILE%\\Documents\\My Games\\<title>. Files
are XML and/or INI. This walks every file, parses XML attributes and elements as
key/value pairs (and INI sections as a fallback), and prints the union, marking
keys that never appear in the in-game options menu as candidates.

Session 1 (GRW-XR). Read-only.

Usage:
    python tools/config_dump.py --out docs/RAW/user-config-dump.txt
    python tools/config_dump.py --root "C:\\path\\to\\config" --out ...
"""
import argparse
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET

FLAG_RE = re.compile(
    r"fov|camera|render|scale|resolution|aspect|framerate|frameratecap|fps|hud|"
    r"motionblur|motion_blur|depthoffield|depth_of_field|dof|taa|sharpen|"
    r"antialias|vsync|windowed|fullscreen|borderless|monitor|display|"
    r"lod|quality|shadow|volumetric|godray|bloom|vignette|chromatic|"
    r"upscal|dynamicres|latency|prerender|triplebuffer|stereo|3d",
    re.I,
)

CANDIDATE_ROOTS = [
    os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Ghost Recon Wildlands"),
    os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Tom Clancy's Ghost Recon Wildlands"),
    os.path.expandvars(r"%USERPROFILE%\OneDrive\Documents\My Games\Ghost Recon Wildlands"),
    os.path.expandvars(r"%USERPROFILE%\OneDrive\Documents\My Games\Tom Clancy's Ghost Recon Wildlands"),
    os.path.expandvars(r"%LOCALAPPDATA%\Ghost Recon Wildlands"),
]


def find_root():
    for r in CANDIDATE_ROOTS:
        if os.path.isdir(r):
            return r
    # Last resort: glob under My Games for anything Ghost Recon shaped.
    for base in (os.path.expandvars(r"%USERPROFILE%\Documents\My Games"),
                 os.path.expandvars(r"%USERPROFILE%\OneDrive\Documents\My Games")):
        for hit in glob.glob(os.path.join(base, "*Ghost*Recon*")):
            if os.path.isdir(hit):
                return hit
    return None


def walk_xml(node, path, out):
    tag = node.tag
    here = f"{path}/{tag}" if path else tag
    for k, v in sorted(node.attrib.items()):
        out.append((f"{here}@{k}", v))
    text = (node.text or "").strip()
    if text and len(node) == 0:
        out.append((here, text))
    for child in node:
        walk_xml(child, here, out)


def parse_file(path):
    """Return list of (key, value). Tries XML, falls back to INI-ish lines."""
    out = []
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except Exception as exc:
        return [(f"<<unreadable>>", str(exc))]

    # XML attempt
    try:
        root = ET.fromstring(raw)
        walk_xml(root, "", out)
        if out:
            return out
    except Exception:
        pass

    # INI / key=value attempt
    try:
        text = raw.decode("utf-8-sig", "replace")
    except Exception:
        text = raw.decode("latin-1", "replace")
    section = ""
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith((";", "#", "//")):
            continue
        m = re.match(r"^\[(.+?)\]$", line)
        if m:
            section = m.group(1)
            continue
        m = re.match(r"^([\w.\-]+)\s*[=:]\s*(.*)$", line)
        if m:
            key = f"{section}/{m.group(1)}" if section else m.group(1)
            out.append((key, m.group(2)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--out", default="docs/RAW/user-config-dump.txt")
    a = ap.parse_args()

    root = a.root or find_root()
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)

    with open(a.out, "w", encoding="utf-8", errors="replace") as w:
        if not root:
            w.write("*** USER CONFIG DIRECTORY NOT FOUND ***\n\nChecked:\n")
            for r in CANDIDATE_ROOTS:
                w.write(f"  {r}\n")
            w.write("\nThe game has probably never been launched on this machine,\n"
                    "or it stores settings elsewhere. Launch the game once to the\n"
                    "main menu, quit, and re-run this script.\n")
            print(f"wrote {a.out} (no config directory found)", file=sys.stderr)
            return

        w.write(f"# user config root: {root}\n\n")
        w.write("## FILES\n")
        allkeys = []
        for dirpath, _d, files in os.walk(root):
            for fn in sorted(files):
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, root)
                try:
                    size = os.path.getsize(full)
                except OSError:
                    size = -1
                w.write(f"  {rel:60s} {size:10d} bytes\n")

        for dirpath, _d, files in os.walk(root):
            for fn in sorted(files):
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, root)
                if os.path.getsize(full) > 4 * 1024 * 1024:
                    w.write(f"\n\n===== {rel} (skipped, >4 MiB) =====\n")
                    continue
                pairs = parse_file(full)
                w.write(f"\n\n===== {rel}  ({len(pairs)} keys) =====\n")
                for k, v in pairs:
                    w.write(f"{k} = {v}\n")
                    allkeys.append((rel, k, v))

        w.write("\n\n## FLAGGED KEYS (VR-relevant levers)\n")
        w.write("# matched on: fov camera render scale resolution aspect framerate\n")
        w.write("# hud motionblur depthoffield taa sharpen antialias vsync windowed\n")
        w.write("# lod quality shadow volumetric bloom vignette chromatic dynamicres stereo\n\n")
        n = 0
        for rel, k, v in allkeys:
            if FLAG_RE.search(k):
                n += 1
                w.write(f"[{rel}] {k} = {v}\n")
        w.write(f"\n{n} flagged of {len(allkeys)} total keys\n")

    print(f"wrote {a.out} (root={root})")


if __name__ == "__main__":
    main()
