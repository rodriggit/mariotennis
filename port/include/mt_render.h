// RT64 renderer for the runtime (ultramodern::renderer::RendererContext).
#ifndef MT_RENDER_H
#define MT_RENDER_H

#include <memory>

#include "ultramodern/renderer_context.hpp"

// Factor of the game's last osViSetXScale (port/src/mt_hooks.cpp).
extern "C" float mt_vi_x_scale_factor(void);

namespace RT64 {
    struct Application;
}

namespace mt::renderer {
    // How 2D images (menus, scoreboards, title screens) are drawn; from config.ini, set
    // before the renderer starts.
    enum class Upscale2D { Original, ScaledOnly, All };
    void set_2d_options(bool linear_filter, Upscale2D upscale);

    class RT64Context final : public ultramodern::renderer::RendererContext {
    public:
        RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
        ~RT64Context() override;

        bool valid() override { return static_cast<bool>(app); }
        bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                           const ultramodern::renderer::GraphicsConfig& new_config) override;
        void enable_instant_present() override;
        void send_dl(const OSTask* task) override;
        void update_screen() override;
        void shutdown() override;
        uint32_t get_display_framerate() const override;
        float get_resolution_scale() const override;

    private:
        std::unique_ptr<RT64::Application> app;
    };

    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
}

#endif
