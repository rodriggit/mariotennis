"""List short loops that spin on a fixed address (candidates for SPIN_WAITS in gen_config.py).

Usage (from the repo root): .venv/Scripts/python.exe tools/find_spinwaits.py
"""
import struct, tomllib, rabbitizer
r = open("build/recomp.z64","rb").read(); s = tomllib.loads(open("build/syms.toml").read())
LOADS = {0x20,0x21,0x23,0x24,0x25,0x27,0x37}   # lb lh lw lbu lhu lwu ld
BRANCH = {1,4,5,6,7,0x14,0x15,0x16,0x17}
for x in s["section"]:
    for f in x["functions"]:
        st = x["rom"] + f["vram"] - x["vram"]
        W = [struct.unpack_from(">I", r, st+i)[0] for i in range(0, f["size"], 4)]
        for i, w in enumerate(W):
            op = w >> 26
            if op not in BRANCH and op != 2:
                continue
            pc = f["vram"] + i*4
            t = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2) if op == 2 else pc + 4 + (((w & 0xFFFF) ^ 0x8000) - 0x8000)*4
            if not (f["vram"] <= t < pc) or (pc - t) > 4*10:
                continue
            body = W[(t - f["vram"])//4 : i+2]   # include delay slot
            ok, has_load, written, bases = True, False, set(), set()
            for b in body:
                o = b >> 26
                if b == 0 or o in BRANCH or o == 2: continue
                if o in LOADS: has_load = True; bases.add((b>>21)&31); written.add((b>>16)&31); continue
                if o == 0x0F: written.add((b>>16)&31); continue        # lui
                if o in (0x0C,0x0D,0x09) and ((b>>21)&31) in written:   # andi/ori/addiu on loaded/lui regs
                    written.add((b>>16)&31); continue
                ok = False; break
            # every load base must be constant within the loop (only set by lui, or not written)
            if ok and has_load:
                consts = all(bb in {(bw>>16)&31 for bw in body if bw>>26==0x0F} or bb not in written for bb in bases)
                if consts:
                    print("%-6s %-16s loop %08X..%08X" % (x["name"], f["name"], t, pc))
