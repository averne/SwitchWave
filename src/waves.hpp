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

#include <array>

#include <deko3d.hpp>

#include "render.hpp"

namespace sw::ui {

class Waves {
    public:
        Waves(Renderer &renderer);

        void render();

    private:
        struct WaveParams {
            float amplitude, period, phase, offset;
        };

        struct UniformBuffer {
            std::uint64_t counter, timestamp;
            std::uint64_t timestamp_start;
            float alpha, padding;
            std::array<WaveParams, 5> wave_params;
        };

    private:
        Renderer &renderer;

        dk::UniqueMemBlock memblock;
        dk::Shader shaders[2];
        DkCmdList cmdlist;
        std::size_t uniform_offset;
};

} // namespace sw::ui
