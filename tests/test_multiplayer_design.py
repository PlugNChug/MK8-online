"""Debug multiplayer UI; Steam responses/actions are intercepted, never sent to real friends."""
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
class MultiplayerDesignTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mk8-mp-design-')
        self.addCleanup(self.temp.cleanup)
        self.local = Path(self.temp.name)
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
        self.errors, self.actions = [], []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.friends = dict(persona='Piloto de teste', room=None, invites=[dict(lobby='123',friend='Ana')], friends=[
            dict(id='11',name='Ana',online=True,presence='online',room='123'),
            dict(id='22',name='Bruno',online=True,presence='busy',room=None),
            dict(id='33',name='<b>Caio</b>',online=False,presence='offline',room=None),
            dict(id='44',name='Duda',online=False,presence='offline',room='456'),
        ])
        self.offline = False
        self.page.route('**/api/steam/friends', lambda route: route.fulfill(json=
            {'ok':False,'error':'Steam indisponível neste teste.'} if self.offline else {'ok':True,'data':self.friends}))
        for endpoint in ('steam/host', 'steam/join', 'steam/dismiss', 'steam/invite', 'session/stop', 'session/steam-overlay-invite'):
            self.page.route('**/api/'+endpoint, self.action)
        self.page.goto(self.url+'/#session')
        expect(self.page.locator('#mp-friends-count')).to_have_text('4', timeout=15000)
        token = self.page.locator('meta[name="mk8-token"]').get_attribute('content')
        created = self.page.request.post(self.url+'/api/mii/create', headers={'X-MK8-Token':token},
                                         data={'name':'Piloto','color':0,'gender':0})
        self.assertTrue(created.json()['ok'])
        self.status = self.page.request.get(self.url+'/api/status', headers={'X-MK8-Token':token}).json()['data']
        self.page.route('**/api/status', lambda route: route.fulfill(json={'ok':True,'data':self.status}))

    def action(self, route):
        self.actions.append((route.request.url.split('/api/')[1], route.request.post_data_json))
        route.fulfill(json={'ok':True,'data':{'message':'Ação interceptada no teste.'}})

    def tearDown(self):
        self.assertEqual(self.errors, [])

    def stop_server(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=10)

    def shot(self, name):
        folder = ROOT/'local/screenshots'
        folder.mkdir(exist_ok=True, parents=True)
        self.page.evaluate('scrollTo(0, 0)')
        self.page.screenshot(path=str(folder/f'multiplayer-{name}.png'), full_page=True, animations='disabled')

    def test_guide_modes_filters_and_join_preserve_action_payloads(self):
        page = self.page
        self.assertLess(page.locator('.mp-quick-guide').bounding_box()['y'], page.locator('.mp-player').bounding_box()['y'])
        for index in (1,2,0):
            page.locator(f'[data-mp-guide="{index}"]').click()
            expect(page.locator(f'[data-mp-guide="{index}"]')).to_have_attribute('aria-selected','true')
        page.locator('[data-mp-guide="0"]').focus()
        page.keyboard.press('ArrowRight')
        expect(page.locator('[data-mp-guide="1"]')).to_be_focused()
        self.assertEqual(self.actions, [])
        expect(page.locator('#steam-host')).to_be_visible()
        expect(page.locator('#steam-host-manual')).to_be_hidden()
        page.locator('#mp-host-mode [value="manual"]').check()
        page.locator('#steam-host-manual').click()
        page.wait_for_function('() => !busy')
        page.locator('#mp-host-mode [value="automatic"]').check()
        page.locator('#steam-host').click()
        page.wait_for_function('() => !busy')
        self.assertEqual(self.actions, [('steam/host',{'automatic':False}),('steam/host',{'automatic':True})])
        page.locator('[data-mp-filter="online"]').click()
        expect(page.locator('#steam-friends li:visible')).to_have_count(2)
        page.locator('[data-mp-filter="rooms"]').click()
        expect(page.locator('#steam-friends li:visible')).to_have_count(2)
        page.locator('#friend-filter').fill('Bruno')
        expect(page.locator('#mp-filter-empty')).to_be_visible()
        page.locator('[data-mp-filter="all"]').click()
        expect(page.locator('#steam-friends li:visible')).to_have_count(1)
        page.locator('#friend-filter').fill('Caio')
        expect(page.locator('#steam-friends li')).to_contain_text('<b>Caio</b>')
        expect(page.locator('#steam-friends li > span b')).to_have_count(0)
        page.locator('#friend-filter').fill('')
        expect(page.locator('#mp-rooms-count')).to_have_text('3')
        self.shot('01-friends')
        page.locator('#mp-social-rooms').click()
        page.locator('#steam-invites button', has_text='Aceitar').click()
        page.wait_for_function('() => !busy')
        self.assertEqual(self.actions[-1], ('steam/join',{'lobby':'123','players':1}))
        page.locator('#steam-invites button', has_text='Recusar').click()
        page.wait_for_function('() => !busy')
        self.assertEqual(self.actions[-1], ('steam/dismiss',{'lobby':'123'}))
        page.route('**/api/steam/join', lambda route: route.fulfill(json={'ok':False,'error':'DLC incompatível no teste.'}))
        page.locator('#steam-rooms li', has_text='Duda').get_by_role('button',name='Entrar',exact=True).click()
        expect(page.locator('#notice')).to_contain_text('DLC incompatível')
        self.shot('02-invites')
        page.set_viewport_size({'width':390,'height':844})
        for key in ('friends','rooms'):
            page.locator('#mp-social-'+key).click()
            self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.shot('03-mobile')

    def test_direct_guest_choices_follow_the_active_build_and_navigation_mode(self):
        page = self.page
        self.status.update(candidate=True)
        self.status['session'].update(direct_lan_guest_supported=True, steam_auto_join=True)
        page.evaluate('refresh()')
        for panel, selector, room, count in [
                ('rooms','#steam-invites','123',2), ('rooms','#steam-rooms','456',1),
                ('friends','#steam-friends','123',2)]:
            page.locator('#mp-social-'+panel).click()
            row = page.locator(selector+' li').filter(has_text='Duda' if room == '456' else 'Ana')
            row.get_by_role('button', name=f'Entrar ({count}P)', exact=True).click()
            page.wait_for_function('() => !busy')
            self.assertEqual(self.actions[-1], ('steam/join',{'lobby':room,'players':count}))
        page.set_viewport_size({'width':390,'height':844})
        page.locator('#mp-social-rooms').click()
        self.assertTrue(page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        for capable, automatic in [(True,False),(False,True)]:
            self.status['session'].update(direct_lan_guest_supported=capable,steam_auto_join=automatic)
            page.evaluate('refresh()')
            expect(page.locator('#steam-invites').get_by_role('button',name='Aceitar')).to_be_visible()
            expect(page.locator('#steam-rooms').get_by_role('button',name='Entrar (2P)')).to_have_count(0)
            if not capable:
                expect(page.locator('#steam-direct-upgrade')).to_be_visible()

    def test_hosting_offline_invites_and_steam_retry(self):
        page = self.page
        self.status['session'].update(hosting=True,steam_active=True,connected=True,slot=0,participants=1)
        self.friends['room'] = '999'
        page.evaluate('refresh()')
        page.evaluate('loadSteam()')
        expect(page.locator('#mp-host-mode')).to_be_hidden()
        expect(page.locator('#steam-room')).to_be_visible()
        page.locator('#friend-filter').fill('Caio')
        page.locator('#steam-friends').get_by_role('button',name='Convidar',exact=True).click()
        page.wait_for_function('() => !busy')
        self.assertEqual(self.actions[-1], ('steam/invite',{'friend':'33'}))
        page.locator('#friend-filter').fill('Ana')
        page.locator('#steam-friends').get_by_role('button',name='Entrar',exact=True).click()
        page.wait_for_function('() => !busy')
        self.assertEqual(self.actions[-1], ('steam/join',{'lobby':'123','players':1}))
        self.status['game_pid'] = 123
        page.evaluate('refresh()')
        expect(page.locator('#leave')).to_be_disabled()
        self.shot('04-hosting')
        self.offline = True
        page.locator('#mp-refresh').click()
        expect(page.locator('#steam-state')).to_contain_text('Steam indisponível')
        expect(page.locator('#mp-friends-count')).to_have_text('0')
        expect(page.locator('#steam-friends')).to_contain_text('Abra a Steam')
        expect(page.locator('#mp-refresh')).to_be_enabled()
        self.offline = False
        page.locator('#mp-refresh').click()
        expect(page.locator('#mp-friends-count')).to_have_text('4')
        expect(page.locator('#steam-state')).to_contain_text('Piloto de teste')


if __name__ == '__main__':
    unittest.main()
