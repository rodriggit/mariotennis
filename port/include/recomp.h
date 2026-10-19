// N64Recomp's recomp.h with TLB-aware memory accesses, for the recompiled game code and
// the port (this directory comes first in their include path; the runtime keeps the
// original).
//
// The original maps an address to rdram + (addr - 0x80000000): fine for KSEG0, but Mario
// Tennis also reads data through TLB mappings (e.g. 0x02000000, the CPU view of RSP
// segment 2). Every access now goes through mt_mem_offset: KSEG0 takes the same single
// compare, KSEG1 maps to the same physical memory, and user addresses (< 0x80000000) go
// through mt_tlb_pages, a 4 KB page table filled by osMapTLB_recomp (port/src/mt_hooks.cpp).
// The unaligned-access helpers follow N64Recomp's (MIT) but use the translated macros.
#ifndef MT_RECOMP_H
#define MT_RECOMP_H

// Rename the original helpers: they expand the original MEM_W at their definition.
#define load_doubleword mt_orig_load_doubleword
#define do_lwl mt_orig_do_lwl
#define do_lwr mt_orig_do_lwr
#define do_swl mt_orig_do_swl
#define do_swr mt_orig_do_swr
#define do_ldl mt_orig_do_ldl
#define do_ldr mt_orig_do_ldr
#define do_sdl mt_orig_do_sdl
#define do_sdr mt_orig_do_sdr
#include_next "recomp.h"
#undef load_doubleword
#undef do_lwl
#undef do_lwr
#undef do_swl
#undef do_swr
#undef do_ldl
#undef do_ldr
#undef do_sdl
#undef do_sdr

#ifdef __cplusplus
extern "C" {
#endif

// Physical RDRAM offset of each 4 KB page of the user segment (0x00000000-0x7FFFFFFF).
// Unmapped pages keep the original translation (addr + 0x80000000, past RDRAM), so a
// stray access still faults instead of silently hitting memory.
extern uint32_t mt_tlb_pages[0x80000];

#ifdef __cplusplus
}
#endif

static inline uint64_t mt_mem_offset(gpr addr) {
    uint32_t a = (uint32_t)addr;
    if (a - 0x80000000u < 0x20000000u) {
        return a - 0x80000000u;                       // KSEG0
    }
    if (a - 0xA0000000u < 0x20000000u) {
        return a - 0xA0000000u;                       // KSEG1
    }
    if (a < 0x80000000u) {
        return (uint64_t)mt_tlb_pages[a >> 12] + (a & 0xFFFu);   // TLB-mapped
    }
    return (uint64_t)(a - 0x80000000u);               // KSEG2/3: as the original
}

#undef MEM_W
#undef MEM_H
#undef MEM_B
#undef MEM_HU
#undef MEM_BU
#undef SD
#undef LD

#define MEM_W(offset, reg) \
    (*(int32_t*)(rdram + mt_mem_offset((reg) + (offset))))

#define MEM_H(offset, reg) \
    (*(int16_t*)(rdram + mt_mem_offset(((reg) + (offset)) ^ 2)))

#define MEM_B(offset, reg) \
    (*(int8_t*)(rdram + mt_mem_offset(((reg) + (offset)) ^ 3)))

#define MEM_HU(offset, reg) \
    (*(uint16_t*)(rdram + mt_mem_offset(((reg) + (offset)) ^ 2)))

#define MEM_BU(offset, reg) \
    (*(uint8_t*)(rdram + mt_mem_offset(((reg) + (offset)) ^ 3)))

#define SD(val, offset, reg) { \
    MEM_W((offset) + 4, reg) = (int32_t)(uint32_t)((gpr)(val) >> 0); \
    MEM_W((offset) + 0, reg) = (int32_t)(uint32_t)((gpr)(val) >> 32); \
}

static inline uint64_t load_doubleword(uint8_t* rdram, gpr reg, gpr offset) {
    uint64_t lo = (uint64_t)(uint32_t)MEM_W(reg, offset + 4);
    uint64_t hi = (uint64_t)(uint32_t)MEM_W(reg, offset + 0);
    return (lo << 0) | (hi << 32);
}

