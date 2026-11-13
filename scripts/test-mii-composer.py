"""Mii creator with the real launcher, browser, private parts library and prepared game; disposable profiles."""
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
        candidate = ROOT/'local/candidate.json'  # the prepared game provides the body (read in place)
        assert candidate.is_file(), 'Prepared game required'
        shutil.copyfile(candidate, local/'candidate.json')
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
                page.route('**/api/mii/composer/download', lambda route: route.fulfill(json={
                    'ok':True,'data':{'state':'failed','detail':'Download indisponível no teste manual.'}}))
                page.goto(url, wait_until='load')
                token = page.locator('meta[name="mk8-token"]').get_attribute('content')

                def api(path, body=None, code=200):
                    args = {'headers': {'X-MK8-Token': token}}
                    response = page.request.get(url+'/api/'+path, **args) if body is None else page.request.post(url+'/api/'+path, data=body, **args)
                    assert response.status == code, response.text()
                    value = response.json()
                    return value.get('data') if code == 200 else value

                page.locator('[data-tab="miis"]').click()
                # The parts library is chosen once; then the head is drawn on MK8's own body.
                expect(page.locator('#mii-library')).to_be_visible()
                page.locator('#mii-library summary').click()
                page.locator('#mii-library-path').fill(str(RESOURCE))
                page.locator('#mii-library-load').click()
                expect(page.locator('#mii-library')).to_be_hidden(timeout=30000)
                expect(page.locator('#mii-stage')).to_have_attribute('data-body', 'MaleBody', timeout=30000)
                page.locator('#mii-name').fill('Editor A')
                page.locator('#mii-save').click()
                expect(page.locator('#mii-stage')).to_have_attribute('data-head', 'true', timeout=30000)
                # Every hairstyle is a button showing this Mii with that hairstyle.
                page.locator('#mii-tabs [data-part="hair"]').click()
                expect(page.locator('#mii-options')).to_have_attribute('data-thumbnails', 'ready', timeout=120000)
                expect(page.locator('#mii-options .mii-choice[data-offset="49"] canvas')).to_have_count(132)
                page.locator('#mii-options .mii-choice[data-offset="49"][data-value="24"]').click()
                page.locator('#mii-tabs [data-part="eyes"]').click()
                expect(page.locator('#mii-options .mii-choice[data-offset="52"] canvas').first).to_be_visible(timeout=60000)
                page.locator('#mii-options .mii-choice[data-offset="52"][data-value="10"]').click()
                page.locator('#mii-options').screenshot(path=str(output/'creator-eyes.png'))
                page.locator('#mii-tabs [data-part="body"]').click()
                expect(page.locator('#mii-options .mii-swatch[data-offset="39"]')).to_have_count(12)
                assert 'rgb' in page.locator('#mii-options .mii-swatch[data-offset="39"]').first.evaluate('b => getComputedStyle(b).backgroundColor')
                page.locator('#mii-options .mii-choice[data-offset="40"][data-value="1"]').click()
                expect(page.locator('#mii-stage')).to_have_attribute('data-body', 'FemaleBody')
                page.locator('#mii-options input[data-offset="41"]').fill('127')
                page.locator('#mii-options .mii-swatch[data-offset="39"][data-value="5"]').click()
                page.wait_for_timeout(500)
                page.locator('#mii-stage').screenshot(path=str(output/'creator-body.png'))
                page.screenshot(path=str(output/'creator.png'))
                page.locator('#mii-save').click()
                expect(page.locator('#mii-status')).to_contain_text('Mii salvo')
                # Creating the Mii (with the library chosen) prepared the game's Mii resources in the background.
                deadline = time.monotonic()+180
                while api('status')['mii_preparation']['resources']['state'] == 'running':
                    assert time.monotonic() < deadline, 'Mii resources timed out'
                    page.wait_for_timeout(500)
                assert api('status')['mii_preparation']['resources']['state'] == 'done'
                data = api('mii/export')['data']
                assert (data[49], data[52], data[40], data[41], data[39]) == (24, 10, 1, 127, 5), data[39:53]
                assert api('mii/resources')['enabled']
                # The body is the game's own: GLB from the prepared game, refused for anything but the verified file.
                body = page.request.get(url+'/api/mii/body', headers={'X-MK8-Token': token})
                assert body.status == 200 and body.body()[:4] == b'glTF'
                assert page.request.get(url+'/api/mii/body').status == 403
                page.reload(wait_until='load')
                page.locator('[data-tab="miis"]').click()
                expect(page.locator('#mii-stage')).to_have_attribute('data-body', 'FemaleBody', timeout=30000)
                expect(page.locator('#mii-name')).to_have_value('Editor A')
                assert not errors, errors
                assert not [u for u in requests if not u.startswith((url, 'blob:', 'data:'))]
                (output/'creator.json').write_text(json.dumps({'charinfo': data, 'body': 'FemaleBody',
                    'resources_enabled': True, 'native_integration_verified': False}, indent=2))
                browser.close()
                print('PASS: library once, drawn options, MK8 body by gender/height/color, save, game resources, reload; no external requests.')
        finally:
            process.terminate()
            process.wait(timeout=15)
            log.close()
            assert process.poll() is not None and local.parent == Path(tempfile.gettempdir()).resolve()


if __name__ == '__main__':
    run()
