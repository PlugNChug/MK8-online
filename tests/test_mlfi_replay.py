"""End-to-end test for the native MLFI replay (tools/mlfi-replay/mlfi_replay.c).

The replay runs the complete captured ML frame-interpolation pass graph — five preprocess dispatches
and pass0..pass190 — on a headless RADV device and reads the generated frame back. This test drives
the real program at 1280x720: it legalises the extracted modules with
tools/radv-mlfi/fix_image_types.py, builds the executable, runs two frames with the Khronos
validation layer, and asserts the summary, a non-trivial output that changes with the input, and a
deterministic steady state when the input is frozen.

It skips when the user's extraction, the model bank, the Wine capture, spirv-tools or a C compiler
are absent; nothing AMD-owned is required in the repository (see docs/MLFI-REPLAY.md).

Environment overrides:
  MK8_MLFI_REPLAY_SOURCE   extraction root (default $XDG_CACHE_HOME/mk8online/mlfi)
  MK8_MLFI_REPLAY_CAPTURE  capture root (default .../mlfi-capture/canonical-4k-1f)
  MK8_MLFI_REPLAY_REQUIRE=1 turn a missing prerequisite or a runtime failure into a failure
"""
import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PREPARE = ROOT / 'tools/mlfi-replay/prepare.py'
BUILD = ROOT / 'tools/mlfi-replay/build.sh'
FIXER = ROOT / 'tools/radv-mlfi/fix_image_types.py'


def cache_root() -> Path:
    return Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache')


def source_root() -> Path:
    value = os.environ.get('MK8_MLFI_REPLAY_SOURCE')
    return Path(value) if value else cache_root() / 'mk8online/mlfi'


def capture_root() -> Path:
    value = os.environ.get('MK8_MLFI_REPLAY_CAPTURE')
    return Path(value) if value else cache_root() / 'mk8online/mlfi-capture/canonical-4k-1f'


def require() -> bool:
    return os.environ.get('MK8_MLFI_REPLAY_REQUIRE') == '1'


def missing_prerequisites() -> str:
    source = source_root()
    capture = capture_root()
    if not (source / 'manifest.json').is_file():
        return f'no MLFI extraction at {source} (run tools/extract_mlfi.py)'
    if not (source / 'model/model_blob.bin').is_file():
        return f'no model_blob.bin under {source}/model'
    if not (capture / 'raw').is_dir():
        return f'no Wine capture at {capture}'
    for tool in ('spirv-dis', 'spirv-as', 'cc'):
        if not shutil.which(tool):
            return f'{tool} not found'
    if not FIXER.is_file():
        return f'{FIXER} not found'
    return ''


def frame_lines(text: str):
    lines = {}
    for line in text.splitlines():
        match = re.match(r'MLFI-REPLAY frame (\d+) hash=([0-9a-f]+) nonzero=(\d+)/(\d+)', line)
        if match:
            lines[int(match.group(1))] = {
                'hash': match.group(2),
                'nonzero': int(match.group(3)),
                'total': int(match.group(4)),
            }
    return lines


def summary_value(text: str, key: str):
    match = re.search(rf'MLFI-REPLAY summary .*?\b{re.escape(key)}=(\S+)', text)
    return match.group(1) if match else None


