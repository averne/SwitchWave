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

const vec4 positions[4] = vec4[](
    vec4(-1.0, -1.0, 0.0, 1.0),
    vec4(+1.0, -1.0, 0.0, 1.0),
    vec4(+1.0, +1.0, 0.0, 1.0),
    vec4(-1.0, +1.0, 0.0, 1.0)
);

layout (location = 0) out vec2 out_pos;

void main() {
    vec4 pos    = positions[gl_VertexID];
    out_pos     = vec2(pos);
    gl_Position = pos;
}
