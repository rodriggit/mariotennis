// The Transfer Pak with the Game Boy Color Mario Tennis in it, for the game's Game Boy link
// (file 71: NuSystem's nuContGBPak* at 0x80121DD0 over libultra's osGbpak* at 0x80122370).
// Those routines reach the pak only through __osPfsGetStatus, __osContRamRead and
// __osContRamWrite, implemented here.
//
// The game checks three things:
// - the ROM header: the Nintendo logo, the title "CGBTENNIS" / BM8E, licensee "01" and the
//   header checksum;
// - that the cartridge's address lines work: ROM read at 0x0, 0x80, 0x100, ... 0x4000 must
//   differ from one address to the next, and disabled RAM must differ from ROM;
// - then it reads its save from the cartridge RAM (0xA000, bank 0), and writes to it when
//   it sends data to the GBC game.
// So the GBC ROM isn't needed. The header is built from the copies of the logo and title
// the game compares against (in file 71), the rest of the ROM is filler that differs at
// every address, and the RAM is the GBC game's .sav ([transfer_pak] in config.ini). The
// .sav is read again each time the game powers the pak on. What the game writes goes back
// to the .sav when it disables the RAM or powers the pak off; before the first write of a
// run, the file is copied to .sav.bak.
//
// Without a .sav there is no pak, as the runtime does for the Controller Pak; rumble goes
// through the runtime's osMotor* / __osMotorAccess.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "recomp.h"
#include "mt_hooks.h"
#include "mt_config.h"
#include "mt_pak.h"

namespace {
    constexpr gpr PFS_ERR_NOPACK = 1;

    // f71 data the game compares the cartridge header against (osGbpakReadId and 0x80120360)
    constexpr int32_t kGameLogo = (int32_t)0x80125EE0;    // 0x30 bytes, header 0x104
    constexpr int32_t kGameTitle = (int32_t)0x80125C20;   // 15 bytes, header 0x134
    constexpr uint8_t kSaveSignature[] = "CAMELOTGBTENNIS";   // the GBC save, at RAM 0x20
    constexpr size_t kSaveMinSize = 0x2000;   // the game reads offsets below 0x2000

    std::filesystem::path save_path;
    int pak_port = -1;

    // Transfer Pak registers, by pak address (32-byte blocks):
    // 0x8000 power (0x84 on, 0xFE off; reads back 0x84 when on);
    // 0xA000 which 16 KB of the cartridge's address space 0xC000-0xFFFF shows (0-3);
    // 0xB000 access mode (1 = cartridge on) when written; status when read;
    // 0xC000-0xFFFF the cartridge.
    bool powered = false;
    bool access = false;
    uint8_t window_bank = 0;

    // The cartridge, an MBC3 with 32 KB of battery RAM.
    std::array<uint8_t, 0x150> header{};
    std::array<uint8_t, 0x8000> sram{};
    bool ram_enabled = false;
    uint8_t rom_bank = 1;
    uint8_t ram_bank = 0;

    // The .sav as last read: sram goes over its first bytes when written back, so anything
    // an emulator keeps after the RAM (a clock) stays as it was.
    std::vector<uint8_t> save_file;
    bool sram_dirty = false;
    bool backed_up = false;

