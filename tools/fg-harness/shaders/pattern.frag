// Moving pattern for the frame-generation harness.
//
// Vertical bars scroll horizontally by (phase) pixels, and the lower half has a different tint. Frame N uses
// phase = N * FG_HARNESS_SHIFT_PIXELS, so a generated frame that interpolates half-way sits half a step
// between two rendered frames and is measurably different from both.
#version 450

layout(push_constant) uniform Params {
    // x = surface width, y = surface height, z = scroll phase in pixels, w = ignored
    vec4 info;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    float x = gl_FragCoord.x + pc.info.z;
    float bar = step(0.5, fract(x / 48.0));

    vec3 dark = vec3(0.05, 0.10, 0.30);
    vec3 warm = vec3(0.95, 0.55, 0.10);
    vec3 color = mix(dark, warm, bar);

    // A second, slower horizontal band separates the top and bottom halves.
    float band = step(0.5, fract((gl_FragCoord.y + 0.25 * pc.info.z) / 160.0));
    color = mix(color, vec3(0.10, 0.80, 0.30), band * 0.25);

    // Corner marker so a readback can prove the image is not a uniform clear.
    float marker = step(pc.info.x - 48.0, gl_FragCoord.x) * step(pc.info.y - 48.0, gl_FragCoord.y);
    color = mix(color, vec3(1.0, 0.0, 0.0), marker);

    outColor = vec4(color, 1.0);
}
