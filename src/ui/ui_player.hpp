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

#include <chrono>
#include <limits>
#include <list>
#include <type_traits>
#include <vector>

#include <imgui.h>

#include "libmpv.hpp"
#include "context.hpp"
#include "utils.hpp"
#include "ui/ui_common.hpp"

namespace sw::ui {

using namespace std::chrono_literals;
using namespace std::string_view_literals;

template <typename T>
static constexpr ImGuiDataType to_imgui_data_type() {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_same_v<Decayed, std::int8_t>)
        return ImGuiDataType_S8;
    else if constexpr (std::is_same_v<Decayed, std::uint8_t>)
        return ImGuiDataType_U8;
    else if constexpr (std::is_same_v<Decayed, std::int16_t>)
        return ImGuiDataType_S16;
    else if constexpr (std::is_same_v<Decayed, std::uint16_t>)
        return ImGuiDataType_U16;
    else if constexpr (std::is_same_v<Decayed, std::int32_t>)
        return ImGuiDataType_S32;
    else if constexpr (std::is_same_v<Decayed, std::uint32_t>)
        return ImGuiDataType_U32;
    else if constexpr (std::is_same_v<Decayed, std::int64_t>)
        return ImGuiDataType_S64;
    else if constexpr (std::is_same_v<Decayed, std::uint64_t>)
        return ImGuiDataType_U64;
    else if constexpr (std::is_same_v<Decayed, float>)
        return ImGuiDataType_Float;
    else if constexpr (std::is_same_v<Decayed, double>)
        return ImGuiDataType_Double;
    else
        return -1;
}

class SeekBar final: public Widget {
    public:
        using FloatUS = std::chrono::duration<float, std::micro>;

        struct ChapterInfo {
            std::string title;
            double time;
        };

        struct SeekableRange {
            double start, end;
        };

    public:
        constexpr static FloatUS VisibleDelay         = 3s;
        constexpr static FloatUS VisibleFadeIO        = 0.2s;
        constexpr static float   BarWidth             = 1.0;
        constexpr static float   BarHeight            = 0.10;
        constexpr static float   SeekBarWidth         = 0.7 * SeekBar::BarWidth;
        constexpr static float   SeekBarContourPx     = 3;
        constexpr static float   SeekBarPadding       = 0.01;
        constexpr static float   SeekBarLinesWidthPx  = 2;
        constexpr static int     SeekBarPopButtons    = HidNpadButton_Left | HidNpadButton_Right;

    public:
        SeekBar(Renderer &renderer, Context &context, LibmpvController &lmpv);
        virtual ~SeekBar() override;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;
        virtual void render() override;

        inline void begin_visible() {
            if (!this->is_appearing)
                this->visible_start = std::chrono::system_clock::now();
            this->is_visible = true;
        }

        inline const ChapterInfo *get_current_chapter() const {
            if (this->chapters.empty())
                return nullptr;

            for (auto it = this->chapters.rbegin(); it != this->chapters.rend(); ++it) {
                if (this->time_pos > it->time)
                    return &*it;
            }
            return nullptr;
        }

    public:
        bool is_visible   = false;
        bool ignore_input = false;

        int          pause       = false;
        double       time_pos    = 0;
        double       duration    = 0;
        double       percent_pos = 0;
        std::int64_t chapter     = 0;
        char        *media_title = nullptr;

        std::vector<ChapterInfo>   chapters;
        std::vector<SeekableRange> seekable_ranges;

    private:
        LibmpvController &lmpv;
        Context          &context;

        std::chrono::system_clock::time_point visible_start;

        bool  is_appearing = false;
        float fadeio_alpha = 0.0f;

        Renderer::Texture play_texture, pause_texture, previous_texture, next_texture;
};

struct MpvOptionCheckbox {
    std::string_view name, display_name;
    bool value = false;

    int observe(LibmpvController &lmpv) {
        return lmpv.observe_property(this->name, MPV_FORMAT_FLAG, nullptr, +[](void *user, mpv_event_property *prop) {
            auto *self = static_cast<MpvOptionCheckbox *>(user);
            self->value = !!*static_cast<int *>(prop->data);
        }, this);
    }

    int unobserve(LibmpvController &lmpv) const {
        return lmpv.unobserve_property(this->name);
    }

