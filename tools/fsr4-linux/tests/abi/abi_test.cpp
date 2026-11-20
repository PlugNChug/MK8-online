// Byte-compares the recovered FSR 4.1.2 ABI computation against reference traces.
//
// Usage:
//   fsr4_abi_test <trace-dir> <renderW> <renderH> <outW> <outH> <flags> <sharpen> <sharpness>
//
// Reads tensor.bin / constants.bin / spd.bin / rcas.bin from <trace-dir> when
// present and reports whether the computed blocks match exactly.
//
// SPDX-License-Identifier: GPL-3.0-only

#include "abi/fsr4_412_abi.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size) in.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

bool compare(const char* label, const uint8_t* computed, const std::vector<uint8_t>& expected) {
    if (expected.empty()) {
        std::printf("%-12s SKIP (no reference)\n", label);
        return true;
    }
    if (std::memcmp(computed, expected.data(), expected.size()) == 0) {
        std::printf("%-12s OK (%zu bytes)\n", label, expected.size());
        return true;
    }
    std::printf("%-12s MISMATCH (%zu bytes)\n", label, expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        if (computed[i] != expected[i]) {
            std::printf("  first diff at %zu: computed 0x%02x expected 0x%02x\n", i,
                        computed[i], expected[i]);
            break;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 9) {
        std::fprintf(stderr,
                     "usage: %s <trace-dir> <renderW> <renderH> <outW> <outH> <flags> "
                     "<sharpen> <sharpness>\n",
                     argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    fsr4_412::DispatchInfo dispatch;
    dispatch.render_w = static_cast<uint32_t>(std::atoi(argv[2]));
    dispatch.render_h = static_cast<uint32_t>(std::atoi(argv[3]));
    dispatch.upscale_w = static_cast<uint32_t>(std::atoi(argv[4]));
    dispatch.upscale_h = static_cast<uint32_t>(std::atoi(argv[5]));
    const uint32_t flags = static_cast<uint32_t>(std::strtoul(argv[6], nullptr, 0));
    dispatch.enable_sharpening = std::atoi(argv[7]) != 0;
    dispatch.sharpness = std::strtof(argv[8], nullptr);
    dispatch.motion_scale_x = static_cast<float>(dispatch.render_w);
    dispatch.motion_scale_y = static_cast<float>(dispatch.render_h);
    dispatch.pre_exposure = 1.0f;

    fsr4_412::ContextInfo context;
    context.max_render_w = dispatch.render_w;
    context.max_render_h = dispatch.render_h;
    context.max_upscale_w = dispatch.upscale_w;
    context.max_upscale_h = dispatch.upscale_h;
    context.flags = flags;
    context.first_dispatch = true;

    const fsr4_412::Dims out{dispatch.upscale_w, dispatch.upscale_h};
    const fsr4_412::Dims cls = fsr4_412::classMaximum(out.width, out.height);

    uint8_t tensor[272];
    uint8_t constants[104];
    fsr4_412::writeTensorSizes(out, tensor);
    fsr4_412::writeOptimizedConstants(context, dispatch, constants);

    uint32_t spd_x = 0, spd_y = 0;
    fsr4_412::spdGroups({dispatch.render_w, dispatch.render_h}, spd_x, spd_y);
    uint8_t spd[32];
    fsr4_412::writeSpdConstants({dispatch.render_w, dispatch.render_h}, spd_x, spd_y,
                                dispatch.pre_exposure, spd);

    uint8_t rcas[32];
    fsr4_412::writeRcasConstants(dispatch.sharpness, dispatch.pre_exposure, rcas);

    std::printf("case %s: %ux%u -> %ux%u flags=0x%x sharpness=%.3f class=%ux%u\n",
                dir.c_str(), dispatch.render_w, dispatch.render_h, dispatch.upscale_w,
                dispatch.upscale_h, flags, dispatch.sharpness, cls.width, cls.height);
    std::printf("scratch    %llu bytes\n",
                static_cast<unsigned long long>(fsr4_412::scratchCapacity(out)));
    std::printf("spd groups %u x %u\n", spd_x, spd_y);

    bool ok = true;
    ok &= compare("tensor", tensor, readFile(dir + "/tensor.bin"));
    ok &= compare("constants", constants, readFile(dir + "/constants.bin"));
    if (flags & fsr4_412::kAutoExposure)
        ok &= compare("spd", spd, readFile(dir + "/spd.bin"));
    if (dispatch.enable_sharpening)
        ok &= compare("rcas", rcas, readFile(dir + "/rcas.bin"));

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
