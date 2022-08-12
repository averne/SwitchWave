#pragma once

#include <cmath>
#include <concepts>
#include <switch.h>

#include "../render.hpp"

namespace ampnx {

class Widget {
    public:
        Widget(Renderer &renderer): renderer(renderer) { }
        virtual ~Widget() = default;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) = 0;

        virtual void render() = 0;

        constexpr float screen_rel_width(auto x) const {
            return std::round(static_cast<float>(x) * this->renderer.image_width);
        }

        constexpr float screen_rel_height(auto y) const {
            return std::round(static_cast<float>(y) * this->renderer.image_height);
        }

        template <typename T>
        constexpr T screen_rel_vec(auto x, auto y) const {
            return { Widget::screen_rel_width(x), Widget::screen_rel_height(y) };
        }

    protected:
        Renderer &renderer;
};

} // namespace ampnx
