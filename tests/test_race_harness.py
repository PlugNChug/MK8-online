"""Regressões do roteiro assistido; usa somente arquivos autorais."""
import importlib.util
from contextlib import redirect_stderr
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('race_harness', ROOT / 'scripts/test-race.py')
race = importlib.util.module_from_spec(spec)
spec.loader.exec_module(race)


class RaceHarnessTests(unittest.TestCase):
    def test_assisted_input_releases_accelerator_and_captures_assists(self):
        script = race.race_input('grand-prix', 17100, assisted=True,
                                 assist_toggle=['accelerate'])
        events = [part.split(':') for part in script.split(',')]
        self.assertIn(['1450', 'PLUS', '6'], events)
        self.assertIn(['1540', 'R', '6'], events)
        self.assertIn(['1660', 'PLUS', '6'], events)
        self.assertTrue(all(int(frame) < 2700 for frame, _, _ in events))
        self.assertNotIn(':L:', script)  # não inverter a direção já habilitada
        self.assertIn(':L:', race.race_input('grand-prix', 17100, assisted=True,
                                            assist_toggle=['steering']))

    def test_existing_acceleration_script_is_unchanged(self):
        self.assertTrue(race.race_input('grand-prix', 3400).endswith(',2700:A:710'))
        self.assertIn('1700:RIGHT:6', race.race_input('time-trial', 3400, cup=1))

    def test_long_session_keeps_one_process_and_confirms_subsequent_menus(self):
        args = race.parse_args(['grand-prix', '--assisted', '--long-session'])
        self.assertEqual((args.last_frame, args.timeout, args.shots, args.shot_step),
                         (131100, 3000, 33, 4000))
        self.assertEqual(args.repeat, 1)
        script = race.race_input('grand-prix', args.last_frame, assisted=True,
                                 long_session=True)
        events = [part.split(':') for part in script.split(',')]
        later = [(int(f), b, int(n)) for f, b, n in events if int(f) >= 16000]
        self.assertGreater(len(later), 400)
        self.assertTrue(all(b == 'A' and n == 6 for _, b, n in later))
        self.assertLess(later[-1][0], args.last_frame)
        self.assertLess(len(script.encode('ascii')), 16384)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            race.parse_args(['grand-prix', '--long-session'])
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            race.parse_args(['grand-prix', '--assisted', '--long-session',
                             '--last-frame', '2000000'])

    def test_invalid_combinations_fail_before_start(self):
        for argv in (['time-trial', '--assisted'],
                     ['grand-prix', '--assist-toggle', 'accelerate'],
                     ['grand-prix', '--assisted', '--input', '1:A:6'],
                     ['grand-prix', '--repeat', '0'],
                     ['grand-prix', '--last-frame', '0'],
                     ['grand-prix', '--timeout', '0']):
            with self.subTest(argv=argv), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                race.parse_args(argv)

    def test_assisted_defaults_allow_a_full_race_and_existing_defaults_survive(self):
        args = race.parse_args(['grand-prix', '--assisted', '--repeat', '2'])
        self.assertEqual((args.last_frame, args.timeout), (17100, 600))
        self.assertEqual((args.shots, args.shot_step), (8, 2000))
        old = race.parse_args(['grand-prix'])
        self.assertEqual((old.last_frame, old.timeout, old.shots, old.shot_step),
                         (3400, 180, 2, 400))

    def test_session_copies_save_and_cache_and_disables_frame_generation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, session, cache = root/'source', root/'session', root/'cache'
            (source/'user-no-mii/save').mkdir(parents=True)
            (source/'user-no-mii/save/test.dat').write_bytes(b'original')
            (source/'user-no-mii/mk8d.ini').write_text('volume = 0.7\nframe_gen = 120\n')
            (source/'mii-profile.json').write_text('{}')
            (source/'user-no-mii/fonts').mkdir()
            (source/'user-no-mii/fonts/authored-font').write_bytes(b'fixture')
            cache.mkdir()
            (cache/'shader').write_bytes(b'cache')
            session.mkdir()
            before = race.save_hashes(source/'user-no-mii/save')
            race.prepare_session(source, session, cache)
            (session/'data/save/test.dat').write_bytes(b'changed by game')
            self.assertEqual(race.save_hashes(source/'user-no-mii/save'), before)
            self.assertIn('frame_gen = off', (session/'data/mk8d.ini').read_text())
            self.assertEqual((session/'data/cache/shader').read_bytes(), b'cache')
            self.assertEqual((cache/'shader').read_bytes(), b'cache')
            self.assertTrue((session/'data/build').is_dir())
            self.assertEqual((session/'data/fonts/authored-font').read_bytes(), b'fixture')
            (source/'mii-profile.json').unlink()
            (source/'user-no-mii/mk8d.ini').write_text('volume = 0.7\n')
            second = root/'without-profile'
            second.mkdir()
            race.prepare_session(source, second, None)
            self.assertIn('frame_gen = off', (second/'data/mk8d.ini').read_text())
            self.assertFalse((second/'mii-profile.json').exists())

    def test_repeat_forwards_only_cache_and_stops_on_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'user-no-mii/save').mkdir(parents=True)
            (root/'user-no-mii/save/data').write_bytes(b'original')
            args = race.parse_args(['grand-prix', '--repeat', '2',
                                    '--candidate', str(root/'candidate.json')])
            seen = []

            def run_once(args, source, cache):
                self.assertEqual(source, root)
                self.assertEqual((source/'user-no-mii/save/data').read_bytes(), b'original')
                seen.append(cache)
                session = root / f'run-{len(seen)}'
                (session/'data/cache').mkdir(parents=True)
                (session/'data/cache/shader').write_bytes(b'compiled')
                return session

            with patch.object(race, 'run_once', side_effect=run_once):
                race.run(args)
            self.assertEqual(seen, [None, root/'run-1/data/cache'])
            with patch.object(race, 'run_once', side_effect=RuntimeError('crash')) as run:
                with self.assertRaisesRegex(RuntimeError, 'crash'):
                    race.run(args)
                self.assertEqual(run.call_count, 1)

    def test_source_save_change_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'user-no-mii/save').mkdir(parents=True)
            save = root/'user-no-mii/save/data'
            save.write_bytes(b'original')
            args = race.parse_args(['grand-prix', '--candidate', str(root/'candidate.json')])

            def unexpected_write(*_):
                save.write_bytes(b'changed')
                raise RuntimeError('test stopped')

            with patch.object(race, 'run_once', side_effect=unexpected_write):
                with self.assertRaisesRegex(RuntimeError, 'save de origem mudou'):
                    race.run(args)

    def test_fresh_runtime_log_is_copied_but_stale_log_is_not(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            captured, runtime = root/'game.log', root/'mk8drecomp.log'
            captured.write_text('pipe')
            runtime.write_text('runtime frames')
            race.collect_log(captured, runtime, runtime.stat().st_mtime_ns + 1)
            self.assertEqual(captured.read_text(), 'pipe')
            race.collect_log(captured, runtime, 0)
            self.assertEqual(captured.read_text(), 'runtime frames')

    def test_power_changes_are_timestamped_without_discarding_measurements(self):
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder)/'game.log'
            child = Mock()
            child.poll.side_effect = [None, None, None, None, 0, 0]
            connected = {'ac_line_status': 1, 'battery_percent': 80}
            battery = {'ac_line_status': 0, 'battery_percent': 80}
            with patch.object(race, 'read_power_status', side_effect=[connected, connected, battery, connected]), \
                    patch.object(race, 'sleep'), patch.object(race, 'monotonic', return_value=0), \
                    patch.object(race.shutil, 'disk_usage', return_value=Mock(free=5*1024**3)):
                race.wait_capture(child, 10, log)
            rows = [json.loads(line) for line in (Path(folder)/'power-events.jsonl').read_text().splitlines()]
            self.assertEqual([r['ac_line_status'] for r in rows], [1, 0, 1])
            self.assertTrue(all(r['unix_ns'] > 0 and r['elapsed_seconds'] == 0 for r in rows))
            child.kill.assert_not_called()

    def test_missing_power_telemetry_does_not_prevent_capture(self):
        with tempfile.TemporaryDirectory() as folder:
            child = Mock()
            child.poll.side_effect = [None, 0, 0]
            with patch.object(race, 'read_power_status', return_value=None), \
                    patch.object(race, 'sleep'), patch.object(race, 'monotonic', return_value=0), \
                    patch.object(race.shutil, 'disk_usage', return_value=Mock(free=5*1024**3)):
                race.wait_capture(child, 10, Path(folder)/'game.log')
            self.assertFalse((Path(folder)/'power-events.jsonl').exists())


if __name__ == '__main__':
    unittest.main()
