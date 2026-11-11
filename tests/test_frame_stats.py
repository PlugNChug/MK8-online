"""Analisa contadores de memória com quadros conhecidos e logs autorais."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('frame_stats', ROOT/'scripts/frame-stats.py')
stats = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stats)


class MemoryStatsTests(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
