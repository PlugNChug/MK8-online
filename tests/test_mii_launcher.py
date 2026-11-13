"""Mii creator with the real launcher, a browser and local profile files; no game, parts library or online account.

Without the parts library every option is still a button (numbered instead of drawn), so creation, editing and
saving are exercised here; scripts/test-mii-composer.py covers the drawn options and body with the private files."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from playwright.sync_api import sync_playwright, expect

ROOT=Path(__file__).resolve().parents[1]
PARTS=['Corpo','Rosto','Cabelo','Olhos','Sobrancelhas','Nariz','Boca','Barba','Óculos','Pinta']


class MiiLauncherTests(unittest.TestCase):
    def test_create_edit_switch_and_save_from_the_creator(self):
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
                        page=browser.new_page(locale='pt-BR',viewport={'width':1280,'height':900})
                        errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
                        page.route('**/api/mii/composer/download', lambda route: route.fulfill(json={
                            'ok':True,'data':{'state':'failed','detail':'Download indisponível no teste offline.'}}))
                        page.goto(url);page.get_by_role('button',name='Perfil Mii',exact=True).click()
                        token=page.locator('meta[name="mk8-token"]').get_attribute('content')
                        def export():
                            return page.request.get(url+'/api/mii/export',headers={'X-MK8-Token':token}).json()['data']['data']
                        # Only creation is on the screen: no profile yet, the library is asked for once, no game body.
                        expect(page.locator('#mii-save')).to_have_text('Criar Mii', timeout=30000)
                        expect(page.locator('#mii-tabs button')).to_have_text(PARTS)
                        expect(page.locator('#mii-library')).to_be_visible()
                        expect(page.locator('#mii-library-status')).to_have_text('Download indisponível no teste offline.')
                        expect(page.locator('#mii-library-download')).to_be_enabled()
                        expect(page.locator('#mii-save')).to_have_text('Criar Mii')
                        expect(page.locator('#mii-options')).to_contain_text('Dê um nome ao Mii')
                        for old in ('#mii-canvas','#mii-form','#mii-import','#mii-export','#mii-composer','#mii-resource-dir'):
                            expect(page.locator(old)).to_have_count(0)
                        page.locator('#mii-save').click()  # no name
                        expect(page.locator('#mii-status')).to_have_text('Use um nome de 1 a 10 caracteres.')
                        self.assertFalse((local/'mii-profile.json').exists())
                        page.locator('#mii-name').fill('João')
                        page.locator('#mii-save').click()
                        expect(page.locator('#mii-saved .mii-saved-item')).to_have_text(['João'])
                        expect(page.locator('#mii-save')).to_have_text('Salvar Mii')
                        first=json.loads((local/'mii-profile.json').read_text(encoding='utf-8'))
                        # Creating continues into the face; each option is a button, the current one pressed.
                        expect(page.locator('#mii-tabs [data-part="face"]')).to_have_attribute('aria-selected','true')
                        shapes=page.locator('#mii-options .mii-choice[data-offset="45"]')
                        expect(shapes).to_have_count(12)
                        shapes.nth(3).click()
                        expect(shapes.nth(3)).to_have_attribute('aria-pressed','true')
                        expect(page.locator('#mii-status')).to_have_text('Alterações não salvas.')
                        # Body: type, height and build, with the body preview following them.
                        page.locator('#mii-tabs [data-part="body"]').click()
                        page.locator('#mii-options .mii-choice[data-offset="40"][data-value="1"]').click()
                        page.locator('#mii-options input[data-offset="41"]').fill('120')
                        page.locator('#mii-options .mii-range').nth(1).get_by_role('button',name='+').click()
                        page.locator('#mii-tabs [data-part="mouth"]').click()
                        page.locator('#mii-options .mii-choice[data-offset="69"][data-value="20"]').click()
                        page.locator('#mii-name').fill('Ana')
                        page.locator('#mii-save').click()
                        expect(page.locator('#mii-status')).to_contain_text('Mii salvo')
                        data=export()
                        self.assertEqual((data[45],data[40],data[41],data[42],data[69]),(3,1,120,65,20))
                        stored=json.loads((local/'mii-profile.json').read_text(encoding='utf-8'))
                        self.assertEqual(stored['id'],first['id'])
                        self.assertEqual(stored['appearance']['name'],'Ana')
                        expect(page.locator('#mii-saved .mii-saved-item')).to_have_text(['Ana'])
                        # A second Mii, then back to the first one through the saved list.
                        page.locator('#mii-new').click()
                        expect(page.locator('#mii-save')).to_have_text('Criar Mii')
                        page.locator('#mii-name').fill('Luigi')
                        page.locator('#mii-save').click()
                        expect(page.locator('#mii-saved .mii-saved-item')).to_have_count(2)
                        self.assertEqual(export()[40],0)
                        page.locator('#mii-saved .mii-saved-item',has_text='Ana').click()
                        expect(page.locator('#mii-name')).to_have_value('Ana')
                        self.assertEqual(export()[69],20)
                        # Unsaved edits are not lost silently when switching.
                        page.locator('#mii-tabs [data-part="nose"]').click()
                        page.locator('#mii-options .mii-choice[data-offset="66"][data-value="5"]').click()
                        page.once('dialog',lambda d:d.dismiss())
                        page.locator('#mii-saved .mii-saved-item',has_text='Luigi').click()
                        expect(page.locator('#mii-name')).to_have_value('Ana')
                        # The body comes from the prepared game; without one the screen says so instead of failing.
                        expect(page.locator('#mii-status')).not_to_have_text('')
                        page.reload();page.get_by_role('button',name='Perfil Mii',exact=True).click()
                        expect(page.locator('#mii-name')).to_have_value('Ana')
                        page.set_viewport_size({'width':780,'height':900})
                        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
                        page.locator('#language').select_option('en')
                        expect(page.locator('#mii-tabs button').first).to_have_text('Body')
                        expect(page.locator('#mii-save')).to_have_text('Save Mii')
                        self.assertFalse(errors,errors)
                        screenshot=ROOT/'local/performance-validation/mii-editor.png'
                        screenshot.parent.mkdir(parents=True,exist_ok=True)
                        page.screenshot(path=str(screenshot),full_page=True)
                    finally:
                        browser.close()
            finally:
                process.terminate();process.wait(timeout=15);process.stderr.close()


if __name__=='__main__':
    unittest.main()
