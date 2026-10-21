// The Transfer Pak (port/src/mt_pak.cpp), set up from [transfer_pak] in config.ini.
#ifndef MT_PAK_H
#define MT_PAK_H

#include <filesystem>

namespace mt::pak {
    // Reads [transfer_pak]; relative paths start from config_folder. Problems go to
    // mt::config::warn. Call after mt::config::load.
    void load_transfer_pak(const std::filesystem::path& config_folder);

    // The controller (0-3) with the Transfer Pak, or -1 if there is none.
    int transfer_pak_port();
}

#endif
