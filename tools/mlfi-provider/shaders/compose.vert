// MLFI provider compose vertex shader: one fullscreen triangle, no vertex buffer.
//
// This shader is part of the MK8-online MLFI provider, not AMD's effect.  The vendor's final
// 8,980-byte `main` (rw_prediction -> swapchain with watermark/UI composition) is not captured, so
// this pass is our replacement composition (see docs/MLFI-PROVIDER.md).
#version 450

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
