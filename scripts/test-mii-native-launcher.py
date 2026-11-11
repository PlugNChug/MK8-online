"""Prepare a real editor head and start MK8 through the launcher, using isolated saves."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import tempfile
import time

from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]


def run(source):
    assert shutil.disk_usage(ROOT/'local').free >= 4*1024**3
    metadata = json.loads(subprocess.check_output(['cargo', 'metadata', '--no-deps', '--format-version', '1'], cwd=ROOT, text=True))
    launcher = Path(metadata['target_directory'])/'debug/Launcher.exe'
    local = Path(tempfile.mkdtemp(prefix='mii-native-launcher-', dir=ROOT/'local')).resolve()
    print('Evidence:', local, flush=True)
    shutil.copyfile(ROOT/'local/candidate.json', local/'candidate.json')
    shutil.copytree(ROOT/'local/user-no-mii/save', local/'user-no-mii/save')
    shutil.copyfile(ROOT/'local/user-no-mii/mk8d.ini', local/'user-no-mii/mk8d.ini')
    glb = source.read_bytes()
    size = struct.unpack_from('<I', glb, 12)[0]
    doc = json.loads(glb[20:20+size])
    data = doc['asset']['extras']['mii_charinfo']
    script = runpy.run_path(str(ROOT/'scripts/test-race.py'))['race_input']('time-trial', 4800, character='mii')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('MK8_', 'LARECOMP_', 'RECOMP_', 'NX_LAN_'))}
    env.update(LARECOMP_INPUT=script, LARECOMP_WINDOW_SHOT='2000:4800:1400',
               LARECOMP_SHOT_PATH=(local/'frame-%05d.png').as_posix(), LARECOMP_EXIT_AFTER_SHOT='1')
    log = (local/'launcher-test.log').open('w')
    process = subprocess.Popen([str(launcher), '--headless', '--data-dir', str(local)], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    game_pid = None
    try:
        for _ in range(200):
            if (local/'launcher.json').exists():
                break
            assert process.poll() is None
            time.sleep(.1)
        url = json.loads((local/'launcher.json').read_text())['url']
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True, args=['--enable-unsafe-swiftshader'])
            page = browser.new_page(viewport={'width': 1400, 'height': 1200})
            errors = []
            page.on('pageerror', lambda e: errors.append(str(e)))
            page.goto(url, wait_until='networkidle')
            token = page.locator('meta[name="mk8-token"]').get_attribute('content')

            def api(path, body=None, code=200):
                kwargs = {'headers': {'X-MK8-Token': token}, 'timeout': 60000}
                response = page.request.get(url+'/api/'+path, **kwargs) if body is None else page.request.post(url+'/api/'+path, data=body, **kwargs)
                assert response.status == code, response.text()
                value = response.json()
                return value.get('data') if code == 200 else value['error']

            def conversion(expected):
                deadline = time.monotonic()+150
                while time.monotonic() < deadline:
                    info = api('mii/native')
                    if info['state'] != 'running':
                        assert info['state'] == expected, info
                        return info
                    page.wait_for_timeout(500)
                raise AssertionError('Conversion timed out')

            api('mii/composer/resource', {'path': str(ROOT/'local/mii-library-research/AFLResHigh_2_3.dat')})
            api('mii/import', data)
            api('mii/composer/save', {'expected': data, 'data': data, 'glb': list(glb)})
            page.reload(wait_until='networkidle')
            page.locator('[data-tab="miis"]').click()
            page.locator('#mii-compose-open').click()
            expect(page.locator('#mii-native-prepare')).to_be_enabled(timeout=30000)
            page.locator('#mii-native-prepare').click()
            info = conversion('done')
            assert info['ready'] and info['enabled']
            print('UI conversion completed.', flush=True)
            expect(page.locator('#mii-native-status')).to_contain_text('próxima abertura')
            page.screenshot(path=str(local/'prepared.png'))
            profile_dir = local/'profiles'/bytes(data[:16]).hex()
            receipt = json.loads((profile_dir/'native-head.json').read_text())
            assert receipt['glb_sha256'] == hashlib.sha256(glb).hexdigest()
            overlay = Path(receipt['overlay']['path'])
            # The game reads Driver/MiiM.szs for every Mii; female heads use the MiiF body under that name.
            assert overlay.samefile(next(profile_dir.glob('native/*/model/romfs/Driver/MiiM.szs')))
            manifest = json.loads((overlay.parents[2]/'manifest.json').read_text())
            assert manifest['body'] == ('MiiF' if data[40] else 'MiiM') and manifest['overlay_file'] == 'MiiM.szs'
            page.locator('#mii-compose-close').click()
            page.locator('[data-tab="play"]').click()
            expect(page.locator('#play-solo')).to_be_enabled(timeout=30000)
            with page.expect_response(lambda r: r.url.endswith('/api/play'), timeout=60000) as started:
                page.locator('#play-solo').click()
            assert started.value.status == 200, started.value.text()
            game_pid = api('status')['game_pid']
            assert game_pid
            print('Game started through launcher:', game_pid, flush=True)
            deadline = time.monotonic()+180
            while time.monotonic() < deadline:
                info = api('status')
                if info['game_pid'] is None:
                    game_pid = None
                    break
                page.wait_for_timeout(1000)
            assert game_pid is None, 'Game timed out'
            assert info['game_exit'].endswith('0'), info['game_exit']
            for name in ('frame-02000.png', 'frame-03400.png', 'frame-04800.png'):
                assert (local/name).is_file(), name
            assert not (overlay.parents[2]/'save').exists(), 'Overlay must not capture saves'
            print('Menu and opening-lap captures completed.', flush=True)
            changed = data.copy()
            changed[39] = (changed[39]+1) % 12
            api('mii/data', changed)
            stale = api('mii/native')
            assert stale['enabled'] and not stale['ready']
            assert 'desatualizado' in api('play', {'online': False}, 400)
            assert api('status')['game_pid'] is None
            page.reload(wait_until='networkidle')
            page.locator('[data-tab="miis"]').click()
            page.locator('#mii-compose-open').click()
            expect(page.locator('#mii-native-disable')).to_be_enabled()
            page.locator('#mii-native-disable').click()
            expect(page.locator('#mii-native-disable')).to_be_disabled()
            assert not api('mii/native')['enabled']
            # A different active profile while conversion runs must never be auto-enabled.
            api('mii/data', data)
            api('mii/native/prepare', {'expected': data})
            api('mii/create', {'name': 'Other', 'gender': 0, 'color': 0})
            failed = conversion('failed')
            assert not failed['enabled'] and 'perfil mudou' in failed['detail']
            assert not json.loads((profile_dir/'native-head.json').read_text())['enabled']
            # An unsupported FFL part must report process failure, not activate old output.
            api('mii/select', {'id': bytes(data[:16]).hex()})
            doc['meshes'][0]['primitives'][0]['extras']['modulateType'] = 8
            raw_json = json.dumps(doc, separators=(',', ':')).encode()
            raw_json += b' '*(-len(raw_json) % 4)
            binary_chunk = glb[20+size:]
            bad_glb = b'glTF'+struct.pack('<III', 2, 20+len(raw_json)+len(binary_chunk), len(raw_json))+b'JSON'+raw_json+binary_chunk
            api('mii/composer/save', {'expected': data, 'data': data, 'glb': list(bad_glb)})
            api('mii/native/prepare', {'expected': data})
            assert not conversion('failed')['enabled']
            # Restore the valid saved head and reactivate its cached native asset without another conversion.
            api('mii/composer/save', {'expected': data, 'data': data, 'glb': list(glb)})
            jobs_before = set((profile_dir/'native').iterdir())
            page.reload(wait_until='networkidle')
            page.locator('[data-tab="miis"]').click()
            page.locator('#mii-compose-open').click()
            expect(page.locator('#mii-native-prepare')).to_have_text('Ativar no jogo')
            page.locator('#mii-native-prepare').click()
            expect(page.locator('#mii-native-status')).to_contain_text('próxima abertura')
            assert api('mii/native')['enabled']
            page.locator('#mii-native-disable').click()
            expect(page.locator('#mii-native-status')).to_have_text('Uso no jogo desativado.')
            assert set((profile_dir/'native').iterdir()) == jobs_before
            assert not errors, errors
            (local/'evidence.json').write_text(json.dumps({'source_glb': source.name, 'body': manifest['body'], 'launcher_ui_conversion': True, 'launcher_game_exit': 0,
                'profile_id': bytes(data[:16]).hex(), 'native_receipt': receipt, 'stale_launch_rejected': True,
                'profile_switch_rejected': True, 'converter_failure_reported': True, 'cached_reactivation': True, 'gameplay_verified': False,
                'multiplayer_verified': False}, indent=2))
            browser.close()
            print('PASS: UI conversion, actual launcher game start, stale refusal, disable, profile-switch and process-failure guards.', flush=True)
    finally:
        if game_pid:
            subprocess.run(['taskkill', '/PID', str(game_pid), '/T', '/F'], capture_output=True, check=False)
        process.terminate()
        process.wait(timeout=15)
        log.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--glb', type=Path, default=ROOT/'local/mii-composer-check/profile-B.glb',
                        help='Saved editor GLB; profile-C.glb from test-mii-composer.py is the female case')
    run(parser.parse_args().glb)
