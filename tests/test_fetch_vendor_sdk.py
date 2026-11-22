"""The vendor SDK fetcher fails closed and never leaves an unverified file."""
import hashlib
import os
import http.server
from pathlib import Path
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import fetch_vendor_sdk as sdk


PAYLOAD = bytes(range(256)) * 40 + b'seven!!'  # several read() calls with the chunk below
GOOD_SHA = hashlib.sha256(PAYLOAD).hexdigest()
BAD = PAYLOAD[:-1] + b'X'  # same size, different bytes: exercises the hash check


class Handler(http.server.BaseHTTPRequestHandler):
    files = {}
    requests = []

    def log_message(self, *args):
        pass

    def do_GET(self):
        Handler.requests.append(self.path)
        body = Handler.files.get(self.path)
        if body is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def config(base, payload=PAYLOAD, sha=None, size=None, name='artifact.bin'):
    license_text = b'fixture licence text'
    return {
        'title': 'fixture',
        'redistribution': 'none',
        'license': {
            'name': 'LICENSE.txt',
            'url': base + '/license',
            'size': len(license_text),
            'sha256': hashlib.sha256(license_text).hexdigest(),
            'spdx': 'LicenseRef-Fixture',
            'summary': 'fixture terms',
        },
        'artifacts': [{
            'name': name,
            'url': base + '/artifact',
            'size': len(payload) if size is None else size,
            'sha256': hashlib.sha256(payload).hexdigest() if sha is None else sha,
        }],
    }


