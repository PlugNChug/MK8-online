#pragma once
#include <stddef.h>
#include "settings_children.h"

// Called only by the render thread while visiting the production opt_* page bodies.
bool mk8_collecting_rows();
void mk8_collect_section(const char *label);
int mk8_collect_row(unsigned id,const char *label,const char *value,const char *help,bool enabled,bool adjustable,bool named_option=true);
bool mk8_collect_text(unsigned id,const char *label,char *buf,size_t cap,const char *help);
void mk8_collect_exit_feedback(bool failed,const char *message);
