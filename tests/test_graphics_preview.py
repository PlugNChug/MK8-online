"""Bounded preview images and actual prepared draw/conditional-render command paths."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'toolchain'))
from host import DEPS,prepare_host_sources
from native import DEFAULT_COMPILER,compiler_env

HARNESS=r'''
#include <assert.h>
#include <windows.h>
#include <SDL3/SDL_events.h>
#include "incident.c"
#define LOG(...) ((void)0)
static bool g_debug_frame;
static unsigned stat_skipped,unhandled,gpu_calls;
SKIP_MACRO
INPUT_HOOK
    unhandled++;
}
typedef struct { unsigned width,height; } Backend;
typedef Backend TexBackend;
typedef struct { void *backend; } NvnTexture;
typedef struct { struct { uint64_t code_addr; } stages[3]; } Program;
typedef struct { uint64_t x[5]; } CmdRec;
static uint64_t frame_no;
static uint64_t vk_frame_counter(void) { return frame_no-1; }
static uint64_t vk_pending_seq(void) { return 43; }
static struct {
    bool render_off; Program *program; unsigned poly[3],ds[3];
    struct { uint64_t addr; } vbuf[1]; float viewport[1][4]; int32_t scissor[1][4];
    struct { int ncolor; NvnTexture *color[1],*depth; } rt;
} st;
PREVIEW_HELPER
static void draw(CmdRec *r,bool indexed) {
    if (r->x[2]==7) SKIP_DRAW();
    unsigned pass_w=768,pass_h=432;
EMIT_HOOK
    gpu_calls++;
}
static bool mk8_cw_any;
static unsigned mk8_cw_reports;
static void mk8_cw_flush(const char *why) { abort(); }
static uint64_t plat_now_ns(void) { return 1000; }
static uint64_t ns_to_gpu_ticks(uint64_t n) { return n; }
enum {CMD_SET_RENDER_ENABLE,CMD_DRAW_ARRAYS,CMD_DRAW_ELEMENTS,CMD_REPORT_VALUE};
static void command(int kind,CmdRec *r) {
    switch(kind) {
CONDITION_CASE
DRAW_ARRAYS
DRAW_ELEMENTS
VALUE_CASE
    }
}
typedef struct { unsigned pool,n,nrep,dirty; bool active; uint64_t seq;
    uint64_t *rep_out[256]; unsigned rep_after[256]; } TestQueries;
static unsigned mk8_occlusion_flags,query_begins,query_ends;
static void vkCmdBeginQuery(unsigned cb,unsigned pool,unsigned index,unsigned flags) { query_begins++; }
static void vkCmdEndQuery(unsigned cb,unsigned pool,unsigned index) { query_ends++; }
static void queue_report(TestQueries *o,bool reset_samples,uint64_t *out) {
    unsigned qcb=0;
QUEUE_HOOK
}
static DWORD WINAPI counter_writer(void *arg) {
    for (unsigned i=0;i<5000;i++) mk8_gfx_counter_value(101,43,(uintptr_t)arg,i);
    return 0;
}
static bool fail_png;
static unsigned char *rgba;
static void writer(const char *path,const uint8_t *data,uint32_t w,uint32_t h) {
    if (mk8_gfx_preview_image(path,data,w,h)) return;
    if (fail_png && strstr(path,"/preview/")) { mk8_gfx_png_result(path,false); return; }
    FILE *f=fopen(path,"wb"); assert(f);
    uint32_t authored[4]={w,h,data ? data[0] : 0,0x12345678};
    assert(fwrite(authored,1,sizeof authored,f)==sizeof authored); assert(!fclose(f));
    mk8_gfx_png_result(path,true);
}
static void key(SDL_Scancode sc,bool repeat) {
    SDL_Event e={.type=SDL_EVENT_KEY_DOWN}; e.key.scancode=sc; e.key.repeat=repeat; input_handle_event(&e);
}
int main(int argc,char **argv) {
    assert(argc==2); const char *mode=argv[1];
    bool on=mk8_gfx_enabled(); char path[512]="pending";
    key(SDL_SCANCODE_F7,true); assert(!mk8_gfx_preview_active());
    key(SDL_SCANCODE_F7,false);
    assert(!mk8_gfx_begin(100,false,1280,720,44,0,0,0,false,path,sizeof path));
    if (!on) {
        assert(!mk8_gfx_counter_compare(1,2,1,0,true,1,2,false,false));
        mk8_gfx_counter_complete(999); mk8_gfx_counter_value(1,2,3,4);
        assert(!mk8_gfx_preview_active() && !atomic_load(&issued) && unhandled==2); return 0;
    }
    assert(mk8_gfx_preview_active());
    rgba=malloc(1280*720*4); assert(rgba); memset(rgba,127,1280*720*4);
    Backend color={768,432},depth={768,432}; NvnTexture ct={&color},dt={&depth};
    Program p={.stages={{17},{18},{0}}}; st.program=&p; st.rt.ncolor=1; st.rt.color[0]=&ct; st.rt.depth=&dt;
    st.vbuf[0].addr=1234; st.ds[0]=1; st.ds[1]=1; st.ds[2]=4; st.poly[0]=2;
    st.viewport[0][2]=1920; st.viewport[0][3]=1080; st.scissor[0][2]=1920; st.scissor[0][3]=1080;
    bool short_capture=!strcmp(mode,"early") || !strcmp(mode,"limits") || !strncmp(mode,"counter",7);
    unsigned total=short_capture ? 3 : 60;
    for (unsigned i=0;i<total;i++) {
        frame_no=101+i;
        uint64_t q[4]={1,0,2,0}; CmdRec cond={.x={0,(uint64_t)(uintptr_t)q,1}};
        command(CMD_SET_RENDER_ENABLE,&cond); assert(st.render_off);
        CmdRec r={.x={4,0,6,123,1}};
        command(CMD_DRAW_ARRAYS,&r);
        cond.x[0]=1; command(CMD_SET_RENDER_ENABLE,&cond); assert(!st.render_off);
        command(CMD_DRAW_ARRAYS,&r); r.x[2]=7; command(CMD_DRAW_ELEMENTS,&r);
        if (!strcmp(mode,"limits") && i==0) for(unsigned j=0;j<1540;j++) command(CMD_DRAW_ARRAYS,&r);
        if (!strcmp(mode,"counters") && i==0) {
            TestQueries o={.pool=1,.n=1,.seq=44,.active=true};
            queue_report(&o,false,q); queue_report(&o,false,q+2); queue_report(&o,true,NULL);
            assert(o.nrep==3 && o.n==4 && query_begins==3 && query_ends==3);
            o.pool=0; queue_report(&o,false,q); assert(o.nrep==3);
            q[0]=q[2]=0; cond.x[0]=1; command(CMD_SET_RENDER_ENABLE,&cond); assert(st.render_off);
            mk8_gfx_counter_submit(frame_no,44); mk8_gfx_counter_complete(43);
            q[0]=9; mk8_gfx_counter_write(44,(uintptr_t)q,9,0,9,0,1,4,true);
            command(CMD_SET_RENDER_ENABLE,&cond); assert(!st.render_off);
            CmdRec value={.x={5,(uintptr_t)(q+2)}}; command(CMD_REPORT_VALUE,&value); assert(q[2]==5);
            mk8_gfx_counter_reset(44,2,9,5); mk8_gfx_counter_complete(42);
            uint8_t *pages=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); assert(pages);
            DWORD old; assert(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old));
            uint64_t *edge=(void *)(pages+4096-24); edge[0]=UINT64_MAX; edge[2]=UINT64_MAX-1;
            cond.x[1]=(uintptr_t)edge; command(CMD_SET_RENDER_ENABLE,&cond); assert(!st.render_off);
            VirtualFree(pages,0,MEM_RELEASE);
            cond.x[1]=0; command(CMD_SET_RENDER_ENABLE,&cond); assert(st.render_off);
        }
        if (!strcmp(mode,"counter_threads") && i==0) {
            HANDLE threads[4];
            for(unsigned j=0;j<4;j++) { threads[j]=CreateThread(NULL,0,counter_writer,(void *)(uintptr_t)(j+1),0,NULL); assert(threads[j]); }
            assert(WaitForMultipleObjects(4,threads,true,10000)==WAIT_OBJECT_0);
            for(unsigned j=0;j<4;j++) CloseHandle(threads[j]);
        }
        mk8_gfx_occlusion_result(42,123,(uint64_t)i*10,(i&1)==0);
        if (short_capture && i==total-1) key(SDL_SCANCODE_F8,false);
        bool busy=(!strcmp(mode,"conflict") && i==4) || (!strcmp(mode,"delayed") && i==59);
        snprintf(path,sizeof path,"pending");
        bool capture=mk8_gfx_begin(frame_no,busy,1280,720,44,0,0,0,false,path,sizeof path);
        if (busy) assert(!capture && !strcmp(path,"pending"));
        if (capture) {
            if (!strcmp(mode,"bounds") && path[0]=='@') assert(mk8_gfx_preview_image(path,NULL,UINT32_MAX,UINT32_MAX));
            else writer(path,rgba,1280,720);
            if (!strcmp(mode,"failure") && i==total-1) fail_png=true;
            mk8_gfx_finish(writer);
        }
    }
    if (!strcmp(mode,"delayed")) {
        frame_no++; assert(mk8_gfx_begin(frame_no,false,1280,720,44,0,0,0,false,path,sizeof path));
        writer(path,rgba,1280,720); mk8_gfx_finish(writer);
    }
    if (!strcmp(mode,"counter_threads")) {
        key(SDL_SCANCODE_F7,false);
        assert(!mk8_gfx_begin(200,false,1280,720,44,0,0,0,false,path,sizeof path));
        for(unsigned i=0;i<3;i++) {
            frame_no=201+i; mk8_gfx_counter_value(frame_no,45,55,i);
            if(i==2) key(SDL_SCANCODE_F8,false);
            if(mk8_gfx_begin(frame_no,false,1280,720,44,0,0,0,false,path,sizeof path)) {
                writer(path,rgba,1280,720); mk8_gfx_finish(writer);
            }
        }
    }
    assert(!mk8_gfx_preview_active());
    uint64_t before=atomic_load(&issued); mk8_gfx_occlusion_result(999,1,0,false); assert(atomic_load(&issued)==before);
    assert(gpu_calls==total && stat_skipped>=total);
    free(rgba); return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(),'Native compiler required')
class GraphicsPreviewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory()
        cls.root=Path(cls.temp.name)
        runtime=prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',cls.root)
        text=(runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
        inp=(runtime/'hle/input.c').read_text(encoding='utf-8')
        inp=inp[inp.index('void input_handle_event(const SDL_Event *e) {'):]
        inp=inp[:inp.index('    extern bool touchpad_handle_event')]
        helper=text[text.index('static void mk8_preview_command('):text.index('static void exec_list(uint64_t handle, int depth) {')]
        condition=text[text.index('        case CMD_SET_RENDER_ENABLE:',text.index('static void exec_list(')):]
        condition=condition[:condition.index('        case CMD_REPORT_VALUE:')]
        arrays=text[text.index('        case CMD_DRAW_ARRAYS:',text.index('static void exec_list(')):]
        arrays=arrays[:arrays.index('        case CMD_DRAW_ELEMENTS:')]
        elements=text[text.index('        case CMD_DRAW_ELEMENTS:',text.index('static void exec_list(')):]
        elements=elements[:elements.index('        case CMD_UPDATE_UNIFORM_BUFFER:')]
        value=text[text.index('        case CMD_REPORT_VALUE:',text.index('static void exec_list(')):]
        value=value[:value.index('        case CMD_CLEAR_BUFFER:')]
        queue=text[text.index('                    if (o->pool && o->nrep < MK8_OQ_MAX) {'):]
        queue=queue[:queue.index('                    pthread_mutex_unlock(&cq_lock);')]
        macro=next(line for line in text.splitlines() if line.startswith('#define SKIP_DRAW()'))
        source=HARNESS.replace('SKIP_MACRO',macro).replace('INPUT_HOOK',inp).replace('PREVIEW_HELPER',helper)
        source=source.replace('CONDITION_CASE',condition).replace('DRAW_ARRAYS\n',arrays).replace('DRAW_ELEMENTS\n',elements)
        source=source.replace('VALUE_CASE',value).replace('QUEUE_HOOK',queue).replace('MK8_OQ_MAX','256')
        source=source.replace('EMIT_HOOK','    mk8_gfx_preview_emit(pass_w, pass_h);')
        (cls.root/'test.c').write_text(source,encoding='utf-8')
        cls.exe=cls.root/'preview.exe'
        build=subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O2','-UNDEBUG',
            '-I'+str(runtime),'-I'+str(DEPS/'include'),'-I'+str(ROOT/'runtime/mk8/graphics'),str(cls.root/'test.c'),
            str(ROOT/'runtime/mk8/graphics/preview.c'),'-lbcrypt','-o',str(cls.exe)],
            env=compiler_env(),capture_output=True,text=True)
        if build.returncode: raise RuntimeError(build.stderr)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def run_case(self,mode):
        folder=self.root/mode; folder.mkdir()
        run=subprocess.run([str(self.exe),mode],cwd=folder,capture_output=True,text=True,timeout=30,
            env=dict(os.environ,MK8_GFX_DIAGNOSTICS='0' if mode=='disabled' else '1'))
        self.assertEqual(run.returncode,0,run.stdout+run.stderr)
        manifests=list(folder.glob('graphics-incidents/*/manifest.json'))
        return folder,manifests,run

    def test_f7_disabled_preserves_input_and_creates_no_artifacts(self):
        folder,manifests,_=self.run_case('disabled')
        self.assertFalse(manifests); self.assertFalse((folder/'graphics-incidents').exists())

    def test_consecutive_images_and_prepared_conditional_draw_outcomes(self):
        _,manifests,_=self.run_case('capture'); self.assertEqual(len(manifests),1)
        p=manifests[0]; m=json.loads(p.read_text()); preview=m['preview']
        self.assertEqual(preview['frames'],60); self.assertEqual(preview['images'],60)
        self.assertTrue(preview['consecutive']); self.assertFalse(preview['early_stop'])
        frames=[json.loads(line) for line in (p.parent/'preview/frames.jsonl').read_text().splitlines()]
        draws=[json.loads(line) for line in (p.parent/'preview/draws.jsonl').read_text().splitlines()]
        self.assertEqual([f['host_frame'] for f in frames],list(range(101,161)))
        self.assertEqual(len(list((p.parent/'preview').glob('*.png'))),59)
        self.assertEqual(frames[-1]['image'],'../screenshot.png')
        self.assertTrue(all(f['conditional_off']==1 and f['vk_draw_calls']==1 for f in frames))
        self.assertEqual([d['render_off'] for d in draws[:3]],[1,0,0])
        self.assertEqual([d['vk_calls'] for d in draws[:3]],[0,1,0])
        self.assertTrue(draws[2]['skip_line']); self.assertEqual(draws[1]['pass_extent'],[768,432])
        self.assertEqual(draws[1]['viewport'],[0,0,1920,1080])
        events=[json.loads(line) for line in p.with_name('events.jsonl').read_text().splitlines()]
        reports=[e for e in events if e['kind']=='occlusion_report']
        self.assertEqual(len(reports),60); self.assertFalse(reports[1]['query_available'])

    def test_capture_conflicts_are_visible_as_image_gaps(self):
        _,manifests,_=self.run_case('conflict'); m=json.loads(manifests[0].read_text())
        self.assertFalse(m['preview']['consecutive']); self.assertEqual(m['preview']['images'],59)

    def test_compared_values_queue_writes_and_completion_observations_are_preserved(self):
        _,manifests,_=self.run_case('counters')
        p=manifests[0]; m=json.loads(p.read_text())
        self.assertEqual(m['schema'],4)
        rows=[json.loads(s) for s in (p.parent/'preview/counters.jsonl').read_text().splitlines()]
        self.assertEqual(m['preview']['counter_trace']['missing'],0)
        self.assertEqual(m['preview']['counter_trace']['stored'],len(rows))
        by_id={e['id']:e for e in rows}
        draws=[json.loads(s) for s in (p.parent/'preview/draws.jsonl').read_text().splitlines()]
        for draw in draws:
            event=by_id[draw['condition_id']]
            self.assertEqual(event['kind'],'compare')
            self.assertEqual(event['report_token'],draw['condition_token'])
            self.assertEqual(event['render_off'],bool(draw['render_off']))
        queue=[e for e in rows if e['kind']=='report_queue']
        self.assertEqual([e['accepted'] for e in queue],[True,True,True,False])
        self.assertEqual([e['queries_before'] for e in queue[:3]],[1,2,3])
        self.assertTrue(queue[2]['reset']); self.assertEqual(queue[2]['report_token'],'0000000000000000')
        stale=next(e for e in rows if e['kind']=='compare' and e['value_a']==e['value_b']==0)
        self.assertTrue(stale['render_off']); self.assertFalse(stale['completion_known'])
        write=next(e for e in rows if e['kind']=='report_write')
        self.assertGreater(write['id'],stale['id']); self.assertEqual(write['value'],9)
        self.assertEqual(write['observed_completed_submission'],43); self.assertEqual(write['submission'],44)
        after=next(e for e in rows if e['kind']=='compare' and e['value_a']==9)
        self.assertEqual(after['value_b'],0); self.assertFalse(after['render_off'])
        self.assertEqual(after['report_token'],write['report_token'])
        cpu=next(e for e in rows if e['kind']=='report_value')
        self.assertEqual(cpu['value'],5); self.assertEqual(cpu['report_token'],stale['second_report_token'])
        edge=next(e for e in rows if e['kind']=='compare' and e['value_a']==2**64-1)
        self.assertEqual(edge['value_b'],2**64-2); self.assertFalse(edge['equal'])
        null=next(e for e in rows if e['kind']=='compare' and not e['values_valid'])
        self.assertIsNone(null['value_a']); self.assertIsNone(null['value_b']); self.assertTrue(null['equal'])
        self.assertEqual(len([e for e in rows if e['kind']=='submission_complete']),1)
        self.assertEqual(next(e['submission'] for e in rows if e['kind']=='submission_sent'),44)
        self.assertTrue(any(e['kind']=='counter_reset' and e['base']==9 and e['sum']==5 for e in rows))

    def test_counter_ring_wrap_concurrent_writers_and_second_capture_are_explicit(self):
        _,manifests,_=self.run_case('counter_threads')
        self.assertEqual(len(manifests),2)
        manifests.sort(key=lambda p:json.loads(p.read_text())['host_frame'])
        first,second=[json.loads(p.read_text()) for p in manifests]
        stats=first['preview']['counter_trace']
        rows=[json.loads(s) for s in (manifests[0].parent/'preview/counters.jsonl').read_text().splitlines()]
        self.assertGreater(stats['issued'],20000); self.assertGreater(stats['missing'],0)
        self.assertEqual(stats['stored'],len(rows)); self.assertLessEqual(len(rows),stats['capacity'])
        self.assertEqual([e['id'] for e in rows],sorted({e['id'] for e in rows}))
        again=[json.loads(s) for s in (manifests[1].parent/'preview/counters.jsonl').read_text().splitlines()]
        self.assertEqual(second['preview']['counter_trace']['missing'],0)
        self.assertEqual(len(again),3); self.assertEqual([e['value'] for e in again],[0,1,2])
        self.assertGreater(again[0]['id'],rows[-1]['id'])

    def test_f8_can_end_the_bounded_window_early(self):
        _,manifests,_=self.run_case('early'); m=json.loads(manifests[0].read_text())
        self.assertTrue(m['preview']['early_stop']); self.assertEqual(m['preview']['frames'],3)

    def test_draw_bounds_report_truncation(self):
        _,manifests,_=self.run_case('limits'); m=json.loads(manifests[0].read_text())
        self.assertGreater(m['preview']['truncated_draws'],0)
        self.assertEqual(m['preview']['max_draws_per_frame'],1536)

    def test_invalid_readback_dimensions_are_not_reported_as_complete_images(self):
        _,manifests,_=self.run_case('bounds'); m=json.loads(manifests[0].read_text())
        self.assertFalse(m['preview']['consecutive']); self.assertEqual(m['preview']['images'],1)

    def test_failed_preview_png_does_not_publish_a_complete_bundle(self):
        _,manifests,run=self.run_case('failure')
        self.assertFalse(manifests); self.assertIn('capture failed',run.stderr)

    def test_deferred_final_capture_does_not_mislabel_its_frame(self):
        _,manifests,_=self.run_case('delayed'); m=json.loads(manifests[0].read_text())
        self.assertEqual(m['host_frame'],161); self.assertFalse(m['preview']['consecutive'])
        rows=[json.loads(line) for line in (manifests[0].parent/'preview/frames.jsonl').read_text().splitlines()]
        self.assertEqual(rows[-1]['host_frame'],160); self.assertIsNone(rows[-1]['image'])


if __name__=='__main__': unittest.main()