class FetcherTests(unittest.TestCase):
    def setUp(self):
        self._chunk = sdk.CHUNK
        sdk.CHUNK = 1024  # exercise multi-chunk reads without a large fixture
        self.addCleanup(setattr, sdk, 'CHUNK', self._chunk)
        Handler.files = {
            '/license': b'fixture licence text',
            '/artifact': PAYLOAD,
        }
        Handler.requests = []
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = f'http://127.0.0.1:{self.server.server_port}'
        # The host's /tmp is a heavily used tmpfs; prefer the build tree when present.
        scratch = ROOT / 'target'
        self.folder = tempfile.TemporaryDirectory(
            prefix='mk8-vendor-sdk-', dir=str(scratch) if scratch.is_dir() else None)
        self.root = Path(self.folder.name)
        self.cfg = config(self.base)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.folder.cleanup()

    def test_license_gate_runs_before_any_network_access(self):
        with self.assertRaises(sdk.LicenseNotAccepted):
            sdk.fetch_vendor('acme', [], self.cfg, root=self.root, quiet=True)
        self.assertEqual(Handler.requests, [])
        self.assertEqual(list(self.root.iterdir()), [])

    def test_verified_download_is_placed_atomically_with_its_licence(self):
        paths = sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True)
        self.assertEqual([p.name for p in paths], ['artifact.bin'])
        vendor_dir = self.root / 'acme'
        self.assertEqual((vendor_dir / 'artifact.bin').read_bytes(), PAYLOAD)
        self.assertEqual((vendor_dir / 'LICENSE.txt').read_bytes(), b'fixture licence text')
        self.assertEqual(sorted(p.name for p in vendor_dir.iterdir()),
                         ['LICENSE.txt', 'artifact.bin'])
        self.assertFalse((vendor_dir / 'artifact.bin.part').exists())

    def test_second_run_reuses_verified_bytes_without_a_server(self):
        sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True)
        self.server.shutdown()
        self.server.server_close()
        Handler.requests = []
        sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True)
        self.assertEqual(Handler.requests, [])

    def test_bad_hash_removes_the_partial_and_keeps_the_previous_copy(self):
        sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True)
        target = self.root / 'acme' / 'artifact.bin'
        Handler.files['/artifact'] = BAD
        with self.assertRaisesRegex(sdk.VerificationError, 'SHA-256 mismatch'):
            sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True,
                             force=True)
        self.assertEqual(target.read_bytes(), PAYLOAD)
        self.assertFalse((target.parent / 'artifact.bin.part').exists())

    def test_a_differing_local_file_is_not_replaced_without_force(self):
        vendor_dir = self.root / 'acme'
        vendor_dir.mkdir()
        (vendor_dir / 'LICENSE.txt').write_bytes(b'fixture licence text')
        target = vendor_dir / 'artifact.bin'
        target.write_bytes(b'local edit')
        Handler.requests = []
        with self.assertRaisesRegex(sdk.VerificationError, '--force'):
            sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True)
        self.assertEqual(target.read_bytes(), b'local edit')
        self.assertEqual(Handler.requests, [])

    def test_truncated_download_fails_closed(self):
        Handler.files['/artifact'] = PAYLOAD[:100]
        with self.assertRaisesRegex(sdk.VerificationError, 'size mismatch'):
            sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True)
        self.assertFalse((self.root / 'acme' / 'artifact.bin').exists())

    def test_bad_licence_text_fails_before_the_artifacts(self):
        Handler.files['/license'] = b'fixture licence TEXT'
        with self.assertRaisesRegex(sdk.VerificationError, 'SHA-256 mismatch'):
            sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True)
        self.assertFalse((self.root / 'acme' / 'artifact.bin').exists())
        self.assertFalse((self.root / 'acme' / 'LICENSE.txt').exists())

    def test_check_reports_missing_then_ok(self):
        missing = sdk.check_vendor('acme', self.cfg, root=self.root)
        self.assertEqual([status for _, status in missing], ['missing: ' + str(p) for p in (
            self.root / 'acme' / 'LICENSE.txt', self.root / 'acme' / 'artifact.bin')])
        sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True)
        self.assertEqual([status for _, status in sdk.check_vendor('acme', self.cfg, self.root)],
                         ['ok', 'ok'])

    def test_only_fetches_the_named_artifact_and_keeps_its_licence(self):
        sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True,
                         only=['artifact.bin'])
        self.assertEqual(sorted(p.name for p in (self.root / 'acme').iterdir()),
                         ['LICENSE.txt', 'artifact.bin'])

    def test_manifest_paths_cannot_escape_the_vendor_directory(self):
        self.cfg['artifacts'][0]['name'] = '../escape.bin'
        with self.assertRaises(sdk.UnsafePath):
            sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True)
        self.assertFalse((self.root / 'escape.bin').exists())

    def test_remote_plain_http_is_refused(self):
        self.cfg['artifacts'][0]['url'] = 'http://example.com/artifact'
        with self.assertRaisesRegex(sdk.VendorSdkError, 'non-HTTPS'):
            sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True)

    def test_disk_reserve_is_enforced_before_downloading(self):
        class Usage:
            free = 0
        with self.assertRaises(sdk.NotEnoughSpace):
            sdk.fetch_vendor('acme', ['acme'], self.cfg, root=self.root, quiet=True,
                             disk_usage=lambda _: Usage())
        self.assertEqual(Handler.requests, [])

    @unittest.skipIf(os.name == 'nt', 'XDG paths are POSIX (the Linux FSR page); /data is not absolute on Windows')
    def test_xdg_data_home_and_home_fallback(self):
        self.assertEqual(sdk.data_root({'XDG_DATA_HOME': '/data'}),
                         Path('/data/mk8online'))
        self.assertEqual(sdk.data_root({'HOME': '/home/someone'}),
                         Path('/home/someone/.local/share/mk8online'))
        with self.assertRaises(sdk.VendorSdkError):
            sdk.data_root({'XDG_DATA_HOME': 'relative'})
        with self.assertRaises(sdk.UnsafePath):
            sdk.vendor_root('../outside', Path('/data'))

    def test_manifest_itself_is_pinned(self):
        # The shipped manifest is the security boundary: a missing size/hash or a
        # non-HTTPS URL would make the download unpinnable.
        for vendor, cfg in sdk.VENDORS.items():
            with self.subTest(vendor=vendor):
                for entry in [cfg['license'], *cfg['artifacts']]:
                    self.assertGreater(entry['size'], 0)
                    self.assertRegex(entry['sha256'], r'^[0-9a-f]{64}$')
                    self.assertTrue(entry['url'].startswith('https://'))
                self.assertTrue(cfg['license']['spdx'])
                self.assertIn('not bundle', cfg['redistribution'].replace('does not', 'not'))


if __name__ == '__main__':
    unittest.main()