    template <typename T = void*>
    void run(LibmpvController &lmpv, T(*transform)(LibmpvController&, bool) = nullptr) {
        if (ImGui::Checkbox(this->display_name.data(), &this->value)) {
            if (transform)
                lmpv.set_property_async(this->name.data(), transform(lmpv, this->value));
            else
                lmpv.set_property_async(this->name.data(), int(this->value));
        }
    }
};

template <typename T, std::size_t N>
struct MpvOptionCombo {
    std::string_view name, display_name;
    std::array<std::pair<std::string_view, T>, N> options;
    int cur_idx = 0;

    int observe(LibmpvController &lmpv) {
        return lmpv.observe_property(this->name, LibmpvController::to_mpv_format<T>(), nullptr, +[](void *user, mpv_event_property *prop) {
            auto *self = static_cast<MpvOptionCombo *>(user);

            for (std::size_t i = 0; i < self->options.size(); ++i) {
                auto &&[_, val] = self->options[i];

                using Decayed = std::decay_t<T>;
                if constexpr (std::is_same_v<Decayed, char *> || std::is_same_v<Decayed, const char *>) {
                    if (std::string_view(val) == *static_cast<T *>(prop->data)) {
                        self->cur_idx = i;
                        break;
                    }
                } else {
                    if (val == *static_cast<T *>(prop->data)) {
                        self->cur_idx = i;
                        break;
                    }
                }
            }
        }, this);
    }

    int unobserve(LibmpvController &lmpv) const {
        return lmpv.unobserve_property(this->name);
    }

    template <typename U = void*>
    void run(LibmpvController &lmpv, U(*transform)(LibmpvController&, T) = nullptr) {
        if (ImGui::BeginCombo(this->display_name.data(), this->options[this->cur_idx].first.data())) {
            SW_SCOPEGUARD([] { ImGui::EndCombo(); });

            for (std::size_t i = 0; i < this->options.size(); i++) {
                bool is_selected = std::size_t(this->cur_idx) == i;
                if (ImGui::Selectable(this->options[i].first.data(), is_selected)) {
                    this->cur_idx = i;
                    if (transform)
                        lmpv.set_property_async(this->name, transform(lmpv, this->options[this->cur_idx].second));
                    else
                        lmpv.set_property_async(this->name, this->options[this->cur_idx].second);
                }

                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
        }
    }
};

template <typename T>
struct MpvOptionBoundedScalar {
    std::string_view name;
    std::string_view display_name;
    T min, max;
    T default_value = 0;
    const char *format = nullptr;
    T cur_value = default_value;

    int observe(LibmpvController &lmpv) {
        return lmpv.observe_property(this->name, &this->cur_value);
    }

    int unobserve(LibmpvController &lmpv) const {
        return lmpv.unobserve_property(this->name);
    }

    void run(LibmpvController &lmpv, const char *reset_label = nullptr) {
        if (ImGui::SliderScalar(this->display_name.data(), to_imgui_data_type<T>(),
                &this->cur_value, &this->min, &this->max, this->format)) {
            lmpv.set_property_async(this->name, this->cur_value);
        }

        if (reset_label) {
            ImGui::SameLine();
            if (ImGui::Button(reset_label))
                lmpv.set_property_async(this->name, this->cur_value = this->default_value);
        }
    }

    void reset(LibmpvController &lmpv) {
        lmpv.set_property_async(this->name, this->cur_value = this->default_value);
    }
};

template <typename T>
struct MpvOptionScalar {
    std::string_view name;
    std::string_view display_name;
    float speed = 0.01;
    T default_value = 0;
    const char *format = nullptr;
    T cur_value = default_value;

    int observe(LibmpvController &lmpv) {
        return lmpv.observe_property(this->name, &this->cur_value);
    }

    int unobserve(LibmpvController &lmpv) const {
        return lmpv.unobserve_property(this->name);
    }

    void run(LibmpvController &lmpv, const char *reset_label = nullptr) {
        constexpr T min = std::numeric_limits<T>::min(), max = std::numeric_limits<T>::min();

        if (ImGui::DragScalar(this->display_name.data(), to_imgui_data_type<T>(),
                &this->cur_value, this->speed, &min, &max, this->format)) {
            lmpv.set_property_async(this->name, this->cur_value);
        }

        if (reset_label) {
            ImGui::SameLine();
            if (ImGui::Button(reset_label))
                lmpv.set_property_async(this->name, this->cur_value = this->default_value);
        }
    }

