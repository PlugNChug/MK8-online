/* radv-mlfi repro: the smallest harness that turns the MLFI composite-pass crash into a
 * reproducible experiment, and that can verify a legalized module by running it.
 *
 * Modes
 *   pipeline (default)  create the module, layout and compute pipeline; print PASS/FAIL
 *   --pass151           dispatch (a legalized) mlfi_4k_pass151: fills ScratchBuffer with a
 *                       deterministic pattern, dispatches 20x12 groups, reads both R16G16_SFLOAT
 *                       outputs back and checks every pixel against the arithmetic in the shader
 *   --dispatch GX GY    dispatch any module with buffers/images allocated from its own descriptor
 *                       layout; prints per-image readback hashes (no correctness claim)
 *
 * The program never reads AMD's DLL: the caller passes an extracted (and optionally rewritten)
 * .spv.  It exists to separate "the module is invalid SPIR-V" from "RADV rejects valid SPIR-V",
 * which is the difference between an application-side workaround and a driver bug.
 *
 * Build: tools/radv-mlfi/build.sh
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define MAX_TYPES 65536
#define MAX_BINDINGS 32
#define MAX_IMAGES 32
#define BINDING_SLOTS 512

/* ------------------------------------------------------------------ SPIR-V front end ---------- */

enum { K_UNKNOWN, K_FLOAT, K_INT };

typedef struct {
   uint32_t set, binding;
   VkDescriptorType type;
   /* image bindings only */
   uint32_t sampled_type_kind; /* K_FLOAT / K_INT */
   uint32_t sampled_type_bits;
   uint32_t dim, sampled, format; /* OpTypeImage operands */
} Binding;

typedef struct {
   uint32_t count;
   Binding items[MAX_BINDINGS];
   char entry[64];
   uint32_t execution_model;
   uint32_t local_size[3];
   uint32_t image_count;
   struct {
      uint32_t kind, bits, dim, sampled, format;
   } images[MAX_IMAGES];
} Module;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static const char *format_name(uint32_t f) {
   static const char *names[] = {
      "Unknown", "Rgba32f", "Rgba16f", "R32f", "Rgba8", "Rgba8Snorm", "Rg32f", "Rg16f", "R11fG11fB10f",
      "R16f", "Rgba16", "Rgb10A2", "Rg16", "Rg8", "R16", "R8", "Rgba16Snorm", "Rg16Snorm", "Rg8Snorm",
      "R16Snorm", "R8Snorm", "Rgba32i", "Rgba32ui", "Rgba16i", "Rgba16ui", "Rgba8i", "Rgba8ui", "R32i",
      "R32ui", "Rg32i", "Rg32ui", "Rg16i", "Rg16ui", "Rg8i", "Rg8ui", "R16i", "R16ui", "R8i", "R8ui",
      "R64ui", "R64i"};
   return f < sizeof(names) / sizeof(names[0]) ? names[f] : "?";
}

static const char *dim_name(uint32_t d) {
   static const char *names[] = {"1D", "2D", "3D", "Cube", "Rect", "Buffer", "SubpassData"};
   return d < 7 ? names[d] : "?";
}

