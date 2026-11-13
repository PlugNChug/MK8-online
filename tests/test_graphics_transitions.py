"""Execute prepared fullscreen/scale code and the bounded transition checkpoints."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env

C_MAIN=r'''
#include <assert.h>
#include <pthread.h>
#include "incident.c"
int window_key(int repeat,int key);
void window_apply(int fail);
void window_checkbox(void);
int window_wanted(void);
int window_reported(void);
static uint64_t vk_frame_counter(void) { return 100; }
static float g_rs_x=1,g_rs_y=1;
SCALE_FUNCTION
static void *window_writer(void *arg) {
    for (unsigned i=0;i<10000;i++) mk8_gfx_fullscreen_request(1,(i&1)!=0);
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);
    char path[512];
    if (!mk8_gfx_enabled()) {
        window_apply(0); assert(window_key(0,0)); window_apply(0);
        assert(window_wanted() && window_reported());
        renderer_set_scale(2,3); assert(g_rs_x==2 && g_rs_y==3);
        mk8_gfx_swapchain(1,1280,720,44); mk8_gfx_scale(1,1,1);
        assert(!atomic_load(&issued) && !swapchain_generation);
        take_snapshot(); assert(!snapshot_count && !checkpoint_count); return 0;
    }
    if (!strcmp(argv[1],"nonfinite")) {
        mk8_gfx_scale(100,NAN,INFINITY);
    } else if (!strcmp(argv[1],"concurrent")) {
        assert(!mk8_gfx_begin(100,true,1280,720,44,0,0,0,false,path,sizeof path));
        pthread_t t[4];
        for (unsigned i=0;i<4;i++) assert(!pthread_create(&t[i],NULL,window_writer,NULL));
        for (unsigned k=0;k<100;k++) {
            take_snapshot(); assert(checkpoint_count<=8);
            for (unsigned i=0;i<checkpoint_count;i++) {
                GfxEvent *e=&checkpoint_snapshot[i];
                assert(e->kind==3 && e->frame==100 && e->frame_sample && e->data[0]==1 && e->data[1]<=1);
                assert(!i || e->sequence>checkpoint_snapshot[i-1].sequence);
            }
        }
        for (unsigned i=0;i<4;i++) assert(!pthread_join(t[i],NULL));
        take_snapshot(); assert(checkpoint_count==2);
        assert(atomic_load(&issued)==40001); return 0;
    } else {
        window_apply(0); // initial window state, before any present marker
        assert(!window_wanted() && !window_reported());
        assert(!mk8_gfx_begin(100,true,1280,720,44,0,0,0,false,path,sizeof path));
        assert(!window_key(1,0)); // repeated F11 ignored by real handler
        assert(window_key(0,0)); assert(window_wanted());
        window_apply(0); assert(window_reported());
        mk8_gfx_swapchain(101,1280,720,44);
        mk8_gfx_swapchain(101,2560,1600,44);
        mk8_gfx_swapchain(101,2560,1600,44); // same size still a new generation
        renderer_set_scale(1,1);
        uint64_t before=atomic_load(&issued);
        renderer_set_scale(1,1); assert(atomic_load(&issued)==before);
        renderer_set_scale(1.25f,1.5f); assert(g_rs_x==1.25f && g_rs_y==1.5f);
        assert(window_key(0,1)); // actual Alt+Enter path
        assert(!window_wanted()); window_apply(1); // SDL failure must remain failure
        assert(window_reported());
        take_snapshot();
        GfxEvent last_apply={0};
        for (unsigned i=0;i<snapshot_count;i++) if(snapshot[i].kind==4) last_apply=snapshot[i];
        assert(last_apply.data[0]==0 && last_apply.data[1]==0 && last_apply.data[2]==1);
        window_checkbox(); assert(window_wanted()); window_apply(0);
        take_snapshot(); assert(checkpoint_count==8 && swapchain_generation==3);
        for (unsigned i=0;i<snapshot_count;i++) {
            GfxEvent *e=&snapshot[i];
            if (e->kind==3 || e->kind==4) assert(e->frame_sample);
            if (e->kind==5 || e->kind==6) assert(!e->frame_sample);
        }
        // Old F11/creation/scale events leave the ring but remain in bounded checkpoints.
        for (unsigned i=0;i<5000;i++) assert(!mk8_gfx_begin(1000+i,true,1280,720,44,0,0,0,false,path,sizeof path));
        take_snapshot(); assert(snapshot_count==GFX_CAPACITY && checkpoint_count==8);
        for (unsigned i=0;i<snapshot_count;i++) assert(snapshot[i].kind==2);
    }
    mk8_gfx_request();
    assert(mk8_gfx_begin(90000,false,1280,720,44,0,0,0,false,path,sizeof path));
    FILE *f=fopen(path,"wb"); assert(f); fputs("synthetic-image-for-native-test",f); fclose(f);
    mk8_gfx_png_result(path,true); mk8_gfx_end(); assert(!*active); return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(),'Native compiler required')
class GraphicsTransitionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory()
        cls.root=Path(cls.temp.name)
        runtime=prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',cls.root)
        menu=(runtime/'ui/menu.mm').read_text(encoding='utf-8')
        start=menu.index('        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||')
        shortcut=menu[start:menu.index('        /* fallthrough */',start)]
        start=menu.index('    static bool first = true;',menu.index('void menu_apply_window_requests('))
        apply=menu[start:menu.index('\n}\n',start)]
        start=menu.index('    bool fs = g_settings.fullscreen;')
        checkbox=menu[start:menu.index('    opt_toggle("Show FPS"',start)]
        texture=(runtime/'nvn/vulkan/textures.c').read_text(encoding='utf-8')
        start=texture.index('void renderer_set_scale(float sx, float sy) {')
        scale=texture[start:texture.index('\nuint64_t renderer_present_count',start)]
        cpp=r'''
#include <atomic>
#include <assert.h>
#include <SDL3/SDL.h>
#include "mk8_graphics_incident.h"
static struct { bool fullscreen; } g_settings;
static std::atomic<bool> g_want_fullscreen_apply;
static bool reported,failed;
static int saves;
static void settings_save(void) { saves++; }
static bool test_set(SDL_Window *w,bool value) { if (failed) return false; reported=value; return true; }
static void test_size(SDL_Window *w,int *x,int *y) { *x=reported ? 2560 : 1280; *y=reported ? 1600 : 720; }
static SDL_WindowFlags test_flags(SDL_Window *w) { return reported ? SDL_WINDOW_FULLSCREEN : 0; }
static bool opt_toggle(const char *,bool *v,bool,const char *) { *v=true; return true; }
#define SDL_SetWindowFullscreen test_set
#define SDL_GetWindowSizeInPixels test_size
#define SDL_GetWindowFlags test_flags
extern "C" int window_key(int repeat,int other) {
    SDL_Event event={}; const SDL_Event *e=&event;
    event.key.repeat=repeat!=0;
    event.key.scancode=other ? SDL_SCANCODE_RETURN : SDL_SCANCODE_F11;
    event.key.mod=other ? SDL_KMOD_ALT : SDL_KMOD_NONE;
''' + shortcut + r'''
    return false;
}
extern "C" void window_apply(int fail) {
    SDL_Window *w=(SDL_Window *)1; failed=fail!=0;
''' + apply + r'''
}
extern "C" void window_checkbox(void) {
''' + checkbox + r'''
}
extern "C" int window_wanted(void) { return g_settings.fullscreen; }
extern "C" int window_reported(void) { return reported; }
'''
        (cls.root/'window.cpp').write_text(cpp,encoding='utf-8')
        (cls.root/'main.c').write_text(C_MAIN.replace('SCALE_FUNCTION',scale),encoding='utf-8')
        flags=['-target','x86_64-windows-gnu','-O2','-UNDEBUG','-I'+str(runtime),
               '-I'+str(DEPS/'include'),'-I'+str(ROOT/'runtime/mk8/graphics')]
        for compiler,name in (('cc','main.c'),('c++','window.cpp'),('cc',str(ROOT/'runtime/mk8/graphics/preview.c'))):
            source=Path(name) if Path(name).is_absolute() else cls.root/name
            command=[str(DEFAULT_COMPILER),compiler,*flags,'-c',str(source),'-o',str(cls.root/(source.name+'.o'))]
            result=subprocess.run(command,env=compiler_env(),capture_output=True,text=True)
            if result.returncode: raise RuntimeError(result.stderr)
        cls.exe=cls.root/'transitions.exe'
        result=subprocess.run([str(DEFAULT_COMPILER),'c++',*flags,str(cls.root/'main.c.o'),str(cls.root/'window.cpp.o'),str(cls.root/'preview.c.o'),
            '-lbcrypt','-o',str(cls.exe)],env=compiler_env(),capture_output=True,text=True)
        if result.returncode: raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self,mode):
        folder=self.root/mode
        folder.mkdir()
        run=subprocess.run([str(self.exe),mode],cwd=folder,capture_output=True,text=True,timeout=30,
            env=dict(os.environ,MK8_GFX_DIAGNOSTICS='0' if mode=='disabled' else '1'))
        self.assertEqual(run.returncode,0,run.stdout+run.stderr)
        return folder

    def test_prepared_fullscreen_and_scale_behavior_with_recording_disabled(self):
        folder=self.run_case('disabled')
        self.assertFalse((folder/'graphics-incidents').exists())

    def test_transitions_order_failure_results_and_checkpoints_survive_wrap(self):
        folder=self.run_case('transitions')
        path=next(folder.glob('graphics-incidents/*/manifest.json'))
        manifest=json.loads(path.read_text())
        self.assertEqual(manifest['schema'],4)
        checkpoints=manifest['transition_checkpoints']
        self.assertEqual(len(checkpoints),8)
        kinds={kind:[e for e in checkpoints if e['kind']==kind]
               for kind in ('fullscreen_request','fullscreen_apply','swapchain','render_scale')}
        self.assertTrue(all(len(events)==2 for events in kinds.values()))
        self.assertEqual([e['source'] for e in kinds['fullscreen_request']],[1,3])
        self.assertEqual([e['generation'] for e in kinds['swapchain']],[1,3])
        self.assertEqual([e['width'] for e in kinds['swapchain']],[1280,2560])
        self.assertEqual([e['scale_x'] for e in kinds['render_scale']],[1,1.25])
        self.assertEqual(kinds['render_scale'][-1]['scale_y'],1.5)
        self.assertTrue(kinds['fullscreen_request'][0]['frame_sample'])
        self.assertEqual(kinds['fullscreen_request'][0]['host_frame'],100)
        self.assertEqual(kinds['fullscreen_apply'][0]['host_frame'],0)
        rolling=[json.loads(line) for line in path.with_name('events.jsonl').read_text().splitlines()]
        self.assertTrue(all(e['kind']=='present' for e in rolling))
        self.assertEqual(manifest['checkpoint_drops'],0)

    def test_window_events_and_checkpoint_snapshots_are_concurrent_and_bounded(self):
        self.run_case('concurrent')

    def test_nonfinite_scale_values_produce_valid_json_without_modifying_renderer(self):
        folder=self.run_case('nonfinite')
        path=next(folder.glob('graphics-incidents/*/manifest.json'))
        manifest=json.loads(path.read_text())
        scale=manifest['transition_checkpoints'][0]
        self.assertIsNone(scale['scale_x'])
        self.assertIsNone(scale['scale_y'])


if __name__=='__main__':
    unittest.main()
