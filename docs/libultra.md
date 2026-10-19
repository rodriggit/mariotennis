# libultra in main

The table is `tools/libultra.py` (120 functions); build_recomp.py names them in
syms.toml and asserts that each address is a function start. Only names in
N64Recomp's reimplemented/ignored lists matter functionally; standard C helpers
(memcpy 0x80032CC0, strcpy 0x80032C80, strlen 0x80032CA0, bzero 0x80033240) are
recompiled as ordinary code and left unnamed.

## Layout
libultra is not contiguous: a few pieces sit in the game range
(0x80032BC0–0x800333A0, 0x8003A980–0x8003AE00) and the rest in
0x80040E60–0x80049A80. NuSys wrappers (nuPiInit 0x80040F20, nuPiReadRom
0x80040F60, controller/EEPROM/rumble managers 0x80041020–0x800416C8) are game
code and stay unnamed.

## How the less obvious ones were pinned
- __osDisableInt / __osRestoreInt: the one that clears SR.IE (mask 0xFFFE) and
  returns the old IE bit is DisableInt.
- osRecvMesg vs osSendMesg: Recv loops while validCount == 0 and reads
  msg[first]; Send writes msg[(first+validCount) % msgCount]. osJamMesg writes
  msg[(first+msgCount-1) % msgCount] and wakes the receiver through
  osStartThread (0x8003A980).
- osEepromLongWrite (0x80041780) calls osSetTimer + osRecvMesg (15 ms delay)
  around 0x80047E10, so that one is osEepromWrite; LongRead (0x80041870)
  only calls 0x80047BF0 = osEepromRead.
- osContInit (0x80043220) waits 0x165A0BC counts (500 ms). The helpers it
  shares with other functions fix osContStartQuery/GetQuery
  (__osPackRequestData / __osContGetInitData) versus osContStartReadData /
  GetReadData (__osPackReadData).
- __osPfsSelectBank (0x80047360) writes the bank to pak address 0x400 through
  0x800475C0 → __osContRamWrite; 0x800473D0 → __osContRamRead.
- __osEPiRawReadIo / WriteIo: same domain set-up, ReadIo ends with a load from
  KSEG1|addr, WriteIo with a store.
- osInitialize (0x80047908) is called directly by the game entry 0x80031000 (old
  single-function form, no __osInitialize_common wrapper).
- Cache: the game ships its own D-cache routines (TagLo walk); 0x8003AC10 /
  0x8003AC64 / 0x8003AD40 are named osWritebackDCacheAll / osWritebackDCache /
  osInvalDCache, which the runtime implements as no-ops — the right behaviour
  for a recomp and it keeps `cache`/TagLo out of the recompiler.

## Leftovers to watch when recompiling
- 0x80032FA0–0x800333A0 holds a second, game-local copy of exception-handler
  pieces (send_mesg-like code at 0x800332DC using __osEventStateTab, no
  callers). If N64Recomp rejects their cop0 instructions, list them as ignored
  in the config.
- Functions in N64Recomp's ignored list without a runtime implementation
  (osMapTLB, osMapTLBRdb, __osContRamRead/Write, __osSiRawStartDma, ...) need a
  `_recomp` implementation in the port if game code still calls them; the
  first compile will list exactly which ones.
- Transfer Pak code (0x80048F38–0x80049950, uses __osContRamRead/Write) is not
  covered by the runtime.
