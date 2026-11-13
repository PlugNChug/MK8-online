"""Exercise the built native OAuth client with a loopback server, never real credentials."""
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import unittest
from urllib.parse import parse_qs, urlsplit, urlencode
from urllib.request import urlopen
from urllib.error import HTTPError

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == 'nt', 'Windows runtime required')
class NextendoTests(unittest.TestCase):
    def run_flow(self, scenario, login_play=False):
        candidate = json.loads((ROOT/'local/candidate.json').read_text())
        exe = candidate['executable']['path']
        calls = []
        class Api(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_POST(self):
                body = self.rfile.read(int(self.headers['Content-Length'])).decode()
                calls.append((self.path, parse_qs(body)))
                if scenario == 'server_error':
                    self.send_response(503); self.end_headers(); return
                payload = json.dumps({'access_token':'synthetic-access', 'refresh_token':'synthetic-refresh', 'expires_in':3600}).encode()
                self.send_response(200); self.send_header('Content-Length', str(len(payload))); self.end_headers(); self.wfile.write(payload)
            def do_GET(self):
                calls.append((self.path, self.headers.get('Authorization')))
                payload = json.dumps({'allow': True, 'nex_token': '' if scenario == 'no_nex' else 'synthetic-nex', 'pid':1800000001, 'username':'Fixture', 'expires_in':86400}).encode()
                self.send_response(200); self.send_header('Content-Length', str(len(payload))); self.end_headers(); self.wfile.write(payload)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Api)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix='mk8-nextendo-') as tmp:
                folder = Path(tmp)
                (folder/'save.dat').write_bytes(b'preserve-me')
                env = dict(os.environ, MK8_NEXTENDO_TEST='1', RECOMP_NEXTENDO_API=f'http://127.0.0.1:{server.server_port}')
                if scenario == 'disabled': env['MK8_NEXTENDO_TEST'] = '0'
                proc = subprocess.Popen([exe, '--nextendo-login-play' if login_play else '--nextendo-auth-test'], cwd=folder, env=env,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    if scenario == 'disabled':
                        output, errors = proc.communicate(timeout=15)
                        self.assertNotEqual(proc.returncode, 0)
                        self.assertFalse(calls)
                        return
                    lines = queue.Queue()
                    def read_url():
                        for line in proc.stdout:
                            if '/oauth/authorize?' in line:
                                lines.put(line[line.index('http'):].strip()); return
                        lines.put('')
                    threading.Thread(target=read_url, daemon=True).start()
                    url = lines.get(timeout=15).strip()
                    params = parse_qs(urlsplit(url).query)
                    self.assertTrue(url, 'Native probe ended before producing an authorization URL')
                    self.assertEqual(params['client_id'], ['nxc_5QmpWFAhTHSd'])
                    self.assertEqual(params['scope'], ['identity game.matchmaking'])
                    self.assertEqual(params['code_challenge_method'], ['S256'])
                    redirect = params['redirect_uri'][0]
                    self.assertEqual(urlsplit(redirect).hostname, '127.0.0.1')
                    reply = {'state': 'wrong' if scenario == 'wrong_state' else params['state'][0]}
                    reply.update({'error':'access_denied'} if scenario == 'denied' else {'code':'synthetic-code'})
                    try:
                        with urlopen(redirect+'?'+urlencode(reply), timeout=20) as response: response.read()
                    except HTTPError as error:
                        self.assertEqual(error.code, 400)
                    output, errors = proc.communicate(timeout=25)
                    self.assertEqual(proc.returncode, 0 if scenario == 'success' else 2 if scenario == 'no_nex' else 1, errors)
                    if scenario == 'success': self.assertIn('BAAS adapter generated a token', output)
                    if scenario in ('wrong_state', 'denied'):
                        self.assertFalse(calls)
                    else:
                        form = calls[0][1]
                        verifier = form['code_verifier'][0]
                        challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b'=').decode()
                        self.assertEqual(challenge, params['code_challenge'][0])
                        self.assertEqual(form['redirect_uri'], [redirect])
                        self.assertEqual(form['client_id'], params['client_id'])
                        self.assertNotIn('client_secret', form)
                        for secret in (verifier, 'synthetic-access', 'synthetic-refresh', 'synthetic-nex'):
                            self.assertNotIn(secret, output+errors)
                    self.assertEqual((folder/'save.dat').read_bytes(), b'preserve-me')
                    self.assertFalse((folder/'net').exists(), 'No token/key files may be created')
                    if login_play:
                        self.assertEqual({p.name for p in folder.iterdir()}, {'save.dat'},
                                         'Failed login must stop before game/data initialization')
                finally:
                    if proc.poll() is None: proc.kill(); proc.communicate()
        finally:
            server.shutdown(); server.server_close(); thread.join()

    def test_native_oauth_and_nex(self): self.run_flow('success')
    def test_rejects_wrong_state(self): self.run_flow('wrong_state')
    def test_denied_consent(self): self.run_flow('denied')
    def test_nex_failure_is_not_success(self): self.run_flow('no_nex')
    def test_server_error(self): self.run_flow('server_error')
    def test_disabled_by_default(self): self.run_flow('disabled')
    def test_login_play_stops_on_denial(self): self.run_flow('denied', login_play=True)
    def test_login_play_stops_without_nex(self): self.run_flow('no_nex', login_play=True)

    def test_per_host_tls_pin_separation(self):
        import sys
        sys.path.insert(0, str(ROOT / 'toolchain'))
        from native import DEFAULT_COMPILER, compiler_env
        from host import DEPS
        from nextendo import prepare as prepare_nextendo
        import shutil
        upstream = ROOT / 'upstream/smm2recomp/recomp/runtime'
        with tempfile.TemporaryDirectory(prefix='mk8-nextendo-pin-') as tmp:
            tmp_path = Path(tmp)
            shutil.copytree(upstream, tmp_path / 'runtime')
            prepare_nextendo(tmp_path / 'runtime')
            harness = tmp_path / 'pin_test.c'
            harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime/net/netcfg.h"
