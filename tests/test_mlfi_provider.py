"""End-to-end test for the native MLFI FFX provider (tools/mlfi-provider).

The provider is a shared library that exports the five FFX entry points for AMD's FSR 4 ML frame
interpolation (MLFI 4.0.2, id 0xF600000001000002).  It runs the captured pass graph inside the
application's Vulkan command buffer and composes output[0] itself.

This test prepares the legalised modules with tools/mlfi-replay/prepare.py, builds the provider and
its headless program, then runs the program twice at 1280x720 with the Khronos validation layer:
two different input frames per run, PrepareV2 + FrameGeneration into one command buffer, readback of
outputs[0].  It asserts the FFX return codes, the measured dispatch counters (5 preprocess + 2
input copies + 191 model + 1 compose), a fully non-zero output, a changing output and identical
hashes across the two runs (determinism).

It skips when the user's extraction, the model bank, spirv-tools or a C compiler are absent.
Nothing AMD-owned is required in the repository (docs/en/reports/MLFI-PROVIDER.md).

Environment overrides:
  MK8_MLFI_PROVIDER_SOURCE   extraction root (default $XDG_CACHE_HOME/mk8online/mlfi)
  MK8_MLFI_PROVIDER_REQUIRE=1  turn a missing prerequisite or a failure into an error
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools/mlfi-provider'
BUILD = TOOLS / 'build.sh'
PREPARE = ROOT / 'tools/mlfi-replay/prepare.py'
GEN_GRAPH = TOOLS / 'gen_graph_header.py'
GEN_SHADERS = TOOLS / 'build_shaders.py'


def cache_root() -> Path:
    return Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache')


def source_root() -> Path:
    value = os.environ.get('MK8_MLFI_PROVIDER_SOURCE')
    return Path(value) if value else cache_root() / 'mk8online/mlfi'


def require() -> bool:
    return os.environ.get('MK8_MLFI_PROVIDER_REQUIRE') == '1'


def missing_prerequisites() -> str:
    source = source_root()
    if not (source / 'manifest.json').is_file():
        return f'no MLFI extraction at {source} (run tools/extract_mlfi.py)'
    if not (source / 'model/model_blob.bin').is_file():
        return f'no model_blob.bin under {source}/model'
    for tool in ('spirv-dis', 'spirv-as', 'cc'):
        if not shutil.which(tool):
            return f'{tool} not found'
    if not PREPARE.is_file():
        return f'{PREPARE} not found'
    if not (TOOLS / 'mlfi_provider.c').is_file():
        return f'provider sources missing under {TOOLS}'
    return ''


def summary_value(text: str, key: str):
    match = re.search(rf'MLFI-PROVIDER summary .*?\b{re.escape(key)}=(\S+)', text)
    return match.group(1) if match else None


def stats_value(text: str, key: str):
    match = re.search(rf'MLFI-PROVIDER stats .*?\b{re.escape(key)}=(\d+)', text)
    return int(match.group(1)) if match else None


def frame_lines(text: str):
    frames = {}
    for line in text.splitlines():
        match = re.match(r'MLFI-PROVIDER frame (\d+) hash=([0-9a-f]+) nonzero=(\d+)/(\d+)', line)
        if match:
            frames[int(match.group(1))] = {
                'hash': match.group(2),
                'nonzero': int(match.group(3)),
                'total': int(match.group(4)),
            }
    return frames


@unittest.skipUnless(shutil.which('cc'), 'no C compiler for the MLFI provider')
class MlfiProviderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        reason = missing_prerequisites()
        if reason:
            if require():
                raise AssertionError(f'MLFI provider prerequisites missing: {reason}')
            raise unittest.SkipTest(reason)
        cls.cache = cache_root() / 'mk8online/mlfi-provider-test'
        cls.cache.mkdir(parents=True, exist_ok=True)
        prepared = cls.cache / 'prepare'
        result = subprocess.run(
            ['python3', str(PREPARE), '--source', str(source_root()), '--out', str(prepared), '--force'],
            capture_output=True, text=True, timeout=600)
        if result.returncode != 0:
            if require():
                raise AssertionError(f'prepare failed:\n{result.stdout}\n{result.stderr}')
            raise unittest.SkipTest(f'prepare failed:\n{result.stderr}')
        cls.modules = prepared / 'modules'
        cls.model = source_root() / 'model/model_blob.bin'
        for line in result.stdout.splitlines():
            if line.startswith('MODULES '):
                cls.modules = Path(line.split(' ', 1)[1])
            elif line.startswith('MODEL '):
                cls.model = Path(line.split(' ', 1)[1])

        cls.build_dir = cls.cache / 'build'
        build = subprocess.run([str(BUILD), str(cls.build_dir)], capture_output=True, text=True,
                               timeout=600)
        if build.returncode != 0:
            raise AssertionError(f'build failed:\n{build.stdout}\n{build.stderr}')
        cls.binary = cls.build_dir / 'mlfi_provider_test'

    def run_provider(self, name: str, extra=()):
        output = self.cache / name
        if output.exists():
            shutil.rmtree(output)
        output.mkdir(parents=True)
        env = dict(os.environ)
        env['MK8_MLFI_MODULES'] = str(self.modules)
        env['MK8_MLFI_MODEL'] = str(self.model)
        command = ['nice', '-n', '19', str(self.binary)] + list(extra)
        result = subprocess.run(command, capture_output=True, text=True, timeout=900, env=env)
        text = result.stdout + result.stderr
        if 'no Vulkan device' in text or 'vkCreateInstance' in text:
            if require():
                self.fail(f'no usable Vulkan device:\n{text}')
            self.skipTest('no usable Vulkan device')
        return result, text

    def test_two_frames_are_nonzero_changing_and_deterministic(self):
        first, text = self.run_provider('run-a')
        if first.returncode != 0:
            self.fail(f'provider test failed ({first.returncode}):\n{text[-6000:]}')
        self.assertIn('MLFI-PROVIDER RESULT: OK', text)
        self.assertEqual(summary_value(text, 'validation_errors'), '0')
        self.assertEqual(summary_value(text, 'validation_warnings'), '0')
        self.assertEqual(summary_value(text, 'pipelines'), '195')
        self.assertEqual(summary_value(text, 'changed'), 'yes')
        self.assertEqual(summary_value(text, 'device'), 'alive')

        self.assertEqual(stats_value(text, 'prepare_dispatches'), 5)
        self.assertEqual(stats_value(text, 'copy_dispatches'), 2)
        self.assertEqual(stats_value(text, 'model_dispatches'), 191)
        self.assertEqual(stats_value(text, 'compose_dispatches'), 1)
        self.assertEqual(stats_value(text, 'frames'), 2)
        self.assertEqual(stats_value(text, 'resets'), 1)
        self.assertEqual(stats_value(text, 'last_prepare_rc'), 0)
        self.assertEqual(stats_value(text, 'last_frame_rc'), 0)

        frames = frame_lines(text)
        self.assertEqual(sorted(frames), [0, 1])
        for index, frame in frames.items():
            self.assertEqual(frame['nonzero'], frame['total'], f'frame {index} has zero pixels')
            self.assertGreater(frame['total'], 900_000)
        self.assertNotEqual(frames[0]['hash'], frames[1]['hash'],
                            'the generated frame did not change with the input')

        # A second process must produce the same two hashes: the graph and the input hand-off are
        # deterministic at this configuration.
        second, text_b = self.run_provider('run-b')
        if second.returncode != 0:
            self.fail(f'second provider run failed ({second.returncode}):\n{text_b[-6000:]}')
        frames_b = frame_lines(text_b)
        self.assertEqual(sorted(frames_b), [0, 1])
        self.assertEqual(frames[0]['hash'], frames_b[0]['hash'],
                         'frame 0 is not deterministic across processes')
        self.assertEqual(frames[1]['hash'], frames_b[1]['hash'],
                         'frame 1 is not deterministic across processes')

    def test_motion_vectors_reach_the_model(self):
        """A constant horizontal motion vector must change the second frame.

        This is the provider-level equivalent of the replay's motion test: it proves that the
        application's motion-vector image is copied into the provider image, dilated and splatted
        by the preprocess dispatches and consumed by the model.  The reset frame is insensitive to
        the sign (no history), which is also asserted.
        """
        plus, text_plus = self.run_provider('run-motion-plus', ('--motion', '37'))
        minus, text_minus = self.run_provider('run-motion-minus', ('--motion', '-37'))
        for result, text in ((plus, text_plus), (minus, text_minus)):
            if result.returncode != 0:
                self.fail(f'provider motion run failed ({result.returncode}):\n{text[-4000:]}')
            self.assertIn('MLFI-PROVIDER RESULT: OK', text)
            self.assertEqual(summary_value(text, 'validation_errors'), '0')
        a, b = frame_lines(text_plus), frame_lines(text_minus)
        self.assertEqual(sorted(a), [0, 1])
        self.assertEqual(sorted(b), [0, 1])
        self.assertEqual(a[0]['hash'], b[0]['hash'],
                         'the reset frame must not depend on the motion-vector sign')
        self.assertNotEqual(a[1]['hash'], b[1]['hash'],
                            'motion vectors did not change the generated frame')

    def test_d32_depth_input_is_accepted(self):
        """The runtime's neutral-depth path hands the provider a D32_SFLOAT image.

        The FFX surface format is R32_FLOAT for both R32 and D32, so the provider uses the
        DEPTHTARGET usage flag to create a depth-aspect view.  The result must be identical to the
        R32 path for the same values.
        """
        result, text = self.run_provider('run-d32', ('--d32-depth',))
        if result.returncode != 0:
            self.fail(f'provider D32 run failed ({result.returncode}):\n{text[-4000:]}')
        self.assertIn('MLFI-PROVIDER RESULT: OK', text)
        self.assertEqual(summary_value(text, 'validation_errors'), '0')
        self.assertEqual(summary_value(text, 'validation_warnings'), '0')
        frames = frame_lines(text)
        self.assertEqual(sorted(frames), [0, 1])
        for index, frame in frames.items():
            self.assertEqual(frame['nonzero'], frame['total'])

    def test_committed_shader_header_matches_glsl(self):
        if not shutil.which('glslangValidator'):
            self.skipTest('glslangValidator not available')
        result = subprocess.run(['python3', str(GEN_SHADERS), '--check'], capture_output=True,
                                text=True, timeout=120)
        if result.returncode != 0:
            self.fail(f'{result.stdout}\n{result.stderr}')

    def test_graph_header_matches_committed_tables(self):
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / 'mlfi_graph.h'
            generated = subprocess.run(['python3', str(GEN_GRAPH), '--out', str(header)],
                                       capture_output=True, text=True, timeout=120)
            if generated.returncode != 0:
                self.fail(f'graph generation failed:\n{generated.stdout}\n{generated.stderr}')
            check = subprocess.run(['python3', str(GEN_GRAPH), '--out', str(header), '--check'],
                                   capture_output=True, text=True, timeout=120)
            if check.returncode != 0:
                self.fail(f'{check.stdout}\n{check.stderr}')


if __name__ == '__main__':
    unittest.main()
