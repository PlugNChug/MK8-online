"""MK8 Nextendo adaptation applied to a verified runtime build copy."""

from pathlib import Path
import shutil

def prepare(runtime):
    def edit(relative, old, new):
        path = runtime / relative
        text = path.read_text(encoding='utf-8')
        if text.count(old) != 1:
            raise ValueError('Nextendo boundary changed: ' + relative)
        path.write_text(text.replace(old, new), encoding='utf-8')

    def region(relative, begin, end, replacement):
        text = (runtime / relative).read_text(encoding='utf-8')
        if text.count(begin) != 1 or text.count(end) != 1:
            raise ValueError('Nextendo region changed: ' + relative)
        start, stop = text.index(begin), text.index(end)
        if start >= stop:
            raise ValueError('Nextendo region order changed: ' + relative)
        edit(relative, text[start:stop], replacement)

    source = Path(__file__).resolve().parents[1] / 'runtime/mk8/net'
    shutil.copyfile(source / 'social.h', runtime / 'net/mk8_social.h')
    shutil.copyfile(source / 'nextendo_ca.h', runtime / 'net/nextendo_ca.h')
    # Prepared headers participate in the host cache key, unlike .inc files.
    shutil.copyfile(source / 'social.inc', runtime / 'net/mk8_social_impl.h')
    edit('net/nextendo.c', '#define SCOPE "identity%20game.matchmaking"',
         '#define SCOPE ((getenv("MK8_MENU_PROTOTYPE") && !strcmp(getenv("MK8_MENU_PROTOTYPE"), "0")) ? "identity%20game.matchmaking" : "identity%20game.matchmaking%20friends")')
    edit('net/nextendo.c', 'void nx_test_reset(void) {',
         '#include "mk8_social_impl.h"\n\nvoid nx_test_reset(void) {')
    edit('net/nextendo.c', '#define CLIENT_ID "nxc_R-JWtJ6JrrgY"',
         '#define CLIENT_ID "nxc_5QmpWFAhTHSd"\nextern bool mk8_nextendo_enabled(void);')
    # Third-party integration contract: identify our registered public client on API requests.
    edit('net/nextendo.c', '{"/oauth/token", "/api/oauth/token"}',
         '{"/api/oauth/token", "/oauth/token"}')
    edit('net/nextendo.c', '"%s/oauth/authorize", api_base()',
         '"%s/api/oauth/authorize", api_base()')
    edit('net/nextendo.c',
         '"Content-Type: application/x-www-form-urlencoded\\r\\nAccept: application/json\\r\\n",',
         '"Content-Type: application/x-www-form-urlencoded\\r\\nAccept: application/json\\r\\nX-Nextendo-Client-Id: " CLIENT_ID "\\r\\n",')
    for token in ('S.access', 'tok'):
        edit('net/nextendo.c',
             '"Authorization: Bearer %s\\r\\nAccept: application/json\\r\\n", ' + token,
             '"Authorization: Bearer %s\\r\\nAccept: application/json\\r\\nX-Nextendo-Client-Id: " CLIENT_ID "\\r\\n", ' + token)
    edit('net/nextendo.c', 'static void random_bytes(uint8_t *out, size_t n) {',
         'static bool random_bytes(uint8_t *out, size_t n) {')
    edit('net/nextendo.c', '    if (mbedtls_ctr_drbg_seed(&d, mbedtls_entropy_func, &e, (const unsigned char *)pers, sizeof pers) ||\n        mbedtls_ctr_drbg_random(&d, out, n)) {\n        for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(rand() ^ (int)time(NULL) >> (i & 7));  // (no entropy: weak)\n    }',
         '    bool ok = !mbedtls_ctr_drbg_seed(&d, mbedtls_entropy_func, &e, (const unsigned char *)pers, sizeof pers) &&\n        !mbedtls_ctr_drbg_random(&d, out, n);')
    edit('net/nextendo.c', '    mbedtls_entropy_free(&e);\n}', '    mbedtls_entropy_free(&e);\n    return ok;\n}')
    edit('net/nextendo.c', '    random_bytes(rnd, sizeof rnd);', '''    if (!mk8_nextendo_enabled() || !random_bytes(rnd, sizeof rnd)) {
        pthread_mutex_lock(&lock);
        set_state(NX_ERROR, "Enable MK8_NEXTENDO_TEST=1; secure randomness is required.");
        pthread_mutex_unlock(&lock);
        return false;
    }''')
    # Tokens and signing keys must not land in a save folder or support archive.
    edit('net/token.c', '    jwt_ok = jwt_key_read() || jwt_key_make();', '    jwt_ok = jwt_key_make();')
    edit('net/token.c', '    uint8_t d[8];\n    random_bytes(d, sizeof d);',
         '    uint8_t d[8];\n    if (mbedtls_ctr_drbg_random(&jwt_drbg, d, sizeof d)) return false;')
    region('net/token.c', 'static bool jwt_key_read(void)', 'static bool jwt_key_make(void)', '')
    region('net/token.c', '    static unsigned char pem[4096];', 'static void jwt_load(void)',
           '    return true; // Test signing key stays in memory.\n}\n\n')
    edit('net/token.c', 'static void random_bytes(uint8_t *out, size_t n) {', 'static bool random_bytes(uint8_t *out, size_t n) {')
    edit('net/token.c', '    if (mbedtls_ctr_drbg_seed(&d, mbedtls_entropy_func, &e, (const unsigned char *)pers, sizeof pers) ||\n        mbedtls_ctr_drbg_random(&d, out, n)) {\n        for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(rand() ^ (int)time(NULL) >> (i & 7));  // (no entropy: weak)\n    }',
         '    bool ok = !mbedtls_ctr_drbg_seed(&d, mbedtls_entropy_func, &e, (const unsigned char *)pers, sizeof pers) &&\n        !mbedtls_ctr_drbg_random(&d, out, n);')
    edit('net/token.c', '    mbedtls_entropy_free(&e);\n}', '    mbedtls_entropy_free(&e);\n    return ok;\n}')
    edit('net/token.c', '    random_bytes((uint8_t *)&nsa_id, sizeof nsa_id);\n    random_bytes(key, sizeof key);',
         '    if (!random_bytes((uint8_t *)&nsa_id, sizeof nsa_id) || !random_bytes(key, sizeof key)) return;')
    edit('net/token.c', 'static void uuid4(char out[37]) {', 'static bool uuid4(char out[37]) {')
    edit('net/token.c', '    random_bytes(b, sizeof b);', '    if (!random_bytes(b, sizeof b)) return false;')
    edit('net/token.c', 'b[12], b[13], b[14], b[15]);\n}', 'b[12], b[13], b[14], b[15]);\n    return true;\n}')
    edit('net/token.c', '    uuid4(jti);', '    if (!uuid4(jti)) return 0;')
    edit('net/token.c', '    random_bytes(jti, sizeof jti);', '    if (!have_key || !random_bytes(jti, sizeof jti)) return 0;')
    edit('main.c', '    plat_init();', '''    if (argc == 2 && !strcmp(argv[1], "--nextendo-auth-test")) {
        extern int mk8_nextendo_probe(bool);
        return mk8_nextendo_probe(false);
    }
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--nextendo-login-play")) {
            extern int mk8_nextendo_probe(bool);
            int result = mk8_nextendo_probe(true);
            if (result) return result;
            break;
        }
    }
    plat_init();''')
    edit('ui/online_rows.mm', 'extern "C" bool online_rows_available(void) { return true; }',
         'extern "C" bool mk8_nextendo_enabled(void);\nextern "C" bool online_rows_available(void) { return mk8_nextendo_enabled(); }')
    edit('ui/online_rows.mm', '    opt_section("Online");',
         '    if (!mk8_nextendo_enabled()) return;\n    opt_section("Nextendo - experimental / session only");')
    edit('ui/online_rows.mm', '    if (e && *e == \'1\' && !nextendo_signed_in()) sign_in();',
         '    if (mk8_nextendo_enabled() && e && *e == \'1\' && !nextendo_signed_in()) sign_in();')
    edit('ui/online_rows.mm', 'it "\n             "keeps a sign-in token for this device.',
         'the launcher remembers "\n             "this sign-in while it is open.')
    edit('ui/online_rows.mm', '    if (st.state == NX_SIGNED_IN &&', '''    if (st.state == NX_WAITING_BROWSER && opt_action("Cancel sign-in", "Cancel", "Close the local login listener."))
        nextendo_sign_out();
    if (st.state == NX_SIGNED_IN &&''')
    edit('net/netcfg.c', '    backend_eff = backend_booted = backend_set;', '''    extern bool mk8_nextendo_enabled(void);
    if (backend_set == NET_BACKEND_NEXTENDO && !mk8_nextendo_enabled()) backend_set = NET_BACKEND_OURS;
    backend_eff = backend_booted = backend_set;''')
    # Sensitive NEX traffic must never be included in support logs, even with debug env flags.
    edit('net/netcfg.c', 'bool net_log_data(void) { init(); return log_data_on; }',
         'bool net_log_data(void) { init(); return backend_set != NET_BACKEND_NEXTENDO && log_data_on; }')
    edit('net/netcfg.h',
         'bool net_tls_pin(uint8_t out[32]);     // NET_TRUST_PINNED: the server certificate\'s SPKI SHA-256 to require',
         'bool net_tls_pin(uint8_t out[32]);     // NET_TRUST_PINNED: the server certificate\'s SPKI SHA-256 to require\n'
         'bool net_tls_pin_for_host(const char *host, uint8_t out[32]);')
    edit('net/netcfg.c', 'static uint8_t pin[32];',
         'static uint8_t pin[32], znc_pin[32];\nstatic bool have_znc_pin;')
    edit('net/netcfg.c',
         '        if (cfg("RECOMP_NEXTENDO_TLS_PIN", "nextendo_tls_pin", v, sizeof v) && !parse_pin(v, pin))\n'
         '            LOG("[net] nextendo_tls_pin: 64 hex digits wanted (the SPKI SHA-256); using the built-in pin\\n");',
         '        if (cfg("RECOMP_NEXTENDO_TLS_PIN", "nextendo_tls_pin", v, sizeof v) && !parse_pin(v, pin))\n'
         '            LOG("[net] nextendo_tls_pin: 64 hex digits wanted (the SPKI SHA-256); using the built-in pin\\n");\n'
         '        have_znc_pin = cfg("RECOMP_NEXTENDO_ZNC_TLS_PIN", "nextendo_znc_tls_pin", v, sizeof v) && parse_pin(v, znc_pin);')
    edit('net/netcfg.c', '    memset(pin, 0, sizeof pin);',
         '    memset(pin, 0, sizeof pin);\n    memset(znc_pin, 0, sizeof znc_pin);\n    have_znc_pin = false;')
    edit('net/netcfg.c',
         'bool net_tls_pin(uint8_t out[32]) {\n    init();\n    if (trust != NET_TRUST_PINNED) return false;\n    memcpy(out, pin, 32);\n    return true;\n}',
         'bool net_tls_pin(uint8_t out[32]) {\n    init();\n    if (trust != NET_TRUST_PINNED) return false;\n    memcpy(out, pin, 32);\n    return true;\n}\n'
         'bool net_tls_pin_for_host(const char *host, uint8_t out[32]) {\n    init();\n    if (trust != NET_TRUST_PINNED) return false;\n'
         '    if (have_znc_pin && host && !strcmp(host, "api-lp1.znc.srv.nintendo.net")) {\n        memcpy(out, znc_pin, 32);\n        return true;\n    }\n'
         '    memcpy(out, pin, 32);\n    return true;\n}')
    edit('services/ssl.c', '    bool alpn_asked;', '    bool alpn_asked, require_pin;\n    uint8_t expected_pin[32];')
    edit('services/ssl.c',
         '        uint8_t pin[32];\n        if (!net_tls_pin(pin)) {',
         '        c->require_pin = true;\n        if (!net_tls_pin_for_host(c->sni, c->expected_pin)) {')
    edit('services/ssl.c',
         '    uint8_t pin[32];\n    if (net_tls_trust() == NET_TRUST_PINNED && (!net_tls_pin(pin) || !ssl_pin_check(mbedtls_ssl_get_peer_cert(&c->ssl), pin))) {',
         '    if (c->require_pin && !ssl_pin_check(mbedtls_ssl_get_peer_cert(&c->ssl), c->expected_pin)) {')
    edit('services/ssl.c', '#include "mbedtls/x509_crt.h"', '''#include "mbedtls/x509_crt.h"
#include <stdbool.h>
#include "../net/nextendo_ca.h"
static bool nextendo_uses_ca(const char *sni) {
    const char *community = getenv("MK8_NEXTENDO_COMMUNITY");
    return community && !strcmp(community, "1") && strcmp(sni, "g2b309e01-lp1.s.n.srv.nintendo.net");
}''')
    edit('services/ssl.c', '    } else if (trust == NET_TRUST_PINNED) {', '''    } else if (trust == NET_TRUST_PINNED && nextendo_uses_ca(c->sni)) {
        if (!c->sni[0] || is_ip(c->sni) ||
            mbedtls_x509_crt_parse(&c->ca, nextendo_ca_pem, sizeof nextendo_ca_pem)) return false;
        mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else if (trust == NET_TRUST_PINNED) {''')
    edit('services/ssl.c', '    mbedtls_ssl_set_hostname(&c->ssl, c->sni[0] && !is_ip(c->sni) ? c->sni : NULL);',
         '    if (mbedtls_ssl_set_hostname(&c->ssl, c->sni[0] && !is_ip(c->sni) ? c->sni : NULL)) return false;')
    edit('net/netcfg.c', '        add_rule(NEXTENDO_NAT_HOST,', '''        if (getenv("MK8_NEXTENDO_COMMUNITY") && !strcmp(getenv("MK8_NEXTENDO_COMMUNITY"), "1"))
            add_rule("g2b309e01-lp1.s.n.srv.nintendo.net", nx_nat, "Nextendo MK8", true);
        add_rule(NEXTENDO_NAT_HOST,''')
    region('net/netcfg.c', '#define OPTS(nextendo_available)', '    static NetBackendOption signed_out[]', '''#define OPTS(available) { \\
        {NET_BACKEND_OURS, "ours", "Offline", "MK8 Online Play is disabled. Launcher LAN rooms use a separate transport.", true}, \\
        {NET_BACKEND_LOCAL_NEX, "local-nex", "Local test server", "Developer NEX server on the local network.", false}, \\
        {NET_BACKEND_NEXTENDO, "nextendo", "Nextendo Network", "Experimental MK8 4.0.1 online service. Login, lobby and racing require separate validation. Sign in below.", available}, \\
        {NET_BACKEND_OCW, "ocw", "OpenCourse World", "Not supported for MK8.", false} }
''')
