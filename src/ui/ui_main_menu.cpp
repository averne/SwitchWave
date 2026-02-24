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

#include <cstring>
#include <array>
#include <algorithm>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_nx.h>
#include <imgui_deko3d.h>

extern "C" {
#include <libavcodec/codec_desc.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavcodec/version.h>
#include <libavformat/version.h>
#include <libavutil/version.h>
#include <libswscale/version.h>
#include <libavfilter/version.h>
#include <libswresample/version.h>
}

#include <ass/ass.h>

#include "utils.hpp"
#include "fs/fs_recent.hpp"
#include "fs/fs_http.hpp"

#include "ui/ui_main_menu.hpp"

namespace sw::ui {

using namespace std::chrono_literals;

extern "C" const char mpv_version[];

namespace {

std::string_view app_version_str = "v" APP_VERSION,
    app_build_date_str = __DATE__ " " __TIME__;

#define FORMAT_TIME(s) ((s)/60/60%99), ((s)/60%60), ((s)%60)

} // namespace

MainMenuGui::MainMenuGui(Renderer &renderer, Context &context):
        Widget(renderer), context(context),
        explorer(renderer, context), editor(renderer, context), settings(renderer, context), infohelp(renderer) {
    // Enable nav highlight when booting
    auto &imctx   = *ImGui::GetCurrentContext();
    auto &imstyle = ImGui::GetStyle();

    imctx.NavDisableHighlight  = false;
    imctx.NavDisableMouseHover = true;
    imstyle.Alpha              = 0.85f;
}

bool MainMenuGui::update_state(PadState &pad, HidTouchScreenState &touch) {
    auto down = padGetButtonsDown(&pad);
    if ((down & HidNpadButton_Plus) && !ImGui::nx::isSwkbdVisible()) {
        this->context.want_quit = true;
        return false;
    }

    if (!this->context.cur_file.empty())
        return false;

    this->explorer.update_state(pad, touch);
    this->editor  .update_state(pad, touch);
    this->settings.update_state(pad, touch);
    this->infohelp.update_state(pad, touch);

    return true;
}

void MainMenuGui::render() {
    auto &imctx   = *ImGui::GetCurrentContext();
    auto &imstyle = ImGui::GetStyle();

    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        SW_SCOPEGUARD([] { ImGui::PopStyleVar(); });

        ImGui::Begin("##mainwindow", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetWindowPos (this->screen_rel_vec<ImVec2>(0, 0));
        ImGui::SetWindowSize(this->screen_rel_vec<ImVec2>(1, 1));
        ImGui::SetWindowFontScale(this->scale_factor());
        SW_SCOPEGUARD([] { ImGui::End(); });

        auto bar_min = ImGui::GetCursorScreenPos() + this->screen_rel_vec<ImVec2>(0.3, 0);
        auto bar_max = bar_min + ImVec2(this->screen_rel_width(0.7), imctx.FontSize + imstyle.FramePadding.y * 2);

        ImGui::BeginTabBar("##maintabbar", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton |
            ImGuiTabBarFlags_NoTabListScrollingButtons | ImGuiTabBarFlags_NoTooltip);
        SW_SCOPEGUARD([] { ImGui::EndTabBar(); });

        ImGui::GetCurrentTabBar()->BarRect = ImRect(bar_min, bar_max);

        // ImGui::SetCursorPosX(this->screen_rel_width(0.2));
        // ImGui::Dummy(this->screen_rel_vec<ImVec2>(0.2, 0));

        if (ImGui::BeginTabItem("Explorer", nullptr, ImGuiTabItemFlags_NoReorder)) {
            SW_SCOPEGUARD([] { ImGui::EndTabItem(); });
            this->cur_tab = Tab::Explorer;
        }

        if (ImGui::BeginTabItem("Editor",     nullptr, ImGuiTabItemFlags_NoReorder)) {
            SW_SCOPEGUARD([] { ImGui::EndTabItem(); });
            this->cur_tab = Tab::ConfigEdit;
        }

        if (ImGui::BeginTabItem("Settings", nullptr, ImGuiTabItemFlags_NoReorder)) {
            SW_SCOPEGUARD([] { ImGui::EndTabItem(); });
            this->cur_tab = Tab::Settings;
        }

        if (ImGui::BeginTabItem("Info & Help", nullptr, ImGuiTabItemFlags_NoReorder)) {
            SW_SCOPEGUARD([] { ImGui::EndTabItem(); });
            this->cur_tab = Tab::InfoHelp;
        }

        if (ImGui::TabItemButton("Exit", ImGuiTabItemFlags_NoReorder))
            this->context.want_quit = true;

        this->explorer.is_displayed = this->editor.is_displayed =
            this->settings.is_displayed = this->infohelp.is_displayed = false;

        switch (this->cur_tab) {
            default:
            case Tab::Explorer:
                this->explorer.is_displayed = true;
                this->explorer.render();
                break;
            case Tab::ConfigEdit:
                this->editor.is_displayed   = true;
                this->editor.render();
                break;
            case Tab::Settings:
                this->settings.is_displayed = true;
                this->settings.render();
                break;
            case Tab::InfoHelp:
                this->infohelp.is_displayed = true;
                this->infohelp.render();
                break;
        }
    }

    if (context.last_error) {
        static const char *error_type_description[] = {
            [Context::ErrorType::Io]         = "I/O error",
            [Context::ErrorType::Network]    = "Network error",
            [Context::ErrorType::Mpv]        = "Player error",
            [Context::ErrorType::LibAv]      = "LibAV error",
            [Context::ErrorType::AppletMode] = "Applet mode",
        };

        char buf[AV_ERROR_MAX_STRING_SIZE];
        const char *error_desc = nullptr, *error_type_desc = error_type_description[context.last_error_type];
        switch (context.last_error_type) {
            case Context::ErrorType::Io:
            case Context::ErrorType::Network:
            default:
                error_desc = std::strerror(context.last_error);
                break;
            case Context::ErrorType::Mpv:
                error_desc = mpv_error_string(context.last_error);
                break;
            case Context::ErrorType::LibAv:
                error_desc = av_make_error_string(buf, sizeof(buf), this->context.last_error);
                break;
            case Context::ErrorType::AppletMode:
                error_desc = "Starting in applet mode.\nThis may lead to stability issues.";
                break;
        }

        ImGui::OpenPopup(error_type_desc);
        if (ImGui::BeginPopupModal(error_type_desc, nullptr,
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            SW_SCOPEGUARD([] { ImGui::EndPopup(); });

            ImGui::SetWindowFontScale(this->scale_factor());

            ImGui::SetWindowSize(this->screen_rel_vec<ImVec2>(0.5, 0.2));
            ImGui::SetWindowPos(this->screen_rel_vec<ImVec2>((1.0 - 0.5) / 2, (1.0 - 0.2) / 2));

            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImColor(0.9f, 0.2f, 0.1f).Value);
                SW_SCOPEGUARD([] { ImGui::PopStyleColor(); });

                ImGui::TextWrapped("%s (%d)", error_desc, context.last_error);
            }

            auto ok_string = "Ok";
            auto size = ImGui::GetContentRegionAvail() - ImGui::CalcTextSize(ok_string) -
                imstyle.ItemInnerSpacing - imstyle.ItemSpacing;

            ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(size.x / 2, size.y));

            if (ImGui::Button(ok_string))
                context.last_error = 0;
            ImGui::SetNavID(ImGui::GetItemID(), ImGuiNavLayer_Main, 0, ImRect());
        }
    }
}

