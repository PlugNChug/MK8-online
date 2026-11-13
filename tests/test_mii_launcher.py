"""Real launcher, browser and local profile files; no commercial resources or online account."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from playwright.sync_api import sync_playwright, expect

ROOT=Path(__file__).resolve().parents[1]


class MiiLauncherTests(unittest.TestCase):
    def test_create_edit_import_export_and_resource_rejection(self):
        with tempfile.TemporaryDirectory(prefix='mk8-mii-browser-') as directory:
            local=Path(directory)
            process=subprocess.Popen([os.environ.get('MK8_LAUNCHER_EXE',str(ROOT/'target/debug/Launcher.exe')),
                                      '--headless','--data-dir',str(local)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline=time.monotonic()+30
                while not (local/'launcher.json').exists():
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(),deadline)
                    time.sleep(.05)
                url=json.loads((local/'launcher.json').read_text())['url']
                with sync_playwright() as pw:
                    browser=pw.chromium.launch(headless=True)
                    try:
                        page=browser.new_page(locale='pt-BR',viewport={'width':1200,'height':900})
                        errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
                        page.goto(url);page.get_by_role('button',name='Perfil Mii',exact=True).click()
                        page.get_by_label('Nome do Mii',exact=True).fill('João')
                        page.get_by_role('button',name='Criar e avançar',exact=True).click()
                        expect(page.locator('#notice')).to_contain_text('Perfil salvo')
                        expect(page.locator('#mii-profiles option')).to_have_count(1)
                        # Naming a new profile continues into the creator, grouped by part.
                        expect(page.locator('#mii-composer')).to_be_visible()
                        expect(page.locator('#mii-full-nav button')).to_have_text(['Corpo','Rosto','Cabelo','Olhos','Sobrancelhas','Nariz','Boca','Barba e bigode','Óculos','Pinta','Avançado'])
                        expect(page.locator('#mii-full-fields input[data-offset]')).to_have_count(49)
                        page.locator('#mii-full-nav button',has_text='Boca').click()
                        expect(page.locator('#mii-full-nav button',has_text='Boca')).to_have_attribute('aria-selected','true')
                        mouth=page.locator('fieldset[data-group="mouth"] input[type=range]').first
                        mouth.fill('7')  # the slider drives the number field the profile is saved from
                        expect(page.locator('#mii-full-fields input[data-offset="69"]')).to_have_value('7')
                        page.locator('#mii-compose-close').click()
                        first=json.loads((local/'mii-profile.json').read_text(encoding='utf-8'))
                        expect(page.locator('#mii-canvas')).to_have_attribute('data-rendered','true')
                        page.locator('#mii-name').fill('Luigi')
                        page.get_by_role('button',name='Salvar Mii',exact=True).click()
                        expect(page.locator('#mii-profiles option')).to_have_text('Luigi')
                        expect(page.get_by_role('button',name='Salvar Mii',exact=True)).to_be_enabled()
                        self.assertEqual(json.loads((local/'mii-profile.json').read_text(encoding='utf-8'))['id'],first['id'])
                        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
                        headers={'X-MK8-Token':token}
                        raw=page.request.get(url+'/api/mii/export',headers=headers).json()['data']['data']
                        self.assertEqual(len(raw),88)
                        raw[15]^=1; raw[0x31]=131; raw[0x32]=99
                        page.locator('#mii-import').set_input_files({'name':'custom.charinfo','mimeType':'application/octet-stream','buffer':bytes(raw)})
                        expect(page.locator('#mii-profiles option')).to_have_count(2)
                        expect(page.get_by_role('button',name='Salvar Mii',exact=True)).to_be_enabled()
                        exported=page.request.get(url+'/api/mii/export',headers=headers).json()['data']['data']
                        self.assertEqual(exported,raw)
                        # The 49 Switch fields live in the 3D editor modal; without an AFL they still save the profile.
                        page.locator('#mii-compose-open').click()
                        expect(page.locator('#mii-compose-status')).to_contain_text('Selecione o arquivo AFL')
                        expect(page.locator('#mii-full-fields input[data-offset="49"]')).to_have_value('131')
                        page.locator('#mii-full-fields input[data-offset="66"]').fill('17') # nose
                        page.locator('#mii-full-save').click()
                        expect(page.locator('#mii-full-save')).to_be_enabled()
                        # Wait for persisted server state rather than the edited input itself.
                        for _ in range(100):
                            new=page.request.get(url+'/api/mii/export',headers=headers).json()['data']['data']
                            if new[66]==17: break
                            page.wait_for_timeout(50)
                        self.assertEqual(new[66],17);self.assertEqual(new[49],131)
                        page.locator('#mii-compose-close').click()
                        expect(page.get_by_role('button',name='Salvar Mii',exact=True)).to_be_enabled()
                        with page.expect_download() as download:
                            page.locator('#mii-export').click()
                        self.assertEqual(Path(download.value.path()).read_bytes(),bytes(new))
                        expect(page.get_by_role('button',name='Salvar Mii',exact=True)).to_be_enabled()
                        before=(local/'mii-profile.json').read_bytes()
                        response=page.request.post(url+'/api/mii/import',headers=headers,data=json.dumps([0]*88))
                        self.assertFalse(response.json()['ok']);self.assertEqual((local/'mii-profile.json').read_bytes(),before)
                        response=page.request.post(url+'/api/mii/resources',headers=headers,data=json.dumps({'directory':directory,'enabled':True}))
                        self.assertFalse(response.json()['ok']);self.assertFalse((local/'mii-resources.json').exists())
                        page.reload();page.get_by_role('button',name='Perfil Mii',exact=True).click()
                        expect(page.locator('#mii-name')).to_have_value('Luigi')
                        page.set_viewport_size({'width':780,'height':900})
                        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
                        self.assertFalse(errors,errors)
                        screenshot=ROOT/'local/performance-validation/mii-editor.png'
                        screenshot.parent.mkdir(parents=True,exist_ok=True)
                        page.screenshot(path=str(screenshot),full_page=True)
                        fallback=browser.new_page(locale='pt-BR')
                        fallback.add_init_script('''const original=HTMLCanvasElement.prototype.getContext;
                            HTMLCanvasElement.prototype.getContext=function(kind,...args){
                              return kind==='webgl'?null:original.call(this,kind,...args);
                            };''')
                        fallback.goto(url);fallback.get_by_role('button',name='Perfil Mii',exact=True).click()
                        expect(fallback.locator('#mii-preview-status')).to_contain_text('indisponível')
                        expect(fallback.locator('#mii-name')).to_have_value('Luigi')
                        fallback.locator('#mii-name').fill('SemGPU')
                        fallback.get_by_role('button',name='Salvar Mii',exact=True).click()
                        expect(fallback.locator('#notice')).to_contain_text('Perfil salvo')
                        fallback.close()
                    finally:
                        browser.close()
            finally:
                process.terminate();process.wait(timeout=15);process.stderr.close()


if __name__=='__main__':
    unittest.main()
