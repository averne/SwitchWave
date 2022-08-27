#pragma once

#include <chrono>
#include <vector>

#include "widget.hpp"
#include "../libmpv.hpp"

namespace ampnx {

using namespace std::chrono_literals;

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

        template <typename T>
        struct NumericProperty {
            std::string_view display_name;
            std::string_view prop_name;
            T value;
            T min, max;
        };

        struct PassInfo {
            std::string desc;
            std::vector<double> samples;
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
        std::string video_pixfmt, video_hw_piwfmt,
            audio_format, audio_layout;
        int video_width = 0, video_height = 0, video_width_scaled = 0, video_height_scaled = 0,
            audio_num_channels = 0, audio_samplerate = 0;
        std::int64_t video_bitrate = 0, audio_bitrate = 0;
        double avsync = 0, container_specified_fps = 0, container_estimated_fps = 0;
        std::int64_t dropped_vo_frames = 0, dropped_dec_frames = 0;

        // double sub_speed;
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
