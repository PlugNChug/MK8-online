#pragma once
#include <stdbool.h>
#include <stdint.h>

enum Mk8EditCommand {
    MK8_EDIT_BEGIN, MK8_EDIT_ASSIGN, MK8_EDIT_SWAP, MK8_EDIT_CLEAR, MK8_EDIT_CLEAR_ALL,
    MK8_EDIT_KEYBOARD, MK8_EDIT_APPLY, MK8_EDIT_CANCEL, MK8_EDIT_STOP_LISTEN
};
enum Mk8EditState {
    MK8_EDIT_CLOSED, MK8_EDIT_PENDING, MK8_EDIT_READY, MK8_EDIT_LISTEN,
    MK8_EDIT_NEED_LEFT, MK8_EDIT_NEED_RIGHT, MK8_EDIT_APPLIED, MK8_EDIT_FAILED,
    MK8_EDIT_AMBIGUOUS, MK8_EDIT_PAIR_HELD
};

bool mk8_edit_command(enum Mk8EditCommand action,int row,int other);
enum Mk8EditState mk8_edit_view(uint16_t names[4][80],int *listening_row);
bool mk8_edit_has_combined(void);
unsigned mk8_controller_join_players(void);
bool mk8_controller_join_keyboard(void);
bool mk8_controller_join_back_pressed(void);
bool mk8_controller_join_visible(void);
bool mk8_controller_layout_available(void);
void mk8_title_input_active(bool active);
