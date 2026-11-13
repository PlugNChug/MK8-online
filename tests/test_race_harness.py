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
    def test_saved_frame_generation_can_be_reproduced_without_changing_source(self):
        args = race.parse_args(['time-trial', '--frame-gen', 'saved', '--audio'])
        self.assertTrue(args.audio)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'user-no-mii/save').mkdir(parents=True)
            (root/'user-no-mii/save/data').write_bytes(b'original')
            ini = 'frame_gen = 120\nvolume = 0.35\nresolution = match\n'
            (root/'user-no-mii/mk8d.ini').write_text(ini)
            race.prepare_session(root, root/'saved', None, args.frame_gen)
            race.prepare_session(root, root/'control', None)
            self.assertEqual((root/'saved/data/mk8d.ini').read_text(), ini)
            self.assertIn('frame_gen = off', (root/'control/data/mk8d.ini').read_text())
            self.assertEqual((root/'user-no-mii/mk8d.ini').read_text(), ini)
            self.assertEqual((root/'user-no-mii/save/data').read_bytes(), b'original')

    def test_manual_handover_enables_real_input_without_host_pad_override(self):
        args = race.parse_args(['time-trial', '--cup', 'golden-dash', '--track', '3', '--manual'])
        self.assertTrue(args.manual)
        env = {'LARECOMP_INPUT': '10:A:6'}
        race.manual_input_environment(env)
        self.assertEqual(env, {'LARECOMP_INPUT': '10:A:6', 'LARECOMP_NO_USER_INPUT': '0'})
        for name in ('LARECOMP_HOST_INPUT', 'LARECOMP_BIND_TEST'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'controle físico'):
                race.manual_input_environment({name: ''})
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                race.parse_args(['time-trial', '--manual', '--env', name+'='])
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            race.parse_args(['grand-prix', '--assisted', '--manual'])

    def test_manual_dlc_launch_passes_controller_through_and_keeps_saves_isolated(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'user-no-mii/save').mkdir(parents=True)
            (root/'user-no-mii/save/original').write_bytes(b'untouched')
            original_ini = 'frame_gen = off\nvolume = 0.35\n'
            (root/'user-no-mii/mk8d.ini').write_text(original_ini)
            exe, dlc = root/'synthetic.exe', root/'dlc/installed.bin'
            exe.write_bytes(b'author-created test input; never executed')
            dlc.parent.mkdir()
            dlc.write_bytes(b'synthetic DLC receipt')
            def record(path):
                return {'path': str(path), 'sha256': race.hashlib.sha256(path.read_bytes()).hexdigest()}
            manifest = root/'candidate.json'
            manifest.write_text(json.dumps({'identity': 'synthetic', 'executable': record(exe),
                'files': {'gamelib': record(exe), 'dlc-manifest': record(dlc)}, 'data_root': str(root/'game')}))
            args = race.parse_args(['time-trial', '--cup', 'golden-dash', '--track', '3', '--manual',
                                    '--candidate', str(manifest), '--last-frame', '6500'])
            child = Mock(returncode=0)
            def completed(*_):
                session = Path(launch.call_args.kwargs['env']['LARECOMP_DATA_ROOT']).parent
                (session/'frame-06500.png').write_bytes(b'synthetic capture')
            for audible in (False, True):
                with self.subTest(audio=audible), patch.object(race, 'LOCAL', root), \
                        patch.object(race.subprocess, 'Popen', return_value=child) as launch, \
                        patch.object(race, 'wait_capture', side_effect=completed):
                    if audible:
                        args = race.parse_args(['time-trial', '--cup', 'golden-dash', '--track', '3', '--manual',
                                                '--audio', '--candidate', str(manifest), '--last-frame', '6500'])
                    session = race.run_once(args, root, None)
                env = launch.call_args.kwargs['env']
                self.assertEqual(env['LARECOMP_NO_USER_INPUT'], '0')
                self.assertNotIn('LARECOMP_HOST_INPUT', env)
                self.assertNotIn('LARECOMP_BIND_TEST', env)
                self.assertEqual(env['MK8_DLC_ROOT'], str(dlc.parent))
                self.assertEqual(env['LARECOMP_AUDIO_GAIN'], '1' if audible else '0')
                self.assertEqual((root/'user-no-mii/save/original').read_bytes(), b'untouched')
                self.assertNotEqual(env['LARECOMP_DATA_ROOT'], str(root/'user-no-mii'))
                self.assertEqual((root/'user-no-mii/mk8d.ini').read_text(), original_ini)
                self.assertIn('volume = 0.35', (session/'data/mk8d.ini').read_text())
                recorded = json.loads((session/'run.json').read_text())
                self.assertTrue(recorded['manual'])
                self.assertEqual(recorded['audio'], audible)

    def test_london_flyover_preserves_intro_and_selects_dlc_after_menu_opens(self):
        args = race.parse_args(['grand-prix', '--assisted', '--cup', 'rock', '--intro'])
        events = [part.split(':') for part in race.race_input(
            args.mode, args.last_frame, cup=race.CUPS.index(args.cup),
            assisted=True, intro=True).split(',')]
        self.assertEqual((args.last_frame, args.shots, args.shot_step), (5000, 13, 150))
        self.assertIn(['2200', 'R', '6'], events)
        self.assertEqual([b for f, b, _ in events if 2250 <= int(f) < 2500], ['RIGHT'] * 4)
        self.assertIn(['2500', 'A', '6'], events)
        self.assertIn(['2650', 'A', '6'], events)
        self.assertFalse(any(b == 'A' and int(f) > 2650 for f, b, _ in events))

    def test_moo_meadows_cup_is_selected_after_kart_parts(self):
        args = race.parse_args(['grand-prix', '--assisted', '--cup', 'shell'])
        events = [part.split(':') for part in race.race_input(
            args.mode, args.last_frame, cup=race.CUPS.index(args.cup), assisted=True).split(',')]
        moves = [(int(f), b) for f, b, _ in events if b in ('RIGHT', 'DOWN') and int(f) > 760]
        self.assertEqual(moves, [(2300, 'DOWN')])
        self.assertIn(['2500', 'A', '6'], events)

    def test_coconut_mall_selects_dlc_before_fourth_track(self):
        args = race.parse_args(['time-trial', '--cup', 'golden-dash', '--track', '3'])
        events = [part.split(':') for part in race.race_input(
            args.mode, 6500, cup=race.CUPS.index(args.cup), track=args.track).split(',')]
        self.assertIn(['1800', 'R', '6'], events)
        self.assertIn(['2000', 'A', '6'], events)
        self.assertEqual([int(f) for f, b, _ in events if b == 'RIGHT'], [2100, 2130, 2160])
        self.assertIn(['2300', 'A', '6'], events)
        self.assertEqual(events[-1], ['2900', 'A', '3610'])

    def test_intro_rejects_routes_that_would_skip_or_replace_it(self):
        for argv in (['time-trial', '--intro'], ['grand-prix', '--intro'],
                     ['grand-prix', '--assisted', '--intro', '--long-session'],
                     ['grand-prix', '--assisted', '--track', '1']):
            with self.subTest(argv=argv), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                race.parse_args(argv)

    def test_dlc_course_requires_candidate_manifest_and_uses_its_location(self):
        args = race.parse_args(['time-trial', '--cup', 'golden-dash', '--track', '3'])
        with self.assertRaisesRegex(ValueError, 'DLC'):
            race.session_dlc_root(args, {'files': {}})
        manifest = ROOT/'local/isolated-dlc/installed.bin'
        candidate = {'files': {'dlc-manifest': {'path': str(manifest)}}}
        self.assertEqual(race.session_dlc_root(args, candidate), str(manifest.parent))
        self.assertEqual(race.session_dlc_root(race.parse_args(['grand-prix', '--assisted']), candidate), '')
        self.assertIsNone(race.session_dlc_root(race.parse_args(['time-trial']), candidate))

    def test_missing_dlc_stops_before_session_creation_or_game_launch(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            exe = root/'synthetic.exe'
            exe.write_bytes(b'author-created test input; never executed')
            manifest = root/'candidate.json'
            manifest.write_text(json.dumps({'executable': {'path': str(exe),
                'sha256': race.hashlib.sha256(exe.read_bytes()).hexdigest()}, 'files': {}}))
            args = race.parse_args(['time-trial', '--cup', 'golden-dash', '--track', '3',
                                    '--candidate', str(manifest)])
            with patch.object(race.tempfile, 'mkdtemp') as create, patch.object(race.subprocess, 'Popen') as launch:
                with self.assertRaisesRegex(ValueError, 'DLC'):
                    race.run_once(args, root, None)
                create.assert_not_called()
                launch.assert_not_called()

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

    def test_fullscreen_policy_changes_only_the_copied_ini(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            source=root/'source'
            (source/'user-no-mii/save').mkdir(parents=True)
            (source/'user-no-mii/save/test.dat').write_bytes(b'original')
            ini='volume = 0.7\nframe_gen = 120\nfullscreen = 1\nunknown = keep\n'
            (source/'user-no-mii/mk8d.ini').write_text(ini)
            for policy,value in (('saved','1'),('on','1'),('off','0')):
                session=root/policy
                session.mkdir()
                race.prepare_session(source,session,None,'saved',policy)
                actual=(session/'data/mk8d.ini').read_text()
                self.assertIn('fullscreen = '+value,actual)
                self.assertIn('frame_gen = 120',actual)
                self.assertIn('unknown = keep',actual)
                self.assertEqual((source/'user-no-mii/mk8d.ini').read_text(),ini)
                self.assertEqual((source/'user-no-mii/save/test.dat').read_bytes(),b'original')
            args=race.parse_args(['time-trial','--fullscreen','off'])
            self.assertEqual(args.fullscreen,'off')
            self.assertEqual(race.parse_args(['time-trial']).fullscreen,'saved')

    def test_fullscreen_missing_setting_and_invalid_policy(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            source=root/'source'
            (source/'user-no-mii/save').mkdir(parents=True)
            (source/'user-no-mii/mk8d.ini').write_text('volume = 0.7\n')
            for policy,value in (('on','1'),('off','0')):
                session=root/policy
                session.mkdir()
                race.prepare_session(source,session,None,'saved',policy)
                self.assertIn('fullscreen = '+value,(session/'data/mk8d.ini').read_text())
            with self.assertRaisesRegex(ValueError,'tela cheia'):
                race.prepare_session(source,root/'invalid',None,'saved','invalid')
            self.assertFalse((root/'invalid').exists())

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
