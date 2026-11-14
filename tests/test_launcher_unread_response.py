"""A large response the page has not read yet must not stop the launcher from answering other requests.

The Mii creator reads the parts library only after FFL's wasm arrives from the same launcher (issues #15/#16).
This drives the real launcher over sockets: one connection asks for a large public asset and does not read it."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


def send(host, port, path, rcvbuf=None, timeout=10):
    s = socket.socket()
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.settimeout(timeout)
    s.connect((host, port))
    s.sendall(f'GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n'.encode())
    return s


def read_all(s):
    data = b''
    while chunk := s.recv(65536):
        data += chunk
    s.close()
    return data


class UnreadResponseTests(unittest.TestCase):
    def test_unread_large_body_does_not_block_the_next_request(self):
        exe = os.environ.get('MK8_LAUNCHER_EXE', str(ROOT / 'target/release/Launcher.exe'))
        with tempfile.TemporaryDirectory(prefix='mk8-unread-') as directory:
            local = Path(directory)
            process = subprocess.Popen([exe, '--headless', '--data-dir', str(local)], stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL,
                                       creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            try:
                deadline = time.monotonic() + 30
                while not (local / 'launcher.json').exists():
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.05)
                host, port = json.loads((local / 'launcher.json').read_text())['url'].split('//')[1].split(':')
                port = int(port)
                stalled = send(host, port, '/vendor/three/three.core.js', rcvbuf=4096)
                time.sleep(1)  # (the launcher is writing that body now)
                started = time.monotonic()
                try:
                    wasm = read_all(send(host, port, '/vendor/ffl/ffl-emscripten.wasm', timeout=5))
                except socket.timeout:
                    self.fail('the launcher stopped answering while a large response was unread')
                self.assertLess(time.monotonic() - started, 5)
                self.assertTrue(wasm.startswith(b'HTTP/1.1 200'))
                self.assertIn(b'\0asm', wasm)
                body = read_all(stalled)
                head, content = body.split(b'\r\n\r\n', 1)
                self.assertTrue(head.startswith(b'HTTP/1.1 200'))
                self.assertIn(b'transfer-encoding: chunked', head.lower())
                served = b''
                while True:
                    size, content = content.split(b'\r\n', 1)
                    size = int(size, 16)
                    if not size:
                        break
                    served, content = served + content[:size], content[size + 2:]
                self.assertGreater(len(served), 256 * 1024)  # (larger than the socket buffers on both ends)
                self.assertIn(b'three', served[-4096:].lower() + served[:4096].lower())
            finally:
                process.kill()
                process.wait()


if __name__ == '__main__':
    unittest.main()
