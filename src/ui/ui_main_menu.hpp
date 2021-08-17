// Copyright (c) 2024 averne <averne381@gmail.com>
//
// This file is part of SwitchWave.
//
// SwitchWave is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// SwitchWave is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SwitchWave.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <switch.h>
#include <imgui.h>

#include "context.hpp"
#include "fs/fs_common.hpp"
#include "ui/ui_common.hpp"
#include "ui/ui_explorer.hpp"

namespace sw::ui {

class MediaExplorer final: public Widget {
    public:
        MediaExplorer(Renderer &renderer, Context &context);
        virtual ~MediaExplorer() override;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;

        virtual void render() override;

    private:
        void metadata_thread_fn(std::stop_token token);

    public:
        bool is_displayed = false;

    private:
        struct MediaMetadata {
            const char *container_name = nullptr;
            std::uint32_t num_streams, num_vstreams, num_astreams, num_sstreams;
            std::int64_t duration;
            const char *video_codec_name, *video_profile_name;
            int video_width, video_height;
            double video_framerate;
            const char *video_pix_format;
            const char *audio_codec_name, *audio_profile_name;
            int num_audio_channels;
            int audio_sample_rate;
            const char *audio_sample_format;
        };

    private:
        Context &context;
        Explorer explorer;

        std::jthread metadata_thread;
        std::mutex metadata_query_mutex;
        std::condition_variable metadata_query_condvar;
        fs::Node      *metadata_query_node   = nullptr;
        MediaMetadata *metadata_query_target = nullptr;

        std::vector<std::unique_ptr<MediaMetadata>> media_metadata;
};

class ConfigEditor final: public Widget {
    public:
        ConfigEditor(Renderer &renderer, Context &context);
        virtual ~ConfigEditor();

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;

        virtual void render() override;

    private:
        void install_swkbd_callbacks(SwkbdInline *swkbd);
        void reset_swkbd_state(SwkbdInline *swkbd);

        int reset_text();
        int save_text();

    public:
        bool is_displayed = false;

    private:
        static inline int cur_config_file = 0;
        static inline std::array config_files = {
            fs::Path(Context::AppDirectory) / "mpv.conf",
            fs::Path(Context::AppDirectory) / Context::SettingsFilename,
        };

        Context &context;

        constexpr static std::string_view swkbd_string_reset = "      ";
        constexpr static std::size_t      swkbd_cursor_reset = 3;
        static_assert(swkbd_string_reset.length() % 2 == 0 && swkbd_string_reset.length() / 2 == swkbd_cursor_reset);

        std::string      config_text;
        int              cursor_pos = 0;

        std::string_view config_path;

        bool want_cursor_update  = false;
        bool has_swkbd_visible   = false;
        bool is_in_error         = false;
        bool has_unsaved_changes = false;

        static inline ConfigEditor *s_this;
};

class SettingsEditor final: public Widget {
    public:
        SettingsEditor(Renderer &renderer, Context &context);
        virtual ~SettingsEditor();

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;

        virtual void render() override;

    private:
        void install_swkbd_callbacks(SwkbdInline *swkbd);
        void reset_swkbd_state(SwkbdInline *swkbd, const utils::StaticString32 &str, SwkbdType type = SwkbdType_Normal);

    public:
        bool is_displayed = false;

    private:
        Context &context;

        Renderer::Texture delete_texture;

        SwkbdAppearArg appear_args;
        utils::StaticString32 *cur_edited_string;

        int cursor_pos          = 0;
        bool want_cursor_update = false;
        bool has_swkbd_visible  = false;
        ImGuiID cur_input_id    = 0;

        static inline SettingsEditor *s_this;
};

class InfoHelp final: public Widget {
    public:
        InfoHelp(Renderer &renderer): Widget(renderer) { }
        virtual ~InfoHelp() = default;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;

        virtual void render() override;

    public:
        bool is_displayed = false;
};

class MainMenuGui final: public Widget {
    public:
        MainMenuGui(Renderer &renderer, Context &context);
        virtual ~MainMenuGui() = default;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;

        virtual void render() override;

    private:
        enum class Tab {
            Explorer,
            ConfigEdit,
            Settings,
            InfoHelp,
        };

    private:
        Context &context;

        Tab cur_tab = Tab::Explorer;

        MediaExplorer   explorer;
        ConfigEditor    editor;
        SettingsEditor  settings;
        InfoHelp        infohelp;

        Renderer::Texture explorer_texture, edit_texture,
            info_texture, settings_texture, exit_texture;
};

} // namespace sw::ui