void rt_log(const char *fmt, ...) { (void)fmt; }
uint64_t plat_now_ns(void) { return 0; }
bool nextendo_signed_in(void) { return true; }
bool nextendo_online_status(int timeout_ms, bool *allow, char *reason, size_t cap) {
    (void)timeout_ms; (void)reason; (void)cap; *allow = true; return true;
}
bool mk8_nextendo_enabled(void) { return true; }
bool game_net_config(const char *k, char *out, size_t cap) {
    if (!strcmp(k, "backend") && cap > 8) { strcpy(out, "nextendo"); return true; }
    return false;
}
void game_net_config_set(const char *k, const char *v) { (void)k; (void)v; }
int main(void) {
    uint8_t game_pin[32], znc_pin[32], other_pin[32];
    _putenv("RECOMP_NEXTENDO_TLS_PIN=770551f13152f3d55370de6c039d3859cff0d39882f27c6c9fc6cfdb327d74b8");
    _putenv("RECOMP_NEXTENDO_ZNC_TLS_PIN=");
    net_config_reload();
    assert(net_tls_pin_for_host("g2b309e01-lp1.s.n.srv.nintendo.net", game_pin));
    assert(net_tls_pin_for_host("api-lp1.znc.srv.nintendo.net", znc_pin));
    assert(memcmp(game_pin, znc_pin, 32) == 0);
    assert(game_pin[0] == 0x77 && game_pin[1] == 0x05);

    _putenv("RECOMP_NEXTENDO_ZNC_TLS_PIN=eb22631902f276218368e59dfac73f656bf8391c356e6c37f079049db40d596f");
    net_config_reload();
    assert(net_tls_pin_for_host("g2b309e01-lp1.s.n.srv.nintendo.net", game_pin));
    assert(net_tls_pin_for_host("api-lp1.znc.srv.nintendo.net", znc_pin));
    assert(net_tls_pin_for_host("other.nintendo.net", other_pin));
    assert(game_pin[0] == 0x77 && game_pin[1] == 0x05);
    assert(znc_pin[0] == 0xeb && znc_pin[1] == 0x22);
    assert(memcmp(game_pin, other_pin, 32) == 0);
    assert(memcmp(game_pin, znc_pin, 32) != 0);

    _putenv("RECOMP_NEXTENDO_ZNC_TLS_PIN=invalid-short-pin");
    net_config_reload();
    assert(net_tls_pin_for_host("api-lp1.znc.srv.nintendo.net", znc_pin));
    assert(memcmp(game_pin, znc_pin, 32) == 0);
    return 0;
}
''', encoding='utf-8')
            exe = tmp_path / 'pin_test.exe'
            subprocess.check_call([
                str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-std=gnu11', '-O1', '-UNDEBUG',
                '-I' + str(DEPS / 'include'), '-L' + str(DEPS / 'lib'),
                str(harness), str(tmp_path / 'runtime/net/netcfg.c'),
                '-lwinpthread', '-lws2_32', '-o', str(exe)
            ], env=compiler_env())
            subprocess.check_call([str(exe)], env=compiler_env())


if __name__ == '__main__': unittest.main()
