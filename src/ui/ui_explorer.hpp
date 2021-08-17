#pragma once

#include <switch.h>

#include "render.hpp"
#include "ui/ui_common.hpp"

namespace sw::ui {

class Explorer: public Widget {
    public:
        Explorer(Renderer &renderer, Context &context);
        virtual ~Explorer();

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;

        virtual void render() override;

        static constexpr inline std::string_view path_from_entry_name(std::string_view name) {
            return name.substr(name.find("##")+2);
        }

        static constexpr inline std::string_view filename_from_entry_name(std::string_view name) {
            return name.substr(0, name.find("##"));
        }

    public:
        Context &context;

        Renderer::Texture file_texture, folder_texture,
            recent_texture, sd_texture, usb_texture, network_texture;

        bool is_focused = false;

        fs::Path path;
        fs::Path selection;

        std::vector<fs::Node> entries;
        std::size_t cur_focused_entry = -1;

        bool is_initial_scan     = true;
        bool need_directory_scan = true;
        bool want_focus_reset    = false;
};

} // namespace sw::ui
