"""Statically relocate a TLB-mapped overlay to its physical KSEG0 address.

File 209 runs at virtual 0x50000000, which overlays 86 and 462 always map
(osMapTLB, 2 x 16 KiB pages) onto the fixed buffer 0x80230000. N64Recomp has
no TLB, so we recompile the file at 0x80230000 instead. This rewrites:
  - lui 0x5000 (hi half of every self address; all lo halves are < 0x8000)
  - j/jal targets (26-bit, segment taken from the PC)
  - 32-bit data words pointing into the window (jump tables, pointer tables)
and reports the data words, which the runtime must also patch in RAM after
the game loads the original bytes.

Usage: reloc_tlb.py ROM FILE_INDEX OLD_VRAM NEW_VRAM [OUT_BIN]
"""
import struct
import sys

from filetable import is_compressed, read_table

WINDOW = 0x8000  # osMapTLB page mask 0x6000 -> 16 KiB even + 16 KiB odd

LOADS_STORES = set(range(0x20, 0x28)) | set(range(0x28, 0x2F)) | {0x31, 0x35, 0x39, 0x3D}


def relocate(data, old, new):
    words = [struct.unpack_from(">I", data, i)[0] for i in range(0, len(data) - 3, 4)]
    last_code = max(i for i, w in enumerate(words) if w == 0x03E00008) + 1
    old_hi, new_hi = old >> 16, new >> 16
    out = list(words)
    report = {"lui": 0, "jump": 0, "data": [], "max_off": 0, "problems": []}

    for i in range(last_code + 1):
        w = words[i]
        op = w >> 26
        if op == 0x0F and (w & 0xFFFF) == old_hi:
            rt = (w >> 16) & 31
            # every use of rt before it is overwritten must be an address with a small lo
            used = False
            for j in range(i + 1, min(i + 12, last_code + 1)):
                x = words[j]
                xo, xs, xt = x >> 26, (x >> 21) & 31, (x >> 16) & 31
                lo = x & 0xFFFF
                if xs == rt and (xo in LOADS_STORES or xo == 0x09):
                    if lo >= 0x8000:
                        report["problems"].append("+%05X lo 0x%04X >= 0x8000" % (j * 4, lo))
                    report["max_off"] = max(report["max_off"], lo)
                    used = True
                elif xo == 0 and (x & 0x3F) == 0x21 and rt in (xs, xt):
                    used = True  # indexed table access: lui; addu; lw lo(...)
                if xo == 0x0F and xt == rt:
                    break
            if not used:
                report["problems"].append("+%05X lui with no address use" % (i * 4))
            out[i] = (w & 0xFFFF0000) | new_hi
            report["lui"] += 1
        elif op in (2, 3):
            target = (old & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if old <= target < old + len(data):
                nt = target - old + new
                out[i] = (w & 0xFC000000) | ((nt >> 2) & 0x03FFFFFF)
                report["jump"] += 1
            else:
                report["problems"].append("+%05X jump outside file -> %08X" % (i * 4, target))

    for i in range(last_code + 1, len(words)):
        w = words[i]
        if old <= w < old + WINDOW:
            out[i] = w - old + new
            report["data"].append((i * 4, w, out[i]))

    if report["max_off"] >= WINDOW:
        report["problems"].append("access at +0x%X beyond the mapped window" % report["max_off"])
    blob = b"".join(struct.pack(">I", w) for w in out) + data[len(out) * 4:]
    return blob, report


if __name__ == "__main__":
    rom = open(sys.argv[1], "rb").read()
    idx, old, new = int(sys.argv[2]), int(sys.argv[3], 16), int(sys.argv[4], 16)
    data = is_compressed(rom, *read_table(rom)[idx])
    blob, rep = relocate(data, old, new)
    print("file %d: 0x%X bytes, %08X -> %08X" % (idx, len(data), old, new))
    print("  lui rewritten:   %d" % rep["lui"])
    print("  j/jal rewritten: %d" % rep["jump"])
    print("  data words:      %d" % len(rep["data"]))
    print("  highest offset:  0x%X (window 0x%X)" % (rep["max_off"], WINDOW))
    for off, a, b in rep["data"]:
        print("    +%05X  %08X -> %08X" % (off, a, b))
    print("  problems: %d" % len(rep["problems"]))
    for p in rep["problems"]:
        print("    " + p)
    if len(sys.argv) > 5:
        open(sys.argv[5], "wb").write(blob)