void MediaExplorer::metadata_thread_fn(std::stop_token token) {
    while (!token.stop_requested()) {
        {
            auto lk = std::unique_lock(this->metadata_query_mutex);
            if (this->metadata_query_condvar.wait_for(lk, 100ms) == std::cv_status::timeout)
                continue;
        }

        auto *entry = this->metadata_query_node;
        MediaMetadata media_info = {};

        if (entry) {
            auto entry_path = Explorer::path_from_entry_name(entry->name);

            // For HTTP filesystems, pass the URL directly to avformat (ffmpeg supports HTTP natively)
            std::string path;
            AVDictionary *format_opts = nullptr;
            SW_SCOPEGUARD([&format_opts] { av_dict_free(&format_opts); });
            if (auto *fs_ptr = this->context.get_filesystem(fs::Path::mountpoint(entry_path));
                    fs_ptr && fs_ptr->type == fs::Filesystem::Type::Network) {
                auto *net_fs = static_cast<const fs::NetworkFilesystem *>(fs_ptr);
                if (net_fs->protocol == fs::NetworkFilesystem::Protocol::Http ||
                        net_fs->protocol == fs::NetworkFilesystem::Protocol::Https) {
                    path = static_cast<const fs::HttpFs *>(net_fs)->make_url(entry_path);
                    av_dict_set(&format_opts, "auth_type", "basic", 0);
                    av_dict_set(&format_opts, "user_agent", "SwitchWave/1.0", 0);
                }
            }

            // Add explicit protocol prefix, otherwise ffmpeg confuses the mountpoint for a protocol
            if (path.empty())
                path = std::string("file:") + entry_path.data();

            auto *avformat_ctx = avformat_alloc_context();
            SW_SCOPEGUARD([&avformat_ctx] { avformat_close_input(&avformat_ctx); });
            if (!avformat_ctx)
                goto end;

            if (auto rc = avformat_open_input(&avformat_ctx, path.c_str(), nullptr, &format_opts); rc) {
                char buf[AV_ERROR_MAX_STRING_SIZE];
                std::printf("Failed to open input %s: %s\n", path.c_str(), av_make_error_string(buf, sizeof(buf), rc));
                this->context.set_error(rc, Context::ErrorType::LibAv);
                goto end;
            }

            media_info.container_name = avformat_ctx->iformat->long_name;

            if (auto rc = avformat_find_stream_info(avformat_ctx, nullptr); rc) {
                char buf[AV_ERROR_MAX_STRING_SIZE];
                std::printf("Failed to match format for %s: %s\n", path.c_str(), av_make_error_string(buf, sizeof(buf), rc));
                this->context.set_error(rc, Context::ErrorType::LibAv);
                goto end;
            }

            media_info.duration    = (avformat_ctx->duration > 0) ? avformat_ctx->duration / AV_TIME_BASE : 0;
            media_info.num_streams = avformat_ctx->nb_streams;

            for (std::size_t i = 0; i < avformat_ctx->nb_streams; ++i) {
                auto *s    = avformat_ctx->streams[i];
                auto *desc = avcodec_descriptor_get(s->codecpar->codec_id);

                switch (s->codecpar->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        if (!media_info.video_codec_name) {
                            media_info.video_codec_name    = desc->long_name;
                            media_info.video_profile_name  = desc->profiles ? desc->profiles[0].name : nullptr;
                            media_info.video_width         = s->codecpar->width;
                            media_info.video_height        = s->codecpar->height;
                            media_info.video_framerate     = av_q2d(s->r_frame_rate);
                            media_info.video_pix_format    = av_get_pix_fmt_name(AVPixelFormat(s->codecpar->format));
                        }
                        ++media_info.num_vstreams;
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        if (!media_info.audio_codec_name) {
                            media_info.audio_codec_name    = desc->long_name;
                            media_info.audio_profile_name  = desc->profiles ? desc->profiles[0].name : nullptr;
                            media_info.num_audio_channels  = s->codecpar->ch_layout.nb_channels;
                            media_info.audio_sample_rate   = s->codecpar->sample_rate;
                            media_info.audio_sample_format = av_get_sample_fmt_name(AVSampleFormat(s->codecpar->format));
                        }
                        ++media_info.num_astreams;
                        break;
                    case AVMEDIA_TYPE_SUBTITLE:
                        ++media_info.num_sstreams;
                        break;
                    default:
                        break;
                }
            }

end:
            if (this->metadata_query_node == entry && this->metadata_query_target)
                *this->metadata_query_target = media_info;
        }

        this->metadata_query_node   = nullptr;
        this->metadata_query_target = nullptr;
    }
}