static int parse_module(const uint8_t *code, size_t size, Module *m) {
   if (size < 20 || rd32(code) != 0x07230203) return -1;
   uint32_t words = (uint32_t)(size / 4);
   memset(m, 0, sizeof(*m));
   m->local_size[0] = m->local_size[1] = m->local_size[2] = 1;

   enum { K_OTHER = 0, K_SAMPLER, K_IMAGE, K_STRUCT, K_POINTER, K_SAMPLED_IMAGE };
   static uint8_t kind[MAX_TYPES];
   static uint32_t pointer_pointee[MAX_TYPES], image_id_of[MAX_TYPES];
   static uint32_t image_sampled[MAX_TYPES], image_format[MAX_TYPES], image_dim[MAX_TYPES];
   static uint32_t image_stype[MAX_TYPES];
   static uint32_t struct_block[MAX_TYPES];
   static uint32_t var_type[MAX_TYPES], var_storage[MAX_TYPES];
   static uint32_t binding_of[MAX_TYPES], set_of[MAX_TYPES];
   static uint32_t scalar_kind[MAX_TYPES], scalar_bits[MAX_TYPES];
   static uint8_t has_binding[MAX_TYPES], has_set[MAX_TYPES];
   static uint8_t image_seen[MAX_TYPES];
   memset(kind, 0, sizeof(kind));
   memset(has_binding, 0, sizeof(has_binding));
   memset(has_set, 0, sizeof(has_set));
   memset(var_type, 0, sizeof(var_type));
   memset(image_seen, 0, sizeof(image_seen));

   uint32_t at = 5;
   while (at + 1 <= words) {
      uint32_t word = rd32(code + at * 4);
      uint32_t count = word >> 16, op = word & 0xffff;
      if (!count || at + count > words) break;
      switch (op) {
      case 15: { /* OpEntryPoint */
         const char *name = (const char *)(code + (at + 3) * 4);
         if (!m->entry[0] && count >= 4) {
            strncpy(m->entry, name, sizeof(m->entry) - 1);
            m->execution_model = rd32(code + (at + 1) * 4);
         }
         break;
      }
      case 16: /* OpExecutionMode */
         if (count >= 6 && rd32(code + (at + 2) * 4) == 17) { /* LocalSize */
            m->local_size[0] = rd32(code + (at + 3) * 4);
            m->local_size[1] = rd32(code + (at + 4) * 4);
            m->local_size[2] = rd32(code + (at + 5) * 4);
         }
         break;
      case 71: { /* OpDecorate */
         uint32_t target = rd32(code + (at + 1) * 4) & 0xffff;
         uint32_t dec = rd32(code + (at + 2) * 4);
         uint32_t value = count >= 4 ? rd32(code + (at + 3) * 4) : 0;
         if (dec == 33) { binding_of[target] = value; has_binding[target] = 1; }
         else if (dec == 34) { set_of[target] = value; has_set[target] = 1; }
         else if (dec == 2) struct_block[target] = 2;      /* Block */
         else if (dec == 3) struct_block[target] = 3;      /* BufferBlock */
         break;
      }
      case 21: /* OpTypeInt */
         scalar_kind[rd32(code + (at + 1) * 4) & 0xffff] = K_INT;
         scalar_bits[rd32(code + (at + 1) * 4) & 0xffff] = rd32(code + (at + 2) * 4);
         break;
      case 22: /* OpTypeFloat */
         scalar_kind[rd32(code + (at + 1) * 4) & 0xffff] = K_FLOAT;
         scalar_bits[rd32(code + (at + 1) * 4) & 0xffff] = rd32(code + (at + 2) * 4);
         break;
      case 25: { /* OpTypeImage */
         uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
         kind[id] = K_IMAGE;
         image_id_of[id] = id;
         image_stype[id] = rd32(code + (at + 2) * 4) & 0xffff;
         image_dim[id] = rd32(code + (at + 3) * 4);
         image_sampled[id] = rd32(code + (at + 7) * 4);
         image_format[id] = count >= 9 ? rd32(code + (at + 8) * 4) : 0;
         if (!image_seen[id] && m->image_count < MAX_IMAGES) {
            uint32_t st = image_stype[id];
            m->images[m->image_count].kind = scalar_kind[st];
            m->images[m->image_count].bits = scalar_bits[st];
            m->images[m->image_count].dim = image_dim[id];
            m->images[m->image_count].sampled = image_sampled[id];
            m->images[m->image_count].format = image_format[id];
            m->image_count++;
            image_seen[id] = 1;
         }
         break;
      }
      case 26: /* OpTypeSampler */
         kind[rd32(code + (at + 1) * 4) & 0xffff] = K_SAMPLER;
         break;
      case 27: { /* OpTypeSampledImage */
         uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
         kind[id] = K_SAMPLED_IMAGE;
         image_id_of[id] = rd32(code + (at + 2) * 4) & 0xffff;
         break;
      }
      case 30: /* OpTypeStruct */
         kind[rd32(code + (at + 1) * 4) & 0xffff] = K_STRUCT;
         break;
      case 32: /* OpTypePointer */
         kind[rd32(code + (at + 1) * 4) & 0xffff] = K_POINTER;
         pointer_pointee[rd32(code + (at + 1) * 4) & 0xffff] = rd32(code + (at + 3) * 4) & 0xffff;
         break;
      case 59: { /* OpVariable */
         uint32_t type = rd32(code + (at + 1) * 4) & 0xffff;
         uint32_t id = rd32(code + (at + 2) * 4) & 0xffff;
         var_type[id] = type;
         var_storage[id] = count >= 4 ? rd32(code + (at + 3) * 4) : 0;
         break;
      }
      default:
         break;
      }
      at += count;
   }

   for (uint32_t id = 0; id < MAX_TYPES; ++id) {
      if (!var_type[id] || !has_binding[id]) continue;
      uint32_t storage = var_storage[id];
      if (storage != 0 && storage != 2 && storage != 12) continue;
      uint32_t ptr = var_type[id];
      if (kind[ptr] != K_POINTER) continue;
      uint32_t pointee = pointer_pointee[ptr];
      if (m->count >= MAX_BINDINGS) break;
      Binding *b = &m->items[m->count];
      VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
      if (kind[pointee] == K_SAMPLER) {
         type = VK_DESCRIPTOR_TYPE_SAMPLER;
      } else if (kind[pointee] == K_SAMPLED_IMAGE || kind[pointee] == K_IMAGE) {
         uint32_t img = (kind[pointee] == K_SAMPLED_IMAGE) ? image_id_of[pointee] : pointee;
         type = (kind[pointee] == K_SAMPLED_IMAGE)  ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                : image_sampled[img] == 2         ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                  : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
         b->sampled_type_kind = scalar_kind[image_stype[img]];
         b->sampled_type_bits = scalar_bits[image_stype[img]];
         b->dim = image_dim[img];
         b->sampled = image_sampled[img];
         b->format = image_format[img];
      } else if (kind[pointee] == K_STRUCT) {
         type = (struct_block[pointee] == 2 && storage == 2) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      }
      if (type == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;
      b->set = has_set[id] ? set_of[id] : 0;
      b->binding = binding_of[id];
      b->type = type;
      m->count++;
   }
   return 0;
}

static VkFormat format_for_binding(const Binding *b) {
   if (b->format) {
      switch (b->format) {
      case 1: return VK_FORMAT_R32G32B32A32_SFLOAT;
      case 2: return VK_FORMAT_R16G16B16A16_SFLOAT;
      case 3: return VK_FORMAT_R32_SFLOAT;
      case 6: return VK_FORMAT_R32G32_SFLOAT;
      case 7: return VK_FORMAT_R16G16_SFLOAT;
      case 9: return VK_FORMAT_R16_SFLOAT;
      case 21: return VK_FORMAT_R32G32B32A32_SINT;
      case 22: return VK_FORMAT_R32G32B32A32_UINT;
      case 27: return VK_FORMAT_R32_SINT;
      case 28: return VK_FORMAT_R32_UINT;
      case 39: return VK_FORMAT_R64_UINT;
      default: break;
      }
   }
   if (b->sampled_type_kind == K_FLOAT)
      return b->sampled_type_bits == 16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
   if (b->sampled_type_kind == K_INT)
      return b->sampled_type_bits == 64 ? VK_FORMAT_R64_UINT : VK_FORMAT_R32G32B32A32_UINT;
   return VK_FORMAT_R32G32B32A32_SFLOAT;
}

static uint32_t format_bpp(VkFormat f) {
   switch (f) {
   case VK_FORMAT_R16G16_SFLOAT: return 4;
   case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT: return 4;
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R64_UINT: return 8;
   default: return 16;
   }
}

/* ------------------------------------------------------------------ Vulkan plumbing ----------- */

static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static VkPhysicalDeviceMemoryProperties mem_props;
static int device_lost;

typedef struct {
   VkBuffer buffer;
   VkDeviceMemory memory;
   void *map;
   VkDeviceSize size;
} Buffer;

typedef struct {
   VkImage image;
   VkDeviceMemory memory;
   VkImageView view;
   VkFormat format;
   uint32_t width, height;
} Image;

static void die(const char *fmt, ...) {
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
   exit(2);
}

static uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags props) {
   for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) return i;
   die("no memory type for bits 0x%x", bits);
   return 0;
}

