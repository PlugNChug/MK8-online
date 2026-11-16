"""Embedded installation UI: real configuration, controlled long-running jobs."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from playwright.sync_api import expect, sync_playwright

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.environ.get('MK8_LAUNCHER_EXE'), 'Requires a built launcher')
class InstallationWizardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mk8-install-wizard-')
        self.addCleanup(self.temp.cleanup)
        self.local = Path(self.temp.name)
        env = os.environ.copy()
        env.pop('MK8_WEB_ROOT', None)
        self.process = subprocess.Popen([env['MK8_LAUNCHER_EXE'], '--headless', '--data-dir', str(self.local)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
        self.addCleanup(self.stop_server)
        deadline = time.monotonic() + 25
        while not (self.local/'launcher.json').exists():
            self.assertIsNone(self.process.poll())
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.05)
        self.url = json.loads((self.local/'launcher.json').read_text())['url']
        self.pw = sync_playwright().start()
        self.addCleanup(self.pw.stop)
        self.browser = self.pw.chromium.launch()
        self.addCleanup(self.browser.close)
        self.page = self.browser.new_page(locale='pt-BR', viewport={'width':1440,'height':1000})
        self.errors, self.actions = [], []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.tools = False
        self.setup = None
        self.build = None
        self.done = False
        self.page.route('**/api/status', self.status)
        self.page.route('**/api/setup', self.job)
        self.page.route('**/api/setup/cancel', self.job)
        self.page.route('**/api/prepare', self.job)
        self.page.route('**/api/build/cancel', self.job)
        self.page.route('**/api/ctgp/import', self.job)
        self.page.route('**/api/dlc/import', self.job)
        self.page.goto(self.url+'/#install')
        expect(self.page.locator('#flow-step-0')).to_have_attribute('aria-current','step')

    def stop_server(self):
        self.process.terminate()
        self.process.wait(timeout=10)

    def tearDown(self):
        self.assertEqual(self.errors, [])

    def status(self, route):
        response = route.fetch()
        body = response.json()
        data = body['data']
        data['tools_ready'] = self.tools
        if self.setup:
            data['setup'].update(self.setup)
        if self.build:
            data['build'].update(self.build)
        if self.done:
            data['candidate'] = 'test-build'
            data['candidate_stale'] = False
            data['dlc'].update(state='ready', installed=True, enabled=False)
            data['ctgp'].update(state='ready', installed=True, enabled=False, courses=64, layout='added')
        route.fulfill(response=response, json=body)

    def job(self, route):
        path = route.request.url.split('/api/')[1]
        self.actions.append((path, route.request.post_data_json))
        if path == 'setup':
            self.setup = dict(state='running', steps=[], completed=0, total=3)
        elif path == 'setup/cancel':
            self.setup = dict(state='failed', detail='Instalação cancelada no teste.')
        elif path == 'prepare':
            self.build = dict(state='running', percent=17, phase='Compilando runtime', started_ms=int(time.time()*1000))
        elif path == 'build/cancel':
            self.build = dict(state='cancelled', detail='Build cancelada no teste.')
        route.fulfill(json={'ok':True,'data':{'message':'Operação recebida.'}})

    def refresh(self):
        self.page.evaluate('() => refresh()')

    def files(self):
        for field, filename in [('base_package','base.nsp'), ('update_package','update.nsp'), ('prod_keys','prod.keys')]:
            file = self.local/filename
            file.write_bytes(b'UI test only, not game data')
            self.page.locator('#'+field).fill(str(file))
            self.page.locator('#'+field).blur()
            self.page.wait_for_function('() => !busy')
        self.refresh()

    def test_embedded_gates_real_config_build_cancel_retry_and_optional_content(self):
        page = self.page
        expect(page.locator('#design-toggle')).to_have_count(0)
        expect(page.locator('#flow-scenario')).to_have_count(0)
        self.assertNotEqual(page.request.get(self.url+'/preview.js').status,200)
        expect(page.locator('#flow-step-1')).to_be_disabled()
        expect(page.locator('#flow-step-2')).to_be_disabled()
        expect(page.locator('#flow-step-3')).to_be_disabled()
        page.locator('#setup-start').click()
        expect(page.locator('#setup-cancel')).to_be_visible()
        expect(page.locator('#flow-next')).to_be_disabled()
        page.locator('#setup-cancel').click()
        expect(page.locator('#setup-detail')).to_contain_text('cancelada')
        page.locator('#setup-start').click()
        self.tools = True
        self.setup = dict(state='done', steps=[])
        self.refresh()
        page.locator('#flow-next').click()
        expect(page.locator('#base_package')).to_be_visible()
        expect(page.locator('#flow-step-2')).to_be_disabled()
        self.files()
        config = json.loads((self.local/'config.json').read_text())
        self.assertEqual(config['base_package'],str(self.local/'base.nsp'))
        page.locator('#prepare-dlc-package').fill(str(self.local/'optional.nsp'))
        page.locator('#flow-next').click()
        page.locator('#build').click()
        expect(page.locator('#build-pct')).to_have_text('17%')
        self.assertEqual([body for name,body in self.actions if name=='prepare'][-1], {'dlc_package':str(self.local/'optional.nsp')})
        expect(page.locator('#flow-step-3')).to_be_disabled()
        page.once('dialog', lambda dialog:dialog.accept())
        page.locator('#cancel').click()
        expect(page.locator('#build-detail')).to_contain_text('cancelada')
        page.locator('#build').click()
        page.wait_for_function('() => !busy')
        self.build = dict(state='failed',detail='Falha controlada',percent=18)
        self.refresh()
        expect(page.locator('#build-detail')).to_have_text('Falha controlada')
        page.locator('#build').click()
        page.wait_for_function('() => !busy')
        self.build = dict(state='candidate',percent=100)
        self.done = True
        self.refresh()
        page.locator('#flow-next').click()
        expect(page.locator('#dlc-card')).to_be_visible()
        page.locator('[data-install-addon="ctgp"]').click()
        expect(page.locator('#ctgp-card')).to_be_visible()
        page.locator('#ctgp-source').fill(str(self.local/'ctgp'))
        page.locator('#ctgp-layout').select_option('added')
        page.once('dialog', lambda dialog:dialog.accept())
        page.locator('#ctgp-import').click()
        page.wait_for_function('() => !busy')
        self.assertTrue(any(name=='ctgp/import' and body['layout']=='added' for name,body in self.actions))
        page.locator('[data-tab="settings"]').click()
        page.locator('#config-tab-content').click()
        page.locator('[data-config-content="ctgp"]').click()
        expect(page.locator('#ctgp-card')).to_be_visible()
        expect(page.locator('#ctgp-source')).to_have_value(str(self.local/'ctgp'))
        page.locator('[data-tab="install"]').click()
        expect(page.locator('#ctgp-card')).to_be_visible()
        expect(page.locator('#ctgp-card')).to_have_count(1)
        folder = ROOT/'local/screenshots'
        folder.mkdir(parents=True,exist_ok=True)
        page.screenshot(path=str(folder/'production-install-addons.png'),full_page=True,animations='disabled')
        page.set_viewport_size({'width':390,'height':844})
        page.locator('#flow-back').click()
        expect(page.locator('#build')).to_be_visible()
        self.assertLessEqual(page.evaluate('document.documentElement.scrollWidth'),391)
        page.screenshot(path=str(folder/'production-install-mobile.png'),full_page=True,animations='disabled')

    def test_existing_tools_are_reused_and_reloading_preserves_file_paths(self):
        self.tools = True
        self.refresh()
        expect(self.page.locator('#setup-start')).to_be_hidden()
        self.page.locator('#flow-step-1').click()
        self.files()
        self.page.reload()
        expect(self.page.locator('#flow-step-2')).to_have_attribute('aria-current','step')
        self.page.locator('#flow-back').click()
        expect(self.page.locator('#base_package')).to_have_value(str(self.local/'base.nsp'))
        self.assertFalse(self.actions)

    def test_modules_translate_without_changing_user_text_or_option_values(self):
        page = self.page
        page.locator('#language').select_option('en')
        expect(page.locator('#flow-heading-0')).to_have_text('Tools')
        expect(page.locator('#flow-guidance')).to_have_text('Install the tools to unlock game file selection.')
        page.locator('[data-tab="support"]').click()
        expect(page.locator('#support-bug-form h2')).to_have_text('Tell us what happened')
        page.locator('#support-bug-title').fill('Nome particular')
        page.locator('#support-bug-description').fill('Texto do usuário: Instalar e Perfil Mii')
        page.locator('#support-bug-area').select_option('Perfil Mii')
        page.locator('#language').select_option('es')
        expect(page.locator('#support-bug-form h2')).to_have_text('Cuéntanos qué ocurrió')
        expect(page.locator('#support-bug-description')).to_have_value('Texto do usuário: Instalar e Perfil Mii')
        expect(page.locator('#support-bug-area')).to_have_value('Perfil Mii')
        self.assertIn('## Contexto',page.locator('#support-issue-preview').input_value())
        page.locator('[data-tab="settings"]').click()
        expect(page.locator('#config-tab-video')).to_contain_text('Vídeo y controles')
        page.locator('#config-field-theme').select_option('NES')
        page.locator('#language').select_option('pt-BR')
        expect(page.locator('#config-field-theme')).to_have_value('NES')
        expect(page.locator('#config-tab-video')).to_contain_text('Alterações não salvas')
        page.locator('[data-tab="support"]').click()
        expect(page.locator('#support-bug-form h2')).to_have_text('Conte o que aconteceu')
        expect(page.locator('#support-bug-title')).to_have_value('Nome particular')


if __name__ == '__main__':
    unittest.main()
