"""Real browser + two launcher processes; never needs proprietary game inputs."""
import ctypes
import re
import json
import os
import socket
from pathlib import Path
import subprocess
import struct
import tempfile
import time
import unittest
from playwright.sync_api import sync_playwright, expect

ROOT=Path(__file__).resolve().parents[1]


class LauncherTests(unittest.TestCase):
    def test_nextendo_opt_in_button_and_backend_guard(self):
        page = self.page()
        try:
            page.locator('[data-tab="play"]').click()
            expect(page.locator('#play-nextendo')).to_be_visible()
            expect(page.locator('#play-nextendo')).to_be_disabled()
            token = page.locator('meta[name="mk8-token"]').get_attribute('content')
            headers = {'X-MK8-Token': token}
            refused = page.request.post(self.urls[0]+'/api/play', headers=headers,
                                        data=json.dumps({'online':False, 'nextendo':True})).json()
            self.assertFalse(refused['ok'])
            self.assertRegex(refused['error'], 'Mii|Nextendo')
            status = page.request.get(self.urls[0]+'/api/status', headers=headers).json()['data']
            key = status['nextendo_game_key']
            self.assertFalse(status['nextendo_trusted'])
            invalid = page.request.post(self.urls[0]+'/api/nextendo/trust', headers=headers,
                                        data=json.dumps({'key':'0'*64})).json()
            self.assertFalse(invalid['ok'])
            status.update(nextendo_supported=False, candidate=True, game_pid=None)
            pending_status = []
            hold_status = False
            def send_status(route):
                if hold_status:
                    pending_status.append(route)
                else:
                    route.fulfill(json={'ok':True,'data':status})
            page.route('**/api/status', send_status)
            calls = []
            page.route('**/api/mii/active', lambda route: route.fulfill(json={'ok':True,'data':'Piloto'}))
            def play(route):
                calls.append(route.request.post_data_json)
                route.fulfill(json={'ok':True,'data':{'message':'Entre no Nextendo pelo navegador. No jogo, escolha Online Play. CTGP fica desativado nesta sessão.'}})
            page.route('**/api/play', play)
            expect(page.locator('#nextendo-rebuild')).to_be_visible()
            status['nextendo_supported'] = True
            expect(page.locator('#play-nextendo')).to_be_enabled()
            # A background status poll may take longer than cancelling and reopening consent.
            hold_status = True
            page.locator('#play-nextendo').click()
            expect(page.locator('#nextendo-key')).to_have_text(key)
            page.locator('#nextendo-cancel').click()
            self.assertFalse(calls)
            page.locator('#play-nextendo').click()
            expect(page.locator('#nextendo-consent')).to_be_visible()
            with page.expect_response('**/api/play'):
                page.locator('#nextendo-accept').click()
            self.assertEqual(calls, [{'online':False, 'nextendo':True}])
            hold_status = False
            for request in pending_status:
                send_status(request)
            actual = page.request.get(self.urls[0]+'/api/status', headers=headers).json()['data']
            self.assertTrue(actual['nextendo_trusted'])
            status['nextendo_trusted'] = True
            expect(page.locator('#notice')).to_contain_text('CTGP')
            page.locator('#language').select_option('en')
            expect(page.locator('#play-nextendo')).to_have_text('Play on Nextendo')
            status['game_pid'] = 123
            expect(page.locator('#play-nextendo')).to_be_disabled()
            message = 'Nextendo refused online play for this account: link a Discord account at nextendo.network (account page) and try again.'
            status.update(game_pid=None, game_exit='exit code: 2', nextendo_refusal_message=message)
            expect(page.locator('#notice')).to_have_text(message)
            expect(page.locator('#game-state')).to_have_text(message)
            status.update(nextendo_refusal_message='Nextendo refused online access: maintenance', last_report='report-2.zip')
            expect(page.locator('#notice')).to_have_text('Nextendo refused online access: maintenance')
            status.update(nextendo_refusal_message=None, last_report='report-3.zip')
            expect(page.locator('#notice')).to_have_text('The game exited with an error. A report was saved in Support.')
        finally:
            page.close()

    def test_dlc_mismatch_is_visible_before_game_launch_and_translated(self):
        page = self.page()
        try:
            page.route('**/api/mii/active', lambda route: route.fulfill(json={'ok':True,'data':'Piloto'}))
            page.locator('[data-tab="session"]').click()
            expect(page.locator('#steam-dlc-hint')).to_contain_text('mesma DLC')
            page.route('**/api/steam/friends', lambda route: route.fulfill(json={'ok':True,'data':{
                'invites':[{'lobby':'123','friend':'Test host'}], 'friends':[], 'room':None}}))
            error = 'A sala usa o Booster Course Pass. Importe e ative o DLC em Configurações para entrar.'
            page.route('**/api/steam/join', lambda route: route.fulfill(status=400,json={'ok':False,'error':error}))
            page.locator("#mp-social-rooms").click()
            page.get_by_role('button', name='Aceitar', exact=True).click()
            expect(page.locator('#notice')).to_have_text(error)
            page.locator('#language').select_option('en')
            expect(page.locator('#steam-dlc-hint')).to_contain_text('same DLC enabled')
            page.get_by_role('button', name='Accept', exact=True).click()
            expect(page.locator('#notice')).to_contain_text('Booster Course Pass')
            page.locator('#language').select_option('pt-BR')
        finally:
            page.close()

    def test_all_steam_friends_are_searchable_and_offline_invites_are_available(self):
        page = self.page()
        try:
            token = page.locator('meta[name="mk8-token"]').get_attribute('content')
            status = page.request.get(self.urls[0]+'/api/status',headers={'X-MK8-Token':token}).json()['data']
            status['session'].update(hosting=True, steam_active=True, connected=True, slot=0)
            page.route('**/api/status',lambda route:route.fulfill(json={'ok':True,'data':status}))
            # Backend ordering is exercised natively in the session crate; the page must preserve it.
            friends = [dict(id=str(i),name=name,online=True,presence=presence,room=None)
                       for i,(name,presence) in enumerate([('A busy','busy'),('Z online','online'),('A away','away')])]
            friends += [dict(id=str(100+i),name=f'Offline {i:04}',online=False,presence='offline',room=None) for i in range(720)]
            friends.append(dict(id='9999',name='Invisible Friend',online=False,presence='offline',room=None))
            page.route('**/api/steam/friends',lambda route:route.fulfill(json={'ok':True,'data':dict(friends=friends,invites=[],room='123',persona='Test')}))
            sent=[]
            def invite(route):
                sent.append(route.request.post_data_json)
                route.fulfill(json={'ok':True,'data':{'message':'Convite enviado pela Steam.'}})
            page.route('**/api/steam/invite',invite)
            page.locator('[data-tab="session"]').click()
            rows=page.locator('#steam-friends li')
            expect(rows).to_have_count(724)
            expect(rows.nth(0)).to_contain_text('Ocupado')
            expect(rows.nth(2)).to_contain_text('Ausente')
            page.locator('#friend-filter').fill('OFFLINE 0719')
            expect(rows).to_have_count(1)
            expect(rows.first).to_contain_text('Offline 0719')
            page.locator('#friend-filter').fill('invisible')
            expect(rows).to_have_count(1)
            expect(rows.first).to_contain_text('Offline')
            rows.first.get_by_role('button',name='Convidar',exact=True).click()
            expect(page.locator('#notice')).to_have_text('Convite enviado pela Steam.')
            self.assertEqual(sent,[{'friend':'9999'}])  # Intercepted: no real invite is sent.
            page.locator('#friend-filter').fill('unmatched name')
            expect(rows).to_have_text(['Nenhum amigo encontrado nesta busca.'])
            page.locator('#language').select_option('en')
            expect(rows).to_have_text(['No friends match your search.'])
            page.locator('#friend-filter').fill('')
            expect(rows).to_have_count(724)
            expect(rows.nth(0)).to_contain_text('Busy')
            expect(rows.nth(2)).to_contain_text('Away')
            page.screenshot(path=str(ROOT/'local/screenshots/all-friends.png'),full_page=True)
            page.locator('#language').select_option('pt-BR')
            expect(page.locator('html')).to_have_attribute('lang','pt-BR')
        finally:
            page.close()

    def test_optional_dlc_and_manual_room_controls(self):
        page = self.page()
        try:
            self.show_files(page,ready=True)
            expect(page.locator('#prepare-dlc-package')).to_be_visible()
            page.locator('#prepare-dlc-package').fill('optional-owned-dlc.nsp')
            page.screenshot(path=str(ROOT/'local/screenshots/optional-dlc-install.png'),full_page=True)
            requests = []
            def prepare(route):
                requests.append(route.request.post_data_json)
                route.fulfill(json={'ok':True, 'data':{'message':'Preparando o jogo.'}})
            page.route('**/api/prepare', prepare)
            page.locator('#flow-step-2').click()
            page.locator('#build').click()
            expect(page.locator('#notice')).to_have_text('Preparando o jogo.')
            page.wait_for_function('() => !busy')
            self.assertEqual(requests, [{'dlc_package':'optional-owned-dlc.nsp'}])
            page.locator('#flow-step-1').click()
            page.locator('#prepare-dlc-package').fill('')
            page.locator('#flow-step-2').click()
            page.locator('#build').click()
            page.wait_for_function('() => !busy')
            self.assertEqual(requests[-1], {'dlc_package':''})
            page.locator('[data-tab="session"]').click()
            hosts = []
            def host(route):
                hosts.append(route.request.post_data_json)
                route.fulfill(json={'ok':True,'data':{}})
            page.route('**/api/steam/host', host)
            page.locator('#mp-host-mode [value=manual]').check()
            page.locator('#steam-host-manual').click()
            page.wait_for_function('() => !busy')
            page.locator('#mp-host-mode [value=automatic]').check()
            page.locator('#steam-host').click()
            page.wait_for_function('() => !busy')
            self.assertEqual(hosts, [{'automatic':False}, {'automatic':True}])
            page.locator('#mp-social-rooms').click()
            page.locator('#steam-navigation').select_option('manual')
            expect(page.locator('#notice')).to_have_text('Preferência de convites salva neste computador.')
            page.reload()
            page.locator('[data-tab="session"]').click()
            page.locator('#mp-social-rooms').click()
            expect(page.locator('#steam-navigation')).to_have_value('manual')
            page.locator('#steam-navigation').select_option('automatic')
            page.wait_for_function('() => !busy')
            page.screenshot(path=str(ROOT/'local/screenshots/manual-room-desktop.png'),full_page=True)
            page.set_viewport_size({'width':390,'height':844})
            self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
            page.screenshot(path=str(ROOT/'local/screenshots/manual-room-mobile.png'),full_page=True)
            page.locator('[data-tab="play"]').click()
            page.locator('#play-dlc-manage').click()
            expect(page.locator('#install')).to_be_visible()
            expect(page.locator('#dlc-card')).to_be_in_viewport()
        finally:
            page.close()

    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory(prefix="mk8-e2e-")
        cls.processes=[];cls.urls=[]
        env = os.environ.copy()
        relay_exe = env.get("MK8_TEST_RELAY_EXE")
        if relay_exe:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            relay = subprocess.Popen(
                [relay_exe, "127.0.0.1:0", "127.0.0.1", "127.0.0.1", str(min(port, 65000))],
                stdout=subprocess.PIPE, text=True,
            )
            cls.addClassCleanup(relay.wait, timeout=10)
            cls.addClassCleanup(relay.terminate)
            line = relay.stdout.readline().strip()
            cls.addClassCleanup(relay.stdout.close)
            if not line.startswith("MK8 relay "):
                raise RuntimeError("Relay failed to start")
            cls.relay_url = "http://" + line.removeprefix("MK8 relay ")
            env["MK8_RELAY_URL"] = cls.relay_url
        for index in range(2):
            folder=Path(cls.temporary.name)/str(index)
            process=subprocess.Popen([os.environ.get('MK8_LAUNCHER_EXE',str(ROOT/"target/release/Launcher.exe")),"--headless","--data-dir",str(folder)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0)
            cls.processes.append(process)
            start=time.monotonic()
            while not (folder/"launcher.json").exists():
                if process.poll() is not None or time.monotonic()-start>10:
                    raise RuntimeError("Launcher failed to start")
                time.sleep(.05)
            cls.urls.append(json.loads((folder/"launcher.json").read_text())["url"])
        cls.pw=sync_playwright().start()
        cls.browser=cls.pw.chromium.launch(headless=True)

    @classmethod
    def tearDownClass(cls):
        cls.browser.close();cls.pw.stop()
        for process in cls.processes:
            process.terminate();process.wait(timeout=10)
        cls.temporary.cleanup()

    def test_private_python_does_not_require_system_path(self):
        baseline = self.page()
        try:
            token = baseline.locator('meta[name="mk8-token"]').get_attribute('content')
            state = baseline.request.get(self.urls[0]+'/api/status', headers={'X-MK8-Token': token}).json()['data']
            expected = [c for c in state['tools'] if 'Python' in c['name']]
        finally:
            baseline.close()
        folder = Path(self.temporary.name) / 'without-system-python'
        exe = os.environ.get('MK8_LAUNCHER_EXE', str(ROOT/'target/release/Launcher.exe'))
        env = dict(os.environ, PATH='', PYTHONHOME='Z:/missing-python', PYTHONPATH='Z:/missing-modules')
        process = subprocess.Popen([exe, '--headless', '--data-dir', str(folder)], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            start = time.monotonic()
            while not (folder/'launcher.json').exists():
                self.assertIsNone(process.poll())
                self.assertLess(time.monotonic()-start, 20)
                time.sleep(.05)
            url = json.loads((folder/'launcher.json').read_text())['url']
            page = self.browser.new_page()
            try:
                page.goto(url)
                token = page.locator('meta[name="mk8-token"]').get_attribute('content')
                state = page.request.get(url+'/api/status', headers={'X-MK8-Token': token}).json()['data']
                checks = [c for c in state['tools'] if 'Python' in c['name']]
                self.assertEqual(len(checks), 1)
                self.assertEqual(checks, expected)
            finally:
                page.close()
        finally:
            process.terminate(); process.wait(timeout=10)

    @unittest.skipUnless(os.name=='nt','Native WebView2 required')
    def test_local_manual_window_content_origin_and_close(self):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
        env=dict(os.environ,WEBVIEW2_USER_DATA_FOLDER=str(Path(self.temporary.name)/'manual-webview'),
                 WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=f'--remote-debugging-port={port} --remote-allow-origins=http://127.0.0.1:{port}')
        process=subprocess.Popen([os.environ.get('MK8_LAUNCHER_EXE',str(ROOT/'target/release/Launcher.exe')),'--how-to-play'],env=env)
        try:
            deadline=time.monotonic()+40
            while True:
                try:
                    browser=self.pw.chromium.connect_over_cdp(f'http://127.0.0.1:{port}',timeout=1000);break
                except Exception:
                    self.assertIsNone(process.poll());self.assertLess(time.monotonic(),deadline);time.sleep(.2)
            page=browser.contexts[0].pages[0]
            errors=[];page.on('pageerror',lambda error:errors.append(str(error)))
            expect(page.get_by_role('heading',name='How to Play',exact=True)).to_be_visible()
            expect(page.get_by_role('heading',name='Basic controls')).to_be_visible()
            close=page.locator('form').get_attribute('action')
            origin=page.url.rstrip('/')
            self.assertEqual(page.request.post(origin+close,headers={'Origin':'https://example.invalid'}).status,403)
            self.assertIsNone(process.poll())
            for width in (900,390):
                page.set_viewport_size({'width':width,'height':780})
                self.assertTrue(page.evaluate('document.documentElement.scrollWidth<=innerWidth'))
            page.get_by_role('button',name='Back to game').click(no_wait_after=True)
            self.assertEqual(process.wait(timeout=10),0)
            self.assertFalse(errors,errors)
        finally:
            if process.poll() is None:process.terminate();process.wait(timeout=10)

    def test_console_font_import_and_system_default_persist_without_touching_saves(self):
        source=Path(self.temporary.name)/'font-source'
        source.mkdir()
        raw=bytearray(12+7*16+7*4)
        raw[:4]=b'\x00\x01\x00\x00'
        struct.pack_into('>H',raw,4,7)
        for i,tag in enumerate((b'cmap',b'glyf',b'head',b'hhea',b'hmtx',b'loca',b'maxp')):
            struct.pack_into('>4sIII',raw,12+i*16,tag,0,12+7*16+i*4,4)
        (source/'FontStandard.ttf').write_bytes(raw)
        local=Path(self.temporary.name)/'0'
        save=local/'user-no-mii/save/font-test-marker'
        save.parent.mkdir(parents=True,exist_ok=True)
        save.write_bytes(b'keep-save')
        page=self.page()
        try:
            page.locator('#language').select_option('en')
            page.locator('[data-tab="settings"]').click()
            page.locator('#config-tab-fonts').click()
            expect(page.locator('#font-mode')).to_have_value('system')
            expect(page.locator('#font-mode option[value="system"]')).to_have_text('System Default')
            page.locator('#font-folder').fill(str(source))
            page.locator('#font-import').click()
            expect(page.locator('#font-mode')).to_have_value('console')
            expect(page.locator('#font-status')).to_have_text('Imported fonts available.')
            imported=json.loads((local/'font-library.json').read_text())
            destination=Path(imported['files'][0]['path'])
            self.assertEqual(destination.read_bytes(),raw)
            (source/'FontStandard.ttf').write_bytes(b'broken')
            page.locator('#font-import').click()
            expect(page.locator('#notice-popup')).to_have_class('error')
            self.assertEqual(json.loads((local/'font-library.json').read_text()),imported)
            page.locator('#font-mode').select_option('system')
            page.wait_for_function('() => !busy')
            page.reload()
            page.locator('[data-tab="settings"]').click()
            page.locator('#config-tab-fonts').click()
            expect(page.locator('#font-mode')).to_have_value('system')
            self.assertTrue(destination.is_file())
            page.locator('#font-mode').select_option('console')
            page.wait_for_function('() => !busy')
            self.assertTrue(json.loads((local/'font-library.json').read_text())['enabled'])
            self.assertEqual(save.read_bytes(),b'keep-save')
            token=page.locator('meta[name="mk8-token"]').get_attribute('content')
            state=page.request.get(self.urls[0]+'/api/status',headers={'X-MK8-Token':token}).json()['data']
            state['game_pid']=123
            page.route('**/api/status',lambda route:route.fulfill(json={'ok':True,'data':state}))
            page.evaluate('refresh()')
            expect(page.locator('#font-mode')).to_be_disabled()
            expect(page.locator('#font-import')).to_be_disabled()
            page.unroute('**/api/status')
            page.locator('#language').select_option('es')
            expect(page.locator('#font-title')).to_have_text('Fuentes del juego')
            expect(page.locator('#font-mode option[value="system"]')).to_have_text('Predeterminada del sistema')
            expect(page.locator('#font-mode option[value="console"]')).to_have_text('Fuentes de consola importadas')
            expect(page.locator('#font-status')).to_have_text('Fuentes importadas disponibles.')
            page.locator('#language').select_option('pt-BR')
            expect(page.locator('#font-mode option[value="system"]')).to_have_text('Padrão do sistema')
            page.screenshot(path=str(ROOT/'local/graphics-diagnostics-work/font-import-ui.png'),full_page=True)
        finally:
            page.locator('#language').select_option('pt-BR')
            page.close()

    def page(self,index=0,viewport=None):
        page=self.browser.new_page(locale="pt-BR", viewport=viewport or {"width":1365,"height":1024})
        self.addCleanup(page.close)
        page.route('**/api/mii/composer/download', lambda route: route.fulfill(json={
            'ok':True,'data':{'state':'failed','detail':'Download indisponível no teste offline.'}}))
        page.goto(self.urls[index]);page.wait_for_load_state("networkidle")
        # Language tests persist their choice on the shared server. Reset and
        # await the save before a later reload, instead of relying on locale.
        page.locator('#language').select_option('pt-BR')
        expect(page.locator('html')).to_have_attribute('lang', 'pt-BR')
        expect(page.locator('#language')).to_be_enabled()
        return page

    def show_files(self, page, index=0, ready=False):
        def status(route):
            response=route.fetch(); body=response.json()
            body['data']['tools_ready']=True
            if ready:
                body['data']['files']['ready']=True
                body['data']['candidate']=True
            route.fulfill(response=response,json=body)
        page.route('**/api/status',status)
        page.evaluate('() => refresh()')
        page.locator('#flow-step-1').click()

    def prepare_error(self,page,index=0):
        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
        result=page.request.post(self.urls[index]+'/api/prepare',headers={'X-MK8-Token':token},data={}).json()
        self.assertFalse(result['ok'])
        return result['error']

    def test_notices_are_fixed_dismissible_and_respect_reduced_motion(self):
        page = self.page()
        try:
            page.locator('[data-tab="settings"]').click()
            expect(page.locator('[data-key="volume"]')).to_be_enabled()
            page.evaluate("notice('Aviso de teste')")
            popup = page.locator("#notice-popup")
            close = page.locator("#notice-close")
            expect(popup).to_be_visible()
            expect(page.locator("#notice")).to_have_text("Aviso de teste")
            page.wait_for_timeout(260)
            top = popup.bounding_box()["y"]
            page.evaluate("window.scrollTo(0, document.body.scrollHeight)")
            self.assertAlmostEqual(popup.bounding_box()["y"], top, delta=1)
            # Modules can fit without document scrolling; the notification remains fixed.
            close.click()
            expect(popup).to_be_hidden()
            expect(page.locator("#notice")).to_have_text("")
            page.emulate_media(reduced_motion="reduce")
            page.evaluate("notice('Falha de teste', true)")
            expect(popup).to_be_visible()
            self.assertEqual(popup.evaluate("el => el.getAnimations().length"), 0)
            expect(page.locator("#notice")).to_have_class("error")
            close.focus()
            page.keyboard.press("Enter")
            expect(popup).to_be_hidden()
            page.set_viewport_size({"width":390, "height":844})
            page.evaluate("notice('Mensagem longa: ' + 'arquivo/'.repeat(50))")
            expect(popup).to_be_visible()
            self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
            box = popup.bounding_box()
            self.assertGreaterEqual(box["x"], 0)
            self.assertLessEqual(box["x"] + box["width"], 390)
            page.locator("#language").select_option("en")
            expect(close).to_have_attribute("aria-label", "Close notification")
            close.click()
            page.locator("#language").select_option("pt-BR")
            expect(popup).to_be_hidden()
        finally:
            page.close()

    def test_settings_save_locks_edits_through_delayed_response_and_reload(self):
        folder = Path(self.temporary.name) / "0/user-no-mii"
        folder.mkdir(exist_ok=True)
        ini = folder / "mk8d.ini"
        ini.write_text("volume = 1\nshow_fps = 0\nunknown_future = preserve\n", encoding="utf-8")
        page = self.page()
        phase, held_post, held_get = ["post"], [], []
        def delay(route):
            if phase[0] == "post" and route.request.method == "POST":
                held_post.append((route, route.fetch()))
            elif phase[0] == "reload" and route.request.method == "GET":
                held_get.append((route, route.fetch()))
            else:
                route.continue_()
        try:
            page.locator('[data-tab="settings"]').click()
            volume = page.locator('[data-key="volume"]')
            fps = page.locator('[data-key="show_fps"]')
            expect(volume).to_be_enabled()
            page.locator("#config-sub-audio").click()
            volume.fill("70")
            page.route("**/api/settings", delay)
            page.locator("#settings-save").click()
            expect(volume).to_be_disabled()
            expect(fps).to_be_disabled()
            expect(page.locator("#settings-save")).to_be_disabled()
            expect(page.locator("#settings-reload")).to_be_disabled()
            page.wait_for_timeout(2300)
            expect(volume).to_be_disabled()
            self.assertEqual(len(held_post), 1)
            phase[0] = "reload"
            held_post[0][0].fulfill(response=held_post[0][1])
            page.wait_for_timeout(2300)
            self.assertEqual(len(held_get), 1)
            expect(volume).to_be_disabled()
            expect(fps).to_be_disabled()
            expect(page.locator("#settings-reload")).to_be_disabled()
            phase[0] = "pass"
            held_get[0][0].fulfill(response=held_get[0][1])
            expect(volume).to_be_enabled()
            expect(volume).to_have_value("70")
            expect(fps).not_to_be_checked()
            page.locator("#config-sub-image").click()
            fps.check()
            page.locator("#settings-save").click()
            expect(page.locator("#settings-pending")).to_have_text("")
            self.assertIn("volume = 0.7", ini.read_text())
            self.assertIn("show_fps = 1", ini.read_text())
            self.assertIn("unknown_future = preserve", ini.read_text())
        finally:
            page.close()

    @unittest.skipUnless(os.name != 'nt', 'The FSR page is Linux-only; Windows uses Video > Graphics (apps/launcher/web/app.js)')
    def test_fsr_terms_precede_the_managed_install(self):
        page = self.page(index=1)
        page.evaluate("() => api('fsr/settings', {upscaling: '3.1.6', provider_lib: '', frame_generation: 'off', dxc: ''})")
        page.locator('[data-tab="fsr"]').click()
        expect(page.locator('#fsr-upscaling')).to_have_value('3.1.6')
        expect(page.locator('#fsr-provider')).to_be_hidden()
        expect(page.locator('#fsr-install')).to_be_hidden()
        error = page.evaluate("() => api('fsr/install', {}).catch(error => error.message)")
        self.assertIn('aceite a licença da AMD', error)

        terms = page.locator('#fsr-license-load')
        expect(terms).to_have_text('Ler e aceitar os termos da AMD')
        # Synthetic consent response exercises the browser without accepting the vendor's terms.
        license_info = {"verified": True, "present": True, "accepted": False, "bytes": 13, "sha256": "a" * 64}
        def status_with_terms(route):
            response = route.fetch()
            status = response.json()
            status['data']['license'] = license_info
            route.fulfill(response=response, json=status)
        def accept_fixture(route):
            license_info['accepted'] = True
            route.fulfill(json={"ok": True, "data": {"license": license_info}})
        page.route('**/api/fsr', status_with_terms)
        page.route('**/api/fsr/license', lambda route: route.fulfill(json={
            'ok': True, 'data': {'text': 'Fixture terms', 'license': license_info}}))
        page.route('**/api/fsr/accept', accept_fixture)
        terms.click()
        expect(page.locator('#fsr-license-text')).to_have_text('Fixture terms')
        expect(page.locator('#fsr-license-save')).to_be_disabled()
        page.locator('#fsr-license-accept').check()
        expect(page.locator('#fsr-install')).to_be_hidden()
        page.locator('#fsr-license-save').click()
        expect(page.locator('#fsr-install')).to_be_visible()
        expect(page.locator('#fsr-install')).to_be_enabled()
        expect(page.locator('#fsr-provider')).to_be_hidden()

    @unittest.skipUnless(os.name != 'nt', 'The FSR page is Linux-only; Windows uses Video > Graphics (apps/launcher/web/app.js)')
    def test_fsr_picker_save_survives_an_overlapping_status_refresh(self):
        page = self.page()
        page.evaluate("() => api('fsr/settings', {upscaling: '3.1.6', provider_lib: '', frame_generation: 'off', dxc: ''})")
        def show_picker(route):
            response = route.fetch()
            body = response.json()
            body['data']['native_dialogs'] = True
            route.fulfill(response=response, json=body)
        page.route('**/api/status', show_picker)
        page.evaluate('() => refresh()')
        page.locator('[data-tab="fsr"]').click()
        provider = page.locator('#fsr-provider')
        picker = page.locator('[data-browse="fsr-provider"]')
        expect(provider).to_be_hidden()
        page.locator('#fsr-advanced summary').click()
        expect(picker).to_be_visible()
        expect(provider).to_be_enabled()

        stale = []

        def hold_status(route):
            if stale:
                route.continue_()
            else:
                stale.append((route, route.fetch()))

        path = str(Path(self.temporary.name) / 'custom provider.so')
        page.route('**/api/fsr', hold_status)
        # Headless launchers have no native dialog. The settings and status requests use the real API.
        page.route('**/api/browse', lambda route: route.fulfill(json={'ok': True, 'data': {'path': path}}))
        page.evaluate('() => { void window.mk8Fsr.load(); }')
        deadline = time.monotonic() + 5
        while not stale and time.monotonic() < deadline:
            page.wait_for_timeout(50)
        self.assertTrue(stale, 'the pre-save FSR status request did not start')
        with page.expect_response('**/api/fsr/settings') as saved:
            picker.click()
        self.assertEqual(saved.value.json()['data']['settings']['provider_lib'], path)
        expect(provider).to_be_disabled()
        expect(page.locator('#notice')).not_to_have_attribute('data-message', 'Provedor FSR salvo.', timeout=500)
        route, response = stale[0]
        route.fulfill(response=response)
        expect(picker).to_be_enabled()
        expect(provider).to_have_value(path, timeout=1500)
        expect(page.locator('#notice')).to_have_attribute('data-message', 'Provedor FSR salvo.')
        settings = json.loads((Path(self.temporary.name) / '0/launcher-settings.json').read_text('utf-8'))
        self.assertEqual(settings['fsr']['provider_lib'], path)

    def test_settings_save_failure_unlocks_form_and_keeps_edits_for_retry(self):
        folder = Path(self.temporary.name) / "0/user-no-mii"
        folder.mkdir(exist_ok=True)
        ini = folder / "mk8d.ini"
        ini.write_text("volume = 1\n", encoding="utf-8")
        page = self.page()
        held = []
        def fail(route):
            if route.request.method == "POST":
                held.append(route)
            else:
                route.continue_()
        try:
            page.locator('[data-tab="settings"]').click()
            volume = page.locator('[data-key="volume"]')
            expect(volume).to_be_enabled()
            page.locator("#config-sub-audio").click()
            volume.fill("70")
            page.route("**/api/settings", fail)
            page.locator("#settings-save").click()
            expect(volume).to_be_disabled()
            page.wait_for_timeout(2300)
            expect(volume).to_be_disabled()
            self.assertEqual(len(held), 1)
            held[0].fulfill(status=500, json={"ok":False, "error":"Falha simulada ao salvar."})
            expect(page.locator("#notice")).to_contain_text("Falha simulada ao salvar.")
            expect(volume).to_be_enabled()
            expect(volume).to_have_value("70")
            expect(page.locator("#settings-save")).to_be_enabled()
            expect(page.locator("#settings-reload")).to_be_enabled()
            expect(page.locator("#settings-pending")).to_have_text("Alterações não salvas.")
            self.assertEqual(ini.read_text(), "volume = 1\n")
            page.unroute("**/api/settings", fail)
            page.locator("#settings-save").click()
            expect(page.locator("#settings-pending")).to_have_text("")
            self.assertIn("volume = 0.7", ini.read_text())
        finally:
            page.close()

    def test_settings_tab_updates_shared_ini_and_keeps_unsaved_edits_during_polling(self):
        folder = Path(self.temporary.name) / "0/user-no-mii"
        folder.mkdir(exist_ok=True)
        ini = folder / "mk8d.ini"
        legacy = b"pads_from_p2 = 1\r\njoycon_mode = horizontal\r\n"
        ini.write_bytes(b"# teste\r\nvolume = 0.50\r\nunknown_future = preserve\r\n" + legacy)
        page = self.page()
        try:
            page.locator('[data-tab="settings"]').click()
            expect(page.locator("#settings input[data-key=volume]")).to_have_value("50")
            page.locator("#config-sub-audio").click()
            page.locator("#settings input[data-key=volume]").fill("75")
            page.wait_for_timeout(2300)
            expect(page.locator("#settings input[data-key=volume]")).to_have_value("75")
            page.locator("#config-sub-image").click()
            page.locator("#settings select[data-key=resolution]").select_option("75%")
            page.locator("#config-sub-controls").click()
            expect(page.locator("[data-key=pads_from_p2], [data-key=joycon_mode]")).to_have_count(0)
            page.get_by_text("Mapeamento do teclado", exact=True).click()
            page.locator("#settings select[data-key=bind_key_a]").select_option("J")
            expect(page.locator("#settings select[data-key=bind_key_b]")).to_have_value("K")
            page.locator("#settings select[data-key=bind_key_a]").select_option("Space")
            page.locator("#settings-save").click()
            expect(page.locator("#notice")).to_contain_text("Configurações salvas.")
            self.assertIn(legacy, ini.read_bytes())
            self.assertIn("unknown_future = preserve", ini.read_text())
            self.assertIn("bind_key_a = Space", ini.read_text())
            page.reload()
            page.locator('[data-tab="settings"]').click()
            expect(page.locator("#settings input[data-key=volume]")).to_have_value("75")
            page.locator("#config-sub-controls").click()
            page.get_by_text("Mapeamento do teclado", exact=True).click()
            expect(page.locator("#settings select[data-key=bind_key_a]")).to_have_value("Space")
            ini.write_text(ini.read_text() + "show_fps = 1\n", encoding="utf-8")
            expect(page.locator("#settings input[data-key=show_fps]")).to_be_checked(timeout=5000)
            page.locator("#settings details[open] summary").click()
            page.locator("#config-tab-content").click()
            expect(page.locator("#settings #dlc-enabled")).to_be_visible()
            self.assertEqual(page.locator("#install #dlc-enabled").count(), 0)
            page.locator("#language").select_option("en")
            expect(page.locator("#settings")).to_contain_text("Settings")
            screenshots = ROOT / "local/screenshots"
            screenshots.mkdir(parents=True, exist_ok=True)
            page.screenshot(path=str(screenshots / "launcher-settings-desktop.png"), full_page=True)
            page.set_viewport_size({"width":390, "height":844})
            self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
            page.screenshot(path=str(screenshots / "launcher-settings-mobile.png"), full_page=True)
            page.locator("#language").select_option("pt-BR")
            expect(page.locator("html")).to_have_attribute("lang", "pt-BR")
        finally:
            page.close()

    def test_multiplayer_uses_the_mii_and_only_steam_rooms(self):
        page = self.page()
        token = page.locator('meta[name="mk8-token"]').get_attribute('content')
        api = lambda path, body=None: (page.request.get(self.urls[0]+"/api/"+path, headers={"X-MK8-Token":token}) if body is None
                                       else page.request.post(self.urls[0]+"/api/"+path, headers={"X-MK8-Token":token}, data=json.dumps(body))).json()
        page.get_by_role("button", name="Multijogador", exact=True).click()
        # Only Steam rooms are offered; the other transports stay for scripts behind the API.
        for removed in ("#host", "#host-internet", "#invite-input", "#scan-rooms", "#username", "#invite", "#endpoint"):
            expect(page.locator(removed)).to_have_count(0)
        expect(page.get_by_role("button", name="Criar sala", exact=True)).to_be_visible()
        # Without a Mii the tab asks for one, and rooms are refused.
        expect(page.locator("#mp-mii-text")).to_have_text("Crie seu Mii para abrir ou entrar em salas.")
        refused = api("steam/host", {})
        self.assertFalse(refused["ok"])
        self.assertIn("Mii", refused["error"])
        self.assertIn("Mii", api("steam/join", {"lobby": "123"})["error"])
        self.assertFalse(api("steam/join", {"lobby": "12a"})["ok"])
        page.get_by_role("button", name="Criar Mii agora", exact=True).click()
        expect(page.locator("#miis")).to_be_visible()
        page.locator("#mii-name").fill("João")
        page.locator("#mii-continue").click()
        expect(page.locator("#mii-saved .mii-saved-item")).to_have_text(["João"])
        page.get_by_role("button", name="Multijogador", exact=True).click()
        expect(page.locator("#mp-mii-text")).to_have_text("Você entra nas salas como João.")
        expect(page.get_by_role("button", name="Trocar Mii", exact=True)).to_be_visible()
        self.assertTrue(api("mii")["ok"])
        page.close()

    def test_01_preparation_missing_files_and_disabled_play(self):
        page=self.page();errors=[];page.on("pageerror",lambda e:errors.append(str(e)))
        expect(page.get_by_role("heading",name="Instalação",exact=True)).to_be_visible()
        expect(page.locator('#flow-step-2')).to_be_disabled()
        self.assertIn('NPDM indisponível', self.prepare_error(page))
        page.get_by_role("button",name="Jogar",exact=True).click()
        expect(page.get_by_role("button",name="Jogar solo",exact=True)).to_be_disabled()
        expect(page.locator('#game-log')).to_contain_text('Nenhum teste iniciado')
        page.get_by_role("button",name="Instalar",exact=True).click()
        (ROOT/"local/screenshots").mkdir(parents=True,exist_ok=True)
        page.screenshot(path=str(ROOT/"local/screenshots/launcher-desktop.png"),full_page=True)
        self.assertFalse(errors);page.close()

    def test_02_local_api_rejects_missing_token_cross_origin_and_large_body(self):
        page=self.page();token=page.locator('meta[name="mk8-token"]').get_attribute("content")
        self.assertEqual(page.request.get(self.urls[0]+"/api/status").status,403)
        self.assertEqual(page.request.post(self.urls[0]+"/api/play", data="{}").status,403)
        self.assertEqual(page.request.post(self.urls[0]+"/api/build",headers={"X-MK8-Token":token,"Origin":"https://example.invalid"},data="{}").status,403)
        self.assertEqual(page.request.post(self.urls[0]+"/api/config",headers={"X-MK8-Token":token},data="x"*40000).status,413)
        self.assertEqual(page.request.get(self.urls[0]+"/api/status",headers={"X-MK8-Token":token,"Host":"example.invalid"}).status,403)
        page.close()

    def test_07_saving_visible_paths_preserves_legacy_configuration(self):
        page = self.page()
        token = page.locator('meta[name="mk8-token"]').get_attribute('content')
        headers = {'X-MK8-Token': token}
        endpoint = self.urls[0] + '/api/config'
        config = page.request.get(endpoint, headers=headers).json()['data']
        legacy = {'shaders': 'retained-shaders', 'vulkan_sdk': 'retained-sdk',
                  'cmake': 'retained-cmake', 'overlay': True}
        config.update(legacy)
        self.assertTrue(page.request.post(endpoint, headers=headers, data=json.dumps(config)).json()['ok'])
        page.reload()
        # No ExeFS / RomFS / NACP fields any more: the launcher finds those itself.
        for gone in ('RomFS extraído', 'Pasta ExeFS do MK8', 'Metadados NACP'):
            expect(page.get_by_label(gone, exact=True)).to_have_count(0)
        self.show_files(page)
        page.get_by_label('Jogo base', exact=True).fill('updated-base.nsz')
        page.get_by_label('Jogo base', exact=True).press('Tab')
        expect(page.locator('#notice')).to_contain_text('Arquivos salvos')
        stored = page.request.get(endpoint, headers=headers).json()['data']
        self.assertEqual(stored['base_package'], 'updated-base.nsz')
        self.assertEqual({key: stored[key] for key in legacy}, legacy)
        stored['base_package'] = ''
        page.request.post(endpoint, headers=headers, data=json.dumps(stored))
        page.close()

    def test_06_test_launch_requires_candidate(self):
        page=self.page()
        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
        response=page.request.post(self.urls[0]+'/api/play',headers={'X-MK8-Token':token},data='{}')
        self.assertFalse(response.json()['ok'])
        page.close()

    def test_03_two_launcher_sessions_and_invite_rejection(self):
        # The LAN/P2P transports have no buttons any more; scripts/test-multiplayer.py drives them through the API.
        tokens = [self.page(i) for i in (0, 1)]
        host, client = tokens
        def api(page, index, path, body=None):
            token = page.locator('meta[name="mk8-token"]').get_attribute('content')
            headers = {"X-MK8-Token": token}
            response = (page.request.get(self.urls[index]+"/api/"+path, headers=headers) if body is None
                        else page.request.post(self.urls[index]+"/api/"+path, headers=headers, data=json.dumps(body)))
            return response.json()
        def connected(page, index):
            for _ in range(100):
                if api(page, index, "status")["data"]["session"]["connected"]:
                    return True
                page.wait_for_timeout(100)
            return False
        self.assertIn("Convite inválido", api(client, 1, "session/join", {"invite": "mk8://invalid"})["error"])
        self.assertTrue(api(host, 0, "session/host", {"bind": "127.0.0.1:0"})["ok"])
        self.assertTrue(connected(host, 0))
        session = api(host, 0, "status")["data"]["session"]
        self.assertTrue(session["room_code"].startswith("MK8P-"))
        self.assertTrue(api(client, 1, "session/join", {"invite": session["room_code"]})["ok"])
        self.assertTrue(connected(client, 1))
        self.assertTrue(api(client, 1, "session/stop", {})["ok"])
        invite = api(host, 0, "session/invite", {"endpoint": session["relay"]})["data"]["invite"]
        import base64
        encoded = invite.removeprefix("mk8://")
        payload = json.loads(base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4)))
        self.assertEqual(payload["endpoint"], session["relay"])
        self.assertFalse(api(client, 1, "session/join", {"invite": "MK8S-invalid"})["ok"])
        self.assertTrue(api(client, 1, "session/join", {"invite": invite})["ok"])
        self.assertTrue(connected(client, 1))
        # The tab shows the room in progress (the host's own transport is not a guest).
        host.get_by_role("button", name="Multijogador", exact=True).click()
        expect(host.locator("#steam-room")).to_be_visible(timeout=10000)
        expect(host.locator("#network-state")).to_have_text("Na sala", timeout=10000)
        host.screenshot(path=str(ROOT/"local/screenshots/launcher-session.png"),full_page=True)
        for page, index in ((client, 1), (host, 0)):
            self.assertTrue(api(page, index, "session/stop", {})["ok"])
            page.close()

    @unittest.skipUnless(os.environ.get("MK8_TEST_RELAY_EXE"), "Relay binary not configured")
    def test_internet_code_and_bidirectional_udp(self):
        host, guest = self.page(), self.page(1)
        for page in (host, guest):
            page.get_by_role("button", name="Multijogador", exact=True).click()
        host_token = host.locator('meta[name="mk8-token"]').get_attribute("content")
        code = host.request.post(self.urls[0]+"/api/session/host-internet", headers={"X-MK8-Token":host_token}, data="{}").json()["data"]["code"]
        self.assertRegex(code, r"^MK8-[A-Za-z0-9_-]{22}$")
        self.assertFalse(host.request.post(self.relay_url+"/v1/close", data={"code":code, "owner":"invalid"}).json()["ok"])
        self.assertFalse(guest.request.post(self.relay_url+"/v1/join", data={"code":code, "identity":"wrong"}).json()["ok"])
        guest_token = guest.locator('meta[name="mk8-token"]').get_attribute("content")
        self.assertTrue(guest.request.post(self.urls[1]+"/api/session/join", headers={"X-MK8-Token":guest_token}, data=json.dumps({"invite":code})).json()["ok"])
        expect(guest.locator("#network-state")).to_have_text("Na sala", timeout=10000)

        def bridge(page, index):
            token = page.locator('meta[name="mk8-token"]').get_attribute("content")
            return page.request.get(self.urls[index]+"/api/status", headers={"X-MK8-Token":token}).json()["data"]["session"]

        def connect(sock, status):
            sock.bind(("127.0.0.1", 0))
            sock.settimeout(3)
            endpoint = ("127.0.0.1", status["bridge_port"])
            sock.sendto(b"MMKJ"+bytes([status["slot"], 9])+b"MK8ONLINE", endpoint)
            self.assertEqual(sock.recv(64), b"MMKW\x00")
            return endpoint

        a, b = bridge(host, 0), bridge(guest, 1)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as x, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as y:
            addr_a, addr_b = connect(x, a), connect(y, b)
            for src, dst, status, addr in ((x,y,a,addr_a), (y,x,b,addr_b)):
                data = b"MMKDVL1"+bytes([10,13,37,status["slot"]+1,0,1,255,255,255,255,0,2])+b"internet-fixture"
                src.sendto(data, addr)
                self.assertEqual(dst.recv(256), data)
        for page, index in ((host, 0), (guest, 1)):
            token = page.locator('meta[name="mk8-token"]').get_attribute("content")
            self.assertTrue(page.request.post(self.urls[index]+"/api/session/stop", headers={"X-MK8-Token":token}, data="{}").json()["ok"])
        self.assertFalse(guest.request.post(self.relay_url+"/v1/join", data={"code":code, "identity":"transport-fixture-v1"}).json()["ok"])
        again = host.request.post(self.urls[0]+"/api/session/host-internet", headers={"X-MK8-Token":host_token}, data="{}").json()["data"]["code"]
        self.assertNotEqual(again, code)
        self.assertTrue(host.request.post(self.urls[0]+"/api/session/stop", headers={"X-MK8-Token":host_token}, data="{}").json()["ok"])
        host.close()
        guest.close()

    def test_04_mobile_no_horizontal_overflow(self):
        page=self.page(viewport={"width":390,"height":844})
        self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
        page.get_by_role("button",name="Multijogador",exact=True).click()
        expect(page.get_by_role("heading",name="Multijogador",exact=True)).to_be_visible()
        self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
        page.screenshot(path=str(ROOT/"local/screenshots/launcher-mobile.png"),full_page=True)
        page.close()

    def test_05_arm32_explanation_before_generation(self):
        folder=Path(self.temporary.name)/"arm32";folder.mkdir()
        raw=bytearray(0xc0);raw[:4]=b"META";raw[0xc]=4
        struct.pack_into('<II',raw,0x70,0x80,0x40);raw[0x80:0x84]=b'ACI0'
        struct.pack_into('<Q',raw,0x90,0x0100152000022000)
        (folder/'exefs').mkdir()
        (folder/'romfs/Course').mkdir(parents=True)
        (folder/'control').mkdir()
        (folder/'control/control.nacp').touch()
        (folder/'exefs/main.npdm').write_bytes(raw)
        page=self.page()
        self.show_files(page)
        page.get_by_text('Já tenho o jogo extraído numa pasta', exact=True).click()
        page.get_by_label('Pasta do jogo extraído', exact=True).fill(str(folder))
        page.locator('#game-folder').blur()
        page.wait_for_function('() => !busy')
        expect(page.locator('#architecture-state')).to_contain_text('AArch32')
        expect(page.locator('#flow-step-2')).to_be_disabled()
        page.reload()
        # Reopened in folder mode, with the reason next to the folder.
        expect(page.locator('#architecture-state')).to_contain_text('AArch32')
        expect(page.locator('#files-state')).to_have_text('Pasta incompleta')
        page.close()


    def test_nsz_requires_both_packages_and_missing_keys_are_reported(self):
        page = self.page(1)
        source = Path(self.temporary.name) / "package-fixtures"
        source.mkdir()
        base, patch = source / "base.nsz", source / "update.nsz"
        base.touch()
        patch.touch()
        self.show_files(page, 1)
        page.get_by_label("Jogo base", exact=True).fill(str(base))
        page.get_by_label("Jogo base", exact=True).blur()
        page.wait_for_function('() => !busy')
        self.assertIn('Selecione dois arquivos',self.prepare_error(page,1))
        page.get_by_label("Atualização 4.0.1", exact=True).fill(str(patch))
        page.get_by_label("Arquivo prod.keys", exact=True).fill(str(source / "missing.keys"))
        page.get_by_label("Arquivo prod.keys", exact=True).blur()
        page.wait_for_function('() => !busy')
        self.assertIn('prod.keys não encontrado',self.prepare_error(page,1))
        expect(page.locator('#flow-step-2')).to_be_disabled()
        # Two of the three files exist: the step counts them and is not ready.
        expect(page.locator("#files-state")).to_have_text("2 de 3")
        expect(page.locator("#tile-base")).to_have_attribute("data-ok", "true")
        expect(page.locator("#tile-keys")).to_have_attribute("data-ok", "false")
        page.close()

    def test_10_guided_install_steps_without_false_success(self):
        page=self.page();errors=[];page.on("pageerror",lambda e:errors.append(str(e)))
        expect(page.locator('.stepper > li')).to_have_count(3)
        expect(page.locator('article.step')).to_have_count(3)
        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
        headers={'X-MK8-Token':token}
        status=page.request.get(self.urls[0]+'/api/status',headers=headers).json()['data']
        # The tools step may only claim success when every tool check passed.
        if status['tools_ready']:
            expect(page.locator('#tools-state')).to_have_text('Prontas')
            expect(page.locator('#setup-start')).to_be_hidden()
        else:
            expect(page.locator('#tools-state')).to_have_text('Pendente')
            expect(page.get_by_role('button',name='Baixar ferramentas',exact=True)).to_be_visible()
        # Empty data dir: no files, no build -> nothing is reported as ready.
        expect(page.locator('#files-state')).not_to_have_text(re.compile('Prontos|Extraídos'))
        expect(page.locator('#stage-files')).to_have_attribute('data-state','pending')
        expect(page.locator('#prepare-state')).to_have_text('Pendente')
        expect(page.locator('#play-state')).to_have_text('Bloqueado')
        self.assertEqual(status['files']['ready'],False)
        # Incomplete prerequisites keep the build locked; file selection follows tools.
        expect(page.locator('#flow-step-2')).to_be_disabled()
        self.show_files(page)
        for label in ('Jogo base','Atualização 4.0.1','Arquivo prod.keys'):
            expect(page.get_by_label(label, exact=True)).to_be_visible()
        expect(page.locator('#build-live')).to_be_hidden()
        # Headless server has no native window: pickers stay hidden and the API refuses
        # instead of pretending a path was chosen.
        expect(page.locator('[data-browse="base_package"]')).to_be_hidden()
        browse=page.request.post(self.urls[0]+'/api/browse',headers=headers,data='{"kind":"folder"}').json()
        self.assertFalse(browse['ok'])
        self.assertTrue(page.request.post(self.urls[0]+'/api/setup/cancel',headers=headers,data='{}').json()['ok'])
        status=page.request.get(self.urls[0]+'/api/status',headers=headers).json()['data']
        self.assertEqual(status['setup']['state'],'idle')
        self.assertFalse(status['native_dialogs'])
        page.screenshot(path=str(ROOT/"local/screenshots/launcher-install.png"),full_page=True)
        self.assertFalse(errors);page.close()

    def test_13_build_progress_screen(self):
        """The live build panel: percent, phase, ticking timer, estimate and log toggle (status mocked)."""
        page=self.page();errors=[];page.on("pageerror",lambda e:errors.append(str(e)))
        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
        real=page.request.get(self.urls[0]+'/api/status',headers={'X-MK8-Token':token}).json()['data']
        started=int(time.time()*1000)-125000
        def status(route):
            data=dict(real, tools_ready=True)
            data['build']={'state':'running','detail':'Preparando o jogo.','pid':1,'percent':37.5,
                           'phase':'Compilando para o seu PC','started_ms':started,'duration_s':None}
            route.fulfill(json={'ok':True,'data':data})
        page.route('**/api/status',status)
        page.reload()
        expect(page.locator('#build-live')).to_be_visible()
        expect(page.locator('#build-pct')).to_have_text('37%')
        expect(page.locator('#build-phase')).to_have_text('Compilando para o seu PC')
        expect(page.locator('#prepare-state')).to_have_text('37%')
        expect(page.locator('#stage-build')).to_have_attribute('data-state','running')
        expect(page.locator('#build-elapsed')).to_have_text(re.compile(r'^02:0\d$'))
        first=page.locator('#build-elapsed').inner_text()
        time.sleep(1.3)
        self.assertNotEqual(page.locator('#build-elapsed').inner_text(),first)
        expect(page.locator('#build-eta')).to_have_text(re.compile(r'min'))
        expect(page.get_by_role('button',name='Cancelar build',exact=True)).to_be_enabled()
        expect(page.get_by_role('button',name='Iniciar build',exact=True)).to_be_hidden()
        page.locator('#step-build [data-log="build-log"]').click()
        expect(page.locator('#build-log')).to_be_visible()
        page.screenshot(path=str(ROOT/"local/screenshots/launcher-build.png"),full_page=True)
        def done(route):
            data=dict(real, tools_ready=True);data['candidate']=True
            data['build']={'state':'candidate','detail':'ok','pid':None,'percent':100,'phase':'Concluído',
                           'started_ms':started,'duration_s':1218.5}
            route.fulfill(json={'ok':True,'data':data})
        page.unroute('**/api/status');page.route('**/api/status',done)
        expect(page.locator('#build-done')).to_be_visible(timeout=5000)
        expect(page.locator('#build-took')).to_contain_text('20:19')
        page.get_by_role('button',name='Jogar agora',exact=True).click()
        expect(page.get_by_role('heading',name='Jogar',exact=True)).to_be_visible()
        self.assertFalse(errors);page.close()

    def test_14_build_from_an_earlier_version_is_offered_for_update(self):
        """A game built by another launcher version (an update kept the data folder) is not shown as ready."""
        page=self.page();errors=[];page.on("pageerror",lambda e:errors.append(str(e)))
        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
        real=page.request.get(self.urls[0]+'/api/status',headers={'X-MK8-Token':token}).json()['data']
        self.assertFalse(real['candidate_stale'])  # (no game in this data folder)
        tools_installed = False
        files_ready = True
        def status(route):
            data=dict(real);data['candidate']=True;data['candidate_stale']=True;data['candidate_version']='0.1.2'
            data.update(tools_ready=tools_installed, files=dict(real['files'], ready=files_ready, folder=False),
                        setup=dict(real['setup'], state='done' if tools_installed else 'idle'))
            data['build']={'state':'idle','detail':'','pid':None,'percent':0,'phase':'','started_ms':None,'duration_s':None}
            route.fulfill(json={'ok':True,'data':data})
        page.route('**/api/status',status)
        page.reload()
        expect(page.locator('#flow-step-0')).to_have_attribute('aria-current', 'step')
        expect(page.locator('#flow-step-2')).to_be_disabled()
        tools_installed = True
        expect(page.locator('#flow-step-2')).to_have_attribute('aria-current', 'step')
        expect(page.locator('#build-stale')).to_be_visible()
        expect(page.locator('#build-stale')).to_contain_text('versão 0.1.2')
        expect(page.get_by_role('button',name='Atualizar a build',exact=True)).to_be_visible()
        expect(page.locator('#prepare-state')).to_have_text('Atualizar')
        expect(page.locator('#build-done')).to_be_hidden()
        tools_installed = files_ready = False
        page.reload()
        expect(page.locator('#flow-step-0')).to_have_attribute('aria-current', 'step')
        tools_installed = True
        expect(page.locator('#flow-step-1')).to_have_attribute('aria-current', 'step')
        expect(page.locator('#flow-step-2')).to_be_disabled()
        self.assertFalse(errors);page.close()

    def test_mii_resource_stream_finishes_before_wasm_request(self):
        # A slow response exposes the ordering even on browsers that eagerly buffer
        # localhost downloads. No proprietary AFL data is needed for this regression.
        page=self.browser.new_page()
        self.addCleanup(page.close)
        page.add_init_script("""(() => {
          const fetch = window.fetch.bind(window);
          let drained = false, chunks = 0;
          window.fetch = (input, options) => {
            const url = String(input);
            if (url.endsWith('/api/mii/composer/resource')) {
              return Promise.resolve(new Response(new ReadableStream({
                async pull(controller) {
                  await new Promise(resolve => setTimeout(resolve, 10));
                  if (++chunks <= 16) controller.enqueue(new Uint8Array(65536));
                  else { drained = true; controller.close(); }
                }
              }, {highWaterMark: 0})));
            }
            if (url.endsWith('/ffl-emscripten.wasm')) {
              document.documentElement.dataset.miiStreamDrainedBeforeWasm = String(drained);
              // Stop before interpreting the synthetic bytes as an AFL library.
              return Promise.reject(new Error('Synthetic resource: stop before FFL initialization'));
            }
            return fetch(input, options);
          };
        })();""")
        page.goto(self.urls[0])
        expect(page.locator('html')).to_have_attribute('data-mii-stream-drained-before-wasm','true')
        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
        result=page.request.get(self.urls[0]+'/api/status',headers={'X-MK8-Token':token},timeout=5000)
        self.assertTrue(result.json()['ok'])

    def test_mii_preparation_releases_play_and_clears_only_its_stale_notice(self):
        page=self.page()
        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
        real=page.request.get(self.urls[0]+'/api/status',headers={'X-MK8-Token':token}).json()['data']
        state={'state':'running','detail':'Preparando recursos'}
        def status(route):
            data=dict(real,candidate=True,game_pid=None)
            data['session']=dict(real['session'],connected=True)
            data['mii_preparation']={'resources':dict(state),'native':{'state':'idle','detail':''}}
            route.fulfill(json={'ok':True,'data':data})
        page.route('**/api/status',status)
        page.reload();page.locator('[data-tab="play"]').click()
        expect(page.locator('#mii-preparation-state')).to_contain_text('Preparando os modelos Mii')
        expect(page.locator('#play-solo')).to_be_disabled()
        expect(page.locator('#play-online')).to_be_disabled()
        page.evaluate("notice('Aguarde a conversão Mii terminar.', true)")
        state.update(state='done',detail='Pronto')
        expect(page.locator('#play-solo')).to_be_enabled(timeout=5000)
        expect(page.locator('#play-online')).to_be_enabled()
        expect(page.locator('#notice')).to_have_text('Modelos Mii prontos. Você já pode jogar.')
        expect(page.locator('#notice')).not_to_have_class(re.compile(r'error'))
        page.evaluate("notice('Outro erro importante', true)")
        page.evaluate('refresh()')
        expect(page.locator('#notice')).to_have_text('Outro erro importante')
        state.update(state='running')
        page.evaluate('refresh()')
        expect(page.locator('#play-solo')).to_be_disabled()
        page.evaluate("notice('Aguarde a conversão Mii terminar.', true)")
        state.update(state='failed',detail='Falha de teste na conversão')
        page.evaluate('refresh()')
        expect(page.locator('#mii-preparation-state')).to_have_text('Falha de teste na conversão')
        expect(page.locator('#notice')).to_have_text('Falha de teste na conversão')
        state.update(state='done',detail='Pronto')
        page.locator('#language').select_option('en')
        page.evaluate('refresh()')
        expect(page.locator('#mii-preparation-state')).to_have_text('Mii models are ready. You can play now.')
        page.locator('#language').select_option('pt-BR')
        page.close()

    def test_12_support_report_and_updates(self):
        page=self.page();errors=[];page.on("pageerror",lambda e:errors.append(str(e)))
        folder=Path(self.temporary.name)/'0'
        (folder/'game.log').write_text('FATAL: e2e fixture crash' + chr(10),encoding='utf-8')
        page.get_by_role('button',name='Suporte',exact=True).click()
        expect(page.get_by_role('heading',name='Suporte',exact=True)).to_be_visible()
        expect(page.locator('#version')).to_have_text(re.compile(r'^v\d+\.\d+\.\d+'))
        page.get_by_role('button',name='Gerar relatório',exact=True).click()
        expect(page.locator('#notice')).to_contain_text('Relatório de logs criado')
        reports=list((folder/'reports').glob('mk8-report-*.zip'))
        self.assertEqual(len(reports),1)
        listing=subprocess.run(['tar','-tf',str(reports[0])],capture_output=True,text=True,check=True).stdout
        self.assertIn('report.json',listing);self.assertIn('game.log',listing)
        # Headless: no Explorer window, and an edition without a configured repository says so.
        expect(page.locator('#open-reports')).to_be_hidden()
        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
        headers={'X-MK8-Token':token}
        self.assertFalse(page.request.post(self.urls[0]+'/api/open',headers=headers,data='{"what":"reports"}').json()['ok'])
        updates = page.request.get(self.urls[0]+'/api/status', headers=headers).json()['data']['updates']
        if not updates['configured']:
            page.get_by_role('button',name='Verificar atualizações',exact=True).click()
            expect(page.locator('#notice')).to_contain_text('não configuradas')
            expect(page.get_by_role('button',name='Atualizar agora',exact=True)).to_be_disabled()
        page.screenshot(path=str(ROOT/"local/screenshots/launcher-support.png"),full_page=True)
        self.assertFalse(errors);page.close()

    @unittest.skipUnless(os.name=="nt","WebView2 window is Windows-only")
    def test_11_native_window_opens_install_screen(self):
        folder=Path(self.temporary.name)/'window';folder.mkdir()
        with socket.socket() as probe:
            probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
        env=dict(os.environ,WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=f'--remote-debugging-port={port}',WEBVIEW2_USER_DATA_FOLDER=str(folder/'webview'))
        process=subprocess.Popen([os.environ.get('MK8_LAUNCHER_EXE',str(ROOT/"target/release/Launcher.exe")),"--data-dir",str(folder)],env=env)
        user32=ctypes.windll.user32
        native_hwnds=[]
        def titles():
            found=[]
            native_hwnds.clear()
            @ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p)
            def visit(hwnd,_):
                pid=ctypes.c_ulong();user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd),ctypes.byref(pid))
                if pid.value==process.pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
                    native_hwnds.append(hwnd)
                    buf=ctypes.create_unicode_buffer(256);user32.GetWindowTextW(ctypes.c_void_p(hwnd),buf,256);found.append(buf.value)
                return True
            user32.EnumWindows(visit,0)
            return found
        try:
            start=time.monotonic()
            while 'MK8-online' not in titles():
                self.assertIsNone(process.poll(),'Launcher exited before opening its window')
                self.assertLess(time.monotonic()-start,30,'Native window did not appear')
                time.sleep(.2)
            browser=None
            while browser is None and time.monotonic()-start<45:
                try:browser=self.pw.chromium.connect_over_cdp(f'http://127.0.0.1:{port}')
                except Exception:time.sleep(.3)
            self.assertIsNotNone(browser,'WebView2 debugging endpoint not reachable')
            page=browser.contexts[0].pages[0]
            page.locator('#language').select_option('pt-BR')
            expect(page.get_by_role('heading',name='Instalação',exact=True)).to_be_visible(timeout=15000)
            url=json.loads((folder/'launcher.json').read_text())['url']
            self.assertEqual(page.url.rstrip('/'),url)
            # Inside the native window the folder pickers are offered.
            self.show_files(page)
            expect(page.locator('[data-browse="base_package"]')).to_be_visible()
            page.locator('#language').select_option('en')
            expect(page.locator('[data-browse="base_package"]')).to_have_attribute('aria-label', 'Choose base game')
            page.get_by_role('button',name='Play',exact=True).click()
            expect(page.get_by_role('heading',name='Play',exact=True)).to_be_visible()
            page.locator('[data-tab="settings"]').click()
            page.locator('#config-tab-fonts').click()
            expect(page.locator('#font-mode')).to_be_enabled()
            expect(page.locator('#font-mode option[value="system"]')).to_have_text('System Default')
            expect(page.locator('[data-browse="font-folder"]')).to_be_visible()
            # The window refuses to navigate away from the local launcher.
            page.evaluate("location.href='https://example.invalid/'")
            time.sleep(1.5)
            self.assertEqual(page.url.rstrip('/'),url)
            page.unroute_all(behavior='wait')
            browser.close()
        finally:
            # Close this fixture's window normally so WebView2 can release its data folder.
            titles()
            for hwnd in native_hwnds:
                user32.PostMessageW(ctypes.c_void_p(hwnd), 0x0010, 0, 0)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate();process.wait(timeout=10)
            time.sleep(2)


if __name__=="__main__":unittest.main()
