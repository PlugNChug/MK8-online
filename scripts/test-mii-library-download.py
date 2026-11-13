"""Opt-in live archive download, real failure/retry, offline cache and browser checks.

Build the launcher first. Uses disposable data, never the user's profiles or library.
The normal offline browser suite does not depend on the archive's availability.
"""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]
SHA256 = '4a4be71d75162c20b48720ef89cd3d4e6cd4e8e21bcbc2aed63ee08d96de7722'


@contextmanager
def launcher(executable, local, offline=False):
    env = os.environ.copy()
    if offline:
        env.update(HTTPS_PROXY='http://127.0.0.1:1', ALL_PROXY='http://127.0.0.1:1', NO_PROXY='')
    marker = local / 'launcher.json'
    marker.unlink(missing_ok=True)
    with (local / 'download-test.log').open('a') as log:
        process = subprocess.Popen([str(executable), '--headless', '--data-dir', str(local)],
                                   cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 30
            while not marker.exists():
                assert process.poll() is None, 'Launcher exited'
                assert time.monotonic() < deadline, 'Launcher did not start'
                time.sleep(.05)
            yield json.loads(marker.read_text())['url']
        finally:
            process.terminate()
            process.wait(timeout=15)


def run():
    metadata = json.loads(subprocess.check_output(
        ['cargo', 'metadata', '--no-deps', '--format-version', '1'], cwd=ROOT, text=True))
    executable = Path(metadata['target_directory']) / 'debug/Launcher.exe'
    with tempfile.TemporaryDirectory(prefix='mk8-library-') as directory, sync_playwright() as pw:
        local = Path(directory)
        target = local / 'mii-library/AFLResHigh_2_3.dat'
        config = local / 'mii-composer.json'
        browser = pw.chromium.launch(headless=True, args=['--enable-unsafe-swiftshader'])
        try:
            # A real unreachable proxy exercises curl failure, cleanup and the retry button.
            with launcher(executable, local, offline=True) as url:
                page = browser.new_page(locale='pt-BR')
                errors = []
                page.on('pageerror', lambda e: errors.append(str(e)))
                page.goto(url, wait_until='networkidle')
                token = page.locator('meta[name="mk8-token"]').get_attribute('content')
                headers = {'X-MK8-Token': token}
                endpoint = url + '/api/mii/composer/download'
                assert page.request.get(endpoint, headers=headers).json()['data']['state'] == 'idle'
                assert page.request.post(endpoint, data={}).status == 403
                assert not target.exists()
                page.locator('[data-tab="miis"]').click()
                expect(page.locator('#mii-library-status')).to_contain_text('Não foi possível baixar do Internet Archive', timeout=30000)
                expect(page.locator('#mii-library-download')).to_be_enabled()
                page.locator('#mii-library-download').click()
                expect(page.locator('#mii-library-download')).to_be_enabled(timeout=30000)
                assert page.request.get(endpoint, headers=headers).json()['data']['state'] == 'failed'
                assert not config.exists() and not target.exists()
                assert not list((local / 'mii-library').glob('*.partial'))
                page.locator('#mii-library summary').click()
                expect(page.locator('#mii-library-path')).to_be_visible()
                page.locator('#language').select_option('en')
                expect(page.locator('#mii-library-download')).to_have_text('Retry download')
                expect(page.locator('#mii-library-status')).to_contain_text('Could not download from the Internet Archive')
                assert not errors, errors
                page.close()
            # First successful visit: no resource fixture and no network mocks.
            with launcher(executable, local) as url:
                page = browser.new_page(locale='pt-BR', viewport={'width':1280,'height':900})
                page.goto(url, wait_until='networkidle')
                page.locator('#language').select_option('pt-BR')
                page.locator('[data-tab="miis"]').click()
                expect(page.locator('#mii-library-status')).to_contain_text('Internet Archive', timeout=30000)
                expect(page.locator('#mii-library')).to_be_hidden(timeout=120000)
                assert target.stat().st_size == 4579008
                assert hashlib.sha256(target.read_bytes()).hexdigest() == SHA256
                assert Path(json.loads(config.read_text())['path']).samefile(target)
                assert not list(target.parent.glob('*.partial'))
                expect(page.get_by_text('Fonte do download das peças:', exact=False)).to_be_visible()
                output = ROOT / 'local/mii-composer-check'
                output.mkdir(exist_ok=True)
                page.screenshot(path=str(output / 'download-ready.png'), full_page=True)
                page.close()
            # Cache remains usable after restart with no HTTPS access, even if its config is absent.
            modified = target.stat().st_mtime_ns
            config.unlink()
            with launcher(executable, local, offline=True) as url:
                page = browser.new_page(locale='pt-BR')
                page.goto(url, wait_until='networkidle')
                page.locator('[data-tab="miis"]').click()
                deadline = time.monotonic() + 30
                while not config.exists():
                    assert time.monotonic() < deadline, 'Cached library was not configured'
                    page.wait_for_timeout(100)
                expect(page.locator('#mii-library')).to_be_hidden(timeout=30000)
                assert config.exists() and target.stat().st_mtime_ns == modified
                page.reload(wait_until='networkidle')
                page.locator('[data-tab="miis"]').click()
                expect(page.locator('#mii-library')).to_be_hidden(timeout=30000)
                token = page.locator('meta[name="mk8-token"]').get_attribute('content')
                result = page.request.post(url+'/api/mii/composer/download', data={}, headers={'X-MK8-Token':token})
                assert result.json()['data']['state'] == 'done'
                assert target.stat().st_mtime_ns == modified
                page.close()
        finally:
            browser.close()
    print('PASS: deferred automatic download, real network failure/retry, auth, archive/hash, cleanup, attribution, English, offline cache and reload.')


if __name__ == '__main__':
    run()
