"""Course selection precedes confirmation; acceleration lasts through the capture."""
import runpy
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

script = runpy.run_path(str(Path(__file__).resolve().parents[1]/'scripts/test-race.py'))
race_input = script['race_input']
wait_capture = script['wait_capture']


class RaceScriptTests(unittest.TestCase):
    def test_course_selection_and_acceleration(self):
        for course, turns in [('stadium', 0), ('water-park', 1), ('sweet-sweet-canyon', 2)]:
            steps = [entry.split(':') for entry in race_input('time-trial', 6500, course).split(',')]
            self.assertEqual([int(s[0]) for s in steps], sorted(int(s[0]) for s in steps))
            rights = [s for s in steps if s[1] == 'RIGHT']
            self.assertEqual(len(rights), turns)
            self.assertTrue(all(1740 < int(s[0]) < 1900 for s in rights))
            self.assertEqual(steps[-1], ['2700', 'A', '3810'])

    def test_grand_prix_keeps_existing_navigation(self):
        script = race_input('grand-prix', 5200)
        self.assertNotIn('RIGHT', script)
        self.assertNotIn('880:DOWN', script)
        self.assertTrue(script.endswith('2700:A:2510'))

    def test_capture_monitor_preserves_exit_status(self):
        with subprocess.Popen([sys.executable, '-c', 'raise SystemExit(7)']) as child:
            with patch('shutil.disk_usage', return_value=SimpleNamespace(free=8 * 1024**3)):
                wait_capture(child, 10, 'test.log')
            self.assertEqual(child.returncode, 7)

    def test_capture_monitor_stops_owned_process_on_low_disk_or_timeout(self):
        for free, timeout, reason in [(3 * 1024**3, 10, '4 GiB'), (8 * 1024**3, -1, 'timed out')]:
            with self.subTest(reason=reason):
                with subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']) as child:
                    with patch('shutil.disk_usage', return_value=SimpleNamespace(free=free)):
                        with self.assertRaisesRegex(SystemExit, reason):
                            wait_capture(child, timeout, 'test.log')
                    self.assertIsNotNone(child.poll())


if __name__ == '__main__':
    unittest.main()