    void reset(LibmpvController &lmpv) {
        lmpv.set_property_async(this->name, this->cur_value = this->default_value);
    }
};

class PlayerMenu final: public Widget {
    public:
        constexpr static float MenuWidth            = 0.4;
        constexpr static float MenuHeight           = 0.925;
        constexpr static float MenuPosX             = 0.58;
        constexpr static float MenuPosY             = 0.02;
        constexpr static float SubMenuWidth         = 0.35;
        constexpr static float SubMenuHeight        = 0.4;
        constexpr static float VideoSubMenuHeight   = 0.75;
        constexpr static float SubMenuPosX          = 0.22;
        constexpr static float SubMenuPosY          = 0.02;
        constexpr static float FilepickerWidth      = 0.625;
        constexpr static float FilepickerHeight     = 0.875;
        constexpr static float FilepickerPosX       = 0.02;
        constexpr static float FilepickerPosY       = 0.02;
        constexpr static auto  StatsRefreshInterval = 1.0s;

    public:
        PlayerMenu(Renderer &renderer, Context &context, LibmpvController &lmpv);
        virtual ~PlayerMenu() override;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;
        virtual void render() override;

    public:
        struct TrackInfo {
            std::string  name;
            std::int64_t track_id;
            bool         selected;
        };

        struct PassInfo {
            std::string desc;
            double average, peak, last;
            std::vector<double> samples;
        };

        struct PlaylistEntryInfo {
            std::string name;
            std::int64_t id;
            bool playing;
        };

    public:
        bool is_visible = false;

        std::vector<std::string> profile_list;

        std::vector<TrackInfo> video_tracks;
        std::vector<TrackInfo> audio_tracks;
        std::vector<TrackInfo> sub_tracks;

        std::vector<PassInfo> passes_info;

        std::vector<PlaylistEntryInfo> playlist_info;

        char *file_format = nullptr, *video_codec = nullptr, *audio_codec = nullptr,
            *hwdec_current = nullptr, *hwdec_interop = nullptr;
        std::string video_pixfmt, video_hw_pixfmt, video_colorspace, video_color_range, video_gamma,
            audio_format, audio_layout;
        int video_width = 0, video_height = 0, video_width_scaled = 0, video_height_scaled = 0,
            audio_num_channels = 0, audio_samplerate = 0;
        std::int64_t video_bitrate = 0, audio_bitrate = 0;
        double avsync = 0, container_specified_fps = 0, container_estimated_fps = 0;
        std::int64_t dropped_vo_frames = 0, dropped_dec_frames = 0;
        double demuxer_cache_begin = 0, demuxer_cache_end = 0, demuxer_cache_speed = 0;
        std::int64_t demuxer_cached_bytes = 0, demuxer_forward_bytes = 0;
        int video_unscaled, keepaspect;

    private:
        enum class SubwindowType {
            None,
            VideoQuality,
            ZoomPos,
            ColorEqualizer,
            ShaderFilepicker,
            SubtitleFilepicker,
            PlaylistFilepicker,
        };

        struct MpvTracklist {
            std::string_view name;
            std::string_view display_name;
            std::vector<TrackInfo> &tracks;

            void run(LibmpvController &lmpv) {
                if (ImGui::BeginListBox(this->display_name.data(), ImVec2(-1, 0))) {
                    SW_SCOPEGUARD([] { ImGui::EndListBox(); });

                    for (auto &track: this->tracks) {
                        if (ImGui::Selectable(track.name.c_str(), track.selected))
                            lmpv.set_property_async(this->name.data(), track.track_id);

                        if (track.selected)
                            ImGui::SetItemDefaultFocus();
                    }
                }
            }
        };

    private:
        constexpr bool is_filepicker(SubwindowType type) {
            return type == SubwindowType::ShaderFilepicker || type == SubwindowType::SubtitleFilepicker ||
                type == SubwindowType::PlaylistFilepicker;
        }

    private:
        LibmpvController &lmpv;
        Context          &context;
        Explorer         explorer;

        std::chrono::system_clock::time_point last_stats_update;

        std::int64_t playlist_selection_id = 0;

        int perf_plot_is_pie = false;
        int perf_plot_pie_type = 0;

        MpvTracklist video_tracklist = {
            .name         = "vid",
            .display_name = "##videolist",
            .tracks       = this->video_tracks,
        };