@unittest.skipUnless(shutil.which('cc'), 'no C compiler for the MLFI replay')
class MlfiReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        reason = missing_prerequisites()
        if reason:
            if require():
                raise unittest.SkipTest(f'MLFI replay prerequisites missing: {reason}')
            raise unittest.SkipTest(reason)
        cls.cache = cache_root() / 'mk8online/mlfi-replay-test'
        cls.cache.mkdir(parents=True, exist_ok=True)
        cls.modules = cls.cache / 'modules'
        result = subprocess.run(
            ['python3', str(PREPARE), '--out', str(cls.cache / 'prepare'), '--force'],
            capture_output=True, text=True, timeout=600)
        if result.returncode != 0:
            if require():
                raise AssertionError(f'prepare failed:\n{result.stdout}\n{result.stderr}')
            raise unittest.SkipTest(f'prepare failed:\n{result.stderr}')
        for line in result.stdout.splitlines():
            if line.startswith('MODULES '):
                cls.modules = Path(line.split(' ', 1)[1])
            elif line.startswith('MODEL '):
                cls.model = Path(line.split(' ', 1)[1])
        build = subprocess.run([str(BUILD), str(cls.cache / 'build')],
                               capture_output=True, text=True, timeout=600)
        if build.returncode != 0:
            raise AssertionError(f'build failed:\n{build.stderr}')
        cls.binary = cls.cache / 'build/mlfi_replay'
        cls.capture = capture_root()

    def run_replay(self, name: str, frames: int, extra=()):
        out = self.cache / name
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        command = ['nice', '-n', '19', str(self.binary),
                   '--modules', str(self.modules),
                   '--model', str(self.model),
                   '--capture', str(self.capture),
                   '--out', str(out),
                   '--frames', str(frames)] + list(extra)
        result = subprocess.run(command, capture_output=True, text=True, timeout=900)
        text = result.stdout + result.stderr
        if 'no Vulkan physical device' in text or 'vkCreateInstance failed' in text:
            if require():
                self.fail(f'no usable Vulkan device:\n{text}')
            self.skipTest('no usable Vulkan device')
        return result, text, out

    def test_full_pass_graph_produces_a_changing_frame(self):
        result, text, out = self.run_replay('run-moving', 2)
        if result.returncode != 0:
            self.fail(f'replay failed ({result.returncode}):\n{text[-4000:]}')
        self.assertIn('MLFI-REPLAY RESULT: OK', text)
        self.assertEqual(summary_value(text, 'validation_errors'), '0')
        self.assertEqual(summary_value(text, 'pipelines'), '195')
        self.assertEqual(summary_value(text, 'dispatches'), '196')
        self.assertEqual(summary_value(text, 'changed'), 'yes')

        frames = frame_lines(text)
        self.assertEqual(sorted(frames), [0, 1])
        for index, frame in frames.items():
            self.assertEqual(frame['nonzero'], frame['total'],
                             f'frame {index} has zero pixels')
            self.assertGreater(frame['total'], 900_000)
        self.assertNotEqual(frames[0]['hash'], frames[1]['hash'],
                            'the generated frame did not change with the input')

        for name in ('prediction_f0.png', 'prediction_f1.png', 'prediction_f0.raw'):
            path = out / name
            self.assertTrue(path.is_file(), f'{name} missing')
            self.assertGreater(path.stat().st_size, 1 << 20, f'{name} is suspiciously small')

    def test_motion_vectors_reach_the_model(self):
        """A constant non-zero motion-vector image must change the generated frame.

        With zero motion vectors the splat/flow path is degenerate, so this is the cheap check
        that the preprocess wiring (dilated MV -> splat -> model) is actually connected.  The
        reset frame (0) is identical for both signs; the second frame must differ.
        """
        plus = self.run_replay('run-motion-plus', 2, ('--motion', '37'))[1]
        minus = self.run_replay('run-motion-minus', 2, ('--motion', '-37'))[1]
        for text in (plus, minus):
            self.assertIn('MLFI-REPLAY RESULT: OK', text)
            self.assertEqual(summary_value(text, 'validation_errors'), '0')
        a, b = frame_lines(plus), frame_lines(minus)
        self.assertEqual(sorted(a), [0, 1])
        self.assertEqual(a[0]['hash'], b[0]['hash'], 'the reset frame must not depend on the MV history')
        self.assertNotEqual(a[1]['hash'], b[1]['hash'],
                            'motion vectors did not change the generated frame')

    def test_frozen_input_reaches_a_deterministic_steady_state(self):
        result, text, _ = self.run_replay('run-static', 3, ('--static-input',))
        if result.returncode != 0:
            self.fail(f'replay failed ({result.returncode}):\n{text[-4000:]}')
        frames = frame_lines(text)
        self.assertEqual(sorted(frames), [0, 1, 2])
        # Frame 0 is the reset frame (empty packed-splat history); frames 1 and 2 must be identical.
        self.assertNotEqual(frames[0]['hash'], frames[1]['hash'])
        self.assertEqual(frames[1]['hash'], frames[2]['hash'],
                         'frozen input did not converge to a fixed output')


if __name__ == '__main__':
    unittest.main()
