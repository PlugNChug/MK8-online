// Preserve the generated source location when Zig's default UBSan panic strips its stack trace.
// Stop on the invalid access; never turn an invalid load into a fabricated guest value.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { const char *file; uint32_t line, column; } SourceLocation;
typedef struct { SourceLocation location; const void *type; uint8_t alignment, kind; } TypeMismatch;

void __ubsan_handle_type_mismatch_v1(const TypeMismatch *data, uintptr_t pointer) {
    fprintf(stderr, "[memory-fault] pointer=0x%llx kind=%u at %s:%u:%u\n",
            (unsigned long long)pointer, data->kind,
            data->location.file ? data->location.file : "unknown",
            data->location.line, data->location.column);
    fflush(stderr);
    exit(86);
}
void __ubsan_handle_type_mismatch_v1_abort(const TypeMismatch *data, uintptr_t pointer) {
    __ubsan_handle_type_mismatch_v1(data, pointer);
}
