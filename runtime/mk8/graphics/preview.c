// Explicit F7 window only. Images stay in memory until all selected frames are collected.
#include "incident.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

#define PREVIEW_FRAMES 60
#define PREVIEW_DRAWS 1024
#define PREVIEW_W 320
#define PREVIEW_H 180
#define PREVIEW_PIXELS (PREVIEW_W*PREVIEW_H*4)
typedef struct {
    uint64_t frame,ns,readback_ns;
    unsigned attempted,suppressed,emitted,stored,truncated,w,h;
    bool image;
} PreviewFrame;
_Static_assert(PREVIEW_FRAMES*(sizeof(Mk8PreviewDraw)*PREVIEW_DRAWS+PREVIEW_PIXELS)<32u*1024*1024,
               "Preview memory budget changed");
static atomic_bool arm_requested, tracing;
static PreviewFrame frames[PREVIEW_FRAMES];
static Mk8PreviewDraw *draws,*current_draw;
static uint8_t *pixels;
static PreviewFrame *current,*waiting;
static unsigned count;
static uint64_t first_frame,readback_start,condition_mode,condition_token;
static bool ready,command_active,condition_valid,condition_equal,early_stop;
static char expected_png[300];
static bool png_ok;
static struct { bool present,consecutive,early; unsigned frames,images,truncated; } saved;