MediaExplorer::MediaExplorer(Renderer &renderer, Context &context):
        Widget(renderer), context(context), explorer(renderer, context) {
    this->metadata_thread = std::jthread(&MediaExplorer::metadata_thread_fn, this);
}

MediaExplorer::~MediaExplorer() {
    this->metadata_thread.request_stop();
    this->metadata_query_condvar.notify_all();
    this->metadata_thread.join();
}

bool MediaExplorer::update_state(PadState &pad, HidTouchScreenState &touch) {
    bool scanning = this->explorer.need_directory_scan;
    if (scanning) {
        this->metadata_query_node   = nullptr;
        this->metadata_query_target = nullptr;
    }

    this->explorer.update_state(pad, touch);

    if (scanning) {
        this->context.cur_path = this->explorer.path.base();
        this->media_metadata.clear();
        this->media_metadata.resize(this->explorer.entries.size());
    }

    return true;
}

void MediaExplorer::render() {
    if (!ImGui::BeginTable("##explorerbl", 2))
        return;

    SW_SCOPEGUARD([] { ImGui::EndTable (); });

    ImGui::TableSetupColumn("##explorercol", ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.625));
    ImGui::TableSetupColumn("##desccol",     ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.3));

    ImGui::TableNextColumn();

    this->explorer.render();
    if (!this->explorer.selection.empty())
        this->context.cur_file = std::move(this->explorer.selection.base());

    ImGui::TableNextColumn();
    ImGui::SeparatorText("Description");

    auto ent_idx = this->explorer.cur_focused_entry;
    if (ent_idx == -1ul)
        return;

    auto &entry = this->explorer.entries[ent_idx];

    ImGui::NewLine();

    auto fname = Explorer::filename_from_entry_name(entry.name);
    ImGui::TextWrapped("Name: %.*s", int(fname.length()), fname.data());

    if (entry.type == fs::Node::Type::Directory)
        return;

    auto [size, suffix] = utils::to_human_size(entry.size);
    ImGui::Text("Size: %.2f%s", size, suffix.data());

    ImGui::NewLine();

    auto &metadata = this->media_metadata[ent_idx];
    if (!metadata) {
        bool ret = ImGui::Button("Press \ue0e6/\ue0e7 to show metadata", ImVec2(-1, 0));
        if (ret || ImGui::IsKeyPressed(ImGuiKey_GamepadL2) || ImGui::IsKeyPressed(ImGuiKey_GamepadR2)) {
            metadata = std::make_unique<MediaMetadata>();

            this->metadata_query_node   = &entry;
            this->metadata_query_target = metadata.get();

            auto lk = std::unique_lock(this->metadata_query_mutex);
            this->metadata_query_condvar.notify_one();
        }
        return;
    }

    if (!metadata->container_name)
        return;

    auto bullet_wrapped = [](std::string_view fmt, auto &&...args) {
        ImGui::Bullet();
        ImGui::TextWrapped(fmt.data(), std::forward<decltype(args)>(args)...);
    };

    ImGui::SetWindowFontScale(0.7 * this->scale_factor());
    SW_SCOPEGUARD([this] { ImGui::SetWindowFontScale(this->scale_factor()); });

    auto &i = *metadata;
    ImGui::TextWrapped("Format: %s (%d stream%s)", i.container_name,
        i.num_streams, i.num_streams != 1 ? "s" : "");
    ImGui::TextWrapped("Duration: %ld:%02ld:%02ld", FORMAT_TIME(i.duration));

    ImGui::SeparatorText("Video");
    bullet_wrapped("%d stream%s", i.num_vstreams, i.num_vstreams != 1 ? "s" : "");
    bullet_wrapped("Codec: %s", i.video_codec_name);
    if (i.video_profile_name) bullet_wrapped("Profile: %s", i.video_profile_name);
    bullet_wrapped("Dimensions: %dx%d", i.video_width, i.video_height);
    bullet_wrapped("Framerate: %.3fHz", i.video_framerate);
    bullet_wrapped("Pixel format: %s", i.video_pix_format);

    ImGui::SeparatorText("Audio");
    bullet_wrapped("%d stream%s", i.num_astreams, i.num_astreams != 1 ? "s" : "");
    bullet_wrapped("Codec: %s (%d channels)", i.audio_codec_name, i.num_audio_channels);
    if (i.audio_profile_name) bullet_wrapped("Profile: %s", i.audio_profile_name);
    bullet_wrapped("Samplerate: %dHz", i.audio_sample_rate);
    bullet_wrapped("Sample format: %s", i.audio_sample_format);

    ImGui::SeparatorText("Subtitles");
    bullet_wrapped("%d stream%s", i.num_sstreams, i.num_sstreams != 1 ? "s" : "");
}

