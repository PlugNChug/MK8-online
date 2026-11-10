"""Drive an experimental candidate with scripted input and isolated saves."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from time import monotonic, sleep


ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / 'local'
PERF = re.compile(r'\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms')


# The 4.0.1 cup screen in Time Trial: two rows of six cups, then the cup's four tracks in a row.
CUPS = ('mushroom', 'flower', 'star', 'special', 'egg', 'crossing',
        'shell', 'banana', 'leaf', 'lightning', 'triforce', 'bell')
COURSES = ('stadium', 'water-park', 'sweet-sweet-canyon')


def race_input(mode, last_frame, course='stadium', cup=0, track=None, character='mario'):
    if character == 'mii':
        if mode != 'time-trial' or cup or track not in (None, 0) or course != 'stadium':
            raise ValueError('Mii diagnostic currently supports Time Trial / Mushroom / Stadium only')
        # MK8 4.0.1: wrap from Mario to the final row/column, select the first Mii,
        # then Normal Suit. The extra two dialogs shift the kart/course inputs.
        start = ('400:A:6,520:DOWN:6,580:A:6,760:A:6,880:DOWN:6,940:A:6,'
                 '1100:UP:6,1160:LEFT:6,1240:A:6,1500:A:6,1760:A:6,'
                 '2020:A:6,2200:A:6,2380:A:6,2560:A:6,2740:A:6,2920:A:6')
        return start + f',3100:A:{max(last_frame - 3090, 10)}'
    start = '400:A:6,520:DOWN:6,580:A:6,760:A:6'
    if mode == 'time-trial':
        track = COURSES.index(course) if track is None else track
        start += ',880:DOWN:6'
        for frame in range(940, 2401, 160):
            if frame == 1740:  # the cup screen: column, then row (the cursor starts on Mushroom Cup)
                # (the screen takes input from ~1680 on, a few frames later in some runs: a press there was lost and the
                # Egg Cup run raced Cloudtop Cruise; the moves start later and the confirm waits for them)
                moves = ['RIGHT'] * (cup % 6) + ['DOWN'] * (cup // 6)
                for step, button in enumerate(moves):
                    start += f',{1700 + step * 16}:{button}:6'
                frame = 1820
            elif frame == 1900:
                for step in range(track):
                    start += f',{1890 + step * 28}:RIGHT:6'
                frame = 2000
            start += f',{frame}:A:6'
    else:
        start += ''.join(f',{frame}:A:6' for frame in range(920, 2401, 160))
    return start + f',2700:A:{max(last_frame - 2690, 10)}'


def profile_environment(profile):
    """Match launcher profile overlays; the native service still validates CharInfo."""
    data = json.loads(profile.read_text(encoding='utf-8'))
    if data['schema'] != 1:
        raise ValueError('Unsupported Mii profile schema')
    identity = bytes(data['id'])
    if len(identity) != 16 or identity[8] & 0xc0 != 0x80:
        raise ValueError('Invalid Mii profile identity')
    appearance = data['appearance']
    name, color, gender = appearance['name'], appearance['color'], appearance['gender']
    if (not name.strip() or len(name) > 10 or any(ord(c) < 32 or 127 <= ord(c) <= 159
            or 0xd800 <= ord(c) <= 0xdfff or ord(c) > 0xffff for c in name)
            or type(color) is not int or not 0 <= color <= 11
            or type(gender) is not int or gender not in (0, 1)):
        raise ValueError('Invalid Mii profile appearance')
    env = dict(MK8_MII_ID=identity.hex(), MK8_MII_NAME=name,
               MK8_MII_COLOR=str(color), MK8_MII_GENDER=str(gender))
    raw = data.get('charinfo')
    if raw is not None:
        raw = bytearray(raw)
        if len(raw) != 88:
            raise ValueError('Mii CharInfo must contain 88 bytes')
        raw[:16] = identity
        raw[16:38] = name.encode('utf-16le').ljust(22, b'\0')
        raw[0x27], raw[0x28] = color, gender
        env['MK8_MII_CHARINFO'] = raw.hex()
    else:
        face = appearance.get('face', {})
        fields = [('skin', 0, 5), ('shape', 0, 2), ('hair', 1, 3), ('hair_color', 1, 7),
                  ('eyes', 0, 2), ('eye_color', 0, 5), ('glasses', False, 1),
                  ('height', 64, 127), ('build', 64, 127)]
        values = [face.get(key, default) for key, default, _ in fields]
        if any(not isinstance(value, int) or not 0 <= value <= limit
               for value, (_, _, limit) in zip(values, fields)):
            raise ValueError('Invalid Mii face parameters')
        env['MK8_MII_FACE'] = bytes(values).hex()
    return env


def wait_capture(child, timeout, log):
    """Monitor only this test's process; the runtime parent waits for its ASLR child."""
    started = monotonic()
    try:
        while child.poll() is None:
            if shutil.disk_usage(LOCAL).free < 4 * 1024**3:
                raise SystemExit(f'Capture stopped to preserve the 4 GiB disk reserve. See {log}')
            if monotonic() - started > timeout:
                raise SystemExit(f'Capture timed out after {timeout}s. See {log}')
            sleep(.5)
    finally:
        if child.poll() is None:
            if os.name == 'nt':
                subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            if child.poll() is None:
                child.kill()
            child.wait(timeout=15)


