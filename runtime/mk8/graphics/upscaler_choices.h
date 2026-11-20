#pragma once
#include "settings.h"
#include <stdlib.h>

/* The launcher verifies optional providers before exposing them to this process.
   Keep an unavailable saved preference visible so it can also be changed back. */
typedef struct {
    const char *names[5];
    int values[5], count, selected;
} Mk8Upscalers;

static inline Mk8Upscalers mk8_upscaler_choices(int selected) {
    static const char *const names[] = {"Bilinear",
#ifdef __APPLE__
        "MetalFX",
#else
        "AMD FSR 1",
#endif
        "AMD FSR 3.1.6", "AMD FSR 4.1.2", "NVIDIA DLSS SR"};
    static const int values[] = {UPSCALE_BILINEAR, UPSCALE_SHARP, UPSCALE_FSR3, UPSCALE_FSR4, UPSCALE_DLSS};
    static const char *const permission[] = {NULL, NULL, "MK8_READY_FSR3", "MK8_READY_FSR4", "MK8_READY_DLSS"};
    Mk8Upscalers list = {0};
    for (int i = 0; i < 5; ++i) {
        if (permission[i] && !getenv(permission[i]) && selected != values[i]) continue;
        if (selected == values[i]) list.selected = list.count;
        list.names[list.count] = names[i];
        list.values[list.count++] = values[i];
    }
    return list;
}

