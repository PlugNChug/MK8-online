#pragma once
#include "menu_language.h"

static const Mk8MenuLabel mk8_nx_account={"Nextendo Account","Conta Nextendo","Cuenta de Nextendo"};
static const Mk8MenuLabel mk8_nx_players={"Players","Jogadores","Jugadores"};
static const Mk8MenuLabel mk8_nx_status={"Status","Estado","Estado"};
static const Mk8MenuLabel mk8_nx_mode={"Mode","Modo","Modo"};
static const Mk8MenuLabel mk8_nx_room={"Nextendo room","Sala Nextendo","Sala de Nextendo"};
static const Mk8MenuLabel mk8_nx_current_room={"Your current room","Sua sala atual","Tu sala actual"};
static const Mk8MenuLabel mk8_nx_empty={"No rooms are listed right now.","Nenhuma sala listada no momento.","No hay salas en la lista."};
static const Mk8MenuLabel mk8_nx_no_current_room={"You haven't joined a room yet.","Você ainda não entrou em uma sala.","Todavía no te has unido a una sala."};
static const Mk8MenuLabel mk8_nx_loading={"Updating Nextendo data...","Atualizando dados do Nextendo...","Actualizando datos de Nextendo..."};
static const Mk8MenuLabel mk8_nx_online_play={"To join a race, select Online Play.","Para correr, selecione Online Play.","Para correr, elige Juego en línea."};
static const Mk8MenuLabel mk8_nx_check_account={"Check your account connection.","Confira a conexão da sua conta.","Comprueba la conexión de tu cuenta."};
static const Mk8MenuLabel mk8_nx_launcher_signin={"Choose Play on Nextendo in the launcher, then sign in through your browser.",
    "No launcher, selecione Jogar no Nextendo e faça login pelo navegador.",
    "Selecciona Jugar en Nextendo en el lanzador e inicia sesión en el navegador."};
static const Mk8MenuLabel mk8_nx_refresh_interval={"Data refreshes every 30 seconds.","Dados atualizados a cada 30 segundos.","Los datos se actualizan cada 30 s."};

static const Mk8MenuLabel mk8_nx_friends_empty={"Your friends list is empty.","Sua lista de amigos está vazia.","Tu lista de amigos está vacía."};
static const Mk8MenuLabel mk8_nx_invites_unavailable={"Invites and joining are unavailable.","Convites e entrada não disponíveis.","Invitaciones y acceso no disponibles."};

static inline const char *mk8_nx_presence(Mk8MenuLanguage lang,int presence) {
    switch(presence) {
    case 0: return mk8_menu_text(lang,"Offline","Offline","Sin conexión");
    case 1: return mk8_menu_text(lang,"Online","Online","En línea");
    case 2: return mk8_menu_text(lang,"Playing","Jogando","Jugando");
    default: return mk8_menu_text(lang,"Unknown","Não informada","Desconocida");
    }
}

static inline const char *mk8_nx_service_error(Mk8MenuLanguage lang,int status) {
    switch (status) {
    case 401: return mk8_menu_text(lang,"Session expired. Sign in again.","Sessão expirada. Faça login novamente.","Tu sesión caducó. Vuelve a entrar.");
    case 403: return mk8_menu_text(lang,"Allow friends access in Nextendo.","Autorize o acesso a amigos no Nextendo.","Permite el acceso a amigos en Nextendo.");
    case 429: return mk8_menu_text(lang,"Too many requests. Wait and retry.","Muitas solicitações. Aguarde e repita.","Muchas solicitudes. Espera y reintenta.");
    case 200: return mk8_menu_text(lang,"Unexpected service response.","Resposta inesperada do serviço.","Respuesta inesperada del servicio.");
    default: return mk8_menu_text(lang,"Service unreachable. Try again.","Serviço indisponível. Tente de novo.","Servicio inaccesible. Vuelve a intentar.");
    }
}
