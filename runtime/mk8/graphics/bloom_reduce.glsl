// Combine four adjacent texels into two linear lookups along each axis.
// At an odd reduction size, a single bilinear lookup misses part of the footprint.
vec3 mk8_reduce_axis(float center, float width) {
    float left = center - 0.5 * width;
    float first = floor(left);
    float end = left + width;
    float leftWeight = min(first + 2.0, end) - left;
    float rightWeight = width - leftWeight;
    float leftCenter = first + 0.5 + clamp(end - first - 1.0, 0.0, 1.0) / leftWeight;
    float rightCenter = first + 2.5 + clamp(end - first - 3.0, 0.0, 1.0) / max(rightWeight, 0.00001);
    return vec3(leftCenter, rightCenter, rightWeight / width);
}

vec4 mk8_reduce_sample(sampler2D image, vec2 uv) {
    vec2 size = vec2(textureSize(image, 0));
    vec2 dx = dFdx(uv) * size, dy = dFdy(uv) * size;
    vec2 stepSize = vec2(dx.x, dy.y);
    if (rscale[1].z < 0.5 || textureQueryLevels(image) != 1 ||
        abs(dx.y) + abs(dy.x) > 0.001 || any(lessThan(stepSize, vec2(1.97))) ||
        any(greaterThan(stepSize, vec2(2.03)))) return texture(image, uv);

    // Recover the integral viewport: float derivative noise at exactly 2:1
    // must not change how normal even-sized reductions are rendered.
    vec2 dst = round(size / stepSize);
    vec2 width = size / dst;
    if (all(equal(size, 2.0 * dst)) ||
        any(greaterThan(abs(uv * size - gl_FragCoord.xy * width), vec2(0.02))))
        return texture(image, uv);
    vec3 x = mk8_reduce_axis(uv.x * size.x, width.x);
    vec3 y = mk8_reduce_axis(uv.y * size.y, width.y);
    return mix(mix(textureLod(image, vec2(x.x, y.x) / size, 0.0),
                   textureLod(image, vec2(x.y, y.x) / size, 0.0), x.z),
               mix(textureLod(image, vec2(x.x, y.y) / size, 0.0),
                   textureLod(image, vec2(x.y, y.y) / size, 0.0), x.z), y.z);
}
#define texture(sampler_, uv_) mk8_reduce_sample(sampler_, uv_)
