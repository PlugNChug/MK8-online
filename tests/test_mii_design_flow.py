"""Guided debug Mii creation against real temporary profiles, without commercial assets."""
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
class MiiDesignFlowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mk8-mii-flow-')
        self.local = Path(self.temp.name)
        (self.local/'user-no-mii/save').mkdir(parents=True)
        (self.local/'user-no-mii/save/marker').write_bytes(b'preserve')
        self.process = subprocess.Popen(
            [os.environ['MK8_LAUNCHER_EXE'], '--headless', '--data-dir', str(self.local)],
            env={**os.environ, **({} if os.environ.get('MK8_TEST_EMBEDDED') == '1' else {'MK8_WEB_ROOT': str(ROOT/'apps/launcher/web')})},
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
        deadline = time.monotonic() + 25
        while not (self.local/'launcher.json').is_file():
            self.assertIsNone(self.process.poll())
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.05)
        self.url = json.loads((self.local/'launcher.json').read_text())['url']
        self.pw = sync_playwright().start()
        self.browser = self.pw.chromium.launch()
        self.page = self.browser.new_page(locale='pt-BR', viewport={'width':1440,'height':1000})
        self.errors = []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.page.route('**/api/mii/composer/download', lambda route: route.fulfill(json={
            'ok':True, 'data':{'state':'failed','detail':'Biblioteca indisponível neste teste offline.'}}))
        self.page.goto(self.url+'/#miis')
        expect(self.page.locator('#mii-continue')).to_be_enabled(timeout=30000)
        self.token = self.page.locator('meta[name="mk8-token"]').get_attribute('content')

    def tearDown(self):
        try:
            self.assertEqual(self.errors, [])
            self.assertEqual((self.local/'user-no-mii/save/marker').read_bytes(), b'preserve')
        finally:
            self.browser.close()
            self.pw.stop()
            self.process.terminate()
            self.process.wait(timeout=10)
            self.temp.cleanup()

    def export(self):
        return self.page.request.get(self.url+'/api/mii/export', headers={'X-MK8-Token':self.token}).json()['data']['data']

    def test_play_modes_wait_for_customization_and_resume_selected_nextendo(self):
        page = self.page
        for route, body in [('play', {}), ('play', {'online':True}), ('play', {'nextendo':True}),
                            ('steam/host', {}), ('steam/join', {'lobby':'123'})]:
            response = page.request.post(self.url+'/api/'+route, headers={'X-MK8-Token':self.token},
                                         data=json.dumps(body)).json()
            self.assertFalse(response['ok'])
            self.assertIn('Mii', response['error'])
        status = page.evaluate("api('status')")
        status.update(candidate=True, nextendo_supported=True, nextendo_trusted=False)
        page.route('**/api/status', lambda route: route.fulfill(json={'ok':True, 'data':status}))
        calls = []
        def play(route):
            calls.append(route.request.post_data_json)
            route.fulfill(json={'ok':True,'data':{}})
        page.route('**/api/play', play)
        page.locator('[data-tab="play"]').click()
        page.locator('#play-solo').click()
        expect(page.locator('#mii-play-prompt')).to_be_visible()
        page.locator('#mii-play-cancel').click()
        expect(page.locator('#play')).to_be_visible()
        page.locator('[data-tab="session"]').click()
        page.locator('#steam-host').click()
        expect(page.locator('#mii-play-prompt')).to_be_visible()
        page.locator('#mii-play-cancel').click()
        expect(page.locator('#session')).to_be_visible()
        page.locator('[data-tab="play"]').click()
        page.locator('#play-nextendo').click()
        expect(page.locator('#nextendo-consent')).to_be_hidden()
        page.locator('#mii-name').fill('Piloto')
        page.locator('#mii-continue').click()
        expect(page.locator('#mii-workshop')).to_be_visible()
        expect(page.locator('#mii-save')).to_have_text('Salvar e continuar')
        self.assertFalse(calls)
        page.locator('#mii-save').click()
        expect(page.locator('#nextendo-consent')).to_be_visible()
        page.locator('#nextendo-accept').click()
        page.wait_for_function('() => !busy')
        self.assertEqual(calls, [{'online':False, 'nextendo':True}])
        page.locator('#play-solo').click()
        page.wait_for_function('() => !busy')
        self.assertEqual(calls[-1], {'online':False})

    def test_direct_play_editor_can_be_cancelled_without_starting_a_game(self):
        page = self.page
        calls = []
        page.on('request', lambda req: calls.append(req.url) if req.url.endswith('/api/play') else None)
        page.goto(self.url+'/#create-mii-to-play')
        page.reload()
        expect(page.locator('#mii-play-prompt')).to_be_visible()
        page.locator('#mii-play-cancel').click()
        expect(page.locator('#play')).to_be_visible()
        page.reload()
        expect(page.locator('#play')).to_be_visible()
        expect(page.locator('#mii-play-prompt')).to_be_hidden()
        self.assertFalse(calls)

    def shot(self, name):
        folder = ROOT/'local/screenshots'
        folder.mkdir(exist_ok=True, parents=True)
        self.page.evaluate('scrollTo(0, 0)')
        self.page.screenshot(path=str(folder/f'mii-flow-{name}.png'), full_page=True, animations='disabled')

    def test_name_gate_face_body_save_rename_and_existing_profiles(self):
        page = self.page
        expect(page.locator('#mii-workshop')).to_be_hidden()
        expect(page.locator('#mii-step-customize')).to_be_disabled()
        page.locator('#mii-continue').click()
        self.assertFalse((self.local/'mii-profile.json').exists())
        page.locator('#mii-name').fill('   ')
        page.locator('#mii-continue').click()
        self.assertFalse((self.local/'mii-profile.json').exists())
        page.locator('#mii-name').fill('João')
        self.shot('01-name')
        page.locator('#mii-name').press('Enter')
        expect(page.locator('#mii-workshop')).to_be_visible()
        expect(page.locator('#mii-area-face')).to_have_attribute('aria-pressed','true')
        expect(page.locator('#mii-preview-unavailable')).to_be_visible()
        expect(page.locator('#mii-library')).to_be_visible()
        first = json.loads((self.local/'mii-profile.json').read_text(encoding='utf-8'))
        page.locator('[data-offset="45"][data-value="3"]').click()
        page.locator('#mii-tabs [data-part="eyes"]').click()
        expect(page.locator('#mii-category-title')).to_have_text('Olhos')
        page.locator('[data-offset="52"][data-value="4"]').click()
        page.locator('#mii-tabs [data-part="eyes"]').focus()
        page.keyboard.press('ArrowRight')
        expect(page.locator('#mii-tabs [data-part="brows"]')).to_be_focused()
        expect(page.locator('#mii-category-title')).to_have_text('Sobrancelhas')
        self.shot('02-face')
        page.locator('#mii-next-area').click()
        expect(page.locator('#mii-tabs')).to_be_hidden()
        expect(page.locator('#mii-category-title')).to_have_text('Corpo e cor favorita')
        page.locator('[data-offset="40"][data-value="1"]').click()
        page.locator('input[data-offset="41"]').fill('120')
        page.locator('[data-offset="39"][data-value="5"]').click()
        self.shot('03-body')
        page.locator('#mii-edit-name').click()
        expect(page.locator('#mii-name')).to_have_value('João')
        page.locator('#mii-name').fill('Ana')
        page.locator('#mii-continue').click()
        expect(page.locator('#mii-workshop-title')).to_have_text('Personalize Ana')
        page.locator('#mii-save').click()
        expect(page.locator('#mii-status')).to_contain_text('Mii salvo')
        data = self.export()
        self.assertEqual((data[45], data[52], data[40], data[41], data[39]), (3, 4, 1, 120, 5))
        stored = json.loads((self.local/'mii-profile.json').read_text(encoding='utf-8'))
        self.assertEqual(stored['id'], first['id'])
        self.assertEqual(stored['appearance']['name'], 'Ana')
        page.locator('#mii-new').click()
        expect(page.locator('#mii-workshop')).to_be_hidden()
        expect(page.locator('#mii-step-customize')).to_be_disabled()
        page.locator('#mii-name').fill('Luigi')
        page.locator('#mii-continue').click()
        expect(page.locator('#mii-saved button')).to_have_count(2)
        page.locator('#mii-saved button', has_text='Ana').click()
        expect(page.locator('#mii-workshop-title')).to_have_text('Personalize Ana')
        page.locator('#mii-area-face').click()
        page.locator('#mii-tabs [data-part="nose"]').click()
        page.locator('[data-offset="66"][data-value="5"]').click()
        page.once('dialog', lambda dialog: dialog.dismiss())
        page.locator('#mii-new').click()
        expect(page.locator('#mii-workshop')).to_be_visible()
        page.once('dialog', lambda dialog: dialog.dismiss())
        page.locator('#mii-saved button', has_text='Luigi').click()
        expect(page.locator('#mii-workshop-title')).to_have_text('Personalize Ana')
        page.reload()
        expect(page.locator('#mii-workshop')).to_be_visible()
        expect(page.locator('#mii-workshop-title')).to_have_text('Personalize Ana')
        self.assertEqual(self.export()[45], 3)
        page.set_viewport_size({'width':390,'height':844})
        page.locator('#mii-area-face').click()
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.shot('04-mobile-editor')
        page.locator('#mii-edit-name').click()
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.shot('05-mobile-name')

    def test_failed_creation_stays_on_name_and_can_retry(self):
        page = self.page
        page.route('**/api/mii/create', lambda route: route.fulfill(json={'ok':False,'error':'Falha de teste ao criar.'}))
        page.locator('#mii-name').fill('Teste')
        page.locator('#mii-continue').click()
        expect(page.locator('#mii-status')).to_contain_text('Falha de teste')
        expect(page.locator('#mii-identity-form')).to_be_visible()
        expect(page.locator('#mii-step-customize')).to_be_disabled()
        expect(page.locator('#mii-continue')).to_be_enabled()
        self.assertFalse((self.local/'mii-profile.json').exists())
        page.unroute('**/api/mii/create')
        page.locator('#mii-continue').click()
        expect(page.locator('#mii-workshop-title')).to_have_text('Personalize Teste')

    def test_first_mii_can_be_created_and_saved_without_webgl(self):
        page = self.page
        page.locator('#language').select_option('es')
        expect(page.locator('html')).to_have_attribute('lang', 'es')
        page.add_init_script("""
            const getContext = HTMLCanvasElement.prototype.getContext;
            HTMLCanvasElement.prototype.getContext = function(kind, ...options) {
                if (kind.includes('webgl')) return null;
                return getContext.call(this, kind, ...options);
            };
        """)
        page.reload()
        expect(page.locator('#mii-identity-form h2')).to_have_text('¿Cómo se llamará tu Mii?')
        expect(page.locator('#mii-continue')).to_be_enabled()
        page.locator('#mii-name').fill('Piloto')
        page.locator('#mii-continue').click()
        expect(page.locator('#mii-preview-unavailable')).to_be_visible()
        choice = page.locator('[data-offset="45"][data-value="3"]')
        expect(choice).to_have_text('4')
        choice.click()
        page.locator('#mii-save').click()
        expect(page.locator('#mii-status')).to_contain_text('Mii guardado')
        self.assertEqual(self.export()[45], 3)
        profile = json.loads((self.local/'mii-profile.json').read_text(encoding='utf-8'))
        self.assertEqual(profile['appearance']['name'], 'Piloto')


if __name__ == '__main__':
    unittest.main()
