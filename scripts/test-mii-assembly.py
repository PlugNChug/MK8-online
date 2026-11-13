"""Exercise the phase-2 composer with private, locally acquired dependencies."""
import functools
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import threading

from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from toolchain.mii_library import RESOURCE_SHA256

RESEARCH = ROOT/'local/mii-library-research'
FFL = RESEARCH/'FFL.js'
REVISION = '4b62d7e6bb7b5ba3932b5236a2c05a2ef1b02fec'


class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, '.cjs': 'text/javascript'}

    def log_message(self, *args):
        pass


def run():
    if shutil.disk_usage(tempfile.gettempdir()).free < 4*1024**3:
        raise RuntimeError('At least 4 GiB free required')
    resource = RESEARCH/'AFLResHigh_2_3.dat'
    assert hashlib.sha256(resource.read_bytes()).hexdigest() == RESOURCE_SHA256
    assert subprocess.check_output(['git', '-C', str(FFL), 'rev-parse', 'HEAD'], text=True).strip() == REVISION
    assert not subprocess.check_output(['git', '-C', str(FFL), 'status', '--porcelain'], text=True).strip()
    assert hashlib.sha256((RESEARCH/'GLTFExporter.js').read_bytes()).hexdigest() == '0dd325edc550303a2955012f7aa07988e6da8e7b86d4743454c87105cafe058b'
    output = ROOT/'local/mii-assembly-check'
    output.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mk8-assembly-') as folder:
        root = Path(folder)
        shutil.copyfile(ROOT/'toolchain/mii_assembly.html', root/'index.html')
        shutil.copyfile(resource, root/'resource.dat')
        for name in ['ffl.js', 'ffl-emscripten.cjs', 'ffl-emscripten.wasm', 'LICENSE', 'helpers/GeometryConverter.js', 'materials/FFLShaderMaterial.js']:
            dest = root/'ffl'/name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(FFL/name, dest)
        shutil.copytree(ROOT/'apps/launcher/web/vendor/three', root/'three')
        shutil.copyfile(RESEARCH/'GLTFExporter.js', root/'three/GLTFExporter.js')
        server = ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(root)))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with sync_playwright() as p:
                browser = p.chromium.launch(headless=True, args=['--enable-unsafe-swiftshader'])
                page = browser.new_page(viewport={'width': 1150, 'height': 1100})
                errors, requests, exports = [], [], []
                page.on('pageerror', lambda e: errors.append(str(e)))
                page.on('console', lambda m: errors.append(m.text) if m.type == 'error' else None)
                page.on('request', lambda r: requests.append(r.url))
                page.goto(f'http://127.0.0.1:{server.server_port}/', wait_until='networkidle')
                print('Initial:', page.locator('#status').inner_text(), flush=True)
                expect(page.locator('#status')).to_have_text('Modelo montado', timeout=30000)
                for variant in ('A', 'B'):
                    page.get_by_role('button', name='Perfil '+variant, exact=True).click()
                    expect(page.locator('#status')).to_have_text('Modelo montado')
                    info = json.loads(page.locator('#details').inner_text())
                    assert info['variant'] == variant
                    assert len(info['parts']) >= 5 and all(part['normal'] for part in info['parts'])
                    page.locator('canvas').screenshot(path=str(output/f'profile-{variant}.png'))
                    page.locator('canvas').evaluate('''c => {
                        const gl=c.getContext('webgl2');
                        window.assemblyPixels=new Uint8Array(c.width*c.height*4);
                        gl.readPixels(0,0,c.width,c.height,gl.RGBA,gl.UNSIGNED_BYTE,window.assemblyPixels);
                    }''')
                    with page.expect_download() as pending:
                        page.get_by_role('button', name='Exportar GLB').click()
                    dest = output/f'profile-{variant}.glb'
                    pending.value.save_as(dest)
                    data = dest.read_bytes()
                    assert struct.unpack_from('<4sII', data) == (b'glTF', 2, len(data))
                    size = struct.unpack_from('<I', data, 12)[0]
                    doc = json.loads(data[20:20+size])
                    assert len(doc['images']) >= 2 and len(doc['meshes']) >= 5
                    assert all('NORMAL' in prim['attributes'] for mesh in doc['meshes'] for prim in mesh['primitives'])
                    page.get_by_role('button', name='Ver GLB exportado').click()
                    expect(page.locator('#status')).to_have_text('GLB reaberto')
                    page.locator('canvas').screenshot(path=str(output/f'export-{variant}.png'))
                    difference = page.locator('canvas').evaluate('''c => {
                        const gl=c.getContext('webgl2'),pixels=new Uint8Array(c.width*c.height*4);
                        gl.readPixels(0,0,c.width,c.height,gl.RGBA,gl.UNSIGNED_BYTE,pixels);
                        let total=0;for(let i=0;i<pixels.length;i++)if(i%4!==3)total+=Math.abs(pixels[i]-window.assemblyPixels[i]);
                        return total/(c.width*c.height*3);
                    }''')
                    assert difference < 0.1, ('Export changed image colors', difference)
                    exports.append(dict(variant=variant, sha256=hashlib.sha256(data).hexdigest(), bytes=len(data), details=info,
                                        export_pixel_mean_error=difference))
                assert exports[0]['sha256'] != exports[1]['sha256']
                page.locator('#hair').fill('132')
                page.get_by_role('button', name='Aplicar cabelo').click()
                expect(page.locator('#status')).to_contain_text('fora de')
                errors = [e for e in errors if 'Cabelo fora de' not in e]
                page.locator('#hair').fill('12')
                page.get_by_role('button', name='Aplicar cabelo').click()
                expect(page.locator('#status')).to_have_text('Modelo montado')
                assert json.loads(page.locator('#details').inner_text())['fields']['hair'] == 12
                assert not errors, errors
                assert all(url.startswith(f'http://127.0.0.1:{server.server_port}/') or url.startswith('blob:') for url in requests), requests
                (output/'manifest.json').write_text(json.dumps(dict(ffl_revision=REVISION, resource_sha256=RESOURCE_SHA256,
                    exports=exports, external_requests=False, native_integration_verified=False), indent=2), encoding='utf-8')
                browser.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
    print('Assembly, distinct exports, bounded editing and local-only requests passed.')


if __name__ == '__main__':
    run()
