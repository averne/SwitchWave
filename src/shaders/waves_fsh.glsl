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

#version 460

struct WaveParams {
    float amplitude, freq, phase, offset;
};

layout (location = 0) in  vec2 in_pos;
layout (location = 0) out vec4 out_col;

layout (std140, binding = 0) uniform FragUBO {
    uint counter_lo, counter_hi, timestamp_lo, timestamp_hi;
    uint timestamp_start_lo, timestamp_start_hi;
    float alpha;
    WaveParams wave_params[5];
} ubo;

float timestamp_to_s(uint timestamp) {
    return float(timestamp * 625.0 / 384.0e9);
}

#define SMOOTHSTEP_HEIGHT 0.5

float draw_wave(WaveParams p, float delta) {
    float w = p.amplitude * sin(p.freq * in_pos.x + p.phase * delta) + p.offset;
    if (in_pos.y <= w)
        return smoothstep(w - SMOOTHSTEP_HEIGHT, w, in_pos.y);
    else
        return 0.0;
}

void main() {
    float delta = timestamp_to_s(ubo.timestamp_lo);

    float val = 0;
    val = max(val, mix(val, draw_wave(ubo.wave_params[0], delta), 0.5));
    val = max(val, mix(val, draw_wave(ubo.wave_params[1], delta), 0.5));
    val = max(val, mix(val, draw_wave(ubo.wave_params[2], delta), 0.5));
    val = max(val, mix(val, draw_wave(ubo.wave_params[3], delta), 0.5));
    val = max(val, mix(val, draw_wave(ubo.wave_params[4], delta), 0.5));
    val *= ubo.alpha;

    out_col = vec4(val, val, val, 1.0);
}
