#include <algorithm>
#include <tuple>
#include <utility>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

#include "../imgui_impl_hos/imgui_deko3d.h"

#include "../utils.hpp"

#include "player_gui.hpp"

namespace ampnx {

#define FORMAT_TIME(s) (s)/60/60%99, (s)/60%60, (s)%60

bool PlayerGui::update_state(PadState &pad, HidTouchScreenState &touch) {
    auto down = padGetButtonsDown(&pad);

    if (down & HidNpadButton_Plus)
        return false;

    this->menu    .update_state(pad, touch);
    this->seek_bar.update_state(pad, touch);

    if (down & HidNpadButton_Y)
        this->lmpv.command_async("screenshot-to-file", "screenshot.png", "subtitles");

    if (down & (HidNpadButton_R | HidNpadButton_L))
        this->lmpv.seek((down & HidNpadButton_R) ? 5 : -5);

    if (down & (HidNpadButton_ZR | HidNpadButton_ZL))
        this->lmpv.seek((down & HidNpadButton_ZR) ? 60 : -60);

    if (!this->is_visible()) {
        if (down & HidNpadButton_A)
            this->lmpv.command_async("cycle", "pause");

        constexpr auto pop_seekbar_mask = HidNpadButton_Left | HidNpadButton_Up |
            HidNpadButton_Right | HidNpadButton_Down;
        if (touch.count || (down & pop_seekbar_mask))
            this->seek_bar.begin_visible();
    }

    return true;
}

void PlayerGui::render() {
    this->menu    .render();
    this->seek_bar.render();
}

SeekBar::SeekBar(Renderer &renderer, LibmpvController &lmpv): Widget(renderer), lmpv(lmpv) {
    this->play_texture      = this->renderer.load_texture("romfs:/textures/play-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->pause_texture     = this->renderer.load_texture("romfs:/textures/pause-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->previous_texture  = this->renderer.load_texture("romfs:/textures/previous-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->next_texture      = this->renderer.load_texture("romfs:/textures/next-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);

    this->lmpv.observe_property("pause",       &this->pause);
    this->lmpv.observe_property("time-pos",    &this->time_pos);
    this->lmpv.observe_property("duration",    &this->duration);
    this->lmpv.observe_property("percent-pos", &this->percent_pos);

    this->lmpv.observe_property("chapter-list", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self     = static_cast<SeekBar  *>(user);
        auto *node     = static_cast<mpv_node *>(prop->data);
        auto *chapters = node->u.list;

        self->chapters.resize(chapters->num);

        for (int i = 0; i < chapters->num; ++i) {
            auto *chapter = chapters->values[i].u.list;

            self->chapters[i] = {
                .title = LibmpvController::node_map_find<char *>(chapter, "title"),
                .time  = LibmpvController::node_map_find<double>(chapter, "time" ),
            };
        }

        mpv_free_node_contents(node);
    }, this);

    this->lmpv.observe_property("demuxer-cache-state", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self        = static_cast<SeekBar  *>(user);
        auto *node        = static_cast<mpv_node *>(prop->data);
        auto *cache_state = node->u.list;

        auto *ranges = LibmpvController::node_map_find<mpv_node_list *>(cache_state, "seekable-ranges");

        self->seekable_ranges.resize(ranges->num);

        for (int i = 0; i < ranges->num; ++i) {
            auto *range = ranges->values[i].u.list;

            self->seekable_ranges[i] = {
                .start = LibmpvController::node_map_find<double>(range, "start"),
                .end   = LibmpvController::node_map_find<double>(range, "end"  ),
            };
        }

        mpv_free_node_contents(node);
    }, this);
}

SeekBar::~SeekBar() {
    this->lmpv.unobserve_property("pause");
    this->lmpv.unobserve_property("time-pos");
    this->lmpv.unobserve_property("duration");
    this->lmpv.unobserve_property("percent-pos");
    this->lmpv.unobserve_property("chapter-list");
    this->lmpv.unobserve_property("demuxer-cache-state");
}

