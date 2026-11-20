"""Design Debug settings modules keep real isolated settings and contextual previews."""
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
class SettingsModulesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pw = sync_playwright().start()
        cls.browser = cls.pw.chromium.launch()

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.pw.stop()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mk8-settings-design-')
        self.local = Path(self.temp.name)
        user = self.local/'user-no-mii'
        (user/'save').mkdir(parents=True)
        (user/'mk8d.ini').write_text('future_setting = preserve\ntheme = Default\n', encoding='utf-8')
        (user/'save/marker').write_bytes(b'keep-save')
        env = {**os.environ, **({} if os.environ.get('MK8_TEST_EMBEDDED') == '1' else {'MK8_WEB_ROOT': str(ROOT/'apps/launcher/web')})}
        self.process = subprocess.Popen([os.environ['MK8_LAUNCHER_EXE'], '--headless', '--data-dir', str(self.local)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        deadline = time.monotonic()+20
        while not (self.local/'launcher.json').is_file():
            self.assertIsNone(self.process.poll())
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.05)
        self.url = json.loads((self.local/'launcher.json').read_text())['url']
        self.page = self.browser.new_page(locale='pt-BR', viewport={'width':1440, 'height':1000})
        self.errors = []
        self.page.on('pageerror', lambda error:self.errors.append(str(error)))
        self.page.on('console', lambda msg:self.errors.append(msg.text) if 'Content Security Policy' in msg.text else None)
        self.page.goto(self.url+'/#settings')
        expect(self.page.locator('#config-field-theme')).to_be_visible()
        expect(self.page.locator('#config-field-theme')).to_be_enabled()

    def tearDown(self):
        try:
            self.assertEqual(self.errors, [])
            self.assertEqual((self.local/'user-no-mii/save/marker').read_bytes(), b'keep-save')
        finally:
            self.page.close()
            self.process.terminate()
            self.process.wait(timeout=10)
            self.temp.cleanup()

    def shot(self, name):
        folder = ROOT/'local/screenshots'
        folder.mkdir(parents=True, exist_ok=True)
        self.page.screenshot(path=str(folder/f'config-{name}.png'), full_page=True, animations='disabled')

    def test_native_settings_opt_in_preserves_other_options_and_game_data(self):
        path = self.local/'debug-options.json'
        previous = {'graphics_capture':True, 'audio_recording':True, 'native_settings':False}
        path.write_text(json.dumps(previous), encoding='utf-8')
        ini = self.local/'user-no-mii/mk8d.ini'
        original = ini.read_bytes()
        page = self.page
        page.reload()
        switch = page.locator('#debug-settings')
        expect(switch).not_to_be_checked()
        switch.check()
        expect(page.locator('#debug-status')).to_have_text('Preferência salva. A alteração vale ao iniciar o jogo.')
        expect(switch).to_be_enabled()
        self.assertEqual(json.loads(path.read_text()), {**previous, 'native_settings':True})
        self.assertEqual(ini.read_bytes(), original)
        page.reload()
        expect(switch).to_be_checked()
        token = page.locator('meta[name="mk8-token"]').get_attribute('content')
        invalid = page.request.post(self.url+'/api/debug-options', headers={'X-MK8-Token':token},
            data=json.dumps({**previous, 'native_settings':'yes'})).json()
        self.assertFalse(invalid['ok'])
        self.assertTrue(json.loads(path.read_text())['native_settings'])
        for language, label in [('en','Enable native settings menu'), ('es','Activar el menú nativo de ajustes')]:
            page.locator('#language').select_option(language)
            expect(page.locator('label').filter(has=switch)).to_contain_text(label)
        self.shot('native-settings')
        page.set_viewport_size({'width':390,'height':844})
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.shot('native-settings-mobile')

    def test_native_settings_failed_save_restores_saved_value_and_retries(self):
        page = self.page
        switch = page.locator('#debug-settings')
        expect(switch).not_to_be_checked()
        def reject(route):
            if route.request.method == 'POST':
                route.fulfill(status=500,json={'ok':False,'error':'Fixture write failure'})
            else:
                route.continue_()
        page.route('**/api/debug-options',reject)
        switch.check()
        expect(page.locator('#debug-status')).to_have_text('Não foi possível salvar a preferência.')
        expect(switch).to_be_enabled()
        expect(switch).not_to_be_checked()
        self.assertFalse((self.local/'debug-options.json').exists())
        page.unroute('**/api/debug-options',reject)
        switch.check()
        expect(page.locator('#debug-status')).to_have_text('Preferência salva. A alteração vale ao iniciar o jogo.')
        expect(switch).to_be_enabled()
        self.assertTrue(json.loads((self.local/'debug-options.json').read_text())['native_settings'])

    def test_optional_graphics_component_requires_reading_and_acceptance(self):
        page = self.page
        caps = {'gpu':'Radeon test GPU', 'rdna4':False, 'rtx':False, 'amd':False,
                'nvidia':False, 'versions':[]}
        installs = []
        def install(route):
            installs.append(route.request.post_data_json)
            caps.update(amd=True, versions=['3.1.6'])
            route.fulfill(json={'ok':True, 'data':{'running':True}})
        page.route('**/api/graphics', lambda route: route.fulfill(json={'ok':True, 'data':{'caps':caps, 'running':False}}))
        page.route('**/api/graphics/license', lambda route: route.fulfill(json={'ok':True, 'data':{'text':'Test component license'}}))
        page.route('**/api/graphics/install', install)
        page.locator('#config-tab-content').click()
        page.locator('button[data-tab="play"]').click()
        page.locator('#play-graphics').click()
        expect(page.locator('#config-field-upscaler')).to_be_focused()
        expect(page.locator('#graphics-gpu')).to_contain_text(caps['gpu'])
        scaler = page.locator('#config-field-upscaler')
        expect(scaler.locator('option[value="dlss"]')).to_have_js_property('disabled', True)
        expect(scaler.locator('option[value="fsr4"]')).to_have_js_property('disabled', True)
        scaler.select_option('fsr3')
        expect(page.locator('#settings-save')).to_be_disabled()
        expect(page.locator('#graphics-install')).to_be_disabled()
        page.locator('#graphics-license').click()
        expect(page.locator('#graphics-license-text')).to_have_text('Test component license')
        self.assertEqual(installs, [])
        page.locator('#graphics-consent').check()
        page.locator('#graphics-install').click()
        expect(page.locator('#settings-save')).to_be_enabled()
        self.assertEqual(installs, [{'vendor':'amd', 'accept_terms':True}])

    def test_upgrade_from_earlier_runtime_with_same_launcher_version(self):
        page = self.page
        status = page.evaluate("api('status')")
        version = status['version']
        (self.local/'built-with.json').write_text(json.dumps({'version':version}))
        host = Path(status['root'])/'prebuilt/host/manifest.json'
        candidate = {'schema':2, 'identity':'test', 'profile':'mk8d-smm2-experimental',
                     'executable':{'path':'old.exe', 'sha256':'0'*64, 'bytes':1},
                     'files':{}, 'data_root':'.', 'shader_root':'.', 'gameplay_verified':False,
                     'dlc_supported':True, 'nextendo_supported':True}
        path = self.local/'candidate.json'
        path.write_text(json.dumps(candidate))
        self.assertTrue(page.evaluate("api('status')")['candidate_stale'])
        if host.is_file():
            candidate['executable']['sha256'] = json.loads(host.read_text())['files']['mk8drecomp.exe']
            path.write_text(json.dumps(candidate))
            self.assertFalse(page.evaluate("api('status')")['candidate_stale'])
        candidate['executable']['sha256'] = '1'*64
        candidate['temporal_upscaling_supported'] = True
        path.write_text(json.dumps(candidate))
        self.assertTrue(page.evaluate("api('status')")['candidate_stale'])
        candidate['romfs_long_paths_supported'] = True
        path.write_text(json.dumps(candidate))
        self.assertTrue(page.evaluate("api('status')")['candidate_stale'])
        candidate['nextendo_session_supported'] = True
        path.write_text(json.dumps(candidate))
        self.assertFalse(page.evaluate("api('status')")['candidate_stale'])

    def test_visual_settings_previews_drafts_and_real_save(self):
        page = self.page
        expect(page.locator('#config-tab-fonts')).to_be_enabled()
        expect(page.locator('#config-tab-content')).to_be_enabled()
        page.locator('#config-field-theme').select_option('NES')
        expect(page.locator('#config-menu-example')).to_have_class('config-menu-example nes')
        self.shot('01-menu-nes')
        page.locator('#config-field-aspect').select_option('original')
        expect(page.locator('#config-scene')).to_have_class('config-scene original-ratio')
        self.assertLess(page.locator('#config-scene').bounding_box()['width'], page.locator('.config-screen').bounding_box()['width']*.8)
        page.locator('#config-field-fullscreen').check()
        expect(page.locator('.config-windowbar')).to_be_hidden()
        page.locator('#config-field-show_fps').check()
        expect(page.locator('#config-fps-example')).to_be_visible()
        page.locator('#config-field-resolution').select_option('50%')
        expect(page.locator('#config-field-upscaler')).to_be_enabled()
        self.shot('02-image')
        page.locator('#config-tab-fonts').click()
        expect(page.locator('#font-mode')).to_be_visible()
        page.locator('#config-font-text').fill('<piloto> João 123')
        expect(page.locator('#config-font-sample')).to_have_text('<piloto> João 123')
        expect(page.locator('#config-font-sample piloto')).to_have_count(0)
        expect(page.locator('#config-tab-video')).to_contain_text('Alterações não salvas')
        self.shot('03-fonts')
        page.locator('#config-tab-video').click()
        expect(page.locator('#config-field-theme')).to_have_value('NES')
        page.locator('#settings-save').click()
        expect(page.locator('#settings-save')).to_be_disabled()
        page.reload()
        expect(page.locator('#config-field-theme')).to_have_value('NES')
        expect(page.locator('#config-field-fullscreen')).to_be_checked()
        ini = (self.local/'user-no-mii/mk8d.ini').read_text()
        self.assertIn('theme = NES', ini)
        self.assertIn('future_setting = preserve', ini)
        token = page.locator('meta[name="mk8-token"]').get_attribute('content')
        state = page.request.get(self.url+'/api/status',headers={'X-MK8-Token':token}).json()['data']
        state['game_pid'] = 123
        page.route('**/api/status',lambda route:route.fulfill(json={'ok':True,'data':state}))
        page.evaluate('refresh()')
        expect(page.locator('#config-field-fullscreen')).to_be_disabled()
        expect(page.locator('#config-tab-fonts')).to_be_enabled()

    def test_aspect_stretch_labels_preview_and_legacy_ini_values(self):
        page = self.page
        aspect = page.locator('#config-field-aspect')
        aspect.select_option('original')
        page.locator('#settings-save').click()
        expect(page.locator('#settings-save')).to_be_disabled()
        page.reload()
        expect(aspect).to_have_value('original')
        expect(page.locator('#config-scene')).to_have_class('config-scene original-ratio')
        self.assertIn('aspect = original', (self.local/'user-no-mii/mk8d.ini').read_text())
        # The same scene widens; the illustration must not suggest a wider field of view.
        original = page.locator('.config-track-art circle').first.bounding_box()
        self.assertAlmostEqual(original['width'] / original['height'], 1, delta=.02)
        for language, label, caption in [('pt-BR', 'Esticar', 'Imagem esticada'),
                                         ('en', 'Stretch', 'Stretched image'),
                                         ('es', 'Estirar', 'Imagen estirada')]:
            page.locator('#language').select_option(language)
            expect(aspect.locator('option[value="unlocked"]')).to_have_text(label)
            aspect.select_option('unlocked')
            expect(page.locator('#config-scene')).to_have_class('config-scene')
            expect(page.locator('#config-screen-caption')).to_contain_text(caption)
        stretched = page.locator('.config-track-art circle').first.bounding_box()
        self.assertAlmostEqual(stretched['width'] / stretched['height'], 21 / 16, delta=.02)
        self.assertAlmostEqual(stretched['width'] / original['width'], 21 / 16, delta=.02)
        page.locator('#settings-save').click()
        expect(page.locator('#settings-save')).to_be_disabled()
        page.reload()
        expect(aspect).to_have_value('unlocked')
        self.assertIn('aspect = unlocked', (self.local/'user-no-mii/mk8d.ini').read_text())
        self.shot('aspect-stretch')
        page.set_viewport_size({'width':390,'height':844})
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.shot('aspect-stretch-mobile')

    def test_ultrawide_round_trip_preview_and_frame_generation_fallback(self):
        page = self.page
        aspect = page.locator('#config-field-aspect')
        expect(aspect).to_have_value('unlocked')
        expect(aspect.locator('option[value="ultrawide"]')).to_have_count(1)
        expect(aspect).to_have_attribute('aria-describedby', 'config-field-aspect-hint')
        aspect.select_option('original')
        art = page.locator('.config-track-art')
        original = art.locator('circle').first.bounding_box()
        clipped = art.bounding_box()
        sides = art.locator('.config-world-extra')
        expect(sides).to_have_count(2)
        left, right = (sides.nth(i).bounding_box() for i in range(2))
        self.assertLess(left['x'] + left['width'], clipped['x'])
        self.assertGreater(right['x'], clipped['x'] + clipped['width'])
        aspect.select_option('ultrawide')
        wide = art.locator('circle').first.bounding_box()
        screen = page.locator('.config-screen').bounding_box()
        self.assertAlmostEqual(wide['width'] / wide['height'], 1, delta=.02)
        self.assertAlmostEqual(wide['width'], original['width'], delta=.2)
        self.assertAlmostEqual(wide['height'], original['height'], delta=.2)
        self.assertAlmostEqual(wide['x'], original['x'], delta=.2)
        self.assertAlmostEqual(art.bounding_box()['width'], screen['width'], delta=.2)
        for index in range(2):
            landmark = sides.nth(index).bounding_box()
            self.assertGreater(landmark['x'], screen['x'])
            self.assertLess(landmark['x'] + landmark['width'], screen['x'] + screen['width'])
        for language, caption in [('pt-BR', 'Campo de visão ampliado'),
                                  ('en', 'Wider field of view'),
                                  ('es', 'Campo de visión ampliado')]:
            page.locator('#language').select_option(language)
            aspect.focus()
            expect(aspect.locator('option[value="ultrawide"]')).to_have_text('Ultrawide (experimental)')
            expect(page.locator('#config-screen-caption')).to_contain_text(caption)
            expect(page.locator('#config-preview-description')).to_contain_text('16:9')
            expect(page.locator('#config-preview-description')).to_contain_text('32:9')
        page.locator('#language').select_option('pt-BR')
        page.locator('#settings-save').click()
        expect(page.locator('#settings-save')).to_be_disabled()
        page.reload()
        expect(aspect).to_have_value('ultrawide')
        aspect.focus()
        ini = self.local/'user-no-mii/mk8d.ini'
        self.assertIn('aspect = ultrawide', ini.read_text())
        self.assertIn('future_setting = preserve', ini.read_text())
        page.locator('#config-field-theme').focus()
        expect(page.locator('#config-scene')).to_have_class('config-scene original-ratio')
        expect(page.locator('#config-screen-caption')).to_contain_text('Menus mantêm 16:9')
        frame_gen = page.locator('#config-field-frame_gen')
        frame_gen.select_option('120')
        expect(aspect).to_have_value('ultrawide')
        expect(page.locator('#config-scene')).to_have_class('config-scene original-ratio')
        expect(page.locator('#config-screen-caption')).to_contain_text('16:9 com geração de quadros')
        page.locator('#settings-save').click()
        expect(page.locator('#settings-save')).to_be_disabled()
        page.reload()
        expect(aspect).to_have_value('ultrawide')
        expect(frame_gen).to_have_value('120')
        self.assertIn('frame_gen = 120', ini.read_text())
        frame_gen.select_option('off')
        aspect.focus()
        expect(page.locator('#config-scene')).to_have_class('config-scene')
        self.shot('ultrawide')
        page.set_viewport_size({'width':390,'height':844})
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.shot('ultrawide-mobile')

    def test_audio_controller_mappings_and_keyboard_modules(self):
        page = self.page
        page.locator('#config-sub-audio').click()
        page.locator('#config-volume-range').fill('65')
        expect(page.locator('#config-field-volume')).to_have_value('65')
        expect(page.locator('#config-volume-display')).to_have_text('65%')
        self.assertGreater(page.locator('.config-sound-bars i').nth(4).bounding_box()['height'], 10)
        page.locator('#config-field-mute_in_background').check()
        expect(page.locator('#config-background-state')).to_have_text('Silencia em segundo plano')
        self.shot('04-audio')
        page.locator('#config-sub-controls').click()
        page.locator('#config-field-prompts').select_option('playstation')
        expect(page.locator('[data-pad-text="south"]')).to_have_text('×')
        page.get_by_text('Mapeamento do controle', exact=True).click()
        page.locator('#config-field-bind_pad_a').select_option('south')
        expect(page.locator('#config-field-bind_pad_b')).to_have_value('east')
        expect(page.locator('[data-pad="south"]')).to_have_class('highlight')
        expect(page.locator('#config-binding-example')).to_contain_text('A →')
        self.shot('05-controller')
        page.get_by_text('Mapeamento do teclado', exact=True).click()
        page.locator('#config-field-bind_key_a').select_option('Space')
        expect(page.locator('#config-keyboard-example')).to_be_visible()
        expect(page.locator('#config-key-selected')).to_have_text('Space')
        expect(page.locator('#config-controller-preview svg')).to_be_hidden()
        page.locator('#config-tab-video').focus()
        page.keyboard.press('ArrowRight')
        expect(page.locator('#config-tab-fonts')).to_be_focused()
        expect(page.locator('#config-panel-fonts')).to_be_visible()
        page.keyboard.press('ArrowRight')
        expect(page.locator('#config-panel-content')).to_be_visible()
        page.locator('#config-tab-video').click()
        page.locator('#config-sub-audio').click()
        expect(page.locator('#config-field-volume')).to_have_value('65')

    def test_quick_game_language_save_preserves_drafts_and_handles_failure(self):
        page = self.page
        launcher_language = page.locator('#language').input_value()
        expect(page.locator('#settings-game-language')).to_be_visible()
        expect(page.locator('#config-field-language')).to_be_hidden()
        page.locator('#config-field-theme').select_option('NES')
        page.locator('[data-tab="play"]').click()
        patches = []
        page.on('request', lambda request: patches.append(request.post_data_json)
                if request.url.endswith('/api/settings') and request.method == 'POST' else None)
        page.locator('#play-game-language').select_option('fr')
        expect(page.locator('#play-game-language-status')).to_contain_text('Salvo')
        self.assertEqual(patches, [{'language': 'fr'}])
        ini = (self.local/'user-no-mii/mk8d.ini').read_text()
        self.assertIn('language = fr', ini)
        self.assertIn('theme = Default', ini)
        expect(page.locator('#language')).to_have_value(launcher_language)
        self.shot('08-play-language')
        page.locator('[data-tab="settings"]').click()
        expect(page.locator('#settings-game-language')).to_have_value('fr')
        expect(page.locator('#config-field-theme')).to_have_value('NES')
        expect(page.locator('#settings-save')).to_be_enabled()
        page.locator('#settings-save').click()
        expect(page.locator('#settings-save')).to_be_disabled()
        self.assertEqual(patches[-1], {'theme': 'NES'})
        page.reload()
        expect(page.locator('#settings-game-language')).to_have_value('fr')
        page.route('**/api/settings', lambda route: route.fulfill(json={'ok':False,'error':'Falha de teste'})
                   if route.request.method == 'POST' else route.continue_())
        page.locator('#settings-game-language').select_option('en-US')
        expect(page.locator('#settings-game-language-status')).to_contain_text('Não foi possível salvar')
        expect(page.locator('#settings-game-language')).to_have_value('fr')
        page.unroute('**/api/settings')
        page.locator('#settings-game-language').select_option('en-US')
        expect(page.locator('#settings-game-language-status')).to_contain_text('Salvo')
        page.set_viewport_size({'width':390,'height':844})
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.shot('09-settings-language-mobile')
        page.locator('[data-tab="play"]').click()
        expect(page.locator('#play-game-language')).to_have_value('en-US')
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.assertGreater(page.locator('#play .game-language-copy').bounding_box()['width'], 160)
        self.shot('10-play-language-mobile')
        waiting = []
        page.route('**/api/settings', lambda route: waiting.append(route)
                   if route.request.method == 'POST' else route.continue_())
        page.locator('#play-game-language').select_option('es')
        expect(page.locator('#play-game-language-status')).to_contain_text('Salvando')
        expect(page.locator('#settings-game-language')).to_be_disabled()
        self.assertEqual(len(waiting), 1)
        page.locator('#play-solo').dispatch_event('click')
        expect(page.locator('#notice')).to_contain_text('Aguarde o idioma')
        waiting[0].continue_()
        expect(page.locator('#play-game-language-status')).to_contain_text('Salvo')
        page.unroute('**/api/settings')
        token = page.locator('meta[name="mk8-token"]').get_attribute('content')
        state = page.request.get(self.url+'/api/status',headers={'X-MK8-Token':token}).json()['data']
        state['game_pid'] = 123
        page.route('**/api/status',lambda route:route.fulfill(json={'ok':True,'data':state}))
        page.evaluate('refresh()')
        expect(page.locator('#play-game-language')).to_be_disabled()
        expect(page.locator('#settings-game-language')).to_be_disabled()

    def test_content_preview_shortcut_and_mobile(self):
        page = self.page
        page.locator('#config-tab-content').click()
        expect(page.locator('#config-package-dlc')).to_be_visible()
        expect(page.locator('#ctgp-card')).to_be_hidden()
        page.locator('#config-content-ctgp').click()
        actual = page.locator('#ctgp-layout').input_value()
        page.locator('#config-layout-preview').select_option('added')
        expect(page.locator('.config-cup-grid.ctgp i')).to_have_count(16)
        expect(page.locator('#ctgp-layout')).to_have_value(actual)
        self.shot('06-ctgp')
        page.locator('[data-tab="play"]').click()
        page.locator('.play-shortcuts [data-anchor="ctgp-title"]').click()
        expect(page.locator('#ctgp-card')).to_be_visible()
        page.set_viewport_size({'width':390,'height':844})
        page.emulate_media(reduced_motion='reduce')
        for module in ('video', 'fonts', 'content'):
            page.locator(f'#config-tab-{module}').click()
            expect(page.locator(f'#config-panel-{module}')).to_be_visible()
            self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'), module)
        page.locator('#config-tab-video').click()
        page.locator('#config-field-theme').select_option('NES')
        self.shot('07-mobile')


if __name__ == '__main__':
    unittest.main()
