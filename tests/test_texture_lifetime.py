import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER
from host import prepare_host_sources


class TextureLifetimeTests(unittest.TestCase):
    def test_preparation_covers_recording_and_replay_owners(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', Path(folder))
            text = (runtime/'nvn/cmdbuf.c').read_text()
            self.assertIn('mk8_command_texture(r, rt.depth)', text)
            self.assertIn('mk8_command_texture(r, ct.tex)', text)
            self.assertEqual(text.count('mk8_command_texture(r, t.tex)'), 2)
            self.assertEqual(text.count('mk8_command_texture(r, t[1].tex)'), 3)
            self.assertIn('cb->buf_used = 0;  // references now belong', text)
            self.assertEqual(text.count('mk8_free_list(graveyard['), 2)
            self.assertIn('mk8_copy_command_textures(d, r)', text)
            self.assertIn('mk8_rt_release(&prev)', (runtime/'nvn/vulkan/exec.c').read_text())

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_finalize_before_replay_and_recording_transfer(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            code = root/'ownership.c'
            code.write_text('''#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define FATAL(...) abort()
typedef struct { unsigned mk8_refs; unsigned marker; } NvnTexture;
static unsigned destroyed;
static void backend_texture_destroyed(NvnTexture *t) { assert(t->marker==42); destroyed++; }
#include "graphics/texture_refs.inc"
typedef struct { uint32_t op,size; struct Mk8TextureRefs *mk8_textures; } CmdRec;
#define CMD_END 0
#include "graphics/command_texture_refs.inc"
int main(void) {
 NvnTexture *t=calloc(1,sizeof *t); t->mk8_refs=1;t->marker=42;
 CmdRec buffer[2]={{1,sizeof(CmdRec),NULL},{0,sizeof(CmdRec),NULL}};
 mk8_command_texture(buffer,(uintptr_t)t);mk8_command_texture(buffer,(uintptr_t)t);
 assert(t->mk8_refs==2); // duplicate color/depth ownership is deduplicated per command
 CmdRec *list=malloc(sizeof buffer);memcpy(list,buffer,sizeof buffer);
 size_t used=0; // EndRecording transferred ownership; resetting/finalizing the builder cannot drop it
 mk8_release_records((uint8_t*)buffer,used);
 mk8_texture_release(t); // game finalizes before queued execution
 assert(!destroyed && t->marker==42 && t->mk8_refs==1);
 CmdRec copy[2];memcpy(copy,list,sizeof copy);mk8_copy_command_textures(copy,list);
 assert(t->mk8_refs==2 && copy[0].mk8_textures!=list[0].mk8_textures);
 mk8_texture_retain(t); // executor state keeps the target across lists
 mk8_release_records((uint8_t*)copy,sizeof copy);
 mk8_free_list((uint8_t*)list);assert(!destroyed && t->mk8_refs==1);
 mk8_texture_release(t);assert(destroyed==1);
 t=calloc(1,sizeof *t);t->mk8_refs=1;t->marker=42;
 memset(buffer,0,sizeof buffer);buffer[0].op=1;buffer[0].size=sizeof(CmdRec);
 mk8_command_texture(buffer,(uintptr_t)t);
 mk8_texture_release(t);
 mk8_release_records((uint8_t*)buffer,sizeof(CmdRec)); // abandoned recording
 assert(destroyed==2 && !buffer[0].mk8_textures);
 mk8_texture_release(NULL);mk8_command_texture(buffer,0);
 return 0;
}
''', encoding='utf-8')
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            exe = root/'ownership.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-UNDEBUG', '-I'+str(ROOT/'runtime/mk8'), str(code), '-o', str(exe)], env=env, check=True)
            subprocess.run([str(exe)], check=True)
