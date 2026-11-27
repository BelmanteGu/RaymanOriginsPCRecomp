#version 450
layout(location = 1) in vec4 uv;
layout(binding = 0) uniform sampler2D s0;
layout(location = 0) out vec4 o;
void main() { o = texture(s0, uv.xy); }
