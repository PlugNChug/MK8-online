"""DLSS loader failure paths and ABI conformance, exercised natively.

No NVIDIA GPU is needed: the probe only resolves paths, dlopens the NGX feature plugin and
core, and checks their exports. The tests run the real dlss.cpp compiled with
-DMK8_DLSS_PROBE_ONLY, so they exercise the code that ships rather than searching its text.

Cases: no plugin, corrupted/truncated plugin, a library without NGX exports, deterministic
directory/version selection, licence detection, core override, the fetched NVIDIA plugin
(skipped when absent) and a static_assert comparison of ngx/mk8_ngx_abi.h against the
official SDK headers (skipped when the downloader has not fetched them).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GRAPHICS = ROOT / 'runtime/mk8/graphics'
DLSS_CPP = GRAPHICS / 'dlss.cpp'
COMPILER = shutil.which('clang++')

HARNESS = r'''
#include "dlss.h"
#include <stdio.h>

int main(void) {
    Mk8DlssProbe probe;
    bool ok = mk8_dlss_probe(&probe);
    printf("ok=%d\n", (int)ok);
    printf("status=%s\n", mk8_dlss_status_name(probe.status));
    printf("plugin_loaded=%d\n", (int)probe.plugin_loaded);
    printf("plugin_exports_ok=%d\n", (int)probe.plugin_exports_ok);
    printf("core_loaded=%d\n", (int)probe.core_loaded);
    printf("core_exports_ok=%d\n", (int)probe.core_exports_ok);
    printf("licence_present=%d\n", (int)probe.licence_present);
    printf("directory=%s\n", probe.directory);
    printf("plugin=%s\n", probe.plugin);
    printf("core=%s\n", probe.core);
    printf("reason=%s\n", probe.reason);
    if (!ok) mk8_dlss_log_unavailable(&probe);
    return 0;
}
'''

ABI_CHECK = r'''
#include <cstddef>
#include "ngx/mk8_ngx_abi.h"
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"

static_assert(sizeof(Mk8NgxResourceVk) == sizeof(NVSDK_NGX_Resource_VK), "resource size");
static_assert(offsetof(Mk8NgxResourceVk, Type) == offsetof(NVSDK_NGX_Resource_VK, Type), "resource type");
static_assert(offsetof(Mk8NgxResourceVk, ReadWrite) == offsetof(NVSDK_NGX_Resource_VK, ReadWrite), "resource rw");
static_assert(offsetof(Mk8NgxResourceVk, Resource.ImageViewInfo.ImageView) ==
              offsetof(NVSDK_NGX_Resource_VK, Resource.ImageViewInfo.ImageView), "resource view");
static_assert(offsetof(Mk8NgxResourceVk, Resource.ImageViewInfo.SubresourceRange) ==
              offsetof(NVSDK_NGX_Resource_VK, Resource.ImageViewInfo.SubresourceRange), "resource range");
static_assert(sizeof(Mk8NgxFeatureCommonInfo) == sizeof(NVSDK_NGX_FeatureCommonInfo), "common size");
static_assert(offsetof(Mk8NgxFeatureCommonInfo, PathListInfo) == offsetof(NVSDK_NGX_FeatureCommonInfo, PathListInfo), "common path");
static_assert(offsetof(Mk8NgxFeatureCommonInfo, InternalData) == offsetof(NVSDK_NGX_FeatureCommonInfo, InternalData), "common internal");
static_assert(offsetof(Mk8NgxFeatureCommonInfo, LoggingInfo) == offsetof(NVSDK_NGX_FeatureCommonInfo, LoggingInfo), "common logging");
static_assert(sizeof(Mk8NgxFeatureDiscoveryInfo) == sizeof(NVSDK_NGX_FeatureDiscoveryInfo), "discovery size");
static_assert(offsetof(Mk8NgxFeatureDiscoveryInfo, ApplicationDataPath) ==
              offsetof(NVSDK_NGX_FeatureDiscoveryInfo, ApplicationDataPath), "discovery path");
static_assert(offsetof(Mk8NgxFeatureDiscoveryInfo, FeatureInfo) ==
              offsetof(NVSDK_NGX_FeatureDiscoveryInfo, FeatureInfo), "discovery info");
static_assert(sizeof(Mk8NgxFeatureRequirement) == sizeof(NVSDK_NGX_FeatureRequirement), "requirement size");
static_assert(sizeof(Mk8NgxDlssCreateParams) == sizeof(NVSDK_NGX_DLSS_Create_Params), "create size");
static_assert(offsetof(Mk8NgxDlssCreateParams, InFeatureCreateFlags) ==
              offsetof(NVSDK_NGX_DLSS_Create_Params, InFeatureCreateFlags), "create flags");
static_assert(MK8_NGX_API_VERSION == NVSDK_NGX_Version_API, "API version");
int main(void) { return 0; }
'''


def fetched_ngx_root() -> Path:
    base = os.environ.get('XDG_DATA_HOME')
    root = Path(base) if base else Path.home() / '.local/share'
    return root / 'mk8online/nvidia'


class DlssProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform != 'linux':
            raise unittest.SkipTest('the DLSS probe build is exercised on Linux')
        if not COMPILER:
            raise unittest.SkipTest('clang++ is not available')
        cls.temp = tempfile.TemporaryDirectory(prefix='mk8-dlss-probe-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        harness = cls.work / 'harness.cpp'
        harness.write_text(HARNESS, encoding='utf-8')
        cls.probe = cls.work / 'probe'
        result = subprocess.run(
            [COMPILER, '-std=c++17', '-O2', '-DMK8_DLSS_PROBE_ONLY', '-I', str(GRAPHICS),
             str(harness), str(DLSS_CPP), '-o', str(cls.probe), '-ldl'],
            capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError('cannot build the DLSS probe:\n' + result.stderr[-4000:])
        cls.dummy = cls.work / 'libnvidia-ngx-dlss.so.310.9.16'
        source = cls.work / 'dummy.c'
        source.write_text('int mk8_dummy_symbol;\n', encoding='utf-8')
        result = subprocess.run(['clang', '-shared', '-fPIC', str(source), '-o', str(cls.dummy)],
                                capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError('cannot build the dummy plugin:\n' + result.stderr[-4000:])
        cls.fake_plugin = cls.work / 'libnvidia-ngx-dlss.so.fake'
        source.write_text(
            'int NVSDK_NGX_VULKAN_CreateFeature1(void) { return 0; }\n'
            'int NVSDK_NGX_VULKAN_CreateFeature(void) { return 0; }\n'
            'int NVSDK_NGX_VULKAN_EvaluateFeature(void) { return 0; }\n'
            'int NVSDK_NGX_VULKAN_ReleaseFeature(void) { return 0; }\n',
            encoding='utf-8')
        result = subprocess.run(['clang', '-shared', '-fPIC', str(source), '-o', str(cls.fake_plugin)],
                                capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError('cannot build the fake plugin:\n' + result.stderr[-4000:])

    def environment(self, data_home: Path, **values: str) -> dict:
        env = os.environ.copy()
        for name in ('MK8_DLSS_DIR', 'MK8_DLSS_LIB', 'MK8_DLSS_CORE'):
            env.pop(name, None)
        env['XDG_DATA_HOME'] = str(data_home)
        env.update(values)
        return env

    def run_probe(self, data_home: Path, **values: str) -> tuple[dict, str]:
        result = subprocess.run([str(self.probe)], env=self.environment(data_home, **values),
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        fields = {}
        for line in result.stdout.splitlines():
            key, _, value = line.partition('=')
            fields[key] = value
        return fields, result.stderr

    def test_no_plugin_names_the_directory_and_the_fetcher(self):
        with tempfile.TemporaryDirectory(prefix='mk8-dlss-empty-') as data:
            fields, stderr = self.run_probe(Path(data))
        self.assertEqual(fields['status'], 'no-plugin')
        self.assertEqual(fields['ok'], '0')
        self.assertIn('no DLSS plugin', fields['reason'])
        self.assertIn('mk8online/nvidia', fields['reason'])
        self.assertIn('fetch_vendor_sdk.py', fields['reason'])
        self.assertIn('[dlss] DLSS unavailable: no DLSS plugin', stderr)
        self.assertEqual(len([line for line in stderr.splitlines() if line.startswith('[dlss]')]), 1)

    def test_corrupted_plugin_reports_an_error_without_crashing(self):
        with tempfile.TemporaryDirectory(prefix='mk8-dlss-bad-') as data:
            bad = Path(data) / 'libnvidia-ngx-dlss.so.0'
            bad.write_bytes(b'\x7fELF' + b'\x00\xde\xad\xbe\xef' * 8)
            fields, _ = self.run_probe(Path(data), MK8_DLSS_LIB=str(bad))
        self.assertEqual(fields['status'], 'plugin-load-failed')
        self.assertEqual(fields['plugin_loaded'], '0')
        self.assertIn(str(bad), fields['reason'])
        self.assertIn('did not load', fields['reason'])

    def test_truncated_real_plugin_is_rejected(self):
        real = fetched_ngx_root() / 'libnvidia-ngx-dlss.so.310.9.16'
        if not real.is_file():
            self.skipTest('fetched DLSS plugin is not present')
        with tempfile.TemporaryDirectory(prefix='mk8-dlss-trunc-') as data:
            truncated = Path(data) / 'libnvidia-ngx-dlss.so.0'
            truncated.write_bytes(real.read_bytes()[:4096])
            fields, _ = self.run_probe(Path(data), MK8_DLSS_LIB=str(truncated))
        self.assertEqual(fields['status'], 'plugin-load-failed')
        self.assertIn(str(truncated), fields['reason'])

    def test_a_library_without_ngx_exports_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='mk8-dlss-wrong-') as data:
            fields, _ = self.run_probe(Path(data), MK8_DLSS_LIB=str(self.dummy))
        self.assertEqual(fields['status'], 'plugin-exports-missing')
        self.assertEqual(fields['plugin_loaded'], '1')
        self.assertIn('NVSDK_NGX_VULKAN_EvaluateFeature', fields['reason'])
        self.assertIn('NVSDK_NGX_VULKAN_CreateFeature1', fields['reason'])

    def test_directory_scan_prefers_the_higher_version_and_reports_the_licence(self):
        with tempfile.TemporaryDirectory(prefix='mk8-dlss-dir-') as data:
            directory = Path(data) / 'mk8online/nvidia'
            directory.mkdir(parents=True)
            (directory / 'libnvidia-ngx-dlss.so.0').write_bytes(b'not an ELF object')
            shutil.copy(self.fake_plugin, directory / 'libnvidia-ngx-dlss.so.310.9.16')
            (directory / 'LICENSE.txt').write_text('NVIDIA RTX SDKs License (test fixture)\n', encoding='utf-8')
            fields, _ = self.run_probe(Path(data))
        self.assertEqual(fields['status'], 'no-core')  # the fake plugin's exports are enough
        self.assertEqual(fields['plugin_loaded'], '1')
        self.assertEqual(fields['plugin_exports_ok'], '1')
        self.assertEqual(fields['licence_present'], '1')
        self.assertTrue(fields['plugin'].endswith('libnvidia-ngx-dlss.so.310.9.16'), fields['plugin'])
        self.assertIn('no NGX core library', fields['reason'])

    def test_a_core_override_without_vulkan_exports_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='mk8-dlss-core-') as data:
            fields, _ = self.run_probe(Path(data), MK8_DLSS_LIB=str(self.fake_plugin),
                                       MK8_DLSS_CORE=str(self.dummy))
        self.assertEqual(fields['status'], 'core-exports-missing')
        self.assertEqual(fields['plugin_exports_ok'], '1')
        self.assertEqual(fields['core_loaded'], '1')
        self.assertIn('NVSDK_NGX_VULKAN_Init_with_ProjectID', fields['reason'])

    def test_fetched_plugin_loads_and_exports_the_vulkan_entry_points(self):
        directory = fetched_ngx_root()
        plugin = directory / 'libnvidia-ngx-dlss.so.310.9.16'
        if not plugin.is_file():
            self.skipTest('fetched DLSS plugin is not present')
        with tempfile.TemporaryDirectory(prefix='mk8-dlss-real-') as data:
            fields, _ = self.run_probe(Path(data), MK8_DLSS_DIR=str(directory))
        self.assertEqual(fields['plugin_loaded'], '1')
        self.assertEqual(fields['plugin_exports_ok'], '1')
        self.assertTrue(fields['plugin'].endswith(plugin.name), fields['plugin'])
        self.assertIn(fields['status'],
                      {'ok', 'no-core', 'core-load-failed', 'core-exports-missing'})
        if fields['status'] != 'ok':
            self.assertEqual(fields['core_loaded'], '0')
            self.assertIn('NGX core', fields['reason'])

    def test_abi_declarations_match_the_official_headers(self):
        directory = fetched_ngx_root() / 'ngx/include'
        if not (directory / 'nvsdk_ngx_vk.h').is_file():
            self.skipTest('fetched NGX headers are not present')
        source = self.work / 'abi.cpp'
        source.write_text(ABI_CHECK, encoding='utf-8')
        binary = self.work / 'abi'
        result = subprocess.run(
            [COMPILER, '-std=c++17', '-O0', '-I', str(GRAPHICS), '-I', str(directory),
             str(source), '-o', str(binary)],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr[-6000:])
        self.assertEqual(subprocess.run([str(binary)]).returncode, 0)


if __name__ == '__main__':
    unittest.main()