        MpvTracklist audio_tracklist = {
            .name         = "aid",
            .display_name = "##audiolist",
            .tracks       = this->audio_tracks,
        };

        MpvTracklist sub_tracklist = {
            .name         = "sid",
            .display_name = "##sublist",
            .tracks       = this->sub_tracks,
        };

        MpvOptionCombo<const char *, 6> fbo_format_combo = {
            .name         = "fbo-format",
            .display_name = "FBO format",
            .options      = std::array{
                std::pair{"RGBA16F"sv,  "rgba16f"},
                std::pair{"RG11B10F"sv, "rg11b10f"},
                std::pair{"RGB10A2"sv,  "rgb10_a2"},
                std::pair{"RGBA16"sv,   "rgba16"},
                std::pair{"RGBA8"sv,    "rgba8"},
                std::pair{"RGBA32F"sv,  "rgba32f"},
            },
        };

        MpvOptionCheckbox hdr_peak_checkbox = {
            .name         = "hdr-compute-peak",
            .display_name = "Compute HDR peak",
            .value        = true,
        };

        MpvOptionCheckbox deinterlace_checkbox = {
            .name         = "deinterlace",
            .display_name = "Software deinterlacing",
            .value        = false,
        };

        bool  has_sharpness_filter = false, has_denoise_filter = false;
        float sharpness_value = 0.0f,       denoise_value = 0.0f;
        int   sharpness_dimensions = 0,     denoise_dimensions = 0;

        bool has_hw_deinterlace = false;
        int hw_deinterlace_mode = 1;

        MpvOptionCheckbox use_hwdec_checkbox = {
            .name         = "hwdec",
            .display_name = "Use hardware decoding",
            .value        = true,
        };

        MpvOptionCombo<double, 10> aspect_ratio_combo = {
            .name         = "video-aspect-override",
            .display_name = "Aspect ratio",
            .options      = {
                // Defined as floats because mpv internally stores the ratio as such, leading to precision issues
                std::pair{"Auto"sv,    double(-1.0f)},
                std::pair{"Disable"sv, double(0.0f)},
                std::pair{"1:1"sv,     double(1.0f/1.0f)},
                std::pair{"3:2"sv,     double(3.0f/2.0f)},
                std::pair{"4:3"sv,     double(4.0f/3.0f)},
                std::pair{"14:9"sv,    double(14.0f/9.0f)},
                std::pair{"14:10"sv,   double(14.0f/10.0f)},
                std::pair{"16:9"sv,    double(16.0f/9.0f)},
                std::pair{"16:10"sv,   double(16.0f/10.0f)},
                std::pair{"2.35:1"sv,  double(2.35f/1.0f)},
                // std::pair{"5:4"sv,     double(5.0f/4.0f)},
                // std::pair{"11:8"sv,    double(11.0f/8.0f)},
            },
        };

        std::array<MpvOptionBoundedScalar<double>, 3> video_zoom_options = {
            MpvOptionBoundedScalar<double>{"video-zoom"sv,  "Zoom",  -2, 2, 0, "%.2f"},
            MpvOptionBoundedScalar<double>{"video-pan-x"sv, "Pan X", -1, 1, 0, "%.2f"},
            MpvOptionBoundedScalar<double>{"video-pan-y"sv, "Pan Y", -1, 1, 0, "%.2f"},
        };

        MpvOptionCombo<std::int64_t, 4> rotation_combo = {
            .name         = "video-rotate",
            .display_name = "Rotation",
            .options      = {
                std::pair{"0°"sv,   std::int64_t(0)},
                std::pair{"90°"sv,  std::int64_t(90)},
                std::pair{"180°"sv, std::int64_t(180)},
                std::pair{"270°"sv, std::int64_t(270)},
            },
        };

        std::array<MpvOptionBoundedScalar<std::int64_t>, 5> video_color_options = {
            MpvOptionBoundedScalar<std::int64_t>{"brightness"sv, "Brightness", -100, 100, 0, "%d"},
            MpvOptionBoundedScalar<std::int64_t>{"contrast"sv,   "Contrast",   -100, 100, 0, "%d"},
            MpvOptionBoundedScalar<std::int64_t>{"saturation"sv, "Saturation", -100, 100, 0, "%d"},
            MpvOptionBoundedScalar<std::int64_t>{"gamma"sv,      "Gamma",      -100, 100, 0, "%d"},
            MpvOptionBoundedScalar<std::int64_t>{"hue"sv,        "Hue",        -100, 100, 0, "%d"},
        };

