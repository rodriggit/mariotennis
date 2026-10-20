# Recompilation design

Based on N64Recomp a13e5cf and N64ModernRuntime df7e820 (read locally at the
pinned commits, see versions.md).

## Input to N64Recomp: symbol file + "recomp ROM"
N64Recomp's symbol-file mode (`[input] symbols_file_path` + `rom_file_path`,
parsed by `Context::from_symbol_file`, src/config.cpp) reads each function's
words from the given ROM at `section.rom + (func.vram - section.vram)`.

```toml
[[section]]
name = "main"
rom = 0x01000000
vram = 0x80031000
size = 0x2AE10
functions = [ { name = "func_80031000", vram = 0x80031000, size = 0x290 }, ... ]
# relocs = [...] would make the section relocatable; we don't use them
```

Our code is compressed, so we build our own **recomp ROM** (build artifact,
never committed): the original ROM followed by every code section,
decompressed, from 0x01000000 on (16-byte aligned):
- boot: rom 0x1000, vram 0x80300000 (original location, uncompressed)
- main, every code overlay, the 13 modules at 0x80200000, the 20 window
  modules (vram 0x10000000), and file 209 already relocated to 0x80230000.

Synthetic ROM addresses must stay out of 0x1000–0x101000: the runtime's
`init` calls `load_overlays(0x1000, entrypoint, 1 MiB)` and would register any
section in that range as loaded.

## Runtime overlay API (librecomp/src/overlays.cpp)
- `load_overlays(rom, ram, size)`: registers every section whose rom lies in
  [rom, rom+size) as loaded at `ram + (section.rom - rom)`.
- `unload_overlays(ram, size)`: unregisters sections overlapping the range;
  **exits if a section is only partially covered**.
- `get_function(addr)`: exact lookup for jalr / function pointers; exits if
  missing.

## Hooks we add
1. **Boot** (function 0x80300050, before `j 0x80031000`): register main with
   `load_overlays(MAIN_SYNTH_ROM, 0x80031000, 0x2AE10)`; the decompressor has
   already written main's code+data into RAM, as on hardware.
2. **File loads**: compressed files go through rom_lz_read(rom, dest, size)
   at 0x80035CC0, called by load_lz (0x80037DFC) and directly by the boot code
   main_80031290 (file 78). Its entry records the ROM start
   (`mt_file_load_begin`, per thread: the pointer saved on its stack advances
   while reading) and its epilogue reports the load with dest from sp+0x210C
   (`mt_file_load_end`). load_raw (0x80037E48) reports uncompressed loads after
   its DMA (s0 = ROM start, s1 = dest). Both end in `mt_on_file_loaded(rom, dest)`:
   - map original ROM start → (synthetic rom, size) for code files;
   - unload whatever we previously registered overlapping [dest, dest+size)
     using our own record of exact (ram, size), then `load_overlays`;
   - file 209: patch its 33 pointer words in RAM (0x5000xxxx → 0x8023xxxx);
   - files 86/462: patch the two 209 entry pointers (0x80138090/94,
     0x8011FCA0/A4) to 0x80230008/0x80230000;
   - window modules: remember which module sits in which buffer.
3. **osMapTLB_recomp** (our implementation): vaddr 0x10000000 → set
   `window_base`, unload the previous module at 0x10000000, load the module
   held in that buffer at 0x10000000. vaddr 0x50000000 → nothing.
4. **Window modules**: per module, 5 instruction patches (`lui rX, 0x1000` →
   nop) + 5 hooks (`ctx->rX = (gpr)(int32_t)window_base;`).

## Function boundaries
`tools/build_recomp.py ROM build/` writes build/recomp.z64 and build/syms.toml
(2257 functions in 46 sections); `tools/check_syms.py build/` checks that every
function ends in a jump. Method: spimdisasm per CPU-code range, then split at
starts it misses because they are only reached through pointers: leading
`j` entry tables (each stub is a function), jal targets and lui/addiu pointers
from any section, and `addiu sp,-N` prologues — a split is accepted only
right after `jr ra` + delay slot. Nop padding is merged into the previous
function.

Main's CPU code is two ranges: 0x80031000–0x8003DA00 (game) and
0x80040E60–0x80049A80 (libultra); 0x8003DA00–0x80040E60 is RSP microcode
(F3DEX2 text starts at 0x8003DA00 with `j 0x04001064`).

Shared tails: N64Recomp only accepts jumps to a function start or inside the
same function. A function that jumps or branches into the middle of a later
function, or falls through into the next one, is extended to cover that code
(`extend_shared_tails`); the functions then overlap, which is fine because each
one reads its own words from the ROM. Cases: the memset entries main_80033204
(falls into 0x80033210) and main_80033210 (`j 0x80033244`, inside
main_80033240); main_80032BA0 (probably data, no callers); and libultra's
__osException / __osCleanupThread, which N64Recomp ignores anyway (so does the
backward jump in send_mesg). check_syms now reports no function without a
terminator.

## Startup hot-patch and private libultra copies
main_80031000 (game startup) copies 0x800332DC–0x80033360 over send_mesg
(0x80041E6C) and 0x80033360–0x80033394 over __osEnqueueThread (0x80042064),
then starts its first thread (id 1, entry 0x80031D60, pri 10) with a private
copy of osStartThread at 0x80031C4C. The runtime replaces the exception
handler and thread functions, so: the copies are harmless RAM writes; the
`jal 0x80031C4C` at 0x80031134 is patched to `jal osStartThread`; and
main_80031C4C, main_800332DC and main_80032D20 (a wake-up helper only called by
osRecvMesg/osSendMesg) are `ignored`.

