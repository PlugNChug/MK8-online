"""Exercise local GLB rendering and rejection with authored geometry, never user assets."""
import base64
import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import unittest
import zlib
from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]


def fixture():
    binary = struct.pack('<9f', -1, -1, 0, 1, -1, 0, 0, 1, 0)
    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(b'\0\xff\0\0\xff')) + chunk(b'IEND', b''))
    binary += png
    doc = {
        'asset': {'version': '2.0', 'extras': {'miiData': base64.b64encode(b'authored-data').decode()}},
        'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'name': 'miiHead', 'mesh': 0}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0}, 'material': 0}]}],
        'materials': [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}}}],
        'images': [{'mimeType': 'image/png', 'bufferView': 1}], 'textures': [{'source': 0}],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
                       'min': [-1, -1, 0], 'max': [1, 1, 0]}],
        'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 36},
                        {'buffer': 0, 'byteOffset': 36, 'byteLength': len(png)}],
        'buffers': [{'byteLength': len(binary)}],
    }
    return doc, binary


def encode(doc, binary):
    raw = json.dumps(doc).encode()
    raw += b' ' * (-len(raw) % 4)
    binary += b'\0' * (-len(binary) % 4)
    return (struct.pack('<4sII', b'glTF', 2, 28 + len(raw) + len(binary))
            + struct.pack('<I4s', len(raw), b'JSON') + raw
            + struct.pack('<I4s', len(binary), b'BIN\0') + binary)


class MiiGlbTests(unittest.TestCase):
    def test_local_render_pairing_and_invalid_resources(self):
        with tempfile.TemporaryDirectory(prefix='mk8-glb-') as directory:
            root = Path(directory)
            exe = os.environ.get('MK8_LAUNCHER_EXE', str(ROOT/'target/debug/Launcher.exe'))
            child = subprocess.Popen([exe, '--headless', '--data-dir', directory],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline = time.monotonic() + 30
                while not (root/'launcher.json').exists():
                    self.assertIsNone(child.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.05)
                url = json.loads((root/'launcher.json').read_text())['url']
                with sync_playwright() as pw:
                    browser = pw.chromium.launch(headless=True)
                    try:
                        page = browser.new_page(locale='en-US')
                        errors, requests = [], []
                        page.on('pageerror', lambda error: errors.append(str(error)))
                        page.on('console', lambda msg: errors.append(msg.text) if msg.type == 'error' else None)
                        page.on('request', lambda req: requests.append(req.url))
                        page.goto(url+'/mii-model.html')
                        doc, binary = fixture()
                        def upload(data):
                            page.locator('#model-file').set_input_files({
                                'name': 'authored.glb', 'mimeType': 'model/gltf-binary', 'buffer': data})
                        upload(encode(doc, binary))
                        expect(page.locator('#model-canvas')).to_have_attribute('data-rendered', 'true', timeout=30000)
                        self.assertFalse(errors, errors)
                        expect(page.locator('#model-canvas')).to_have_attribute('data-rendered', 'true')
                        expect(page.locator('#model-info')).to_contain_text('"triangles": 1')
                        page.locator('#model-view').select_option('head')
                        page.locator('#model-wire').check()
                        page.locator('#model-bones').check()
                        for data, result in [(b'authored-data', 'Exact match'), (b'wrong', 'does not match')]:
                            page.locator('#model-data').set_input_files({
                                'name': 'fixture.miic', 'mimeType': 'application/octet-stream', 'buffer': data})
                            expect(page.locator('#model-match')).to_contain_text(result)
                        cases = []
                        bad = copy.deepcopy(doc); bad['buffers'][0]['uri'] = 'https://example.invalid/asset.bin'; cases.append(bad)
                        bad = copy.deepcopy(doc); bad['nodes'][0]['children'] = [0]; cases.append(bad)
                        bad = copy.deepcopy(doc); bad['accessors'][0]['count'] = 200001; cases.append(bad)
                        bad = copy.deepcopy(doc); bad['bufferViews'][0]['byteLength'] = 9999; cases.append(bad)
                        for bad in cases:
                            upload(encode(bad, binary))
                            expect(page.locator('#model-status')).to_contain_text('GLB:')
                            expect(page.locator('#model-canvas')).not_to_have_attribute('data-rendered', 'true')
                        upload(b'not a GLB')
                        expect(page.locator('#model-status')).to_contain_text('GLB:')
                        upload(encode(doc, binary))
                        expect(page.locator('#model-canvas')).to_have_attribute('data-rendered', 'true')
                        self.assertFalse(errors, errors)
                        self.assertTrue(all(r.startswith(url+'/') or r.startswith('blob:') for r in requests), requests)
                        self.assertFalse((root/'mii-profile.json').exists())
                        self.assertFalse((root/'user-no-mii').exists())
                    finally:
                        browser.close()
            finally:
                child.terminate(); child.wait(timeout=15)


if __name__ == '__main__':
    unittest.main()
