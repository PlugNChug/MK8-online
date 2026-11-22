"""Real launcher processes and an authored executable; no commercial game or saves."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get('MK8_NATIVE_TOOLS') or ROOT/'local/native-tools')
EXE = Path(os.environ.get('MK8_LAUNCHER_EXE', ROOT/'target/release/Launcher.exe'))
FIXTURE = r'''
use std::{env,fs,process::Command,thread,time::Duration};
fn main() {
    let args: Vec<_> = env::args().collect();
    if args.get(1).is_some_and(|s| s == "tool") {
        let mut child = Command::new(env::current_exe().unwrap()).args(["leaf", &args[2]]).spawn().unwrap();
        fs::write(std::path::Path::new(&args[2]).join("tool.pid"), std::process::id().to_string()).unwrap();
        child.wait().unwrap(); return;
    }
    if args.get(1).is_some_and(|s| s == "leaf") {
        fs::write(std::path::Path::new(&args[2]).join("leaf.pid"), std::process::id().to_string()).unwrap();
        loop { thread::sleep(Duration::from_millis(50)); }
    }
    // These inherited Steam filters must not hide the native runtime's controls.
    assert_eq!(env::var("SDL_GAMECONTROLLER_IGNORE_DEVICES").unwrap(), "");
    assert_eq!(env::var("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT").unwrap(), "");
    assert_eq!(env::var("SDL_GAMECONTROLLERCONFIG").unwrap(), "authored-custom-mapping");
    if env::var("MK8_TEST_INPUT_PROBE").is_ok() {
        assert_eq!(env::var("MK8_UI_LANGUAGE").unwrap(), env::var("MK8_SETTINGS_LANGUAGE").unwrap());
        let state = ["MK8_AUTO_LAN", "MK8_LAN_PLAYERS", "LARECOMP_LAN_RELAY", "LARECOMP_LAN_ROOM", "LARECOMP_LAN_SLOT", "MK8_SETTINGS_EXPERIMENT", "MK8_SETTINGS_LANGUAGE"]
            .map(|key| format!("{}={}", key, env::var(key).unwrap_or_default())).join("\n");
        fs::write("navigation.txt", state).unwrap();
        if env::var_os("MK8_PACK_PROBE").is_some() {
            fs::write("pack-path.txt",env::var("MK8_ROMFS_OVERLAY").unwrap()).unwrap();
            fs::write("menu-path.txt",env::var("MK8_SETTINGS_ROMFS").unwrap_or_default()).unwrap();
        }
        if args.iter().any(|arg| arg=="--nextendo-login-play") {
            assert!(env::var_os("MK8_SETTINGS_ROMFS").is_none());
            assert_eq!(env::var("MK8_ROMFS_OVERLAY").unwrap(), "");
            assert!(env::var_os("MK8_MENU_PROTOTYPE").is_none());
            fs::write("nextendo-theme.txt",fs::read("mk8d.ini").unwrap()).unwrap();
        }
        return;
    }
    fs::write("game.pid", std::process::id().to_string()).unwrap();
    while !std::path::Path::new("finish").exists() { thread::sleep(Duration::from_millis(50)); }
    fs::write("completed-save.bin", b"saved normally").unwrap();
}
'''

def wait_for(condition, seconds=20):
    until = time.monotonic()+seconds
    while time.monotonic() < until:
        value = condition()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError('Timed out waiting for process/fixture')


def alive(pid):
    dll = ctypes.WinDLL('kernel32', use_last_error=True)
    dll.OpenProcess.restype = ctypes.c_void_p
    dll.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    dll.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = dll.OpenProcess(0x100000, False, pid)
    if not handle:
        return False
    try:
        return dll.WaitForSingleObject(handle, 0) == 258
    finally:
        dll.CloseHandle(handle)


def system_language():
    """The launcher's default without language.json (apps/launcher/src/language.rs `effective`)."""
    primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
    return {0x16: 'pt-BR', 0x09: 'en'}.get(primary, 'es')


