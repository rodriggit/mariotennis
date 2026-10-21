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

// A file at original ROM offset rom is now at dest (load_raw, and mt_file_load_end).
void mt_on_file_loaded(uint8_t* rdram, uint32_t rom, int32_t dest);

// rom_lz_read (0x80035CC0) entry and exit: decompressing the file at rom into dest.
void mt_file_load_begin(uint32_t rom);
void mt_file_load_end(uint8_t* rdram, int32_t dest);

// Runtime scheduling (ultramodern/src/scheduling.cpp): wait for an external event, then
// run the highest-priority ready thread. Called from the idle thread's loop.
void yield_self(uint8_t* rdram);

// libultra functions N64Recomp ignores but the runtime does not implement.
// recomp.h (recomp_context) is included before this header in generated code.
void osMapTLB_recomp(uint8_t* rdram, recomp_context* ctx);
// the game's osVirtualToPhysical, TLB-aware (the runtime's ignores TLB mappings)
void osVirtualToPhysical_tlb(uint8_t* rdram, recomp_context* ctx);
// the game's osViSetXScale (the runtime's ignores the factor)
void osViSetXScale_mt(uint8_t* rdram, recomp_context* ctx);
// NuSys EEPROM manager callbacks (0x80041698 read, 0x800416C8 write), after their
// osEepromLongRead/Write call: print the access with its data (MT_TRACE_EEPROM)
void mt_eeprom_trace(uint8_t* rdram, recomp_context* ctx, int write);
// main_800346A0's on-screen test: the camera's half width in pixels, widened for the
// window's aspect ratio when aspect_ratio = expand
int32_t mt_cull_half_width(int32_t half_width);
// f71_800ECBE4: the menus' Toad x after its character-select slide (slide 0..8, 8 = fully
// out); pushed further out when aspect_ratio = expand widens the picture
float mt_menu_toad_x(float x, int32_t slide);
// f71_800FA614: the x of the main menu's selected-character coin (from the picture's
// center); moved past the wide picture when its slot is off the 4:3 one
float mt_menu_coin_x(float x);
// f71_800EBA18 / f71_801001DC: the "MARIO TENNIS" banners of the main menu and play mode.
// Writes RT64 extended GBI commands at gfx before a banner strip's texrect (part 0 top,
// 1 bottom; strip 0..2) or after the last one (part 2) and returns the new gfx pointer,
// so with aspect_ratio = expand the banners sit against the edges of the wide picture
int32_t mt_menu_banner(uint8_t* rdram, int32_t gfx, int32_t part, int32_t strip);
// f86_8012CEC4: the match's pause menu has filled its item list; merge Rules and Controller
// and append the exit item
void mt_pause_items(uint8_t* rdram);
// f86_8012DF60: start of a pause menu frame (buttons = pressed this frame); A on the Rules
// panel fades its text out and switches it to the controls. Returns the buttons to use
uint32_t mt_pause_frame(uint8_t* rdram, uint32_t buttons);
// f86_8013083C: the alpha the rules text is drawn with
int32_t mt_pause_text_alpha(int32_t alpha);
// f86_8012DF60: the id of the item whose action runs; returns the one to run
int32_t mt_pause_action(int32_t item);
// f86_8012DF60: Controller's action has loaded its image
void mt_pause_controller(uint8_t* rdram);
// pak access from the Transfer Pak code (port/src/mt_pak.cpp): the emulated Transfer Pak
// when [transfer_pak] gb_save is set, else "no pak"
void __osPfsGetStatus_recomp(uint8_t* rdram, recomp_context* ctx);
void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx);
void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
