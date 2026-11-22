#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { SETTINGS_CATEGORIES=3, SETTINGS_VISIBLE=5 };
enum { SETTING_CHOICE, SETTING_NUMBER, SETTING_BINDING, SETTING_RESET, SETTING_INFO };
enum { SETTINGS_STEP, SETTINGS_SET, SETTINGS_ACTIVATE, SETTINGS_BEGIN, SETTINGS_FINISH, SETTINGS_RESTART };
enum { SETTINGS_DETACHED=2 };  // Finish after the game takes the settings page away.
typedef struct {
    int id, kind, current, maximum;
    bool enabled;
    char label[64], value[80], help[80];
    int minimum;
} Mk8Setting;

int mk8_settings_values_count(unsigned category);
bool mk8_settings_values_row(unsigned category, unsigned row, Mk8Setting *out);
uint64_t mk8_settings_values_revision(void);
bool mk8_settings_values_request(int operation, int id, int value);
void mk8_settings_values_sync(void);
int mk8_settings_values_saved(void); // 0 idle, 1 pending, 2 saved, 3 restart choice, 4 restarting, -1 failed
bool mk8_settings_values_capturing(void);
void mk8_settings_values_context(bool main_menu);
bool mk8_settings_values_main_menu(void);
bool mk8_settings_style_active(void);