bool SeekBar::update_state(PadState &pad, HidTouchScreenState &touch) {
    auto down = padGetButtonsDown(&pad);
    auto now  = std::chrono::system_clock::now();

    if (this->is_visible && (down || touch.count)) {
        this->visible_start = now;
    } else if (this->is_visible) {
        auto delta = std::chrono::duration_cast<FloatUS>(now - this->visible_start);

        if (this->fadeio_alpha != 1.0f && delta < SeekBar::VisibleFadeIO) {
            this->fadeio_alpha = delta / SeekBar::VisibleFadeIO;
            this->is_appearing = true;
        } else if (delta > SeekBar::VisibleDelay - SeekBar::VisibleFadeIO) {
            this->fadeio_alpha = (SeekBar::VisibleDelay - delta) / SeekBar::VisibleFadeIO;
        } else {
            this->fadeio_alpha = 1.0f;
            this->is_appearing = false;
        }

        if (delta >= SeekBar::VisibleDelay)
            this->is_visible = false, this->fadeio_alpha = 0.0f;
    }

    auto stick_pos = padGetStickPos(&pad, 0);
    if (std::abs(stick_pos.x) > SeekBar::SeekJoystickDeadZone) {
        this->begin_visible();

        char time[10] = {};
        std::snprintf(time, sizeof(time), "%+f", (float(stick_pos.x) - SeekBar::SeekJoystickDeadZone) / float(JOYSTICK_MAX));
        this->lmpv.command_async("seek", time, "relative-percent");
    }

    return false;
}

void SeekBar::render() {
    if (!this->is_visible)
        return;

    auto &io    =  ImGui::GetIO();
    auto &style =  ImGui::GetStyle();
    auto &imctx = *ImGui::GetCurrentContext();

    imctx.NavDisableHighlight = false;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,            this->fadeio_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    AMPNX_SCOPEGUARD([] { ImGui::PopStyleVar(); });
    AMPNX_SCOPEGUARD([] { ImGui::PopStyleVar(); });

    ImGui::Begin("##seekbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowSize(Widget::screen_rel_vec<ImVec2>(SeekBar::BarWidth, SeekBar::BarHeight));
    ImGui::SetWindowPos (Widget::screen_rel_vec<ImVec2>((1.0 - SeekBar::BarWidth) / 2.0, 1.0 - SeekBar::BarHeight));
    ImGui::SetWindowFontScale(float(this->renderer.image_height) / 720.0f);
    AMPNX_SCOPEGUARD([] { ImGui::End(); });

    auto window_height = ImGui::GetWindowHeight();
    auto img_size = window_height - 2 * (style.WindowPadding.y + style.FramePadding.y);
    auto img_handle = ImGui::deko3d::makeTextureID(
        (this->pause ? this->play_texture : this->pause_texture).handle, true);

    // Workaround, due to how ImGui calculates ImageButton ids internally
    // the button gets deselected when changing the texture
    auto ImageButtonWidthId = [style](ImGuiID id, ImTextureID user_texture_id, const ImVec2& size,
            const ImVec2& uv0=ImVec2(0, 0), const ImVec2& uv1=ImVec2(1,1), int frame_padding=-1,
            const ImVec4& bg_col=ImVec4(0,0,0,0), const ImVec4& tint_col=ImVec4(1,1,1,1)) {
        const ImVec2 padding=(frame_padding >= 0) ? ImVec2((float)frame_padding, (float)frame_padding) : style.FramePadding;
        return ImGui::ImageButtonEx(id, user_texture_id, size, uv0, uv1, padding, bg_col, tint_col);
    };

    // Play/pause/skip buttons
    auto playpause_id = ImGui::GetID("##playpause");
    if (ImageButtonWidthId(playpause_id, img_handle, ImVec2(img_size, img_size)))
        this->lmpv.command_async("cycle", "pause");
    if (this->is_appearing) {
        imctx.NavWindow = imctx.CurrentWindow;
        ImGui::SetNavID(playpause_id, imctx.NavLayer, 0, ImRect());
    }

    ImGui::SameLine();
    ImGui::ImageButton(ImGui::deko3d::makeTextureID(this->previous_texture.handle, true),
        ImVec2{img_size, img_size});

    ImGui::SameLine();
    ImGui::ImageButton(ImGui::deko3d::makeTextureID(this->next_texture.handle,     true),
        ImVec2{img_size, img_size});

    auto text_yoffset = (window_height - ImGui::GetFontSize()) / 2.0f;

    // Current timestamp
    ImGui::SameLine(); ImGui::SetCursorPosY(text_yoffset);
    ImGui::Text("%02u:%02u:%02u", FORMAT_TIME(std::uint32_t(this->time_pos)));

    // Seek bar
    ImGui::SameLine(); ImGui::SetCursorPosY(SeekBar::SeekBarContourPx / 2);
    auto cursor = ImGui::GetCursorScreenPos();
    auto padding = ImVec2(Widget::screen_rel_height(SeekBar::SeekBarPadding),
        Widget::screen_rel_height(SeekBar::SeekBarPadding));
    auto bb          = ImRect(cursor, cursor + Widget::screen_rel_vec<ImVec2>(0.7, SeekBar::SeekBarHeight));
    auto interior_bb = ImRect(bb.Min + padding, bb.Max - padding);

    ImGui::ItemSize(bb);
    ImGui::ItemAdd(bb, ImGui::GetID("##seekbar"), nullptr, ImGuiItemFlags_Disabled);

    auto ts_to_seekbar_pos = [this, &interior_bb](double timestamp) {
        return std::round(interior_bb.Min.x +
            interior_bb.GetWidth() * float(timestamp) / float(this->duration));
    };

    auto seekbar_pos_to_ts = [this, &interior_bb](float x) {
        return (x - interior_bb.Min.x) / interior_bb.GetWidth() * float(this->duration);
    };

    if (io.MouseDown[0] && interior_bb.Contains(io.MousePos) && interior_bb.Contains(io.MouseClickedPos[0]))
        this->lmpv.set_property_async("time-pos", double(seekbar_pos_to_ts(io.MousePos.x)));

    auto *list = ImGui::GetWindowDrawList();
    list->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(ImGuiCol_Button), 0, 0, SeekBar::SeekBarContourPx);

    list->AddRectFilled(interior_bb.Min,
        ImVec2{interior_bb.Min.x + interior_bb.GetWidth() * float(this->percent_pos) / 100.0f, interior_bb.Max.y},
        ImGui::GetColorU32(ImGuiCol_ButtonActive));

    // Chapters
    for (auto &chapter: this->chapters) {
        // Skip the first chapter
        if (chapter.time == 0.0)
            continue;

        auto pos_x = ts_to_seekbar_pos(chapter.time);
        list->AddLine(ImVec2{pos_x, interior_bb.Min.y},
            ImVec2{pos_x, interior_bb.Max.y},
            ImGui::GetColorU32({1.0, 1.0, 1.0, 1.0}), SeekBar::SeekBarLinesWidthPx);
    }

    auto pos_y = interior_bb.GetCenter().y;
    for (auto &range: this->seekable_ranges) {
        list->AddLine(ImVec2{ts_to_seekbar_pos(range.start), pos_y},
            ImVec2{ts_to_seekbar_pos(range.end), pos_y},
            ImGui::GetColorU32({1.0, 1.0, 1.0, 1.0}), SeekBar::SeekBarLinesWidthPx);
    }

    // Total duration
    ImGui::SameLine(); ImGui::SetCursorPosY(text_yoffset);
    ImGui::Text("%02u:%02u:%02u", FORMAT_TIME(std::uint32_t(this->duration)));
}