static void buffer_create(Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
   VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
   ci.size = size;
   ci.usage = usage;
   ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   if (vkCreateBuffer(device, &ci, NULL, &b->buffer) != VK_SUCCESS) die("vkCreateBuffer failed");
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(device, b->buffer, &req);
   VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
   ai.allocationSize = req.size;
   ai.memoryTypeIndex = find_memory(req.memoryTypeBits, props);
   if (vkAllocateMemory(device, &ai, NULL, &b->memory) != VK_SUCCESS) die("vkAllocateMemory(buffer) failed");
   vkBindBufferMemory(device, b->buffer, b->memory, 0);
   b->size = size;
   b->map = NULL;
   if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      if (vkMapMemory(device, b->memory, 0, size, 0, &b->map) != VK_SUCCESS) die("vkMapMemory failed");
   }
}

static void image_create(Image *im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
   memset(im, 0, sizeof(*im));
   im->format = format;
   im->width = w;
   im->height = h;
   VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
   ci.imageType = VK_IMAGE_TYPE_2D;
   ci.format = format;
   ci.extent = (VkExtent3D){w, h, 1};
   ci.mipLevels = 1;
   ci.arrayLayers = 1;
   ci.samples = VK_SAMPLE_COUNT_1_BIT;
   ci.tiling = VK_IMAGE_TILING_OPTIMAL;
   ci.usage = usage;
   ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   if (vkCreateImage(device, &ci, NULL, &im->image) != VK_SUCCESS) die("vkCreateImage(%u) failed", format);
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(device, im->image, &req);
   VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
   ai.allocationSize = req.size;
   ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (vkAllocateMemory(device, &ai, NULL, &im->memory) != VK_SUCCESS) die("vkAllocateMemory(image) failed");
   vkBindImageMemory(device, im->image, im->memory, 0);
   VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
   vi.image = im->image;
   vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
   vi.format = format;
   vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   if (vkCreateImageView(device, &vi, NULL, &im->view) != VK_SUCCESS) die("vkCreateImageView failed");
}

static VkCommandBuffer begin_once(void) {
   VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   pci.queueFamilyIndex = queue_family;
   VkCommandPool pool;
   if (vkCreateCommandPool(device, &pci, NULL, &pool) != VK_SUCCESS) die("vkCreateCommandPool failed");
   VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
   ai.commandPool = pool;
   ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   ai.commandBufferCount = 1;
   VkCommandBuffer cb;
   if (vkAllocateCommandBuffers(device, &ai, &cb) != VK_SUCCESS) die("vkAllocateCommandBuffers failed");
   VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   vkBeginCommandBuffer(cb, &bi);
   return cb; /* the pool is intentionally leaked: this is a test harness */
}

