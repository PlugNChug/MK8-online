#include <stdio.h>
#include <SDL3/SDL.h>
#include "mbedtls/platform_util.h"
#include "net/nextendo.h"
#include "net/netcfg.h"
#include "net/token.h"
#include "net/mk8_social.h"
#include <stdlib.h>
#include <string.h>

extern void game_net_config_set(const char *, const char *);
extern bool mk8_nextendo_enabled(void);

static bool open_login(bool open_browser) {
    char url[1024];
    if (!nextendo_begin_sign_in(NULL, url, sizeof url)) {
        NxStatus status; nextendo_status(&status);
        fprintf(stderr, "Nextendo: %s\n", status.message);
        return false;
    }
    puts(url); fflush(stdout); // PKCE challenge only, never the verifier or callback code.
    if (open_browser && !strncmp(url, "https://", 8))
        SDL_OpenURL(url);  // (portable: Windows and Linux)
    return true;
}

// CLI probe runs before data-path/game initialization and never reads game files.
int mk8_nextendo_probe(bool keep_session) {
    bool reuse = keep_session && mk8_nextendo_enabled() && nextendo_signed_in();
    if (!reuse && !open_login(keep_session)) return 1;
    for (int i = 0; i < 3100; ++i) {
        NxStatus status; nextendo_status(&status);
        if (status.state == NX_SIGNED_IN) {
            bool ok = nextendo_prepare_game_login(15000);
            if (!ok && reuse && !nextendo_signed_in()) {
                // The server rejected the saved grant. Start PKCE once in this launch.
                reuse = false;
                if (!open_login(keep_session)) return 1;
                continue;
            }
            puts(ok ? "OAuth and NEX token validated; gameplay not verified." : "OAuth accepted; NEX token unavailable.");
            if (ok) {
                game_net_config_set("backend", "nextendo");
                net_config_reload();
                char jwt[4096];
                ok = net_backend() == NET_BACKEND_NEXTENDO && net_id_token(jwt, sizeof jwt) > 0;
                mbedtls_platform_zeroize(jwt, sizeof jwt);
                puts(ok ? "BAAS adapter generated a token; server acceptance not verified." : "BAAS adapter unavailable or online access refused.");
            }
            if (ok && getenv("MK8_NEXTENDO_SOCIAL_TEST")) {
                Mk8Social *social=calloc(1,sizeof *social);
                if (!social) return 2;
                mk8_social_refresh();
                for (int j=0;j<250;++j) {
                    mk8_social_snapshot(social);
                    if (!social->busy) break;
                    SDL_Delay(100);
                }
                printf("Social: friends=%d count=%d lobby=%d in_lobby=%d players=%d/%d\n",
                    social->friends_ok,social->count,social->lobby_ok,social->in_lobby,social->players,social->capacity);
                int ids=0;
                for (int r=0;r<social->lobby_count;++r) if (social->lobbies[r].id[0]) ++ids;
                printf("Listings: valid=%d rooms=%d players=%d ids=%d\n",social->listings_ok,social->lobby_count,social->online_players,ids);
                ok=!social->busy && social->friends_ok && social->lobby_ok && social->listings_ok;
                nextendo_sign_out();
                mk8_social_snapshot(social);
                if (social->friends_ok || social->count || social->lobby_ok) ok=false;
                free(social);
            }
            if (!keep_session) nextendo_sign_out();
            return ok ? 0 : 2;
        }
        if (status.state == NX_ERROR) {
            fprintf(stderr, "Nextendo: %s\n", status.message);
            nextendo_sign_out();
            return 1;
        }
        SDL_Delay(100);
    }
    nextendo_sign_out();
    return 3;
}
