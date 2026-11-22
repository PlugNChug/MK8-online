// SPDX-License-Identifier: GPL-3.0-only

#include "abi/fsr4_412_abi.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fsr4_412 {
namespace {

struct ClassEntry {
    uint32_t max_w;
    uint32_t max_h;
};

constexpr ClassEntry kClasses[] = {
    {1920, 1080},
    {3840, 2160},
    {7680, 4320},
};

// Decoder/encoder pyramid, recovered from the DLL writer at RVA 0xbfc0.
constexpr float kTensorScale[17] = {
    0.5f, 1.0f, 0.5f, 0.5f, 0.25f, 0.25f, 0.25f, 0.125f, 0.125f,
    0.125f, 0.25f, 0.25f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f,
};

uint32_t roundUp8(uint32_t value) { return (value + 7u) / 8u * 8u; }

const ClassEntry& classFor(uint32_t out_w, uint32_t out_h) {
    for (const ClassEntry& entry : kClasses) {
        if (out_w <= entry.max_w && out_h <= entry.max_h) return entry;
    }
    return kClasses[2];
}

} // namespace

Dims classMaximum(uint32_t out_w, uint32_t out_h) {
    const ClassEntry& entry = classFor(out_w, out_h);
    return {entry.max_w, entry.max_h};
}

void writeTensorSizes(Dims upscale, uint8_t out[272]) {
    const uint32_t width = roundUp8(upscale.width);
    const uint32_t height = roundUp8(upscale.height);
    std::memcpy(out, nullptr, 0); // silence unused warnings; buffer filled below
    uint32_t* entries = reinterpret_cast<uint32_t*>(out);
    for (int i = 0; i < 17; ++i) {
        entries[i * 4 + 0] = static_cast<uint32_t>(static_cast<float>(width) * kTensorScale[i]);
        entries[i * 4 + 1] = static_cast<uint32_t>(static_cast<float>(height) * kTensorScale[i]);
        entries[i * 4 + 2] = 0;
        entries[i * 4 + 3] = 0;
    }
}

uint64_t scratchCapacity(Dims upscale) {
    const Dims max = classMaximum(upscale.width, upscale.height);
    const uint64_t s1 = 4ull * (max.width + 4ull) * (max.height + 4ull);
    const uint64_t s2 = 2ull * (max.width + 8ull) * (max.height + 8ull);
    return 2ull * s1 + s2;
}

uint16_t floatToHalf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;

    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        const int32_t shift = 14 - exponent;
        uint32_t half_mantissa = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (half_mantissa & 1u)))
            ++half_mantissa;
        return static_cast<uint16_t>(sign | half_mantissa);
    }
    if (exponent >= 31) {
        if (mantissa == 0) return static_cast<uint16_t>(sign | 0x7c00u);
        return static_cast<uint16_t>(sign | 0x7c00u | (mantissa >> 13) | 1u);
    }

    // AMD's RCAS constant truncates the mantissa (0.87055 -> 0x3AF6, not the
    // round-to-nearest 0x3AF7), so match that here.
    const uint32_t half_mantissa = mantissa >> 13;
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) |
                                 half_mantissa);
}

void writeOptimizedConstants(const ContextInfo& context, const DispatchInfo& dispatch,
                             uint8_t out[104]) {
    const uint32_t out_w = dispatch.upscale_w;
    const uint32_t out_h = dispatch.upscale_h;
    const bool display_mv = (context.flags & kDisplayResolutionMotionVectors) != 0;
    const float motion_w = static_cast<float>(display_mv ? out_w : dispatch.render_w);
    const float motion_h = static_cast<float>(display_mv ? out_h : dispatch.render_h);

    OptimizedConstants c{};
    c.inv_size[0] = 1.0f / static_cast<float>(out_w);
    c.inv_size[1] = 1.0f / static_cast<float>(out_h);
    c.scale[0] = static_cast<float>(out_w) / static_cast<float>(dispatch.render_w);
    c.scale[1] = static_cast<float>(out_h) / static_cast<float>(dispatch.render_h);
    c.inv_scale[0] = 1.0f / c.scale[0];
    c.inv_scale[1] = 1.0f / c.scale[1];
    c.jitter[0] = dispatch.jitter_x;
    c.jitter[1] = dispatch.jitter_y;
    c.mv_scale[0] = dispatch.motion_scale_x / motion_w;
    c.mv_scale[1] = dispatch.motion_scale_y / motion_h;
    c.tex_size[0] = static_cast<float>(context.max_upscale_w);
    c.tex_size[1] = static_cast<float>(context.max_upscale_h);
    c.max_render_size[0] = static_cast<float>(context.max_render_w);
    c.max_render_size[1] = static_cast<float>(context.max_render_h);

    if (context.flags & kMotionVectorsJitterCancellation) {
        const float prev_x =
            context.has_previous_jitter ? context.previous_jitter[0] : dispatch.jitter_x;
        const float prev_y =
            context.has_previous_jitter ? context.previous_jitter[1] : dispatch.jitter_y;
        c.motion_vector_jitter_cancel[0] = (prev_x - dispatch.jitter_x) / motion_w;
        c.motion_vector_jitter_cancel[1] = (prev_y - dispatch.jitter_y) / motion_h;
    }

    c.width = static_cast<int32_t>(out_w);
    c.height = static_cast<int32_t>(out_h);
    c.reset = (dispatch.reset || context.first_dispatch) ? 1 : 0;
    c.width_lr = static_cast<int32_t>(dispatch.render_w);
    c.height_lr = static_cast<int32_t>(dispatch.render_h);
    c.pre_exposure = dispatch.pre_exposure;
    c.previous_pre_exposure =
        context.has_previous_exposure ? context.previous_exposure : 0.0f;
    c.rcas_enabled = dispatch.enable_sharpening ? 1 : 0;
    c.rcas_sharpness = dispatch.sharpness;
    c.pad = 0.0f;

    std::memcpy(out, &c, sizeof(c));
}

