"""Execute the LAN vote policy without substituting network receipts or model objects."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
import experimental
from native import DEFAULT_COMPILER


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class LanVoteTests(unittest.TestCase):
    def test_absent_animation_does_not_replace_receipt_or_slot_validation(self):
        raw = br'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef struct { uint64_t x[32]; } Cpu;
static int mode, online, received, busy, slot, ready, done, queries, requests, completed, visuals;
static uint64_t manager[16], models[8];
static uint32_t state[16];
int mk8_no_mii_renderer(void) { return mode; }
static uint32_t LD32(uint64_t p) { uint32_t n; memcpy(&n,(void *)(uintptr_t)p,4); return n; }
static uint64_t LD64(uint64_t p) { uint64_t n; memcpy(&n,(void *)(uintptr_t)p,8); return n; }
static void f_7000427e6c(Cpu *c) { queries++; c->x[0]=ready; }
static void f_7000427e74(Cpu *c) { (void)c; requests++; }
static void f_7000427888(Cpu *c) { queries++; c->x[0]=ready; }
static void f_7000427960(Cpu *c) { (void)c; requests++; }
static void f_70004e472c(Cpu *c) { (void)c; visuals++; }
void f_70004e4370(Cpu *c) {
    c->x[19]=(uintptr_t)state;
    if (!received) return;
    c->x[0]=(uintptr_t)manager;
    if (online) {
        c->x[30] = 0x70004e43e8ull; f_7000427888(c);
    } else {
        c->x[30] = 0x70004e43dcull; f_7000427e6c(c);
    }
    if (busy) goto L_70004e44b0;
    c->x[8] = (uint32_t)((uint32_t)c->x[0] ^ 0x1ull);
    if (((c->x[8] >> 0) & 1) != 0) { goto L_70004e44b0; }
    if (slot > 11) goto L_70004e44b0;
    state[0x20/4]=1;
    if (online) {
        c->x[30] = 0x70004e44a4ull; f_7000427960(c);
    } else {
        c->x[30] = 0x70004e4490ull; f_7000427e74(c);
    }
L_70004e44b0:;
    if (state[0x20/4]!=1 || !done) return;
L_70004e44e4:;
    state[0x20/4]=2;
    c->x[30] = 0x70004e4500ull; f_70004e472c(c);
    completed++;
}
int main(void) {
    manager[0x68/8]=(uintptr_t)models;
    for(mode=0;mode<2;mode++)
    for(online=0;online<2;online++)
    for(unsigned a=0;a<2;a++) for(unsigned b=0;b<2;b++)
    for(received=0;received<2;received++) for(busy=0;busy<2;busy++)
    for(slot=11;slot<=12;slot++)
    for(ready=0;ready<2;ready++) for(done=0;done<2;done++) {
        memset(state,0,sizeof state);
        ((uint32_t*)models)[0x18/4]=a;
        ((uint32_t*)models)[0x28/4]=b;
        queries=requests=completed=visuals=0;
        Cpu c={0}; f_70004e4370(&c);
        int skip=mode&&!a&&!b, valid=received&&!busy&&slot<=11;
        assert(completed==(valid&&(skip||(ready&&done))));
        assert(queries==(received&&!skip));
        assert(visuals==(completed&&!skip));
        assert(requests==(valid&&!skip&&ready));
        assert(state[0x20/4]==(completed?2:requests?1:0));
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            src = root/'main'/experimental.LAN_VOTE_UNIT
            src.parent.mkdir()
            src.write_bytes(raw)
            with self.assertRaises(ValueError):
                experimental.patch_no_mii_unit(src, root/'out')
            with patch.object(experimental, 'LAN_VOTE_SHA256', hashlib.sha256(raw).hexdigest()):
                prepared = experimental.patch_no_mii_unit(src, root/'out')
            exe = root/'vote.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                            '-O2', '-UNDEBUG', str(prepared), '-o', str(exe)],
                env=dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache')), check=True)
            subprocess.run([str(exe)], check=True, timeout=20)

    def test_portrait_screen_omits_allocation_and_consumers_only_for_absent_resources(self):
        raw = br'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef struct { uint64_t x[32]; } Cpu;
static int mode, requests, consumers, continued;
static unsigned char manager[0x600], screen[0x1600];
int mk8_no_mii_renderer(void) { return mode; }
static uint32_t LD32(uint64_t p) { uint32_t v; memcpy(&v,(void *)(uintptr_t)p,4); return v; }
static uint64_t LD64(uint64_t p) { uint64_t v; memcpy(&v,(void *)(uintptr_t)p,8); return v; }
static void ST64(uint64_t p, uint64_t v) { memcpy((void *)(uintptr_t)p,&v,8); }
void screen_step(Cpu *c) {
    if(c->x[8]!=0x1d && c->x[8]!=0x34) goto L_70000d5200;
L_70000d50e4:;
    requests++;
    ST64(c->x[19]+0x1538, 123);
L_70000d5200:;
    continued++;
L_70000d53cc:;
    if(c->x[8]==0x1d || c->x[8]==0x34) {
        assert(LD64(c->x[19]+0x1538)==123);
        consumers++;
    }
L_70000d5634:;
    continued++;
}
int main(void) {
 for(mode=0;mode<2;mode++) for(int present=0;present<4;present++)
 for(int type=0;type<3;type++) for(int missing=0;missing<2;missing++) {
    memset(manager,0,sizeof manager); memset(screen,0,sizeof screen);
    uint32_t count=present&1; memcpy(manager+0x540,&count,4);
    ST64((uintptr_t)manager+0x548, present&2?456:0);
    Cpu c={0}; c.x[19]=(uintptr_t)screen; c.x[23]=missing?0:(uintptr_t)manager;
    c.x[8]=type==0?0x1d:type==1?0x34:7;
    requests=consumers=continued=0;
    screen_step(&c);
    int omitted=mode&&(missing||!present);
    assert(requests==(type!=2&&!omitted));
    assert(consumers==requests); assert(continued==2);
    assert(LD64((uintptr_t)screen+0x1538)==(requests?123:0));
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); src=root/'main/main_026.c'; src.parent.mkdir(); src.write_bytes(raw)
            with self.assertRaises(ValueError):
                experimental.patch_no_mii_unit(src, root/'out')
            with patch.object(experimental, 'PORTRAIT_SCREEN_SHA256', hashlib.sha256(raw).hexdigest()):
                prepared=experimental.patch_no_mii_unit(src, root/'out')
            exe=root/'portrait.exe'
            subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O2','-UNDEBUG',str(prepared),'-o',str(exe)],
                env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache')),check=True)
            subprocess.run([str(exe)],check=True,timeout=20)