PlayerMenu::PlayerMenu(Renderer &renderer, LibmpvController &lmpv): Widget(renderer), lmpv(lmpv) {
    this->lmpv.observe_property("track-list", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self   = static_cast<PlayerMenu *>(user);
        auto *node   = static_cast<mpv_node *>(prop->data);
        auto *tracks = node->u.list;

        auto disable_track = TrackInfo{
            .track_id = 0,
            .name     = "None",
            .selected = false,
        };

        self->video_tracks.clear(), self->video_tracks.push_back(disable_track);
        self->audio_tracks.clear(), self->audio_tracks.push_back(disable_track);
        self->sub_tracks  .clear(), self->sub_tracks  .push_back(disable_track);

        for (int i = 0; i < tracks->num; ++i) {
            auto *track = tracks->values[i].u.list;

            auto  id    = LibmpvController::node_map_find<std::int64_t>(track, "id");
            auto *title = LibmpvController::node_map_find<char *>      (track, "title");
            auto *lang  = LibmpvController::node_map_find<char *>      (track, "lang");

            std::string name;
            if (title && lang) {
                name.resize(std::snprintf(nullptr, 0, "%s (%s)", title, lang) + 1);
                std::snprintf(name.data(), name.size(), "%s (%s)", title, lang);
            } else if (title) {
                name = title;
            } else if (lang) {
                name = lang;
            } else {
                name.resize(std::snprintf(nullptr, 0, "[Unnamed %ld]", id) + 1);
                std::snprintf(name.data(), name.size(), "[Unnamed %ld]", id);
            }

            auto info = TrackInfo{
                .track_id =   id,
                .name     =   name,
                .selected = !!LibmpvController::node_map_find<std::uint32_t>(track, "selected"),
            };

            std::string_view type = LibmpvController::node_map_find<char *>(track, "type");
            if (type == "video")
                self->video_tracks.push_back(std::move(info));
            else if (type == "audio")
                self->audio_tracks.push_back(std::move(info));
            else if (type == "sub")
                self->sub_tracks  .push_back(std::move(info));
        }

        mpv_free_node_contents(node);
    }, this);

    this->last_stats_update = std::chrono::system_clock::now();

    this->lmpv.observe_property("file-format", &this->file_format);
    this->lmpv.observe_property("video-codec", &this->video_codec);
    this->lmpv.observe_property("audio-codec", &this->audio_codec);
    this->lmpv.observe_property("hwdec-current", &this->hwdec_current);
    this->lmpv.observe_property("hwdec-interop", &this->hwdec_interop);
    this->lmpv.observe_property("avsync", &this->avsync);
    this->lmpv.observe_property("frame-drop-count", &this->dropped_vo_frames,
#ifdef DEBUG
    [](void*, mpv_event_property *prop) {
        std::printf("VO  dropped: %ld\n", *static_cast<std::int64_t *>(prop->data));
    }
#else
    nullptr
#endif
    );
    this->lmpv.observe_property("decoder-frame-drop-count", &this->dropped_dec_frames,
#ifdef DEBUG
    [](void*, mpv_event_property *prop) {
        std::printf("DEC dropped: %ld\n", *static_cast<std::int64_t *>(prop->data));
    }
#else
    nullptr
#endif
    );
    this->lmpv.observe_property("video-bitrate", &this->video_bitrate);
    this->lmpv.observe_property("audio-bitrate", &this->audio_bitrate);
    this->lmpv.observe_property("container-fps", &this->container_specified_fps);
    this->lmpv.observe_property("estimated-vf-fps", &this->container_estimated_fps);

    this->lmpv.observe_property("video-params", MPV_FORMAT_NODE, nullptr, [](void *user, mpv_event_property *prop) {
        auto *self   = static_cast<PlayerMenu *>(user);
        auto *node   = static_cast<mpv_node *>(prop->data);
        auto *params = node->u.list;

        self->video_width       = LibmpvController::node_map_find<std::int64_t>(params, "w");
        self->video_height      = LibmpvController::node_map_find<std::int64_t>(params, "h");
        self->video_pixfmt      = LibmpvController::node_map_find<char *>(params, "pixelformat")    ?: "";
        self->video_hw_piwfmt   = LibmpvController::node_map_find<char *>(params, "hw-pixelformat") ?: "";
        self->video_colorspace  = LibmpvController::node_map_find<char *>(params, "colormatrix")    ?: "";
        self->video_color_range = LibmpvController::node_map_find<char *>(params, "colorlevels")    ?: "";
        self->video_gamma       = LibmpvController::node_map_find<char *>(params, "gamma")          ?: "";

        mpv_free_node_contents(node);
    }, this);

    this->lmpv.observe_property("osd-dimensions", MPV_FORMAT_NODE, nullptr, [](void *user, mpv_event_property *prop) {
        auto *self = static_cast<PlayerMenu *>(user);
        auto *node = static_cast<mpv_node *>(prop->data);
        auto *dims = node->u.list;

        self->video_width_scaled   = LibmpvController::node_map_find<std::int64_t>(dims, "w") -
            LibmpvController::node_map_find<std::int64_t>(dims, "ml") -
            LibmpvController::node_map_find<std::int64_t>(dims, "mr");
        self->video_height_scaled  = LibmpvController::node_map_find<std::int64_t>(dims, "h") -
            LibmpvController::node_map_find<std::int64_t>(dims, "mt") -
            LibmpvController::node_map_find<std::int64_t>(dims, "mb");

        mpv_free_node_contents(node);
    }, this);

    this->lmpv.observe_property("audio-params", MPV_FORMAT_NODE, nullptr, [](void *user, mpv_event_property *prop) {
        auto *self   = static_cast<PlayerMenu *>(user);
        auto *node   = static_cast<mpv_node *>(prop->data);
        auto *params = node->u.list;

        self->audio_format       = LibmpvController::node_map_find<char *>(params, "format")   ?: "";
        self->audio_layout       = LibmpvController::node_map_find<char *>(params, "channels") ?: "";
        self->audio_samplerate   = LibmpvController::node_map_find<std::int64_t>(params, "samplerate");
        self->audio_num_channels = LibmpvController::node_map_find<std::int64_t>(params, "channel-count");

        mpv_free_node_contents(node);
    }, this);
}

