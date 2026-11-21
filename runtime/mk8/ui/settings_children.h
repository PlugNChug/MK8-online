#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char label[192], value[192], help[384];
    bool enabled, adjustable;
} Mk8NativeRow;

void mk8_native_rows_open(int group);
void mk8_native_rows_close(void);
int mk8_native_rows_count(unsigned *revision,unsigned *depth);
bool mk8_native_rows_get(int index,unsigned revision,Mk8NativeRow *out);
bool mk8_native_rows_activate(int index,unsigned revision,int step);
bool mk8_native_rows_back(void);

#ifdef __cplusplus
}
#endif
