#pragma once

#include <chrono>
#include <limits>
#include <type_traits>
#include <vector>

#include "widget.hpp"
#include "../libmpv.hpp"
#include "../utils.hpp"

namespace ampnx {

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

    public:
        constexpr static FloatUS VisibleDelay         = 3s;
        constexpr static FloatUS VisibleFadeIO        = 0.2s;
        constexpr static float   BarWidth             = 1.0;
        constexpr static float   BarHeight            = 0.07;
        constexpr static float   SeekBarWidth         = 0.7 * SeekBar::BarWidth;
        constexpr static float   SeekBarHeight        = 0.9 * SeekBar::BarHeight;
        constexpr static float   SeekBarContourPx     = 4;
        constexpr static float   SeekBarPadding       = 0.01;
        constexpr static float   SeekBarLinesWidthPx  = 2;
        constexpr static auto    SeekJoystickDeadZone = 4000;

    public:
        SeekBar(Renderer &renderer, LibmpvController &lmpv);
        virtual ~SeekBar() override;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;
        virtual void render() override;

        inline void begin_visible() {
            this->is_visible    = true;
            this->visible_start = std::chrono::system_clock::now();
        }

        inline bool is_paused() const {
            return this->pause;
        }

    public:
        bool is_visible = false;

    private:
        struct ChapterInfo {
            std::string title;
            double time;
        };

        struct SeekableRange {
            double start, end;
        };

    private:
        LibmpvController &lmpv;

        std::chrono::system_clock::time_point visible_start;

        bool  is_appearing = false;
        float fadeio_alpha = 0.0f;

        int    pause       = false;
        double time_pos    = 0;
        double duration    = 0;
        double percent_pos = 0;

        std::vector<ChapterInfo>   chapters;
        std::vector<SeekableRange> seekable_ranges;

        Renderer::Texture play_texture, pause_texture, previous_texture, next_texture;
};

struct MpvOptionCheckbox {
    std::string_view name, display_name;
    bool value = false;

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

