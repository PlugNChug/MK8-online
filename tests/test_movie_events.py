"""Movie event callbacks must not yield while owning the decoder's host mutex."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class MovieEventsTests(unittest.TestCase):
    def test_signal_stop_interleaving_and_constructor_abi(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root/'events.c'
            source.write_text(r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
typedef struct { uint64_t x[32]; } Cpu;
typedef struct { int lock; bool stopped,video; uint64_t events[4]; } Decoder;
static Decoder d;
static int signal_count,clear_count,interleave_stop,armed,ctor_count;
static void pthread_mutex_lock(int *m) { assert(!*m); *m=1; }
static void pthread_mutex_unlock(int *m) { assert(*m); *m=0; }
static void *self_state(Cpu *c) { return &d; }
static uint64_t guest_sym(const char *s) { return 123; }
static void guest_call(Cpu *c,uint64_t fn) {
 assert(!d.lock && fn==123 && c->x[0]>=1 && c->x[0]<=4);
 clear_count++;armed=0;memset(c->x,0,64);
}
static void dec_stop(Cpu *c);
static void signal_guest_event(Cpu *c,uint64_t e) {
 assert(!d.lock && e==1);signal_count++;
 if(interleave_stop) { Cpu other={0};dec_stop(&other); }
 armed=1;
}
#define LOG(...) ((void)0)
#include "services/movie_events.inc"
typedef struct { int n; struct { const char *key; bool is_str; int64_t ival; } e[4]; } MediaData;
static MediaData metadata;
static MediaData *md(uint64_t x) { return &metadata; }
#define HLE_FUNC(symbol,name) static void name(Cpu *c)
static void movie_decoder_ctor(Cpu *c) {
 assert(c->x[0]==100 && c->x[1]==2 && c->x[2]==4 && c->x[3]==1);ctor_count++;
}
#include "services/movie_compat.inc"
int main(void) {
 Cpu c={0};for(int i=0;i<4;i++)d.events[i]=i+1;
 c.x[0]=100;c.x[1]=2;c.x[2]=4;c.x[3]=1;c.x[19]=999;
 mk8_movie_decoder_ctor(&c);assert(ctor_count==1 && c.x[19]==999);
 dec_signal(&c,&d,0);assert(signal_count==1 && armed && !d.lock);
 interleave_stop=1;dec_signal(&c,&d,0);
 assert(d.stopped && !armed && clear_count==5 && c.x[0]==100 && c.x[19]==999);
 dec_signal(&c,&d,0);dec_stop(&c);assert(signal_count==2 && clear_count==5);
 int out[4]={-9,-9,-9,-9};c.x[1]=(uintptr_t)"crop";
 for(int i=0;i<4;i++)c.x[2+i]=(uintptr_t)&out[i];
 mk8_movie_find_rect(&c);assert(!c.x[0] && out[0]==-9);
 metadata.n=2;metadata.e[0].key="width";metadata.e[0].ival=512;
 metadata.e[1].key="height";metadata.e[1].ival=608;
 mk8_movie_find_rect(&c);assert(c.x[0]==1 && out[0]==0 && out[1]==0 && out[2]==511 && out[3]==607);
 c.x[1]=(uintptr_t)"unknown";out[0]=5;
 mk8_movie_find_rect(&c);assert(!c.x[0] && out[0]==5);return 0;
}
''', encoding='utf-8')
            exe = root/'events.exe'
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-UNDEBUG', '-I'+str(ROOT/'runtime/mk8'), str(source), '-o', str(exe)], env=env, check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
