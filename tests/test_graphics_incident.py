"""Execute the recorder with author-owned events and injected I/O failures."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER, compiler_env
from host import DEPS, prepare_host_sources
import graphics_incident

HARNESS = r'''
#include <assert.h>
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int low_disk, reparse;
static DWORD test_attributes(LPCSTR path) {
    if (reparse && strstr(path,"graphics-incidents")) return FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT;
    return GetFileAttributesA(path);
}
static BOOL test_space(LPCSTR p, PULARGE_INTEGER a, PULARGE_INTEGER b, PULARGE_INTEGER c) {
    a->QuadPart=low_disk ? (4ull<<30)-1 : 100ull<<30; return TRUE;
}
#define GetDiskFreeSpaceExA test_space
#define GetFileAttributesA test_attributes
#include "incident.c"
#undef GetDiskFreeSpaceExA
#undef GetFileAttributesA
static uint64_t codes[3]={0x7100000100,0x7100000200,0};
static void *writer(void *arg) {
    for (unsigned i=0;i<30000;i++) mk8_gfx_skip(i+10000,codes,(uintptr_t)arg,0,0,true,true,-0.5);
    return NULL;
}
static bool begin(uint64_t frame,bool busy,char *path,unsigned cap) {
    return mk8_gfx_begin(frame,busy,1280,720,44,123,456,789,false,path,cap);
}
static void image(const char *path,bool success) {
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite("fake-png-for-native-test",1,24,f)==24); assert(!fclose(f));
    mk8_gfx_png_result(path,success);
}
int main(int argc,char **argv) {
    assert(argc==2); char path[512]="unchanged";
    if (!strcmp(argv[1],"disabled")) {
        assert(!mk8_gfx_enabled()); mk8_gfx_skip(1,codes,1,0,0,true,false,0); mk8_gfx_request();
        assert(!begin(100000,false,path,sizeof path));
        assert(!atomic_load(&issued) && !atomic_load(&request_ns) && !*active);
        assert(GetFileAttributesA("graphics-incidents")==INVALID_FILE_ATTRIBUTES); return 0;
    }
    assert(mk8_gfx_enabled());
    if (!strcmp(argv[1],"ring")) {
        for (unsigned i=0;i<GFX_CAPACITY+17;i++) mk8_gfx_skip(i+1,codes,i,0,0,true,true,-0.5);
        take_snapshot(); assert(snapshot_count==GFX_CAPACITY && snapshot[0].sequence==18);
        for (unsigned i=1;i<snapshot_count;i++) assert(snapshot[i].sequence==snapshot[i-1].sequence+1);
        uint64_t next=atomic_load(&issued);
        atomic_store(&ring[next%GFX_CAPACITY].busy,true);
        record((GfxEvent){.kind=1}); atomic_store(&ring[next%GFX_CAPACITY].busy,false);
        assert(atomic_load(&lost)==1);
        pthread_t threads[4];
        for (unsigned i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,writer,(void *)(uintptr_t)(i+1)));
        for (unsigned k=0;k<30;k++) {
            take_snapshot();
            for (unsigned i=0;i<snapshot_count;i++) {
                GfxEvent *e=&snapshot[i];
                assert(e->kind==1 && e->vs==codes[0] && e->fs==codes[1] && e->remaining_us==-500);
                assert(!i || e->sequence>snapshot[i-1].sequence);
            }
        }
        for (unsigned i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
        assert(atomic_load(&issued)==GFX_CAPACITY+18+120000); return 0;
    }
    if (!strcmp(argv[1],"benchmark")) {
        const unsigned n=2000000; uint64_t t=clock_ns(); atomic_store(&enabled,0);
        for (unsigned i=0;i<n;i++) mk8_gfx_skip(i,codes,1,0,0,true,false,0);
        uint64_t off=clock_ns()-t; t=clock_ns(); atomic_store(&enabled,1);
        for (unsigned i=0;i<n;i++) mk8_gfx_skip(i,codes,1,0,0,true,false,0);
        printf("{\"iterations\":%u,\"off_ns_per_event\":%.3f,\"on_ns_per_event\":%.3f}\n",n,(double)off/n,(double)(clock_ns()-t)/n);
        return 0;
    }
    mk8_gfx_skip(89999,codes,1234,0,0,true,true,-1);
    mk8_gfx_request(); uint64_t requested=atomic_load(&request_ns); assert(requested);
    mk8_gfx_request(); assert(atomic_load(&request_ns)==requested);
    assert(!begin(90000,true,path,sizeof path) && !strcmp(path,"unchanged"));
    assert(atomic_load(&request_ns)==requested);
    if (!strcmp(argv[1],"low-disk")) low_disk=1;
    if (!strcmp(argv[1],"limit")) incidents=GFX_MAX_INCIDENTS;
    if (!strcmp(argv[1],"reparse")) reparse=1;
    if (!strcmp(argv[1],"invalid")) { FILE *f=fopen("graphics-incidents","wb"); assert(f); fclose(f); }
    if (!strcmp(argv[1],"low-disk") || !strcmp(argv[1],"limit") || !strcmp(argv[1],"invalid") || !strcmp(argv[1],"reparse")) {
        assert(!begin(90001,false,path,sizeof path) && !*active && !atomic_load(&request_ns)); return 0;
    }
    if (!strcmp(argv[1],"small-path")) { assert(!begin(90001,false,path,2) && !*active); return 0; }
    if (!strcmp(argv[1],"extent")) {
        assert(!mk8_gfx_begin(90001,false,UINT32_MAX,UINT32_MAX,44,0,0,0,false,path,sizeof path));
        assert(!*active); return 0;
    }
    assert(begin(90001,false,path,sizeof path));
    assert(active_frame==90001 && snapshot_count==3 && snapshot[0].kind==1);
    if (!strcmp(argv[1],"no-image")) { mk8_gfx_end(); return 0; }
    if (!strcmp(argv[1],"partial-image")) { image(path,false); mk8_gfx_end(); return 0; }
    if (!strcmp(argv[1],"disk-after-image")) { image(path,true); low_disk=1; mk8_gfx_end(); return 0; }
    if (!strcmp(argv[1],"write-failure")) {
        char blocked[300]; snprintf(blocked,sizeof blocked,"%s/events.jsonl.pending",active);
        HANDLE h=CreateFileA(blocked,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        assert(h!=INVALID_HANDLE_VALUE); image(path,true); mk8_gfx_end(); CloseHandle(h); return 0;
    }
    image(path,true); mk8_gfx_end(); assert(!*active); return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class GraphicsIncidentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory()
        cls.output=Path(cls.temp.name)
        (cls.output/'mk8_incident_build.h').write_text(graphics_incident.build_metadata(),encoding='utf-8')
        source=cls.output/'test.c'
        source.write_text(HARNESS,encoding='utf-8')
        cls.exe=cls.output/'incident.exe'
        result=subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O2','-UNDEBUG',
            '-I'+str(cls.output),'-I'+str(ROOT/'runtime/mk8/graphics'),str(source),
            str(ROOT/'runtime/mk8/graphics/preview.c'),'-lbcrypt','-o',str(cls.exe)],
            env=compiler_env(),capture_output=True,text=True)
        if result.returncode:
            cls.temp.cleanup()
            raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self,mode):
        folder=self.output/mode
        folder.mkdir()
        result=subprocess.run([str(self.exe),mode],cwd=folder,
            env=dict(os.environ,MK8_GFX_DIAGNOSTICS='0' if mode=='disabled' else '1'),
            capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return folder,result

    def test_disabled_has_no_events_requests_or_files(self):
        self.run_case('disabled')

    def test_ring_wrap_order_contention_and_concurrent_snapshots(self):
        self.run_case('ring')

    def test_late_capture_conflict_and_manifest(self):
        folder,_=self.run_case('capture')
        manifests=list(folder.glob('graphics-incidents/*/manifest.json'))
        self.assertEqual(len(manifests),1)
        manifest=json.loads(manifests[0].read_text())
        self.assertEqual(manifest['host_frame'],90001)
        self.assertEqual(len(manifest['executable_sha256']),64)
        self.assertTrue(manifest['complete'])
        self.assertFalse(manifest['gameplay_verified'])
        events=[json.loads(line) for line in manifests[0].with_name('events.jsonl').read_text().splitlines()]
        self.assertEqual(events[0]['kind'],'shader_skip')
        self.assertEqual(events[0]['fs_code_va'],'0000007100000200')
        self.assertEqual(events[-1]['host_frame'],90001)
        self.assertEqual(set(p.name for p in manifests[0].parent.iterdir()),
                         {'manifest.json','events.jsonl','summary.txt','screenshot.png'})

    def test_io_bounds_low_disk_invalid_destination_and_failures(self):
        for case in ('low-disk','limit','invalid','reparse','extent','small-path','no-image',
                     'partial-image','disk-after-image','write-failure'):
            with self.subTest(case=case):
                folder,result=self.run_case(case)
                self.assertIn('capture failed',result.stderr)
                self.assertFalse(list(folder.glob('graphics-incidents/*/manifest.json')))

    def test_microbenchmark(self):
        _,result=self.run_case('benchmark')
        values=json.loads(result.stdout)
        self.assertGreater(values['on_ns_per_event'],0)
        print('Recorder microbenchmark: '+result.stdout.strip(),flush=True)

    def test_preparation_rejects_unknown_and_duplicate_boundaries_without_writes(self):
        for bad in ('unknown','duplicate'):
            with self.subTest(bad=bad),tempfile.TemporaryDirectory() as folder:
                runtime=Path(folder)
                originals={}
                for relative,changes in graphics_incident.edits().items():
                    path=runtime/relative
                    path.parent.mkdir(parents=True,exist_ok=True)
                    text='\n'.join((old+'\n')*count for old,_,count in changes)
                    if relative=='nvn/vulkan/device.c':
                        text=text.replace(changes[-1][0],'bad boundary') if bad=='unknown' else text+changes[-1][0]
                    path.write_text(text,encoding='utf-8')
                    originals[path]=path.read_bytes()
                with self.assertRaisesRegex(ValueError,'Graphics incident boundary changed'):
                    graphics_incident.prepare(runtime)
                self.assertEqual({p:p.read_bytes() for p in originals},originals)

    def test_packaged_build_without_git_keeps_unknown_metadata(self):
        with patch.object(graphics_incident.subprocess,'run',side_effect=FileNotFoundError('git unavailable')):
            metadata=graphics_incident.build_metadata()
        self.assertIn('#define MK8_INCIDENT_COMMIT "unknown"',metadata)
        self.assertIn('#define MK8_INCIDENT_BRANCH "unknown"',metadata)
        self.assertIn('#define MK8_INCIDENT_DIRTY "null"',metadata)

    def test_prepared_hotkey_draw_rejection_and_capture_arbitration_execute(self):
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder)
            runtime=prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',output)
            input_text=(runtime/'hle/input.c').read_text(encoding='utf-8')
            input_hook=input_text[input_text.index('void input_handle_event(const SDL_Event *e) {'):]
            input_hook=input_hook[:input_hook.index('    extern bool touchpad_handle_event')]
            execute=(runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            helper=execute[execute.index('static void mk8_gfx_program_skip('):]
            helper=helper[:helper.index('\n}')+2]
            device=(runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            capture=device[device.index(graphics_incident.DEVICE_BEGIN):]
            capture=capture[:capture.index('        if (shot && f1')]
            rejection=next(new for old,new,_ in graphics_incident.edits()['nvn/vulkan/exec.c']
                           if old=='if (!prog_ready(pr)) SKIP_DRAW();')
            self.assertIn(rejection,execute)
            code=r'''
#include <assert.h>
#include <windows.h>
#include <SDL3/SDL_events.h>
#include "incident.c"
static int unhandled;
''' + input_hook + r'''
    unhandled++;
}
typedef struct { uint64_t code[3]; atomic_int state; } Prog;
static __thread bool mk8_gfx_budget_miss;
static __thread double mk8_gfx_remaining;
static uint64_t vk_frame_counter(void) { return 90000; }
static int shader_mode(void) { return 0; }
static bool mk8_targets_redrawn(void) { return true; }
''' + helper + r'''
static bool prog_ready(Prog *p) { return p && atomic_load(&p->state)==1; }
#define SKIP_DRAW() return false
static bool draw(Prog *pr) {
''' + rejection + r'''
    return true;
}
static int saves;
static void vk_frame_end(const char *path) {
    assert(path);
    FILE *f=fopen(path,"wb"); assert(f); fputs("synthetic-native-image",f); fclose(f);
    mk8_gfx_png_result(path,true); saves++;
}
static void vk_frame_end_at(const char *path,uint64_t at) { assert(at==11); vk_frame_end(path); }
static void vk_save_png(const char *path,const uint8_t *data,uint32_t w,uint32_t h) {
    FILE *f=fopen(path,"wb"); assert(f); fputs("synthetic-native-image",f); fclose(f);
    mk8_gfx_png_result(path,true);
}
#define FG_HALF_SLOT_NS 1
static struct { struct { unsigned width,height; } sc_extent; unsigned sc_format;
    struct { unsigned driverVersion,vendorID,deviceID; } props; } g_vk={.sc_extent={1280,720}};
static uint64_t g_frame_counter=90000;
static void present(bool shot,bool req,bool fg_gen) {
    char shot_path[512]="existing.png";
    uint64_t t_gen=10;
    void *fgc=fg_gen ? (void *)1 : NULL;
''' + capture + r'''
}
int main(void) {
    assert(mk8_gfx_enabled());
    Prog p={.code={17,18,19}};
    assert(!draw(&p)); p.state=1; assert(draw(&p)); p.state=2; assert(!draw(&p)); assert(!draw(NULL));
    take_snapshot(); assert(snapshot_count==3 && snapshot[0].frame==90001 && snapshot[0].fs==18);
    SDL_Event e={.type=SDL_EVENT_KEY_DOWN}; e.key.scancode=SDL_SCANCODE_F8; e.key.repeat=true;
    input_handle_event(&e); assert(!atomic_load(&request_ns));
    e.key.repeat=false; input_handle_event(&e); uint64_t requested=atomic_load(&request_ns); assert(requested);
    e.type=SDL_EVENT_KEY_UP; input_handle_event(&e); assert(!unhandled);
    present(true,false,false); assert(atomic_load(&request_ns)==requested); // scheduled screenshot
    present(false,true,true); assert(atomic_load(&request_ns)==requested); // pending screenshot
    present(false,false,true); assert(!atomic_load(&request_ns) && !*active && saves==3);
    mk8_gfx_request(); present(false,false,false); assert(!atomic_load(&request_ns) && saves==4);
    atomic_store(&enabled,0); e.type=SDL_EVENT_KEY_DOWN; input_handle_event(&e); assert(unhandled==1);
    return 0;
}
'''
            source=output/'integration.c'
            source.write_text(code,encoding='utf-8')
            exe=output/'integration.exe'
            build=subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O2','-UNDEBUG',
                '-I'+str(runtime),'-I'+str(DEPS/'include'),'-I'+str(ROOT/'runtime/mk8/graphics'),
                str(source),str(ROOT/'runtime/mk8/graphics/preview.c'),'-lbcrypt','-o',str(exe)],env=compiler_env(),capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            result=subprocess.run([str(exe)],cwd=output,env=dict(os.environ,MK8_GFX_DIAGNOSTICS='1'),
                                  capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(len(list(output.glob('graphics-incidents/*/manifest.json'))),2)


if __name__=='__main__':
    unittest.main()