PlayerMenu::~PlayerMenu() {
    this->lmpv.unobserve_property("track-list");
    this->lmpv.unobserve_property("file-format");
    this->lmpv.unobserve_property("video-codec");
    this->lmpv.unobserve_property("audio-codec");
    this->lmpv.unobserve_property("hwdec-current");
    this->lmpv.unobserve_property("hwdec-interop");
    this->lmpv.unobserve_property("avsync");
    this->lmpv.unobserve_property("frame-drop-count");
    this->lmpv.unobserve_property("decoder-frame-drop-count");
    this->lmpv.unobserve_property("video-bitrate");
    this->lmpv.unobserve_property("audio-bitrate");
    this->lmpv.unobserve_property("container-fps");
    this->lmpv.unobserve_property("estimated-vf-fps");
    this->lmpv.unobserve_property("video-params");
    this->lmpv.unobserve_property("osd-dimensions");
    this->lmpv.unobserve_property("audio-params");
}

bool PlayerMenu::update_state(PadState &pad, HidTouchScreenState &touch) {
    auto down = padGetButtonsDown(&pad);
    auto now  = std::chrono::system_clock::now();

    if (down & HidNpadButton_X)
        this->is_visible ^= 1;

    if (now - this->last_stats_update > PlayerMenu::StatsRefreshTimeout) {
        this->last_stats_update = now;
        this->lmpv.get_property_async("vo-passes", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
            auto *self   = static_cast<PlayerMenu *>(user);
            auto *node   = static_cast<mpv_node *>(prop->data);
            auto *passes = node->u.list;

            AMPNX_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

            self->passes_info.clear();

            if (!passes)
                return;

            auto *fresh = LibmpvController::node_map_find<mpv_node_list *>(passes, "fresh");
            for (int i = 0; i < fresh->num; ++i) {
                auto *pass = fresh->values[i].u.list;
                auto *samples = LibmpvController::node_map_find<mpv_node_list *>(pass, "samples");

                auto &info = self->passes_info.emplace_back(
                    LibmpvController::node_map_find<char *>(pass, "desc"),
                    double(LibmpvController::node_map_find<std::int64_t>(pass, "avg"))  / 1.0e6,
                    double(LibmpvController::node_map_find<std::int64_t>(pass, "peak")) / 1.0e6,
                    double(LibmpvController::node_map_find<std::int64_t>(pass, "last")) / 1.0e6
                );

                info.samples.resize(samples->num);
                std::transform(samples->values, samples->values + samples->num, info.samples.begin(), [](auto &in) {
                    // implot renders doubles so we store that to avoid a conversion step
                    return double(in.u.int64) / 1.0e6;
                });
            }
        }, this);

        this->lmpv.get_property_async("demuxer-cache-state", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
            auto *self  = static_cast<PlayerMenu *>(user);
            auto *node  = static_cast<mpv_node *>(prop->data);
            auto *state = node->u.list;
            if (!state)
                return;

            self->demuxer_cache_begin   = LibmpvController::node_map_find<double>(state, "reader-pts");
            self->demuxer_cache_end     = LibmpvController::node_map_find<double>(state, "cache-end");
            self->demuxer_cached_bytes  = LibmpvController::node_map_find<std::int64_t>(state, "total-bytes");
            self->demuxer_forward_bytes = LibmpvController::node_map_find<std::int64_t>(state, "fw-bytes");
            self->demuxer_cache_speed   = LibmpvController::node_map_find<std::int64_t>(state, "raw-input-rate");
        }, this);
    }

    return false;
}

