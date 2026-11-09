"""Parallel and ranged downloads put back exactly the served bytes, and fall back to the next mirror."""
import http.server
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from windows_file_lock import locked_file

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import downloads

DATA = bytes(range(256)) * (40 * 1024 + 7)  # ~10 MB: above the size where a file is split into ranges


class Handler(http.server.BaseHTTPRequestHandler):
    ranges = True
    requests = []

    def log_message(self, *args):
        pass

    def head(self, body=False):
        Handler.requests.append((self.command, self.path, self.headers.get('Range')))
        if self.path != '/file.bin':
            self.send_error(404)
            return
        data, status = DATA, 200
        wanted = self.headers.get('Range') if self.ranges else None
        if wanted:
            first, last = map(int, wanted.removeprefix('bytes=').split('-'))
            data, status = DATA[first:last + 1], 206
        self.send_response(status)
        if self.ranges:
            self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        if body:
            self.wfile.write(data)

    def do_HEAD(self):
        self.head()

    def do_GET(self):
        self.head(body=True)


@unittest.skipUnless(os.name == 'nt', 'curl.exe do Windows')
class DownloadTests(unittest.TestCase):
    def test_locked_destination_preserves_previous_and_downloaded_bytes(self):
        self.target.write_bytes(b'previous download')
        with locked_file(self.target):
            with self.assertRaisesRegex(OSError, 'publicar o download'):
                downloads.fetch([self.base+'/file.bin', self.base+'/file.bin'], self.target,
                                sha256=hashlib.sha256(DATA).hexdigest())
            self.assertEqual(self.target.read_bytes(), b'previous download')
            self.assertEqual(self.target.with_name('file.bin.pending').read_bytes(), DATA)
            self.assertEqual(len(Handler.requests), 1, 'A local lock must not retry a mirror')
        downloads.fetch(self.base+'/file.bin', self.target, sha256=hashlib.sha256(DATA).hexdigest())
        self.assertEqual(self.target.read_bytes(), DATA)

    def setUp(self):
        Handler.ranges, Handler.requests = True, []
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = f'http://127.0.0.1:{self.server.server_port}'
        self.folder = tempfile.TemporaryDirectory(prefix='mk8-download-')
        self.target = Path(self.folder.name) / 'file.bin'

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.folder.cleanup()

    def test_ranges_are_joined_in_order_and_parts_removed(self):
        downloads.fetch(self.base + '/file.bin', self.target, parts=4)
        self.assertEqual(self.target.read_bytes(), DATA)
        self.assertEqual(sum(1 for r in Handler.requests if r[2]), 4)
        self.assertEqual(sorted(p.name for p in self.target.parent.iterdir()), ['file.bin'])

    def test_server_without_ranges_is_fetched_whole(self):
        Handler.ranges = False
        downloads.fetch(self.base + '/file.bin', self.target, parts=4)
        self.assertEqual(self.target.read_bytes(), DATA)
        self.assertFalse(any(r[2] for r in Handler.requests))

    def test_next_mirror_after_a_failure_and_error_when_all_fail(self):
        downloads.fetch([self.base + '/missing.bin', self.base + '/file.bin'], self.target)
        self.assertEqual(self.target.read_bytes(), DATA)
        self.target.unlink()
        with self.assertRaisesRegex(ValueError, 'Falha no download'):
            downloads.fetch([self.base + '/missing.bin'], self.target)
        self.assertEqual(list(self.target.parent.iterdir()), [])

    def test_several_files_at_once(self):
        jobs = [([self.base + '/file.bin'], self.target.with_name(f'{i}.bin')) for i in range(5)]
        downloads.fetch_all(jobs, workers=3)
        self.assertTrue(all(target.read_bytes() == DATA for _, target in jobs))

    def test_verified_download_repairs_bad_cache_and_reuses_good_cache(self):
        expected = hashlib.sha256(DATA).hexdigest()
        self.target.write_bytes(b'interrupted download')
        downloads.fetch_all([(self.base + '/file.bin', self.target, expected)])
        self.assertEqual(self.target.read_bytes(), DATA)
        requests = len(Handler.requests)
        downloads.fetch(self.base + '/missing.bin', self.target, sha256=expected)
        self.assertEqual(len(Handler.requests), requests)
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            downloads.fetch(self.base + '/file.bin', self.target, sha256='0' * 64)
        self.assertEqual(self.target.read_bytes(), DATA)
        self.assertFalse(self.target.with_name('file.bin.pending').exists())


if __name__ == '__main__':
    unittest.main()
