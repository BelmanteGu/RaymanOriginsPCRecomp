#version 450
layout(location = 0) out vec4 color;
layout(location = 1) out vec4 uv;
layout(push_constant) uniform P { float layer; } p;
void main() {
    vec2 q = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);
    color = vec4(1.0, 0.9, 0.8, 0.35 + 0.02 * p.layer);
    uv = vec4(q * 3.0 + p.layer * 0.13, 0.0, 0.0);
}
