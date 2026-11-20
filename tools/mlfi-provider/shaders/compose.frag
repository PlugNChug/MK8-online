// MLFI provider compose fragment shader: copy `rw_prediction` into the application's output image.
//
// This shader is part of the MK8-online MLFI provider, not AMD's effect.  It exists because the
// vendor's final composition `main` was never dispatched in the capture (MLFI-CAPTURE.md 4.1), so
// the provider has to write outputs[0] itself.  The output is a colour attachment, which keeps
// channel order and sRGB/normalised conversion driver-defined and lets the pass write alpha = 1.
#version 450

layout(set = 0, binding = 0) uniform sampler2D prediction;

layout(location = 0) out vec4 color;

void main()
{
    vec4 p = texelFetch(prediction, ivec2(gl_FragCoord.xy), 0);
    color = vec4(clamp(p.rgb, 0.0, 1.0), 1.0);
}