ConfigEditor::ConfigEditor(Renderer &renderer, Context &context): Widget(renderer), context(context) {
    ConfigEditor::s_this = this;

    this->config_path = ConfigEditor::config_files[ConfigEditor::cur_config_file].base();

    this->config_text.reserve(0x400000); // 4MB
    this->is_in_error = !!this->reset_text();
}

ConfigEditor::~ConfigEditor() {
    swkbdInlineSetChangedStringCallback(ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetMovedCursorCallback  (ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetDecidedEnterCallback (ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetDecidedCancelCallback(ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetInputText(ImGui::nx::getSwkbd(), "");
    swkbdInlineSetCursorPos(ImGui::nx::getSwkbd(), 0);
}

void ConfigEditor::install_swkbd_callbacks(SwkbdInline *swkbd) {
    swkbdInlineSetChangedStringCallback(ImGui::nx::getSwkbd(), +[](const char *str, SwkbdChangedStringArg *arg) {
        constexpr static auto reset_length = ConfigEditor::swkbd_string_reset.length();
        if (arg->stringLen == reset_length)
            return;

        SW_SCOPEGUARD([] { s_this->reset_swkbd_state(ImGui::nx::getSwkbd()); });

        int delta = arg->stringLen - reset_length;
        if ((s_this->config_text.length() + delta + 1 > s_this->config_text.capacity()) ||
                (delta < 0 && s_this->cursor_pos == 0))
            return;

        if (delta > 0)
            s_this->config_text.resize(s_this->config_text.length() + delta);

        std::memmove(s_this->config_text.data() + s_this->cursor_pos + delta, s_this->config_text.data() + s_this->cursor_pos,
            s_this->config_text.length() - s_this->cursor_pos + 1);
        if (delta > 0)
            std::memcpy(s_this->config_text.data() + s_this->cursor_pos, str + ConfigEditor::swkbd_cursor_reset, delta);

        if (delta < 0)
            s_this->config_text.resize(s_this->config_text.length() + delta);

        s_this->cursor_pos         += delta;
        s_this->want_cursor_update  = true;

        s_this->has_unsaved_changes = true;
    });

    swkbdInlineSetMovedCursorCallback(ImGui::nx::getSwkbd(), +[](const char *str, SwkbdMovedCursorArg *arg) {
        if (arg->cursorPos == ConfigEditor::swkbd_cursor_reset)
            return;

        SW_SCOPEGUARD([] { s_this->reset_swkbd_state(ImGui::nx::getSwkbd()); });

        s_this->cursor_pos        += arg->cursorPos - ConfigEditor::swkbd_cursor_reset;
        s_this->cursor_pos         = std::max(0, s_this->cursor_pos);
        s_this->want_cursor_update = true;
    });

    swkbdInlineSetDecidedEnterCallback(ImGui::nx::getSwkbd(), +[](const char *str, SwkbdDecidedEnterArg *arg) {
        // Exit input box
        ImGui::GetCurrentContext()->ActiveId = 0;
    });

    swkbdInlineSetDecidedCancelCallback(ImGui::nx::getSwkbd(), +[]() {
        // Exit input box
        ImGui::GetCurrentContext()->ActiveId = 0;
    });
}

void ConfigEditor::reset_swkbd_state(SwkbdInline *swkbd) {
    swkbdInlineSetKeytopBgAlpha(swkbd, 1.0f);
    swkbdInlineSetFooterBgAlpha(swkbd, 1.0f);

    swkbdInlineSetInputText(swkbd, ConfigEditor::swkbd_string_reset.data());
    swkbdInlineSetCursorPos(swkbd, ConfigEditor::swkbd_cursor_reset);
};

bool ConfigEditor::update_state(PadState &pad, HidTouchScreenState &touch) {
    if (!this->is_displayed && this->has_swkbd_visible) {
        ImGui::nx::hideSwkbd();
        this->has_swkbd_visible = false;
    }

    return true;
}

void ConfigEditor::render() {
    {
        ImGui::PushItemWidth(this->screen_rel_width(0.4));
        SW_SCOPEGUARD([] { ImGui::PopItemWidth(); });

        if (ImGui::Combo("##configfile", &ConfigEditor::cur_config_file,
                +[](void* data, int idx, const char **out_text) {
                    *out_text = ConfigEditor::config_files[idx].c_str();
                    return true;
                }, nullptr, ConfigEditor::config_files.size())) {
            this->config_path = ConfigEditor::config_files[ConfigEditor::cur_config_file].base();

            if (this->reset_text()) {
                this->is_in_error         = true;
            } else {
                this->cursor_pos          = 0;
                this->want_cursor_update  = true;
                this->has_unsaved_changes = false;
            }
        }
    }

    if (this->has_unsaved_changes) {
        auto col = (ImGui::nx::getCurrentTheme() == ColorSetId_Dark) ?
            ImColor(0xf2, 0x77, 0x7a) : ImColor(0xbb, 0x11, 0x14);
        ImGui::SameLine(0, this->screen_rel_width(0.08));
        ImGui::TextColored(col, "You have unsaved changes");
    }

    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(this->screen_rel_width(0.88), ImGui::GetCursorScreenPos().y));

    if (ImGui::Button("Save")) {
        if (this->save_text()) {
            this->is_in_error         = true;
        } else {
            this->has_unsaved_changes = false;
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        if (this->reset_text()) {
            this->is_in_error         = true;
        } else {
            this->cursor_pos          = 0;
            this->want_cursor_update  = true;
            this->has_unsaved_changes = false;
        }
    }

    if (this->is_in_error) {
        ImGui::SetCursorPos(this->screen_rel_vec<ImVec2>(0.4, 0.4));

        ImGui::BeginGroup();
        SW_SCOPEGUARD([] { ImGui::EndGroup(); });

        ImGui::TextColored(ImColor(200, 50, 10), "Failed to load configuration file");
        if (ImGui::Button("Create file")) {
            auto *fp = std::fopen(this->config_path.data(), "w");
            this->is_in_error = fp == nullptr;
            std::fclose(fp);

            if (!this->is_in_error) {
                if (this->reset_text()) {
                    this->is_in_error         = true;
                } else {
                    this->cursor_pos          = 0;
                    this->want_cursor_update  = true;
                    this->has_unsaved_changes = false;
                }
            }
        }

        return;
    }

    auto is_swkbd_visible = ImGui::nx::isSwkbdVisible();
    auto reserved_height = (is_swkbd_visible && ImGui::nx::getSwkbd()->state > SwkbdState_Inactive) ? this->screen_rel_height(0.55) : 1;
    ImGui::InputTextMultiline("##source", this->config_text.data(), this->config_text.capacity(), ImVec2{-1, -reserved_height},
        ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_CallbackAlways,
        +[](ImGuiInputTextCallbackData *data) -> int {
            auto *self = static_cast<ConfigEditor *>(data->UserData);

            if (self->want_cursor_update) {
                data->CursorPos = self->cursor_pos;
                self->want_cursor_update = false;
            }

            self->cursor_pos = data->CursorPos;
            data->ClearSelection();

            return 0;
    }, this);

    if (ImGui::IsItemActive()) {
        if (auto *input_state = ImGui::GetInputTextState(ImGui::GetActiveID()); input_state && !is_swkbd_visible) {
            this->install_swkbd_callbacks(ImGui::nx::getSwkbd());
            this->reset_swkbd_state(ImGui::nx::getSwkbd());

            ImGui::nx::showSwkbd();
            this->has_swkbd_visible = true;

            // Follow the cursor for the first frame, as the input box will be resized when the keyboard spawns
            input_state->CursorFollow = true;
        }
    } else {
        if (is_swkbd_visible) {
            ImGui::nx::hideSwkbd();
            this->has_swkbd_visible = false;
        }
    }
}

int ConfigEditor::reset_text() {
    auto rc = utils::read_whole_file(this->config_text, this->config_path.data(), "r");
    if (!rc)
        return 0;

    switch (rc = errno) {
        case ENOENT:
            rc = fsdevCreateFile(this->config_path.data(), 0, 0);
            if (R_FAILED(rc))
                this->context.set_error(rc);
            return rc;
        default:
            this->context.set_error(errno);
            return rc;
    }
}

int ConfigEditor::save_text() {
    FILE *fp = std::fopen(this->config_path.data(), "w");
    if (!fp) {
        std::printf("Failed to open %s\n", this->config_path.data());
        this->context.set_error(errno);
        return -1;
    }
    SW_SCOPEGUARD([fp] { std::fclose(fp); });

    auto len = this->config_text.length();

    if (auto written = std::fwrite(this->config_text.data(), 1, len, fp); written != len) {
        std::printf("Failed to write to %s: wrote %ld, expected %ld\n", this->config_path.data(), written, len);
        this->context.set_error(errno);
        return -1;
    }

    return 0;
}

SettingsEditor::SettingsEditor(Renderer &renderer, Context &context): Widget(renderer), context(context) {
    SettingsEditor::s_this = this;

    this->delete_texture = this->renderer.load_texture("romfs:/textures/delete-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
}

SettingsEditor::~SettingsEditor() {
    this->renderer.unregister_texture(this->delete_texture);

    swkbdInlineSetChangedStringCallback(ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetMovedCursorCallback  (ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetDecidedEnterCallback (ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetDecidedCancelCallback(ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetInputText(ImGui::nx::getSwkbd(), "");
    swkbdInlineSetCursorPos(ImGui::nx::getSwkbd(), 0);
}

void SettingsEditor::install_swkbd_callbacks(SwkbdInline *swkbd) {
    swkbdInlineSetChangedStringCallback(ImGui::nx::getSwkbd(), +[](const char *str, SwkbdChangedStringArg *arg) {
        if (arg->stringLen <= s_this->cur_edited_string->capacity())
            *s_this->cur_edited_string = str;

        s_this->cursor_pos         = arg->cursorPos;
        s_this->want_cursor_update = true;
    });

    swkbdInlineSetMovedCursorCallback(ImGui::nx::getSwkbd(), +[](const char *str, SwkbdMovedCursorArg *arg) {
        if (arg->cursorPos == s_this->cursor_pos)
            return;

        s_this->cursor_pos         = arg->cursorPos;
        s_this->want_cursor_update = true;
    });

    swkbdInlineSetDecidedEnterCallback(ImGui::nx::getSwkbd(), +[](const char *str, SwkbdDecidedEnterArg *arg) {
        s_this->cursor_pos         = 0;
        s_this->want_cursor_update = true;

        // Exit input box
        ImGui::GetCurrentContext()->ActiveId = 0;
    });

    swkbdInlineSetDecidedCancelCallback(ImGui::nx::getSwkbd(), +[]() {
        // Exit input box
        ImGui::GetCurrentContext()->ActiveId = 0;
    });
}

void SettingsEditor::reset_swkbd_state(SwkbdInline *swkbd, const utils::StaticString32 &str, SwkbdType type) {
    swkbdInlineMakeAppearArg(&this->appear_args, type);
    swkbdInlineAppearArgSetStringLenMax(&this->appear_args, str.capacity());
    this->appear_args.dicFlag          = 0;
    this->appear_args.returnButtonFlag = 0;

    swkbdInlineSetKeytopBgAlpha(swkbd, 1.0f);
    swkbdInlineSetFooterBgAlpha(swkbd, 1.0f);

    swkbdInlineSetInputText(swkbd, str.c_str());
}

bool SettingsEditor::update_state(PadState &pad, HidTouchScreenState &touch) {
    if (!this->is_displayed && this->has_swkbd_visible) {
        ImGui::nx::hideSwkbd();
        this->has_swkbd_visible = false;
    }

    return true;
}

void SettingsEditor::render() {
    ImGui::Text("Settings");

    ImGui::SameLine((ImGui::GetContentRegionAvail() -
        ImGui::CalcTextSize(app_version_str.data(), app_version_str.data() + app_version_str.size())).x);
    ImGui::Text(app_version_str.data());

    ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, 3.0f);

    if (ImGui::Button("Read from file")) {
        if (this->context.read_from_file())
            std::printf("Failed to read configuration\n");
    }

    ImGui::SameLine();
    if (ImGui::Button("Save to file")) {
        if (this->context.write_to_file())
            std::printf("Failed to save configuration\n");
    }

    ImGui::NewLine();
    ImGui::Text("Network");

    utils::StaticString64 id_buffer;
    auto make_id = [&id_buffer](std::size_t i, std::string_view s) {
        std::snprintf(id_buffer.data(), id_buffer.capacity(), "%s##%ld", s.data(), i);
        return id_buffer.data();
    };

    auto last_input_id = this->cur_input_id;
    SwkbdType wanted_swkbd_type = SwkbdType_Normal;
    bool is_any_input_active = false;

    auto input_with_swkbd = [&](std::size_t i, std::string_view id, auto &str,
            ImGuiInputTextFlags flags = ImGuiInputTextFlags_None, SwkbdType swkbd_type = SwkbdType_Normal) {
        ImGui::PushItemWidth(-1);
        SW_SCOPEGUARD([] { ImGui::PopItemWidth(); });

        ImGui::InputText(make_id(i, id.data()), str.data(), str.capacity(),
            flags | ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_CallbackAlways,
            +[](ImGuiInputTextCallbackData *data) -> int {
                auto *self = static_cast<SettingsEditor *>(data->UserData);

                if (self->want_cursor_update) {
                    data->CursorPos          = self->cursor_pos;
                    self->want_cursor_update = false;
                }

                if (data->CursorPos != self->cursor_pos)
                    swkbdInlineSetCursorPos(ImGui::nx::getSwkbd(), data->CursorPos);

                self->cursor_pos = data->CursorPos;
                data->ClearSelection();

                return 0;
        }, this);

        auto active = ImGui::IsItemActive();
        is_any_input_active |= active;

        if (active) {
            this->cur_input_id = ImGui::GetItemID();
            this->cur_edited_string = &str;
            wanted_swkbd_type = swkbd_type;
        }

        return active;
    };

    if (ImGui::BeginTable("##netlistbox", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY,
            ImVec2(0, this->screen_rel_height(0.2)))) {
        SW_SCOPEGUARD([] { ImGui::EndTable(); });

        ImGui::SetWindowFontScale(0.8);
        SW_SCOPEGUARD([&] { ImGui::SetWindowFontScale(1.0); });

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##delcol",   ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.02));
        ImGui::TableSetupColumn("Type",       ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.06));
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Host",       ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.15));
        ImGui::TableSetupColumn("Port",       ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.06));
        ImGui::TableSetupColumn("Share/path", ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.15));
        ImGui::TableSetupColumn("Username",   ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.12));
        ImGui::TableSetupColumn("Password",   ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.12));
        ImGui::TableSetupColumn("Status",     ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.09));
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < this->context.network_infos.size(); ++i) {
            auto &info = this->context.network_infos[i];

            if (i > 0)
                ImGui::TableNextRow();

            ImVec4 tint_col = (ImGui::nx::getCurrentTheme() == ColorSetId_Dark) ?
                ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);

            ImGui::TableNextColumn();
            bool ret = ImGui::ImageButton(make_id(i, "##deletebtn"),
                ImGui::deko3d::makeTextureID(this->delete_texture.handle, true),
                ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()), ImVec2(0, 0), ImVec2(1, 1),
                ImVec4(0, 0, 0, 0), tint_col);
            if (ret) {
                ret = this->context.unregister_network_fs(*info);
                if (ret)
                    this->context.set_error(ret, Context::ErrorType::Network);

                std::erase_if(this->context.network_infos, [&info](const auto &i) {
                    return i->fs_name == info->fs_name;
                });

                continue;
            }

            ImGui::TableNextColumn();
            {
                ImGui::PushItemWidth(-1);
                SW_SCOPEGUARD([] { ImGui::PopItemWidth(); });

                if (ImGui::BeginCombo(make_id(i, "##protocolcombo"), fs::NetworkFilesystem::protocol_name(info->protocol).data())) {
                    SW_SCOPEGUARD([] { ImGui::EndCombo(); });

                    for (std::size_t i = 0; i < fs::NetworkFilesystem::Protocol::ProtocolMax; ++i) {
                        auto prot_name = fs::NetworkFilesystem::protocol_name(fs::NetworkFilesystem::Protocol(i));
                        if (ImGui::Selectable(prot_name.data(), i == info->protocol))
                            info->protocol = fs::NetworkFilesystem::Protocol(i);
                    }
                }
            }

            ImGui::TableNextColumn();
            if (input_with_swkbd(i, "##nameinput", info->fs_name) && info->fs)
                this->context.unregister_network_fs(*info);

            ImGui::TableNextColumn();
            input_with_swkbd(i, "##hostinput", info->host);

            ImGui::TableNextColumn();
            input_with_swkbd(i, "##portinput", info->port, ImGuiInputTextFlags_None, SwkbdType_NumPad);

            ImGui::TableNextColumn();
            input_with_swkbd(i, "##shareinput", info->share);

            ImGui::TableNextColumn();
            input_with_swkbd(i, "##usernameinput", info->username);

            ImGui::TableNextColumn();
            input_with_swkbd(i, "##passwordinput", info->password, ImGuiInputTextFlags_Password);

            ImGui::TableNextColumn();
            bool is_connected = info->fs && info->fs->connected();
            if (ImGui::Button(make_id(i, !is_connected ? "Connect" : "Disconnect"))) {
                int ret;
                if (!is_connected)
                    ret = this->context.register_network_fs  (*info);
                else
                    ret = this->context.unregister_network_fs(*info);
                if (ret)
                    this->context.set_error(ret, Context::ErrorType::Network);
            }
        }
    }

    if ((this->cur_input_id != last_input_id) || (is_any_input_active && !this->has_swkbd_visible)) {
        this->install_swkbd_callbacks(ImGui::nx::getSwkbd());
        this->reset_swkbd_state(ImGui::nx::getSwkbd(), *this->cur_edited_string, wanted_swkbd_type);

        ImGui::nx::showSwkbd(&this->appear_args);
        this->has_swkbd_visible  = true;
    } else if (!is_any_input_active && this->has_swkbd_visible) {
        ImGui::nx::hideSwkbd();
        this->has_swkbd_visible = false;
    }

    if (ImGui::Button("New"))
        this->context.network_infos.push_back(std::make_unique<Context::NetworkFsInfo>());

    ImGui::NewLine();

    ImGui::BeginTable("##settingssplittable", 2);
    SW_SCOPEGUARD([] { ImGui::EndTable (); });

    ImGui::TableNextColumn();
    ImGui::Text("Video");

    ImGui::Checkbox("Use fast presentation",      &this->context.use_fast_presentation);
    ImGui::Checkbox("Disable screensaver",        &this->context.disable_screensaver);
    ImGui::Checkbox("Override screenshot button", &this->context.override_screenshot_button);

    ImGui::NewLine();
    ImGui::Text("Misc");
    ImGui::Checkbox("Quit to home menu",          &this->context.quit_to_home_menu);

    ImGui::TableNextColumn();
    ImGui::Text("History");

    {
        ImGui::PushItemWidth(this->screen_rel_width(0.2));
        SW_SCOPEGUARD([] { ImGui::PopItemWidth(); });

        std::size_t history_size_min = 0;
        ImGui::DragScalar("Max entries", ImGuiDataType_U64, &context.history_size, 0.05f, &history_size_min);
    }

    if (ImGui::Button("Clear history")) {
        for (auto &fs: this->context.filesystems) {
            if (fs->type == fs::Filesystem::Type::Recent)
                reinterpret_cast<fs::RecentFs *>(fs.get())->clear();
        }
    }

    // We would need to parse mpv.conf to be certain of the watch_later directory's location
    ImGui::SameLine();
    if (ImGui::Button("Clear playback positions")) {
        auto path = fs::Path(Context::AppDirectory) / "watch_later";

        // Using rmdir would need to clear all the files inside beforehand, so just use a faster native call
        if (auto rc = fsdevDeleteDirectoryRecursively(path.c_str()); R_FAILED(rc))
            this->context.set_error(EIO);
    }

    ImGui::NewLine();
    ImGui::Text("USB");

    if (ImGui::BeginTable("##usblistbox", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY,
            this->screen_rel_vec<ImVec2>(0.4, 0.2))) {
        SW_SCOPEGUARD([] { ImGui::EndTable(); });

        ImGui::SetWindowFontScale(0.8);
        SW_SCOPEGUARD([&] { ImGui::SetWindowFontScale(1.0); });

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.25));
        ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.05));
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.1));
        ImGui::TableHeadersRow();

        auto &devs = this->context.ums.get_devices();
        for (std::size_t i = 0; i < devs.size(); ++i) {
            auto &dev = devs[i];

            if (i > 0)
                ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text(dev.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text(LIBUSBHSFS_FS_TYPE_STR(dev.type));

            ImGui::TableNextColumn();
            if (ImGui::Button(make_id(i, "Unmount"))) {
                std::erase_if(this->context.filesystems, [&dev](const auto &fs) {
                    return dev.mount_name == fs->mount_name;
                });
                this->context.cur_fs = this->context.filesystems.front();

                context.ums.unmount_device(dev);
            }
        }
    }
}

