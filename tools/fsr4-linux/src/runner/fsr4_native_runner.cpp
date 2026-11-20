// Native Linux runner for one FSR 4.1.2 case.
//
// Runs the exact pass sequence captured from AMD's signed Vulkan DLL (see
// tests/reference/fsr4_trace.c) against the SPIR-V extracted from that DLL, and
// writes the upscaled image as raw RGBA8 for comparison with the reference.
//
// The manifest (case.txt) is produced by tests/reference/prepare_case.py:
//
//   render 960 540
//   output 1920 1080
//   scratch 20880256
//   weights  /path/FP8_standard.bin
//   tensor   /path/tensor.bin
//   constants /path/constants.bin
//   color    /path/in_color.rgba8
//   depth    /path/in_depth.f32
//   motion   /path/in_mv.rg16f
//   spdfile  /path/spd.bin            (optional, auto exposure)
//   rcasfile /path/rcas.bin           (optional, sharpening)
//   spd  <gx> <gy> <gz> <spv> <entry> (optional, first pass)
//   pass <name> <gx> <gy> <gz> <spv> <entry>
//   rcas <gx> <gy> <gz> <spv> <entry> (optional, last pass)
//
// SPDX-License-Identifier: GPL-3.0-only

#include <vulkan/vulkan.h>

#include "abi/fsr4_412_abi.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define VK_CHECK(x)                                                                  \
    do {                                                                             \
        VkResult r_ = (x);                                                           \
        if (r_ != VK_SUCCESS) {                                                      \
            std::fprintf(stderr, "VK_FAIL %s = %d (%s:%d)\n", #x, (int)r_, __FILE__, \
                         __LINE__);                                                  \
            std::exit(1);                                                            \
        }                                                                            \
    } while (0)

namespace {

enum PassKind { kModel = 0, kSpd = 1, kRcas = 2 };

struct Pass {
    std::string name;
    std::string spv_path;
    std::string entry;
    uint32_t groups[3] = {1, 1, 1};
    int kind = kModel;
};

struct Case {
    uint32_t render_w = 0, render_h = 0;
    uint32_t output_w = 0, output_h = 0;
    uint64_t scratch_size = 0;
    std::string weights_path, tensor_path, constants_path;
    std::string color_path, depth_path, motion_path;
    std::string spd_path, rcas_path;
    uint32_t flags = 0;
    int sharpen = 0;
    float sharpness = 0.0f;
    std::vector<Pass> passes;
};

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open " + path);
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size && !in.read(reinterpret_cast<char*>(data.data()), size))
        throw std::runtime_error("cannot read " + path);
    return data;
}

std::string dirName(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

std::string joinPath(const std::string& dir, const std::string& path) {
    if (!path.empty() && (path[0] == '/' || (path.size() > 1 && path[1] == ':')))
        return path;
    return dir + "/" + path;
}

Case loadCase(const std::string& manifest_path) {
    std::ifstream in(manifest_path);
    if (!in) throw std::runtime_error("cannot open manifest " + manifest_path);
    const std::string dir = dirName(manifest_path);
    Case c;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "render") {
            ss >> c.render_w >> c.render_h;
        } else if (key == "output") {
            ss >> c.output_w >> c.output_h;
        } else if (key == "scratch") {
            ss >> c.scratch_size;
        } else if (key == "weights") {
            std::string p; ss >> p; c.weights_path = joinPath(dir, p);
        } else if (key == "tensor") {
            std::string p; ss >> p; c.tensor_path = joinPath(dir, p);
        } else if (key == "constants") {
            std::string p; ss >> p; c.constants_path = joinPath(dir, p);
        } else if (key == "color") {
            std::string p; ss >> p; c.color_path = joinPath(dir, p);
        } else if (key == "depth") {
            std::string p; ss >> p; c.depth_path = joinPath(dir, p);
        } else if (key == "motion") {
            std::string p; ss >> p; c.motion_path = joinPath(dir, p);
        } else if (key == "flags") {
            std::string v; ss >> v; c.flags = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
        } else if (key == "sharpen") {
            ss >> c.sharpen;
        } else if (key == "sharpness") {
            ss >> c.sharpness;
        } else if (key == "spdfile") {
            std::string p; ss >> p; c.spd_path = joinPath(dir, p);
        } else if (key == "rcasfile") {
            std::string p; ss >> p; c.rcas_path = joinPath(dir, p);
        } else if (key == "spd" || key == "rcas" || key == "pass") {
            Pass p;
            if (key == "pass") {
                ss >> p.name >> p.groups[0] >> p.groups[1] >> p.groups[2];
                p.kind = kModel;
            } else if (key == "spd") {
                p.name = "spd";
                p.kind = kSpd;
                ss >> p.groups[0] >> p.groups[1] >> p.groups[2];
            } else {
                p.name = "rcas";
                p.kind = kRcas;
                ss >> p.groups[0] >> p.groups[1] >> p.groups[2];
            }
            std::string spv;
            ss >> spv;
            p.spv_path = joinPath(dir, spv);
            ss >> p.entry;
            if (!ss) throw std::runtime_error("bad pass line: " + line);
            c.passes.push_back(p);
        }
    }
    if (!c.output_w || !c.output_h || c.passes.empty())
        throw std::runtime_error("incomplete manifest " + manifest_path);
    return c;
}

