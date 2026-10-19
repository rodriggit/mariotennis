"""Generate build/recomp.toml (N64Recomp config) from build/syms.toml + build/recomp.z64.

Usage: gen_config.py BUILD_DIR

Hooks call into the port (port/include/mt_hooks.h):
  - boot entry: set sp like the skipped stub did; register main once decompressed
  - load_lz (0x80037DFC): report every file load (s0 = ROM start, s1 = destination)
  - window modules: each data `lui rX, 0x1000` becomes nop + rX = mt_window_base
"""
import struct
import sys
import tomllib
from pathlib import Path

import sections as S
from filetable import is_compressed, read_table
from reloc_tlb import relocate

# File loading. load_lz (0x80037DFC) and main_80031290 (boot, file 78) both decompress
# with rom_lz_read(rom, dest, size) at 0x80035CC0; load_raw (0x80037E48) DMAs uncompressed
# files. rom_lz_read advances its saved ROM pointer, so the start is recorded on entry and
# the load reported before its epilogue (dest is still at sp+0x210C).
ROM_LZ_READ = 0x80035CC0
ROM_LZ_READ_EPILOGUE = 0x80035F00
LOAD_RAW = 0x80037E48
LOAD_RAW_DONE = 0x80037E90        # after the DMA; s0 = ROM start, s1 = dest
BOOT_J_MAIN = 0x80300070          # `j 0x80031000` in boot_80300050
BOOT_COUNT_READS = [(0x80300058, 18), (0x8030006C, 6)]   # mfc0 s2/a2, Count (v0 is dead at both)
BOOT_CACHE_FUNCS = [0x803002A0, 0x803002D0]              # I-cache invalidate / D-cache writeback
MAIN_SIZE = 0x2AE10

# Spin waits on a flag another thread changes. The runtime only switches threads when the
# running one blocks or yields (no interrupts), so each loop head yields until an external
# event (VI, AI, SP, PI...) and lets any higher-priority thread that became ready run.
SPIN_WAITS = [
    ("main_80031D60", 0x80031D90),   # NuSys idle thread polling its callback (always NULL)
    ("f78_80100010", 0x80100018),    # boot: while (*(s32*)0x80062608 != 0)
]

# Startup (main_80031000) starts its first thread with a private copy of osStartThread;
# call libultra's (0x8003A980), which the runtime reimplements.
START_THREAD_CALL = 0x80031134
OS_START_THREAD = 0x8003A980

# Not recompiled: nothing that is recompiled calls them, and they use libultra internals
# (thread queues, SI, pak RAM) that the runtime does not provide.
IGNORED = [
    "main_80031C4C",   # private osStartThread copy (see START_THREAD_CALL)
    "main_80032D20",   # wake-up helper, only called by osRecvMesg/osSendMesg
    "main_800332DC",   # send_mesg/__osEnqueueThread replacements, copied over libultra at boot
    "main_80048F38",   # Controller Pak id routines (__osRepairPackId, __osCheckPackId,
    "main_80049224",   #   __osGetId, __osCheckId, __osPfsRWInode by call pattern);
    "main_80049388",   #   the runtime reports no Controller Pak
    "main_80049554",
    "main_8004960C",
    "osVirtualToPhysical_tlb",   # TLB-aware version in the port (mt_hooks.cpp)
    "osViSetXScale_mt",          # applies the factor, in the port (mt_hooks.cpp)
]


# RDRAM accessed through KSEG1 (0xA0000000, uncached); the runtime only maps KSEG0, so
# each `lui rX, 0xA0nn` becomes `lui rX, 0x80nn` (same memory, cached view).
KSEG1_LUIS = [
    ("boot", 0x8030008C),   # boot clear loop end, 0xA0100000
    ("boot", 0x8030009C),   # boot clear loop start, 0xA0000400
    ("main", 0x80031174),   # memset of RDRAM 1-4 MB at 0xA0100000
    ("main", 0x800357E0),   # osVirtualToPhysical(p) + 0xA0000000
    ("main", 0x80035A68),   # osVirtualToPhysical(p) + 0xA0000000
]


def kseg1_patches(rom, syms):
    out = []
    for sec_name, vram in KSEG1_LUIS:
        sec = next(s for s in syms["section"] if s["name"] == sec_name)
        f = next(f for f in sec["functions"] if f["vram"] <= vram < f["vram"] + f["size"])
        w = struct.unpack_from(">I", rom, sec["rom"] + vram - sec["vram"])[0]
        assert w >> 26 == 0x0F and (w & 0xFF00) == 0xA000, "%08X is not lui 0xA0nn" % vram
        out.append((f["name"], vram, w - 0x2000))
    return out


