"""Sanity-check syms.toml against recomp.z64: every function must end in a jump.

A function's last non-nop words must be a terminator (jr ra / jr reg / j / b) and its
delay slot. Prints the functions that do not, which usually means a bad split.
Usage: check_syms.py BUILD_DIR
"""
import struct
import sys
import tomllib
from pathlib import Path

build = Path(sys.argv[1])
rom = (build / "recomp.z64").read_bytes()
syms = tomllib.loads((build / "syms.toml").read_text())


def is_terminator(w):
    op = w >> 26
    if op == 2:                                   # j
        return True
    if op == 0 and (w & 0x3F) == 0x08:            # jr reg
        return True
    if op == 4 and (w >> 16) & 0x3FF == 0:        # beq zero, zero (b)
        return True
    if w == 0x42000018:                           # eret
        return True
    return False


bad = 0
total = 0
for sec in syms["section"]:
    for f in sec["functions"]:
        total += 1
        start = sec["rom"] + f["vram"] - sec["vram"]
        W = [struct.unpack_from(">I", rom, start + i)[0] for i in range(0, f["size"], 4)]
        j = len(W) - 1
        while j > 0 and W[j] == 0:
            j -= 1
        # j is the delay slot (or a terminator whose nop delay slot was skipped)
        ok = (j >= 1 and is_terminator(W[j - 1])) or is_terminator(W[j])
        if not ok:
            bad += 1
            print("%-6s %-20s size 0x%X  ends %s" % (sec["name"], f["name"], f["size"],
                  " ".join("%08X" % w for w in W[max(0, j - 2):j + 1])))
print("%d/%d functions without a terminator" % (bad, total))

# every jal must land on a function start (N64Recomp: "No function found for jal target")
starts = {f["vram"] for sec in syms["section"] for f in sec["functions"]}
missing = {}
for sec in syms["section"]:
    for f in sec["functions"]:
        start = sec["rom"] + f["vram"] - sec["vram"]
        for i in range(0, f["size"], 4):
            w = struct.unpack_from(">I", rom, start + i)[0]
            if w >> 26 == 3:
                pc = f["vram"] + i
                t = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if t not in starts:
                    missing.setdefault(t, []).append(f["name"])
for t, callers in sorted(missing.items()):
    print("jal target %08X is not a function start (called from %s)" % (t, ", ".join(sorted(set(callers))[:4])))
print("%d jal targets without a function" % len(missing))
