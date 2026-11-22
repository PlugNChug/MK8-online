// Two linear samples per axis reproduce a [1,2,1]/4 prefilter followed by
// linear interpolation at the original UV, including fractional DRS phases.
vec3 mk8_first_down_axis(float texel) {
    float base = floor(texel - 0.5) + 0.5;
    float q = texel - base;
    float right = 1.0 + 2.0 * q;
    float left = 4.0 - right;
    return vec3(base - (1.0 - q) / left, base + 1.0 + q / right, right * 0.25);
}

vec4 mk8_first_down(sampler2D image, vec2 uv) {
    vec2 size = vec2(textureSize(image, 0));
    vec2 dx = dFdx(uv) * size, dy = dFdy(uv) * size;
    if (!(rscale[1].z > 0.5 && rscale[1].w == 0.0 &&
          abs(dx.x - 2.0) < 0.06 && abs(dy.y - 2.0) < 0.06 && abs(dx.y) + abs(dy.x) < 0.01))
        return texture(image, uv);
    vec3 x = mk8_first_down_axis(uv.x * size.x);
    vec3 y = mk8_first_down_axis(uv.y * size.y);
    return mix(mix(textureLod(image, vec2(x.x, y.x) / size, 0.0),
                   textureLod(image, vec2(x.y, y.x) / size, 0.0), x.z),
               mix(textureLod(image, vec2(x.x, y.y) / size, 0.0),
                   textureLod(image, vec2(x.y, y.y) / size, 0.0), x.z), y.z);
}
#define texture(sampler_, uv_) mk8_first_down(sampler_, uv_)