def words_of(rom, sec, f):
    start = sec["rom"] + f["vram"] - sec["vram"]
    return [struct.unpack_from(">I", rom, start + i)[0] for i in range(0, f["size"], 4)]


def is_branch_or_jump(w):
    op = w >> 26
    if op in (2, 3) or 4 <= op <= 7 or 0x14 <= op <= 0x17:
        return True
    if op == 1:                                    # regimm branches
        return True
    if op == 0 and (w & 0x3F) in (0x08, 0x09):     # jr / jalr
        return True
    if op == 0x11 and ((w >> 21) & 31) == 8:       # bc1x
        return True
    return False


def window_patches(rom, syms):
    patches, hooks = [], []
    window = {"f%d" % i for i in S.WINDOW_MODULES}
    for sec in syms["section"]:
        if sec["name"] not in window:
            continue
        count = 0
        for f in sec["functions"]:
            W = words_of(rom, sec, f)
            for i, w in enumerate(W):
                if w >> 26 != 0x0F or (w & 0xFFFF) != 0x1000:
                    continue
                rt = (w >> 16) & 31
                # keep `lui; addiu 0x10; jalr` (the internal callback pointer)
                nxt = W[i + 1] if i + 1 < len(W) else 0
                if nxt >> 26 == 0x09 and (nxt & 0xFFFF) == 0x0010:
                    continue
                assert i == 0 or not is_branch_or_jump(W[i - 1]), \
                    "%s: lui at %08X is in a delay slot" % (f["name"], f["vram"] + i * 4)
                vram = f["vram"] + i * 4
                patches.append((f["name"], vram, 0))
                hooks.append((f["name"], vram, "ctx->r%d = (gpr)(int32_t)mt_window_base;" % rt))
                count += 1
        assert count == 5, "%s: expected 5 data lui 0x1000, found %d" % (sec["name"], count)
    return patches, hooks


def write_file_table(build, rom, syms):
    """build/mt_files.inl: every file of the ROM table for the port's load hook."""
    code = {s["name"]: s for s in syms["section"]}
    table = read_table(rom)
    lines = ["// Generated by tools/gen_config.py - do not edit",
             "// { original ROM start, decompressed size, synth ROM (0 = no code), vram, file index }",
             "static const MtFile mt_files[] = {"]
    for idx, (start, end) in enumerate(table):
        size = int.from_bytes(rom[start + 1:start + 4], "big") if is_compressed(rom, start, end) else end - start
        sec = code.get("f%d" % idx)
        lines.append("    { 0x%08X, 0x%06X, 0x%08X, (int32_t)0x%08X, %d }," % (
            start, size, sec["rom"] if sec else 0, sec["vram"] if sec else 0, idx))
    lines.append("};")

    # file 209: data words that point into its TLB window, relocated like the code
    data = is_compressed(rom, *table[209])
    _, rep = relocate(data, S.TLB_209_VRAM, S.TLB_209_BUFFER)
    lines.append("// file 209: { offset, relocated value } of the words pointing into 0x50000000")
    lines.append("static const MtWordPatch mt_209_words[] = {")
    lines += ["    { 0x%04X, (int32_t)0x%08X }," % (off, new) for off, _, new in rep["data"]]
    lines.append("};")
    (build / "mt_files.inl").write_text("\n".join(lines) + "\n")


def write_aspmain_config(build, rom, main_rom):
    """build/aspMain.toml for RSPRecomp: the audio microcode, whose command handlers are
    reached through the table at the start of its data (indirect branch targets)."""
    data_off = main_rom + S.ASPMAIN_DATA - S.MAIN_VRAM
    table = [struct.unpack_from(">H", rom, data_off + 2 * i)[0] for i in range(16)]
    # IMEM addresses are 12 bits: entries appear both as 0x10EC and 0x02B0 (= 0x12B0).
    # Unused commands hold 0.
    imem_start, imem_end = 0x1080, 0x1080 + S.ASPMAIN_TEXT_SIZE
    targets = sorted({0x1000 | (t & 0xFFF) for t in table
                      if imem_start <= (0x1000 | (t & 0xFFF)) < imem_end})
    out = [
        "# Generated by tools/gen_config.py - do not edit",
        "text_offset = 0x%08X" % (main_rom + S.ASPMAIN_TEXT - S.MAIN_VRAM),
        "text_size = 0x%X" % S.ASPMAIN_TEXT_SIZE,
        "text_address = 0x04001080",
        'rom_file_path = "recomp.z64"',
        'output_file_path = "rsp/aspMain.cpp"',
        'output_function_name = "aspMain"',
        "extra_indirect_branch_targets = [ %s ]" % ", ".join("0x%04X" % t for t in targets),
    ]
    (build / "aspMain.toml").write_text("\n".join(out) + "\n")
    (build / "rsp").mkdir(exist_ok=True)
    print("aspMain: %d command handlers" % len(targets))