## Spin waits
The runtime is cooperative: a game thread keeps running until it blocks
(osRecvMesg...) or yields; there are no interrupts to preempt it, so a loop
polling a flag set by another thread never ends. gen_config.py's SPIN_WAITS
hooks each loop head with the runtime's `yield_self`, which waits for an
external event and runs the highest-priority ready thread, as an interrupt
would on hardware:
- 0x80031D90: NuSys' idle thread (main_80031D60, priority 0) polling the idle
  callback, which is only ever set to NULL (by main_80031E04);
- 0x80100018: boot overlay 78 waiting for `*(s32*)0x80062608 == 0`.

Candidates were found by scanning for short backward loops whose only memory
accesses are loads from a fixed (lui-based) address; the other hits are
libultra hardware polls the runtime replaces.

## Software TLB
Besides the code windows (0x10000000, 0x50000000), the game reads data through
TLB mappings: e.g. f31_800CA108 maps 0x02000000–0x02037FFF in 8 KB entries,
the CPU view of RSP segment 2, so pointers inside models/display lists can be
followed by the CPU. Those pointers live in data, so they can't be patched.

The runtime reserves 4 GB for RDRAM but backs only 512 MB, and recomp.h maps
an address to `rdram + (addr - 0x80000000)`, so 0x02xxxxxx faults.
`port/include/recomp.h` comes first in the include path of the generated code
and the port: it includes N64Recomp's recomp.h (`#include_next`) and redefines
the memory macros and unaligned-access helpers to go through `mt_mem_offset`:
KSEG0 in one compare as before, KSEG1 to the same memory, user addresses
through `mt_tlb_pages` (one entry per 4 KB page, the physical offset).
`osMapTLB_recomp` fills it for every mapping (both halves of the entry, page
size from the page mask) and undoes the previous mapping of a reused index.
The runtime itself keeps the original recomp.h.

The game's osVirtualToPhysical (0x80033160) uses tlbp/tlbr and the runtime's
ignores the TLB, so it is named osVirtualToPhysical_tlb, ignored, and
implemented in the port with the page table.

## VI X scale
The game's VI mode (set up at 0x800315B8) uses a 384-pixel framebuffer with
X scale 0x299, so the whole width fills the picture. File 71 also calls
osViSetXScale(0.7917), which on hardware makes the VI magnify only the left
~304 columns. The runtime ignores the factor and RT64 always displays the full
framebuffer width, so that picture showed up shifted left. osViSetXScale
(0x800464B0) is named osViSetXScale_mt, ignored, and implemented in the port: it
stores the factor, and RT64Context::update_screen scales VI_X_SCALE_REG with it
as __osViSwapContext does. A patch to RT64 (patches/rt64) makes the VI renderer
show only the columns that X scale covers. RT64 still ignores H start, so the
picture fills the width instead of having the small borders it has on a TV.

## Framebuffers sampled as textures
The game renders the crowd and line judges into 64x64 RGBA16 buffers
(0x28D980-0x2A2DC0) now and then, and samples them every frame as two 64x32
halves. Shadows go into 4 KB buffers (0x28C900-0x2A7F00): a 16-bit 64x32 fill
with 0xFFFF, then the silhouette drawn at the same address as a 64-wide I8
image of 38-51 rows, then a 64x64 I8 load. RT64 serves these loads from GPU
copies of its high-resolution framebuffers. patches/rt64/0002 fixes what went
wrong there (see docs/versions.md); MT_RT64_RDRAM_COPIES=1 switches to RDRAM
for comparison.

## KSEG1 accesses
The runtime maps only KSEG0 (0x80000000) to RDRAM. A few places use the
uncached view 0xA0000000: the boot decompressor's clear loop (0x8030008C,
0x8030009C), a memset of RDRAM 1–4 MB at startup (0x80031174), and two
`osVirtualToPhysical(p) + 0xA0000000` (0x800357E0, 0x80035A68). gen_config.py
patches each `lui rX, 0xA0nn` to `lui rX, 0x80nn` (KSEG1_LUIS). The only
hardware-register access outside libultra's known functions was osDpSetStatus
(0x80032C68), now named so the runtime replaces it. Other `lui 0xA…`/`0xB…` in
game code are float or integer constants (e.g. 0xBF80 = -1.0f, 0xAAAA dividers).

## Paks
The runtime emulates no Controller/Transfer Pak (osPfs* return
PFS_ERR_NOPACK); rumble works through osMotor*/__osMotorAccess. Main's pak
status routines are named (osPfsIsPlug, __osPfsGetStatus, ...) and the
Controller Pak id/inode routines (0x80048F38–0x8004960C) are `ignored`. The
Transfer Pak code in file 71 (0x80122xxx) calls __osPfsGetStatus,
__osContRamRead and __osContRamWrite: port/src/mt_pak.cpp returns
PFS_ERR_NOPACK from all three, so the game sees no Transfer Pak.

`tools/scan_unsupported.py build N64Recomp/src/symbol_lists.cpp
N64ModernRuntime port` lists the non-recompiled functions that recompiled code
calls and neither the runtime nor the port defines (must be 0 to link).

## Port side (port/)
- `port/src/mt_hooks.cpp`: mt_on_main_loaded, mt_on_file_loaded,
  osMapTLB_recomp, mt_window_base. Uses build/mt_files.inl (generated by
  gen_config.py: every file's original ROM start and decompressed size, synth
  ROM + VRAM for code files, and file 209's 33 pointer words).
- Save type: EEPROM 16 Kbit — the game reads the whole 0x800 bytes from
  address 0 (nuEepromRead call at 0x8003D614).

## Still to do
- Identify libultra functions in main by name: N64Recomp replaces calls to
  the names in its reimplemented/ignored lists (osCreateThread, osPiStartDma,
  osRecvMesg, ...) with runtime implementations, so they must be named.
- Audio microcode: locate it and recompile with RSPRecomp.
