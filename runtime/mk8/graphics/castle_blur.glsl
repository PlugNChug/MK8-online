layout(std140, set = 1, binding = 18) uniform CastleBlurScale { vec4 pyramidScale[65]; };

// The stock shader hardcodes one near lobe and reads the other from CB1.
// Only these symmetric weights have the unit gain used by the dense kernel.
bool mk8_blur_kernel(uint farBits, uint centerBits, uint nearBits) {
    return farBits == 0x3d8fe9dcu && centerBits == 0x3e6879c6u && nearBits == 0x3ea1e718u;
}

bool mk8_scaled_blur() {
    const bool horizontal = HORIZONTAL;
    float mip = pyramidScale[1].w;
    if (pyramidScale[1].z <= 0.5 || int(mip) >= textureQueryLevels(t0)) return false;
    float cell = abs(v0.z - v0.w) / 1.3846;
    ivec2 extent = textureSize(t0, int(mip));
    float footprint = cell * float(horizontal ? extent.x : extent.y);
    if (!(footprint > 1.05 && footprint <= 4.1)) return false;

    // Expand the two pairs of bilinear taps into their nine guest weights.
    if (!mk8_blur_kernel(cb1[0].x, cb1[0].y, cb1[0].z)) return false;
    float farGuest = abs(v1.z - v0.w) / cell;
    // The guest supplies 3.23077. Allow interpolation/subtraction roundoff.
    if (!(abs(farGuest - 3.23077) <= 0.01)) return false;
    float nearW = as_f(cb1[0][2]), farW = as_f(cb1[0][0]), centerW = as_f(cb1[0][1]);
    float weights[5] = float[5](centerW, nearW * (2.0 - 1.3846), nearW * (1.3846 - 1.0),
                               farW * (4.0 - farGuest), farW * (farGuest - 3.0));
    vec2 uv = horizontal ? vec2(v0.w, v0.x) : vec2(v0.x, v0.w);
    vec2 axis = horizontal ? vec2(cell, 0) : vec2(0, cell);
    int samples = int(clamp(round(footprint), 2.0, 4.0));
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int k = -4; k <= 4; k++) {
        float weight = weights[abs(k)];
        for (int i = 0; i < samples; i++) {
            float within = (float(i) + 0.5) / float(samples) - 0.5;
            sum += weight * textureLod(t0, uv + axis * (float(k) + within), mip);
            total += weight;
        }
    }
    c0 = sum / total;
    if (ltint[0].w > 0.0) {
        float l = dot(c0.rgb, vec3(.299, .587, .114));
        c0.rgb = mix(c0.rgb, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w);
    }
    return true;
}
