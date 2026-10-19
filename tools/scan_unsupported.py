"""List instructions N64Recomp cannot recompile in functions it will recompile.

Usage: scan_unsupported.py BUILD_DIR SYMBOL_LISTS_CPP
Skips functions whose names are in N64Recomp's reimplemented/ignored lists (read from
src/symbol_lists.cpp of the pinned N64Recomp). Flags cop0 moves other than Status
(reg 12), TLB ops, eret, cache.
"""
import re
import struct
import sys
import tomllib
from pathlib import Path

build = Path(sys.argv[1])
lists = Path(sys.argv[2]).read_text()
skip = set()
for block in ("reimplemented_funcs", "ignored_funcs"):
    body = lists.split("N64Recomp::%s" % block, 1)[1].split("};", 1)[0]
    skip |= set(re.findall(r'"([^"]+)"', body))

rom = (build / "recomp.z64").read_bytes()
syms = tomllib.loads((build / "syms.toml").read_text())
COP0 = {0: "Index", 1: "Random", 2: "EntryLo0", 3: "EntryLo1", 4: "Context", 5: "PageMask",
        6: "Wired", 8: "BadVAddr", 9: "Count", 10: "EntryHi", 11: "Compare", 12: "Status",
        13: "Cause", 14: "EPC", 15: "PRId", 16: "Config", 18: "WatchLo", 19: "WatchHi",
        28: "TagLo", 29: "TagHi", 30: "ErrorEPC"}

for sec in syms["section"]:
    for f in sec["functions"]:
        if f["name"] in skip:
            continue
        start = sec["rom"] + f["vram"] - sec["vram"]
        found = []
        for i in range(0, f["size"], 4):
            w = struct.unpack_from(">I", rom, start + i)[0]
            op, rs = w >> 26, (w >> 21) & 31
            vram = f["vram"] + i
            if op == 0x10:
                rd, rt = (w >> 11) & 31, (w >> 16) & 31
                if rs in (0, 4):
                    if rd != 12:
                        found.append("%08X %s r%d, %s" % (vram, "mfc0" if rs == 0 else "mtc0", rt, COP0.get(rd, rd)))
                elif rs == 16:
                    found.append("%08X %s" % (vram, {1: "tlbr", 2: "tlbwi", 6: "tlbwr", 8: "tlbp", 0x18: "eret"}.get(w & 0x3F, "cop0 %02X" % (w & 0x3F))))
            elif op == 0x2F:
                found.append("%08X cache" % vram)
        if found:
            print("%-6s %-24s %s" % (sec["name"], f["name"], "; ".join(found[:6]) + (" ..." if len(found) > 6 else "")))
