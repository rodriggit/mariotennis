// Mario Tennis (USA) recompiled: registers the game with the runtime and wires SDL2 (window,
// audio, input) and RT64 (rendering). Minimal by design: no menu, the ROM path is given on
// the command line once (or the ROM is dropped onto the executable) and stored by the runtime.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include "SDL.h"
#include "SDL_syswm.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
#endif

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "xxHash/xxh3.h"

#include "mt_render.h"

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();
void mt_register_overlays();
void mt_install_crash_handler();
extern RspUcodeFunc aspMain;

// XXH3-64 of the big-endian (.z64) ROM: Mario Tennis (USA), NM8E rev 0
// (SHA-1 999047f07cec931ffbdcc7b33b8502ef602807ee). A mismatching ROM prints its hash.
constexpr uint64_t MT_ROM_HASH = 0x4E535EA5C4BB222FULL;
static const std::u8string game_id = u8"mariotennis.n64.us.1.0";

static SDL_Window* window = nullptr;

static void message_box(const char* msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Mario Tennis Recompiled", msg, window);
}

// ---------------------------------------------------------------------------------------
// Input. SDL is only touched on the main thread (update_gfx); the game thread reads a
// snapshot. Port 1 = keyboard + first gamepad, ports 2-4 = the other gamepads.

namespace btn {
    constexpr uint16_t A = 0x8000, B = 0x4000, Z = 0x2000, START = 0x1000;
    constexpr uint16_t DU = 0x0800, DD = 0x0400, DL = 0x0200, DR = 0x0100;
    constexpr uint16_t L = 0x0020, R = 0x0010;
    constexpr uint16_t CU = 0x0008, CD = 0x0004, CL = 0x0002, CR = 0x0001;
}

constexpr int num_ports = 4;
constexpr float stick_range = 85.0f / 127.0f;   // full deflection of an original stick
constexpr float stick_deadzone = 0.15f;

struct PadState {
    bool connected = false;
    bool rumble_pak = false;
    uint16_t buttons = 0;
    float x = 0.0f, y = 0.0f;
};

static std::mutex input_mutex;
static std::array<PadState, num_ports> pads;
static std::array<std::atomic_bool, num_ports> rumble_requested;
static std::array<SDL_GameController*, num_ports> gamepads{};

static void open_gamepad(int device_index) {
    for (auto& slot : gamepads) {
        if (slot == nullptr) {
            slot = SDL_GameControllerOpen(device_index);
            return;
        }
    }
}

static void close_gamepad(SDL_JoystickID id) {
    for (auto& slot : gamepads) {
        if (slot != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(slot)) == id) {
            SDL_GameControllerClose(slot);
            slot = nullptr;
        }
    }
}

static float axis(SDL_GameController* pad, SDL_GameControllerAxis a) {
    float v = SDL_GameControllerGetAxis(pad, a) / 32767.0f;
    return std::abs(v) < stick_deadzone ? 0.0f : std::clamp(v, -1.0f, 1.0f);
}

static void read_gamepad(SDL_GameController* pad, PadState& s) {
    auto b = [&](SDL_GameControllerButton button, uint16_t mask) {
        if (SDL_GameControllerGetButton(pad, button)) s.buttons |= mask;
    };
    b(SDL_CONTROLLER_BUTTON_A, btn::A);
    b(SDL_CONTROLLER_BUTTON_X, btn::B);
    b(SDL_CONTROLLER_BUTTON_B, btn::B);
    b(SDL_CONTROLLER_BUTTON_START, btn::START);
    b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, btn::L);
    b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, btn::R);
    b(SDL_CONTROLLER_BUTTON_DPAD_UP, btn::DU);
    b(SDL_CONTROLLER_BUTTON_DPAD_DOWN, btn::DD);
    b(SDL_CONTROLLER_BUTTON_DPAD_LEFT, btn::DL);
    b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, btn::DR);
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) s.buttons |= btn::Z;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) s.buttons |= btn::R;
    float cx = axis(pad, SDL_CONTROLLER_AXIS_RIGHTX), cy = axis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
    if (cx < -0.5f) s.buttons |= btn::CL;
    if (cx > 0.5f) s.buttons |= btn::CR;
    if (cy < -0.5f) s.buttons |= btn::CU;
    if (cy > 0.5f) s.buttons |= btn::CD;
    float x = axis(pad, SDL_CONTROLLER_AXIS_LEFTX), y = -axis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (x != 0.0f || y != 0.0f) {
        s.x = x * stick_range;
        s.y = y * stick_range;
    }
    s.rumble_pak |= SDL_GameControllerHasRumble(pad) == SDL_TRUE;
}