def run(args):
    if shutil.disk_usage(LOCAL).free < 4 * 1024**3:
        raise SystemExit('Race capture requires at least 4 GiB free on the local drive')
    candidate = json.loads((args.candidate or LOCAL / 'candidate.json').read_text(encoding='utf-8'))
    for entry in [candidate['executable'], *candidate['files'].values()]:
        path = Path(entry['path'])
        with path.open('rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        if actual != entry['sha256']:
            raise ValueError(f'Candidate changed on disk: {path}')

    session = Path(tempfile.mkdtemp(prefix=f'race-{args.mode}-', dir=LOCAL))
    print(f'Test output: {session}', flush=True)
    data = session / 'data'
    source = LOCAL / 'user-no-mii'
    shutil.copytree(source / 'save', data / 'save')
    ini = (source / 'mk8d.ini').read_text(encoding='utf-8')
    ini = re.sub(r'^frame_gen\s*=.*$', 'frame_gen = off', ini, flags=re.M)
    (data / 'mk8d.ini').write_text(ini, encoding='utf-8')
    if args.cache_from:
        shutil.copytree(args.cache_from, data / 'cache')
    profile = session / 'mii-profile.json'
    shutil.copyfile(args.mii_profile or LOCAL / 'mii-profile.json', profile)

    script = race_input(args.mode, args.last_frame, args.course, CUPS.index(args.cup), args.track,
                        args.character)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('MK8_', 'LARECOMP_', 'RECOMP_', 'NX_LAN_'))}
    env.update(profile_environment(profile))
    env.update(dict(args.env))
    # The runtime writes shader sources and debug-frame pass images under build/ relative to its data root.
    if any(key in env for key in ('LARECOMP_SAVE_MSL', 'LARECOMP_DEBUG_FRAME')):
        (data / 'build').mkdir()
    env.update(
        MK8_EXPERIMENTAL_NO_MII='1', MK8_MII_PROFILE_PATH=str(profile),
        LARECOMP_DATA_ROOT=str(data), LARECOMP_GAME_LIB=candidate['files']['gamelib']['path'],
        LARECOMP_SAVE_DIR=str(data / 'save'),
        LARECOMP_INPUT=args.input or script,
        LARECOMP_WINDOW_SHOT=f'{args.last_frame - args.shot_step * (args.shots - 1)}:{args.last_frame}:{args.shot_step}',
        LARECOMP_SHOT_PATH=(session / 'frame-%05d.png').as_posix(),
        LARECOMP_EXIT_AFTER_SHOT='1',
    )
    (session / 'run.json').write_text(json.dumps({
        'candidate': candidate['identity'], 'mode': args.mode, 'options': args.env,
        'course': args.course, 'cup': args.cup, 'track': args.track,
        'input': env['LARECOMP_INPUT'], 'last_frame': args.last_frame,
        'character': args.character, 'mii_profile_sha256': hashlib.sha256(profile.read_bytes()).hexdigest(),
        'model_rendering_verified': False,
        'cache_from': str(args.cache_from.resolve()) if args.cache_from else None,
    }, indent=2), encoding='utf-8')
    started = monotonic()
    log = session / 'game.log'
    with log.open('wb') as stream:
        child = subprocess.Popen(
            [candidate['executable']['path'], *(['--nextendo-login-play'] if args.nextendo else []),
             str(Path(candidate['data_root']) / 'extracted')],
            cwd=data, env=env, stdin=subprocess.DEVNULL, stdout=stream,
            stderr=subprocess.STDOUT,
        )
        wait_capture(child, args.timeout, log)
    rows = PERF.findall(log.read_text(encoding='utf-8', errors='replace'))
    print(f'Exit {child.returncode}, {monotonic() - started:.1f}s', flush=True)
    for elapsed, frame, fps, peak in rows[-8:]:
        print(f'{elapsed}s | frame {frame} | {fps} FPS | max {peak} ms')
    if child.returncode or not (session / f'frame-{args.last_frame:05d}.png').is_file():
        raise SystemExit('Candidate failed to reach the final capture; inspect game.log')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['grand-prix', 'time-trial'])
    parser.add_argument('--last-frame', type=int, default=3400)
    parser.add_argument('--shots', type=int, default=2)
    parser.add_argument('--shot-step', type=int, default=400)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--input', help='Override frame:button:duration input sequence')
    parser.add_argument('--character', choices=['mario', 'mii'], default='mario',
                        help='Mii selects the first profile and Normal Suit; does not enable model rendering')
    parser.add_argument('--cache-from', type=Path, help='Copy a previous cache directory; saves remain isolated')
    parser.add_argument('--mii-profile', type=Path, help='Copy this profile into the isolated test; never modify it')
    parser.add_argument('--course', choices=['stadium', 'water-park', 'sweet-sweet-canyon'], default='stadium',
                        help='Time Trial course in Mushroom Cup')
    parser.add_argument('--cup', choices=CUPS, default='mushroom', help='Time Trial cup (with --track)')
    parser.add_argument('--track', type=int, choices=range(4), help='Track 0-3 of --cup (overrides --course)')
    parser.add_argument('--env', action='append', default=[], type=lambda x: x.split('=', 1))
    parser.add_argument('--candidate', type=Path, help='candidate.json of another data folder (e.g. a fresh build)')
    parser.add_argument('--nextendo', action='store_true', help='Authenticate in this process before boot; requires MK8_NEXTENDO_TEST=1')
    args = parser.parse_args()
    if args.mode != 'time-trial' and (args.course != 'stadium' or args.cup != 'mushroom' or args.track is not None):
        parser.error('--course, --cup and --track require time-trial')
    if (args.shots < 1 or args.shot_step < 1 or args.last_frame <= args.shot_step * (args.shots - 1)
            or any(len(pair) != 2 for pair in args.env)):
        parser.error('Require positive capture frames, capture count and step, and --env NAME=VALUE')
    run(args)
