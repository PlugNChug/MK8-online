// Explicit F7 window only. Images stay in memory until all selected frames are collected.
#include "incident.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

#define PREVIEW_FRAMES 60
#define PREVIEW_DRAWS 1536
#define PREVIEW_W 320
#define PREVIEW_H 180
#define PREVIEW_PIXELS (PREVIEW_W*PREVIEW_H*4)
typedef struct {
    uint64_t frame,ns,readback_ns;
    unsigned attempted,suppressed,emitted,stored,truncated,w,h;
    bool image;
} PreviewFrame;
static atomic_bool arm_requested, tracing;
static PreviewFrame frames[PREVIEW_FRAMES];
static Mk8PreviewDraw *draws,*current_draw;
static uint8_t *pixels;
static PreviewFrame *current,*waiting;
static unsigned count;
static uint64_t first_frame,readback_start,condition_mode,condition_token,condition_id;
static bool ready,command_active,condition_valid,condition_equal,early_stop;
static char expected_png[300];
static bool png_ok;
static struct { bool present,consecutive,early; unsigned frames,images,truncated; } saved;

#define COUNTER_CAPACITY 16384
typedef struct {
    uint64_t id,ns,frame,submission,completed,token,token_b,value,value_b,base,sum,mode;
    unsigned kind,index,after,queries;
    bool values_valid,equal,render_off,completion_known,available,accepted,reset,frame_sample;
} CounterEvent;
typedef struct { atomic_bool busy; CounterEvent event; } CounterSlot;
static CounterSlot counter_ring[COUNTER_CAPACITY];
static atomic_uint_fast64_t counter_issued,counter_epoch,counter_completed,counter_host_frame;
static atomic_bool counter_completion_known;
static uint64_t counter_first,counter_saved_first,counter_saved_last,counter_saved_issued,counter_saved_stored;
_Static_assert(PREVIEW_FRAMES*(sizeof(Mk8PreviewDraw)*PREVIEW_DRAWS+PREVIEW_PIXELS)+
               sizeof frames+sizeof counter_ring<32u*1024*1024,"Preview memory budget changed");

