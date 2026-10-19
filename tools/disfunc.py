"""Disassemble functions from build/syms.toml by VRAM.

Usage: disfunc.py BUILD_DIR SECTION VRAM [VRAM...]
"""
import struct
import sys
import tomllib
from pathlib import Path

import rabbitizer

build = Path(sys.argv[1])
rom = (build / "recomp.z64").read_bytes()
syms = tomllib.loads((build / "syms.toml").read_text())
sec = next(s for s in syms["section"] if s["name"] == sys.argv[2])
funcs = {f["vram"]: f for f in sec["functions"]}
for a in sys.argv[3:]:
    f = funcs[int(a, 16)]
    print("== %s size 0x%X" % (f["name"], f["size"]))
    start = sec["rom"] + f["vram"] - sec["vram"]
    for i in range(0, f["size"], 4):
        w = struct.unpack_from(">I", rom, start + i)[0]
        if w:
            print("  %08X %s" % (f["vram"] + i, rabbitizer.Instruction(w, f["vram"] + i).disassemble()))