    // Reads the .sav into sram (the rest stays 0xFF). Returns an error text, empty on success.
    std::string read_save() {
        std::ifstream in(save_path, std::ios::binary);
        if (!in) {
            return "could not open " + mt::config::path_text(save_path);
        }
        std::vector<uint8_t> data{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        if (data.size() < kSaveMinSize) {
            return mt::config::path_text(save_path) + " is too small for a Game Boy save (" +
                   std::to_string(data.size()) + " bytes)";
        }
        if (std::memcmp(data.data() + 0x20, kSaveSignature, sizeof(kSaveSignature) - 1) != 0) {
            return mt::config::path_text(save_path) + " is not a save of Mario Tennis for Game Boy Color";
        }
        sram.fill(0xFF);
        std::memcpy(sram.data(), data.data(), std::min(data.size(), sram.size()));
        save_file = std::move(data);
        sram_dirty = false;
        return {};
    }

    // Writes sram back to the .sav (only over the file's own size), through a temporary
    // file so a failed write leaves the old save.
    void write_save() {
        if (!sram_dirty) {
            return;
        }
        sram_dirty = false;
        std::error_code ec;
        if (!backed_up) {
            std::filesystem::path backup = save_path;
            backup += ".bak";
            std::filesystem::copy_file(save_path, backup, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                fprintf(stderr, "[mt] Transfer Pak: could not copy the save to %s (%s); not writing it\n",
                        mt::config::path_text(backup).c_str(), ec.message().c_str());
                return;
            }
            backed_up = true;
        }
        std::memcpy(save_file.data(), sram.data(), std::min(save_file.size(), sram.size()));
        std::filesystem::path temp = save_path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(save_file.data()), std::streamsize(save_file.size()));
            out.close();
            if (!out) {
                fprintf(stderr, "[mt] Transfer Pak: could not write %s\n", mt::config::path_text(temp).c_str());
                std::filesystem::remove(temp, ec);
                return;
            }
        }
        std::filesystem::rename(temp, save_path, ec);
        if (ec) {
            fprintf(stderr, "[mt] Transfer Pak: could not replace %s (%s)\n",
                    mt::config::path_text(save_path).c_str(), ec.message().c_str());
            std::filesystem::remove(temp, ec);
            return;
        }
        fprintf(stderr, "[mt] Transfer Pak: wrote %s\n", mt::config::path_text(save_path).c_str());
    }

    // ROM header 0x100-0x14F, with the logo and title from the game's own copies.
    void build_header(uint8_t* rdram) {
        header.fill(0);
        const uint8_t entry[] = { 0x00, 0xC3, 0x50, 0x01 };   // nop; jp 0x150
        std::memcpy(&header[0x100], entry, sizeof(entry));
        for (int i = 0; i < 0x30; i++) {
            header[0x104 + i] = MEM_BU(i, kGameLogo);
        }
        for (int i = 0; i < 15; i++) {
            header[0x134 + i] = MEM_BU(i, kGameTitle);
        }
        header[0x143] = 0x80;   // Game Boy Color features
        header[0x144] = '0';    // licensee 01, Nintendo
        header[0x145] = '1';
        header[0x147] = 0x13;   // MBC3 + RAM + battery
        header[0x148] = 0x05;   // 1 MB ROM
        header[0x149] = 0x03;   // 32 KB RAM
        header[0x14A] = 0x01;   // not Japan
        header[0x14B] = 0x33;   // licensee in 0x144
        uint8_t sum = 0;
        for (int i = 0x134; i <= 0x14C; i++) {
            sum = uint8_t(sum - header[i] - 1);
        }
        header[0x14D] = sum;
    }

    uint8_t rom_byte(uint32_t address) {
        if (address >= 0x100 && address < 0x150) {
            return header[address];
        }
        // Filler: a different value at each address (the connector test compares them).
        uint32_t x = address * 0x9E3779B1u;
        x ^= x >> 15;
        x *= 0x85EBCA77u;
        return uint8_t(x >> 24);
    }

    uint8_t cart_read(uint16_t address) {
        if (address < 0x4000) {
            return rom_byte(address);
        }
        if (address < 0x8000) {
            return rom_byte(rom_bank * 0x4000u + (address - 0x4000u));
        }
        if (address >= 0xA000 && address < 0xC000 && ram_enabled) {
            return sram[ram_bank * 0x2000u + (address - 0xA000u)];
        }
        return 0xFF;
    }

    void cart_write(uint16_t address, uint8_t value) {
        if (address < 0x2000) {
            ram_enabled = (value & 0x0F) == 0x0A;
            if (!ram_enabled) {
                write_save();
            }
        } else if (address < 0x4000) {
            rom_bank = (value & 0x7F) == 0 ? 1 : (value & 0x7F);
        } else if (address < 0x6000) {
            ram_bank = value & 0x03;
        } else if (address >= 0xA000 && address < 0xC000 && ram_enabled) {
            uint8_t& byte = sram[ram_bank * 0x2000u + (address - 0xA000u)];
            sram_dirty |= byte != value;
            byte = value;
        }
    }

    uint16_t cart_address(uint16_t pak_address, int i) {
        return uint16_t(window_bank * 0x4000u + (pak_address - 0xC000u) + i);
    }

    // One 32-byte block at pak address `address`.
    void pak_read(uint8_t* rdram, uint16_t address, int32_t buffer) {
        for (int i = 0; i < 32; i++) {
            uint8_t value = 0;
            if (address >= 0x8000 && address < 0x9000) {
                value = powered ? 0x84 : 0x00;
            } else if (address >= 0xB000 && address < 0xC000) {
                // cartridge present (0x80); powered and in access mode (0x08 | 0x01)
                value = powered && access ? 0x89 : 0x80;
            } else if (address >= 0xC000) {
                value = powered && access ? cart_read(cart_address(address, i)) : 0x00;
            }
            MEM_B(i, buffer) = (int8_t)value;
        }
    }

    void pak_write(uint8_t* rdram, uint16_t address, int32_t buffer) {
        const uint8_t first = MEM_BU(31, buffer);
        if (address >= 0x8000 && address < 0x9000) {
            write_save();   // anything still pending, before a reload or power-off
            if (first == 0x84 && !powered) {
                // Powered on (osGbpakInit): fresh cartridge state and save.
                build_header(rdram);
                const std::string error = read_save();
                if (!error.empty()) {
                    fprintf(stderr, "[mt] Transfer Pak: %s; keeping the save read before\n", error.c_str());
                }
                ram_enabled = false;
                rom_bank = 1;
                ram_bank = 0;
            }
            powered = first == 0x84;
            if (!powered) {
                access = false;
            }
        } else if (address >= 0xA000 && address < 0xB000) {
            window_bank = first & 0x03;
        } else if (address >= 0xB000 && address < 0xC000) {
            access = (first & 0x01) != 0;
        } else if (address >= 0xC000 && powered && access) {
            for (int i = 0; i < 32; i++) {
                cart_write(cart_address(address, i), MEM_BU(i, buffer));
            }
        }
    }

    bool has_pak(recomp_context* ctx) {
        return pak_port >= 0 && (int32_t)ctx->r5 == pak_port;
    }
}

