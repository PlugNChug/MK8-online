"""Real launcher + browser composition test, with isolated disposable profiles."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]
RESOURCE = ROOT / 'local/mii-library-research/AFLResHigh_2_3.dat'


def run():
    metadata = json.loads(subprocess.check_output(
        ['cargo', 'metadata', '--no-deps', '--format-version', '1'], cwd=ROOT, text=True))
    executable = Path(metadata['target_directory']) / 'debug/Launcher.exe'
    assert executable.is_file(), 'Build the launcher first'
    assert RESOURCE.is_file(), 'Private verified AFL resource required'
    assert shutil.disk_usage(tempfile.gettempdir()).free >= 4 * 1024**3
    output = ROOT / 'local/mii-composer-check'
    output.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mk8-composer-') as folder:
        local = Path(folder).resolve()
        assert local.parent == Path(tempfile.gettempdir()).resolve()
        log = (local / 'test-launcher.log').open('w')
        process = subprocess.Popen([str(executable), '--headless', '--data-dir', str(local)],
                                   cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(200):
                if (local / 'launcher.json').exists():
                    break
                assert process.poll() is None, 'Launcher exited'
                time.sleep(.1)
            url = json.loads((local / 'launcher.json').read_text())['url']
            with sync_playwright() as p:
                browser = p.chromium.launch(headless=True, args=['--enable-unsafe-swiftshader'])
                page = browser.new_page(viewport={'width': 1400, 'height': 1100})
                errors, requests = [], []
                page.on('pageerror', lambda e: errors.append(str(e)))
                page.on('console', lambda m: errors.append(m.text) if m.type == 'error' and '400' not in m.text else None)
                page.on('request', lambda r: requests.append(r.url))
                page.goto(url, wait_until='networkidle')
                token = page.locator('meta[name="mk8-token"]').get_attribute('content')

                def api(path, body=None, code=200):
                    args = {'headers': {'X-MK8-Token': token}}
                    response = page.request.get(url+'/api/'+path, **args) if body is None else page.request.post(url+'/api/'+path, data=body, **args)
                    assert response.status == code, response.text()
                    value = response.json()
                    return value.get('data') if code == 200 else value

                assert page.request.get(url+'/api/mii/composer/resource').status == 403
                page.locator('[data-tab="miis"]').click()
                page.locator('#mii-name').fill('Editor A')
                page.locator('#mii-form button[type=submit]').click()
                expect(page.locator('#mii-full-fields input')).to_have_count(49)
                first = api('mii')['active']
                page.locator('#mii-compose-open').click()
                expect(page.locator('#mii-compose-status')).to_contain_text('Selecione o arquivo AFL')
                page.locator('#mii-compose-path').fill(str(RESOURCE))
                page.locator('#mii-compose-resource').click()
                expect(page.locator('#mii-compose-canvas')).to_have_attribute('data-rendered', 'true', timeout=30000)
                hair = page.locator('#mii-full-fields input[data-offset="49"]')
                hair.fill('132')
                expect(page.locator('#mii-compose-status')).to_contain_text('Valor fora do intervalo')
                hair.fill('24')
                page.locator('#mii-full-fields input[data-offset="46"]').fill('2')
                expect(page.locator('#mii-compose-status')).to_contain_text('Prévia atualizada')
                page.locator('#mii-full-save').click()
                expect(page.locator('#mii-compose-reopen')).to_be_enabled(timeout=30000)
                before = api('mii/export')['data']
                head_a = api('mii/composer')['head']
                assert before[49] == 24 and before[46] == 2
                assert head_a['charinfo'] == before and not head_a['native_integration_verified']
                page.locator('#mii-compose-canvas').screenshot(path=str(output/'profile-A.png'))
                page.screenshot(path=str(output/'editor.png'))
                page.locator('#mii-compose-reopen').click()
                expect(page.locator('#mii-compose-status')).to_have_text('Modelo salvo reaberto.')
                page.locator('#mii-compose-canvas').screenshot(path=str(output/'saved-A.png'))
                with page.expect_download() as download:
                    page.locator('#mii-compose-export').click()
                download.value.save_as(output/'profile-A.glb')
                glb = (output/'profile-A.glb').read_bytes()
                assert hashlib.sha256(glb).hexdigest() == head_a['sha256']
                page.locator('#mii-compose-close').click()
                page.locator('#mii-new').click()
                page.locator('#mii-name').fill('Editor B')
                page.locator('#mii-form button[type=submit]').click()
                expect(page.locator('#mii-full-fields input')).to_have_count(49)
                page.locator('#mii-compose-open').click()
                expect(page.locator('#mii-compose-reopen')).to_be_disabled()
                hair.fill('12')
                page.locator('#mii-full-fields input[data-offset="45"]').fill('4')
                page.locator('#mii-full-save').click()
                expect(page.locator('#mii-compose-reopen')).to_be_enabled(timeout=30000)
                head_b = api('mii/composer')['head']
                assert head_b['sha256'] != head_a['sha256']
                page.locator('#mii-compose-canvas').screenshot(path=str(output/'profile-B.png'))
                # A delayed save for A must not overwrite the newly selected B.
                api('mii/composer/save', {'expected': before, 'data': before, 'glb': list(glb)}, 400)
                assert api('mii/composer')['head'] == head_b
                page.locator('#mii-compose-close').click()
                first_id = bytes(first['id']).hex()
                page.locator('#mii-profiles').select_option(first_id)
                page.locator('#mii-select').click()
                expect(page.locator('#mii-name')).to_have_value('Editor A')
                page.reload(wait_until='networkidle')
                page.locator('[data-tab="miis"]').click()
                page.locator('#mii-compose-open').click()
                expect(page.locator('#mii-compose-reopen')).to_be_enabled(timeout=30000)
                expect(hair).to_have_value('24')
                assert api('mii/composer')['head'] == head_a
                # Invalid GLB and changed resource preserve existing profile and model.
                api('mii/composer/save', {'expected': before, 'data': before, 'glb': [0]*32}, 400)
                invalid = local/'invalid.dat'
                invalid.write_bytes(b'not AFL')
                api('mii/composer/resource', {'path': str(invalid)}, 400)
                assert api('mii/export')['data'] == before
                assert api('mii/composer')['head'] == head_a
                page.locator('#mii-compose-close').click()
                page.locator('#mii-color').fill('7')
                page.locator('#mii-form button[type=submit]').click()
                expect(page.locator('#mii-full-fields input[data-offset="39"]')).to_have_value('7')
                assert api('mii/composer')['head'] is None, 'Changed profile must invalidate its head'
                assert (local/'profiles'/first_id/f"assembled-{head_a['sha256']}.glb").read_bytes() == glb
                assert len(api('mii')['profiles']) == 2
                page.locator('#language').select_option('en')
                expect(page.locator('#mii-compose-open')).to_have_text('Edit Mii in 3D')
                page.locator('#mii-compose-open').click()
                expect(page.locator('#mii-compose-status')).to_contain_text('Preview updated')
                expect(page.locator('#mii-full-save')).to_have_text('Save profile and model')
                assert not errors, errors
                assert not [u for u in requests if not u.startswith((url, 'blob:', 'data:'))]
                (output/'manifest.json').write_text(json.dumps({'profiles': [head_a, head_b], 'reload_verified': True,
                    'stale_save_rejected': True, 'native_integration_verified': False}, indent=2))
                browser.close()
                print('PASS: two profiles, live parts, GLB save/reopen/export, reload, stale/invalid rejection; no external requests.')
        finally:
            process.terminate()
            process.wait(timeout=15)
            log.close()
            assert process.poll() is not None and local.parent == Path(tempfile.gettempdir()).resolve()


if __name__ == '__main__':
    run()
