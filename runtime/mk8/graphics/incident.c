// Opt-in graphics evidence. No guest buffer dumps, renderer pointers, or per-draw I/O.
#include "incident.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <windows.h>
#include <bcrypt.h>
#include "mk8_incident_build.h"

#define GFX_CAPACITY 4096
#define GFX_MAX_INCIDENTS 16
#define GFX_RESERVE (4ull << 30)
#define GFX_OUTPUT_BUDGET (128ull << 20)

typedef struct {
    uint64_t sequence, ns, frame, vs, fs, gs, pipeline;
    int kind, state, mode, redrawable, budget_miss, remaining_us;
    uint64_t generation;
    uint32_t data[6];
    bool frame_sample;
} GfxEvent;
typedef struct { atomic_bool busy; GfxEvent event; } GfxSlot;
static GfxSlot ring[GFX_CAPACITY];
static atomic_uint_fast64_t issued, lost;
static atomic_int enabled = -1;
static atomic_uint_fast64_t request_ns;
static atomic_uint_fast64_t last_present, checkpoint_drops;
// Four transition types, retaining first/latest even when the rolling window expires.
typedef struct { atomic_bool busy; GfxEvent first, latest; } GfxCheckpoint;
static GfxCheckpoint checkpoints[4];
static GfxEvent checkpoint_snapshot[8];
static unsigned checkpoint_count;
static uint64_t snapshot_checkpoint_drops, swapchain_generation;
static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER frequency;
static unsigned incidents;
static char active[240];
static uint64_t active_frame, active_request, active_start;
static uint32_t active_w, active_h, active_format, active_driver, active_vendor, active_device;
static GfxEvent snapshot[GFX_CAPACITY];
static unsigned snapshot_count;
static bool png_success;
static bool active_framegen;
static uint64_t snapshot_issued, snapshot_lost;

static uint64_t clock_ns(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / frequency.QuadPart) * 1000000000ull +
           (uint64_t)(t.QuadPart % frequency.QuadPart) * 1000000000ull / frequency.QuadPart;
}
uint64_t mk8_gfx_time_ns(void) { return clock_ns(); }
uint64_t mk8_gfx_token(uint64_t value) {
    if (!value) return 0;
    value^=value>>33; value*=0xff51afd7ed558ccdull;
    value^=value>>33; value*=0xc4ceb9fe1a85ec53ull;
    return value^(value>>33);
}
static BOOL CALLBACK initialize(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once; (void)parameter; (void)context;
    QueryPerformanceFrequency(&frequency);
    const char *e = getenv("MK8_GFX_DIAGNOSTICS");
    atomic_store(&enabled, e && !strcmp(e, "1"));
    return TRUE;
}
bool mk8_gfx_enabled(void) {
    int on = atomic_load_explicit(&enabled, memory_order_acquire);
    if (on < 0) { InitOnceExecuteOnce(&init_once, initialize, NULL, NULL); on = atomic_load(&enabled); }
    return on != 0;
}
void mk8_gfx_request(void) {
    if (!mk8_gfx_enabled()) return;
    uint64_t empty = 0;
    // Coalesce repeated presses without moving the original request time.
    atomic_compare_exchange_strong(&request_ns, &empty, clock_ns());
}
static void record(GfxEvent e) {
    e.sequence = atomic_fetch_add(&issued, 1) + 1;
    GfxSlot *s = &ring[(e.sequence - 1) % GFX_CAPACITY];
    // No waiting, allocation, I/O, or global logging mutex on this path.
    if (atomic_exchange_explicit(&s->busy, true, memory_order_acquire)) {
        atomic_fetch_add(&lost, 1);
        return;
    }
    bool accepted=s->event.sequence < e.sequence;
    if (accepted) { e.ns = clock_ns(); s->event = e; }
    else atomic_fetch_add(&lost, 1); // delayed writer must not replace a newer wrap
    atomic_store_explicit(&s->busy, false, memory_order_release);
    if (accepted && e.kind>=3 && e.kind<=6) {
        GfxCheckpoint *c=&checkpoints[e.kind-3];
        if (atomic_exchange_explicit(&c->busy,true,memory_order_acquire)) {
            atomic_fetch_add(&checkpoint_drops,1);
            return;
        }
        if (!c->first.sequence || e.sequence<c->first.sequence) c->first=e;
        if (e.sequence>c->latest.sequence) c->latest=e;
        atomic_store_explicit(&c->busy,false,memory_order_release);
    }
}

