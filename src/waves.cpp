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

#include <cmath>
#include <numbers>
#include <ranges>

#include <deko3d.hpp>

#include <imgui.h>
#include <imgui_deko3d.h>

#include "utils.hpp"

#include "waves.hpp"

namespace sw::ui {

Waves::Waves(Renderer &renderer): renderer(renderer) {
    auto dk = this->renderer.get_device();

    std::size_t offset = 0;
    this->memblock = dk::MemBlockMaker(dk, DK_MEMBLOCK_ALIGNMENT)
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code)
        .create();
    auto *addr = static_cast<std::uint8_t *>(this->memblock.getCpuAddr());

    std::vector<std::uint8_t> code;
    for (auto &&[i, path]: std::ranges::views::enumerate(std::array{ "romfs:/shaders/waves_vsh.dksh", "romfs:/shaders/waves_fsh.dksh" })) {
        utils::read_whole_file(code, path, "rb");

        std::memcpy(addr + offset, code.data(), code.size());

        dk::ShaderMaker(this->memblock, offset)
            .initialize(this->shaders[i]);

        offset += utils::align_up(code.size(), DK_SHADER_CODE_ALIGNMENT);
    }

    offset = utils::align_up(offset, DK_UNIFORM_BUF_ALIGNMENT);
    this->uniform_offset = offset;
    offset += sizeof(UniformBuffer);

    // Since our shader only uses the low dword of the report timestamp,
    // the time period needs to be a divisor of the reset step
    float period = 2*std::numbers::pi * 1e9 / dkTimestampToNs(UINT64_C(1) << 32);
    auto *uniform = reinterpret_cast<UniformBuffer *>(addr + this->uniform_offset);
    *uniform = {
        .timestamp_start = dk.getCurrentTimestamp(),
        .wave_params     = {
            WaveParams{ 0.5f, 1.0f, +1.0f*period, +0.0f },
            WaveParams{ 0.7f, 0.2f, +1.0f*period, +0.1f },
            WaveParams{ 0.1f, 0.7f, -1.0f*period, +0.6f },
            WaveParams{ 0.2f, 0.5f, -2.0f*period, -0.5f },
            WaveParams{ 0.3f, 1.2f, +2.0f*period, -0.2f },
        },
    };

    auto cmdbuf = dk::CmdBufMaker(dk)
        .create();

    offset = utils::align_up(offset, DK_CMDMEM_ALIGNMENT);
    cmdbuf.addMemory(this->memblock, offset, utils::align_down(this->memblock.getSize() - offset, DK_CMDMEM_ALIGNMENT));

    auto rast_state = dk::RasterizerState()
        .setCullMode(DkFace_None);
    auto color_state = dk::ColorState()
        .setBlendEnable(0, false);
    auto color_write_state = dk::ColorWriteState();
    auto depth_state = dk::DepthStencilState()
        .setDepthWriteEnable(false)
        .setDepthTestEnable(false)
        .setStencilTestEnable(false);

    cmdbuf.bindRasterizerState(rast_state);
    cmdbuf.bindColorState(color_state);
    cmdbuf.bindColorWriteState(color_write_state);
    cmdbuf.bindDepthStencilState(depth_state);
    cmdbuf.bindUniformBuffer(DkStage_Fragment, 0, this->memblock.getGpuAddr() + this->uniform_offset, sizeof(UniformBuffer));
    cmdbuf.bindShaders(DkStageFlag_GraphicsMask, { &this->shaders[0], &this->shaders[1] });
    cmdbuf.reportCounter(DkCounter_Timestamp, this->memblock.getGpuAddr() + this->uniform_offset + offsetof(UniformBuffer, counter));
    cmdbuf.draw(DkPrimitive_Quads, 4, 1, 0, 0);

    this->cmdlist = cmdbuf.finishList();
}

void Waves::render() {
    auto *mem     = static_cast<std::uint8_t *>(this->memblock.getCpuAddr()) + this->uniform_offset;
    auto *uniform = reinterpret_cast<UniformBuffer *>(mem);

    if (uniform->timestamp)
        uniform->alpha = std::clamp(dkTimestampToNs(uniform->timestamp - uniform->timestamp_start) / 1.5e9f - 0.2f, 0.0f, 1.0f);

    auto queue = this->renderer.get_queue();
    queue.submitCommands(this->cmdlist);
}

} // namespace sw::ui
