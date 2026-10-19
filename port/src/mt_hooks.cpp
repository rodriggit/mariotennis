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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
constexpr int32_t TLB_209_VRAM = (int32_t)0x50000000;
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

// osMapTLB(index, pm, vaddr, evenpaddr, oddpaddr, asid)
void osMapTLB_recomp(uint8_t* rdram, recomp_context* ctx) {
    int32_t vaddr = (int32_t)ctx->r6;
    uint32_t even_paddr = (uint32_t)ctx->r7;
    if (trace) {
        fprintf(stderr, "[mt] osMapTLB 0x%08X -> 0x%08X\n", (uint32_t)vaddr, even_paddr);
    }

    if (vaddr == TLB_209_VRAM) {
        return;  // file 209 is recompiled at its buffer
    }
    if (vaddr != WINDOW_VRAM) {
        fprintf(stderr, "[mt] osMapTLB of unexpected vaddr 0x%08X\n", (uint32_t)vaddr);
        return;
    }

    int32_t base = (int32_t)(0x80000000u | even_paddr);
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

} // extern "C"