@unittest.skipUnless(os.name == 'nt', 'Windows process lifetime')
class LifetimeTests(unittest.TestCase):
    def probe_install(self, data):
        (data/'fixture-game/extracted/romfs').mkdir(parents=True)
        dummy = data/'synthetic-input'
        dummy.write_bytes(b'authored fixture')
        identity = lambda p: {'path':str(p), 'sha256':hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes':p.stat().st_size}
        manifest = {'schema':2, 'identity':'fixture', 'profile':'mk8d-smm2-experimental', 'menu_themes_supported':True, 'menu_light_themes_supported':True,
                    'direct_lan_guest_supported':True,
                    'gameplay_verified':False, 'executable':identity(self.fixture),
                    'files':{k:identity(dummy) for k in ['npdm','main','sdk','subsdk0','gamelib']},
                    'data_root':str(data/'fixture-game'), 'shader_root':str(data)}
        for name in ('vulkan-1.dll', 'SDL3.dll'):
            source = TOOLS/'deps/packages/ucrt64/bin'/name
            if not source.is_file():
                self.skipTest('Prepared SDL/Vulkan required for launch preflight')
            target = self.fixture.parent/name
            shutil.copyfile(source,target)
            manifest['files'][name]=identity(target)
        for source in (TOOLS/'deps/dlls').glob('*.dll'):
            target=self.fixture.parent/source.name
            shutil.copyfile(source,target)
            manifest['files'][source.name]=identity(target)
        saves=data/'user-no-mii'
        (saves/'save').mkdir(parents=True)
        request={'root':str(data/'root'), 'local':str(data), 'install':manifest,
                 'network':['127.0.0.1:47820',2], 'username':'Test','candidate':True}
        env=dict(os.environ,MK8_TEST_INPUT_PROBE='1',MK8_AUTO_LAN='host',MK8_GFX_DIAGNOSTICS='0',
                 SDL_GAMECONTROLLERCONFIG='authored-custom-mapping')
        env.pop('LARECOMP_AUDIO_WAV',None)
        def launch():
            return subprocess.run([str(EXE),'--game-supervisor'],input=json.dumps(request)+'\n',
                                  env=env,capture_output=True,text=True,timeout=30)
        return request,env,launch

    def test_manual_and_automatic_launch_preserve_network_and_clear_inherited_macro(self):
        data=self.base/'navigation'
        request,env,launch=self.probe_install(data)
        preference=data/'debug-options.json'
        ini=data/'user-no-mii/mk8d.ini'
        original=b'theme = NES\nvolume = 1.50\n[future]\nmarker = preserve\n'
        progress=data/'user-no-mii/save/progress.bin'
        progress.write_bytes(b'authored progress marker')
        cases=[(role,None,'1') for role in (None,'host','guest')]
        cases += [(None,True,'0'),(None,False,'1')]
        for role,native,inherited in cases:
            with self.subTest(role=role,native=native,inherited=inherited):
                ini.write_bytes(original)
                env['MK8_SETTINGS_EXPERIMENT']=inherited
                if native is not None:
                    preference.write_text(json.dumps({'graphics_capture':False,'audio_recording':False,
                                                      'native_settings':native}))
                else:
                    self.assertFalse(preference.exists())
                request['auto_lan']=role
                request.pop('browser_language',None)
                if role == 'guest':
                    request['browser_language']='es'
                if native is not None:
                    (data/'language.json').write_text(json.dumps('pt-BR'))
                result=launch()
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                lines=(data/'user-no-mii/navigation.txt').read_text().splitlines()
                self.assertEqual(lines,[f'MK8_AUTO_LAN={role or ""}', 'MK8_LAN_PLAYERS=',
                                        'LARECOMP_LAN_RELAY=127.0.0.1:47820',
                                        'LARECOMP_LAN_ROOM=MK8ONLINE', 'LARECOMP_LAN_SLOT=2',
                                        'MK8_SETTINGS_EXPERIMENT=',
                                        'MK8_SETTINGS_LANGUAGE='+('pt-BR' if native is not None else 'es' if role=='guest' else system_language())])
                expected=original.replace(b'theme = NES',b'theme = Native') if native else original
                self.assertEqual(ini.read_bytes(),expected)
                self.assertEqual(progress.read_bytes(),b'authored progress marker')

    def test_older_game_keeps_legacy_native_switch_and_refuses_new_ini(self):
        data=self.base/'legacy-native'
        request,env,launch=self.probe_install(data)
        manifest=request['install']
        saves=data/'user-no-mii'
        ini=saves/'mk8d.ini'
        original=b'theme = NES\nvolume = 1.50\n'
        preference=data/'debug-options.json'
        env['MK8_SETTINGS_EXPERIMENT']='0'
        manifest.pop('menu_light_themes_supported')
        manifest.pop('menu_themes_supported')
        manifest['files']['settings-layout']=manifest['files']['npdm']
        legacy={'graphics_capture':False,'audio_recording':False,'native_settings':True}
        preference.write_text(json.dumps(legacy))
        ini.write_bytes(original)
        result=launch()
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('MK8_SETTINGS_EXPERIMENT=1',(saves/'navigation.txt').read_text().splitlines())
        self.assertEqual(ini.read_bytes(),original)
        self.assertEqual(json.loads(preference.read_bytes()),legacy)
        # The old parser would overwrite the new value. Refuse before starting it.
        canonical=original.replace(b'theme = NES',b'theme = Native')
        ini.write_bytes(canonical)
        (saves/'navigation.txt').unlink()
        result=launch()
        self.assertNotEqual(result.returncode,0)
        self.assertFalse((saves/'navigation.txt').exists())
        self.assertEqual(ini.read_bytes(),canonical)
        manifest['menu_themes_supported']=True
        for value in (b'SimpleLight',b'NESLight'):
            light=original.replace(b'theme = NES',b'theme = '+value)
            ini.write_bytes(light)
            result=launch()
            self.assertNotEqual(result.returncode,0)
            self.assertIn('temas claros',result.stdout)
            self.assertFalse((saves/'navigation.txt').exists())
            self.assertEqual(ini.read_bytes(),light)
            self.assertEqual(json.loads(preference.read_bytes()),legacy)

    def test_ctgp_without_custom_menu_and_nextendo_launch_with_selected_theme(self):
        data=self.base/'pack-menu'
        request,env,launch=self.probe_install(data)
        saves=data/'user-no-mii'
        ini=saves/'mk8d.ini'
        pack=data/'ctgp/pack'
        (pack/'romfs/Course').mkdir(parents=True)
        (pack/'romfs/Course/fixture.bin').write_bytes(b'track')
        (pack/'manifest.json').write_text(json.dumps({'kind':'ctgp-deluxe','schema':1,
            'files':{'Course/fixture.bin':{'bytes':5}}}))
        (data/'ctgp.json').write_text('{"enabled":true}')
        request['install']['pack_supported']=True
        request.update(network=None)
        env.update(MK8_PACK_PROBE='1', MK8_SETTINGS_ROMFS='stale-layout', MK8_NEXTENDO_TEST='1',
                   RECOMP_NEXTENDO_TLS_PIN='0'*64, RECOMP_NEXTENDO_ZNC_TLS_PIN='1'*64)
        env.pop('MK8_MENU_PROTOTYPE',None)
        result=launch()
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(Path((saves/'pack-path.txt').read_text()),pack/'romfs')
        self.assertEqual((saves/'menu-path.txt').read_text(),'')
        request['nextendo']=True
        for theme in ('Native','Default','SimpleLight','NES','NESLight'):
            with self.subTest(nextendo=theme):
                current=f'theme = {theme}\nvolume = 1.50\n'.encode()
                ini.write_bytes(current)
                result=launch()
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertEqual((saves/'nextendo-theme.txt').read_bytes(),current)

    def test_steam_guest_direct_entry_requires_capable_game_and_relay(self):
        data=self.base/'direct-guest'
        request,env,launch=self.probe_install(data)
        manifest=request['install']
        saves=data/'user-no-mii'
        protected={saves/'mk8d.ini':b'[video]\naspect = original\n[future]\nmarker = preserve\n',
                   saves/'save/progress.bin':b'authored progress marker'}
        for path,content in protected.items():
            path.write_bytes(content)
        env['MK8_LAN_PLAYERS']='2'
        # Role, requested 2P, capable game, connected relay.
        cases=[('guest',True,True,True), ('guest',False,True,True), ('host',True,False,True), (None,True,True,True),
               ('guest',True,False,True), ('guest',True,True,False)]
        for role,two,capable,online in cases:
            with self.subTest(role=role,two=two,capable=capable,online=online):
                manifest['direct_lan_guest_supported']=capable
                request.update(network=['127.0.0.1:47820',2] if online else None, auto_lan=role, two_player_guest=two)
                result=launch()
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                lines=(saves/'navigation.txt').read_text().splitlines()
                expected_role=role if online and (role=='host' or capable) else None
                self.assertEqual(lines[:5],[f'MK8_AUTO_LAN={expected_role or ""}',
                                            f'MK8_LAN_PLAYERS={2 if two and expected_role=="guest" else ""}',
                                            'LARECOMP_LAN_RELAY='+('127.0.0.1:47820' if online else ''),
                                            'LARECOMP_LAN_ROOM='+('MK8ONLINE' if online else ''),
                                            'LARECOMP_LAN_SLOT='+('2' if online else '')])
                for path,content in protected.items():
                    self.assertEqual(path.read_bytes(),content)

    def test_python_tools_ignore_external_home_path_and_user_site(self):
        folder = self.base/'python-environment'
        folder.mkdir()
        (folder/'sibling.py').write_text('answer = 42')
        (folder/'probe.py').write_text('import os, sibling; assert sibling.answer == 42; '
            'assert not os.environ.get("PYTHONHOME"); assert not os.environ.get("PYTHONPATH"); '
            'import site; assert not site.ENABLE_USER_SITE; print("local imports work")')
        def wide(value):
            raw = str(value).encode('utf-16-le')
            return {'Windows': [int.from_bytes(raw[i:i+2], 'little') for i in range(0,len(raw),2)]}
        task = {'root': str(folder/'root'), 'local': str(folder/'data'), 'log': str(folder/'task.log'),
                'parent': os.getpid(), 'program': wide(sys.executable), 'args': [wide(folder/'probe.py')],
                'cwd': str(folder), 'env': [[wide('PYTHONHOME'), wide(folder/'absent')]]}
        env = dict(os.environ, PYTHONHOME=str(folder/'absent'), PYTHONPATH=str(folder/'foreign'),
                   PYTHONNOUSERSITE='0', PYTHONSAFEPATH='1', PYTHONPLATLIBDIR='missing-library')
        result = subprocess.run([str(EXE), '--task-supervisor'], input=json.dumps(task)+'\n',
                                capture_output=True, text=True, env=env, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr+(folder/'task.log').read_text())
        self.assertIn('local imports work', (folder/'task.log').read_text())

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mk8-lifetime-')
        cls.base = Path(cls.temp.name)
        source = cls.base/'fixture.rs'
        source.write_text(FIXTURE)
        cls.fixture = cls.base/'fixture.exe'
        subprocess.run(['rustc', str(source), '-o', str(cls.fixture)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def launcher(self, data):
        marker = data/'launcher.json'
        marker.unlink(missing_ok=True)
        env = dict(os.environ, SDL_GAMECONTROLLER_IGNORE_DEVICES='0x045e/0x028e',
                   SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT='0xffff/0xffff',
                   SDL_GAMECONTROLLERCONFIG='authored-custom-mapping')
        process = subprocess.Popen([str(EXE), '--headless', '--data-dir', str(data)], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, env=env)
        self.addCleanup(lambda: (process.terminate(), process.wait(timeout=10)) if process.poll() is None else None)
        def ready_url():
            try:
                return json.loads(marker.read_text())['url']
            except (FileNotFoundError, PermissionError, json.JSONDecodeError):
                return None  # The marker can exist while the launcher is still writing it.
        url = wait_for(ready_url)
        html = urllib.request.urlopen(url).read().decode()
        token = re.search(r'name="mk8-token" content="([^"]+)"', html)[1]
        def post(route, value=None):
            req = urllib.request.Request(url+route, data=json.dumps(value or {}).encode(),
                headers={'X-MK8-Token': token, 'Content-Type': 'application/json'})
            try:
                response = urllib.request.urlopen(req)
            except urllib.error.HTTPError as error:
                response = error
            with response:
                return json.load(response)
        return process, post

    def test_game_survives_launcher_and_blocks_other_instances_until_exit(self):
        data = self.base/'game-data'
        data.mkdir()
        saves = data/'user-no-mii'
        saves.mkdir()
        (saves/'existing-save.bin').write_bytes(b'original save')
        (data/'config.json').write_text('{}')
        extracted = data/'fixture-game/extracted/romfs'
        extracted.mkdir(parents=True)
        dummy = data/'synthetic-input'
        dummy.write_bytes(b'authored fixture')
        identity = lambda path: {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'bytes': path.stat().st_size}
        manifest = {'schema': 2, 'identity': 'fixture', 'profile': 'mk8d-smm2-experimental',
                    'gameplay_verified': False, 'executable': identity(self.fixture),
                    'files': {k: identity(dummy) for k in ['npdm','main','sdk','subsdk0','gamelib']},
                    'data_root': str(data/'fixture-game'), 'shader_root': str(data)}
        for name in ('vulkan-1.dll', 'SDL3.dll'):
            source = TOOLS/'deps/packages/ucrt64/bin'/name
            if not source.is_file():
                self.skipTest('Prepared SDL/Vulkan required for the launch preflight')
            target = self.fixture.parent/name
            shutil.copyfile(source, target)
            manifest['files'][name] = identity(target)
        for source in (TOOLS/'deps/dlls').glob('*.dll'):
            target = self.fixture.parent/source.name
            shutil.copyfile(source, target)
            manifest['files'][source.name] = identity(target)
        # Refuse before spawning or rotating logs when the local loader is missing.
        (self.fixture.parent/'vulkan-1.dll').rename(self.fixture.parent/'vulkan-1.hidden')
        (data/'candidate.json').write_text(json.dumps(manifest))
        first, post = self.launcher(data)
        self.assertTrue(post('/api/mii/create', {'name':'Pilot','color':0,'gender':0})['ok'])
        self.assertFalse(post('/api/play')['ok'])
        self.assertFalse((saves/'game.pid').exists())
        self.assertEqual((saves/'existing-save.bin').read_bytes(), b'original save')
        (self.fixture.parent/'vulkan-1.hidden').rename(self.fixture.parent/'vulkan-1.dll')
        self.assertTrue(post('/api/play')['ok'])
        wait_for((saves/'game.pid').exists)
        pid = int((saves/'game.pid').read_text())
        def finish_game():
            (saves/'finish').touch()
            wait_for(lambda: not alive(pid))
        self.addCleanup(finish_game)
        second, other = self.launcher(data)
        try:
            for route in ['/api/play', '/api/build', '/api/setup', '/api/config', '/api/update/install']:
                self.assertFalse(other(route)['ok'], route)
            self.assertEqual((data/'config.json').read_text(), '{}')
            first.terminate(); first.wait(timeout=10)
            self.assertTrue(alive(pid))
            self.assertFalse(other('/api/build')['ok'])
            check = subprocess.run([sys.executable, '-c',
                'from build_lock import exclusive_build; import sys\nwith exclusive_build(sys.argv[1]): pass', str(data)],
                cwd=ROOT/'toolchain', capture_output=True)
            self.assertNotEqual(check.returncode, 0, 'Direct CLI must not rebuild an active game')
            self.assertEqual((saves/'existing-save.bin').read_bytes(), b'original save')
        finally:
            (saves/'finish').touch()
            wait_for(lambda: not alive(pid))
            wait_for(lambda: other('/api/config')['ok'])
        self.assertEqual((saves/'completed-save.bin').read_bytes(), b'saved normally')
        second.terminate(); second.wait(timeout=10)

    def test_parent_exit_terminates_tool_and_grandchild(self):
        folder = self.base/'tools'
        folder.mkdir()
        parent = subprocess.Popen([sys.executable, '-c', 'import sys,os; sys.stdin.readline(); os._exit(7)'], stdin=subprocess.PIPE)
        # OsString's serde representation carries UTF-16 units on Windows.
        def wide(value):
            raw = str(value).encode('utf-16-le')
            return {'Windows': [int.from_bytes(raw[i:i+2], 'little') for i in range(0,len(raw),2)]}
        task = {'root': str(folder/'root'), 'local': str(folder/'data'), 'log': str(folder/'task.log'),
                'parent': parent.pid, 'program': wide(self.fixture), 'args': [wide('tool'), wide(folder)],
                'cwd': None, 'env': []}
        supervisor = subprocess.Popen([str(EXE), '--task-supervisor'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            supervisor.stdin.write(json.dumps(task)+'\n'); supervisor.stdin.close()
            self.assertEqual(supervisor.stdout.readline().strip(), 'READY')
            wait_for((folder/'leaf.pid').exists)
            pids = [int((folder/name).read_text()) for name in ['tool.pid','leaf.pid']]
            parent.stdin.write(b'exit\n'); parent.stdin.close(); parent.wait(timeout=10)
            supervisor.wait(timeout=10)
            for pid in pids:
                wait_for(lambda: not alive(pid))
            # Same resources can be claimed again after all descendants were terminated.
            task['parent'] = os.getpid()
            task['program'] = wide('cmd.exe')
            task['args'] = [wide('/c'), wide('exit 0')]
            retry = subprocess.run([str(EXE), '--task-supervisor'], input=json.dumps(task)+'\n',
                                   capture_output=True, text=True, timeout=10)
            self.assertEqual(retry.returncode, 0, retry.stdout+retry.stderr)
            self.assertEqual(retry.stdout.strip(), 'READY')
        finally:
            if parent.poll() is None:
                parent.stdin.close(); parent.wait(timeout=10)
            if supervisor.poll() is None:
                subprocess.run(['taskkill','/PID',str(supervisor.pid),'/T','/F'], capture_output=True)
                supervisor.wait(timeout=10)
            supervisor.stdout.close()