void spdGroups(Dims render, uint32_t& groups_x, uint32_t& groups_y) {
    groups_x = (render.width + 63u) / 64u;
    groups_y = (render.height + 63u) / 64u;
}

void writeSpdConstants(Dims render, uint32_t groups_x, uint32_t groups_y,
                       float pre_exposure, uint8_t out[32]) {
    SpdConstants c{};
    const uint32_t max_dim = render.width > render.height ? render.width : render.height;
    uint32_t mips = 0;
    while ((1u << (mips + 1)) <= max_dim && mips < 20) ++mips;
    // floor(log2(max_dim)) matches the observed values (960 -> 9, 1280 -> 10).
    c.mips = mips;
    c.num_workgroups = groups_x * groups_y;
    c.workgroup_offset[0] = 0;
    c.workgroup_offset[1] = 0;
    c.inv_input_size[0] = 1.0f / static_cast<float>(render.width);
    c.inv_input_size[1] = 1.0f / static_cast<float>(render.height);
    c.pre_exposure = pre_exposure;
    c.pad = 0.0f;
    std::memcpy(out, &c, sizeof(c));
}

void writeRcasConstants(float sharpness, float pre_exposure, uint8_t out[32]) {
    RcasConstants c{};
    const float factor = std::exp2(2.0f * sharpness - 2.0f);
    std::memcpy(&c.config[0], &factor, sizeof(factor));
    const uint16_t half = floatToHalf(factor);
    c.config[1] = (static_cast<uint32_t>(half) << 16) | half;
    c.config[2] = 0;
    c.config[3] = 0;
    c.pre_exposure = pre_exposure;
    c.pad[0] = c.pad[1] = c.pad[2] = 0.0f;
    std::memcpy(out, &c, sizeof(c));
}

void rcasGroups(Dims upscale, uint32_t& groups_x, uint32_t& groups_y) {
    groups_x = (upscale.width + 15u) / 16u;
    groups_y = (upscale.height + 15u) / 16u;
}

DispatchGrid modelPassDispatch(uint32_t pass_index, Dims upscale, Dims render,
                               const uint8_t tensor[272]) {
    auto dims = [&](uint32_t slot) {
        return Dims{*reinterpret_cast<const uint32_t*>(tensor + slot * 16),
                    *reinterpret_cast<const uint32_t*>(tensor + slot * 16 + 4)};
    };
    auto ceil_div = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
    DispatchGrid g;

    if (pass_index == 0) { // prepass
        const Dims t0 = dims(0);
        g.x = ceil_div(t0.width, 32);
        g.y = t0.height;
        return g;
    }
    if (pass_index == 26) { // postpass
        g.x = ceil_div(upscale.width, 64);
        g.y = ceil_div(upscale.height, 2);
        return g;
    }
    if (pass_index % 2 == 0) { // model pass N -> tensor N+1
        const uint32_t n = pass_index / 2;
        const Dims t = dims(n + 1);
        switch (n) {
            case 1: case 2: case 12:
                g.x = ceil_div(t.width, 16); g.y = ceil_div(t.height, 8); break;
            case 3: case 6:
                g.x = ceil_div(t.width, 32); g.y = t.height; break;
            case 4: case 5: case 10:
                g.x = ceil_div(t.width, 16); g.y = ceil_div(t.height, 4); break;
            case 7: case 8:
                g.x = ceil_div(t.width, 16); g.y = ceil_div(t.height, 2); break;
            case 9:
                g.x = ceil_div(t.width, 32); g.y = ceil_div(t.height, 2); break;
            case 11:
                g.x = ceil_div(t.width, 32); g.y = t.height; break;
            default:
                g.x = ceil_div(t.width, 64); g.y = t.height; break;
        }
        return g;
    }

    // Padding pass N = (pass_index - 1) / 2.
    const uint32_t n = (pass_index - 1) / 2;
    static const uint32_t divisors[13] = {2, 2, 2, 4, 4, 4, 8, 8, 8, 4, 4, 2, 2};
    const Dims cls = classMaximum(upscale.width, upscale.height);
    const Dims t = dims(n == 0 ? 0 : n + 1);
    uint32_t bucket_w = cls.width / divisors[n];
    const uint32_t bucket_h = cls.height / divisors[n];
    if (n >= 6 && n <= 8) bucket_w = (bucket_w + 31u) / 32u * 32u;
    const uint32_t pw = std::min(bucket_w + 1u - t.width, 33u);
    const uint32_t ph = std::min(bucket_h + 1u - t.height, 1u);
    const uint64_t elements = static_cast<uint64_t>(t.width + pw + 1) * (t.height + ph + 1) -
                              static_cast<uint64_t>(t.width) * t.height;
    g.x = static_cast<uint32_t>((elements + 31u) / 32u);
    g.y = 1;
    return g;
}

} // namespace fsr4_412
