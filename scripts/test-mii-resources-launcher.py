"""Generate MiiModel-layout resources from the launcher UI and start MK8 with them, using isolated saves."""
import argparse
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import time

from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]
AFL = ROOT/'local/mii-library-research/AFLResHigh_2_3.dat'


def run(profile):
    assert shutil.disk_usage(ROOT/'local').free >= 4*1024**3
    assert AFL.is_file(), 'Private verified AFL resource required'
    metadata = json.loads(subprocess.check_output(['cargo', 'metadata', '--no-deps', '--format-version', '1'], cwd=ROOT, text=True))
    launcher = Path(metadata['target_directory'])/'debug/Launcher.exe'
    local = Path(tempfile.mkdtemp(prefix='mii-resources-launcher-', dir=ROOT/'local')).resolve()
    print('Evidence:', local, flush=True)
    shutil.copyfile(ROOT/'local/candidate.json', local/'candidate.json')
    shutil.copytree(ROOT/'local/user-no-mii/save', local/'user-no-mii/save')
    shutil.copyfile(ROOT/'local/user-no-mii/mk8d.ini', local/'user-no-mii/mk8d.ini')
    if profile:
        shutil.copyfile(profile, local/'mii-profile.json')
    script = runpy.run_path(str(ROOT/'scripts/test-race.py'))['race_input']('time-trial', 2100, character='mii')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('MK8_', 'LARECOMP_', 'RECOMP_', 'NX_LAN_'))}
    env.update(LARECOMP_INPUT=script, LARECOMP_WINDOW_SHOT='1300:2100:800',
               LARECOMP_SHOT_PATH=(local/'frame-%05d.png').as_posix(), LARECOMP_EXIT_AFTER_SHOT='1')
    log = (local/'launcher-test.log').open('w')
    process = subprocess.Popen([str(launcher), '--headless', '--data-dir', str(local)], cwd=ROOT, env=env,
                               stdout=log, stderr=subprocess.STDOUT)
    game_pid = None
    try:
        for _ in range(200):
            if (local/'launcher.json').exists():
                break
            assert process.poll() is None
            time.sleep(.1)
        url = json.loads((local/'launcher.json').read_text())['url']
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
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

            page.locator('[data-tab="miis"]').click()
            page.get_by_text('Modelos dentro do jogo — experimental').click()
            # Without a configured AFL the button reports the requirement and enables nothing.
            page.locator('#mii-resources-generate').click()
            expect(page.locator('#mii-resources-status')).to_contain_text('AFL')
            assert api('mii/resources') is None
            api('mii/composer/resource', {'path': str(AFL)})
            page.locator('#mii-resources-generate').click()
            expect(page.locator('#mii-resources-status')).to_contain_text('ativados', timeout=120000)
            resources = api('mii/resources')
            assert resources['enabled'] and len(resources['files']) == 6
            generated = Path(resources['directory'].removeprefix('\\\\?\\'))
            assert generated.parent.parent == local/'mii-resources-generated'
            manifest = json.loads((generated/'manifest.json').read_text())
            assert not manifest['rendering_verified']
            page.screenshot(path=str(local/'generated.png'))
            print('Resources generated through the launcher UI.', flush=True)
            page.locator('[data-tab="play"]').click()
            expect(page.locator('#play-solo')).to_be_enabled(timeout=30000)
            with page.expect_response(lambda r: r.url.endswith('/api/play'), timeout=60000) as started:
                page.locator('#play-solo').click()
            assert started.value.status == 200, started.value.text()
            game_pid = api('status')['game_pid']
            assert game_pid
            deadline = time.monotonic()+180
            while time.monotonic() < deadline:
                info = api('status')
                if info['game_pid'] is None:
                    game_pid = None
                    break
                page.wait_for_timeout(1000)
            assert game_pid is None, 'Game timed out'
            assert info['game_exit'].endswith('0'), info['game_exit']
            for name in ('frame-01300.png', 'frame-02100.png'):
                assert (local/name).is_file(), name
            # The launcher's game.log stays empty for the re-executed runtime; captures are the evidence.
            # A second generation creates another folder and never overwrites the active one.
            page.locator('[data-tab="miis"]').click()
            page.locator('#mii-resources-generate').click()
            deadline = time.monotonic()+120
            while api('mii/resources/job')['state'] == 'running':
                assert time.monotonic() < deadline, 'Second generation timed out'
                page.wait_for_timeout(500)
            assert api('mii/resources/job')['state'] == 'done'
            assert Path(api('mii/resources')['directory'].removeprefix('\\\\?\\')) != generated
            assert (generated/'ShapeHigh.dat').is_file()
            assert not errors, errors
            (local/'evidence.json').write_text(json.dumps({'launcher_generation': True, 'launcher_game_exit': 0,
                'resources': str(generated), 'rendering_verified': False, 'gameplay_verified': False,
                'multiplayer_verified': False}, indent=2))
            browser.close()
            print('PASS: AFL requirement, UI generation, real launch with generated resources, second generation kept the first.', flush=True)
    finally:
        if game_pid:
            subprocess.run(['taskkill', '/PID', str(game_pid), '/T', '/F'], capture_output=True, check=False)
        process.terminate()
        process.wait(timeout=15)
        log.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, help='mii-profile.json copied into the isolated data folder')
    run(parser.parse_args().profile)
