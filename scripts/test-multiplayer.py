"""Exercise two real launchers/candidates with isolated saves; captures are evidence, not qualification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def protected_data(local):
    files = [local/'config.json', local/'mii-profile.json', local/'user-no-mii/mk8d.ini']
    files += list((local/'user-no-mii/save').rglob('*'))
    files += list((local/'profiles').rglob('*'))
    result = {}
    for path in files:
        if path.is_file():
            with path.open('rb') as stream:
                result[path.relative_to(local).as_posix()] = hashlib.file_digest(stream, 'sha256').hexdigest()
    return result


def api(url, token, route, body=None):
    request = urllib.request.Request(url.rstrip('/') + '/api/' + route,
        data=None if body is None else json.dumps(body).encode(),
        headers={'X-MK8-Token': token, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=60) as response:
        value = json.load(response)
    if not value['ok']:
        raise RuntimeError(value.get('error', value))
    return value['data']


def run(args):
    metadata = json.loads(subprocess.check_output(
        ['cargo', 'metadata', '--no-deps', '--format-version', '1'], cwd=ROOT, text=True))
    binaries = Path(metadata['target_directory']) / 'release'
    local = ROOT / 'local'
    if shutil.disk_usage(local).free < 4 * 1024**3:
        raise RuntimeError('At least 4 GiB free required')
    session = Path(tempfile.mkdtemp(prefix='multiplayer-', dir=local))
    protected_before = protected_data(local)
    print(f'Test output: {session}', flush=True)
    (session/'run.json').write_text(json.dumps({
        'candidate': json.loads((local/'candidate.json').read_text())['identity'],
        'relay': args.relay, 'p2p': args.p2p,
        'host_input': args.host_input, 'guest_input': args.guest_input,
        'shots': args.shots, 'options': args.env,
        'mii_resources': str(args.mii_resources) if args.mii_resources else None,
        'profiles': [str(p) if p else None for p in (args.host_profile, args.guest_profile)],
    }, indent=2), encoding='utf-8')
    children, clients = [], []
    clean = {k: v for k, v in os.environ.items()
             if not k.startswith(('MK8_', 'LARECOMP_', 'RECOMP_', 'NX_LAN_'))}
    started = time.monotonic()
    try:
        if args.relay:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.bind(('127.0.0.1', 0))
                port = min(probe.getsockname()[1], 65000)
            relay = subprocess.Popen([str(binaries/'mk8-relay.exe'),
                '127.0.0.1:0', '127.0.0.1', '127.0.0.1', str(port)],
                stdout=subprocess.PIPE, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(relay)
            line = relay.stdout.readline().strip()
            if not line.startswith('MK8 relay '):
                raise RuntimeError('Relay did not start')
            clean['MK8_RELAY_URL'] = 'http://' + line.removeprefix('MK8 relay ')
        for index in range(2):
            folder = session / str(index)
            folder.mkdir()
            for name in ('candidate.json', 'config.json'):
                shutil.copyfile(local/name, folder/name)
            if args.mii_profile:
                shutil.copyfile(args.mii_profile, folder/'mii-profile.json')
            saves = folder/'user-no-mii'
            shutil.copytree(local/'user-no-mii/save', saves/'save')
            shutil.copyfile(local/'user-no-mii/mk8d.ini', saves/'mk8d.ini')
            if args.cache_from:
                shutil.copytree(args.cache_from, saves/'cache')
            profile = args.host_profile if index == 0 else args.guest_profile
            if profile:
                # Each player gets its own Mii profile; the personal profile is never modified.
                shutil.copyfile(profile, folder/'mii-profile.json')
            env = dict(clean, LARECOMP_INPUT=args.host_input if index == 0 else args.guest_input,
                LARECOMP_WINDOW_SHOT=args.shots, LARECOMP_EXIT_AFTER_SHOT='1',
                LARECOMP_SHOT_PATH=(folder/'frame-%05d.png').as_posix(),
                RECOMP_NET_LOG='1')
            env.update(dict(args.env))
            process = subprocess.Popen([str(binaries/'Launcher.exe'), '--headless',
                '--data-dir', str(folder)], env=env, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(process)
            deadline = time.monotonic() + 20
            while not (folder/'launcher.json').exists():
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Launcher did not start')
                time.sleep(.05)
            url = json.loads((folder/'launcher.json').read_text())['url']
            with urllib.request.urlopen(url) as response:
                html = response.read().decode()
            token = re.search(r'name="mk8-token" content="([^"]+)"', html)[1]
            clients.append((url, token))
            api(url, token, 'username', {'name': 'Host' if index == 0 else 'Guest'})
            if args.mii_resources:
                # Same launcher option as "Modelos dentro do jogo": hashes the six files and enables the renderer.
                api(url, token, 'mii/resources', {'directory': str(args.mii_resources), 'enabled': True})
        if args.relay:
            invite = api(*clients[0], 'session/host-internet', {})['code']
        elif args.p2p:
            api(*clients[0], 'session/host', {'bind': '127.0.0.1:0'})
            invite = api(*clients[0], 'status')['session']['room_code']
            if not invite or not invite.startswith('MK8P-'):
                raise RuntimeError(f'Expected MK8P- room code, got {invite!r}')
        else:
            api(*clients[0], 'session/host', {'bind': '127.0.0.1:0'})
            invite = api(*clients[0], 'session/invite', {})['invite']
        api(*clients[1], 'session/join', {'invite': invite})
        deadline = time.monotonic() + 10
        while not all(api(*client, 'status')['session']['connected'] for client in clients):
            if time.monotonic() > deadline:
                raise RuntimeError('Transport connection timed out')
            time.sleep(.1)
        print('Two transports connected; launching actual games', flush=True)
        for client in clients:
            print(api(*client, 'play', {'online': True})['message'], flush=True)
        # Re-exec can replace the tracked game process. Final screenshots, not its first exit, end the test.
        final_frame = int(args.shots.split(':')[1]) if ':' in args.shots else int(args.shots)
        while not all((session/str(i)/f'frame-{final_frame:05d}.png').is_file() for i in range(2)):
            if shutil.disk_usage(local).free < 4 * 1024**3:
                raise RuntimeError('Stopping test to preserve the 4 GiB disk reserve')
            for i in range(2):
                log = session/str(i)/'game.log'
                if log.exists() and re.search(r'FATAL:|\*\*\* exception', log.read_text(errors='replace')):
                    raise RuntimeError(f'Runtime {i} failed; inspect {log}')
            if time.monotonic() - started > args.timeout:
                raise RuntimeError('Games did not reach final captures; inspect logs')
            time.sleep(1)
        time.sleep(2)
        print(f'Both final captures reached after {time.monotonic()-started:.1f}s', flush=True)
    finally:
        # Kill only these launcher descendants on a failed run; preserve all artifacts for diagnosis.
        for child in reversed(children):
            if child.poll() is None:
                subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                try:
                    child.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=15)
            if child.stdout:
                child.stdout.close()
        protected_after = protected_data(local)
        unchanged = protected_before == protected_after
        (session/'protected-data.json').write_text(json.dumps({
            'unchanged': unchanged, 'before': protected_before, 'after': protected_after,
        }, indent=2), encoding='utf-8')
        if not unchanged:
            raise RuntimeError(f'Protected data changed during the test; inspect {session}/protected-data.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--relay', action='store_true', help='Local relay through the internet-code path')
    parser.add_argument('--p2p', action='store_true', help='Single reusable P2P room code (MK8P-...) path')
    parser.add_argument('--host-input', required=True)
    parser.add_argument('--guest-input', required=True)
    parser.add_argument('--shots', default='1200:2400:400')
    parser.add_argument('--cache-from', type=Path)
    parser.add_argument('--mii-profile', type=Path, help='Optional mii-profile.json to copy into each instance')
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--env', action='append', default=[], type=lambda x: x.split('=', 1))
    parser.add_argument('--mii-resources', type=Path, help='MiiModel-layout folder; enables the original Mii renderer')
    parser.add_argument('--host-profile', type=Path, help='mii-profile.json copied into the host test folder')
    parser.add_argument('--guest-profile', type=Path, help='mii-profile.json copied into the guest test folder')
    parsed = parser.parse_args()
    if parsed.mii_resources:
        parsed.mii_resources = parsed.mii_resources.resolve()
    run(parsed)
