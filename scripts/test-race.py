"""Captura corridas com saves isolados; --assisted usa os auxílios do jogo (Grand Prix)."""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from time import monotonic, sleep, time_ns


ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / 'local'
sys.path.insert(0, str(ROOT / 'toolchain'))
from build_lock import exclusive_build

PERF = re.compile(r'\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms')


# MK8 4.0.1: duas páginas de duas filas/seis copas; no Contra o Relógio, quatro pistas por copa.
CUPS = ('mushroom', 'flower', 'star', 'special', 'egg', 'crossing',
        'shell', 'banana', 'leaf', 'lightning', 'triforce', 'bell',
        'golden-dash', 'lucky-cat', 'turnip', 'propeller', 'rock', 'moon',
        'fruit', 'boomerang', 'feather', 'cherry', 'acorn', 'spiny')
COURSES = ('stadium', 'water-park', 'sweet-sweet-canyon')


def race_input(mode, last_frame, course='stadium', cup=0, track=None,
               assisted=False, assist_toggle=(), long_session=False, intro=False):
    start = '400:A:6,520:DOWN:6,580:A:6,760:A:6'
    if assisted:
        # A tela de estatísticas expõe L (direção) e R (aceleração); são toggles persistidos.
        start += ',920:A:6,1080:A:6,1240:A:6,1450:PLUS:6'
        if 'steering' in assist_toggle:
            start += ',1500:L:6'
        if 'accelerate' in assist_toggle:
            start += ',1540:R:6'
        start += ',1660:PLUS:6,1750:A:6,1900:A:6,2050:A:6'
        if cup or intro:
            # A troca de página antes de 2120 pode ser ignorada durante a animação do kart.
            if cup >= 12:
                start += ',2200:R:6'
            index = cup % 12
            moves = ['RIGHT'] * (index % 6) + ['DOWN'] * (index // 6)
            start += ''.join(f',{2300 + step * 20}:{button}:6' for step, button in enumerate(moves))
            start += ',2500:A:6,2650:A:6'
            if not intro:
                start += ',2810:A:6'
        else:
            start += ',2210:A:6,2370:A:6,2530:A:6'
        if long_session:
            # Confirmar resultados/próxima corrida sem reiniciar o processo nem manter A pressionado.
            start += ''.join(f',{frame}:A:6' for frame in range(16000, last_frame, 240))
        return start
    if mode == 'time-trial':
        track = COURSES.index(course) if track is None else track
        start += ',880:DOWN:6'
        if cup >= 12:
            start += ''.join(f',{frame}:A:6' for frame in range(940, 1581, 160))
            start += ',1800:R:6'
            index = cup % 12
            moves = ['RIGHT'] * (index % 6) + ['DOWN'] * (index // 6)
            start += ''.join(f',{1900 + step * 20}:{button}:6' for step, button in enumerate(moves))
            confirm = 2100 if moves else 2000
            start += f',{confirm}:A:6'
            start += ''.join(f',{confirm + 100 + step * 30}:RIGHT:6' for step in range(track))
            start += ''.join(f',{confirm + offset}:A:6' for offset in (300, 460, 620))
            return start + f',2900:A:{max(last_frame - 2890, 10)}'
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


def read_power_status():
    if os.name != 'nt':
        return None
    class SystemPowerStatus(ctypes.Structure):
        _fields_ = [('ac', ctypes.c_ubyte), ('flags', ctypes.c_ubyte),
                    ('percent', ctypes.c_ubyte), ('saver', ctypes.c_ubyte),
                    ('life', ctypes.c_uint32), ('full_life', ctypes.c_uint32)]
    status = SystemPowerStatus()
    if not ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(status)):
        return {'available': False}
    return {'ac_line_status': status.ac, 'battery_flags': status.flags,
            'battery_percent': status.percent, 'battery_saver': status.saver}


def wait_capture(child, timeout, log):
    """Monitor only this test's process; the runtime parent waits for its ASLR child."""
    log = Path(log)
    started = monotonic()
    previous_power = None
    try:
        while child.poll() is None:
            power = read_power_status()
            if power is not None and power != previous_power:
                with log.with_name('power-events.jsonl').open('a', encoding='utf-8') as stream:
                    stream.write(json.dumps({'elapsed_seconds': round(monotonic()-started, 3),
                                             'unix_ns': time_ns(), **power}) + '\n')
                previous_power = power
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


def save_hashes(folder):
    return {path.relative_to(folder).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(folder.rglob('*')) if path.is_file()}


def prepare_session(source, session, cache, frame_gen='off', fullscreen='saved'):
    if frame_gen not in ('off', 'saved'):
        raise ValueError('Política de frame generation inválida')
    if fullscreen not in ('saved', 'on', 'off'):
        raise ValueError('Política de tela cheia inválida')
    data = session / 'data'
    shutil.copytree(source / 'user-no-mii/save', data / 'save')
    ini = (source / 'user-no-mii/mk8d.ini').read_text(encoding='utf-8')
    if frame_gen == 'off':
        ini = re.sub(r'^frame_gen\s*=.*$', 'frame_gen = off', ini, flags=re.M)
        if not re.search(r'^frame_gen\s*=', ini, flags=re.M):
            ini += '\nframe_gen = off\n'
    if fullscreen != 'saved':
        setting = 'fullscreen = ' + ('1' if fullscreen == 'on' else '0')
        ini = re.sub(r'^fullscreen\s*=.*$', setting, ini, flags=re.M)
        if not re.search(r'^fullscreen\s*=', ini, flags=re.M):
            ini += '\n' + setting + '\n'
    (data / 'mk8d.ini').write_text(ini, encoding='utf-8')
    (data / 'build').mkdir()
    if cache:
        shutil.copytree(cache, data / 'cache')
    if (source / 'user-no-mii/fonts').is_dir():
        shutil.copytree(source / 'user-no-mii/fonts', data / 'fonts')
    if (source / 'mii-profile.json').is_file():
        shutil.copyfile(source / 'mii-profile.json', session / 'mii-profile.json')


def collect_log(log, runtime_log, started_ns):
    # Windows pode redirecionar stderr para junto do EXE; nunca usar um log de outra execução.
    if runtime_log.is_file() and runtime_log.stat().st_mtime_ns >= started_ns:
        shutil.copyfile(runtime_log, log)


def session_dlc_root(args, candidate):
    if CUPS.index(args.cup) >= 12:
        record = candidate['files'].get('dlc-manifest')
        if not record:
            raise ValueError('A copa DLC exige um candidato com DLC instalado e verificado.')
        return str(Path(record['path']).parent)
    return '' if args.assisted else None


def manual_input_environment(env):
    # Mesmo vazio, HOST_INPUT ativa host_test e substitui os estados do controle real.
    if any(name in env for name in ('LARECOMP_HOST_INPUT', 'LARECOMP_BIND_TEST')):
        raise ValueError('--manual não aceita LARECOMP_HOST_INPUT/LARECOMP_BIND_TEST; eles substituem o controle físico.')
    env['LARECOMP_NO_USER_INPUT'] = '0'


def run_once(args, source, cache):
    if shutil.disk_usage(LOCAL).free < 4 * 1024**3:
        raise SystemExit('Race capture requires at least 4 GiB free on the local drive')
    candidate = json.loads((args.candidate or LOCAL / 'candidate.json').read_text(encoding='utf-8'))
    for entry in [candidate['executable'], *candidate['files'].values()]:
        path = Path(entry['path'])
        with path.open('rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        if actual != entry['sha256']:
            raise ValueError(f'Candidate changed on disk: {path}')
    dlc_root = session_dlc_root(args, candidate)

    session = Path(tempfile.mkdtemp(prefix=f'race-{args.mode}-', dir=LOCAL))
    print(f'Test output: {session}', flush=True)
    prepare_session(source, session, cache, args.frame_gen, args.fullscreen)
    data = session / 'data'
    profile = session / 'mii-profile.json'

    script = race_input(args.mode, args.last_frame, args.course, CUPS.index(args.cup), args.track,
                        args.assisted, args.assist_toggle, args.long_session, args.intro)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('MK8_', 'LARECOMP_', 'LASRECOMP_', 'RECOMP_', 'NX_LAN_'))}
    env.update(dict(args.env))
    env.update(
        MK8_EXPERIMENTAL_NO_MII='1', MK8_MII_PROFILE_PATH=str(profile),
        LARECOMP_DATA_ROOT=str(data), LARECOMP_GAME_LIB=candidate['files']['gamelib']['path'],
        LARECOMP_AUDIO_GAIN='1' if args.audio else '0',
        LARECOMP_INPUT=args.input or script,
        LARECOMP_WINDOW_SHOT=f'{args.last_frame - args.shot_step * (args.shots - 1)}:{args.last_frame}:{args.shot_step}',
        LARECOMP_SHOT_PATH=(session / 'frame-%05d.png').as_posix(),
        LARECOMP_EXIT_AFTER_SHOT='1',
    )
    host_captures = []
    if args.assisted:
        host_captures.append('1600:shot:assists')
        env.update(LARECOMP_VK_PROF='1', MK8_FRAME_TIMES='1', MK8_TRACE_VRAM='1')
        if args.cup != 'mushroom' or args.intro:
            host_captures.append('2440:shot:cup')
    elif CUPS.index(args.cup) >= 12 and not args.manual:
        host_captures.append('2250:shot:cup')
    if host_captures:
        env['LARECOMP_HOST_INPUT'] = ','.join(filter(None, [env.get('LARECOMP_HOST_INPUT'), *host_captures]))
    if dlc_root is not None:
        env['MK8_DLC_ROOT'] = dlc_root
    if args.manual:
        manual_input_environment(env)
    (session / 'run.json').write_text(json.dumps({
        'candidate': candidate['identity'], 'mode': args.mode, 'options': args.env,
        'course': args.course, 'cup': args.cup, 'track': args.track,
        'input': env['LARECOMP_INPUT'], 'last_frame': args.last_frame,
        'cache_from': str(cache.resolve()) if cache else None,
        'assisted': args.assisted, 'assist_toggle': args.assist_toggle,
        'long_session': args.long_session,
        'intro': args.intro,
        'manual': args.manual,
        'audio': args.audio,
        'frame_gen': args.frame_gen,
        'fullscreen': args.fullscreen,
        'environment': {key: value for key, value in env.items()
                        if key.startswith(('MK8_', 'LARECOMP_'))},
    }, indent=2), encoding='utf-8')
    started = monotonic()
    started_ns = time_ns()
    log = session / 'game.log'
    before = save_hashes(data / 'save')
    child = None
    try:
        with log.open('wb') as stream:
            child = subprocess.Popen(
                [candidate['executable']['path'], str(Path(candidate['data_root']) / 'extracted')],
                cwd=data, env=env, stdin=subprocess.DEVNULL, stdout=stream,
                stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0,
            )
            wait_capture(child, args.timeout, log)
    finally:
        collect_log(log, Path(candidate['executable']['path']).with_suffix('.log'), started_ns)
        (session / 'result.json').write_text(json.dumps({
            'exit_code': child.returncode if child else None,
            'elapsed_seconds': round(monotonic() - started, 3),
            'started_unix_ns': started_ns,
            'final_capture': (session / f'frame-{args.last_frame:05d}.png').is_file(),
            'assists_capture': (data / 'build/assists.png').is_file(),
            'save_changed': before != save_hashes(data / 'save'),
            'gameplay_verified': False,
        }, indent=2), encoding='utf-8')
    rows = PERF.findall(log.read_text(encoding='utf-8', errors='replace'))
    print(f'Exit {child.returncode}, {monotonic() - started:.1f}s', flush=True)
    for elapsed, frame, fps, peak in rows[-8:]:
        print(f'{elapsed}s | frame {frame} | {fps} FPS | max {peak} ms')
    if child.returncode or not (session / f'frame-{args.last_frame:05d}.png').is_file():
        raise SystemExit('Candidate failed to reach the final capture; inspect game.log')
    if args.assisted and not (data / 'build/assists.png').is_file():
        raise SystemExit('Captura dos auxílios ausente; não considerar o roteiro validado.')
    if args.assisted and '[frame] ' not in log.read_text(encoding='utf-8', errors='replace'):
        raise SystemExit('Log sem tempos de quadro; não considerar a coleta concluída.')
    print('Revisar capturas: atingir o último quadro não comprova chegada nem auxílios ativos.', flush=True)
    return session


