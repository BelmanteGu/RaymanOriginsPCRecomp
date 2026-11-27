#version 450
// vkbench2: cell (gl_VertexIndex / 6) of a grid x grid cut of full-screen layers; push: grid, texture scale, first layer
layout(location = 0) out vec4 color;
layout(location = 1) out vec4 uv;
layout(push_constant) uniform P { vec4 p; } pc;
void main() {
    int grid = int(pc.p.x), cells = grid * grid;
    int idx = gl_VertexIndex / 6, corner = gl_VertexIndex % 6;
    float layer = pc.p.z + float(idx / cells);
    int cell = idx % cells;
    const vec2 q[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 s = (vec2(cell % grid, cell / grid) + q[corner]) / float(grid);
    gl_Position = vec4(s * 2.0 - 1.0, 0.0, 1.0);
    color = vec4(1.0, 0.9, 0.8, 0.35 + 0.02 * layer);
    uv = vec4(s * pc.p.y + layer * 0.137, 0.0, 0.0);
}
