# Phase 1 — Loader and overlay map

## File table
ROM 0x116C0–0x156C0: pairs of u32 (start, end) ROM offsets, 1275 files,
zero-terminated. Main receives its ROM address in a0 at 0x80031000.
Dump with `tools/filetable.py ROM [--code]`.

## Loader (main code)
| Function | Role |
|---|---|
| 0x80037CC0 | file_rom(index) → ROM start |
| 0x80037D44 | file_size(index) |
| 0x80035CC0 | lz_stream(rom, dest, size): PI DMA in 0x1000 chunks + decode |
| 0x80035AB0 | LZ decode core (same format as tools/mtlz.py) |
| 0x80037DFC | load_lz(index, dest) |
| 0x80037E48 | load_raw(index, dest) (uncompressed files) |
| 0x80042F20 | cache writeback/invalidate(addr, size), called after loads |
| 0x80032BC0 | osMapTLB |

`load_lz`/`load_raw` are the hook point for N64Recomp's overlay loading
(call the runtime's load_overlays(rom, ram, size) after each load).

## Code overlays
| File | ROM | Size | VRAM | Loaded by | Notes |
|---|---|---|---|---|---|
| main | 0x1350 | 0x2AE10 | 0x80031000 | boot | |
| 78 | 0xA01F60 | 0x1E0 | 0x80100000 | main | run once at boot, then overwritten |
| 19 | 0x958B70 | 0x94F8 | 0x802EC000 | main | resident |
| 0 | 0x156C0 | 0x1880 | 0x800C8000 | main | top-level mode switch |
| 31 | 0x963AE0 | 0xA020 | 0x800CA000 | 0, 71, 86, 462 | common lib (TLB range helper) |
| 86 | 0xA1F530 | 0x59330 | 0x800E2000 | 0, 454 | game mode A |
| 462 | 0xC6C320 | 0x418A0 | 0x800E2000 | 0 | game mode B (shares slot with 86) |
| 71 | 0x9CD770 | 0x41DB4 | 0x800E8000 | 0 | overlaps 86/462 range |
| 285 | 0xBAF500 | 0x18C8 | 0x802D8000 | 0 | shares slot with 454 |
| 454 | 0xC67B90 | 0x2C60 | 0x802D8000 | 0 | |
| 703 | 0xD19000 | 0x5CF0 | 0x80228000 | 86, 462 | |
| 209 | 0xB82F30 | 0x6370 | **0x50000000 (TLB)** | 86, 462 | osMapTLB(0x1F, 16K pages, 0x50000000 → load buffer), called via function pointers |
| 35 | 0x96AA30 | 0x140C | 0x80500000? | not found | above 4 MB; unused/debug or Expansion Pak only — check at runtime |

VRAMs for 71/86/462 confirmed by internal jal targets (1232/1378/729 hits).

Files 12, 125, 275, 379, 475, 477, 764, 814, 832, 881, 882, 976, 981 load at
0x80200000 (one at a time, by 86/462): small modules — 3-entry jump table,
2–3 real functions (e.g. heap setup 0x8020FBE0–0x80227FFF), then model data.
Ordinary overlays at 0x80200000 (probably courts/stages).

Files 17, 33, 99–106, 108, 147, 163, 178, 200, 473, 774, 965, 989, 1042:
0x1000–0x2000-byte modules compiled for the **TLB window 0x10000000**
(probably per-character logic, see below).

## Risks for N64Recomp
1. **TLB-mapped code (file 209 at 0x50000000) — solved, see below.**
2. **TLB code window at 0x10000000 — plan below.**
3. The runtime's osGetMemSize always returns 8 MiB (Expansion Pak present).
   The game may behave differently than on a 4 MiB console; file 35
   (built for 0x80500000) may be an Expansion-Pak-only path. Check at runtime.
4. Overlays sharing VRAM (86/462, 285/454, 71 overlapping) — supported by
   N64Recomp as separate sections, as long as the loader hook reports them.

## File 209: static relocation instead of TLB
Facts:
- Both loaders (86: 0x800FA7E0, called with a0 = 0x80230000 at 0x800FA8C8;
  462: 0x8010AB00, called at 0x8010ABF4 with 0x80230000) do
  osMapTLB(0x1F, 0x6000, 0x50000000, 0x00230000, 0x00234000), so the window
  0x50000000–0x50008000 is always the buffer 0x80230000–0x80238000.
- 209 reads/writes its own globals there (bss up to +0x63A0), uses jump
  tables and two data pointers (+0x6290, +0x6294). It reaches main only via
  jalr (no jal can leave the 0x5 segment).
- From outside only two entry pointers are used: 0x50000000 / 0x50000008,
  stored as data at 0x80138094 / 0x80138090 (86) and 0x8011FCA4 / 0x8011FCA0 (462).
- N64Recomp (a13e5cf) lists osMapTLB as ignored: not recompiled, no runtime
  implementation.

Plan:
1. Build: `tools/reloc_tlb.py ROM 209 50000000 80230000 out.bin` rewrites
   177 lui, 62 j/jal and 33 data words, with no anomalies; recompile the
   result as an ordinary overlay at VRAM 0x80230000.
2. Runtime, in the load hook (after load_lz):
   - file 209 loaded → rewrite the 33 data words in RAM (only +0x6290/+0x6294
     are read at runtime; jump tables become switch statements, patching them
     is harmless);
   - file 86/462 loaded → rewrite the two entry pointers to 0x80230000/0x80230008.
3. osMapTLB → our own `osMapTLB_recomp` (N64ModernRuntime df7e820 has none);
   for vaddr 0x50000000 it does nothing. The 0x8000-byte cache flush on
   0x50000000 is harmless: the runtime's osWritebackDCache/osInvalDCache/
   osInvalICache are empty (librecomp/src/ultra_translation.cpp).

## The 0x10000000 window: per-character modules
Facts:
- osMapTLB(0x1F, 4K pages, 0x10000000, phys(buf), phys(buf)+0x1000), then
  jalr 0x10000000 (the pointer is a constant in 86/462 data, 0x80139178 /
  0x8011FFC8). buf is per player: 86 table 0x80139140[player], 462 table at
  0x8011FF90[...]; 454 uses the fixed 0x80230000.
- Same TLB index (0x1F) as file 209: only one of the two windows is live at a time.
- All 20 modules have the same shape: entry `j 0x10000124`, a small function
  at +0x10, and exactly 6 `lui 0x1000`:
  - 2 × lwc1 of own float constants,
  - 3 × addiu → pointers to own float tables (0x3C apart), passed in a3 to
    0x800E2020 (game-mode code),
  - 1 × addiu 0x10 → `jalr 0x10000010` (internal call through a pointer).
  No stores, no 0x1000xxxx pointers in data. Calls out only via jalr.
- Two players can run different modules, or the same module from different
  buffers, so a static relocation like 209's does not work.

Plan (N64Recomp a13e5cf supports `[[patches.instruction]]` and
`[[patches.hook]]` with func / before_vram / text):
1. Recompile each module as its own section with VRAM 0x10000000 (20
   sections sharing a VRAM, like overlays).
2. Load hook: remember which module was loaded into which buffer.
3. `osMapTLB_recomp` for vaddr 0x10000000: set a global `window_base` =
   KSEG0 address of the buffer, and tell the runtime that this module's
   section is the one loaded at 0x10000000 (unload the previous one). That
   makes `jalr 0x10000000` and `jalr 0x10000010` resolve to the right module.
4. For the 5 data `lui 0x1000` per module (100 in total, generated by script):
   instruction patch to nop + hook setting the register to `window_base`, so
   the reads hit the player's real buffer.
