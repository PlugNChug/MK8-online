#ifndef MK8_GRAPHICS_INCIDENT_H
#define MK8_GRAPHICS_INCIDENT_H
#include <stdbool.h>
#include <stdint.h>
#include "preview.h"
#ifdef __cplusplus
extern "C" {
#endif

// Host renderer present-attempt ordinal, starting at 1. Not a guest tick or GPU completion.
bool mk8_gfx_enabled(void);
void mk8_gfx_request(void);
void mk8_gfx_skip(uint64_t frame, const uint64_t code[3], uint64_t pipeline,
                  int state, int mode, bool redrawable, bool budget_miss, double remaining_ms);
// These run on the renderer owner only. Never touch g_window_shot_request.
bool mk8_gfx_begin(uint64_t frame, bool screenshot_busy, uint32_t w, uint32_t h,
                   uint32_t format, uint32_t driver, uint32_t vendor, uint32_t device, bool framegen,
                   char *path, unsigned capacity);
void mk8_gfx_end(void);
void mk8_gfx_png_result(const char *path, bool success);
// Window thread: host frame is only an atomic sample of the latest present marker.
void mk8_gfx_fullscreen_request(unsigned source, bool wanted); // 1 F11, 2 other shortcut, 3 menu
void mk8_gfx_fullscreen_apply(bool wanted, bool success, bool reported, unsigned w, unsigned h);
// Renderer owner: actual current host present-attempt identifier.
void mk8_gfx_swapchain(uint64_t frame, unsigned w, unsigned h, unsigned format);
void mk8_gfx_scale(uint64_t frame, float x, float y);
#ifdef __cplusplus
}
#endif
#endif
