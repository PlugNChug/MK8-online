#pragma once
#include <stdbool.h>
#include <stdint.h>

void mk8_auto_lan_menu_tick(uint64_t manager);
bool mk8_guest_lan_requested(void);
int mk8_guest_lan_hold_boot(uint64_t page);
unsigned mk8_guest_lan_editor_players(uint64_t manager);
void mk8_guest_lan_start(uint64_t descriptor, uint64_t manager);
