"""Debug installation storyboard: sequential gates and simulation without API writes."""
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
class InstallationStoryboardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mk8-storyboard-')
        cls.local = Path(cls.temp.name)
        # Keep the release update check out of this offline simulation, including on reload.
        env = dict(os.environ, MK8_WEB_ROOT=str(ROOT/'apps/launcher/web'), MK8_UPDATE_REPO='')
        cls.process = subprocess.Popen([os.environ['MK8_LAUNCHER_EXE'], '--headless', '--data-dir', str(cls.local)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        deadline = time.monotonic() + 20
        while not (cls.local/'launcher.json').is_file():
            if cls.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Debug launcher did not start')
            time.sleep(.05)
        cls.url = json.loads((cls.local/'launcher.json').read_text())['url']
        cls.pw = sync_playwright().start()
        cls.browser = cls.pw.chromium.launch()

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.pw.stop()
        cls.process.terminate()
        cls.process.wait(timeout=10)
        cls.temp.cleanup()

    def setUp(self):
        self.page = self.browser.new_page(locale='pt-BR', viewport={'width': 1440, 'height': 1000})
        self.errors = []
        self.writes = []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.page.goto(self.url+'/?storyboard=1#install')
        expect(self.page.locator('#install-flow')).to_be_visible()
        self.page.wait_for_load_state('networkidle')
        # The existing launcher polls this read-only file-status endpoint using POST.
        self.page.on('request', lambda req: self.writes.append(req.url)
                     if req.method not in ('GET', 'HEAD') and not req.url.endswith('/api/dlc/files') else None)

    def tearDown(self):
        self.assertEqual(self.errors, [])
        self.assertEqual(self.writes, [], 'The storyboard must not install, save config or start a real build')
        self.page.close()

    def shot(self, name):
        folder = ROOT/'local/screenshots'
        folder.mkdir(parents=True, exist_ok=True)
        self.page.screenshot(path=str(folder/f'flow-{name}.png'), full_page=True, animations='disabled')

    def test_sequential_gates_tools_reuse_files_build_and_optional_content(self):
        page = self.page
        expect(page.locator('#flow-next')).to_be_disabled()
        for index in (1, 2, 3):
            expect(page.locator(f'#flow-step-{index}')).to_be_disabled()
            self.assertTrue(page.locator(f'#flow-panel-{index}').evaluate('e => e.inert'))
        self.shot('01-tools')
        page.locator('#flow-reuse').check()
        page.locator('#flow-tools-start').click()
        expect(page.locator('#flow-next')).to_be_disabled()
        expect(page.locator('#flow-next')).to_be_enabled(timeout=5000)
        expect(page.locator('#flow-panel-0')).to_contain_text('reaproveitadas')
        page.locator('#flow-next').click()
        expect(page.locator('#flow-heading-1')).to_be_focused()
        expect(page.locator('#flow-next')).to_be_disabled()
        page.locator('#flow-pick-base').click()
        expect(page.locator('#flow-pick-keys')).to_have_text('Remover')
        expect(page.locator('#flow-next')).to_be_disabled()
        page.locator('#flow-pick-update').click()
        expect(page.locator('#flow-next')).to_be_enabled()
        page.locator('#flow-pick-keys').click()
        expect(page.locator('#flow-next')).to_be_disabled()
        page.locator('#flow-pick-keys').click()
        self.shot('02-files')
        page.locator('#flow-next').click()
        expect(page.locator('#flow-next')).to_be_disabled()
        page.locator('#flow-build-start').click()
        expect(page.locator('#flow-step-3')).to_be_disabled()
        expect(page.locator('#flow-panel-2 progress')).not_to_have_attribute('value', '0')
        self.shot('03-building')
        page.locator('#flow-back').click()
        expect(page.locator('#flow-pick-base')).to_be_disabled()
        page.locator('#flow-next').click()
        page.locator('#flow-build-cancel').click()
        expect(page.locator('#flow-build-start')).to_have_text('Retomar build')
        expect(page.locator('#flow-next')).to_be_disabled()
        page.locator('#flow-build-start').click()
        page.locator('#flow-fail').click()
        expect(page.locator('#flow-panel-2 .failure')).to_contain_text('falha simulada')
        page.locator('#flow-build-start').click()
        expect(page.locator('#flow-next')).to_be_enabled(timeout=12000)
        page.locator('#flow-next').click()
        expect(page.locator('#flow-addon-enable')).to_have_count(0)
        self.shot('04-optional')
        page.locator('#flow-addon-ctgp').click()
        page.locator('#flow-ctgp-layout').select_option('dlc')
        expect(page.locator('#flow-addon-content')).to_contain_text('importe e ative também')
        page.locator('#flow-ctgp-layout').select_option('added')
        expect(page.locator('#flow-addon-import')).to_be_disabled()
        page.locator('#flow-addon-pick').click()
        self.shot('05-ctgp')
        page.locator('#flow-addon-import').click()
        expect(page.locator('#flow-next')).to_be_disabled()
        expect(page.locator('#flow-addon-enable')).to_be_checked(timeout=5000)
        page.locator('#flow-addon-enable').uncheck()
        expect(page.locator('#flow-addon-ctgp')).to_contain_text('Desativado')
        page.locator('#flow-next').click()
        expect(page.locator('#flow-panel-3')).to_contain_text('Prévia concluída')
        self.shot('06-complete')
        self.assertFalse((self.local/'candidate.json').exists())

    def test_recompilation_updates_failure_and_invalidation(self):
        page = self.page
        page.locator('#flow-scenario').select_option('update')
        expect(page.locator('#flow-build-start')).to_have_text('Atualizar a build')
        expect(page.locator('#flow-next')).to_be_disabled()
        page.locator('#flow-scenario').select_option('rebuild')
        expect(page.locator('#flow-build-start')).to_have_text('Recompilar jogo')
        expect(page.locator('#flow-next')).to_be_enabled()
        self.shot('07-rebuild')
        page.locator('#flow-back').click()
        page.locator('#flow-pick-base').click()
        expect(page.locator('#flow-step-2')).to_be_disabled()
        expect(page.locator('#flow-step-3')).to_be_disabled()
        page.locator('#flow-folder').click()
        expect(page.locator('#flow-next')).to_be_enabled()
        page.locator('#flow-scenario').select_option('new')
        page.locator('#flow-tools-start').click()
        page.locator('#flow-fail').click()
        expect(page.locator('#flow-next')).to_be_disabled()
        expect(page.locator('#flow-tools-start')).to_have_text('Tentar novamente')
        page.locator('#flow-tools-start').click()
        expect(page.locator('#flow-next')).to_be_enabled(timeout=7000)
        page.locator('#flow-next').click()
        for key in ('base', 'update', 'dlc'):
            page.locator(f'#flow-pick-{key}').click()
        page.locator('#flow-next').click()
        page.locator('#flow-build-start').click()
        expect(page.locator('#flow-next')).to_be_enabled(timeout=12000)
        page.locator('#flow-next').click()
        expect(page.locator('#flow-addon-enable')).to_be_checked()
        expect(page.locator('#flow-addon-dlc')).to_contain_text('Ativado')

    def test_mobile_keyboard_reduced_motion_and_reload_reset(self):
        page = self.page
        page.set_viewport_size({'width': 390, 'height': 844})
        page.emulate_media(reduced_motion='reduce')
        page.locator('#flow-scenario').select_option('rebuild')
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        page.locator('#flow-step-2').focus()
        page.keyboard.press('ArrowRight')
        expect(page.locator('#flow-step-3')).to_have_attribute('aria-current', 'step')
        expect(page.locator('#flow-heading-3')).to_be_focused()
        self.assertEqual(page.locator('.flow-track').evaluate('e => e.getAnimations().length'), 0)
        page.locator('#flow-addon-ctgp').click()
        self.shot('08-mobile')
        page.locator('#flow-back').click()
        expect(page.locator('#flow-heading-2')).to_be_focused()
        page.reload()
        expect(page.locator('#flow-step-0')).to_have_attribute('aria-current', 'step')
        expect(page.locator('#flow-next')).to_be_disabled()
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        page.locator('#flow-reuse').check()


if __name__ == '__main__':
    unittest.main()
