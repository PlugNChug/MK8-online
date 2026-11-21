"""Exercise native widget construction, navigation ownership, and CPU preservation."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER, compiler_env


class SettingsButtonTests(unittest.TestCase):
    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_constructs_registers_and_grows_the_guest_owned_navigation_buffer(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'rt.h').write_text(r'''
#include "cpu.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
extern uint8_t memory[16384];
#define HEAP_BASE ((uint64_t)(uintptr_t)memory)
#define HEAP_MAX 16384ull
#define GUEST_LO HEAP_BASE
#define GUEST_HI (HEAP_BASE+HEAP_MAX)
#define LOG(...) ((void)0)
#define FATAL(...) abort()
uint64_t test_read(uint64_t);
#undef LD64
#define LD64(a) test_read(a)
Cpu *kernel_current_cpu(void);
void guest_call(Cpu *, uint64_t);
''', encoding='utf-8')
            (root/'test.c').write_text(r'''
#include "rt.h"
#include <assert.h>
#include <string.h>
uint8_t memory[16384] __attribute__((aligned(16)));
static Cpu cpu;
static uint64_t page, parent, example, widget, nav, old, grown, text;
static unsigned calls, frees;
static unsigned background_changes, instant;
static bool illustration_visible;
uint64_t test_read(uint64_t p) {
    if(p==0x70012f9e28ull) return 0x7000001000ull;
    assert(p>=HEAP_BASE && p+8<=GUEST_HI);
    return *(u64u *)(uintptr_t)p;
}
Cpu *kernel_current_cpu(void) { return &cpu; }
void mk8_settings_button_build(uint64_t);
bool mk8_settings_button_decided(uint64_t);
void mk8_settings_button_restore(uint64_t);
void mk8_settings_button_hover(uint64_t,unsigned);
void guest_call(Cpu *c, uint64_t address) {
    calls++;
    if(address==0x70005902d0ull) {
        assert(c->x[0]==0x3d8 && c->x[1]==4); c->x[0]=widget;
    } else if(address==0x7000434d90ull) {
        assert(c->x[0]==widget); ST8(widget+0x3ca,1);
    } else if(address==0x70005fb2a4ull) {
        assert(c->x[1]==12 && c->x[2]==0 && c->x[3]==8);
        ST32(c->x[0],0); ST32(c->x[0]+4,12); ST64(c->x[0]+8,grown);
    } else if(address==0x70005fae48ull) {
        assert(c->x[0]==old && LD64(nav+0x18)==grown); frees++;
    } else if(address==0x70008bc0c4ull) {
        assert(c->x[0]==page && c->x[1]==widget+0x30 && c->x[2]==parent);
        assert(LD64(c->x[3])==0x7000001010ull);
        assert(!strcmp((char *)(uintptr_t)LD64(c->x[3]+8),"L_SubBtn_07"));
        assert(!LD32(c->x[4]));
        assert(LD64(widget)==101 && LD64(widget+0x30)==102 && LD64(widget+0xb8)==103);
        unsigned n=LD32(nav+0x10); assert(n<LD32(nav+0x14));
        ST64(LD64(nav+0x18)+n*8,widget+0x30); ST32(nav+0x10,n+1);
        ST64(widget+0x38,parent);
    } else if(address==0x70008ad8e8ull) {
        assert(c->x[0]==widget+0x30 && LD32(widget+0x48)==107);
        assert(!strcmp((char *)(uintptr_t)LD64(c->x[1]+8),"T_Btn_00")); c->x[0]=text;
    } else if(address==0x70008adc54ull) {
        static const uint16_t expected[]={'M','e','n','u',0};
        assert(c->x[0]==text && !memcmp((void *)(uintptr_t)c->x[1],expected,sizeof expected));
    } else if(address==0x7000435cc0ull) {
        assert(c->x[0]==widget); c->x[0]=1;
    } else if(address==0x7000435d2cull) {
        assert(c->x[0]==widget && c->x[1]==0xffffffffu);
        assert(LD8(widget+0x3ca)==0);
    } else if(address==0x70008afc54ull) {
        assert(c->x[0]==nav);
    } else if(address==0x70008bbee0ull) {
        assert(c->x[0]==page && c->x[1]==0 && (LD32(page+0x1a8)&0x80000000u));
    } else if(address==0x70008afdd0ull) {
        assert(c->x[0]==nav);
        ST32(nav+0x30,11); ST32(nav+0x28,3); c->x[0]=0;
    } else if(address==0x7000421dfcull) {
        c->x[0]=HEAP_BASE+10000;
    } else if(address==0x70004c6360ull) {
        assert(c->x[0]==HEAP_BASE+10000 && LD32(c->x[1])==6 && c->x[2]==instant);
        background_changes++; illustration_visible=true;
    } else if(address==0x70008ade74ull) {
        assert(c->x[0]==HEAP_BASE+12000 && !c->x[1]); illustration_visible=false;
    } else if(address==0x70004da65cull) {
        assert(c->x[0]==page && c->x[1]==0xffffffffu && c->x[2]==instant);
    } else assert(0);
    c->pc=address; c->nzcv=0xabcdef; c->v[0]=0x1234;
}
int main(int argc, char **argv) {
    page=HEAP_BASE; parent=HEAP_BASE+2048; example=HEAP_BASE+4096;
    widget=HEAP_BASE+6144; nav=HEAP_BASE+8192; old=HEAP_BASE+8500;
    grown=HEAP_BASE+8600; text=HEAP_BASE+8700;
    ST64(page+0x448,parent); ST64(page+0x4d0,example); ST64(page+0x1b0,nav);
    ST32(page+0x120,4);
    ST64(example,101); ST64(example+0x30,102); ST64(example+0xb8,103);
    ST32(nav+0x10,11); ST32(nav+0x14,argc>1 ? 13 : 11); ST64(nav+0x18,old);
    ST64(old,111); ST64(old+8,222);
    memset(&cpu,0x17,sizeof cpu); cpu.sp=GUEST_HI-16;
    Cpu saved=cpu;
    mk8_settings_button_build(page);
    assert(!memcmp(&saved,&cpu,sizeof cpu));
    assert(calls==(argc>1 ? 5 : 7) && frees==(argc>1 ? 0 : 1));
    assert(LD32(nav+0x10)==12);
    uint64_t result=LD64(nav+0x18);
    assert(LD64(result)==111 && LD64(result+8)==222 && LD64(result+88)==widget+0x30);
    assert(LD8(widget+0x3ca)==1 && LD32(widget+0x48)==107);
    assert(mk8_settings_button_decided(page) && !memcmp(&saved,&cpu,sizeof cpu));
    unsigned before=calls;
    mk8_settings_button_restore(page);
    assert(calls==before+4 && !memcmp(&saved,&cpu,sizeof cpu) && LD8(widget+0x3ca)==1);
    ST8(widget+0x3ca,0); mk8_settings_button_restore(page);
    assert(LD8(widget+0x3ca)==0 && !memcmp(&saved,&cpu,sizeof cpu));
    ST64(HEAP_BASE+10000+0x2a0,HEAP_BASE+12000);
    for(instant=0;instant<2;instant++) {
        before=calls;mk8_settings_button_hover(page,instant);
        assert(calls==before+4 && background_changes==instant+1 && !illustration_visible);
        assert(!memcmp(&saved,&cpu,sizeof cpu));
    }
}
''', encoding='utf-8')
            exe = root/'test.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-UNDEBUG',
                                     '-I'+str(root), '-I'+str(ROOT/'upstream/smm2recomp/recomp/runtime'),
                                     str(root/'test.c'), str(ROOT/'runtime/mk8/support/settings_button.c'), '-o', str(exe)],
                                    env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-3000:])
            for args in ([], ['spare-capacity']):
                subprocess.run([str(exe), *args], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
