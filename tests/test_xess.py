"""XeSS loader/ABI checks and an opt-in Wine smoke test.

The Linux tests compile the shipped runtime/mk8/graphics/xess.cpp with
-DMK8_XESS_PROBE_ONLY and exercise the real PE parser and failure paths. The Windows
DLL is not executed on Linux; the Wine tests load the official libxess.dll, call its
query path and (with MK8_XESS_WINE_VK=1) create a Vulkan device and call
xessVKCreateContext/xessVKInit. They skip cleanly when Wine, the DLL or the SDK zip
is absent.

Environment:
  MK8_XESS_TEST_DLL   explicit path to the official libxess.dll
  MK8_XESS_SDK_INC    directory containing the official xess.h/xess_vk.h
  MK8_XESS_WINE=1     run the Wine query smoke
  MK8_XESS_WINE_VK=1  also run the Vulkan context/init smoke
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
GRAPHICS = ROOT / 'runtime/mk8/graphics'
XESS_CPP = GRAPHICS / 'xess.cpp'
COMPILER = shutil.which('clang++')
MINGW = shutil.which('x86_64-w64-mingw32-g++')
WINE = shutil.which('wine')
ZIP_SHA256 = '88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990'
DLL_SHA256 = '251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7'
DLL_SIZE = 77795704

PROBE_HARNESS = r'''
#include "xess.h"
#include <stdio.h>
int main(void) {
    Mk8XessProbe p;
    bool ok = mk8_xess_probe(&p);
    printf("ok=%d\n", (int)ok);
    printf("status=%s\n", mk8_xess_status_name(p.status));
    printf("platform=%d\n", (int)p.platform_supported);
    printf("dll_present=%d\n", (int)p.dll_present);
    printf("pe_valid=%d\n", (int)p.pe_valid);
    printf("exports_ok=%d\n", (int)p.exports_ok);
    printf("loaded=%d\n", (int)p.loaded);
    printf("licence=%d\n", (int)p.licence_present);
    printf("dll=%s\n", p.dll);
    printf("reason=%s\n", p.reason);
    if (!ok) mk8_xess_log_unavailable(&p);
    return 0;
}
'''

STUB_HARNESS = r'''
#include "xess.h"
#include <stdio.h>
int main(void) {
    printf("enabled_default=%d\n", (int)mk8_xess_enabled());
    VkImageView a = mk8_xess_upscale(VK_NULL_HANDLE, NULL, NULL, 1920, 1080, 2560, 1440, 16.0f);
    VkImageView b = mk8_xess_upscale(VK_NULL_HANDLE, NULL, NULL, 1920, 1080, 2560, 1440, 16.0f);
    printf("a=%p\nb=%p\n", (void *)a, (void *)b);
    return 0;
}
'''

ABI_CHECK = r'''
#include <cstddef>
#include <type_traits>
#include "xess/mk8_xess_abi.h"
#include "xess.h"
#include "xess_vk.h"

static_assert(sizeof(Mk8XessVersion) == sizeof(xess_version_t), "version size");
static_assert(offsetof(Mk8XessVersion, major) == offsetof(xess_version_t, major), "version major");
static_assert(offsetof(Mk8XessVersion, patch) == offsetof(xess_version_t, patch), "version patch");
static_assert(sizeof(Mk8Xess2D) == sizeof(xess_2d_t), "2d size");
static_assert(sizeof(Mk8XessImageView) == sizeof(xess_vk_image_view_info), "view size");
static_assert(offsetof(Mk8XessImageView, imageView) == offsetof(xess_vk_image_view_info, imageView), "view handle");
static_assert(offsetof(Mk8XessImageView, image) == offsetof(xess_vk_image_view_info, image), "view image");
static_assert(offsetof(Mk8XessImageView, subresourceRange) ==
              offsetof(xess_vk_image_view_info, subresourceRange), "view range");
static_assert(offsetof(Mk8XessImageView, format) == offsetof(xess_vk_image_view_info, format), "view format");
static_assert(offsetof(Mk8XessImageView, width) == offsetof(xess_vk_image_view_info, width), "view width");
static_assert(sizeof(Mk8XessExecuteParams) == sizeof(xess_vk_execute_params_t), "execute size");
static_assert(offsetof(Mk8XessExecuteParams, colorTexture) ==
              offsetof(xess_vk_execute_params_t, colorTexture), "execute color");
static_assert(offsetof(Mk8XessExecuteParams, outputTexture) ==
              offsetof(xess_vk_execute_params_t, outputTexture), "execute output");
static_assert(offsetof(Mk8XessExecuteParams, jitterOffsetX) ==
              offsetof(xess_vk_execute_params_t, jitterOffsetX), "execute jitter");
static_assert(offsetof(Mk8XessExecuteParams, inputWidth) ==
              offsetof(xess_vk_execute_params_t, inputWidth), "execute width");
static_assert(offsetof(Mk8XessExecuteParams, outputColorBase) ==
              offsetof(xess_vk_execute_params_t, outputColorBase), "execute base");
static_assert(sizeof(Mk8XessInitParams) == sizeof(xess_vk_init_params_t), "init size");
static_assert(offsetof(Mk8XessInitParams, outputResolution) ==
              offsetof(xess_vk_init_params_t, outputResolution), "init resolution");
static_assert(offsetof(Mk8XessInitParams, qualitySetting) ==
              offsetof(xess_vk_init_params_t, qualitySetting), "init quality");
static_assert(offsetof(Mk8XessInitParams, initFlags) == offsetof(xess_vk_init_params_t, initFlags), "init flags");
static_assert(offsetof(Mk8XessInitParams, tempBufferHeap) ==
              offsetof(xess_vk_init_params_t, tempBufferHeap), "init buffer heap");
static_assert(offsetof(Mk8XessInitParams, pipelineCache) ==
              offsetof(xess_vk_init_params_t, pipelineCache), "init cache");
static_assert(std::is_enum_v<xess_result_t>, "result is an enum");
static_assert(sizeof(xess_result_t) == sizeof(int), "result is int-sized");
static_assert(MK8_XESS_RESULT_SUCCESS == XESS_RESULT_SUCCESS, "result success");
static_assert(MK8_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE == XESS_RESULT_ERROR_UNSUPPORTED_DEVICE, "result device");
static_assert(MK8_XESS_QUALITY_ULTRA_QUALITY == XESS_QUALITY_SETTING_ULTRA_QUALITY, "quality");
static_assert(MK8_XESS_INIT_FLAG_LDR_INPUT_COLOR == XESS_INIT_FLAG_LDR_INPUT_COLOR, "init ldr");
static_assert(MK8_XESS_INIT_FLAG_INVERTED_DEPTH == XESS_INIT_FLAG_INVERTED_DEPTH, "init depth");
int main(void) { return 0; }
'''

WINE_QUERY_HARNESS = r'''
#include "xess.h"
#include <stdio.h>
int main(void) {
    Mk8XessProbe p;
    bool ok = mk8_xess_probe(&p);
    printf("probe_ok=%d\nstatus=%s\nplatform=%d\nloaded=%d\nreason=%s\n",
           (int)ok, mk8_xess_status_name(p.status), (int)p.platform_supported, (int)p.loaded, p.reason);
    uint32_t count = 0, min_api = 0;
    const char *const *names = NULL;
    Mk8XessVersion version = {0, 0, 0, 0};
    char reason[512] = {0};
    int rc = mk8_xess_query(&count, &names, &min_api, &version, reason, sizeof reason);
    printf("query_rc=%d\nversion=%u.%u.%u\ninstance_extensions=%u\nmin_api=0x%x\nquery_reason=%s\n",
           rc, version.major, version.minor, version.patch, count, min_api, reason);
    fflush(stdout);
    return 0;
}
'''

WINE_VK_HARNESS = r'''
#include <windows.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include "xess.h"

typedef int (*CreateCtx)(VkInstance, VkPhysicalDevice, VkDevice, Mk8XessContextHandle *);
typedef int (*InitCtx)(Mk8XessContextHandle, const Mk8XessInitParams *);
typedef int (*DestroyCtx)(Mk8XessContextHandle);
typedef int (*ReqDev)(VkInstance, VkPhysicalDevice, uint32_t *, const char *const **);
typedef int (*ReqFeat)(VkInstance, VkPhysicalDevice, void **);
static void *sym(HMODULE h, const char *n) { return (void *)GetProcAddress(h, n); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { printf("no_vulkan=1\n"); return 2; }
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)sym(vk, "vkCreateInstance");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)sym(vk, "vkGetInstanceProcAddr");
    if (!create || !gipa) { printf("no_vulkan_entry=1\n"); return 2; }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult vr = create(&ici, NULL, &inst);
    printf("vkCreateInstance=%d\n", (int)vr);
    if (vr != VK_SUCCESS) return 3;
    PFN_vkEnumeratePhysicalDevices enumPhys =
        (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qprops =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice createDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    PFN_vkDestroyDevice destroyDevice = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
    PFN_vkDestroyInstance destroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    uint32_t count = 0;
    enumPhys(inst, &count, NULL);
    VkPhysicalDevice phys[8];
    if (count > 8) count = 8;
    enumPhys(inst, &count, phys);
    printf("physical_devices=%u\n", count);
    if (!count) return 4;
    VkPhysicalDeviceProperties p;
    props(phys[0], &p);
    printf("device=%s\napi=%u.%u.%u\n", p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
           VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    uint32_t qcount = 0;
    qprops(phys[0], &qcount, NULL);
    VkQueueFamilyProperties q[16];
    if (qcount > 16) qcount = 16;
    qprops(phys[0], &qcount, q);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < qcount; i++)
        if (q[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
    if (family == UINT32_MAX) { printf("no_compute_queue=1\n"); return 5; }

    Mk8XessProbe probe;
    if (!mk8_xess_probe(&probe)) { printf("probe_failed=%s\n", probe.reason); return 6; }
    HMODULE xess = LoadLibraryA(probe.dll);
    if (!xess) { printf("xess_load_failed=1\n"); return 6; }
    CreateCtx xcreate = (CreateCtx)sym(xess, "xessVKCreateContext");
    InitCtx xinit = (InitCtx)sym(xess, "xessVKInit");
    DestroyCtx xdestroy = (DestroyCtx)sym(xess, "xessDestroyContext");
    ReqDev xdev = (ReqDev)sym(xess, "xessVKGetRequiredDeviceExtensions");
    ReqFeat xfeat = (ReqFeat)sym(xess, "xessVKGetRequiredDeviceFeatures");
    if (!xcreate || !xinit || !xdestroy || !xdev || !xfeat) { printf("missing_entry=1\n"); return 6; }

    uint32_t ecount = 0;
    const char *const *enames = NULL;
    int rc = xdev(inst, phys[0], &ecount, &enames);
    printf("required_device_extensions_rc=%d\ncount=%u\n", rc, ecount);
    for (uint32_t i = 0; i < ecount && i < 16; i++) printf("ext=%s\n", enames[i]);
    void *features = NULL;
    rc = xfeat(inst, phys[0], &features);
    printf("required_device_features_rc=%d\n", rc);
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    float priority = 1.0f;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ecount;
    dci.ppEnabledExtensionNames = enames;
    dci.pNext = features;
    VkDevice dev = VK_NULL_HANDLE;
    vr = createDevice(phys[0], &dci, NULL, &dev);
    printf("vkCreateDevice=%d\n", (int)vr);
    if (vr != VK_SUCCESS) return 7;
    Mk8XessContextHandle ctx = NULL;
    rc = xcreate(inst, phys[0], dev, &ctx);
    printf("xessVKCreateContext=%d\n", rc);
    if (ctx) {
        Mk8XessInitParams ip = {0};
        ip.outputResolution.x = 2560;
        ip.outputResolution.y = 1440;
        ip.qualitySetting = MK8_XESS_QUALITY_ULTRA_QUALITY;
        ip.initFlags = MK8_XESS_INIT_FLAG_LDR_INPUT_COLOR;
        ip.creationNodeMask = 1;
        ip.visibleNodeMask = 1;
        rc = xinit(ctx, &ip);
        printf("xessVKInit=%d\n", rc);
        xdestroy(ctx);
    }
    destroyDevice(dev, NULL);
    destroyInstance(inst, NULL);
    return 0;
}
'''


def _sha256(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def _roots():
    configured = os.environ.get('XDG_DATA_HOME')
    if configured:
        yield Path(configured) / 'mk8online/intel'
    yield Path.home() / '.local/share/mk8online/intel'
    yield Path.home() / 'Downloads/mkdx-sdks'


def _find_zip() -> Path | None:
    for root in _roots():
        for candidate in (root / 'XeSS_SDK_3.0.2.zip', root / 'xess-3.0.2-work.zip'):
            if candidate.is_file() and _sha256(candidate) == ZIP_SHA256:
                return candidate
    return None


def _find_dll() -> Path | None:
    configured = os.environ.get('MK8_XESS_TEST_DLL')
    if configured and Path(configured).is_file():
        return Path(configured)
    for root in _roots():
        for candidate in (root / 'libxess.dll', root / 'bin/libxess.dll',
                          root / 'xess-3.0.2-work/bin/libxess.dll'):
            if candidate.is_file():
                return candidate
    return None


class XessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not COMPILER:
            raise unittest.SkipTest('clang++ is not available')
        scratch = ROOT / 'target'
        cls.temp = tempfile.TemporaryDirectory(
            prefix='mk8-xess-', dir=str(scratch) if scratch.is_dir() else None)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        harness = cls.work / 'probe.cpp'
        harness.write_text(PROBE_HARNESS, encoding='utf-8')
        cls.probe = cls.work / 'probe'
        result = subprocess.run(
            [COMPILER, '-std=c++17', '-O2', '-DMK8_XESS_PROBE_ONLY', '-I', str(GRAPHICS),
             str(harness), str(XESS_CPP), '-o', str(cls.probe)],
            capture_output=True, text=True, timeout=180)
        if result.returncode:
            raise AssertionError('cannot build the XeSS probe:\n' + result.stderr[-4000:])

        stub = cls.work / 'stub.cpp'
        stub.write_text(STUB_HARNESS, encoding='utf-8')
        cls.stub = cls.work / 'stub'
        result = subprocess.run(
            [COMPILER, '-std=c++17', '-O2', '-I', str(GRAPHICS), str(stub), str(XESS_CPP),
             '-o', str(cls.stub)],
            capture_output=True, text=True, timeout=180)
        if result.returncode:
            raise AssertionError('cannot build the XeSS stub:\n' + result.stderr[-4000:])

        cls.zip = _find_zip()
        cls.dll = _find_dll()
        if cls.dll is None and cls.zip is not None:
            with zipfile.ZipFile(cls.zip) as archive:
                cls.dll = Path(archive.extract('bin/libxess.dll', cls.work / 'sdk'))
        cls.dx11 = None
        if cls.zip is not None:
            with zipfile.ZipFile(cls.zip) as archive:
                cls.dx11 = Path(archive.extract('bin/libxess_dx11.dll', cls.work / 'sdk'))
        cls.sdk_inc = cls._find_headers()

    @classmethod
    def _find_headers(cls) -> Path | None:
        configured = os.environ.get('MK8_XESS_SDK_INC')
        candidates = [Path(configured)] if configured else []
        for root in _roots():
            candidates += [root / 'xess-sdk/inc/xess', root / 'inc/xess']
        for candidate in candidates:
            if (candidate / 'xess.h').is_file() and (candidate / 'xess_vk.h').is_file():
                return candidate
        if cls.zip is not None:
            destination = cls.work / 'sdkinc'
            with zipfile.ZipFile(cls.zip) as archive:
                for name in ('inc/xess/xess.h', 'inc/xess/xess_vk.h'):
                    archive.extract(name, destination)
            candidate = destination / 'inc/xess'
            if (candidate / 'xess.h').is_file():
                return candidate
        return None

    def environment(self, **values):
        env = os.environ.copy()
        for name in ('MK8_XESS_LIB', 'MK8_XESS_DIR', 'MK8_XESS', 'MK8_XESS_DEPTH_INVERTED'):
            env.pop(name, None)
        env.update(values)
        return env

    def run_probe(self, **values):
        result = subprocess.run([str(self.probe)], env=self.environment(**values),
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        fields = {}
        for line in result.stdout.splitlines():
            key, _, value = line.partition('=')
            fields[key] = value
        return fields, result.stderr

    def test_no_dll_names_the_directory_and_the_fetcher(self):
        with tempfile.TemporaryDirectory(prefix='mk8-xess-empty-', dir=str(self.work)) as empty:
            fields, stderr = self.run_probe(MK8_XESS_DIR=empty)
        self.assertEqual(fields['status'], 'no-dll')
        self.assertEqual(fields['ok'], '0')
        self.assertIn('fetch_vendor_sdk.py', fields['reason'])
        self.assertIn('--accept-license intel', fields['reason'])
        self.assertIn('[xess] XeSS unavailable: no libxess.dll', stderr)

    def test_corrupt_dll_is_rejected_without_crashing(self):
        bad = self.work / 'libxess.dll'
        bad.write_bytes(b'MZ' + b'\x00\xde\xad\xbe\xef' * 8)
        fields, _ = self.run_probe(MK8_XESS_LIB=str(bad))
        self.assertEqual(fields['status'], 'dll-load-failed')
        self.assertEqual(fields['pe_valid'], '0')
        self.assertIn(str(bad), fields['reason'])
        self.assertIn('not a complete PE32+ x86-64 DLL', fields['reason'])

    def test_truncated_real_dll_is_rejected(self):
        if self.dll is None:
            self.skipTest('the official libxess.dll is not present')
        with tempfile.TemporaryDirectory(prefix='mk8-xess-trunc-', dir=str(self.work)) as folder:
            truncated = Path(folder) / 'libxess.dll'
            truncated.write_bytes(self.dll.read_bytes()[:4096])
            fields, _ = self.run_probe(MK8_XESS_LIB=str(truncated))
        self.assertEqual(fields['status'], 'dll-load-failed')
        self.assertIn(str(truncated), fields['reason'])
        self.assertIn('section', fields['reason'].lower())

    def test_a_dll_without_xess_vulkan_exports_is_rejected(self):
        if self.dx11 is None:
            self.skipTest('libxess_dx11.dll is not available')
        fields, _ = self.run_probe(MK8_XESS_LIB=str(self.dx11))
        self.assertEqual(fields['status'], 'dll-exports-missing')
        self.assertEqual(fields['pe_valid'], '1')
        self.assertEqual(fields['exports_ok'], '0')
        self.assertIn('xessVKCreateContext', fields['reason'])

    def test_official_dll_matches_the_pinned_hash(self):
        if self.dll is None:
            self.skipTest('the official libxess.dll is not present')
        if self.dll.stat().st_size != DLL_SIZE:
            self.skipTest('the cached libxess.dll is not the 3.0.2 artifact')
        self.assertEqual(_sha256(self.dll), DLL_SHA256)

    def test_real_dll_reports_exports_then_linux_is_unsupported(self):
        if self.dll is None:
            self.skipTest('the official libxess.dll is not present')
        fields, stderr = self.run_probe(MK8_XESS_LIB=str(self.dll))
        self.assertEqual(fields['status'], 'unsupported-platform')
        self.assertEqual(fields['platform'], '0')
        self.assertEqual(fields['dll_present'], '1')
        self.assertEqual(fields['pe_valid'], '1')
        self.assertEqual(fields['exports_ok'], '1')
        self.assertEqual(fields['loaded'], '0')
        self.assertIn('Windows-only', fields['reason'])
        self.assertIn('[xess] XeSS unavailable: XeSS is Windows-only', stderr)
        self.assertEqual(len([line for line in stderr.splitlines() if line.startswith('[xess]')]), 1)

    def test_linux_upscale_is_a_one_line_no_op(self):
        result = subprocess.run([str(self.stub)], env=self.environment(),
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('enabled_default=0', result.stdout)
        self.assertIn('a=(nil)', result.stdout)
        lines = [line for line in result.stderr.splitlines() if line.startswith('[xess]')]
        self.assertEqual(len(lines), 1, result.stderr)
        self.assertIn('Windows-only', lines[0])

    def test_gate_is_exactly_non_empty_and_not_zero(self):
        for value, expected in [('1', '1'), ('yes', '1'), ('0', '0')]:
            result = subprocess.run([str(self.stub)], env=self.environment(MK8_XESS=value),
                                    capture_output=True, text=True, timeout=120)
            self.assertIn(f'enabled_default={expected}', result.stdout, value)

    def test_abi_declarations_match_the_official_headers(self):
        if self.sdk_inc is None:
            self.skipTest('the official XeSS headers are not present')
        source = self.work / 'abi.cpp'
        source.write_text(ABI_CHECK, encoding='utf-8')
        binary = self.work / 'abi'
        result = subprocess.run(
            [COMPILER, '-std=c++17', '-O0', '-I', str(self.sdk_inc), '-I', str(GRAPHICS),
             str(source), '-o', str(binary)],
            capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stderr[-6000:])
        self.assertEqual(subprocess.run([str(binary)]).returncode, 0)

    def _compile_wine(self, source: str, name: str) -> Path | None:
        windows_include = self.work / 'vkinc'
        if not (windows_include / 'vulkan/vulkan.h').is_file():
            source_root = Path('/usr/include')
            if not (source_root / 'vulkan/vulkan.h').is_file():
                return None
            shutil.copytree(source_root / 'vulkan', windows_include / 'vulkan')
            shutil.copytree(source_root / 'vk_video', windows_include / 'vk_video')
        text = self.work / f'{name}.cpp'
        text.write_text(source, encoding='utf-8')
        binary = self.work / f'{name}.exe'
        result = subprocess.run(
            [MINGW, '-std=c++17', '-O2', '-static', '-DMK8_XESS_PROBE_ONLY',
             '-I', str(windows_include), '-I', str(GRAPHICS), str(text), str(XESS_CPP),
             '-o', str(binary)],
            capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stderr[-6000:])
        return binary

    @unittest.skipUnless(os.environ.get('MK8_XESS_WINE') == '1',
                         'set MK8_XESS_WINE=1 to run the Wine smoke')
    def test_wine_loads_the_dll_and_calls_the_query_path(self):
        if WINE is None or MINGW is None:
            self.skipTest('wine or x86_64-w64-mingw32-g++ is not available')
        if self.dll is None:
            self.skipTest('the official libxess.dll is not present')
        binary = self._compile_wine(WINE_QUERY_HARNESS, 'wine_query')
        if binary is None:
            self.skipTest('Vulkan headers for the Windows target are not available')
        env = self.environment(MK8_XESS_LIB='Z:' + str(self.dll.resolve()).replace('/', '\\'),
                               WINEDEBUG='-all')
        result = subprocess.run([WINE, str(binary)], env=env, capture_output=True, text=True,
                                timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        fields = {}
        for line in result.stdout.splitlines():
            key, _, value = line.partition('=')
            fields[key] = value
        self.assertEqual(fields.get('probe_ok'), '1', result.stdout + result.stderr)
        self.assertEqual(fields.get('status'), 'ok')
        self.assertEqual(fields.get('loaded'), '1')
        self.assertEqual(fields.get('query_rc'), '0')
        self.assertNotEqual(fields.get('version'), '0.0.0')
        self.assertEqual(fields.get('min_api'), '0x403000')

    @unittest.skipUnless(os.environ.get('MK8_XESS_WINE_VK') == '1',
                         'set MK8_XESS_WINE_VK=1 to run the Vulkan init smoke')
    def test_wine_creates_a_vulkan_device_and_initialises_xess(self):
        if WINE is None or MINGW is None:
            self.skipTest('wine or x86_64-w64-mingw32-g++ is not available')
        if self.dll is None:
            self.skipTest('the official libxess.dll is not present')
        binary = self._compile_wine(WINE_VK_HARNESS, 'wine_vk')
        if binary is None:
            self.skipTest('Vulkan headers for the Windows target are not available')
        env = self.environment(MK8_XESS_LIB='Z:' + str(self.dll.resolve()).replace('/', '\\'),
                               WINEDEBUG='-all')
        result = subprocess.run([WINE, str(binary)], env=env, capture_output=True, text=True,
                                timeout=600)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('vkCreateInstance=0', result.stdout)
        self.assertIn('vkCreateDevice=0', result.stdout)
        self.assertIn('xessVKCreateContext=0', result.stdout)
        self.assertIn('xessVKInit=0', result.stdout)


if __name__ == '__main__':
    unittest.main()