#define LD(offset, reg) \
    load_doubleword(rdram, offset, reg)

static inline gpr do_lwl(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = offset + reg;
    gpr word_address = address & ~0x3;
    uint32_t loaded_value = MEM_W(0, word_address);
    gpr misalignment = address & 0x3;
    gpr masked_value = initial_value & (gpr)(uint32_t)~(0xFFFFFFFFu << (misalignment * 8));
    loaded_value <<= (misalignment * 8);
    return (gpr)(int32_t)(masked_value | loaded_value);
}

static inline gpr do_lwr(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = offset + reg;
    gpr word_address = address & ~0x3;
    uint32_t loaded_value = MEM_W(0, word_address);
    gpr misalignment = address & 0x3;
    gpr masked_value = initial_value & (gpr)(uint32_t)~(0xFFFFFFFFu >> (24 - misalignment * 8));
    loaded_value >>= (24 - misalignment * 8);
    return (gpr)(int32_t)(masked_value | loaded_value);
}

static inline void do_swl(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = offset + reg;
    gpr word_address = address & ~0x3;
    uint32_t initial_value = MEM_W(0, word_address);
    gpr misalignment = address & 0x3;
    uint32_t masked_initial_value = initial_value & ~(0xFFFFFFFFu >> (misalignment * 8));
    uint32_t shifted_input_value = ((uint32_t)val) >> (misalignment * 8);
    MEM_W(0, word_address) = masked_initial_value | shifted_input_value;
}

static inline void do_swr(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = offset + reg;
    gpr word_address = address & ~0x3;
    uint32_t initial_value = MEM_W(0, word_address);
    gpr misalignment = address & 0x3;
    uint32_t masked_initial_value = initial_value & ~(0xFFFFFFFFu << (24 - misalignment * 8));
    uint32_t shifted_input_value = ((uint32_t)val) << (24 - misalignment * 8);
    MEM_W(0, word_address) = masked_initial_value | shifted_input_value;
}

static inline gpr do_ldl(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = offset + reg;
    gpr dword_address = address & ~0x7;
    uint64_t loaded_value = load_doubleword(rdram, 0, dword_address);
    gpr misalignment = address & 0x7;
    gpr masked_value = initial_value & ~(0xFFFFFFFFFFFFFFFFu << (misalignment * 8));
    loaded_value <<= (misalignment * 8);
    return masked_value | loaded_value;
}

static inline gpr do_ldr(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = offset + reg;
    gpr dword_address = address & ~0x7;
    uint64_t loaded_value = load_doubleword(rdram, 0, dword_address);
    gpr misalignment = address & 0x7;
    gpr masked_value = initial_value & ~(0xFFFFFFFFFFFFFFFFu >> (56 - misalignment * 8));
    loaded_value >>= (56 - misalignment * 8);
    return masked_value | loaded_value;
}

static inline void do_sdl(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = offset + reg;
    gpr dword_address = address & ~0x7;
    uint64_t initial_value = load_doubleword(rdram, 0, dword_address);
    gpr misalignment = address & 0x7;
    uint64_t masked_initial_value = initial_value & ~(0xFFFFFFFFFFFFFFFFu >> (misalignment * 8));
    uint64_t shifted_input_value = val >> (misalignment * 8);
    uint64_t ret = masked_initial_value | shifted_input_value;
    MEM_W(0, dword_address + 4) = (uint32_t)ret;
    MEM_W(0, dword_address + 0) = (uint32_t)(ret >> 32);
}

static inline void do_sdr(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = offset + reg;
    gpr dword_address = address & ~0x7;
    uint64_t initial_value = load_doubleword(rdram, 0, dword_address);
    gpr misalignment = address & 0x7;
    uint64_t masked_initial_value = initial_value & ~(0xFFFFFFFFFFFFFFFFu << (56 - misalignment * 8));
    uint64_t shifted_input_value = val << (56 - misalignment * 8);
    uint64_t ret = masked_initial_value | shifted_input_value;
    MEM_W(0, dword_address + 4) = (uint32_t)ret;
    MEM_W(0, dword_address + 0) = (uint32_t)(ret >> 32);
}

#endif
