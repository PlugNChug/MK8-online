#pragma once
#include <stdbool.h>
#include "menu_language.h"

#ifdef __cplusplus
extern "C" {
#endif
bool mk8_mii_menu_available(void);
bool mk8_room_menu_available(void);
void mk8_open_mii_menu(void);
void mk8_open_room_menu(void);
#ifdef __cplusplus
}
#endif

typedef struct {
    Mk8MenuLabel label;
    bool (*available)(void);
    void (*open)(void);
} Mk8MenuLauncherAction;

enum { MENU_MII_PROFILE, MENU_FRIEND_ROOMS, MENU_LAUNCHER_ACTION_COUNT };
static const Mk8MenuLauncherAction mk8_menu_launcher_actions[MENU_LAUNCHER_ACTION_COUNT] = {
    {{"Mii profile","Perfil Mii","Perfil Mii"}, mk8_mii_menu_available, mk8_open_mii_menu},
    {{"Rooms with friends","Salas com amigos","Salas con amigos"}, mk8_room_menu_available, mk8_open_room_menu}
};
