// Load hooks and TLB emulation for Mario Tennis (see docs/recomp_design.md).
//
// The game decompresses its own overlays (load_lz, 0x80037DFC) and runs code
// through two TLB windows. The runtime has no TLB and only knows overlays by
// their ROM address in our recomp ROM, so:
//  - every file load is reported here; code files are registered with the
//    runtime at their load address, replacing whatever overlapped them;
//  - file 209 (TLB 0x50000000) is recompiled at its physical buffer 0x80230000:
//    its pointer words and the two entry pointers held by overlays 86/462 are
//    relocated in RAM after loading;
//  - the per-character modules (TLB 0x10000000) stay at 0x10000000: osMapTLB
//    swaps which module is registered there and sets mt_window_base, which the
//    modules' data accesses read (hooks in recomp.toml).
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "mt_hooks.h"

namespace {

struct MtFile {
    uint32_t orig_rom;
    uint32_t size;
    uint32_t synth_rom;   // 0: no code
    int32_t vram;
    uint16_t index;
};

struct MtWordPatch {
    uint32_t offset;
    int32_t value;
};

#include "mt_files.inl"

constexpr int32_t WINDOW_VRAM = (int32_t)0x10000000;
constexpr int32_t TLB_209_BUFFER = (int32_t)0x80230000;

// Entry pointers to file 209 held in the data of overlays 86 and 462.
struct EntryPtr {
    uint16_t file;
    int32_t addr;
    int32_t value;
};
constexpr EntryPtr entry_ptrs[] = {
    { 86, (int32_t)0x80138090, TLB_209_BUFFER + 8 },
    { 86, (int32_t)0x80138094, TLB_209_BUFFER },
    { 462, (int32_t)0x8011FCA0, TLB_209_BUFFER + 8 },
    { 462, (int32_t)0x8011FCA4, TLB_209_BUFFER },
};

struct Region {
    int32_t ram;
    uint32_t size;
    const MtFile* file;
};

// MT_TRACE=1 logs every file load and TLB mapping.
const bool trace = std::getenv("MT_TRACE") != nullptr;

std::vector<Region> registered;      // code registered with the runtime by us
std::vector<Region> window_buffers;  // window modules sitting in RAM, not registered
const MtFile* window_current = nullptr;

const MtFile* find_file(uint32_t rom) {
    for (const MtFile& f : mt_files) {
        if (f.orig_rom == rom) {
            return &f;
        }
    }
    return nullptr;
}

bool overlaps(int32_t a, uint32_t as, int32_t b, uint32_t bs) {
    return (int64_t)a < (int64_t)b + bs && (int64_t)b < (int64_t)a + as;
}

// Forget everything we tracked inside [ram, ram+size): unload registered code
// with its exact extent (the runtime aborts on partial unloads).
void clear_region(int32_t ram, uint32_t size) {
    for (auto it = registered.begin(); it != registered.end();) {
        if (overlaps(it->ram, it->size, ram, size)) {
            unload_overlays(it->ram, it->size);
            it = registered.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = window_buffers.begin(); it != window_buffers.end();) {
        if (overlaps(it->ram, it->size, ram, size)) {
            it = window_buffers.erase(it);
        } else {
            ++it;
        }
    }
}

void patch_word(uint8_t* rdram, int32_t addr, int32_t value) {
    MEM_W(0, (gpr)addr) = value;
}

} // namespace

extern "C" {

int32_t mt_window_base = 0;

void mt_on_main_loaded(uint8_t* rdram, uint32_t main_rom, int32_t vram, uint32_t size) {
    (void)rdram;
    if (trace) {
        fprintf(stderr, "[mt] main decompressed at 0x%08X\n", (uint32_t)vram);
    }
    load_overlays(main_rom, vram, size);
}

void mt_on_file_loaded(uint8_t* rdram, uint32_t rom, int32_t dest) {
    const MtFile* f = find_file(rom);
    if (f == nullptr) {
        fprintf(stderr, "[mt] load of unknown ROM offset 0x%08X to 0x%08X\n", rom, (uint32_t)dest);
        return;
    }
    if (trace) {
        fprintf(stderr, "[mt] file %u (%s) -> 0x%08X, 0x%X bytes\n", f->index,
                f->synth_rom == 0 ? "data" : "code", (uint32_t)dest, f->size);
    }
    clear_region(dest, f->size);
    if (f->synth_rom == 0) {
        return;  // data
    }

    if (f->vram == WINDOW_VRAM) {
        // per-character module: registered at 0x10000000 when osMapTLB maps this buffer
        window_buffers.push_back({ dest, f->size, f });
        return;
    }
    if (f->vram != dest) {
        fprintf(stderr, "[mt] file %u loaded at 0x%08X, expected 0x%08X\n",
                f->index, (uint32_t)dest, (uint32_t)f->vram);
    }
    load_overlays(f->synth_rom, dest, f->size);
    registered.push_back({ dest, f->size, f });

    if (f->index == 209) {
        for (const MtWordPatch& p : mt_209_words) {
            patch_word(rdram, dest + (int32_t)p.offset, p.value);
        }
    }
    for (const EntryPtr& e : entry_ptrs) {
        if (e.file == f->index) {
            patch_word(rdram, e.addr, e.value);
        }
    }
}

// rom_lz_read advances its ROM pointer while reading, so the start is kept per thread
// (a load blocks on DMA, and another game thread may load meanwhile).
static thread_local uint32_t loading_rom = 0;

void mt_file_load_begin(uint32_t rom) {
    loading_rom = rom;
}

void mt_file_load_end(uint8_t* rdram, int32_t dest) {
    mt_on_file_loaded(rdram, loading_rom, dest);
}

// Software TLB: physical RDRAM offset of every 4 KB page of the user segment, read by the
// memory access macros (port/include/recomp.h). Unmapped pages keep the original
// translation, which points past RDRAM and faults.
uint32_t mt_tlb_pages[0x80000];

static uint32_t unmapped_page(uint32_t page) {
    return (page << 12) + 0x80000000u;
}

static const bool tlb_initialized = [] {
    for (uint32_t page = 0; page < 0x80000; page++) {
        mt_tlb_pages[page] = unmapped_page(page);
    }
    return true;
}();

// What each of the 32 TLB entries maps, to undo it when the entry is reused.
struct TlbEntry {
    uint32_t vaddr;
    uint32_t size;   // both halves
};
static TlbEntry tlb_entries[32];

static void tlb_set_range(uint32_t vaddr, uint32_t size, int64_t paddr) {
    for (uint32_t off = 0; off < size; off += 0x1000) {
        uint32_t page = (vaddr + off) >> 12;
        if (page >= 0x80000) {
            continue;
        }
        mt_tlb_pages[page] = paddr < 0 ? unmapped_page(page) : (uint32_t)(paddr + off);
    }
}

// osMapTLB(index, pm, vaddr, evenpaddr, oddpaddr, asid); oddpaddr -1 = invalid
void osMapTLB_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t index = (uint32_t)ctx->r4 & 31;
    uint32_t page_size = ((uint32_t)ctx->r5 + 0x2000) >> 1;   // OS_PM_4K = 0 -> 4 KB
    int32_t vaddr = (int32_t)ctx->r6;
    int32_t even_paddr = (int32_t)ctx->r7;
    int32_t odd_paddr = MEM_W(0x10, ctx->r29);
    if (trace) {
        fprintf(stderr, "[mt] osMapTLB %u: 0x%08X -> 0x%08X / 0x%08X, page 0x%X\n", index,
                (uint32_t)vaddr, (uint32_t)even_paddr, (uint32_t)odd_paddr, page_size);
    }

    TlbEntry& entry = tlb_entries[index];
    if (entry.size != 0) {
        tlb_set_range(entry.vaddr, entry.size, -1);
    }
    entry = { (uint32_t)vaddr, page_size * 2 };
    tlb_set_range((uint32_t)vaddr, page_size, even_paddr == -1 ? -1 : (int64_t)(even_paddr & 0x1FFFFFFF));
    tlb_set_range((uint32_t)vaddr + page_size, page_size, odd_paddr == -1 ? -1 : (int64_t)(odd_paddr & 0x1FFFFFFF));

    if (vaddr != WINDOW_VRAM) {
        return;  // data windows (0x02000000...) and file 209 (recompiled at its buffer)
    }
    // per-character module window: swap which module's code is registered at 0x10000000

    int32_t base = (int32_t)(0x80000000u | (uint32_t)even_paddr);
    mt_window_base = base;

    const MtFile* module = nullptr;
    for (const Region& r : window_buffers) {
        if (r.ram == base) {
            module = r.file;
            break;
        }
    }
    if (module == window_current) {
        return;
    }
    if (window_current != nullptr) {
        unload_overlays(WINDOW_VRAM, window_current->size);
    }
    window_current = module;
    if (module != nullptr) {
        load_overlays(module->synth_rom, WINDOW_VRAM, module->size);
    } else {
        fprintf(stderr, "[mt] osMapTLB 0x10000000 -> 0x%08X: no module loaded there\n", (uint32_t)base);
    }
}

// The game's osVirtualToPhysical (0x80033160, renamed so the runtime's, which ignores the
// TLB, isn't used): KSEG0/KSEG1 strip the segment, TLB addresses use the page table.
void osVirtualToPhysical_tlb(uint8_t* rdram, recomp_context* ctx) {
    uint32_t addr = (uint32_t)ctx->r4;
    uint32_t phys;
    if (addr >= 0x80000000u && addr < 0xC0000000u) {
        phys = addr & 0x1FFFFFFF;
    } else if (addr < 0x80000000u && mt_tlb_pages[addr >> 12] != unmapped_page(addr >> 12)) {
        phys = mt_tlb_pages[addr >> 12] + (addr & 0xFFF);
    } else {
        phys = addr;
    }
    ctx->r2 = (gpr)(int32_t)phys;
}

// osViSetXScale (0x800464B0, renamed: the runtime's ignores the factor). File 71 sets 0.79
// so the VI shows only the left ~304 of the 384 framebuffer columns, stretched over the
// screen; RT64Context::update_screen applies the factor to VI_X_SCALE_REG.
static std::atomic<float> vi_x_scale_factor{1.0f};

void osViSetXScale_mt(uint8_t* rdram, recomp_context* ctx) {
    float factor = ctx->f12.fl;
    if (trace && factor != vi_x_scale_factor.load()) {
        fprintf(stderr, "[mt] osViSetXScale %f\n", factor);
    }
    vi_x_scale_factor.store(factor);
}

float mt_vi_x_scale_factor(void) {
    return vi_x_scale_factor.load();
}

float mt_widescreen_factor(void);   // rt64_context.cpp

// main_800346A0 (sphere on screen?) compares the projected x against the camera's half
// width in pixels. With aspect_ratio = expand RT64 shows more on each side, so the limit
// grows by the same factor; positions on screen, which use the same field, are untouched.
int32_t mt_cull_half_width(int32_t half_width) {
    return (int32_t)std::lround(half_width * mt_widescreen_factor());
}

// The menus' Toad is at x = x0 - 2 * slide: x0 - 16 when fully out on character select,
// just past the left edge of the 4:3 picture. Scaling that end position by the extra width
// keeps him as far past the new edge; the part of the slide done so far is scaled alike, so
// he slides in and out as before and is untouched with slide 0 or at 4:3.
float mt_menu_toad_x(float x, int32_t slide) {
    const float wide = mt_widescreen_factor();
    if (wide <= 1.0f || slide <= 0) {
        return x;
    }
    const float out_x = (x + 2.0f * float(slide)) - 16.0f;
    return x + (float(slide) / 8.0f) * out_x * (wide - 1.0f);
}

// After the NuSys EEPROM manager's osEepromLongRead/Write call: a1 = block, a2 = buffer,
// a3 = bytes, v0 = result. Prints the access with its data (MT_TRACE_EEPROM=1).
void mt_eeprom_trace(uint8_t* rdram, recomp_context* ctx, int write) {
    static const bool trace_eeprom = std::getenv("MT_TRACE_EEPROM") != nullptr;
    if (!trace_eeprom) {
        return;
    }
    const uint32_t block = (uint32_t)ctx->r5 & 0xFF;
    const gpr buffer = ctx->r6;
    const uint32_t bytes = (uint32_t)ctx->r7 & 0xFFFF;
    std::string hex;
    char b[4];
    for (uint32_t i = 0; i < bytes; i++) {
        snprintf(b, sizeof(b), "%02X", (unsigned)MEM_BU(buffer, i));
        hex += b;
    }
    fprintf(stderr, "[mt] EEPROM %s offset 0x%03X bytes 0x%03X buffer 0x%08X -> %d: %s\n", write ? "write" : "read ",
            block * 8, bytes, (uint32_t)buffer, (int)ctx->r2, hex.c_str());
}

} // extern "C"
