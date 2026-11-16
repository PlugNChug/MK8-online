"""Support UI uses real local report ZIPs; GitHub publication and installers are never executed."""
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from urllib.parse import parse_qs, urlparse
import zipfile

from playwright.sync_api import expect, sync_playwright

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.environ.get('MK8_TEST_DESIGN') == '1', 'Requires the debug launcher')
class SupportDesignTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mk8-support-design-')
        self.addCleanup(self.temp.cleanup)
        self.local = Path(self.temp.name)
        (self.local/'user-no-mii/save').mkdir(parents=True)
        (self.local/'user-no-mii/save/private').write_text('PRIVATE-SAVE')
        (self.local/'prod.keys').write_text('PRIVATE-KEYS')
        (self.local/'game.log').write_text('Diagnostic example '+os.environ.get('USERPROFILE','')+'\\example\n')
        self.process = subprocess.Popen([os.environ['MK8_LAUNCHER_EXE'], '--headless', '--data-dir', str(self.local)],
            env={**os.environ, **({} if os.environ.get('MK8_TEST_EMBEDDED') == '1' else {'MK8_WEB_ROOT': str(ROOT/'apps/launcher/web')})},
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
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
        self.errors = []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.page.goto(self.url+'/#support')
        expect(self.page.locator('#support-bug-title')).to_be_visible()
        self.token = self.page.locator('meta[name="mk8-token"]').get_attribute('content')

    def stop_server(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=10)

    def tearDown(self):
        self.assertEqual(self.errors, [])
        self.assertEqual((self.local/'user-no-mii/save/private').read_text(), 'PRIVATE-SAVE')

    def shot(self, name):
        folder = ROOT/'local/screenshots'
        folder.mkdir(exist_ok=True, parents=True)
        self.page.evaluate('scrollTo(0,0)')
        self.page.screenshot(path=str(folder/f'support-{name}.png'), full_page=True, animations='disabled')

    def fill_bug(self):
        self.page.locator('#support-bug-title').fill('Controle sem resposta na sala')
        self.page.locator('#support-bug-area').select_option(label='Multijogador Steam')
        self.page.locator('#support-bug-description').fill('Após abrir a sala, o controle deixou de responder. <b>Texto literal</b>')
        self.page.locator('.support-more summary').click()
        self.page.locator('#support-bug-steps').fill('1. Abrir a Steam\n2. Criar sala automática')
        self.page.locator('#support-bug-expected').fill('Continuar navegando com o controle.')

    def test_report_zip_draft_export_and_reviewed_github_link(self):
        page = self.page
        endpoint = self.url+'/api/report/download'
        self.assertEqual(page.request.get(endpoint).status,403)
        self.assertEqual(page.request.get(endpoint,headers={'X-MK8-Token':self.token,'Origin':'https://example.com'}).status,403)
        self.assertEqual(page.request.get(endpoint,headers={'X-MK8-Token':self.token}).status,400)
        expect(page.locator('#support-download-zip')).to_be_disabled()
        page.locator('#support-github').click()
        expect(page.locator('#support-bug-title')).to_be_focused()
        self.fill_bug()
        page.locator('#report').click()
        expect(page.locator('#support-zip-status')).to_contain_text('ZIP pronto', timeout=30000)
        page.wait_for_function('() => !busy')
        with page.expect_download() as event:
            page.locator('#support-download-zip').click()
        data = Path(event.value.path()).read_bytes()
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            self.assertIn('report.json',archive.namelist())
            self.assertIn('game.log',archive.namelist())
            all_text = b'\n'.join(archive.read(name) for name in archive.namelist())
            self.assertNotIn(b'PRIVATE-SAVE',all_text)
            self.assertNotIn(b'PRIVATE-KEYS',all_text)
            profile = os.environ.get('USERPROFILE','').encode()
            if profile:
                self.assertNotIn(profile.lower(),all_text.lower())
        query = parse_qs(urlparse(page.locator('#support-github').get_attribute('href')).query)
        self.assertEqual(query['title'],['Controle sem resposta na sala'])
        self.assertIn('Texto literal',query['body'][0])
        self.assertIn('Multijogador Steam',query['body'][0])
        self.assertNotIn(str(self.local),query['body'][0])
        self.assertIn('Anexar manualmente',query['body'][0])
        expect(page.locator('.support-review b')).to_have_count(0)
        with page.expect_download() as event:
            page.locator('#support-download-text').click()
        self.assertIn('## Como reproduzir',Path(event.value.path()).read_text(encoding='utf-8'))
        opened = []
        def github(route):
            opened.append(route.request.url)
            route.fulfill(body='Issue draft intercepted; nothing published.',content_type='text/plain')
        page.context.route('https://github.com/**',github)
        with page.expect_popup() as popup:
            page.locator('#support-github').click()
        popup.value.wait_for_load_state()
        self.assertEqual(urlparse(opened[0]).path,'/Vnnc1/MK8-online/issues/new')
        popup.value.close()
        self.shot('01-report')
        page.locator('#support-bug-description').fill('á'*1900)
        expect(page.locator('#support-long-issue')).to_be_visible()
        self.assertNotIn('body',parse_qs(urlparse(page.locator('#support-github').get_attribute('href')).query))
        with page.expect_download() as event:
            page.locator('#support-download-text').click()
        self.assertIn('á'*1900,Path(event.value.path()).read_text(encoding='utf-8'))
        page.evaluate("Object.defineProperty(navigator, 'clipboard', {value:{writeText:async()=>{throw Error('blocked')}}})")
        page.locator('#support-copy').click()
        expect(page.locator('#support-share-status')).to_contain_text('Ctrl+C')
        page.route('**/api/report',lambda route:route.fulfill(json={'ok':False,'error':'Falha de teste ao gerar.'}))
        page.locator('#report').click()
        expect(page.locator('#support-zip-status')).to_contain_text('Não foi possível gerar')
        expect(page.locator('#support-download-zip')).to_be_enabled()
        page.route('**/api/report/download',lambda route:route.fulfill(status=400,json={'ok':False,'error':'ZIP indisponível.'}))
        page.locator('#support-download-zip').click()
        expect(page.locator('#support-zip-status')).to_contain_text('Não foi possível baixar')
        page.set_viewport_size({'width':390,'height':844})
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.shot('02-mobile')

    def test_update_check_install_confirmation_and_failures(self):
        page = self.page
        state = {'available':False,'installable':False,'message':'Você está na versão mais recente.'}
        page.route('**/api/update/check',lambda route:route.fulfill(json={'ok':True,'data':state}))
        page.locator('#update-check').click()
        expect(page.locator('#update-state')).to_have_text(state['message'])
        expect(page.locator('#update-install')).to_be_disabled()
        page.wait_for_function('() => !busy')
        state.update(available=True,installable=True,message='Atualização de teste disponível.',latest={'tag':'v0.2.1'})
        page.locator('#update-check').click()
        expect(page.locator('#update-install')).to_be_enabled()
        page.wait_for_function('() => !busy')
        installs = []
        def install(route):
            installs.append(route.request.post_data_json)
            route.fulfill(json={'ok':False,'error':'Instalador interceptado no teste.'})
        page.route('**/api/update/install',install)
        page.once('dialog',lambda dialog:dialog.dismiss())
        page.locator('#update-install').click()
        page.wait_for_function('() => !busy')
        self.assertEqual(installs,[])
        page.once('dialog',lambda dialog:dialog.accept())
        page.locator('#update-install').click()
        expect(page.locator('#notice')).to_contain_text('Instalador interceptado')
        self.assertEqual(installs,[{}])
        page.wait_for_function('() => !busy')
        page.route('**/api/update/check',lambda route:route.fulfill(json={'ok':False,'error':'Sem conexão no teste.'}))
        page.locator('#update-check').click()
        expect(page.locator('#notice')).to_contain_text('Sem conexão')
        expect(page.locator('#update-check')).to_be_enabled()

    def test_native_github_handoff_uses_reviewed_fields_without_publication(self):
        page = self.page
        refused = page.request.post(self.url+'/api/support/github',headers={'X-MK8-Token':self.token},data={'title':'Teste','body':'Texto'})
        self.assertEqual(refused.status,400)
        self.assertIn('prévia web',refused.json()['error'])
        state = page.request.get(self.url+'/api/status',headers={'X-MK8-Token':self.token}).json()['data']
        state['windowed'] = True
        page.route('**/api/status',lambda route:route.fulfill(json={'ok':True,'data':state}))
        page.evaluate('refresh()')
        self.fill_bug()
        requests = []
        def handoff(route):
            requests.append(route.request.post_data_json)
            route.fulfill(json={'ok':True,'data':{'message':'Abertura interceptada no teste.'}})
        page.route('**/api/support/github',handoff)
        page.locator('#support-github').click()
        page.wait_for_function('() => !busy')
        self.assertEqual(requests[0]['title'],'Controle sem resposta na sala')
        self.assertIn('Após abrir a sala',requests[0]['body'])
        page.locator('.support-issues-link').click()
        page.wait_for_function('() => !busy')
        self.assertEqual(requests[-1],{'list':True})


if __name__ == '__main__':
    unittest.main()
