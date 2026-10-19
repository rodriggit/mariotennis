"""Build N64Recomp inputs: the recomp ROM and the symbol file.

Usage: build_recomp.py ROM OUT_DIR

Writes OUT_DIR/recomp.z64 (original ROM + every code section decompressed
from 0x01000000 on) and OUT_DIR/syms.toml (sections and functions).
Function boundaries come from spimdisasm on the CPU-code ranges of each section.
"""
import struct
import sys
from pathlib import Path

import spimdisasm

import sections as S
from filetable import is_compressed, read_table
from libultra import LIBULTRA
from mtlz import decompress
from reloc_tlb import relocate

SYNTH_BASE = 0x01000000


def last_code_end(data, start=0, end=None):
    """End offset of CPU code: after the last `jr ra` + delay slot, 16-aligned."""
    end = len(data) if end is None else end
    last = None
    for i in range(start, end - 3, 4):
        if struct.unpack_from(">I", data, i)[0] == 0x03E00008:
            last = i
    return (last + 8 + 15) & ~15


JR_RA = 0x03E00008
ERET = 0x42000018


def words(data, start, end):
    return [struct.unpack_from(">I", data, i)[0] for i in range(start, end - 3, 4)]


def call_targets(data, vram, text_ranges):
    """Absolute targets of jal, of the leading `j` entry table, and of lui+addiu pairs
    (function pointers) in a section's code. Plain `j` elsewhere is mostly an intra-function
    jump, so it is not used."""
    out = set()
    for start, end in text_ranges:
        W = words(data, start, end)
        k = 0
        while k < len(W) and W[k] >> 26 == 2:   # entry table: j f; nop; j g; nop ...
            out.add(((vram + start) & 0xF0000000) | ((W[k] & 0x03FFFFFF) << 2))
            k += 2
        for i, w in enumerate(W):
            op = w >> 26
            if op == 3:
                pc = vram + start + i * 4
                out.add((pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
            elif op == 0x0F:
                rt, hi = (w >> 16) & 31, (w & 0xFFFF) << 16
                for x in W[i + 1:i + 6]:
                    if x >> 26 == 0x09 and (x >> 21) & 31 == rt:
                        lo = x & 0xFFFF
                        out.add((hi + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF)
                        break
    return out


def starts_after_function_end(W, k):
    """True if word index k can begin a function: start of range, or right after a
    `jr ra` + delay slot (optionally followed by nop padding)."""
    if k == 0:
        return True
    if W[k - 1] == ERET:
        return True                       # eret has no delay slot
    if W[k - 1] == JR_RA:
        return False                      # k would be the delay slot
    j = k - 1
    while j >= 0 and W[j] == 0:
        j -= 1
    if j < 0:
        return True
    # j is the last non-nop word: the delay slot (jr ra at j-1), or jr ra itself
    # whose nop delay slot was skipped together with the padding
    return (j >= 1 and W[j - 1] == JR_RA) or (W[j] == JR_RA and j <= k - 2)


def function_seeds(data, vram, text_ranges, targets):
    seeds = set()
    for start, end in text_ranges:
        W = words(data, start, end)
        base = vram + start
        for t in targets:
            if base <= t < base + len(W) * 4 and t % 4 == 0 and starts_after_function_end(W, (t - base) // 4):
                seeds.add(t)
        for k, w in enumerate(W):
            if (w & 0xFFFF8000) == 0x27BD8000 and starts_after_function_end(W, k):  # addiu sp, sp, -N
                seeds.add(base + k * 4)
    return seeds


def find_functions(data, vram, text_ranges, name_prefix, seeds=()):
    """spimdisasm's functions over each (start_off, end_off) text range, split at the
    validated seeds it missed (entries reached only through pointers or jump tables).
    Returns [(name, vram, size)]."""
    bounds = []
    for start, end in text_ranges:
        ctx = spimdisasm.common.Context()
        ctx.changeGlobalSegmentRanges(0, len(data), vram, vram + len(data))
        text = spimdisasm.mips.sections.SectionText(ctx, start, end, vram + start, name_prefix, data, 0, None)
        text.analyze()
        for sym in text.symbolList:
            if sym.sizew:
                bounds.append((sym.vram, sym.vram + sym.sizew * 4))
    starts = {b[0] for b in bounds}
    for s in list(seeds) + S.FORCED_STARTS:
        if any(a < s < e for a, e in bounds):
            starts.add(s)
    # leading entry table: each `j f; nop` stub is its own function (called through
    # pointers), and the first code word after the table starts another one
    for start, end in text_ranges:
        W = words(data, start, end)
        k = 0
        while k + 1 < len(W) and W[k] >> 26 == 2:
            starts.add(vram + start + k * 4)
            k += 2
        if k:
            while k < len(W) and W[k] == 0:
                k += 1
            starts.add(vram + start + k * 4)
    ends = {b[1] for b in bounds}
    cuts = sorted(starts | ends)
    funcs = []
    for a, b in zip(cuts, cuts[1:]):
        if a in starts and any(x <= a < y for x, y in bounds):
            off = a - vram
            if funcs and all(w == 0 for w in words(data, off, b - vram + 3)):
                # nop padding between functions: belongs to the previous one
                name, fa, fs = funcs[-1]
                if fa + fs == a:
                    funcs[-1] = (name, fa, fs + (b - a))
                    continue
            funcs.append(("%s_%08X" % (name_prefix, a), a, b - a))
    return funcs


def local_jump_targets(w, pc):
    """Target of j or of a non-linking branch, else None."""
    op = w >> 26
    if op == 2:
        return (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
    rel = pc + 4 + (((w & 0xFFFF) ^ 0x8000) - 0x8000) * 4
    if 4 <= op <= 7 or 0x14 <= op <= 0x17:
        return rel
    if op == 1 and ((w >> 16) & 31) in (0, 1, 2, 3):
        return rel
    if op == 0x11 and ((w >> 21) & 31) == 8:
        return rel
    return None


def falls_through(W):
    """True if the function's last real instruction does not end control flow."""
    L = len(W) - 1
    while L > 0 and W[L] == 0:
        L -= 1

    def term(w):
        return (w >> 26 == 2 or w == ERET or (w >> 26 == 0 and (w & 0x3F) == 0x08)
                or (w >> 16) == 0x1000)            # j, eret, jr, b
    return not (term(W[L]) or (L >= 1 and term(W[L - 1])))


def extend_shared_tails(data, vram, funcs):
    """N64Recomp only accepts jumps to a function start or inside the same function.
    Functions that jump into the middle of a later function, or fall through into the
    next one (e.g. memset entries sharing a body), are extended to cover that code;
    overlapping functions are fine since each one reads its own words."""
    starts = {fv for _, fv, _ in funcs}
    end_of = {fv: fv + fs for _, fv, fs in funcs}
    ordered = sorted(starts)
    out = []
    for name, fv, fs in funcs:
        end = fv + fs
        changed = True
        while changed:
            changed = False
            W = words(data, fv - vram, end - vram + 3)
            targets = [local_jump_targets(w, fv + i * 4) for i, w in enumerate(W)]
            if falls_through(W) and end in end_of:
                end, changed = end_of[end], True
                continue
            for t in targets:
                if t is None or fv <= t < end or t in starts:
                    continue
                owner = max((s for s in ordered if s <= t), default=None)
                if owner is None or t >= end_of[owner]:
                    continue                       # outside this section: left to N64Recomp
                if t < fv:
                    print("  WARNING %s jumps back into %08X (not handled)" % (name, t))
                    continue
                end, changed = end_of[owner], True
                break
        if end != fv + fs:
            print("  extended %s: 0x%X -> 0x%X" % (name, fs, end - fv))
        out.append((name, fv, end - fv))
    return out


def main():
    rom = open(sys.argv[1], "rb").read()
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    table = read_table(rom)

    sections = []  # (name, rom, vram, data, text_ranges)

    # boot: stays at its original ROM address (the runtime loads 0x1000..+1MB at boot)
    boot = rom[S.BOOT_ROM:S.BOOT_ROM + S.BOOT_TEXT_END]
    sections.append(("boot", S.BOOT_ROM, S.BOOT_VRAM, boot, [(0, S.BOOT_TEXT_END)]))

    main_data, _ = decompress(rom, S.MAIN_ROM)
    rsp_start = S.MAIN_TEXT[0][1] - S.MAIN_VRAM
    lib_start = S.MAIN_TEXT[1][0] - S.MAIN_VRAM
    data_guess = 0x80049B00 - S.MAIN_VRAM  # first data block found by the phase-1 map
    main_ranges = [(0, rsp_start), (lib_start, last_code_end(main_data, lib_start, data_guess))]
    synth = [("main", S.MAIN_VRAM, main_data, main_ranges)]

    for idx, vram in S.all_file_sections():
        data = is_compressed(rom, *table[idx])
        if idx == 209:
            data, rep = relocate(data, S.TLB_209_VRAM, S.TLB_209_BUFFER)
            assert not rep["problems"], rep["problems"]
        synth.append(("f%d" % idx, vram, data, [(0, last_code_end(data))]))

    # lay out the recomp ROM
    image = bytearray(rom)
    pos = SYNTH_BASE
    for name, vram, data, ranges in synth:
        image += b"\0" * (pos - len(image))
        image += data
        sections.append((name, pos, vram, data, ranges))
        pos = (len(image) + 15) & ~15
    (out_dir / "recomp.z64").write_bytes(image)

    # every call/jump/pointer target in any section is a candidate function start anywhere
    targets = set()
    for _, _, vram, data, ranges in sections:
        targets |= call_targets(data, vram, ranges)

    # symbol file
    lines = []
    total = 0
    for name, srom, vram, data, ranges in sections:
        funcs = find_functions(data, vram, ranges, name, function_seeds(data, vram, ranges, targets))
        funcs = extend_shared_tails(data, vram, funcs)
        if name == "main":
            starts = {fv for _, fv, _ in funcs}
            missing = [v for v in LIBULTRA if v not in starts]
            assert not missing, "libultra names not at a function start: %s" % [hex(v) for v in missing]
            funcs = [(LIBULTRA.get(fv, fn), fv, fs) for fn, fv, fs in funcs]
        total += len(funcs)
        lines.append("[[section]]")
        lines.append('name = "%s"' % name)
        lines.append("rom = 0x%08X" % srom)
        lines.append("vram = 0x%08X" % vram)
        lines.append("size = 0x%X" % len(data))
        lines.append("functions = [")
        for fname, fvram, fsize in funcs:
            lines.append('    { name = "%s", vram = 0x%08X, size = 0x%X },' % (fname, fvram, fsize))
        lines.append("]")
        lines.append("")
        print("%-6s rom 0x%08X vram 0x%08X size 0x%06X  text %s  funcs %d" % (
            name, srom, vram, len(data), " ".join("%X-%X" % r for r in ranges), len(funcs)))
    (out_dir / "syms.toml").write_text("\n".join(lines))
    print("total functions:", total)


if __name__ == "__main__":
    main()