def main():
    build = Path(sys.argv[1])
    rom = (build / "recomp.z64").read_bytes()
    syms = tomllib.loads((build / "syms.toml").read_text())
    main_rom = next(s["rom"] for s in syms["section"] if s["name"] == "main")
    write_file_table(build, rom, syms)
    write_aspmain_config(build, rom, main_rom)

    patches, hooks = window_patches(rom, syms)
    # boot stub: `jr $t2` always goes to BOOT_ENTRY; make it a direct tail call.
    # N64Recomp renames the entrypoint function before applying patches.
    patches.append(("recomp_entrypoint", S.BOOT_STUB_JR,
                    0x08000000 | ((S.BOOT_ENTRY >> 2) & 0x03FFFFFF)))
    patches.append(("main_80031000", START_THREAD_CALL, 0x0C000000 | ((OS_START_THREAD >> 2) & 0x03FFFFFF)))
    patches += kseg1_patches(rom, syms)
    # boot entry reads cop0 Count twice (passed to main); N64Recomp only handles Status
    for vram, reg in BOOT_COUNT_READS:
        patches.append(("boot_%08X" % S.BOOT_ENTRY, vram, 0))
        hooks.append(("boot_%08X" % S.BOOT_ENTRY, vram,
                      "osGetCount_recomp(rdram, ctx); ctx->r%d = ctx->r2;" % reg))
    hooks += [
        ("boot_%08X" % S.BOOT_ENTRY, BOOT_J_MAIN,
         "mt_on_main_loaded(rdram, 0x%08X, 0x%08X, 0x%X);" % (main_rom, S.MAIN_VRAM, MAIN_SIZE)),
        ("main_%08X" % ROM_LZ_READ, ROM_LZ_READ, "mt_file_load_begin((uint32_t)ctx->r4);"),
        ("main_%08X" % ROM_LZ_READ, ROM_LZ_READ_EPILOGUE,
         "mt_file_load_end(rdram, (int32_t)MEM_W(0x210C, ctx->r29));"),
        ("main_%08X" % LOAD_RAW, LOAD_RAW_DONE,
         "mt_on_file_loaded(rdram, (uint32_t)ctx->r16, (int32_t)ctx->r17);"),
    ]
    hooks += [(func, vram, "yield_self(rdram);") for func, vram in SPIN_WAITS]

    out = [
        "# Generated by tools/gen_config.py - do not edit",
        "[input]",
        'symbols_file_path = "syms.toml"',
        'rom_file_path = "recomp.z64"',
        'output_func_path = "RecompiledFuncs"',
        "entrypoint = 0x%08X" % S.BOOT_STUB,
        'recomp_include = "#include \\"recomp.h\\"\\n#include \\"mt_hooks.h\\""',
        "",
        "[patches]",
        "# boot cache maintenance after decompressing main: meaningless in the port",
        "stubs = [ %s ]" % ", ".join('"boot_%08X"' % v for v in BOOT_CACHE_FUNCS),
        "ignored = [ %s ]" % ", ".join('"%s"' % f for f in IGNORED),
        "",
    ]
    for func, vram, value in patches:
        out += ["[[patches.instruction]]", 'func = "%s"' % func, "vram = 0x%08X" % vram,
                "value = 0x%08X" % value, ""]
    for func, vram, text in hooks:
        out += ["[[patches.hook]]", 'func = "%s"' % func, "before_vram = 0x%08X" % vram,
                "text = '%s'" % text, ""]
    (build / "recomp.toml").write_text("\n".join(out))
    print("instruction patches: %d, hooks: %d" % (len(patches), len(hooks)))


if __name__ == "__main__":
    main()
