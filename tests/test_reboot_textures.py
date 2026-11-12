"""Reset preserva backends compartilhados até a última referência host ser liberada."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import reboot_lifetime


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define LOG(...) ((void)0)
typedef struct { int refs,reg; bool image; void *pending_rgba; } TexBackend;
typedef struct { TexBackend *backend; } NvnTexture;
static NvnTexture *owners[4], **g_live=owners;
static int g_nlive, freed, retired;
#define ALIAS_BUCKETS 4
typedef struct Alias { TexBackend *b; struct Alias *next; } Alias;
static Alias *g_alias[ALIAS_BUCKETS];
static void alias_remove(TexBackend *b) {
 for(int i=0;i<ALIAS_BUCKETS;i++) for(Alias **a=&g_alias[i];*a;a=&(*a)->next)
  if((*a)->b==b) { Alias *old=*a;*a=old->next;free(old);return; }
}
static void destroy_image(TexBackend *b) { if(b->image){retired++;b->image=false;} }
static void vk_alias_reset(void) {}
static void vk_collect_dead(void) {}
static void vk_alias_texture_destroyed(NvnTexture *t) {}
static TexBackend *tracked;
static void counted_free(void *p) { if(p==tracked)freed++;free(p); }
#define free counted_free
'''

MAIN = r'''
static TexBackend *make(NvnTexture *owner,NvnTexture *a,NvnTexture *b) {
 TexBackend *backend=calloc(1,sizeof *backend);assert(backend);
 *backend=(TexBackend){.refs=(a?2:1)+(b?1:0),.reg=1,.image=true,.pending_rgba=malloc(16)};
 owner->backend=backend;if(a)a->backend=backend;if(b)b->backend=backend;
 owners[0]=owner;g_nlive=1;freed=retired=0;tracked=backend;
 Alias *alias=calloc(1,sizeof *alias);assert(alias);alias->b=backend;g_alias[0]=alias;
 return backend;
}
int main(void) {
 NvnTexture owner={0},a={0},b={0};
 TexBackend *backend=make(&owner,&a,&b);
 vk_textures_reset();
 assert(!freed && !g_nlive && !g_alias[0] && !owner.backend);
 assert(!backend->image && !backend->pending_rgba && backend->refs==2 && retired==1);
 backend_texture_destroyed(&a);assert(!freed && backend->refs==1);
 backend_texture_destroyed(&b);assert(freed==1 && retired==1);
 backend=make(&owner,&a,&b);
 backend_texture_destroyed(&owner);assert(!g_nlive && !freed && backend->refs==2);
 vk_textures_reset();assert(!freed && !g_alias[0] && !backend->image && !backend->pending_rgba);
 backend_texture_destroyed(&a);backend_texture_destroyed(&b);assert(freed==1 && retired==1);
 make(&owner,NULL,NULL);vk_textures_reset();assert(freed==1 && retired==1 && !owner.backend);
 vk_textures_reset();assert(freed==1 && !g_nlive);
 return 0;
}
'''


@unittest.skipUnless(os.name == 'nt' and DEFAULT_COMPILER.is_file(), 'Windows native compiler required')
class RebootTextureTests(unittest.TestCase):
    def test_teardown_waits_for_translation_and_drains_late_releases(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            text = (runtime/'main.c').read_text(encoding='utf-8')
            start = text.index('static void teardown(void) {')
            function = text[start:text.index('\n}\n', start)+3]
            source = out/'order.c'
            source.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define LOG(...) ((void)0)
#define FATAL(...) abort()
#define GUEST_LO 1
#define GUEST_HI 2
#define RT_REBOOT_NONE 0
typedef void (*ResetFn)(void);
static ResetFn hooks[1];
#define __start_resets hooks
#define __stop_resets (hooks+1)
static int stage,retired=1,g_nmodules=3,g_reboot=1;
static uint64_t plat_now_ns(void) { return 0; }
static void reboot_idle(void) {}
static void kernel_stop_threads(void (*idle)(void)) { assert(stage==0);stage=1; }
void mk8_async_drain_hook(void) { assert(stage==1);stage=2; }
void mk8_textures_drain(void) { assert((stage==2 || stage==5) && retired);retired=0;stage++; }
static void backend_clear_window(void) { assert(stage==3 && !retired);stage=4; }
static void backend_reset(void) { assert(stage==4 && !retired);stage=5;retired=1; }
static void kernel_reset(void) { assert(stage==6 && !retired);stage=7; }
static bool plat_reset_guest(uint64_t lo,uint64_t hi) { assert(stage==7);stage=8;return true; }
static void backend_guest_memory_reset(void) { assert(stage==8);stage=9; }
''' + function + '\nint main(void) { teardown(); assert(stage==9 && !g_nmodules && !g_reboot); return 0; }\n',
                              encoding='utf-8')
            exe = out/'order.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                                     '-O1', '-UNDEBUG', str(source), '-o', str(exe)],
                                    env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_shared_backends_survive_reset_until_late_release(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            text = (runtime/'nvn/vulkan/textures.c').read_text(encoding='utf-8')
            functions = []
            for name in ('static void live_remove(', 'static void backend_free(',
                         'void backend_texture_destroyed(', 'void vk_textures_reset('):
                start = text.index(name)
                functions.append(text[start:text.index('\n}\n', start)+3])
            source = out/'reboot.c'
            source.write_text(HARNESS+'\n'.join(functions)+MAIN, encoding='utf-8')
            exe = out/'reboot.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                                     '-O1', '-UNDEBUG', str(source), '-o', str(exe)],
                                    env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_unknown_or_duplicate_boundary_preserves_all_sources(self):
        originals = {name: (ROOT/'upstream/smm2recomp/recomp/runtime'/name).read_text(encoding='utf-8')
                     for name, _, _ in reboot_lifetime.CHANGES}
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            for name, old, _ in reboot_lifetime.CHANGES:
                for replacement in ('changed', old+'\n'+old):
                    inputs = dict(originals)
                    inputs[name] = inputs[name].replace(old, replacement)
                    for path, text in inputs.items():
                        target = runtime/path
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_text(text, encoding='utf-8')
                    with self.assertRaisesRegex(ValueError, 'boundary changed'):
                        reboot_lifetime.prepare(runtime)
                    for path, text in inputs.items():
                        self.assertEqual((runtime/path).read_text(encoding='utf-8'), text)


if __name__ == '__main__':
    unittest.main()
