"""Code sections of Mario Tennis (USA) for recompilation.

Each entry: name, source, vram. Source is "boot", "main" or a file-table index.
VRAMs come from docs/phase1.md (loader call sites, confirmed by jal analysis).
"""

BOOT_ROM = 0x1000
BOOT_VRAM = 0x80300000
BOOT_TEXT_END = 0x340        # stub, decompressor, cache helpers; compressed main follows at +0x350
BOOT_STUB = 0x80300000       # clears bss, sets sp, `jr $t2` to BOOT_ENTRY: not recompiled
BOOT_ENTRY = 0x80300050      # recomp entrypoint: decompresses main, `j 0x80031000`

# function starts the generic rules cannot see (reached through jr reg)
FORCED_STARTS = [BOOT_ENTRY]

MAIN_ROM = 0x1350            # compressed
MAIN_VRAM = 0x80031000
# CPU code in main; 0x8003DA00-0x80040E60 is RSP microcode linked into .text
MAIN_TEXT = [(0x80031000, 0x8003DA00), (0x80040E60, None)]  # None: up to the end of libultra code

# RSP microcode linked into main's .text (NuSys ucode table at 0x80049AC4)
RSPBOOT_TEXT = 0x8003DA00          # 0xD0 bytes
ASPMAIN_TEXT = 0x8003DAD0          # audio ucode, 0xC60 bytes, loaded at IMEM 0x1080
ASPMAIN_TEXT_SIZE = 0xC60
ASPMAIN_DATA = 0x8005A8C0          # 0x2B0 bytes; starts with the 16-entry command table
F3DEX2_FIFO_TEXT = 0x8003E730      # data 0x8005AB70
F3DEX2_XBUS_TEXT = 0x8003FAC0      # data 0x8005AF90

TLB_209_VRAM = 0x50000000
TLB_209_BUFFER = 0x80230000
WINDOW_VRAM = 0x10000000

OVERLAYS = [
    (0, 0x800C8000),
    (19, 0x802EC000),
    (31, 0x800CA000),
    (71, 0x800E8000),
    (78, 0x80100000),
    (86, 0x800E2000),
    (462, 0x800E2000),
    (285, 0x802D8000),
    (454, 0x802D8000),
    (703, 0x80228000),
    (209, TLB_209_BUFFER),   # relocated from 0x50000000, see reloc_tlb.py
]

# small code+model modules loaded at 0x80200000 (courts/stages?)
STAGE_MODULES = [12, 125, 275, 379, 475, 477, 764, 814, 832, 881, 882, 976, 981]
STAGE_VRAM = 0x80200000

# per-character modules run through the TLB window at 0x10000000
WINDOW_MODULES = [17, 33, 99, 100, 101, 102, 103, 104, 105, 106, 108, 147, 163, 178, 200,
                  473, 774, 965, 989, 1042]


def all_file_sections():
    """(file index, vram) for every code file loaded through the file table."""
    out = list(OVERLAYS)
    out += [(i, STAGE_VRAM) for i in STAGE_MODULES]
    out += [(i, WINDOW_VRAM) for i in WINDOW_MODULES]
    return out