// Keyboard on port 1: arrows = stick, X = A, C = B, Z = Z, A/S = L/R, Enter = Start,
// I/K/J/L = C buttons, T/G/F/H = D-pad.
static void read_keyboard(PadState& s) {
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    auto b = [&](SDL_Scancode key, uint16_t mask) { if (k[key]) s.buttons |= mask; };
    b(SDL_SCANCODE_X, btn::A);
    b(SDL_SCANCODE_C, btn::B);
    b(SDL_SCANCODE_Z, btn::Z);
    b(SDL_SCANCODE_A, btn::L);
    b(SDL_SCANCODE_S, btn::R);
    b(SDL_SCANCODE_RETURN, btn::START);
    b(SDL_SCANCODE_I, btn::CU);
    b(SDL_SCANCODE_K, btn::CD);
    b(SDL_SCANCODE_J, btn::CL);
    b(SDL_SCANCODE_L, btn::CR);
    b(SDL_SCANCODE_T, btn::DU);
    b(SDL_SCANCODE_G, btn::DD);
    b(SDL_SCANCODE_F, btn::DL);
    b(SDL_SCANCODE_H, btn::DR);
    float x = float(k[SDL_SCANCODE_RIGHT]) - float(k[SDL_SCANCODE_LEFT]);
    float y = float(k[SDL_SCANCODE_UP]) - float(k[SDL_SCANCODE_DOWN]);
    if (x != 0.0f && y != 0.0f) {
        x *= 0.7071f;
        y *= 0.7071f;
    }
    if (x != 0.0f || y != 0.0f) {
        s.x = x * stick_range;
        s.y = y * stick_range;
    }
}

static void update_input() {
    std::array<PadState, num_ports> next{};
    next[0].connected = true;
    read_keyboard(next[0]);
    for (int i = 0; i < num_ports; i++) {
        if (gamepads[i] == nullptr) {
            continue;
        }
        next[i].connected = true;
        read_gamepad(gamepads[i], next[i]);
        SDL_GameControllerRumble(gamepads[i], rumble_requested[i] ? 0xFFFF : 0, rumble_requested[i] ? 0xFFFF : 0, 100);
    }
    std::lock_guard lock{ input_mutex };
    pads = next;
}

static bool get_input(int port, uint16_t* buttons, float* x, float* y) {
    if (port < 0 || port >= num_ports) {
        return false;
    }
    std::lock_guard lock{ input_mutex };
    if (!pads[port].connected) {
        return false;
    }
    *buttons = pads[port].buttons;
    *x = pads[port].x;
    *y = pads[port].y;
    return true;
}

static void set_rumble(int port, bool on) {
    if (port >= 0 && port < num_ports) {
        rumble_requested[port] = on;
    }
}

static ultramodern::input::connected_device_info_t get_connected_device_info(int port) {
    using namespace ultramodern::input;
    std::lock_guard lock{ input_mutex };
    if (port < 0 || port >= num_ports || !pads[port].connected) {
        return { Device::None, Pak::None };
    }
    return { Device::Controller, pads[port].rumble_pak ? Pak::RumblePak : Pak::None };
}

// ---------------------------------------------------------------------------------------
// Audio: the game's 16-bit stereo samples (channels swapped by the word-swapped RDRAM)
// go through an SDL_AudioStream that resamples to the device rate.

static SDL_AudioDeviceID audio_device = 0;
static SDL_AudioSpec audio_spec{};
static SDL_AudioStream* audio_stream = nullptr;
static uint32_t game_sample_rate = 32000;
static std::mutex audio_mutex;

static void set_frequency(uint32_t freq) {
    std::lock_guard lock{ audio_mutex };
    game_sample_rate = freq;
    if (audio_stream != nullptr) {
        SDL_FreeAudioStream(audio_stream);
    }
    audio_stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, int(freq), audio_spec.format, audio_spec.channels, audio_spec.freq);
}

static size_t output_bytes_per_frame() {
    return audio_spec.channels * (SDL_AUDIO_BITSIZE(audio_spec.format) / 8);
}

