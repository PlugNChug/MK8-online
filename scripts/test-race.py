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


def race_input(mode, last_frame, course='stadium'):
    start = '400:A:6,520:DOWN:6,580:A:6,760:A:6'
    if mode == 'time-trial':
        start += ',880:DOWN:6'
        for frame in range(940, 2401, 160):
            if frame == 1900:
                for step in range(('stadium', 'water-park', 'sweet-sweet-canyon').index(course)):
                    start += f',{1820 + step * 25}:RIGHT:6'
            start += f',{frame}:A:6'
    else:
        start += ''.join(f',{frame}:A:6' for frame in range(920, 2401, 160))
    return start + f',2700:A:{max(last_frame - 2690, 10)}'


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
    candidate = json.loads((LOCAL / 'candidate.json').read_text(encoding='utf-8'))
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
    shutil.copyfile(LOCAL / 'mii-profile.json', profile)

    script = race_input(args.mode, args.last_frame, args.course)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('MK8_', 'LARECOMP_', 'RECOMP_', 'NX_LAN_'))}
    env.update(dict(args.env))
    # The runtime writes shader sources and debug-frame pass images under build/ relative to its data root.
    if any(key in env for key in ('LARECOMP_SAVE_MSL', 'LARECOMP_DEBUG_FRAME')):
        (data / 'build').mkdir()
    env.update(
        MK8_EXPERIMENTAL_NO_MII='1', MK8_MII_PROFILE_PATH=str(profile),
        LARECOMP_DATA_ROOT=str(data), LARECOMP_GAME_LIB=candidate['files']['gamelib']['path'],
        LARECOMP_INPUT=args.input or script,
        LARECOMP_WINDOW_SHOT=f'{args.last_frame - args.shot_step * (args.shots - 1)}:{args.last_frame}:{args.shot_step}',
        LARECOMP_SHOT_PATH=(session / 'frame-%05d.png').as_posix(),
        LARECOMP_EXIT_AFTER_SHOT='1',
    )
    (session / 'run.json').write_text(json.dumps({
        'candidate': candidate['identity'], 'mode': args.mode, 'options': args.env,
        'course': args.course,
        'input': env['LARECOMP_INPUT'], 'last_frame': args.last_frame,
        'cache_from': str(args.cache_from.resolve()) if args.cache_from else None,
    }, indent=2), encoding='utf-8')
    started = monotonic()
    log = session / 'game.log'
    with log.open('wb') as stream:
        child = subprocess.Popen(
            [candidate['executable']['path'], str(Path(candidate['data_root']) / 'extracted')],
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
    parser.add_argument('--cache-from', type=Path, help='Copy a previous cache directory; saves remain isolated')
    parser.add_argument('--course', choices=['stadium', 'water-park', 'sweet-sweet-canyon'], default='stadium',
                        help='Time Trial course in Mushroom Cup')
    parser.add_argument('--env', action='append', default=[], type=lambda x: x.split('=', 1))
    args = parser.parse_args()
    if args.mode != 'time-trial' and args.course != 'stadium':
        parser.error('--course requires time-trial')
    if (args.shots < 1 or args.shot_step < 1 or args.last_frame <= args.shot_step * (args.shots - 1)
            or any(len(pair) != 2 for pair in args.env)):
        parser.error('Require positive capture frames, capture count and step, and --env NAME=VALUE')
    run(args)
