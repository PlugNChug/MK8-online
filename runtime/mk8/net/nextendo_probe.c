#include <stdio.h>
#include <windows.h>
#include "net/nextendo.h"
#include "net/netcfg.h"
#include "net/token.h"
#include <string.h>

extern void game_net_config_set(const char *, const char *);

// CLI probe runs before data-path/game initialization and never reads game files.
int mk8_nextendo_probe(bool keep_session) {
    char url[1024];
    if (!nextendo_begin_sign_in(NULL, url, sizeof url)) {
        NxStatus status; nextendo_status(&status);
        fprintf(stderr, "Nextendo: %s\n", status.message);
        return 1;
    }
    puts(url); fflush(stdout); // PKCE challenge only, never the verifier or callback code.
    for (int i = 0; i < 3100; ++i) {
        NxStatus status; nextendo_status(&status);
        if (status.state == NX_SIGNED_IN) {
            bool ok = nextendo_prepare_game_login(15000);
            puts(ok ? "OAuth and NEX token validated; gameplay not verified." : "OAuth accepted; NEX token unavailable.");
            if (ok) {
                game_net_config_set("backend", "nextendo");
                net_config_reload();
                char jwt[4096];
                ok = net_backend() == NET_BACKEND_NEXTENDO && net_id_token(jwt, sizeof jwt) > 0;
                SecureZeroMemory(jwt, sizeof jwt);
                puts(ok ? "BAAS adapter generated a token; server acceptance not verified." : "BAAS adapter unavailable or online access refused.");
            }
            if (!ok || !keep_session) nextendo_sign_out();
            return ok ? 0 : 2;
        }
        if (status.state == NX_ERROR) {
            fprintf(stderr, "Nextendo: %s\n", status.message);
            nextendo_sign_out();
            return 1;
        }
        Sleep(100);
    }
    nextendo_sign_out();
    return 3;
}
