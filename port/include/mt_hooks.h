// Hooks called from recompiled code (inserted by build/recomp.toml, see tools/gen_config.py
// and docs/recomp_design.md). Implemented in the port.
#ifndef MT_HOOKS_H
#define MT_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// KSEG0 address of the buffer currently mapped at the 0x10000000 TLB window
// (set by osMapTLB_recomp); read by the per-character modules' data accesses.
extern int32_t mt_window_base;

// Boot: main (synth ROM main_rom, size bytes) has been decompressed at vram.
void mt_on_main_loaded(uint8_t* rdram, uint32_t main_rom, int32_t vram, uint32_t size);

// load_lz (0x80037DFC) finished: file at original ROM offset rom is now at dest.
void mt_on_file_loaded(uint8_t* rdram, uint32_t rom, int32_t dest);

// libultra functions N64Recomp ignores but the runtime does not implement.
// recomp.h (recomp_context) is included before this header in generated code.
void osMapTLB_recomp(uint8_t* rdram, recomp_context* ctx);
// pak access from the Transfer Pak code (port/src/mt_pak.cpp): always "no pak"
void __osPfsGetStatus_recomp(uint8_t* rdram, recomp_context* ctx);
void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx);
void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