static void queue_samples(int16_t* data, size_t sample_count) {
    std::lock_guard lock{ audio_mutex };
    if (audio_stream == nullptr) {
        return;
    }
    // Drop the chunk if more than 150 ms are already queued, so latency can't build up.
    uint32_t queued = SDL_GetQueuedAudioSize(audio_device);
    if (queued > output_bytes_per_frame() * audio_spec.freq * 15 / 100) {
        return;
    }
    static std::vector<int16_t> swapped;
    swapped.resize(sample_count);
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        swapped[i + 0] = data[i + 1];
        swapped[i + 1] = data[i + 0];
    }
    SDL_AudioStreamPut(audio_stream, swapped.data(), int(sample_count * sizeof(int16_t)));
    static std::vector<uint8_t> out;
    out.resize(size_t(SDL_AudioStreamAvailable(audio_stream)));
    int got = SDL_AudioStreamGet(audio_stream, out.data(), int(out.size()));
    if (got > 0) {
        SDL_QueueAudio(audio_device, out.data(), uint32_t(got));
    }
}

static size_t get_frames_remaining() {
    std::lock_guard lock{ audio_mutex };
    uint64_t out_frames = SDL_GetQueuedAudioSize(audio_device) / output_bytes_per_frame();
    uint64_t frames = out_frames * game_sample_rate / audio_spec.freq;
    // Report one video frame less, so the game generates audio slightly ahead and
    // the device doesn't run dry.
    uint64_t one_vi = game_sample_rate / 60;
    return size_t(frames > one_vi ? frames - one_vi : 0);
}

static void init_audio() {
    SDL_AudioSpec desired{};
    desired.freq = 48000;
    desired.format = AUDIO_F32SYS;
    desired.channels = 2;
    desired.samples = 0x100;
    audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &audio_spec, 0);
    if (audio_device == 0) {
        fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
        audio_spec = desired;
        return;
    }
    set_frequency(game_sample_rate);
    SDL_PauseAudioDevice(audio_device, 0);
}

// ---------------------------------------------------------------------------------------
// Window and events

static ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        message_box(SDL_GetError());
        std::exit(EXIT_FAILURE);
    }
    return nullptr;
}

static ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    uint32_t flags = SDL_WINDOW_RESIZABLE;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#elif !defined(_WIN32)
    flags |= SDL_WINDOW_VULKAN;
#endif
    window = SDL_CreateWindow("Mario Tennis Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960, flags);
    if (window == nullptr) {
        message_box(SDL_GetError());
        std::exit(EXIT_FAILURE);
    }
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    SDL_GetWindowWMInfo(window, &wm_info);
#if defined(_WIN32)
    return ultramodern::renderer::WindowHandle{ wm_info.info.win.window, GetCurrentThreadId() };
#elif defined(__linux__)
    return window;
#else
    static_assert(false, "Unsupported platform");
#endif
}

static void toggle_fullscreen() {
    auto config = ultramodern::renderer::get_graphics_config();
    config.wm_option = config.wm_option == ultramodern::renderer::WindowMode::Fullscreen
        ? ultramodern::renderer::WindowMode::Windowed
        : ultramodern::renderer::WindowMode::Fullscreen;
    ultramodern::renderer::set_graphics_config(config);
}

static void update_gfx(void*) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                ultramodern::quit();
                break;
            case SDL_CONTROLLERDEVICEADDED:
                open_gamepad(event.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                close_gamepad(event.cdevice.which);
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.scancode == SDL_SCANCODE_F11 ||
                    (event.key.keysym.scancode == SDL_SCANCODE_RETURN && (event.key.keysym.mod & KMOD_ALT))) {
                    toggle_fullscreen();
                }
                break;
        }
    }
    update_input();
}

// ---------------------------------------------------------------------------------------

// The VI thread only sets a placeholder video mode while the game hasn't started; once it
// has, it uses the game's mode. Start the game from the VI callback after a couple of VIs,
// so there is a mode before the game calls osViSetMode (the runtime's menu-less start).
static std::u8string pending_game_id;

static void vi_callback() {
    static int vis = 0;
    if (!pending_game_id.empty() && ++vis >= 2) {
        recomp::start_game(pending_game_id);
        pending_game_id.clear();
    }
}

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    if (task->t.type == M_AUDTASK) {
        return aspMain;
    }
    fprintf(stderr, "Unknown RSP task type: %u\n", (unsigned)task->t.type);
    return nullptr;
}

