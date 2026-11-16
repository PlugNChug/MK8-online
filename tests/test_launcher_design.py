"""Opt-in debug UI check: MK8_TEST_DESIGN=1 and MK8_LAUNCHER_EXE=<debug Launcher>."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from playwright.sync_api import expect, sync_playwright

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.environ.get('MK8_TEST_DESIGN') == '1', 'Requires the debug launcher')
class DesignTests(unittest.TestCase):
    def test_live_design_controls_navigation_and_responsive_layout(self):
        with tempfile.TemporaryDirectory(prefix='mk8-design-test-') as temporary:
            local = Path(temporary)
            env = dict(os.environ, MK8_WEB_ROOT=str(ROOT/'apps/launcher/web'))
            process = subprocess.Popen([os.environ['MK8_LAUNCHER_EXE'], '--headless', '--data-dir', str(local)],
                                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            try:
                deadline = time.monotonic() + 20
                while not (local/'launcher.json').is_file():
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.05)
                url = json.loads((local/'launcher.json').read_text())['url']
                with sync_playwright() as pw:
                    browser = pw.chromium.launch()
                    page = browser.new_page(locale='pt-BR', viewport={'width': 1440, 'height': 1000})
                    errors = []
                    page.on('pageerror', lambda error: errors.append(str(error)))
                    page.route('**/api/mii/composer/download', lambda route: route.fulfill(json={
                        'ok': True, 'data': {'state': 'failed', 'detail': 'Teste offline sem biblioteca.'}}))
                    page.goto(url)
                    expect(page.locator('#design-toggle')).to_be_visible()
                    # The debug override does not bypass the real API's CSRF protection.
                    self.assertEqual(page.request.post(url+'/api/username', data='{}').status, 403)
                    self.assertEqual(page.request.get(url+'/style.css', headers={'Host': 'invalid'}).status, 403)
                    self.assertEqual(page.request.get(url+'/config.json').status, 403)
                    shot = ROOT/'local/screenshots'
                    shot.mkdir(parents=True, exist_ok=True)
                    for width, height in [(1440, 1000), (390, 844)]:
                        page.set_viewport_size({'width': width, 'height': height})
                        for name in ['install', 'play', 'miis', 'session', 'settings', 'support']:
                            page.locator(f'[data-tab="{name}"]').click()
                            expect(page.locator('#'+name)).to_be_visible()
                            if name == 'settings':
                                expect(page.locator('#font-mode')).to_be_enabled()
                                expect(page.locator('[data-key="volume"]')).to_be_enabled()
                            self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'), name)
                            page.screenshot(path=str(shot/f'design-{name}-{width}.png'), full_page=True, animations='disabled')
                    page.set_viewport_size({'width': 1440, 'height': 1000})
                    page.locator('[data-tab="play"]').click()
                    page.locator('.play-shortcuts [data-goto="settings"]').click()
                    expect(page.locator('#settings')).to_be_visible()
                    expect(page.locator('#ctgp-title')).to_be_in_viewport()
                    page.locator('#config-tab-content').click()
                    page.locator('#config-content-ctgp').click()
                    expect(page.locator('#ctgp-title')).to_be_in_viewport()
                    page.locator('[data-tab="play"]').click()
                    page.locator('#design-toggle').click()
                    expect(page.locator('#design-palette')).to_be_focused()
                    page.locator('#design-palette').select_option('slate')
                    page.locator('#design-accent').fill('#ffc000')
                    page.locator('#design-size').fill('16')
                    self.assertEqual(page.locator('html').evaluate('e => getComputedStyle(e).fontSize'), '16px')
                    page.screenshot(path=str(shot/'design-controls.png'), full_page=True)
                    with page.expect_download() as download:
                        page.locator('#design-export').click()
                    css = Path(download.value.path()).read_text()
                    self.assertIn('--accent: #ffc000', css)
                    self.assertIn('font-size: 16px', css)
                    page.keyboard.press('Escape')
                    expect(page.locator('#design-panel')).not_to_be_visible()
                    expect(page.locator('#design-toggle')).to_be_focused()
                    page.reload()
                    self.assertEqual(page.locator('html').evaluate('e => getComputedStyle(e).fontSize'), '16px')
                    page.locator('#design-toggle').click()
                    expect(page.locator('#design-accent')).to_have_value('#ffc000')
                    page.locator('#design-reset').click()
                    expect(page.locator('#design-accent')).to_have_value('#ff794f')
                    page.set_viewport_size({'width': 390, 'height': 844})
                    self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
                    expect(page.locator('#design-close')).to_be_in_viewport()
                    page.locator('#design-close').click()
                    self.assertEqual(errors, [])
                    browser.close()
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)


if __name__ == '__main__':
    unittest.main()