void mk8_gfx_fullscreen_request(unsigned source, bool wanted) {
    if (!mk8_gfx_enabled()) return;
    record((GfxEvent){.kind=3,.frame=atomic_load(&last_present),.frame_sample=true,.data={source,wanted}});
}
void mk8_gfx_fullscreen_apply(bool wanted, bool success, bool reported, unsigned w, unsigned h) {
    if (!mk8_gfx_enabled()) return;
    record((GfxEvent){.kind=4,.frame=atomic_load(&last_present),.frame_sample=true,.data={wanted,success,reported,w,h}});
}
void mk8_gfx_swapchain(uint64_t frame, unsigned w, unsigned h, unsigned format) {
    if (!mk8_gfx_enabled()) return;
    record((GfxEvent){.kind=5,.frame=frame,.generation=++swapchain_generation,.data={w,h,format}});
}
void mk8_gfx_scale(uint64_t frame, float x, float y) {
    if (!mk8_gfx_enabled()) return;
    GfxEvent e={.kind=6,.frame=frame};
    memcpy(&e.data[0],&x,sizeof x); memcpy(&e.data[1],&y,sizeof y);
    record(e);
}
void mk8_gfx_occlusion_result(uint64_t submission,uint64_t report,uint64_t value,bool available) {
    if (!mk8_gfx_preview_active()) return;
    record((GfxEvent){.kind=7,.frame=atomic_load(&last_present),.frame_sample=true,
        .generation=submission,.vs=mk8_gfx_token(report),.pipeline=value,.data={available}});
}
void mk8_gfx_skip(uint64_t frame, const uint64_t code[3], uint64_t pipeline,
                  int state, int mode, bool redrawable, bool budget_miss, double remaining_ms) {
    if (!mk8_gfx_enabled()) return;
    double us = remaining_ms * 1000;
    int remaining = us != us ? 0 : us > INT_MAX ? INT_MAX : us < INT_MIN ? INT_MIN : (int)us;
    record((GfxEvent){.frame=frame, .vs=code ? code[0] : 0, .fs=code ? code[1] : 0,
        .gs=code ? code[2] : 0, .pipeline=pipeline, .kind=1, .state=state, .mode=mode,
        .redrawable=redrawable, .budget_miss=budget_miss, .remaining_us=remaining});
}
static int compare_event(const void *a, const void *b) {
    uint64_t x = ((const GfxEvent *)a)->sequence, y = ((const GfxEvent *)b)->sequence;
    return x > y ? 1 : x < y ? -1 : 0;
}
static void take_snapshot(void) {
    snapshot_count = 0;
    snapshot_issued = atomic_load(&issued);
    for (unsigned i=0; i<GFX_CAPACITY; i++) {
        GfxSlot *s = &ring[i];
        if (atomic_exchange_explicit(&s->busy, true, memory_order_acquire)) continue;
        GfxEvent e = s->event;
        atomic_store_explicit(&s->busy, false, memory_order_release);
        if (e.sequence && e.sequence <= snapshot_issued && snapshot_issued-e.sequence < GFX_CAPACITY)
            snapshot[snapshot_count++] = e;
    }
    qsort(snapshot, snapshot_count, sizeof *snapshot, compare_event);
    snapshot_lost = atomic_load(&lost);
    checkpoint_count=0;
    for (unsigned i=0;i<4;i++) {
        GfxCheckpoint *c=&checkpoints[i];
        if (atomic_exchange_explicit(&c->busy,true,memory_order_acquire)) {
            atomic_fetch_add(&checkpoint_drops,1);
            continue;
        }
        GfxEvent first=c->first, latest=c->latest;
        atomic_store_explicit(&c->busy,false,memory_order_release);
        if (first.sequence && first.sequence<=snapshot_issued) checkpoint_snapshot[checkpoint_count++]=first;
        if (latest.sequence!=first.sequence && latest.sequence<=snapshot_issued)
            checkpoint_snapshot[checkpoint_count++]=latest;
    }
    qsort(checkpoint_snapshot,checkpoint_count,sizeof *checkpoint_snapshot,compare_event);
    snapshot_checkpoint_drops=atomic_load(&checkpoint_drops);
}
// Reject reparse points in the entire output ancestry. Fixed relative destination only.
static bool safe_directory(const char *path) {
    char full[MAX_PATH];
    DWORD n = GetFullPathNameA(path, sizeof full, full, NULL);
    if (!n || n >= sizeof full || full[1] != ':' || full[2] != '\\') return false;
    for (char *p=full+3;; p++) {
        if (*p && *p != '\\') continue;
        char ch=*p; *p=0;
        DWORD attr=GetFileAttributesA(full);
        *p=ch;
        if (attr==INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (!ch) return true;
    }
}
static bool enough_space(void) {
    ULARGE_INTEGER free_bytes;
    return GetDiskFreeSpaceExA("graphics-incidents", &free_bytes, NULL, NULL) &&
           free_bytes.QuadPart >= GFX_RESERVE + GFX_OUTPUT_BUDGET;
}
static void failure(void) { fprintf(stderr, "[gfx-incident] capture failed; incomplete evidence retained locally\n"); }

bool mk8_gfx_begin(uint64_t frame, bool screenshot_busy, uint32_t w, uint32_t h,
                   uint32_t format, uint32_t driver, uint32_t vendor, uint32_t device, bool framegen,
                   char *path, unsigned capacity) {
    if (!mk8_gfx_enabled()) return false;
    atomic_store(&last_present,frame);
    record((GfxEvent){.kind=2, .frame=frame});
    if (mk8_gfx_preview_present(frame,screenshot_busy,atomic_load(&request_ns)!=0,w,h,path,capacity)) return true;
    if (screenshot_busy || !atomic_load(&request_ns) || *active) return false;
    active_request=atomic_exchange(&request_ns, 0);
    if (incidents >= GFX_MAX_INCIDENTS || !w || !h || (uint64_t)w*h > 16777216 ||
        !safe_directory(".")) { failure(); return false; }
    if (!CreateDirectoryA("graphics-incidents", NULL) && GetLastError()!=ERROR_ALREADY_EXISTS) { failure(); return false; }
    if (!safe_directory("graphics-incidents") || !enough_space()) { failure(); return false; }
    active_start=clock_ns();
    snprintf(active, sizeof active, "graphics-incidents/incident-%lu-%llu", (unsigned long)GetCurrentProcessId(),
             (unsigned long long)active_start);
    if (!CreateDirectoryA(active, NULL)) { *active=0; failure(); return false; }
    int n=snprintf(path, capacity, "%s/screenshot.pending.png", active);
    if (n<0 || (unsigned)n>=capacity) { *active=0; failure(); return false; }
    active_frame=frame; active_w=w; active_h=h; active_format=format;
    active_driver=driver; active_vendor=vendor; active_device=device;
    active_framegen=framegen;
    take_snapshot();
    png_success=false;
    incidents++;
    fprintf(stderr, "[gfx-incident] capture host frame %llu\n", (unsigned long long)frame);
    return true;
}

void mk8_gfx_png_result(const char *path, bool success) {
    mk8_gfx_preview_png_result(path,success);
    char expected[300];
    if (!*active) return;
    snprintf(expected, sizeof expected, "%s/screenshot.pending.png", active);
    if (!strcmp(path, expected)) png_success=success;
}

static bool exe_hash(char hex[65]) {
    char path[MAX_PATH];
    DWORD n=GetModuleFileNameA(NULL, path, sizeof path);
    if (!n || n>=sizeof path) return false;
    FILE *f=fopen(path, "rb");
    if (!f) return false;
    BCRYPT_ALG_HANDLE alg=NULL;
    BCRYPT_HASH_HANDLE hash=NULL;
    unsigned char bytes[32768], digest[32];
    bool ok=BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)>=0;
    if (ok) ok=BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0)>=0;
    size_t got;
    while (ok && (got=fread(bytes, 1, sizeof bytes, f))) ok=BCryptHashData(hash, bytes, (ULONG)got, 0)>=0;
    ok=ok && !ferror(f);
    fclose(f);
    if (ok) ok=BCryptFinishHash(hash, digest, sizeof digest, 0)>=0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (ok) for (unsigned i=0; i<32; i++) snprintf(hex+i*2, 3, "%02x", digest[i]);
    return ok;
}
static FILE *open_artifact(const char *name, char path[300]) {
    snprintf(path, 300, "%s/%s.pending", active, name);
    return fopen(path, "wbx");
}
static bool close_artifact(FILE *f, const char *pending, const char *name) {
    bool ok=!ferror(f);
    if (fclose(f)) ok=false;
    char final[300]; snprintf(final, sizeof final, "%s/%s", active, name);
    return ok && MoveFileExA(pending, final, MOVEFILE_WRITE_THROUGH);
}
// Only explicitly selected scalar flags. Never copy the environment or personal paths.
static void flag(FILE *f, const char *name) {
    const char *v=getenv(name);
    bool safe=v && strlen(v)<=32;
    if (safe) for (const char *p=v; *p; p++)
        if (!((*p>='0' && *p<='9') || (*p>='a' && *p<='z') || *p=='-' || *p=='.' || *p==',')) safe=false;
    fprintf(f, "\"%s\":%s%s%s", name, safe ? "\"" : "", safe ? v : "null", safe ? "\"" : "");
}
static void write_event(FILE *f,const GfxEvent *e) {
    static const char *names[]={"unknown","shader_skip","present","fullscreen_request","fullscreen_apply","swapchain","render_scale","occlusion_report"};
    fprintf(f,"{\"sequence\":%llu,\"host_ns\":%llu,\"host_frame\":%llu,\"frame_sample\":%s,\"kind\":\"%s\"",
        (unsigned long long)e->sequence,(unsigned long long)e->ns,(unsigned long long)e->frame,
        e->frame_sample ? "true" : "false",names[e->kind>=1 && e->kind<=7 ? e->kind : 0]);
    if (e->kind==1) fprintf(f,
        ",\"vs_code_va\":\"%016llx\",\"fs_code_va\":\"%016llx\",\"gs_code_va\":\"%016llx\","
        "\"pipeline_key\":\"%016llx\",\"build_state_sample\":%d,\"shader_mode\":%d,\"redrawable\":%d,"
        "\"budget_miss\":%d,\"remaining_before_wait_us\":%d",
        (unsigned long long)e->vs,(unsigned long long)e->fs,(unsigned long long)e->gs,
        (unsigned long long)e->pipeline,e->state,e->mode,e->redrawable,e->budget_miss,e->remaining_us);
    if (e->kind==3) fprintf(f,",\"source\":%u,\"requested_fullscreen\":%s",e->data[0],e->data[1] ? "true" : "false");
    if (e->kind==4) fprintf(f,",\"requested_fullscreen\":%s,\"sdl_success\":%s,\"reported_fullscreen\":%s,\"pixel_width\":%u,\"pixel_height\":%u",
        e->data[0] ? "true" : "false",e->data[1] ? "true" : "false",e->data[2] ? "true" : "false",e->data[3],e->data[4]);
    if (e->kind==5) fprintf(f,",\"generation\":%llu,\"width\":%u,\"height\":%u,\"vk_format\":%u",
        (unsigned long long)e->generation,e->data[0],e->data[1],e->data[2]);
    if (e->kind==6) {
        float x,y; memcpy(&x,&e->data[0],sizeof x); memcpy(&y,&e->data[1],sizeof y);
        fputs(",\"scale_x\":",f); if (isfinite(x)) fprintf(f,"%.9g",(double)x); else fputs("null",f);
        fputs(",\"scale_y\":",f); if (isfinite(y)) fprintf(f,"%.9g",(double)y); else fputs("null",f);
    }
    if (e->kind==7) fprintf(f,",\"submission\":%llu,\"report_token\":\"%016llx\",\"samples\":%llu,\"query_available\":%s",
        (unsigned long long)e->generation,(unsigned long long)e->vs,(unsigned long long)e->pipeline,e->data[0] ? "true" : "false");
    fputc('}',f);
}
static Mk8GfxPngWriter capture_writer;
void mk8_gfx_finish(Mk8GfxPngWriter writer) { capture_writer=writer; mk8_gfx_end(); capture_writer=NULL; }
void mk8_gfx_end(void) {
    if (!*active) { mk8_gfx_preview_frame_done(); return; }
    char pending[300], final[300], path[300], hash[65]="";
    snprintf(pending, sizeof pending, "%s/screenshot.pending.png", active);
    snprintf(final, sizeof final, "%s/screenshot.png", active);
    WIN32_FILE_ATTRIBUTE_DATA attr;
    bool ok=png_success && GetFileAttributesExA(pending, GetFileExInfoStandard, &attr) &&
        !attr.nFileSizeHigh && attr.nFileSizeLow>8 && attr.nFileSizeLow<=GFX_OUTPUT_BUDGET &&
        MoveFileExA(pending, final, MOVEFILE_WRITE_THROUGH);
    if (!ok || !safe_directory(active) || !enough_space() || !exe_hash(hash)) goto failed;
    if (!mk8_gfx_preview_dump(active,capture_writer,active_frame)) goto failed;
    FILE *f=open_artifact("events.jsonl", path);
    if (!f) goto failed;
    for (unsigned i=0; i<snapshot_count; i++) {
        write_event(f,&snapshot[i]); fputc('\n',f);
    }
    if (!close_artifact(f, path, "events.jsonl")) goto failed;
    f=open_artifact("summary.txt", path);
    if (!f) goto failed;
    fprintf(f, "Graphics evidence, not a graphics fix.\nHost frames count renderer present attempts (vk_frame_counter + 1), not guest ticks, generated frames or GPU completion.\n"
        "Shader IDs are code VAs for the supported version, not content hashes. Pipeline keys exclude host pointers.\n"
        "Bounded pre-capture history only; request may be deferred by existing screenshots. No post-trigger window.\n"
        "Present markers precede readback/present. Screenshot uses existing window capture/crop, includes overlay, and disturbs timing.\n"
        "Build state: 0 pending, 1 ready, 2 failed, -1 missing. Shader mode: 0 prefetch, 1 synchronous, 2 async skip.\n"
        "Build state is sampled after rejection and can change concurrently; budget_miss observes an unsuccessful bounded wait.\n"
        "Sequence order is reservation order; concurrent timestamps need not be ordered. Gaps/old overwritten events are not proof of no skipped draws.\n"
        "Window-thread events use an atomic sample of the last present marker (frame_sample=true), not exact draw ownership.\n"
        "Fullscreen source: 1 F11, 2 other shortcut, 3 menu checkbox. Apply records SDL return and instantaneous flags/size; completion may be asynchronous.\n"
        "Swapchain generations count successful creations, even at the same extent. Render-scale events contain the assigned scale.\n"
        "Transition checkpoints retain first/latest by kind outside the rolling window; they are not a continuous history.\n"
        "F7 collects a bounded 60-frame preview: up to 320x180 thumbnails and 1536 command records/frame in memory, then writes files.\n"
        "Preview readbacks disturb timing; missing images and truncated draws are explicit. F8 can end a preview early.\n"
        "vk_calls counts recorded Vulkan draw calls, not GPU completion or visible pixels. render_off bypasses the ordinary skipped-draw counter.\n"
        "Depth-state bits: bit 0 test, bit 1 write, compare function in bits 8..15. Viewport/scissor are command state; pass extent is at emission.\n"
        "Geometry/report tokens are numeric same-run hashes, not mesh identities; no vertex/index buffers are dumped.\n"
        "Occlusion reports include submission, availability and cumulative value at resolution. They use sampled host frames, not GPU frame IDs.\n"
        "F7 counters.jsonl records exact compare operands and post-operation queue/write/reset observations.\nObserved completed submission is a lower bound from existing completion observations, not a poll at comparison time.\nIDs order observer reservations, not GPU execution or atomic guest writes; null values and missing records are explicit.\nReview screenshots before sharing; no automatic upload.\n");
    if (!close_artifact(f, path, "summary.txt")) goto failed;
    f=open_artifact("manifest.json", path);
    if (!f) goto failed;
    fprintf(f, "{\"schema\":4,\"complete\":true,\"gameplay_verified\":false,\"executable_sha256\":\"%s\","
        "\"source_commit\":\"%s\",\"source_branch\":\"%s\",\"source_dirty\":%s,"
        "\"request_ns\":%llu,\"capture_start_ns\":%llu,\"capture_end_ns\":%llu,\"host_frame\":%llu,"
        "\"swapchain_width\":%u,\"swapchain_height\":%u,\"vk_format\":%u,\"driver_version\":%u,\"vendor_id\":%u,\"device_id\":%u,\"framegen_enabled\":%s,"
        "\"capacity\":%u,\"events\":%u,\"issued\":%llu,\"contention_drops\":%llu,"
        "\"window_missing\":%llu,\"flags\":{",
        hash, MK8_INCIDENT_COMMIT, MK8_INCIDENT_BRANCH, MK8_INCIDENT_DIRTY,
        (unsigned long long)active_request, (unsigned long long)active_start, (unsigned long long)clock_ns(),
        (unsigned long long)active_frame, active_w, active_h, active_format, active_driver, active_vendor, active_device,
        active_framegen ? "true" : "false",
        GFX_CAPACITY, snapshot_count, (unsigned long long)snapshot_issued, (unsigned long long)snapshot_lost,
        (unsigned long long)((snapshot_issued<GFX_CAPACITY ? snapshot_issued : GFX_CAPACITY)-snapshot_count));
    const char *flags[]={"MK8_SHADER_WAIT_MS", "LARECOMP_ASYNC_SHADERS", "LARECOMP_AHEAD", "MK8_ASYNC_SUBMIT",
        "LARECOMP_NO_SCALE", "LARECOMP_SCALE", "MK8_CAPTURE_REGION", "MK8_FRAME_TIMES", "MK8_TRACE_VRAM",
        "MK8_OCCLUSION_VISIBLE","MK8_OCCLUSION_ZERO","MK8_OCCLUSION_APPROXIMATE","LARECOMP_NO_DEPTH_TEST","LARECOMP_VK_COPY_VERTICES"};
    for (unsigned i=0; i<sizeof flags/sizeof *flags; i++) { if (i) fputc(',', f); flag(f, flags[i]); }
    fprintf(f, "},\"checkpoint_drops\":%llu,\"transition_checkpoints\":[",(unsigned long long)snapshot_checkpoint_drops);
    for (unsigned i=0;i<checkpoint_count;i++) { if (i) fputc(',',f); write_event(f,&checkpoint_snapshot[i]); }
    fputs("],\"preview\":",f); mk8_gfx_preview_manifest(f); fputs("}\n",f);
    if (!close_artifact(f, path, "manifest.json")) goto failed;
    fprintf(stderr, "[gfx-incident] saved %s (host frame %llu)\n", active, (unsigned long long)active_frame);
    *active=0;
    return;
failed:
    failure();
    *active=0;
}