static uint64_t counter_record(CounterEvent e) {
    uint64_t epoch=atomic_load_explicit(&counter_epoch,memory_order_acquire);
    if (!mk8_gfx_preview_active()) return 0;
    e.id=atomic_fetch_add(&counter_issued,1)+1;
    CounterSlot *slot=&counter_ring[(e.id-1)%COUNTER_CAPACITY];
    if (atomic_exchange_explicit(&slot->busy,true,memory_order_acquire)) return 0;
    bool valid=mk8_gfx_preview_active() && epoch==atomic_load(&counter_epoch) && slot->event.id<e.id;
    if (valid) {
        e.ns=mk8_gfx_time_ns();
        if (!e.frame) { e.frame=atomic_load(&counter_host_frame); e.frame_sample=true; }
        e.completion_known=atomic_load_explicit(&counter_completion_known,memory_order_acquire);
        e.completed=atomic_load(&counter_completed);
        slot->event=e;
    }
    atomic_store_explicit(&slot->busy,false,memory_order_release);
    return valid ? e.id : 0;
}
uint64_t mk8_gfx_counter_compare(uint64_t frame,uint64_t submission,uint64_t mode,uint64_t report,
                                bool has_values,uint64_t a,uint64_t b,bool equal,bool render_off) {
    if (!mk8_gfx_preview_active()) return 0;
    return counter_record((CounterEvent){.kind=1,.frame=frame,.submission=submission,.mode=mode,
        .token=mk8_gfx_token(report),.token_b=has_values ? mk8_gfx_token(report+16) : 0,
        .values_valid=has_values,.value=a,.value_b=b,.equal=equal,.render_off=render_off});
}
void mk8_gfx_counter_queue(uint64_t frame,uint64_t submission,uint64_t report,unsigned index,
                          unsigned after,unsigned queries,bool accepted,bool reset) {
    if (!mk8_gfx_preview_active()) return;
    counter_record((CounterEvent){.kind=2,.frame=frame,.submission=submission,.token=mk8_gfx_token(report),
        .index=index,.after=after,.queries=queries,.accepted=accepted,.reset=reset});
}
void mk8_gfx_counter_write(uint64_t submission,uint64_t report,uint64_t value,uint64_t base,uint64_t sum,
                          unsigned index,unsigned after,unsigned queries,bool available) {
    if (!mk8_gfx_preview_active()) return;
    counter_record((CounterEvent){.kind=3,.submission=submission,.token=mk8_gfx_token(report),
        .value=value,.base=base,.sum=sum,.index=index,.after=after,.queries=queries,.available=available});
}
void mk8_gfx_counter_reset(uint64_t submission,unsigned index,uint64_t base,uint64_t sum) {
    if (!mk8_gfx_preview_active()) return;
    counter_record((CounterEvent){.kind=4,.submission=submission,.index=index,.base=base,.sum=sum});
}
void mk8_gfx_counter_complete(uint64_t done) {
    if (!mk8_gfx_preview_active()) return;
    uint64_t previous=atomic_load(&counter_completed);
    while (done>previous && !atomic_compare_exchange_weak(&counter_completed,&previous,done)) {}
    bool was_known=atomic_exchange_explicit(&counter_completion_known,true,memory_order_acq_rel);
    if (!was_known || done>previous) counter_record((CounterEvent){.kind=5,.submission=done});
}
void mk8_gfx_counter_value(uint64_t frame,uint64_t submission,uint64_t report,uint64_t value) {
    if (!mk8_gfx_preview_active()) return;
    counter_record((CounterEvent){.kind=6,.frame=frame,.submission=submission,
        .token=mk8_gfx_token(report),.value=value});
}
void mk8_gfx_counter_submit(uint64_t frame,uint64_t submission) {
    counter_record((CounterEvent){.kind=7,.frame=frame,.submission=submission});
}

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
    current_draw->condition_id=condition_id;
}
void mk8_gfx_preview_draw_end(void) { command_active=false; current_draw=NULL; }
void mk8_gfx_preview_emit(unsigned w,unsigned h) {
    if (!command_active) return;
    current->emitted++;
    if (current_draw) { current_draw->vk_calls++; current_draw->pass_w=w; current_draw->pass_h=h; }
}
void mk8_gfx_preview_skip(unsigned line) { if (command_active && current_draw) current_draw->skip_line=line; }
void mk8_gfx_preview_condition(uint64_t mode,bool conditional,bool equal,uint64_t token,uint64_t id) {
    if (!mk8_gfx_preview_active()) return;
    condition_mode=mode; condition_valid=conditional; condition_equal=equal; condition_token=token;
    condition_id=id;
}
void mk8_gfx_preview_frame_done(void) {
    if (waiting) { waiting->readback_ns=mk8_gfx_time_ns()-readback_start; waiting=NULL; }
}
bool mk8_gfx_preview_present(uint64_t frame,bool busy,bool report_requested,unsigned w,unsigned h,char *path,unsigned capacity) {
    atomic_store(&counter_host_frame,frame);
    mk8_gfx_preview_frame_done();
    if (atomic_exchange(&arm_requested,false) && !mk8_gfx_preview_active() && !ready) {
        draws=calloc(PREVIEW_FRAMES*PREVIEW_DRAWS,sizeof *draws);
        pixels=calloc(PREVIEW_FRAMES,PREVIEW_PIXELS);
        if (!draws || !pixels) { free(draws); free(pixels); draws=NULL; pixels=NULL;
            fprintf(stderr,"[gfx-preview] allocation failed\n"); return false; }
        memset(frames,0,sizeof frames); count=0; current=NULL; current_draw=NULL;
        command_active=false; condition_valid=false; early_stop=false; first_frame=frame+1;
        condition_id=0; counter_first=atomic_load(&counter_issued)+1;
        atomic_fetch_add_explicit(&counter_epoch,1,memory_order_release);
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
static bool counter_dump(const char *folder) {
    char path[300]; snprintf(path,sizeof path,"%s/counters.jsonl",folder);
    FILE *f=fopen(path,"wbx"); if (!f) return false;
    uint64_t last=atomic_load(&counter_issued);
    counter_saved_first=counter_first; counter_saved_last=last;
    counter_saved_issued=last>=counter_first ? last-counter_first+1 : 0; counter_saved_stored=0;
    uint64_t first=counter_first;
    if (last>=COUNTER_CAPACITY && first<=last-COUNTER_CAPACITY) first=last-COUNTER_CAPACITY+1;
    static const char *names[]={"unknown","compare","report_queue","report_write","counter_reset","submission_complete","report_value","submission_sent"};
    for (uint64_t id=first;id<=last;id++) {
        CounterSlot *slot=&counter_ring[(id-1)%COUNTER_CAPACITY];
        if (atomic_exchange_explicit(&slot->busy,true,memory_order_acquire)) continue;
        CounterEvent e=slot->event;
        atomic_store_explicit(&slot->busy,false,memory_order_release);
        if (e.id!=id) continue;
        counter_saved_stored++;
        fprintf(f,"{\"id\":%llu,\"host_ns\":%llu,\"host_frame\":%llu,\"frame_sample\":%s,\"kind\":\"%s\","
            "\"submission\":%llu,\"completion_known\":%s,\"observed_completed_submission\":%llu,\"report_token\":\"%016llx\"",
            (unsigned long long)e.id,(unsigned long long)e.ns,(unsigned long long)e.frame,e.frame_sample ? "true" : "false",names[e.kind],
            (unsigned long long)e.submission,e.completion_known ? "true" : "false",(unsigned long long)e.completed,(unsigned long long)e.token);
        if (e.kind==1) {
            fprintf(f,",\"second_report_token\":\"%016llx\",\"mode\":%llu,\"values_valid\":%s,\"value_a\":",
                (unsigned long long)e.token_b,(unsigned long long)e.mode,e.values_valid ? "true" : "false");
            if (e.values_valid) fprintf(f,"%llu,\"value_b\":%llu",(unsigned long long)e.value,(unsigned long long)e.value_b);
            else fputs("null,\"value_b\":null",f);
            fprintf(f,",\"equal\":%s,\"render_off\":%s",e.equal ? "true" : "false",e.render_off ? "true" : "false");
        }
        if (e.kind==2 || e.kind==3 || e.kind==4) fprintf(f,",\"report_index\":%u",e.index);
        if (e.kind==2 || e.kind==3) fprintf(f,",\"queries_before\":%u,\"queries_in_slot\":%u",e.after,e.queries);
        if (e.kind==2) fprintf(f,",\"accepted\":%s,\"reset\":%s",e.accepted ? "true" : "false",e.reset ? "true" : "false");
        if (e.kind==3 || e.kind==4) fprintf(f,",\"base\":%llu,\"sum\":%llu",(unsigned long long)e.base,(unsigned long long)e.sum);
        if (e.kind==3) fprintf(f,",\"query_available\":%s",e.available ? "true" : "false");
        if (e.kind==3 || e.kind==6) fprintf(f,",\"value\":%llu",(unsigned long long)e.value);
        fputs("}\n",f);
    }
    return close_file(f);
}
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
                "\"condition_mode\":%llu,\"condition_token\":\"%016llx\",\"condition_id\":%llu,\"vk_calls\":%u,\"skip_line\":%u,"
                "\"color_extent\":[%u,%u],\"depth_extent\":[%u,%u],\"pass_extent\":[%u,%u],\"viewport\":[",
                (unsigned long long)d->frame,d->ordinal,(unsigned long long)d->vs,(unsigned long long)d->fs,
                (unsigned long long)d->geometry_token,d->count,d->mode,d->indexed,d->render_off,d->cull,d->depth_state,
                d->condition_valid,d->condition_equal,(unsigned long long)d->condition_mode,
                (unsigned long long)d->condition_token,(unsigned long long)d->condition_id,d->vk_calls,d->skip_line,
                d->color_w,d->color_h,d->depth_w,d->depth_h,d->pass_w,d->pass_h);
            for (unsigned k=0;k<4;k++) { if(k) fputc(',',draw_file); number(draw_file,d->viewport[k]); }
            fprintf(draw_file,"],\"scissor\":[%d,%d,%d,%d]}\n",d->scissor[0],d->scissor[1],d->scissor[2],d->scissor[3]);
        }
    }
    if (frame_file && !close_file(frame_file)) ok=false;
    if (draw_file && !close_file(draw_file)) ok=false;
    if (ok && !counter_dump(folder)) ok=false;
    free(draws); free(pixels); draws=NULL; pixels=NULL; current=NULL; waiting=NULL; ready=false;
    return ok;
}
void mk8_gfx_preview_manifest(FILE *f) {
    if (!saved.present) { fputs("null",f); return; }
    fprintf(f,"{\"requested_frames\":%u,\"frames\":%u,\"images\":%u,\"consecutive\":%s,\"early_stop\":%s,"
        "\"truncated_draws\":%u,\"max_draws_per_frame\":%u,\"max_thumbnail\":[%u,%u],"
        "\"counter_trace\":{\"capacity\":%u,\"first_id\":%llu,\"last_id\":%llu,\"issued\":%llu,\"stored\":%llu,\"missing\":%llu}}",
        PREVIEW_FRAMES,saved.frames,saved.images,saved.consecutive ? "true" : "false",saved.early ? "true" : "false",
        saved.truncated,PREVIEW_DRAWS,PREVIEW_W,PREVIEW_H,COUNTER_CAPACITY,
        (unsigned long long)(counter_saved_issued ? counter_saved_first : 0),(unsigned long long)(counter_saved_issued ? counter_saved_last : 0),
        (unsigned long long)counter_saved_issued,(unsigned long long)counter_saved_stored,
        (unsigned long long)(counter_saved_issued-counter_saved_stored));
}