static int submit_and_wait(VkCommandBuffer cb) {
   vkEndCommandBuffer(cb);
   VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cb;
   VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   vkCreateFence(device, &fi, NULL, &fence);
   VkResult r = vkQueueSubmit(queue, 1, &si, fence);
   if (r != VK_SUCCESS) {
      printf("submit: FAIL VkResult=%d\n", r);
      if (r == VK_ERROR_DEVICE_LOST) device_lost = 1;
      return 0;
   }
   r = vkWaitForFences(device, 1, &fence, VK_TRUE, 20000000000ull);
   if (r != VK_SUCCESS) {
      printf("fence: FAIL VkResult=%d\n", r);
      if (r == VK_ERROR_DEVICE_LOST) device_lost = 1;
      return 0;
   }
   printf("fence: signaled\n");
   vkDestroyFence(device, fence, NULL);
   return 1;
}

static VkShaderModule load_module(const char *path, uint8_t **code_out, size_t *size_out) {
   FILE *f = fopen(path, "rb");
   if (!f) die("cannot open %s: %s", path, strerror(errno));
   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint8_t *code = malloc((size_t)len);
   if (fread(code, 1, (size_t)len, f) != (size_t)len) die("short read on %s", path);
   fclose(f);
   VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   ci.codeSize = (size_t)len;
   ci.pCode = (const uint32_t *)code;
   VkShaderModule mod;
   VkResult r = vkCreateShaderModule(device, &ci, NULL, &mod);
   if (r != VK_SUCCESS) die("vkCreateShaderModule: VkResult=%d", r);
   *code_out = code;
   *size_out = (size_t)len;
   return mod;
}

static VkDescriptorSetLayout make_set_layout(const Module *m) {
   VkDescriptorSetLayoutBinding binds[MAX_BINDINGS];
   for (uint32_t i = 0; i < m->count; ++i) {
      binds[i].binding = m->items[i].binding;
      binds[i].descriptorType = m->items[i].type;
      binds[i].descriptorCount = 1;
      binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      binds[i].pImmutableSamplers = NULL;
   }
   VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
   ci.bindingCount = m->count;
   ci.pBindings = binds;
   VkDescriptorSetLayout l;
   if (vkCreateDescriptorSetLayout(device, &ci, NULL, &l) != VK_SUCCESS) die("vkCreateDescriptorSetLayout failed");
   return l;
}

static VkPipeline create_pipeline(VkShaderModule mod, const char *entry, VkPipelineLayout layout) {
   VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
   ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
   ci.stage.module = mod;
   ci.stage.pName = entry;
   ci.layout = layout;
   VkPipeline p = VK_NULL_HANDLE;
   VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &p);
   if (r != VK_SUCCESS) {
      printf("pipeline: FAIL VkResult=%d (%s)\n", r,
             r == VK_ERROR_DEVICE_LOST ? "VK_ERROR_DEVICE_LOST"
             : r == VK_ERROR_UNKNOWN   ? "VK_ERROR_UNKNOWN (shader rejected)"
                                       : "see spec");
      return VK_NULL_HANDLE;
   }
   printf("pipeline: OK\n");
   return p;
}

/* ------------------------------------------------------------------ device --------------------- */

typedef struct {
   int no_cm, no_cm2, no_f8, no_i64img, no_f16;
   int debug_report;
} Toggles;

static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_report_cb(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT obj_type, uint64_t object,
                size_t location, int32_t code, const char *prefix, const char *msg, void *user)
{
   fprintf(stderr, "radv-report: %s\n", msg);
   return VK_FALSE;
}