def run(args):
    source = (args.candidate or LOCAL / 'candidate.json').resolve().parent
    cache = args.cache_from
    # Também bloqueia uma preparação ou jogo do launcher que usa esse candidato.
    with exclusive_build(source):
        before = save_hashes(source / 'user-no-mii/save')
        try:
            for _ in range(args.repeat):
                session = run_once(args, source, cache)
                cache = session / 'data/cache' if (session / 'data/cache').is_dir() else None
        finally:
            if before != save_hashes(source / 'user-no-mii/save'):
                raise RuntimeError('O save de origem mudou durante o teste; investigar antes de continuar.')


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['grand-prix', 'time-trial'])
    parser.add_argument('--last-frame', type=int)
    parser.add_argument('--shots', type=int)
    parser.add_argument('--shot-step', type=int)
    parser.add_argument('--timeout', type=int)
    parser.add_argument('--assisted', action='store_true',
                        help='Grand Prix sem segurar A; captura os auxílios no quadro 1600 para revisão')
    parser.add_argument('--intro', action='store_true',
                        help='Preserva o sobrevoo inicial do Grand Prix assistido, sem pressionar A depois da confirmação')
    parser.add_argument('--manual', action='store_true',
                        help='Aceita teclado/controle físico na sessão copiada; sem roteiro host que substitui o gamepad')
    parser.add_argument('--audio', action='store_true',
                        help='Habilita saída audível, respeitando o volume salvo; o padrão silencia somente a saída')
    parser.add_argument('--frame-gen', choices=('off', 'saved'), default='off',
                        help='Desliga frame generation na cópia (padrão) ou preserva o valor salvo para comparação')
    parser.add_argument('--fullscreen', choices=('saved', 'on', 'off'), default='saved',
                        help='Preserva a tela cheia salva (padrão) ou força ligada/desligada somente no INI copiado')
    parser.add_argument('--long-session', action='store_true',
                        help='Confirma menus para continuar corridas/copas no mesmo processo; revisar capturas')
    parser.add_argument('--assist-toggle', action='append', choices=['steering', 'accelerate'], default=[],
                        help='Inverte somente o auxílio indicado na cópia do save; conferir a captura')
    parser.add_argument('--repeat', type=int, default=1,
                        help='Repete com saves novos da mesma origem e cache produzido pela execução anterior')
    parser.add_argument('--input', help='Override frame:button:duration input sequence')
    parser.add_argument('--cache-from', type=Path, help='Copy a previous cache directory; saves remain isolated')
    parser.add_argument('--course', choices=['stadium', 'water-park', 'sweet-sweet-canyon'], default='stadium',
                        help='Time Trial course in Mushroom Cup')
    parser.add_argument('--cup', choices=CUPS, default='mushroom', help='Copa do Contra o Relógio ou Grand Prix assistido')
    parser.add_argument('--track', type=int, choices=range(4), help='Track 0-3 of --cup (overrides --course)')
    parser.add_argument('--env', action='append', default=[], type=lambda x: x.split('=', 1))
    parser.add_argument('--candidate', type=Path, help='candidate.json of another data folder (e.g. a fresh build)')
    args = parser.parse_args(argv)
    if args.assisted and (args.mode != 'grand-prix' or args.input):
        parser.error('--assisted exige grand-prix e não aceita --input')
    if args.assist_toggle and not args.assisted:
        parser.error('--assist-toggle exige --assisted')
    if args.long_session and not args.assisted:
        parser.error('--long-session exige --assisted')
    if args.intro and (not args.assisted or args.long_session):
        parser.error('--intro exige --assisted e não aceita --long-session')
    if args.manual and (args.assisted or any(pair[0] in ('LARECOMP_HOST_INPUT', 'LARECOMP_BIND_TEST') for pair in args.env)):
        parser.error('--manual não aceita --assisted ou roteiros host; use time-trial ou --input para navegar')
    defaults = ((5000, 240, 13, 150) if args.intro else (131100, 3000, 33, 4000) if args.long_session
                else (17100, 600, 8, 2000) if args.assisted else (3400, 180, 2, 400))
    for name, default in zip(('last_frame', 'timeout', 'shots', 'shot_step'), defaults):
        if getattr(args, name) is None:
            setattr(args, name, default)
    if args.mode != 'time-trial' and (args.course != 'stadium' or args.track is not None):
        parser.error('--course e --track exigem time-trial')
    if args.cup != 'mushroom' and args.mode != 'time-trial' and not args.assisted:
        parser.error('--cup exige time-trial ou --assisted')
    if (args.shots < 1 or args.shot_step < 1 or args.last_frame <= args.shot_step * (args.shots - 1)
            or args.timeout <= 0 or args.repeat < 1 or (args.assisted and args.last_frame <= 1600)
            or any(len(pair) != 2 for pair in args.env)):
        parser.error('Require positive capture frames, capture count and step, and --env NAME=VALUE')
    script = args.input or race_input(args.mode, args.last_frame, args.course, CUPS.index(args.cup),
                                      args.track, args.assisted, args.assist_toggle, args.long_session, args.intro)
    if len(script.encode('utf-8')) >= 16384:
        parser.error('Roteiro excede o buffer de entrada do runtime (16383 bytes); reduza a duração.')
    return args


if __name__ == '__main__':
    run(parse_args())
