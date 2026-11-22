// Fullscreen triangle without vertex buffers. The fragment shader draws the moving test pattern.
#version 450

layout(push_constant) uniform Params {
    // x = surface width, y = surface height, z = scroll phase in pixels, w = ignored
    vec4 info;
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vUV = p;
}