        SubwindowType cur_subwindow = SubwindowType::None;

        MpvOptionCombo<const char *, 3> downmix_combo = {
            .name         = "audio-channels",
            .display_name = "##channelmix",
            .options      = {
                std::pair{"Auto"sv,   "auto"},
                std::pair{"Stereo"sv, "stereo"},
                std::pair{"Mono"sv,   "mono"},
            },
        };

        MpvOptionBoundedScalar<double> volume_slider = {
            .name          = "volume",
            .display_name  = "##volumeslider",
            .min           = 0,
            .max           = 150,
            .default_value = 100,
            .format        = "%.1f%%",
        };

        MpvOptionCheckbox mute_checkbox = {
            .name         = "ao-mute",
            .display_name = "Mute",
            .value        = false,
        };

        MpvOptionScalar<double> audio_delay_slider = {
            .name         = "audio-delay",
            .display_name = "##audiodelay",
            .format       = "%.1fs",
        };

        MpvOptionScalar<double> sub_delay_slider = {
            .name         = "sub-delay",
            .display_name = "##subdelay",
            .format       = "%.1fs",
        };

        MpvOptionCombo<double, 7> sub_fps_combo = {
            .name         = "sub-fps",
            .display_name = "##subfps",
            .options      = {
                // Defined as floats because mpv internally stores the fps as such, leading to precision issues
                std::pair{"Video"sv,  double(0.0f)},
                std::pair{"23"sv,     double(23.0f)},
                std::pair{"24"sv,     double(24.0f)},
                std::pair{"25"sv,     double(25.0f)},
                std::pair{"30"sv,     double(30.0f)},
                std::pair{"23.976"sv, double(24000.0f/1001.0f)},
                std::pair{"29.970"sv, double(30000.0f/1001.0f)},
            },
        };

        MpvOptionBoundedScalar<double> sub_scale_slider = {
            .name          = "sub-scale",
            .display_name  = "##subscale",
            .min           = 0,
            .max           = 10,
            .default_value = 1,
            .format        = "Scale: %.1f",
        };

        MpvOptionBoundedScalar<std::int64_t> sub_pos_slider = {
            .name          = "sub-pos",
            .display_name  = "##subpos",
            .min           = 0,
            .max           = 150,
            .default_value = 100,
            .format        = "Position: %d%%",
        };

        MpvOptionCheckbox embedded_fonts_checkbox = {
            .name         = "embeddedfonts",
            .display_name = "Use embedded fonts",
            .value        = true,
        };

        MpvOptionBoundedScalar<double> speed_slider = {
            .name          = "speed",
            .display_name  = "##speed",
            .min           = 0.1,
            .max           = 5.0,
            .default_value = 1.0,
            .format        = "x%.2f",
        };

        MpvOptionCombo<const char *, 3> cache_combo = {
            .name         = "cache",
            .display_name = "##demuxercache",
            .options      = {
                std::pair{"Auto"sv, "auto"},
                std::pair{"Yes"sv,  "yes"},
                std::pair{"No"sv,   "no"},
            },
        };

        MpvOptionCombo<const char *, 9> log_level_combo = {
            .name         = "msg-level",
            .display_name = "##msglevel",
            .options      = {
                std::pair{"No"sv,      "all=no"},
                std::pair{"Fatal"sv,   "all=fatal"},
                std::pair{"Error"sv,   "all=error"},
                std::pair{"Warning"sv, "all=warn"},
                std::pair{"Info"sv,    "all=info"},
                std::pair{"Status"sv,  "all=status"},
                std::pair{"Verbose"sv, "all=v"},
                std::pair{"Debug"sv,   "all=debug"},
                std::pair{"Trace"sv,   "all=trace"},
            },
            .cur_idx      = 5, // status
        };
};

class Console final: public Widget {
    public:
        constexpr static float ConsoleWidth      = 0.4;
        constexpr static float ConsoleHeight     = 0.91;
        constexpr static float ConsolePosX       = 0.58;
        constexpr static float ConsolePosY       = 0.02;
        constexpr static int   ConsoleMaxLogs    = 100;
        constexpr static int   ConsoleMaxHistory = 10;

    public:
        Console(Renderer &renderer, LibmpvController &lmpv);
        virtual ~Console() override;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;
        virtual void render() override;

