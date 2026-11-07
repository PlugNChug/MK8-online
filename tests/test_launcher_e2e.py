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

    def page(self,index=0,viewport=None):
        page=self.browser.new_page(viewport=viewport or {"width":1365,"height":1024})
        page.goto(self.urls[index]);page.wait_for_load_state("networkidle")
        return page

    def test_username_persists_without_avatar(self):
        page = self.page()
        page.get_by_role("button", name="Multijogador", exact=True).click()
        page.get_by_label("Nome de usuário", exact=True).fill("João")
        page.get_by_role("button", name="Salvar nome", exact=True).click()
        expect(page.locator("#notice")).to_contain_text("Nome salvo")
        page.reload()
        page.get_by_role("button", name="Multijogador", exact=True).click()
        expect(page.get_by_label("Nome de usuário", exact=True)).to_have_value("João")
        self.assertEqual(json.loads((Path(self.temporary.name)/'0'/'username.json').read_text(encoding='utf-8')), "João")
        token = page.locator('meta[name="mk8-token"]').get_attribute('content')
        response = page.request.get(self.urls[0]+"/api/mii", headers={"X-MK8-Token":token})
        self.assertFalse(response.json()['ok'])
        page.close()

    def test_01_preparation_missing_files_and_disabled_play(self):
        page=self.page();errors=[];page.on("pageerror",lambda e:errors.append(str(e)))
        expect(page.get_by_role("heading",name="Instalação",exact=True)).to_be_visible()
        page.get_by_role("button",name="Preparar",exact=True).click()
        expect(page.locator("#notice")).to_contain_text("NPDM indisponível")
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
        page.get_by_text('Detalhes técnicos', exact=True).click()
        page.get_by_text('Caminhos manuais', exact=True).click()
        page.get_by_label('RomFS extraído', exact=True).fill('updated-romfs')
        page.get_by_role('button', name='Salvar caminhos', exact=True).click()
        expect(page.locator('#notice')).to_contain_text('Caminhos salvos')
        stored = page.request.get(endpoint, headers=headers).json()['data']
        self.assertEqual(stored['romfs'], 'updated-romfs')
        self.assertEqual({key: stored[key] for key in legacy}, legacy)
        page.close()

    def test_06_test_launch_requires_candidate(self):
        page=self.page()
        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
        response=page.request.post(self.urls[0]+'/api/play',headers={'X-MK8-Token':token},data='{}')
        self.assertFalse(response.json()['ok'])
        page.close()

    def test_03_two_launcher_sessions_and_invite_rejection(self):
        host=self.page();client=self.page(1)
        for page in (host,client):page.get_by_role("button",name="Multijogador",exact=True).click()
        client.locator("#invite-input").fill("mk8://invalid")
        client.get_by_role("button",name="Entrar na sala",exact=True).click()
        expect(client.get_by_role("status")).to_contain_text("Convite inválido")
        host.get_by_role("button",name="Criar sala na rede local",exact=True).click()
        expect(host.locator("#network-state")).to_have_text("Transporte conectado",timeout=10000)
        client.get_by_role("button",name="Buscar salas",exact=True).click()
        expect(client.locator("#rooms li").first).to_be_visible()
        expect(client.locator("#rooms")).not_to_contain_text(":47820")
        expect(host.locator("#endpoint")).to_be_hidden()
        host.get_by_role("button",name="Novo convite",exact=True).click()
        expect(host.locator("#invite-text")).not_to_have_value("")
        client.locator("#invite-input").fill(host.locator("#invite-text").input_value())
        client.get_by_role("button",name="Entrar na sala",exact=True).click()
        expect(client.locator("#network-state")).to_have_text("Transporte conectado",timeout=10000)
        expect(host.locator("#peers")).to_contain_text("2 conexão",timeout=10000)
        expect(client.locator("#network-mode")).to_contain_text("sem jogo")
        host.screenshot(path=str(ROOT/"local/screenshots/launcher-session.png"),full_page=True)
        for page in (client,host):
            page.get_by_role("button",name="Encerrar sessão",exact=True).click()
            expect(page.locator("#network-state")).to_have_text("Desconectado")
            page.close()

    @unittest.skipUnless(os.environ.get("MK8_TEST_RELAY_EXE"), "Relay binary not configured")
    def test_internet_code_and_bidirectional_udp(self):
        host, guest = self.page(), self.page(1)
        for page in (host, guest):
            page.get_by_role("button", name="Multijogador", exact=True).click()
        host.get_by_role("button", name="Criar sala pela internet").click()
        expect(host.locator("#network-state")).to_have_text("Transporte conectado", timeout=10000)
        code = host.locator("#room-code").input_value()
        self.assertRegex(code, r"^MK8-[A-Za-z0-9_-]{22}$")
        self.assertFalse(host.request.post(self.relay_url+"/v1/close", data={"code":code, "owner":"invalid"}).json()["ok"])
        self.assertFalse(guest.request.post(self.relay_url+"/v1/join", data={"code":code, "identity":"wrong"}).json()["ok"])
        guest.locator("#invite-input").fill(code)
        guest.get_by_role("button", name="Entrar na sala", exact=True).click()
        expect(guest.locator("#network-state")).to_have_text("Transporte conectado", timeout=10000)

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
        for page in (host, guest):
            page.get_by_role("button", name="Encerrar sessão", exact=True).click()
            expect(page.locator("#network-state")).to_have_text("Desconectado")
        self.assertFalse(guest.request.post(self.relay_url+"/v1/join", data={"code":code, "identity":"transport-fixture-v1"}).json()["ok"])
        host.get_by_role("button", name="Criar sala pela internet").click()
        expect(host.locator("#network-state")).to_have_text("Transporte conectado", timeout=10000)
        self.assertNotEqual(host.locator("#room-code").input_value(), code)
        host.get_by_role("button", name="Encerrar sessão", exact=True).click()
        expect(host.locator("#network-state")).to_have_text("Desconectado")
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
        page.get_by_label('Pasta do jogo extraído', exact=True).fill(str(folder))
        page.get_by_role('button',name='Preparar',exact=True).click()
        expect(page.locator('#notice')).to_contain_text('AArch32')
        page.reload()
        expect(page.locator('#architecture-state')).to_contain_text('AArch32')
        page.close()


    def test_nsz_requires_both_packages_and_missing_keys_are_reported(self):
        page = self.page(1)
        page.get_by_text("Arquivos NSZ ou NSP", exact=True).click()
        source = Path(self.temporary.name) / "package-fixtures"
        source.mkdir()
        base, patch = source / "base.nsz", source / "update.nsz"
        base.touch()
        patch.touch()
        page.get_by_label("Jogo base", exact=True).fill(str(base))
        page.get_by_role("button", name="Preparar", exact=True).click()
        expect(page.locator("#notice")).to_contain_text("Selecione dois arquivos")
        page.get_by_label("Atualização 4.0.1", exact=True).fill(str(patch))
        page.get_by_label("Arquivo prod.keys", exact=True).fill(str(source / "missing.keys"))
        page.get_by_role("button", name="Preparar", exact=True).click()
        expect(page.locator("#notice")).to_contain_text("prod.keys não encontrado")
        page.close()

    def test_10_guided_install_steps_without_false_success(self):
        page=self.page();errors=[];page.on("pageerror",lambda e:errors.append(str(e)))
        expect(page.locator('.stages > li')).to_have_count(3)
        # Tool rows come from real filesystem checks; whatever they say, the stage
        # may only claim success when every row is a check mark.
        expect(page.locator('#tools-checks .check-item')).to_have_count(4)
        rows=page.locator('#tools-checks .check-item strong').all_text_contents()
        if all(r.startswith('✓') for r in rows):
            expect(page.locator('#tools-state')).to_have_text('Encontradas')
        else:
            expect(page.locator('#tools-state')).to_contain_text('pendente')
        # Empty data dir: no paths, no candidate -> nothing can be reported as ready.
        expect(page.locator('#files-state')).not_to_have_text('Caminhos encontrados')
        expect(page.locator('#prepare-state')).to_have_text('Pendente')
        expect(page.locator('#play-state')).to_have_text('Bloqueado')
        expect(page.get_by_label('Pasta do jogo extraído', exact=True)).to_be_visible()
        expect(page.get_by_label('Jogo base', exact=True)).to_be_hidden()
        page.get_by_text('Arquivos NSZ ou NSP', exact=True).click()
        expect(page.get_by_label('Jogo base', exact=True)).to_be_visible()
        expect(page.get_by_label('Pasta do jogo extraído', exact=True)).to_be_hidden()
        page.get_by_text('Pasta extraída', exact=True).click()
        # Headless server has no native window: browse buttons stay hidden and the
        # API refuses instead of pretending a path was chosen.
        expect(page.locator('[data-browse="game-folder"]')).to_be_hidden()
        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
        headers={'X-MK8-Token':token}
        browse=page.request.post(self.urls[0]+'/api/browse',headers=headers,data='{"kind":"folder"}').json()
        self.assertFalse(browse['ok'])
        self.assertTrue(page.request.post(self.urls[0]+'/api/setup/cancel',headers=headers,data='{}').json()['ok'])
        status=page.request.get(self.urls[0]+'/api/status',headers=headers).json()['data']
        self.assertEqual(status['setup']['state'],'idle')
        self.assertFalse(status['native_dialogs'])
        page.screenshot(path=str(ROOT/"local/screenshots/launcher-install.png"),full_page=True)
        self.assertFalse(errors);page.close()

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
        if not os.environ.get('MK8_UPDATE_REPO'):
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
        def titles():
            found=[]
            @ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p)
            def visit(hwnd,_):
                pid=ctypes.c_ulong();user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd),ctypes.byref(pid))
                if pid.value==process.pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
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
            expect(page.get_by_role('heading',name='Instalação',exact=True)).to_be_visible(timeout=15000)
            url=json.loads((folder/'launcher.json').read_text())['url']
            self.assertEqual(page.url.rstrip('/'),url)
            # Inside the native window the folder pickers are offered.
            expect(page.locator('[data-browse="game-folder"]')).to_be_visible()
            page.get_by_role('button',name='Jogar',exact=True).click()
            expect(page.get_by_role('heading',name='Jogar',exact=True)).to_be_visible()
            # The window refuses to navigate away from the local launcher.
            page.evaluate("location.href='https://example.invalid/'")
            time.sleep(1.5)
            self.assertEqual(page.url.rstrip('/'),url)
            browser.close()
        finally:
            process.terminate();process.wait(timeout=10)


if __name__=="__main__":unittest.main()