static void create_device(int index, Toggles t) {
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
   app.apiVersion = VK_API_VERSION_1_3;
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   ici.pApplicationInfo = &app;
   const char *inst_ext[] = {VK_EXT_DEBUG_REPORT_EXTENSION_NAME};
   if (t.debug_report) {
      ici.enabledExtensionCount = 1;
      ici.ppEnabledExtensionNames = inst_ext;
   }
   if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS) die("vkCreateInstance failed");
   if (t.debug_report) {
      PFN_vkCreateDebugReportCallbackEXT create_cb =
         (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT");
      if (!create_cb) die("VK_EXT_debug_report not available");
      VkDebugReportCallbackCreateInfoEXT ci = {VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT};
      ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_INFORMATION_BIT_EXT;
      ci.pfnCallback = debug_report_cb;
      VkDebugReportCallbackEXT cb;
      if (create_cb(instance, &ci, NULL, &cb) != VK_SUCCESS) die("vkCreateDebugReportCallbackEXT failed");
   }

   uint32_t n = 0;
   vkEnumeratePhysicalDevices(instance, &n, NULL);
   if (!n) die("no Vulkan device");
   VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
   vkEnumeratePhysicalDevices(instance, &n, devs);
   physical = devs[index < (int)n ? index : 0];
   free(devs);

   VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
   vkGetPhysicalDeviceProperties2(physical, &p2);
   printf("device: %s\n", p2.properties.deviceName);
   printf("driver: 0x%08x deviceID=0x%04x\n", p2.properties.driverVersion, p2.properties.deviceID);
   vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);

   uint32_t qn = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, NULL);
   VkQueueFamilyProperties *q = calloc(qn, sizeof(*q));
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, q);
   queue_family = 0;
   for (uint32_t i = 0; i < qn; ++i)
      if (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { queue_family = i; break; }
   free(q);

   uint32_t ec = 0;
   vkEnumerateDeviceExtensionProperties(physical, NULL, &ec, NULL);
   VkExtensionProperties *ext = calloc(ec, sizeof(*ext));
   vkEnumerateDeviceExtensionProperties(physical, NULL, &ec, ext);
   int has_cm = 0, has_cm2 = 0, has_f8 = 0, has_i64 = 0, has_vmm = 0, has_vp = 0, has_cm1 = 0;
   for (uint32_t i = 0; i < ec; ++i) {
      const char *e = ext[i].extensionName;
      if (!strcmp(e, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) has_cm = 1;
      else if (!strcmp(e, VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME)) has_cm2 = 1;
      else if (!strcmp(e, VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME)) has_cm1 = 1;
      else if (!strcmp(e, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) has_f8 = 1;
      else if (!strcmp(e, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME)) has_i64 = 1;
      else if (!strcmp(e, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME)) has_vmm = 1;
      else if (!strcmp(e, VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME)) has_vp = 1;
   }
   free(ext);

   VkPhysicalDeviceShaderFloat16Int8Features f16 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
   VkPhysicalDeviceVulkanMemoryModelFeatures vmm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
   VkPhysicalDeviceVariablePointersFeatures vp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
   VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
   VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT cm2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_MAINTENANCE_1_FEATURES_EXT};
   VkPhysicalDeviceShaderFloat8FeaturesEXT f8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
   VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT i64 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
   void **tail = &f16.pNext;
   *tail = &vmm; tail = &vmm.pNext;
   *tail = &vp; tail = &vp.pNext;
   *tail = &cm; tail = &cm.pNext;
   *tail = &cm2; tail = &cm2.pNext;
   *tail = &f8; tail = &f8.pNext;
   *tail = &i64;
   VkPhysicalDeviceFeatures2 feat2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
   feat2.pNext = &f16;
   vkGetPhysicalDeviceFeatures2(physical, &feat2);

   VkPhysicalDeviceFeatures core = feat2.features;
   f16.shaderFloat16 = t.no_f16 ? VK_FALSE : f16.shaderFloat16;
   vmm.vulkanMemoryModelDeviceScope = vmm.vulkanMemoryModel;
   vp.variablePointers = vp.variablePointersStorageBuffer;
   f8.shaderFloat8 = (!t.no_f8 && has_f8) ? f8.shaderFloat8 : VK_FALSE;
   f8.shaderFloat8CooperativeMatrix = (!t.no_f8 && has_f8) ? f8.shaderFloat8CooperativeMatrix : VK_FALSE;
   cm.cooperativeMatrix = (!t.no_cm && has_cm) ? cm.cooperativeMatrix : VK_FALSE;
   if (t.no_cm2 || !(has_cm1 || has_cm2)) {
      cm2.cooperativeMatrixConversions = VK_FALSE;
      cm2.cooperativeMatrixPerElementOperations = VK_FALSE;
      cm2.cooperativeMatrixReductions = VK_FALSE;
      cm2.cooperativeMatrixProperties2 = VK_FALSE;
      cm2.cooperativeMatrixGetCoordinate = VK_FALSE;
   }
   if (t.no_i64img) i64.shaderImageInt64Atomics = VK_FALSE;

   const char *used[8];
   uint32_t un = 0;
   if (has_cm && !t.no_cm) used[un++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
   if (has_cm2 && !t.no_cm2) used[un++] = VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME;
   if (has_cm1 && !t.no_cm2) used[un++] = VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME;
   if (has_f8 && !t.no_f8) used[un++] = VK_EXT_SHADER_FLOAT8_EXTENSION_NAME;
   if (has_i64 && !t.no_i64img) used[un++] = VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME;
   if (has_vmm) used[un++] = VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME;
   if (has_vp) used[un++] = VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME;

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
   qci.queueFamilyIndex = queue_family;
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;
   dci.pNext = &f16;
   dci.pEnabledFeatures = &core;
   dci.enabledExtensionCount = un;
   dci.ppEnabledExtensionNames = used;
   if (vkCreateDevice(physical, &dci, NULL, &device) != VK_SUCCESS) die("vkCreateDevice failed");
   vkGetDeviceQueue(device, queue_family, 0, &queue);
}

/* ------------------------------------------------------------------ resources ------------------ */

typedef struct {
   VkDescriptorSet set;
   VkDescriptorPool pool;
   Buffer buffers[MAX_BINDINGS];
   Image images[MAX_IMAGES];
   int image_of_binding[BINDING_SLOTS];
   int buffer_of_binding[BINDING_SLOTS];
   uint32_t buffer_count, image_count;
} Resources;

static void make_resources(const Module *m, VkDescriptorSetLayout layout, Resources *r, uint32_t image_size,
                           int pass151_formats) {
   memset(r, 0, sizeof(*r));
   for (uint32_t i = 0; i < BINDING_SLOTS; ++i) {
      r->image_of_binding[i] = -1;
      r->buffer_of_binding[i] = -1;
   }
   VkDescriptorPoolSize sizes[5] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * MAX_BINDINGS},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_BINDINGS},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_BINDINGS},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_BINDINGS},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_BINDINGS},
   };
   VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
   pci.maxSets = 1;
   pci.poolSizeCount = 5;
   pci.pPoolSizes = sizes;
   if (vkCreateDescriptorPool(device, &pci, NULL, &r->pool) != VK_SUCCESS) die("vkCreateDescriptorPool failed");
   VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
   ai.descriptorPool = r->pool;
   ai.descriptorSetCount = 1;
   ai.pSetLayouts = &layout;
   if (vkAllocateDescriptorSets(device, &ai, &r->set) != VK_SUCCESS) die("vkAllocateDescriptorSets failed");

   VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
   sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
   sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
   sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   sci.maxLod = 0.0f;
   VkSampler sampler;
   if (vkCreateSampler(device, &sci, NULL, &sampler) != VK_SUCCESS) die("vkCreateSampler failed");

   VkWriteDescriptorSet writes[MAX_BINDINGS];
   VkDescriptorBufferInfo binfo[MAX_BINDINGS];
   VkDescriptorImageInfo iinfo[MAX_BINDINGS];
   uint32_t wn = 0;
   for (uint32_t i = 0; i < m->count; ++i) {
      const Binding *b = &m->items[i];
      VkWriteDescriptorSet *w = &writes[wn];
      memset(w, 0, sizeof(*w));
      w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w->dstSet = r->set;
      w->dstBinding = b->binding;
      w->descriptorCount = 1;
      w->descriptorType = b->type;
      if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
         Buffer *buf = &r->buffers[r->buffer_count];
         VkDeviceSize size = b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? 4096 : (64u << 20);
         buffer_create(buf, size,
                       b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                                                    : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
         memset(buf->map, 0, (size_t)size);
         binfo[wn] = (VkDescriptorBufferInfo){buf->buffer, 0, VK_WHOLE_SIZE};
         w->pBufferInfo = &binfo[wn];
         r->buffer_of_binding[b->binding] = (int)r->buffer_count;
         r->buffer_count++;
      } else if (b->type == VK_DESCRIPTOR_TYPE_SAMPLER) {
         iinfo[wn] = (VkDescriptorImageInfo){sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
         w->pImageInfo = &iinfo[wn];
      } else {
         Image *im = &r->images[r->image_count];
         VkFormat fmt = format_for_binding(b);
         /* The provider creates the flow buffers; pass151 stores two half components per pixel, so
          * the view must be RG16F.  The legalized module says "format Unknown", which is exactly the
          * promise that the view decides the format at bind time. */
         if (pass151_formats && (b->binding == 129 || b->binding == 130)) fmt = VK_FORMAT_R16G16_SFLOAT;
         VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
         usage |= (b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
         image_create(im, fmt, image_size, image_size, usage);
         iinfo[wn] = (VkDescriptorImageInfo){b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? sampler : VK_NULL_HANDLE,
                                             im->view, VK_IMAGE_LAYOUT_GENERAL};
         w->pImageInfo = &iinfo[wn];
         r->image_of_binding[b->binding] = (int)r->image_count;
         r->image_count++;
      }
      wn++;
   }
   vkUpdateDescriptorSets(device, wn, writes, 0, NULL);
}

static bool format_is_int(VkFormat f) {
   switch (f) {
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R64_UINT:
   case VK_FORMAT_R64_SINT: return true;
   default: return false;
   }
}

static void transition_all(Resources *r, bool restore_after_clear, int clear_hi) {
   VkCommandBuffer cb = begin_once();
   for (uint32_t i = 0; i < r->image_count; ++i) {
      VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = r->images[i].image;
      b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      VkClearColorValue zero;
      memset(&zero, 0, sizeof(zero));
      if (clear_hi) {
         if (format_is_int(r->images[i].format)) {
            zero.uint32[0] = zero.uint32[1] = zero.uint32[2] = zero.uint32[3] = 1;
         } else {
            zero.float32[0] = zero.float32[1] = zero.float32[2] = zero.float32[3] = 0.5f;
         }
      }
      vkCmdClearColorImage(cb, r->images[i].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                           &(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
      if (restore_after_clear) {
         VkImageMemoryBarrier b2 = b;
         b2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
         b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
         vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
      }
   }
   submit_and_wait(cb);
}

static void read_image(Image *im, Buffer *staging) {
   uint32_t bpp = format_bpp(im->format);
   VkDeviceSize size = (VkDeviceSize)im->width * im->height * bpp;
   buffer_create(staging, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkCommandBuffer cb = begin_once();
   VkBufferImageCopy region = {0};
   region.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
   region.imageExtent = (VkExtent3D){im->width, im->height, 1};
   vkCmdCopyImageToBuffer(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, staging->buffer, 1, &region);
   submit_and_wait(cb);
}

static void image_stats(Resources *r, uint32_t which) {
   Image *im = &r->images[which];
   Buffer staging;
   read_image(im, &staging);
   uint32_t bpp = format_bpp(im->format);
   uint64_t nz = 0, total = (uint64_t)im->width * im->height;
   uint64_t h = 1469598103934665603ull;
   const uint8_t *p = staging.map;
   for (uint64_t i = 0; i < total; ++i) {
      bool any = false;
      for (uint32_t k = 0; k < bpp; ++k) {
         uint8_t v = p[i * bpp + k];
         any |= v != 0;
         h = (h ^ v) * 1099511628211ull;
      }
      nz += any;
   }
   printf("readback image[%u] (format=%u %ux%u): nonzero=%" PRIu64 "/%" PRIu64 " fnv1a=%016" PRIx64 "\n", which,
          (unsigned)im->format, im->width, im->height, nz, total, h);
}

/* ------------------------------------------------------------------ pass151 self-test ---------- */

/* From the module: byte address = 6150320 + 8*(x+1) + 3856*(y+1), read as uint32 at >>2. */
static uint32_t scratch_index(uint32_t x, uint32_t y) {
   return (6150320u + 8u * (x + 1u) + 3856u * (y + 1u)) >> 2;
}

static uint16_t f16_div(uint16_t bits, float divisor) {
   _Float16 v;
   memcpy(&v, &bits, 2);
   float q = (float)v / divisor;
   _Float16 res = (_Float16)q;
   uint16_t out;
   memcpy(&out, &res, 2);
   return out;
}

static uint32_t pack_halves(uint16_t lo, uint16_t hi) { return (uint32_t)lo | ((uint32_t)hi << 16); }

static int verify_pass151(Resources *r, int binding0, int binding1, uint32_t valid_w, uint32_t valid_h,
                          const uint32_t *words) {
   int idx[2] = {r->image_of_binding[binding0], r->image_of_binding[binding1]};
   uint32_t *data[2] = {NULL, NULL};
   for (int k = 0; k < 2; ++k) {
      if (idx[k] < 0) die("pass151: binding %d is not an image", k ? binding1 : binding0);
      Buffer staging;
      read_image(&r->images[idx[k]], &staging);
      data[k] = staging.map;
   }
   uint64_t match = 0, mismatch = 0;
   uint32_t shown = 0;
   for (uint32_t y = 0; y < r->images[idx[0]].height; ++y) {
      for (uint32_t x = 0; x < r->images[idx[0]].width; ++x) {
         for (int k = 0; k < 2; ++k) {
            uint16_t *px = (uint16_t *)data[k] + (y * r->images[idx[k]].width + x) * 2;
            uint16_t exp0 = 0, exp1 = 0;
            if (x < valid_w && y < valid_h) {
               uint32_t w0 = words[scratch_index(x, y)];
               uint32_t w1 = words[scratch_index(x, y) + 1];
               exp0 = f16_div((uint16_t)(w0 & 0xffff), (float)valid_w);
               exp1 = f16_div((uint16_t)(w0 >> 16), (float)valid_h);
               if (k == 1) {
                  exp0 = f16_div((uint16_t)(w1 & 0xffff), (float)valid_w);
                  exp1 = f16_div((uint16_t)(w1 >> 16), (float)valid_h);
               }
            }
            if (px[0] == exp0 && px[1] == exp1) {
               match++;
            } else {
               mismatch++;
               if (shown < 6) {
                  printf("  mismatch binding %d at (%u,%u): got %04x %04x expected %04x %04x\n",
                         k ? binding1 : binding0, x, y, px[0], px[1], exp0, exp1);
                  shown++;
               }
            }
         }
      }
   }
   printf("pass151 verify: matched=%" PRIu64 " mismatched=%" PRIu64 "\n", match, mismatch);
   return mismatch == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ main ------------------------ */

int main(int argc, char **argv) {
   const char *module_path = NULL;
   const char *entry = NULL;
   int device_index = 0, empty_layout = 0, dispatch_mode = 0, pass151 = 0, clear_hi = 0;
   uint32_t groups_x = 0, groups_y = 0;
   Toggles t = {0};
   for (int i = 1; i < argc; ++i) {
      if (!strcmp(argv[i], "--module") && i + 1 < argc) module_path = argv[++i];
      else if (!strcmp(argv[i], "--entry") && i + 1 < argc) entry = argv[++i];
      else if (!strcmp(argv[i], "--device") && i + 1 < argc) device_index = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--empty-layout")) empty_layout = 1;
      else if (!strcmp(argv[i], "--no-cm")) t.no_cm = 1;
      else if (!strcmp(argv[i], "--no-cm2")) t.no_cm2 = 1;
      else if (!strcmp(argv[i], "--no-f8")) t.no_f8 = 1;
      else if (!strcmp(argv[i], "--no-i64img")) t.no_i64img = 1;
      else if (!strcmp(argv[i], "--no-f16")) t.no_f16 = 1;
      else if (!strcmp(argv[i], "--pass151")) pass151 = 1;
      else if (!strcmp(argv[i], "--clear-hi")) clear_hi = 1;
      else if (!strcmp(argv[i], "--debug-report")) t.debug_report = 1;
      else if (!strcmp(argv[i], "--dispatch") && i + 2 < argc) {
         dispatch_mode = 1;
         groups_x = (uint32_t)atoi(argv[++i]);
         groups_y = (uint32_t)atoi(argv[++i]);
      } else if (argv[i][0] != '-') module_path = argv[i];
      else die("unknown option %s", argv[i]);
   }
   if (!module_path) die("usage: repro --module mod.spv [--entry E] [--dispatch GX GY | --pass151]");

   setvbuf(stdout, NULL, _IONBF, 0);
   create_device(device_index, t);

   uint8_t *code = NULL;
   size_t size = 0;
   VkShaderModule mod = load_module(module_path, &code, &size);
   Module m;
   if (parse_module(code, size, &m) != 0) die("%s is not SPIR-V", module_path);
   printf("module: %s (%zu bytes, entry %s)\n", module_path, size, m.entry);
   printf("local size: %u %u %u\n", m.local_size[0], m.local_size[1], m.local_size[2]);
   for (uint32_t i = 0; i < m.image_count; ++i) {
      const char *k = m.images[i].kind == K_FLOAT ? "float" : m.images[i].kind == K_INT ? "int" : "?";
      printf("image[%u]: sampled_type=%s%u dim=%s sampled=%u format=%s\n", i, k, m.images[i].bits,
             dim_name(m.images[i].dim), m.images[i].sampled, format_name(m.images[i].format));
   }
   if (m.execution_model != 5) die("entry point is not GLCompute");

   VkDescriptorSetLayout set_layout = empty_layout ? VK_NULL_HANDLE : make_set_layout(&m);
   VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   lci.setLayoutCount = empty_layout ? 0 : 1;
   lci.pSetLayouts = empty_layout ? NULL : &set_layout;
   VkPipelineLayout layout;
   if (vkCreatePipelineLayout(device, &lci, NULL, &layout) != VK_SUCCESS) die("vkCreatePipelineLayout failed");
   printf("bindings: %u%s\n", empty_layout ? 0 : m.count, empty_layout ? " (layout forced empty)" : "");

   VkPipeline pipeline = create_pipeline(mod, entry ? entry : m.entry, layout);
   if (pipeline == VK_NULL_HANDLE) return 3;

   if (pass151 || dispatch_mode) {
      Resources r;
      uint32_t image_size = pass151 ? 256 : 256;
      make_resources(&m, set_layout, &r, image_size, pass151);
      transition_all(&r, true, clear_hi);

      uint32_t *words = NULL;
      if (pass151) {
         if (groups_x == 0 && groups_y == 0) { groups_x = 20; groups_y = 12; } /* captured 720p grid */
         const uint32_t valid_w = 160, valid_h = 90;
         int bt = r.buffer_of_binding[257];
         int bs = r.buffer_of_binding[128];
         if (bt < 0 || bs < 0) die("pass151: bindings 257/128 are not buffers");
         uint32_t *tensor = r.buffers[bt].map;
         tensor[70 * 4 + 0] = valid_w;
         tensor[70 * 4 + 1] = valid_h;
         words = r.buffers[bs].map;
         uint32_t max_index = scratch_index(groups_x * 8 - 1, groups_y * 8 - 1) + 1;
         if ((uint64_t)(max_index + 2) * 4 > r.buffers[bs].size) die("pass151: scratch too small");
         for (uint32_t i = 0; i <= max_index + 1; ++i) {
            uint16_t lo = (uint16_t)(0x3800 + (i % 13) * 0x0400); /* 0.5, 1.0, ... 3.5 */
            uint16_t hi = (uint16_t)(0xB800 + (i % 7) * 0x0400);  /* -0.5 ... -3.5 */
            words[i] = pack_halves(lo, hi);
         }
      }

      VkCommandBuffer cb = begin_once();
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &r.set, 0, NULL);
      vkCmdDispatch(cb, groups_x, groups_y, 1);
      submit_and_wait(cb);
      printf("dispatch: %ux%u groups\n", groups_x, groups_y);
      if (device_lost) { printf("device: LOST\n"); return 4; }
      for (uint32_t i = 0; i < r.image_count; ++i) image_stats(&r, i);
      printf("device: alive (vkDeviceWaitIdle=%d)\n", vkDeviceWaitIdle(device));
      if (pass151) {
         int rc = verify_pass151(&r, 129, 130, 160, 90, words);
         if (rc) return 5;
      }
   }
   vkDestroyDevice(device, NULL);
   return 0;
}