void PlayerMenu::render() {
    if (!this->is_visible)
        return;

    auto &io = ImGui::GetIO();

    ImGui::Begin("Menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowSize(Widget::screen_rel_vec<ImVec2>(PlayerMenu::MenuWidth, PlayerMenu::MenuHeight));
    ImGui::SetWindowPos (Widget::screen_rel_vec<ImVec2>(PlayerMenu::MenuPosX,  PlayerMenu::MenuPosY));
    ImGui::SetWindowFontScale(float(this->renderer.image_height) / 720.0f);
    AMPNX_SCOPEGUARD([] { ImGui::End(); });

    ImGui::BeginTabBar("##tabbar", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton |
        ImGuiTabBarFlags_NoTabListScrollingButtons | ImGuiTabBarFlags_NoTooltip);
    AMPNX_SCOPEGUARD([] { ImGui::EndTabBar(); });

    if (ImGui::BeginTabItem("Video", nullptr, ImGuiTabItemFlags_NoReorder)) {
        AMPNX_SCOPEGUARD([] { ImGui::EndTabItem(); });

        ImGui::Text("Track");
        this->video_tracklist.run(this->lmpv);

        ImGui::Separator();
        ImGui::Text("Quality");
        this->profile_combo     .run(this->lmpv);
        this->fbo_format_combo  .run(this->lmpv);
        this->hdr_peak_checkbox .run(this->lmpv);
        this->use_hwdec_checkbox.run(this->lmpv, +[]([[maybe_unused]] LibmpvController &lmpv, bool val) {
            return val ? "auto" : "no";
        });

        // TODO: Deinterlace options (with VIC filter)

        ImGui::Separator();
        ImGui::Text("Window");

        constexpr static std::array scaling_opts = {
            "Keep aspect ratio",
            "Stretch to fit",
            "Native",
        };

        static int scaling_opt = 0;
        if (ImGui::BeginCombo("Scale video", scaling_opts[scaling_opt])) {
            AMPNX_SCOPEGUARD([] { ImGui::EndCombo(); });

            for (std::size_t i = 0; i < scaling_opts.size(); i++) {
                bool is_selected = std::size_t(scaling_opt) == i;
                if (ImGui::Selectable(scaling_opts[i], is_selected)) {
                    scaling_opt = i;

                    auto opt = std::string_view(scaling_opts[scaling_opt]);
                    if (opt == "Keep aspect ratio") {
                        this->lmpv.set_property_async("video-unscaled", "no");
                        this->lmpv.set_property_async("keepaspect", "yes");
                    } else if (opt == "Stretch to fit") {
                        this->lmpv.set_property_async("video-unscaled", "no");
                        this->lmpv.set_property_async("keepaspect", "no");
                    } else if (opt == "Native") {
                        this->lmpv.set_property_async("video-unscaled", "yes");
                        this->lmpv.set_property_async("keepaspect", "yes");
                    }
                }

                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
        }

        this->aspect_ratio_combo.run(this->lmpv);

        ImGui::Separator();
        ImGui::Text("Other");

        auto show_submenu_window = [this](std::string_view title, auto &options) {
            ImGui::Begin(title.data(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::SetWindowSize(Widget::screen_rel_vec<ImVec2>(PlayerMenu::SubMenuWidth, PlayerMenu::SubMenuHeight));
            ImGui::SetWindowPos (Widget::screen_rel_vec<ImVec2>(PlayerMenu::SubMenuPosX,  PlayerMenu::SubMenuPosY));
            ImGui::SetWindowFontScale(float(this->renderer.image_height) / 720.0f);
            AMPNX_SCOPEGUARD([] { ImGui::End(); });

            for (auto &prop: options)
                prop.run(this->lmpv);

            if (ImGui::Button("Reset")) {
                for (auto &prop: options)
                    prop.reset(this->lmpv);
            }

            ImGui::SameLine();
            if (ImGui::Button("Return"))
                this->want_subwindow = false;
        };

        if (ImGui::Button("Zoom/Position"))
            this->want_subwindow ^= true, this->subwindow_id = 0;

        ImGui::SameLine();
        if (ImGui::Button("Color equalizer"))
            this->want_subwindow ^= true, this->subwindow_id = 1;

        if (this->want_subwindow) {
            switch (this->subwindow_id) {
                case 0:
                    show_submenu_window("Zoom##window",            this->video_zoom_options);
                    break;
                case 1:
                    show_submenu_window("Color equalizer##window", this->video_color_options);
                    break;
                default:
                    break;
            }
        }
    }

    if (ImGui::BeginTabItem("Audio", nullptr, ImGuiTabItemFlags_NoReorder)) {
        AMPNX_SCOPEGUARD([] { ImGui::EndTabItem(); });

        ImGui::Text("Track");
        this->audio_tracklist.run(this->lmpv);

        ImGui::Separator();
        ImGui::Text("Channel mixing");
        this->downmix_combo.run(this->lmpv);

        ImGui::Separator();
        ImGui::Text("Volume");
        this->volume_slider.run(this->lmpv, "Reset##volume");
        this->mute_checkbox.run(this->lmpv);

        ImGui::Separator();
        ImGui::Text("Delay");
        this->audio_delay_slider.run(this->lmpv, "Reset##audiodelay");
    }

    if (ImGui::BeginTabItem("Subtitles", nullptr, ImGuiTabItemFlags_NoReorder)) {
        AMPNX_SCOPEGUARD([] { ImGui::EndTabItem(); });

        ImGui::Text("Track");
        this->sub_tracklist.run(this->lmpv);

        ImGui::Separator();
        ImGui::Text("Delay");
        this->sub_delay_slider.run(this->lmpv, "Reset##subdelay");

        ImGui::Separator();
        ImGui::Text("FPS");
        this->sub_fps_combo.run(this->lmpv, +[](LibmpvController &lmpv, double val) {
            if (val == 0.0) {
                double tmp;
                if (int res = lmpv.get_property("container-fps", tmp); !res)
                    return tmp;
            }
            return val;
        });

        // TODO: subtitle speed?

        ImGui::Separator();
        ImGui::Text("Size/position");
        this->sub_scale_slider.run(this->lmpv, "Reset##subscale");
        this->sub_pos_slider  .run(this->lmpv, "Reset##subpos");

        ImGui::Separator();
        ImGui::Text("Style");
        this->embedded_fonts_checkbox.run(this->lmpv);
    }

    if (ImGui::BeginTabItem("Misc")) {
        AMPNX_SCOPEGUARD([] { ImGui::EndTabItem(); });

        ImGui::Text("Demuxer cache");
        this->cache_combo.run(this->lmpv);
    }

    auto bullet_wrapped = [](std::string_view fmt, auto &&...args) {
        ImGui::Bullet();
        ImGui::TextWrapped(fmt.data(), std::forward<decltype(args)>(args)...);
    };

    if (ImGui::BeginTabItem("Stats")) {
        AMPNX_SCOPEGUARD([] { ImGui::EndTabItem(); });

        ImGui::BeginTabBar("##statstabbar", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton |
            ImGuiTabBarFlags_NoTabListScrollingButtons | ImGuiTabBarFlags_NoTooltip);
        AMPNX_SCOPEGUARD([] { ImGui::EndTabBar(); });

        if (ImGui::BeginTabItem("Info")) {
            AMPNX_SCOPEGUARD([] { ImGui::EndTabItem(); });

            ImGui::SetWindowFontScale(0.7 * float(this->renderer.image_height) / 720.0f);
            AMPNX_SCOPEGUARD([&] { ImGui::SetWindowFontScale(float(this->renderer.image_height) / 720.0f); });

            ImGui::Text("Source: %s", this->file_format);

            ImGui::Separator();
            ImGui::Text("Video");
            bullet_wrapped("Codec: %s", this->video_codec);
            if (this->hwdec_current)
                bullet_wrapped("hwdec: %s", this->hwdec_current);
            bullet_wrapped("FPS: %.1f (specified) %.1f (estimated)",
                this->container_specified_fps, this->container_estimated_fps);
            bullet_wrapped("A/V desync: %.3fs", this->avsync);
            bullet_wrapped("Dropped: %ld (VO) %ld (decoder)", this->dropped_vo_frames, this->dropped_dec_frames);
            bullet_wrapped("Size: %dx%d, scaled: %dx%d", this->video_width, this->video_height,
                this->video_width_scaled, this->video_height_scaled);
            if (!this->video_hw_piwfmt.empty())
                bullet_wrapped("Pixel format: %s [%s]",
                    this->video_pixfmt.c_str(), this->video_hw_piwfmt.c_str());
            else
                bullet_wrapped("Pixel format: %s", this->video_pixfmt.c_str());
            bullet_wrapped("Colorspace: %s, range: %s, gamma: %s", this->video_colorspace.c_str(),
                this->video_color_range.c_str(), this->video_gamma.c_str());
            bullet_wrapped("Bitrate: %.1fkbps\n", float(this->video_bitrate) / 1000.0f);

            ImGui::Separator();
            ImGui::Text("Audio");
            bullet_wrapped("Codec: %s", this->audio_codec);
            bullet_wrapped("Layout: %s (%d channels)", this->audio_layout.c_str(), this->audio_num_channels);
            bullet_wrapped("Format: %s", this->audio_format.c_str());
            bullet_wrapped("Samplerate: %dHz", this->audio_samplerate);
            bullet_wrapped("Bitrate: %.1fkbps\n", float(this->audio_bitrate) / 1000.0f);

            ImGui::Separator();
            ImGui::Text("Cache");
            bullet_wrapped("Packet queue: %02u:%02u:%02u‒%02u:%02u:%02u (%02u:%02u:%02u)",
                FORMAT_TIME(std::uint32_t(this->demuxer_cache_begin)), FORMAT_TIME(std::uint32_t(this->demuxer_cache_end)),
                FORMAT_TIME(std::uint32_t(this->demuxer_cache_end - this->demuxer_cache_begin)));
            bullet_wrapped("RAM used: %.02fMiB (%.2fMiB forward)", double(this->demuxer_cached_bytes)/0x100000,
                double(this->demuxer_forward_bytes)/0x100000);
            bullet_wrapped("Speed: %.2fMiB/s", this->demuxer_cache_speed/0x100000);

            ImGui::Separator();
            ImGui::Text("Interface");
            bullet_wrapped("FPS: %.1fHz, frame time %.1fms", io.Framerate, io.DeltaTime * 1000.0f);
            bullet_wrapped("Vertices: %d", io.MetricsRenderVertices);
            bullet_wrapped("Indices: %d", io.MetricsRenderIndices);
            bullet_wrapped("Allocations: %d", io.MetricsActiveAllocations);
        }

        if (ImGui::BeginTabItem("Passes")) {
            AMPNX_SCOPEGUARD([] { ImGui::EndTabItem(); });

            static bool is_pie_plot = false;
            if (ImGui::RadioButton("Graphs",   !is_pie_plot)) { is_pie_plot = false; } ImGui::SameLine();
            if (ImGui::RadioButton("Pie chart", is_pie_plot)) { is_pie_plot = true;  }

            static auto perf_type = 0;
            if (is_pie_plot) {
                if (ImGui::RadioButton("Average", perf_type == 0)) { perf_type = 0; } ImGui::SameLine();
                if (ImGui::RadioButton("Peak",    perf_type == 1)) { perf_type = 1; } ImGui::SameLine();
                if (ImGui::RadioButton("Last",    perf_type == 2)) { perf_type = 2; }
            }

            ImGui::SetWindowFontScale(0.5 * float(this->renderer.image_height) / 720.0f);
            AMPNX_SCOPEGUARD([&] { ImGui::SetWindowFontScale(float(this->renderer.image_height) / 720.0f); });

            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetColorU32(ImGuiCol_WindowBg));
            AMPNX_SCOPEGUARD([] { ImGui::PopStyleColor(); });

            auto plot_flags = ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs | ImPlotFlags_NoFrame |
                ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_AntiAliased |
                (is_pie_plot ? ImPlotFlags_Equal : 0) /* |
                (this->passes_info.size() > 20 ? ImPlotFlags_NoLegend : 0) */;

            ImPlot::PushColormap(ImPlotColormap_Dark);
            AMPNX_SCOPEGUARD([] { ImPlot::PopColormap(); });

            if (ImPlot::BeginPlot("Shader passes", ImVec2(-1, -1), plot_flags)) {
                AMPNX_SCOPEGUARD([] { ImPlot::EndPlot(); });

                auto axes_flags = ImPlotAxisFlags_AutoFit |
                    (is_pie_plot ? ImPlotAxisFlags_NoDecorations : 0);

                ImPlot::SetupAxes("", "ms", axes_flags, axes_flags);
                ImPlot::SetupLegend(ImPlotLocation_South, ImPlotLegendFlags_Outside);

                if (!is_pie_plot) {
                    for (auto &stats: this->passes_info)
                        ImPlot::PlotLine(stats.desc.c_str(), stats.samples.data(), stats.samples.size());
                } else {
                    auto names    = std::vector<const char *>(this->passes_info.size());
                    auto averages = std::vector<double>(this->passes_info.size());

                    std::transform(this->passes_info.begin(), this->passes_info.end(), names.begin(),
                        [](auto &&pass) { return pass.desc.c_str(); });

                    std::transform(this->passes_info.begin(), this->passes_info.end(), averages.begin(),
                        [](auto &&pass) { return *(&pass.average + perf_type); });

                    ImPlot::PlotPieChart(names.data(), averages.data(), this->passes_info.size(),
                        0.5, 0.5, 0.4, true, "%.2fms", 0.0);
                }
            }
        }
    }
}

} // namespace ampnx
