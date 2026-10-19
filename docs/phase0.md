# Phase 0 — ROM survey (Mario Tennis USA, NM8E rev 0)

ROM: `source/Mario Tennis.z64` (not tracked), big-endian, 16 MiB,
header CRC 5001CF4F F30CB3BD (recomputed, matches), SHA1 999047f0…07ee.

## Boot
- IPL3 loads ROM 0x1000 → 0x80300000 (entry). Boot stub clears RAM
  0x80000400–0x80100000, sets sp = 0x803FF000.
- 0x80300080 decompresses ROM 0x1350 → 0x80031000 (0x2AE10 bytes, main code),
  flushes caches, then jumps to 0x80031000 with a0 = 0x116C0 (ROM offset
  right after the main block).

## Compression
Custom LZ, decoder in `tools/mtlz.py` (transcribed from 0x80300080).
Header: 1 type byte (0x01) + 24-bit decompressed size. 1268 blocks found
by scanning 16-aligned offsets.

## Code blocks (ROM offset → decompressed size, most-used `lui` = likely VRAM)
| ROM | size | hint |
|---|---|---|
| 0x0001350 | 0x2AE10 | main, 0x80031000 |
| 0x09CD770 | 0x41DB4 | 0x8013xxxx |
| 0x0A1F530 | 0x59330 | 0x8014–0x8016xxxx |
| 0x0C6C320 | 0x418A0 | 0x8012/0x8015xxxx |
| 0x0958B70 | 0x094F8 | 0x802Fxxxx |
| 0x0963AE0 | 0x0A020 | 0x800Dxxxx |
| small: 0x156C0, 0x96AA30, 0xBAF500, 0xD19000, 0xC67B90, 0xB82F30 | | |

Many ~0xA000–0x14000 blocks with exactly 3 `jr ra` and 0x8020/0x8021 hints:
probably data (models?) with a few functions — to check.

## Uncompressed regions
- 0x003C780 "N64 PtrTablesV2", 0x055B20 "N64 WaveTables": libaudio banks
  and samples, up to 0x930080 (~9 MB).
- 0x116C0–0x156C0, 0x94AB60–0x94ED20, 0x96B3D0–0x991D10, others: unknown.

## Hardware
- Graphics ucode: F3DEX2 2.08, fifo + xbus (strings at 0x8005ACA8/0x8005B0C8).
  Supported by RT64 (F3DEX2_FIFO_2_08, F3DEX2_XBUS_2_08).
- Save: EEPROM. osEepromProbe at 0x80041728; read/write at 0x80047C3C/0x80047E5C.
  4K vs 16K still to confirm. No SRAM/FlashRAM.
- Audio: Nintendo libaudio (PtrTablesV2/WaveTables).

## Next
Find the in-game loader (decompress + overlay table: ROM offset → VRAM),
needed for N64Recomp's overlay configuration.
