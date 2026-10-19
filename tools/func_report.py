"""Per-function feature report for one section of build/syms.toml (libultra identification aid).

Usage: func_report.py BUILD_DIR SECTION [VRAM_FROM VRAM_TO]
For each function: size, callees, caller count (from all sections), hardware register
blocks touched, cop0/cache/tlb/eret/syscall use and notable immediates.
"""
import struct
import sys
import tomllib
from collections import defaultdict
from pathlib import Path

import rabbitizer

HW = {0xA404: "SP", 0xA408: "SPpc", 0xA410: "DP", 0xA430: "MI", 0xA440: "VI", 0xA450: "AI",
      0xA460: "PI", 0xA470: "RI", 0xA480: "SI", 0xA800: "dom2", 0xB000: "cart", 0x1FC0: "PIF",
      0xBFC0: "PIF"}

build = Path(sys.argv[1])
rom = (build / "recomp.z64").read_bytes()
syms = tomllib.loads((build / "syms.toml").read_text())
want = sys.argv[2]
lo = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0
hi = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0xFFFFFFFF


def func_words(sec, f):
    start = sec["rom"] + f["vram"] - sec["vram"]
    return [struct.unpack_from(">I", rom, start + i)[0] for i in range(0, f["size"], 4)]


callers = defaultdict(set)
for sec in syms["section"]:
    for f in sec["functions"]:
        for w in func_words(sec, f):
            if w >> 26 == 3:
                t = (f["vram"] & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                callers[t].add(f["name"])

for sec in syms["section"]:
    if sec["name"] != want:
        continue
    names = {f["vram"]: f["name"] for f in sec["functions"]}
    for f in sec["functions"]:
        if not lo <= f["vram"] < hi:
            continue
        W = func_words(sec, f)
        callees, hw, special, imms = [], set(), set(), set()
        for i, w in enumerate(W):
            ins = rabbitizer.Instruction(w, f["vram"] + i * 4)
            op = w >> 26
            if op == 3:
                t = (f["vram"] & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                callees.append("%08X" % t)
            elif op == 0x0F:
                h = w & 0xFFFF
                if h in HW:
                    hw.add(HW[h])
                elif h not in (0x8000, 0x8001, 0x8002, 0x8003, 0x8004, 0x8005, 0x8006):
                    imms.add("lui %04X" % h)
            elif op == 0x10:
                special.add(ins.getOpcodeName() + ("(%s)" % ins.disassemble().split(",")[-1].strip()
                                                   if ins.getOpcodeName() in ("mfc0", "mtc0") else ""))
            elif op == 0x2F:
                special.add("cache")
            elif op == 0 and (w & 0x3F) == 0x0C:
                special.add("syscall")
            elif op == 0x11 and ((w >> 21) & 31) in (2, 6):
                special.add("cfc1/ctc1")
            elif op in (0x09, 0x0D) and ((w >> 21) & 31) == 0:
                v = w & 0xFFFF
                if v >= 0x100 and v not in (0xFFFF,):
                    imms.add("%X" % v)
        cs = sorted(set(callees))
        print("%08X size %4X callers %3d | calls %s | hw %s | %s | %s" % (
            f["vram"], f["size"], len(callers[f["vram"]]),
            ",".join(c[-5:] for c in cs[:8]) + ("+" if len(cs) > 8 else ""),
            ",".join(sorted(hw)) or "-", ",".join(sorted(special)) or "-",
            ",".join(sorted(imms)[:6]) or "-"))