    private:
        void set_text(const std::string_view text);

        constexpr static ImU32 map_log_level_color(mpv_log_level lvl, bool dark = true) {
            switch (lvl) {
                case MPV_LOG_LEVEL_FATAL:
                    return dark ? ImColor(0xf9, 0x91, 0x57) : ImColor(0xc5, 0x4a, 0x07);
                case MPV_LOG_LEVEL_ERROR:
                    return dark ? ImColor(0xf2, 0x77, 0x7a) : ImColor(0xbb, 0x11, 0x14);
                case MPV_LOG_LEVEL_WARN:
                    return dark ? ImColor(0xff, 0xcc, 0x66) : ImColor(0xcc, 0x88, 0x00);
                case MPV_LOG_LEVEL_INFO:
                    return dark ? ImColor(0xff, 0xff, 0xff) : ImColor(0xb3, 0xb3, 0xb3);
                case MPV_LOG_LEVEL_V:
                    return dark ? ImColor(0x99, 0xcc, 0x99) : ImColor(0x44, 0x88, 0x44);
                case MPV_LOG_LEVEL_DEBUG:
                case MPV_LOG_LEVEL_TRACE:
                    return dark ? ImColor(0x93, 0x9f, 0xa0) : ImColor(0x48, 0x50, 0x51);
                default:
                case MPV_LOG_LEVEL_NONE:
                    return ImColor(0x00, 0x00, 0x00, 0x00);
            }
        }

    public:
        bool is_visible = false;

    private:
        struct LogEntry {
            mpv_log_level level;
            std::string message;
        };

    private:
        LibmpvController &lmpv;

        SwkbdAppearArg appear_args;

        std::list<LogEntry> logs;

        std::string input_text;
        int cursor_pos          = 0;
        bool want_cursor_update = false;
        bool want_text_clear    = false;
        bool is_frozen          = false;

        std::list<std::string> cmd_history;
        decltype(cmd_history)::const_iterator cmd_history_pos = {};

        static inline Console *s_this;
};

class PlayerGui final: public Widget {
    public:
        enum class TouchGestureState {
            Tap,
            SlideSeek,
            SlideBrightness,
            SlideVolume,
        };

    public:
        constexpr static float TouchGestureThreshold         = 60.0f;
        constexpr static float TouchGestureXMultipler        = 150.0f; // 2:30
        constexpr static float TouchGestureYMultipler        = 1.5f;   // > 1 so we can go from 0 to 100% in one swipe
        constexpr static auto  MovieCaptureTimeout           = std::chrono::nanoseconds(500ms).count();
        constexpr static auto  BrightnessVolumeChangeTimeout = 0.3s;

    public:
        PlayerGui(Renderer &renderer, Context &context, LibmpvController &lmpv);
        virtual ~PlayerGui() override;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;
        virtual void render() override;

        inline bool is_submenu_visible() const {
            return this->seek_bar.is_visible || this->menu.is_visible || this->console.is_visible;
        }

        inline bool is_visible() const {
            return this->has_show_string || this->is_submenu_visible();
        }

        inline bool is_paused() const {
            return this->seek_bar.pause;
        }

        template <typename T, typename ...Args>
        inline void set_show_string(T timeout, Args &&...args) {
            this->has_show_string = true;
            this->show_string_begin   = std::chrono::system_clock::now();
            this->show_string_timeout = timeout;
            std::snprintf(this->show_string.data(), this->show_string.capacity(), std::forward<Args>(args)...);
        }

    private:
        void screenshot_button_thread_fn(std::stop_token token);

    private:
        LibmpvController &lmpv;
        Context          &context;

        SeekBar seek_bar;
        PlayerMenu menu;
        Console console;

        std::jthread screenshot_button_thread;

        bool has_touch = false;
        TouchGestureState touch_state = TouchGestureState::Tap;
        HidTouchState orig_touch, cur_touch;
        union {
            double time_pos;
            float brightness;
            struct {
                AudioTarget audio_target;
                std::int32_t audio_vol;
            };
        } touch_setting_start;
        double js_time_start = 0.0;

        bool has_show_string = false;
        utils::StaticString64 show_string;
        std::chrono::system_clock::time_point show_string_begin;
        std::chrono::microseconds             show_string_timeout;

        std::chrono::system_clock::time_point last_brightness_change = {},
            last_volume_change = {};
};

} // namespace sw::ui
