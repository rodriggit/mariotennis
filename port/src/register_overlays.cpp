// Registers N64Recomp's section table (build/RecompiledFuncs/recomp_overlays.inl) with the
// runtime, so load_overlays/unload_overlays and function lookups know every code section.
#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "recomp_overlays.inl"

void mt_register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections {
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };
    recomp::overlays::overlays_by_index_t overlays {
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };
    recomp::overlays::register_overlays(sections, overlays);
}
