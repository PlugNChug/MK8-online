#pragma once
#include <stdbool.h>

enum Mk8TipAction {
    MK8_TIP_CONFIRM, MK8_TIP_BACK, MK8_TIP_SWAP, MK8_TIP_CLEAR, MK8_TIP_CLEAR_ALL,
    MK8_TIP_UP, MK8_TIP_DOWN, MK8_TIP_LEFT, MK8_TIP_RIGHT, MK8_TIP_PLUS, MK8_TIP_COUNT
};
enum Mk8TipSource { MK8_TIP_KEYBOARD, MK8_TIP_NINTENDO, MK8_TIP_XBOX, MK8_TIP_PLAYSTATION, MK8_TIP_GENERIC, MK8_TIP_NONE };
struct Mk8ControlTips { enum Mk8TipSource source; char label[MK8_TIP_COUNT][40]; };
void mk8_control_tips(bool settings, struct Mk8ControlTips *out);