// Hash of a ROM file in .z64 byte order (as the runtime computes it), or 0 if unreadable.
static uint64_t rom_file_hash(const std::filesystem::path& path) {
    std::ifstream file{ path, std::ios::binary };
    std::vector<uint8_t> data{ std::istreambuf_iterator<char>(file), {} };
    if (data.size() < 4) {
        return 0;
    }
    data.resize((data.size() + 3) & ~size_t(3));
    size_t swap = data[0] == 0x80 ? 0 : data[0] == 0x37 ? 1 : data[0] == 0x40 ? 3 : 0;
    if (swap != 0) {
        for (size_t i = 0; i < data.size(); i += 4) {
            uint8_t w[4] = { data[i], data[i + 1], data[i + 2], data[i + 3] };
            for (size_t j = 0; j < 4; j++) {
                data[i + (j ^ swap)] = w[j];
            }
        }
    }
    return XXH3_64bits(data.data(), data.size());
}

static std::filesystem::path config_folder() {
    char* pref = SDL_GetPrefPath("mariotennis-recomp", "MarioTennisRecompiled");
    std::filesystem::path path = pref != nullptr ? std::filesystem::u8path(pref) : std::filesystem::current_path();
    SDL_free(pref);
    return path;
}

int main(int argc, char** argv) {
    mt_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    recomp::Version version{};
    recomp::Version::from_string("0.1.0", version);

    SDL_InitSubSystem(SDL_INIT_AUDIO);
    init_audio();

    recomp::register_config_path(config_folder());

    recomp::GameEntry game{
        .rom_hash = MT_ROM_HASH,
        .internal_name = "MarioTennis",
        .game_id = game_id,
        .mod_game_id = "",                // no mod support
        .save_type = recomp::SaveType::Eep16k,
        .is_enabled = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
    };
    recomp::register_game(game);
    mt_register_overlays();

    // Use the ROM given on the command line (stored for later runs), else the stored one.
    std::u8string id = game_id;
    if (argc > 1) {
        std::filesystem::path rom_path = std::filesystem::u8path(argv[1]);
        recomp::RomValidationError result = recomp::select_rom(rom_path, id);
        if (result != recomp::RomValidationError::Good) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                "The ROM was not accepted (error %d).\n"
                "This port needs Mario Tennis (USA), in .z64, .n64 or .v64 format.\n"
                "ROM hash: 0x%016llX",
                (int)result, (unsigned long long)rom_file_hash(rom_path));
            message_box(msg);
            return EXIT_FAILURE;
        }
    }
    recomp::check_all_stored_roms();
    if (!recomp::is_rom_valid(id)) {
        message_box("No ROM yet. Run the program once with the path to your Mario Tennis (USA) ROM,\n"
                    "or drop the ROM file onto the executable.");
        return EXIT_FAILURE;
    }

    ultramodern::renderer::GraphicsConfig graphics{};
    graphics.developer_mode = false;
    graphics.res_option = ultramodern::renderer::Resolution::Auto;
    graphics.wm_option = ultramodern::renderer::WindowMode::Windowed;
    graphics.hr_option = ultramodern::renderer::HUDRatioMode::Original;
    graphics.api_option = ultramodern::renderer::GraphicsApi::Auto;
    graphics.ar_option = ultramodern::renderer::AspectRatio::Original;
    graphics.msaa_option = ultramodern::renderer::Antialiasing::None;
    graphics.rr_option = ultramodern::renderer::RefreshRate::Original;
    graphics.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
    graphics.rr_manual_value = 60;
    graphics.ds_option = 1;
    ultramodern::renderer::set_graphics_config(graphics);

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };
    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = mt::renderer::create_render_context,
    };
    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };
    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };
    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = nullptr,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };
    ultramodern::events::callbacks_t events_callbacks{
        .vi_callback = vi_callback,
    };
    ultramodern::error_handling::callbacks_t error_callbacks{
        .message_box = message_box,
    };
    ultramodern::threads::callbacks_t threads_callbacks{};

    pending_game_id = id;
    recomp::start(version, {}, rsp_callbacks, renderer_callbacks, audio_callbacks, input_callbacks,
                  gfx_callbacks, events_callbacks, error_callbacks, threads_callbacks);
    return EXIT_SUCCESS;
}
