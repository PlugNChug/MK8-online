"""Keep slow frames with overlapping async timings in the performance report."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('frame_stats', ROOT/'scripts/frame-stats.py')
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