    template <typename U = void*>
    void run(LibmpvController &lmpv, U(*transform)(LibmpvController&, T) = nullptr) {
        if (ImGui::BeginCombo(this->display_name.data(), this->options[this->cur_idx].first.data())) {
            AMPNX_SCOPEGUARD([] { ImGui::EndCombo(); });

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
        constexpr static float MenuWidth           = 0.4;
        constexpr static float MenuHeight          = 0.90;
        constexpr static float MenuPosX            = 0.58;
        constexpr static float MenuPosY            = 0.02;
        constexpr static float SubMenuWidth        = 0.32;
        constexpr static float SubMenuHeight       = 0.5;
        constexpr static float SubMenuPosX         = 0.25;
        constexpr static float SubMenuPosY         = 0.05;
        constexpr static auto  StatsRefreshTimeout = 1.0s;

    public:
        PlayerMenu(Renderer &renderer, LibmpvController &lmpv);
        virtual ~PlayerMenu() override;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;
        virtual void render() override;

    public:
        bool is_visible = false;

    private:
        struct TrackInfo {
            std::int64_t track_id;
            std::string  name;
            bool         selected;
        };

        struct PassInfo {
            std::string desc;
            double average, peak, last;
            std::vector<double> samples;
        };

        struct MpvTracklist {
            std::string_view name;
            std::string_view display_name;
            std::vector<TrackInfo> &tracks;

            void run(LibmpvController &lmpv) {
                if (ImGui::BeginListBox(this->display_name.data(), ImVec2(-1, 0))) {
                    AMPNX_SCOPEGUARD([] { ImGui::EndListBox(); });

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
        LibmpvController &lmpv;

        std::chrono::system_clock::time_point last_stats_update;

        std::vector<TrackInfo> video_tracks;
        std::vector<TrackInfo> audio_tracks;
        std::vector<TrackInfo> sub_tracks;

        std::vector<PassInfo> passes_info;

        char *file_format = nullptr, *video_codec = nullptr, *audio_codec = nullptr,
            *hwdec_current = nullptr, *hwdec_interop = nullptr;
        std::string video_pixfmt, video_hw_piwfmt, video_colorspace, video_color_range, video_gamma,
            audio_format, audio_layout;
        int video_width = 0, video_height = 0, video_width_scaled = 0, video_height_scaled = 0,
            audio_num_channels = 0, audio_samplerate = 0;
        std::int64_t video_bitrate = 0, audio_bitrate = 0;
        double avsync = 0, container_specified_fps = 0, container_estimated_fps = 0;
        std::int64_t dropped_vo_frames = 0, dropped_dec_frames = 0;
        double demuxer_cache_begin = 0, demuxer_cache_end = 0, demuxer_cache_speed = 0;
        std::int64_t demuxer_cached_bytes = 0, demuxer_forward_bytes = 0;

        MpvTracklist video_tracklist = {
            .name         = "vid",
            .display_name = "##videolist",
            .tracks       = this->video_tracks,
        };

        MpvTracklist audio_tracklist = {
            .name         = "aid",
            .display_name = "##audio",
            .tracks       = this->audio_tracks,
        };

        MpvTracklist sub_tracklist = {
            .name         = "sid",
            .display_name = "##sublist",
            .tracks       = this->sub_tracks,
        };

        MpvOptionCombo<const char *, 3> profile_combo = {
            .name         = "profile",
            .display_name = "Profile",
            .options      = std::array{
                std::pair{"Default"sv,     "default"},
                std::pair{"Gpu HQ"sv,      "gpu-hq"},
                std::pair{"Low latency"sv, "low-latency"},
            },
        };

        MpvOptionCombo<const char *, 5> fbo_format_combo = {
            .name         = "fbo-format",
            .display_name = "FBO format",
            .options      = std::array{
                std::pair{"RGBA16F"sv,  "rgba16f"},
                std::pair{"RG11B10F"sv, "rg11b10f"},
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

        MpvOptionCheckbox use_hwdec_checkbox = {
            .name         = "hwdec",
            .display_name = "Use hardware decoding",
            .value        = true,
        };

        MpvOptionCombo<const char *, 10> aspect_ratio_combo = {
            .name         = "video-aspect-override",
            .display_name = "Aspect ratio",
            .options      = {
                std::pair{"Auto"sv,    "-1"},
                std::pair{"Disable"sv, "0"},
                std::pair{"1:1"sv,     "1:1"},
                std::pair{"3:2"sv,     "3:2"},
                std::pair{"4:3"sv,     "4:3"},
                std::pair{"14:9"sv,    "14:9"},
                std::pair{"14:10"sv,   "14:10"},
                std::pair{"16:9"sv,    "16:9"},
                std::pair{"16:10"sv,   "16:10"},
                std::pair{"2.35:1"sv,  "2.35:1"},
                // std::pair{"5:4"sv,     "5:4"},
                // std::pair{"11:8"sv,    "11:8"},
            },
        };

        std::array<MpvOptionBoundedScalar<double>, 3> video_zoom_options = {
            MpvOptionBoundedScalar<double>{"video-zoom",  "Zoom",  -2, 2, 0, "%.2f"},
            MpvOptionBoundedScalar<double>{"video-pan-x", "Pan X", -1, 1, 0, "%.2f"},
            MpvOptionBoundedScalar<double>{"video-pan-y", "Pan Y", -1, 1, 0, "%.2f"},
        };

        std::array<MpvOptionBoundedScalar<std::int64_t>, 5> video_color_options = {
            MpvOptionBoundedScalar<std::int64_t>{"brightness", "Brightness", -100, 100, 0, "%d"},
            MpvOptionBoundedScalar<std::int64_t>{"contrast",   "Contrast",   -100, 100, 0, "%d"},
            MpvOptionBoundedScalar<std::int64_t>{"saturation", "Saturation", -100, 100, 0, "%d"},
            MpvOptionBoundedScalar<std::int64_t>{"gamma",      "Gamma",      -100, 100, 0, "%d"},
            MpvOptionBoundedScalar<std::int64_t>{"hue",        "Hue",        -100, 100, 0, "%d"},
        };

        bool want_subwindow = false;
        int subwindow_id;

        MpvOptionCombo<const char *, 10> downmix_combo = {
            .name         = "audio-channels",
            .display_name = "##channelmix",
            .options      = {
                std::pair{"Auto",   "auto"},
                std::pair{"Stereo", "stereo"},
                std::pair{"Mono",   "mono"},
            },
        };

        MpvOptionBoundedScalar<double> volume_slider = {
            .name          = "volume",
            .display_name  = "##volumeslider",
            .min           = 0,
            .max           = 150,
            .default_value = 100,
            .format        = "%.1f%",
        };

        MpvOptionCheckbox mute_checkbox = {
            .name         = "ao-mute",
            .display_name = "Mute",
            .value        = true,
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
                std::pair{"Video",  0.0},
                std::pair{"23",     23.0},
                std::pair{"24",     24.0},
                std::pair{"25",     25.0},
                std::pair{"30",     30.0},
                std::pair{"23.976", 24000.0/1001.0},
                std::pair{"29.970", 30000.0/1001.0},
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

        MpvOptionCombo<const char *, 3> cache_combo = {
            .name         = "cache",
            .display_name = "##demuxercache",
            .options      = {
                std::pair{"Auto", "auto"},
                std::pair{"Yes",  "yes"},
                std::pair{"No",   "no"},
            },
        };
};

class PlayerGui final: public Widget {
    public:
        PlayerGui(Renderer &renderer, LibmpvController &lmpv):
            Widget(renderer), lmpv(lmpv),
            seek_bar(renderer, lmpv), menu(renderer, lmpv) { }
        virtual ~PlayerGui() override = default;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;
        virtual void render() override;

        inline bool is_visible() const {
            return this->seek_bar.is_visible || this->menu.is_visible;
        }

        inline bool is_paused() const {
            return this->seek_bar.is_paused();
        }

    private:
        LibmpvController &lmpv;

        SeekBar seek_bar;
        PlayerMenu menu;
};

} // namespace ampnx