void mt::pak::load_transfer_pak(const std::filesystem::path& config_folder) {
    std::string text = mt::config::get_text("transfer_pak", "gb_save");
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    if (text.empty()) {
        return;
    }
    const std::string controller = mt::config::get("transfer_pak", "controller");
    int port = 0;
    if (controller.size() == 1 && controller[0] >= '1' && controller[0] <= '4') {
        port = controller[0] - '1';
    } else {
        mt::config::warn("[transfer_pak] controller: expected 1 to 4, got \"" + controller + "\"");
    }
    save_path = std::filesystem::u8path(text);
    if (save_path.is_relative()) {
        save_path = config_folder / save_path;
    }
    const std::string error = read_save();
    if (!error.empty()) {
        mt::config::warn("[transfer_pak] gb_save: " + error + "; no Transfer Pak");
        return;
    }
    pak_port = port;
    fprintf(stderr, "[mt] Transfer Pak on controller %d: %s\n", port + 1, mt::config::path_text(save_path).c_str());
}

int mt::pak::transfer_pak_port() {
    return pak_port;
}

// __osPfsGetStatus(mq, channel): 0 = a pak is plugged in.
extern "C" void __osPfsGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = has_pak(ctx) ? 0 : PFS_ERR_NOPACK;
}

// __osContRamRead(mq, channel, block address, buffer)
extern "C" void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (!has_pak(ctx)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    pak_read(rdram, uint16_t(ctx->r6 << 5), (int32_t)ctx->r7);
    ctx->r2 = 0;
}

// __osContRamWrite(mq, channel, block address, buffer, force)
extern "C" void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (!has_pak(ctx)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    pak_write(rdram, uint16_t(ctx->r6 << 5), (int32_t)ctx->r7);
    ctx->r2 = 0;
}
