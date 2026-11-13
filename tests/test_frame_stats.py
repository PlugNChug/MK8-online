"""Analisa contadores de memória com quadros conhecidos e logs autorais."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('frame_stats', ROOT/'scripts/frame-stats.py')
stats = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stats)


class MemoryStatsTests(unittest.TestCase):
    def test_preview_window_is_excluded_even_without_completed_output(self):
        with tempfile.TemporaryDirectory() as folder:
            log=Path(folder)/'game.log'
            text=''.join(f'[frame] {i} {100 if 97<=i<=163 else 16} 3 1 1 11\n' for i in range(90,171))
            log.write_text(text+'[gfx-preview] start host frame 100\n[gfx-preview] end host frame 160\n[gfx-incident] capture failed\n')
            result=stats.stats(log,90,170,0)
            self.assertIn('14 frames',result); self.assertIn('max 16.00 ms',result)
            log.write_text(text+'[gfx-preview] start host frame 100\n')
            self.assertIn('7 frames',stats.stats(log,90,170,0))

    def test_incident_capture_disturbance_is_excluded_even_if_writing_failed(self):
        with tempfile.TemporaryDirectory() as folder:
            log=Path(folder)/'game.log'
            text=''.join(f'[frame] {i} {200 if 97 <= i <= 103 else 16} 3 1 1 11\n' for i in range(90,111))
            log.write_text(text+'[gfx-incident] capture host frame 100\n[gfx-incident] capture failed\n')
            result=stats.stats(log,90,110,0)
            self.assertIn('14 frames',result)
            self.assertIn('max 16.00 ms',result)

    def test_memory_is_aligned_to_frames_and_keeps_heaps_separate(self):
        text = '''[frame] 100 16 3 1 1 11
[mk8:vram] heap 0: 512 MB in 4 blocks, 300 MB used by 80 allocations; VMA estimate 512 of 4096 MB; vertex cache 12 MB; 50 live textures
[mk8:vram] heap 1: 64 MB in 1 blocks, 40 MB used by 4 allocations; VMA estimate 64 of 1024 MB; vertex cache 12 MB; 50 live textures
[mk8:vram] command lists: 40 active, 5 retired, clock 300
[frame] 200 17 4 0 1 12
[mk8:vram] heap 0: 768 MB in 6 blocks, 450 MB used by 90 allocations; VMA estimate 768 of 4096 MB; vertex cache 20 MB; 65 live textures
[mk8:vram] command lists: 42 active, 3 retired, clock 600
[frame] 300 16 3 0 1 12
[mk8:vram] heap 0: 768 MB in 6 blocks, 280 MB used by 70 allocations; VMA estimate 768 of 4096 MB; vertex cache 10 MB; 48 live textures
'''
        heaps, commands = stats.memory_samples(text)
        self.assertEqual([s['frame'] for s in heaps], [100, 100, 200, 300])
        self.assertEqual(heaps[1]['heap'], 1)
        self.assertEqual(heaps[-1]['allocated_mib'], 280)
        self.assertEqual(commands[-1]['active'], 42)
        lines = '\n'.join(stats.memory_stats(text, 100, 200))
        self.assertIn('heap 0', lines)
        self.assertIn('512 -> 768', lines)
        self.assertIn('300 -> 450', lines)
        self.assertIn('heap 1', lines)
        self.assertIn('40 -> 42', lines)
        self.assertNotIn('280', lines)

    def test_missing_or_unaligned_telemetry_is_not_reported_as_zero(self):
        self.assertEqual(stats.memory_samples('no telemetry'), ([], []))
        self.assertEqual(stats.memory_stats('[frame] 1 16 3 1 1 11', 0, 10), [])
        with self.assertRaisesRegex(ValueError, 'quadro'):
            stats.memory_samples('[mk8:vram] heap 0: 512 MB in 4 blocks, 300 MB used by 80 allocations; VMA estimate 512 of 4096 MB; vertex cache 12 MB; 50 live textures')
        with self.assertRaisesRegex(ValueError, 'VRAM'):
            stats.memory_samples('[frame] 1 16 3 1 1 11\n[mk8:vram] heap changed format')


module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FrameStatsTests(unittest.TestCase):
    def report(self, text, first=0, last=100):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'game.log'
            path.write_text(text, encoding='utf-8')
            return module.stats(path, first, last, 8)

    def test_negative_residual_does_not_hide_slow_frame(self):
        report = self.report('[frame] 1 10 1 2 3 4\n[frame] 2 100 80 30 1 -11\n')
        self.assertIn('2 frames', report)
        self.assertIn('mean 18.18 FPS', report)
        self.assertIn('max 100.00 ms', report)
        self.assertIn('>50 ms 1', report)

    def test_malformed_nonfinite_and_zero_totals_are_reported(self):
        report = self.report('[frame] 1 10 1 2 3 4\n[frame] 2 0 0 0 0 0\n'
                             '[frame] 3 1e999 0 0 0 0\n[frame] 4 10 0 broken\n')
        self.assertIn('1 frames', report)
        self.assertIn('rejected invalid rows in log: 3', report)

    def test_capture_exclusion_and_window_remain_effective(self):
        text = ''.join(f'[frame] {i} 10 1 2 3 4\n' for i in range(1, 21))
        report = self.report(text + 'saved frame-00010.png\n', 5, 15)
        self.assertIn('4 frames 5-15', report)


if __name__ == '__main__':
    unittest.main()
