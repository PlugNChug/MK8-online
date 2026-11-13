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


@unittest.skipUnless(os.name == 'nt', 'Windows process lifetime')
class LifetimeTests(unittest.TestCase):
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
            source = ROOT/'local/native-tools/deps/packages/ucrt64/bin'/name
            if not source.is_file():
                self.skipTest('Prepared SDL/Vulkan required for the launch preflight')
            target = self.fixture.parent/name
            shutil.copyfile(source, target)
            manifest['files'][name] = identity(target)
        for source in (ROOT/'local/native-tools/deps/dlls').glob('*.dll'):
            target = self.fixture.parent/source.name
            shutil.copyfile(source, target)
            manifest['files'][source.name] = identity(target)
        # Refuse before spawning or rotating logs when the local loader is missing.
        (self.fixture.parent/'vulkan-1.dll').rename(self.fixture.parent/'vulkan-1.hidden')
        (data/'candidate.json').write_text(json.dumps(manifest))
        first, post = self.launcher(data)
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
