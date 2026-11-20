"""Course selection precedes confirmation; acceleration lasts through the capture."""
import runpy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

script = runpy.run_path(str(Path(__file__).resolve().parents[1]/'scripts/test-race.py'))
race_input = script['race_input']
wait_capture = script['wait_capture']
profile_environment = script['profile_environment']


class RaceScriptTests(unittest.TestCase):
    def test_final_capture_identifies_the_loaded_track(self):
        tracks = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'scripts/test-tracks.py'))
        captures = tracks['capture_courses'](
            '[fs] read Course/Gu_FirstCircuit/course_model.szs\n'
            '[vk] saved D:\\test\\frame-03400.png (1920x1080)\n'
            '[fs] read Course/Du_Animal_Winter/course_model.szs\n'
            '[vk] saved /test/frame-04400.png (1920x1080)')
        self.assertTrue(tracks['matches_course']('mushroom', 0, captures['frame-03400.png']))
        self.assertTrue(tracks['matches_course']('crossing', 3, captures['frame-04400.png']))
        self.assertFalse(tracks['matches_course']('mushroom', 0, captures['frame-04400.png']))
        self.assertFalse(tracks['matches_course']('egg', 0, None))

    def test_interactive_session_uses_live_input_and_preserves_source_save(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'user-no-mii'
            (source / 'save').mkdir(parents=True)
            (source / 'save/test.dat').write_text('original')
            (source / 'mk8d.ini').write_text('frame_gen = on\n')
            (root / 'mii-profile.json').write_text(json.dumps(dict(
                schema=1, id=[0]*8 + [128] + [0]*7,
                appearance=dict(name='Test', color=0, gender=0))))
            # A real child process exercises environment, normal exit and isolated writes.
            (root / 'extracted').write_text(
                'import os,json,pathlib\n'
                'pathlib.Path("observed.json").write_text(json.dumps(dict(os.environ)))\n'
                'pathlib.Path(os.environ["LARECOMP_SAVE_DIR"],"test.dat").write_text("changed")\n')
            exe = Path(sys.executable)
            candidate = dict(identity='fixture', executable=dict(path=str(exe),
                sha256=hashlib.sha256(exe.read_bytes()).hexdigest()),
                files=dict(gamelib=dict(path=str(root / 'extracted'),
                    sha256=hashlib.sha256((root / 'extracted').read_bytes()).hexdigest())),
                data_root=str(root))
            (root / 'candidate.json').write_text(json.dumps(candidate))
            args = SimpleNamespace(candidate=None, mode='time-trial', cache_from=None,
                mii_profile=None, mii_resources=None, course='stadium', cup='mushroom',
                track=None, character='mario', last_frame=3400, shots=2, shot_step=400,
                env=[], input=None, interactive=True, nextendo=False, timeout=15)
            with patch.dict(script['run'].__globals__, LOCAL=root):
                script['run'](args)
            session, = root.glob('race-time-trial-*')
            env = json.loads((session / 'data/observed.json').read_text())
            for key in ('LARECOMP_INPUT', 'LARECOMP_EXIT_AFTER_SHOT', 'LARECOMP_WINDOW_SHOT'):
                self.assertNotIn(key, env)
            self.assertTrue(env['PATH'].startswith(str(root)))
            self.assertEqual((source / 'save/test.dat').read_text(), 'original')
            self.assertEqual((session / 'data/save/test.dat').read_text(), 'changed')

    def test_mii_navigation_selects_profile_and_suit_before_kart(self):
        steps = [s.split(':') for s in race_input('time-trial', 4200, character='mii').split(',')]
        self.assertEqual([int(s[0]) for s in steps], sorted(int(s[0]) for s in steps))
        self.assertEqual([s[1] for s in steps if 1000 < int(s[0]) < 1240], ['UP', 'LEFT'])
        self.assertIn(['1500', 'A', '6'], steps)
        self.assertIn(['1760', 'A', '6'], steps)
        self.assertEqual(steps[-1], ['3100', 'A', '1110'])
        with self.assertRaises(ValueError):
            race_input('grand-prix', 4200, character='mii')

    def test_profile_is_passed_without_changing_source_and_preserves_imported_fields(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'profile.json'
            data = dict(schema=1, id=[0]*8 + [128] + [0]*7,
                        appearance=dict(name='Teste', color=8, gender=1))
            path.write_text(json.dumps(data), encoding='utf-8')
            original = path.read_bytes()
            env = profile_environment(path)
            self.assertEqual(env['MK8_MII_FACE'], '000001010000004040')
            self.assertEqual(env['MK8_MII_NAME'], 'Teste')
            self.assertEqual(path.read_bytes(), original)
            raw = bytearray(88)
            raw[0x31] = 100
            data['charinfo'] = list(raw)
            path.write_text(json.dumps(data), encoding='utf-8')
            original = path.read_bytes()
            exported = bytes.fromhex(profile_environment(path)['MK8_MII_CHARINFO'])
            self.assertEqual(len(exported), 88)
            self.assertEqual(exported[:16], bytes(data['id']))
            self.assertEqual(exported[16:38].decode('utf-16le').rstrip('\0'), 'Teste')
            self.assertEqual(exported[0x27:0x29], bytes([8, 1]))
            self.assertEqual(exported[0x31], 100)
            self.assertEqual(path.read_bytes(), original)
            data['charinfo'] = [0]*140
            path.write_text(json.dumps(data), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, '88 bytes'):
                profile_environment(path)

    def test_course_selection_and_acceleration(self):
        for course, turns in [('stadium', 0), ('water-park', 1), ('sweet-sweet-canyon', 2)]:
            steps = [entry.split(':') for entry in race_input('time-trial', 6500, course).split(',')]
            self.assertEqual([int(s[0]) for s in steps], sorted(int(s[0]) for s in steps))
            rights = [s for s in steps if s[1] == 'RIGHT']
            self.assertEqual(len(rights), turns)
            # (between the cup's confirm at 1820 and the track's at 2000)
            self.assertTrue(all(1820 < int(s[0]) < 2000 for s in rights))
            self.assertIn(['1820', 'A', '6'], steps)
            self.assertIn(['2000', 'A', '6'], steps)
            self.assertEqual(steps[-1], ['2700', 'A', '3810'])

    def test_cup_moves_come_before_the_cup_is_confirmed(self):
        # Bell Cup: five columns right and one row down, then the fourth track.
        steps = [entry.split(':') for entry in race_input('time-trial', 4400, cup=11, track=3).split(',')]
        self.assertEqual([int(s[0]) for s in steps], sorted(int(s[0]) for s in steps))
        cup = [s[1] for s in steps if 1600 < int(s[0]) < 1820 and s[1] != 'A']
        self.assertEqual(cup, ['RIGHT'] * 5 + ['DOWN'])
        track = [s[1] for s in steps if 1820 < int(s[0]) < 2000]
        self.assertEqual(track, ['RIGHT'] * 3)

    def test_grand_prix_keeps_existing_navigation(self):
        script = race_input('grand-prix', 5200)
        self.assertNotIn('RIGHT', script)
        self.assertNotIn('880:DOWN', script)
        steps = [entry.split(':') for entry in script.split(',')]
        held = [(int(frame), int(length)) for frame, button, length in steps if int(frame) >= 2700]
        self.assertTrue(all(button == 'A' for frame, button, _ in steps if int(frame) >= 2700))
        self.assertEqual(held[0], (2700, 119))
        # one released frame between presses, so the standings advance; the last press reaches the capture
        self.assertTrue(all(b[0] == a[0] + 120 and a[1] == 119 for a, b in zip(held, held[1:])))
        self.assertGreaterEqual(held[-1][0] + held[-1][1], 5200)

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
