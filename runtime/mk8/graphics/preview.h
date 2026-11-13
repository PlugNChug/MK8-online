#ifndef MK8_GRAPHICS_PREVIEW_H
#define MK8_GRAPHICS_PREVIEW_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*Mk8GfxPngWriter)(const char *,const uint8_t *,uint32_t,uint32_t);
typedef struct {
    uint64_t frame,vs,fs,geometry_token,condition_token,condition_mode;
    uint32_t count,mode,indexed,render_off,cull,depth_state;
    uint32_t color_w,color_h,depth_w,depth_h,pass_w,pass_h;
    float viewport[4];
    int32_t scissor[4];
    uint32_t ordinal,vk_calls,skip_line,condition_valid,condition_equal;
} Mk8PreviewDraw;
void mk8_gfx_preview_request(void);
bool mk8_gfx_preview_active(void);
void mk8_gfx_preview_draw(const Mk8PreviewDraw *draw);
void mk8_gfx_preview_draw_end(void);
void mk8_gfx_preview_emit(unsigned w,unsigned h);
void mk8_gfx_preview_skip(unsigned line);
void mk8_gfx_preview_condition(uint64_t mode,bool conditional,bool equal,uint64_t token);
bool mk8_gfx_preview_present(uint64_t frame,bool busy,bool report_requested,unsigned w,unsigned h,char *path,unsigned capacity);
bool mk8_gfx_preview_image(const char *path,const uint8_t *rgba,unsigned w,unsigned h);
void mk8_gfx_preview_frame_done(void);
bool mk8_gfx_preview_dump(const char *directory,Mk8GfxPngWriter writer,uint64_t report_frame);
void mk8_gfx_preview_png_result(const char *path,bool success);
void mk8_gfx_preview_manifest(FILE *f);
uint64_t mk8_gfx_time_ns(void);
uint64_t mk8_gfx_token(uint64_t value);
void mk8_gfx_occlusion_result(uint64_t submission,uint64_t report,uint64_t value,bool available);
void mk8_gfx_finish(Mk8GfxPngWriter writer);
#ifdef __cplusplus
}
#endif
#endif
