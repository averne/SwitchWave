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
        constexpr static FloatUS VisibleDelay       = 3s;
        constexpr static FloatUS VisibleFadeIO      = 0.2s;
        constexpr static float  BarWidth            = 1.0;
        constexpr static float  BarHeight           = 0.07;
        constexpr static float  SeekBarWidth        = 0.7 * SeekBar::BarWidth;
        constexpr static float  SeekBarHeight       = 0.9 * SeekBar::BarHeight;
        constexpr static float  SeekBarContourPx    = 4;
        constexpr static float  SeekBarPadding      = 0.01;
        constexpr static float  SeekBarLinesWidthPx = 2;

    public:
        SeekBar(Renderer &renderer, LibmpvController &lmpv);
        virtual ~SeekBar() override;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;
        virtual void render() override;

        void begin_visible(bool visible) {
            this->visible = visible;
            if (visible)
                this->visible_start = std::chrono::system_clock::now();
        }

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

        bool                                  visible      = false;
        bool                                  is_appearing = true;
        float                                 fadeio_alpha = 0.0f;
        std::chrono::system_clock::time_point visible_start;

        int is_paused      = false;
        double time_pos    = 0;
        double duration    = 0;
        double percent_pos = 0;

        std::vector<ChapterInfo>   chapters;
        std::vector<SeekableRange> seekable_ranges;

        Renderer::Texture play_texture, pause_texture, previous_texture, next_texture;
};

class PlayerMenu final: public Widget {
    public:
        PlayerMenu(Renderer &renderer, LibmpvController &lmpv);
        virtual ~PlayerMenu() override;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;
        virtual void render() override;

    private:
        LibmpvController &lmpv;


};

class PlayerGui final: public Widget {
    public:
        PlayerGui(Renderer &renderer, LibmpvController &lmpv):
            Widget(renderer), lmpv(lmpv), seek_bar(renderer, lmpv) { }
        virtual ~PlayerGui() override = default;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;
        virtual void render() override;

    private:
        LibmpvController &lmpv;

        SeekBar seek_bar;
};

} // namespace ampnx