bool InfoHelp::update_state(PadState &pad, HidTouchScreenState &touch) {
    return true;
}

void InfoHelp::render() {
    auto bullet_wrapped = [](auto &&...args) {
        ImGui::Bullet();
        ImGui::TextWrapped(std::forward<decltype(args)>(args)...);
    };

    ImGui::BeginTable("##infohelptbl", 2, ImGuiTableFlags_BordersInnerV);
    SW_SCOPEGUARD([] { ImGui::EndTable(); });

    ImGui::TableSetupColumn("##helpcol", ImGuiTableColumnFlags_WidthFixed, this->screen_rel_width(0.6));

    ImGui::TableNextColumn();

    ImGui::Text("Usage:");

    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() / 2));

    {
        ImGui::SeparatorText("Playback");

        ImGui::Indent();
        SW_SCOPEGUARD([] { ImGui::Unindent(); });

        ImGui::SetWindowFontScale(0.9 * this->scale_factor());
        SW_SCOPEGUARD([this] { ImGui::SetWindowFontScale(this->scale_factor()); });

        bullet_wrapped("Press \ue045 to quit");
        bullet_wrapped("Press \ue000 or \ue002 to pause/play");
        bullet_wrapped("Press \ue0a4/\ue0a5 to seek \u00b15s, or \ue0a6/\ue0a7 for \u00b160s");
        bullet_wrapped("Press \ue0a6/\ue0a7 while holding \ue0af/\ue0b0 to skip chapters");
        bullet_wrapped("Use \ue0c1, or slide the touchscreen \ue121 to seek forward and backward");
        bullet_wrapped("Use \ue0c2 horizontally, or slide the right side of the touchscreen \ue121 to adjust the volume");
        bullet_wrapped("Use \ue0c2 vertically, or slide the left side of the touchscreen \ue121 to adjust the backlight brightness");
        bullet_wrapped("Press \ue081/\ue082 to take a screenshot at the source video resolution");
        bullet_wrapped("Press \ue0b1/\ue0b2 to show the playback bar, and \ue001 to hide it");
    }

    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() / 2));

    {
        ImGui::SeparatorText("Settings menu");

        ImGui::Indent();
        SW_SCOPEGUARD([] { ImGui::Unindent(); });

        ImGui::SetWindowFontScale(0.9 * this->scale_factor());
        SW_SCOPEGUARD([this] { ImGui::SetWindowFontScale(this->scale_factor()); });

        bullet_wrapped("Press \ue003 to open the menu");
        bullet_wrapped("Most relevant settings can be found here, "
            "along with useful statistics on playback and performance");
    }

    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() / 2));

    {
        ImGui::SeparatorText("Console");

        ImGui::Indent();
        SW_SCOPEGUARD([] { ImGui::Unindent(); });

        ImGui::SetWindowFontScale(0.9 * this->scale_factor());
        SW_SCOPEGUARD([this] { ImGui::SetWindowFontScale(this->scale_factor()); });

        bullet_wrapped("Press \ue046 to open the console");
        bullet_wrapped("Any mpv command can be executed here. For more information, "
            "see: https://mpv.io/manual/master/#command-interface");
        bullet_wrapped("The console also shows logs from the player core");
        bullet_wrapped("The logging level can be adjusted in the menu (Misc/Log level)");
    }

    ImGui::TableNextColumn();

    ImGui::TextWrapped("%s %s", APP_TITLE, app_version_str.data());

    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() / 2));
    ImGui::SeparatorText("Libraries");
    bullet_wrapped("FFmpeg: %s\n", av_version_info());

    {
        ImGui::Indent();
        SW_SCOPEGUARD([] { ImGui::Unindent(); });

        ImGui::SetWindowFontScale(0.8);
        SW_SCOPEGUARD([this] { ImGui::SetWindowFontScale(this->scale_factor()); });

        bullet_wrapped("libavcodec: "    LIBAVCODEC_IDENT);
        bullet_wrapped("libavformat: "   LIBAVFORMAT_IDENT);
        bullet_wrapped("libavutil: "     LIBAVUTIL_IDENT);
        bullet_wrapped("libavfilter: "   LIBAVFILTER_IDENT);
        bullet_wrapped("libswscale: "    LIBSWSCALE_IDENT);
        bullet_wrapped("libswresample: " LIBSWRESAMPLE_IDENT);
    }

    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() / 2));
    bullet_wrapped("mpv: %s", mpv_version + 4);

    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() / 2));
    bullet_wrapped("libass: %x.%x.%x-%x",
        (LIBASS_VERSION >> 28 & utils::mask(4)), (LIBASS_VERSION >> 20 & utils::mask(20)),
        (LIBASS_VERSION >> 12 & utils::mask(8)), (LIBASS_VERSION >> 00 & utils::mask(12)));

    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() / 2));
    bullet_wrapped("Dear ImGui: " IMGUI_VERSION);

    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() / 2));
    bullet_wrapped("libusbhsfs: %d.%d.%d", LIBUSBHSFS_VERSION_MAJOR,
        LIBUSBHSFS_VERSION_MINOR, LIBUSBHSFS_VERSION_MICRO);

    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() / 2));
    ImGui::SeparatorText("Built on");
    ImGui::Text(" %s", app_build_date_str.data());
}

} // namespace sw::ui
