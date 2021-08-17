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

#include <cmath>
#include <concepts>
#include <switch.h>

#include "render.hpp"

namespace sw::ui {

class Widget {
    public:
        Widget(Renderer &renderer): renderer(renderer) { }
        virtual ~Widget() = default;

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) = 0;

        virtual void render() = 0;

        constexpr float scale_factor() const {
            return static_cast<float>(this->renderer.image_width) / 1280.0f;
        }

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

} // namespace sw::ui
