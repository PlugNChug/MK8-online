#pragma once
#include <stdbool.h>
#include "menu_language.h"

typedef struct {
    bool connected, signed_in, modded;
    char username[64];
} Mk8PlaySession;

#ifdef __cplusplus
extern "C" {
#endif
void mk8_menu_play_session(Mk8PlaySession *out);
#ifdef __cplusplus
}
#endif

static const Mk8MenuLabel mk8_play_back = {"Back to game", "Voltar ao jogo", "Volver al juego"};
static const Mk8MenuLabel mk8_play_open_nextendo = {"Open Nextendo", "Abrir Nextendo", "Abrir Nextendo"};
static const Mk8MenuLabel mk8_play_account_label = {"Account", "Conta", "Cuenta"};
static const Mk8MenuLabel mk8_play_content_label = {"Content", "Conteúdo", "Contenido"};

enum Mk8PlayField { PLAY_SESSION, PLAY_ACCOUNT, PLAY_CONTENT, PLAY_INSTRUCTION, PLAY_VERSION };
static inline const char *mk8_menu_play_text(Mk8MenuLanguage lang, const Mk8PlaySession *session, enum Mk8PlayField field) {
    switch (field) {
    case PLAY_SESSION:
        return session->connected ? mk8_menu_text(lang,"Nextendo active","Nextendo ativo","Nextendo activo")
                                  : mk8_menu_text(lang,"Local session","Sessão local","Sesión local");
    case PLAY_ACCOUNT:
        return session->signed_in ? session->username : mk8_menu_text(lang,"signed out of Nextendo","sem login Nextendo","sin sesión en Nextendo");
    case PLAY_CONTENT:
        return session->modded ? mk8_menu_text(lang,"CTGP / local mod","CTGP / mod local","CTGP / mod local")
                               : mk8_menu_text(lang,"base game","jogo original","juego base");
    case PLAY_INSTRUCTION:
        return session->connected
            ? mk8_menu_text(lang,"Back in the game, select Online Play.","Volte ao jogo e selecione Online Play.","Vuelve al juego y elige Juego en línea.")
            : mk8_menu_text(lang,"Use Play on Nextendo in the launcher.","No launcher, escolha Jogar no Nextendo.","Elige Jugar en Nextendo en el lanzador.");
    case PLAY_VERSION:
        return "MK8 Deluxe 4.0.1";
    }
    return "";
}