bool mk8_gfx_preview_active(void) { return atomic_load_explicit(&tracing,memory_order_acquire); }
void mk8_gfx_preview_request(void) {
    if (mk8_gfx_enabled()) atomic_store(&arm_requested,true);
}
static PreviewFrame *frame_for(uint64_t frame) {
    if (current && current->frame==frame) return current;
    if (frame<first_frame || count>=PREVIEW_FRAMES) return NULL;
    current=&frames[count++]; current->frame=frame; current->ns=mk8_gfx_time_ns();
    return current;
}
void mk8_gfx_preview_draw(const Mk8PreviewDraw *draw) {
    current_draw=NULL; command_active=false;
    if (!mk8_gfx_preview_active()) return;
    PreviewFrame *f=frame_for(draw->frame);
    if (!f) return;
    command_active=true;
    f->attempted++; f->suppressed+=draw->render_off!=0;
    if (f->stored>=PREVIEW_DRAWS) { f->truncated++; return; }
    current_draw=&draws[(count-1)*PREVIEW_DRAWS+f->stored++];
    *current_draw=*draw;
    current_draw->ordinal=f->attempted;
    current_draw->condition_valid=condition_valid;
    current_draw->condition_equal=condition_equal;
    current_draw->condition_mode=condition_mode;
    current_draw->condition_token=condition_token;
}
void mk8_gfx_preview_draw_end(void) { command_active=false; current_draw=NULL; }
void mk8_gfx_preview_emit(unsigned w,unsigned h) {
    if (!command_active) return;
    current->emitted++;
    if (current_draw) { current_draw->vk_calls++; current_draw->pass_w=w; current_draw->pass_h=h; }
}
void mk8_gfx_preview_skip(unsigned line) { if (command_active && current_draw) current_draw->skip_line=line; }
void mk8_gfx_preview_condition(uint64_t mode,bool conditional,bool equal,uint64_t token) {
    if (!mk8_gfx_preview_active()) return;
    condition_mode=mode; condition_valid=conditional; condition_equal=equal; condition_token=token;
}
void mk8_gfx_preview_frame_done(void) {
    if (waiting) { waiting->readback_ns=mk8_gfx_time_ns()-readback_start; waiting=NULL; }
}
bool mk8_gfx_preview_present(uint64_t frame,bool busy,bool report_requested,unsigned w,unsigned h,char *path,unsigned capacity) {
    mk8_gfx_preview_frame_done();
    if (atomic_exchange(&arm_requested,false) && !mk8_gfx_preview_active() && !ready) {
        draws=calloc(PREVIEW_FRAMES*PREVIEW_DRAWS,sizeof *draws);
        pixels=calloc(PREVIEW_FRAMES,PREVIEW_PIXELS);
        if (!draws || !pixels) { free(draws); free(pixels); draws=NULL; pixels=NULL;
            fprintf(stderr,"[gfx-preview] allocation failed\n"); return false; }
        memset(frames,0,sizeof frames); count=0; current=NULL; current_draw=NULL;
        command_active=false; condition_valid=false; early_stop=false; first_frame=frame+1;
        atomic_store(&tracing,true);
        fprintf(stderr,"[gfx-preview] start host frame %llu\n",(unsigned long long)first_frame);
        return false;
    }
    if (!mk8_gfx_preview_active() || frame<first_frame) return false;
    PreviewFrame *f=frame_for(frame);
    if (report_requested || count>=PREVIEW_FRAMES) {
        early_stop=count<PREVIEW_FRAMES;
        ready=true; atomic_store(&tracing,false); command_active=false; current_draw=NULL;
        fprintf(stderr,"[gfx-preview] end host frame %llu\n",(unsigned long long)frame);
        mk8_gfx_request(); // existing incident arbitration and disk reserve still apply
        return false;
    }
    if (!f) return false;
    if (busy || !w || !h || (uint64_t)w*h>16777216 || capacity<13) return false;
    memcpy(path,"@mk8-preview",13);
    waiting=f; readback_start=mk8_gfx_time_ns();
    return true;
}
bool mk8_gfx_preview_image(const char *path,const uint8_t *rgba,unsigned w,unsigned h) {
    if (strcmp(path,"@mk8-preview")) return false;
    if (!waiting || !rgba || !w || !h || (uint64_t)w*h>16777216) return true;
    unsigned dw=w<PREVIEW_W ? w : PREVIEW_W;
    unsigned dh=(unsigned)((uint64_t)h*dw/w);
    if (!dh) dh=1;
    if (dh>PREVIEW_H) { dh=PREVIEW_H; dw=(unsigned)((uint64_t)w*dh/h); if (!dw) dw=1; }
    uint8_t *dest=pixels+(waiting-frames)*PREVIEW_PIXELS;
    for (unsigned y=0;y<dh;y++) for (unsigned x=0;x<dw;x++) {
        uint64_t src=((uint64_t)y*h/dh*w+(uint64_t)x*w/dw)*4;
        memcpy(dest+((size_t)y*dw+x)*4,rgba+src,4);
    }
    waiting->w=dw; waiting->h=dh; waiting->image=true;
    return true;
}
void mk8_gfx_preview_png_result(const char *path,bool success) {
    if (*expected_png && !strcmp(path,expected_png)) png_ok=success;
}
static bool close_file(FILE *f) { bool ok=!ferror(f); if (fclose(f)) ok=false; return ok; }
static void number(FILE *f,float value) { if (isfinite(value)) fprintf(f,"%.9g",(double)value); else fputs("null",f); }
bool mk8_gfx_preview_dump(const char *directory,Mk8GfxPngWriter writer,uint64_t report_frame) {
    memset(&saved,0,sizeof saved);
    if (!ready) return true;
    saved.present=true; saved.frames=count; saved.early=early_stop; saved.consecutive=true;
    char folder[280],path[300];
    snprintf(folder,sizeof folder,"%s/preview",directory);
    bool ok=writer && CreateDirectoryA(folder,NULL);
    FILE *frame_file=NULL,*draw_file=NULL;
    if (ok) { snprintf(path,sizeof path,"%s/frames.jsonl",folder); frame_file=fopen(path,"wbx"); ok=frame_file!=NULL; }
    if (ok) { snprintf(path,sizeof path,"%s/draws.jsonl",folder); draw_file=fopen(path,"wbx"); ok=draw_file!=NULL; }
    for (unsigned i=0;ok && i<count;i++) {
        PreviewFrame *f=&frames[i];
        bool alias=!f->image && f->frame==report_frame;
        bool image=f->image || alias;
        if (!image || (i && f->frame!=frames[i-1].frame+1)) saved.consecutive=false;
        saved.images+=image; saved.truncated+=f->truncated;
        char name[80]; snprintf(name,sizeof name,"frame-%llu.png",(unsigned long long)f->frame);
        if (f->image) {
            snprintf(expected_png,sizeof expected_png,"%s/%s",folder,name); png_ok=false;
            writer(expected_png,pixels+i*PREVIEW_PIXELS,f->w,f->h);
            *expected_png=0;
            if (!png_ok) { ok=false; break; }
        }
        fprintf(frame_file,"{\"host_frame\":%llu,\"host_ns\":%llu,\"readback_ns\":%llu,\"image\":",
            (unsigned long long)f->frame,(unsigned long long)f->ns,(unsigned long long)f->readback_ns);
        if (image) fprintf(frame_file,"\"%s\"",alias ? "../screenshot.png" : name); else fputs("null",frame_file);
        fprintf(frame_file,",\"thumbnail_width\":%u,\"thumbnail_height\":%u,\"attempted_draws\":%u,\"conditional_off\":%u,"
            "\"vk_draw_calls\":%u,\"stored_draws\":%u,\"truncated_draws\":%u}\n",
            f->w,f->h,f->attempted,f->suppressed,f->emitted,f->stored,f->truncated);
        for (unsigned j=0;j<f->stored;j++) {
            Mk8PreviewDraw *d=&draws[i*PREVIEW_DRAWS+j];
            fprintf(draw_file,"{\"host_frame\":%llu,\"ordinal\":%u,\"vs_code_va\":\"%016llx\",\"fs_code_va\":\"%016llx\","
                "\"geometry_token\":\"%016llx\",\"count\":%u,\"mode\":%u,\"indexed\":%u,\"render_off\":%u,"
                "\"cull\":%u,\"depth_state\":%u,\"condition_valid\":%u,\"condition_equal\":%u,"
                "\"condition_mode\":%llu,\"condition_token\":\"%016llx\",\"vk_calls\":%u,\"skip_line\":%u,"
                "\"color_extent\":[%u,%u],\"depth_extent\":[%u,%u],\"pass_extent\":[%u,%u],\"viewport\":[",
                (unsigned long long)d->frame,d->ordinal,(unsigned long long)d->vs,(unsigned long long)d->fs,
                (unsigned long long)d->geometry_token,d->count,d->mode,d->indexed,d->render_off,d->cull,d->depth_state,
                d->condition_valid,d->condition_equal,(unsigned long long)d->condition_mode,
                (unsigned long long)d->condition_token,d->vk_calls,d->skip_line,
                d->color_w,d->color_h,d->depth_w,d->depth_h,d->pass_w,d->pass_h);
            for (unsigned k=0;k<4;k++) { if(k) fputc(',',draw_file); number(draw_file,d->viewport[k]); }
            fprintf(draw_file,"],\"scissor\":[%d,%d,%d,%d]}\n",d->scissor[0],d->scissor[1],d->scissor[2],d->scissor[3]);
        }
    }
    if (frame_file && !close_file(frame_file)) ok=false;
    if (draw_file && !close_file(draw_file)) ok=false;
    free(draws); free(pixels); draws=NULL; pixels=NULL; current=NULL; waiting=NULL; ready=false;
    return ok;
}
void mk8_gfx_preview_manifest(FILE *f) {
    if (!saved.present) { fputs("null",f); return; }
    fprintf(f,"{\"requested_frames\":%u,\"frames\":%u,\"images\":%u,\"consecutive\":%s,\"early_stop\":%s,"
        "\"truncated_draws\":%u,\"max_draws_per_frame\":%u,\"max_thumbnail\":[%u,%u]}",
        PREVIEW_FRAMES,saved.frames,saved.images,saved.consecutive ? "true" : "false",saved.early ? "true" : "false",
        saved.truncated,PREVIEW_DRAWS,PREVIEW_W,PREVIEW_H);
}