// ---------------------------------------------------------------------------
// Vulkan helpers
// ---------------------------------------------------------------------------
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct DeviceState {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkPhysicalDeviceMemoryProperties memory_properties{};
};

uint32_t findMemoryType(const DeviceState& d, uint32_t type_bits,
                        VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred = 0) {
    for (uint32_t i = 0; i < d.memory_properties.memoryTypeCount; ++i) {
        if (!(type_bits & (1u << i))) continue;
        const auto flags = d.memory_properties.memoryTypes[i].propertyFlags;
        if ((flags & required) == required && (flags & preferred) == preferred) return i;
    }
    for (uint32_t i = 0; i < d.memory_properties.memoryTypeCount; ++i) {
        if (!(type_bits & (1u << i))) continue;
        const auto flags = d.memory_properties.memoryTypes[i].propertyFlags;
        if ((flags & required) == required) return i;
    }
    throw std::runtime_error("no suitable memory type");
}

Buffer createBuffer(const DeviceState& d, VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(d.device, &ci, nullptr, &b.buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(d.device, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(d, req.memoryTypeBits, properties);
    VK_CHECK(vkAllocateMemory(d.device, &ai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(d.device, b.buffer, b.memory, 0));
    return b;
}

Image createImage(const DeviceState& d, VkFormat format, uint32_t width, uint32_t height,
                  VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Image img;
    img.format = format;
    img.width = width;
    img.height = height;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(d.device, &ci, nullptr, &img.image));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(d.device, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(d, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(d.device, &ai, nullptr, &img.memory));
    VK_CHECK(vkBindImageMemory(d.device, img.image, img.memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(d.device, &vi, nullptr, &img.view));
    return img;
}

void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout old_layout,
                  VkImageLayout new_layout, VkAccessFlags src, VkAccessFlags dst,
                  VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                  VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void computeBarrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

std::vector<uint32_t> spirvWords(const std::vector<uint8_t>& bytes) {
    if (bytes.size() % 4) throw std::runtime_error("SPIR-V size not a multiple of 4");
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

VkDevice createDevice(DeviceState& d) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(d.instance, &count, nullptr);
    if (!count) throw std::runtime_error("no Vulkan physical devices");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(d.instance, &count, devices.data());
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            d.physical = candidate;
            break;
        }
    }
    if (!d.physical) d.physical = devices[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(d.physical, &props);
    std::printf("GPU: %s (api %u.%u.%u)\n", props.deviceName,
                VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                VK_VERSION_PATCH(props.apiVersion));

    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceCooperativeMatrix2FeaturesNV cm2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV};
    core.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    f13.pNext = &cm;
    cm.pNext = &f8;
    f8.pNext = &cm2;
    vkGetPhysicalDeviceFeatures2(d.physical, &core);

    std::vector<const char*> extensions;
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(d.physical, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> available(ext_count);
    vkEnumerateDeviceExtensionProperties(d.physical, nullptr, &ext_count, available.data());
    auto has_extension = [&](const char* name) {
        for (const auto& e : available)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    const char* wanted[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
                            VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
                            VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME};
    for (const char* name : wanted) {
        if (!has_extension(name)) {
            std::printf("note: device extension %s unavailable, continuing\n", name);
            continue;
        }
        extensions.push_back(name);
    }
    std::printf("enabled %zu device extensions\n", extensions.size());
    if (!cm.cooperativeMatrix) throw std::runtime_error("cooperativeMatrix unsupported");
    if (!f8.shaderFloat8 || !f8.shaderFloat8CooperativeMatrix)
        throw std::runtime_error("shaderFloat8 unsupported");

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &family_count, families.data());
    bool found = false;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            d.queue_family = i;
            found = true;
            break;
        }
    }
    if (!found) throw std::runtime_error("no compute queue");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = d.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    dci.pNext = &core;
    VK_CHECK(vkCreateDevice(d.physical, &dci, nullptr, &d.device));
    vkGetDeviceQueue(d.device, d.queue_family, 0, &d.queue);
    vkGetPhysicalDeviceMemoryProperties(d.physical, &d.memory_properties);
    return d.device;
}

void writeFile(const std::string& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

VkDescriptorSetLayout createLayout(const DeviceState& d,
                                   const std::vector<VkDescriptorSetLayoutBinding>& binds) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = static_cast<uint32_t>(binds.size());
    ci.pBindings = binds.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(d.device, &ci, nullptr, &layout));
    return layout;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string manifest = argc > 1 ? argv[1] : "case.txt";
        Case c = loadCase(manifest);
        bool has_spd = false, has_rcas = false;
        for (const Pass& p : c.passes) {
            if (p.kind == kSpd) has_spd = true;
            if (p.kind == kRcas) has_rcas = true;
        }
        std::printf("case: %ux%u -> %ux%u, %zu passes, spd=%d rcas=%d scratch=%llu\n",
                    c.render_w, c.render_h, c.output_w, c.output_h, c.passes.size(),
                    (int)has_spd, (int)has_rcas, (unsigned long long)c.scratch_size);

        const std::vector<uint8_t> weights = readFile(c.weights_path);
        const std::vector<uint8_t> color_bytes = readFile(c.color_path);
        const std::vector<uint8_t> depth_bytes = readFile(c.depth_path);
        const std::vector<uint8_t> motion_bytes = readFile(c.motion_path);
        const bool compute_abi = getenv("FSR4_RUNNER_COMPUTE_ABI") != NULL;
        std::vector<uint8_t> tensor, constants, spd_constants, rcas_constants;
        if (compute_abi) {
            tensor.resize(272);
            constants.resize(104);
            fsr4_412::DispatchInfo di;
            di.render_w = c.render_w;
            di.render_h = c.render_h;
            di.upscale_w = c.output_w;
            di.upscale_h = c.output_h;
            di.motion_scale_x = static_cast<float>(c.render_w);
            di.motion_scale_y = static_cast<float>(c.render_h);
            di.pre_exposure = 1.0f;
            di.enable_sharpening = c.sharpen != 0;
            di.sharpness = c.sharpness;
            fsr4_412::ContextInfo ci;
            ci.max_render_w = c.render_w;
            ci.max_render_h = c.render_h;
            ci.max_upscale_w = c.output_w;
            ci.max_upscale_h = c.output_h;
            ci.flags = c.flags;
            ci.first_dispatch = true;
            fsr4_412::writeTensorSizes({c.output_w, c.output_h}, tensor.data());
            fsr4_412::writeOptimizedConstants(ci, di, constants.data());
            uint32_t model_index = 0;
            for (Pass& p : c.passes) {
                if (p.kind == kModel) {
                    const auto grid = fsr4_412::modelPassDispatch(
                        model_index++, {c.output_w, c.output_h}, {c.render_w, c.render_h},
                        tensor.data());
                    p.groups[0] = grid.x;
                    p.groups[1] = grid.y;
                    p.groups[2] = grid.z;
                } else if (p.kind == kRcas) {
                    uint32_t gx = 0, gy = 0;
                    fsr4_412::rcasGroups({c.output_w, c.output_h}, gx, gy);
                    p.groups[0] = gx;
                    p.groups[1] = gy;
                    p.groups[2] = 1;
                }
            }
            if (has_spd) {
                spd_constants.resize(32);
                uint32_t gx = 0, gy = 0;
                fsr4_412::spdGroups({c.render_w, c.render_h}, gx, gy);
                fsr4_412::writeSpdConstants({c.render_w, c.render_h}, gx, gy,
                                            di.pre_exposure, spd_constants.data());
                for (Pass& p : c.passes) {
                    if (p.kind != kSpd) continue;
                    p.groups[0] = (c.render_w + 63) / 64;
                    p.groups[1] = (c.render_h + 63) / 64;
                    p.groups[2] = 1;
                }
            }
            if (has_rcas) {
                rcas_constants.resize(32);
                fsr4_412::writeRcasConstants(c.sharpness, di.pre_exposure,
                                             rcas_constants.data());
            }
            std::printf("ABI: computed tensor/constants%s%s\n", has_spd ? "/spd" : "",
                        has_rcas ? "/rcas" : "");
        } else {
            tensor = readFile(c.tensor_path);
            constants = readFile(c.constants_path);
            if (has_spd) {
                if (c.spd_path.empty()) throw std::runtime_error("spd pass without spdfile");
                spd_constants = readFile(c.spd_path);
                if (spd_constants.size() != 32)
                    throw std::runtime_error("spd.bin must be 32 bytes");
            }
            if (has_rcas) {
                if (c.rcas_path.empty()) throw std::runtime_error("rcas pass without rcasfile");
                rcas_constants = readFile(c.rcas_path);
                if (rcas_constants.size() != 32)
                    throw std::runtime_error("rcas.bin must be 32 bytes");
            }
        }
        if (tensor.size() != 272) throw std::runtime_error("tensor.bin must be 272 bytes");
        if (constants.size() != 104)
            throw std::runtime_error("constants.bin must be 104 bytes");

        DeviceState d;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "fsr4-native-runner";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        VK_CHECK(vkCreateInstance(&ici, nullptr, &d.instance));
        createDevice(d);

        const VkImageUsageFlags sampled_storage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const VkImageAspectFlags color_aspect = VK_IMAGE_ASPECT_COLOR_BIT;

        Image history = createImage(d, VK_FORMAT_R16G16B16A16_SFLOAT, c.output_w,
                                    c.output_h, sampled_storage, color_aspect);
        Image reprojected = createImage(d, VK_FORMAT_R16G16B16A16_SFLOAT, c.output_w,
                                        c.output_h, sampled_storage, color_aspect);
        Image recurrent = createImage(d, VK_FORMAT_R8G8B8A8_UNORM, c.output_w, c.output_h,
                                      sampled_storage, color_aspect);
        Image exposure = createImage(d, VK_FORMAT_R32_SFLOAT, 2, 1, sampled_storage,
                                     color_aspect);
        Image dummy_exposure = createImage(
            d, VK_FORMAT_R32G32_SFLOAT, 1, 1,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, color_aspect);
        Image output = createImage(
            d, VK_FORMAT_R8G8B8A8_UNORM, c.output_w, c.output_h,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, color_aspect);
        Image rcas_input{};
        if (has_rcas) {
            rcas_input = createImage(d, VK_FORMAT_R16G16B16A16_SFLOAT, c.output_w,
                                     c.output_h, sampled_storage, color_aspect);
        }
        Image exposure_atomic{}, exposure_mip{};
        if (has_spd) {
            exposure_atomic = createImage(d, VK_FORMAT_R32_UINT, 1, 1, sampled_storage,
                                          color_aspect);
            const uint32_t mip_w = (c.render_w + 15) / 16;
            const uint32_t mip_h = (c.render_h + 15) / 16;
            exposure_mip = createImage(d, VK_FORMAT_R32_SFLOAT, mip_w, mip_h,
                                       sampled_storage, color_aspect);
        }
        Image input_color = createImage(
            d, VK_FORMAT_R8G8B8A8_UNORM, c.render_w, c.render_h,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, color_aspect);
        Image input_depth = createImage(
            d, VK_FORMAT_D32_SFLOAT, c.render_w, c.render_h,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
        Image input_motion = createImage(
            d, VK_FORMAT_R16G16_SFLOAT, c.render_w, c.render_h,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, color_aspect);

        Buffer weight_buffer = createBuffer(
            d, weights.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Buffer scratch_buffer = createBuffer(
            d, c.scratch_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Buffer tensor_buffer = createBuffer(
            d, 272, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Buffer constants_buffer = createBuffer(
            d, 104, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Buffer spd_buffer{}, rcas_buffer{};
        if (has_spd) {
            spd_buffer = createBuffer(
                d, 32, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        if (has_rcas) {
            rcas_buffer = createBuffer(
                d, 32, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        Buffer staging = createBuffer(
            d, static_cast<VkDeviceSize>(c.output_w) * c.output_h * 4,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const bool dump_after = getenv("FSR4_DUMP_AFTER") != NULL;
        Buffer dbg_staging{};
        if (dump_after) {
            dbg_staging = createBuffer(
                d, static_cast<VkDeviceSize>(c.output_w) * c.output_h * 8,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        Buffer upload = createBuffer(
            d, color_bytes.size() + depth_bytes.size() + motion_bytes.size(),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        auto upload_to = [&](Buffer& b, const std::vector<uint8_t>& data) {
            void* ptr = nullptr;
            VK_CHECK(vkMapMemory(d.device, b.memory, 0, b.size, 0, &ptr));
            std::memcpy(ptr, data.data(), data.size());
            vkUnmapMemory(d.device, b.memory);
        };
        upload_to(weight_buffer, weights);
        upload_to(tensor_buffer, tensor);
        upload_to(constants_buffer, constants);
        upload_to(upload, color_bytes);
        {
            void* ptr = nullptr;
            VK_CHECK(vkMapMemory(d.device, upload.memory, 0, upload.size, 0, &ptr));
            std::memcpy(static_cast<uint8_t*>(ptr) + color_bytes.size(), depth_bytes.data(),
                        depth_bytes.size());
            std::memcpy(static_cast<uint8_t*>(ptr) + color_bytes.size() + depth_bytes.size(),
                        motion_bytes.data(), motion_bytes.size());
            vkUnmapMemory(d.device, upload.memory);
        }
        if (has_spd) upload_to(spd_buffer, spd_constants);
        if (has_rcas) upload_to(rcas_buffer, rcas_constants);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 1000.0f;
        VkSampler sampler = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSampler(d.device, &sci, nullptr, &sampler));

        auto binding = [](uint32_t b, VkDescriptorType t) {
            VkDescriptorSetLayoutBinding lb{};
            lb.binding = b;
            lb.descriptorType = t;
            lb.descriptorCount = 1;
            lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            return lb;
        };

        // Model / prepass / postpass / padding.
        std::vector<VkDescriptorSetLayoutBinding> model_binds = {
            binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            binding(129, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            binding(130, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            binding(131, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            binding(134, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            binding(137, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            binding(141, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            binding(256, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            binding(257, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            binding(320, VK_DESCRIPTOR_TYPE_SAMPLER),
        };
        VkDescriptorSetLayout model_layout = createLayout(d, model_binds);

        // Auto-exposure SPD.
        VkDescriptorSetLayout spd_layout = VK_NULL_HANDLE;
        if (has_spd) {
            std::vector<VkDescriptorSetLayoutBinding> binds = {
                binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                binding(128, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                binding(129, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                binding(130, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                binding(256, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                binding(320, VK_DESCRIPTOR_TYPE_SAMPLER),
            };
            spd_layout = createLayout(d, binds);
        }

        // RCAS.
        VkDescriptorSetLayout rcas_layout = VK_NULL_HANDLE;
        if (has_rcas) {
            std::vector<VkDescriptorSetLayoutBinding> binds = {
                binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                binding(11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                binding(17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                binding(18, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                binding(320, VK_DESCRIPTOR_TYPE_SAMPLER),
            };
            rcas_layout = createLayout(d, binds);
        }

        struct LayoutInfo {
            VkDescriptorSetLayout layout;
            VkPipelineLayout pipeline_layout;
        };
        auto make_pipeline_layout = [&](VkDescriptorSetLayout layout) {
            VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &layout;
            VkPipelineLayout out = VK_NULL_HANDLE;
            VK_CHECK(vkCreatePipelineLayout(d.device, &plci, nullptr, &out));
            return out;
        };
        LayoutInfo model_pl{model_layout, make_pipeline_layout(model_layout)};
        LayoutInfo spd_pl{}, rcas_pl{};
        if (has_spd) spd_pl = {spd_layout, make_pipeline_layout(spd_layout)};
        if (has_rcas) rcas_pl = {rcas_layout, make_pipeline_layout(rcas_layout)};

        const uint32_t set_count = 1 + (has_spd ? 1u : 0u) + (has_rcas ? 1u : 0u);
        VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 * set_count},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * set_count},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 7 * set_count},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * set_count},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1 * set_count},
        };
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = set_count;
        dpci.poolSizeCount = 5;
        dpci.pPoolSizes = pool_sizes;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorPool(d.device, &dpci, nullptr, &descriptor_pool));

        auto alloc_set = [&](VkDescriptorSetLayout layout) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = descriptor_pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &layout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            VK_CHECK(vkAllocateDescriptorSets(d.device, &ai, &set));
            return set;
        };
        auto image_info = [](VkImageView view) {
            VkDescriptorImageInfo info{};
            info.imageView = view;
            info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            return info;
        };

        // Model set.
        VkDescriptorSet model_set = alloc_set(model_layout);
        VkDescriptorImageInfo m0 = image_info(history.view);
        VkDescriptorImageInfo m1 = image_info(input_motion.view);
        VkDescriptorImageInfo m2 = image_info(input_depth.view);
        VkDescriptorImageInfo m3 = image_info(input_color.view);
        VkDescriptorImageInfo m4 = image_info(recurrent.view);
        VkDescriptorImageInfo m6 = image_info(dummy_exposure.view);
        VkDescriptorImageInfo m9 = image_info(reprojected.view);
        VkDescriptorImageInfo m17 = image_info(exposure.view);
        VkDescriptorImageInfo m129 = image_info(history.view);
        VkDescriptorImageInfo m130 = image_info(has_rcas ? rcas_input.view : output.view);
        VkDescriptorImageInfo m131 = image_info(reprojected.view);
        VkDescriptorImageInfo m134 = image_info(recurrent.view);
        VkDescriptorImageInfo m137 = image_info(exposure.view);
        VkDescriptorBufferInfo weight_info{weight_buffer.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo scratch_info{scratch_buffer.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo tensor_info{tensor_buffer.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo constants_info{constants_buffer.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo sampler_info{};
        sampler_info.sampler = sampler;

        std::vector<VkWriteDescriptorSet> writes;
        auto write_image = [&](VkDescriptorSet set, uint32_t b, const VkDescriptorImageInfo* info,
                               VkDescriptorType type) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = b;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pImageInfo = info;
            writes.push_back(w);
        };
        auto write_buffer = [&](VkDescriptorSet set, uint32_t b,
                                const VkDescriptorBufferInfo* info, VkDescriptorType type) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = b;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pBufferInfo = info;
            writes.push_back(w);
        };
        write_image(model_set, 0, &m0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(model_set, 1, &m1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(model_set, 2, &m2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(model_set, 3, &m3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(model_set, 4, &m4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(model_set, 6, &m6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(model_set, 9, &m9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(model_set, 17, &m17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_buffer(model_set, 21, &weight_info, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        write_image(model_set, 129, &m129, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write_image(model_set, 130, &m130, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write_image(model_set, 131, &m131, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write_image(model_set, 134, &m134, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write_image(model_set, 137, &m137, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write_buffer(model_set, 141, &scratch_info, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        write_buffer(model_set, 256, &tensor_info, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        write_buffer(model_set, 257, &constants_info, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        write_image(model_set, 320, &sampler_info, VK_DESCRIPTOR_TYPE_SAMPLER);

        VkDescriptorSet spd_set = VK_NULL_HANDLE;
        VkDescriptorImageInfo s0{}, s128{}, s129{}, s130{};
        VkDescriptorBufferInfo s256{};
        if (has_spd) {
            spd_set = alloc_set(spd_layout);
            s0 = image_info(input_color.view);
            s128 = image_info(exposure_atomic.view);
            s129 = image_info(exposure_mip.view);
            s130 = image_info(exposure.view);
            s256 = {spd_buffer.buffer, 0, VK_WHOLE_SIZE};
            write_image(spd_set, 0, &s0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            write_image(spd_set, 128, &s128, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            write_image(spd_set, 129, &s129, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            write_image(spd_set, 130, &s130, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            write_buffer(spd_set, 256, &s256, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            write_image(spd_set, 320, &sampler_info, VK_DESCRIPTOR_TYPE_SAMPLER);
        }

        VkDescriptorSet rcas_set = VK_NULL_HANDLE;
        VkDescriptorBufferInfo r0{};
        VkDescriptorImageInfo r11{}, r17{}, r18{};
        if (has_rcas) {
            rcas_set = alloc_set(rcas_layout);
            r0 = {rcas_buffer.buffer, 0, VK_WHOLE_SIZE};
            r11 = image_info(output.view);
            r17 = image_info(exposure.view);
            r18 = image_info(rcas_input.view);
            write_buffer(rcas_set, 0, &r0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            write_image(rcas_set, 11, &r11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            write_image(rcas_set, 17, &r17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            write_image(rcas_set, 18, &r18, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            write_image(rcas_set, 320, &sampler_info, VK_DESCRIPTOR_TYPE_SAMPLER);
        }

        vkUpdateDescriptorSets(d.device, static_cast<uint32_t>(writes.size()), writes.data(),
                               0, nullptr);

        // Pipelines.
        std::vector<VkPipeline> pipelines;
        std::vector<VkShaderModule> modules;
        for (const Pass& p : c.passes) {
            const auto bytes = readFile(p.spv_path);
            auto words = spirvWords(bytes);
            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = bytes.size();
            smci.pCode = words.data();
            VkShaderModule module = VK_NULL_HANDLE;
            VK_CHECK(vkCreateShaderModule(d.device, &smci, nullptr, &module));
            modules.push_back(module);

            const LayoutInfo& pl =
                p.kind == kSpd ? spd_pl : (p.kind == kRcas ? rcas_pl : model_pl);
            VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpci.stage.module = module;
            cpci.stage.pName = p.entry.c_str();
            cpci.layout = pl.pipeline_layout;
            VkPipeline pipeline = VK_NULL_HANDLE;
            const VkResult r = vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &cpci,
                                                        nullptr, &pipeline);
            if (r != VK_SUCCESS) {
                std::fprintf(stderr, "pipeline '%s' (%s) failed: %d\n", p.name.c_str(),
                             p.spv_path.c_str(), (int)r);
                return 1;
            }
            pipelines.push_back(pipeline);
        }
        std::printf("created %zu pipelines\n", pipelines.size());

        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = d.queue_family;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateCommandPool(d.device, &cpci, nullptr, &command_pool));
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = command_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(d.device, &cbai, &cb));
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &cbbi));

        auto to_general = [&](Image& img) {
            imageBarrier(cb, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, color_aspect);
        };
        to_general(history);
        to_general(reprojected);
        to_general(recurrent);
        to_general(exposure);
        to_general(dummy_exposure);
        to_general(output);
        if (has_rcas) to_general(rcas_input);
        if (has_spd) {
            to_general(exposure_atomic);
            to_general(exposure_mip);
        }
        imageBarrier(cb, input_color.image, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     color_aspect);
        imageBarrier(cb, input_depth.image, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT);
        imageBarrier(cb, input_motion.image, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     color_aspect);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {c.render_w, c.render_h, 1};
        vkCmdCopyBufferToImage(cb, upload.buffer, input_color.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.bufferOffset = color_bytes.size();
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyBufferToImage(cb, upload.buffer, input_depth.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.bufferOffset = color_bytes.size() + depth_bytes.size();
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdCopyBufferToImage(cb, upload.buffer, input_motion.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        imageBarrier(cb, input_color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, color_aspect);
        imageBarrier(cb, input_depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        imageBarrier(cb, input_motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, color_aspect);

        VkClearColorValue zero{};
        const VkImageSubresourceRange full_range{color_aspect, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, history.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &full_range);
        vkCmdClearColorImage(cb, reprojected.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &full_range);
        vkCmdClearColorImage(cb, recurrent.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &full_range);
        vkCmdClearColorImage(cb, exposure.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &full_range);
        VkClearColorValue exposure_one{};
        exposure_one.float32[0] = 1.0f;
        exposure_one.float32[1] = 1.0f;
        vkCmdClearColorImage(cb, dummy_exposure.image, VK_IMAGE_LAYOUT_GENERAL,
                             &exposure_one, 1, &full_range);
        if (has_spd) {
            VkClearColorValue zero_u{};
            vkCmdClearColorImage(cb, exposure_atomic.image, VK_IMAGE_LAYOUT_GENERAL, &zero_u,
                                 1, &full_range);
            vkCmdClearColorImage(cb, exposure_mip.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                                 &full_range);
        }
        vkCmdFillBuffer(cb, scratch_buffer.buffer, 0, VK_WHOLE_SIZE, 0);
        computeBarrier(cb);

        for (size_t i = 0; i < c.passes.size(); ++i) {
            const Pass& p = c.passes[i];
            if (getenv("FSR4_RUNNER_VERBOSE"))
                std::printf("pass %zu kind=%d %s groups=%u,%u,%u\n", i, p.kind,
                            p.name.c_str(), p.groups[0], p.groups[1], p.groups[2]);
            const LayoutInfo& pl =
                p.kind == kSpd ? spd_pl : (p.kind == kRcas ? rcas_pl : model_pl);
            const VkDescriptorSet set =
                p.kind == kSpd ? spd_set : (p.kind == kRcas ? rcas_set : model_set);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl.pipeline_layout, 0,
                                    1, &set, 0, nullptr);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[i]);
            vkCmdDispatch(cb, p.groups[0], p.groups[1], p.groups[2]);
            computeBarrier(cb);
        }

        if (dump_after) {
            const char* sel = getenv("FSR4_DUMP_AFTER");
            Image* img = (strcmp(sel, "rcas") == 0 && has_rcas)
                             ? &rcas_input
                             : (strcmp(sel, "history") == 0
                                    ? &history
                                    : (strcmp(sel, "reprojected") == 0 ? &reprojected
                                                                       : &output));
            imageBarrier(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, color_aspect);
            VkBufferImageCopy dbg{};
            dbg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            dbg.imageExtent = {c.output_w, c.output_h, 1};
            vkCmdCopyImageToBuffer(cb, img->image, VK_IMAGE_LAYOUT_GENERAL,
                                   dbg_staging.buffer, 1, &dbg);
            imageBarrier(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, color_aspect);
        }

        imageBarrier(cb, output.image, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, color_aspect);
        VkBufferImageCopy out_copy{};
        out_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        out_copy.imageExtent = {c.output_w, c.output_h, 1};
        vkCmdCopyImageToBuffer(cb, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging.buffer, 1, &out_copy);
        VK_CHECK(vkEndCommandBuffer(cb));

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        VK_CHECK(vkCreateFence(d.device, &fci, nullptr, &fence));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(d.queue, 1, &si, fence));
        VK_CHECK(vkWaitForFences(d.device, 1, &fence, VK_TRUE, UINT64_MAX));

        void* mapped = nullptr;
        VK_CHECK(vkMapMemory(d.device, staging.memory, 0, staging.size, 0, &mapped));
        writeFile("out_native.rgba8", mapped, staging.size);
        unsigned long long sum = 0;
        size_t nonzero = 0;
        const uint8_t* px = static_cast<const uint8_t*>(mapped);
        for (VkDeviceSize i = 0; i < staging.size; ++i) {
            sum += px[i];
            if (px[i]) ++nonzero;
        }
        std::printf("output: %zu/%llu bytes non-zero, sum=%llu\n", nonzero,
                    (unsigned long long)staging.size, sum);
        vkUnmapMemory(d.device, staging.memory);
        if (dump_after) {
            void* dbg_ptr = nullptr;
            VK_CHECK(vkMapMemory(d.device, dbg_staging.memory, 0, dbg_staging.size, 0,
                                 &dbg_ptr));
            writeFile("dbg_after.bin", dbg_ptr, (size_t)dbg_staging.size);
            unsigned long long dsum = 0;
            const uint8_t* dp = static_cast<const uint8_t*>(dbg_ptr);
            for (VkDeviceSize i = 0; i < dbg_staging.size; ++i) dsum += dp[i];
            std::printf("dbg_after: sum=%llu\n", dsum);
            vkUnmapMemory(d.device, dbg_staging.memory);
        }
        std::printf("wrote out_native.rgba8\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
