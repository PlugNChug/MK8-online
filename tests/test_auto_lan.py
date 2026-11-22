"""Exercise the LAN router with the menu states observed on MK8 4.0.1."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from toolchain.native import DEFAULT_COMPILER, compiler_env

ROOT = Path(__file__).resolve().parents[1]
HEADER = r'''
#pragma once
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern unsigned char memory[65536];
#define HEAP_BASE ((uint64_t)(uintptr_t)memory)
#define MAIN_BASE 0x7000000000ull
#define STACK_BASE 0x7200000000ull
#define STACK_SIZE 0x40000000ull
#define LD64(p) (*(uint64_t *)(uintptr_t)(p))
#define LD32(p) (*(uint32_t *)(uintptr_t)(p))
#define LD8(p) (*(uint8_t *)(uintptr_t)(p))
#define ST32(p,v) (LD32(p)=(v))
#define ST8(p,v) (LD8(p)=(v))
#define LOG(...) ((void)0)
typedef struct { uint64_t x[31], sp; } Cpu;
Cpu *kernel_current_cpu(void);
uint64_t kernel_heap_size(void);
void guest_call(Cpu *,uint64_t);
'''


HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) ((void)0)
uint64_t g_test_buttons, g_test_buttons_until;
static int beacons;
int vlan_send(uint16_t a, uint32_t b, uint16_t c, const void *d, size_t n) {
    assert(a==47999 && c==47999 && b==0xffffffffu && n==8);
    assert(!memcmp(d,"MK8-ROOM",8));beacons++;return 0;
}
#include "net/auto_lan.inc"
int main(void) {
    uint64_t presses[20], last=0;int count=0;
    int host=!strcmp(getenv("MK8_AUTO_LAN"),"host");
    assert(mk8_auto_lan_requested()==host);
    for(uint64_t frame=0;frame<2500;frame++) {
        if(frame==20)mk8_auto_lan_movie("MovieMenu.mp4");
        if(frame==1500)mk8_auto_lan_vlan_up();
        mk8_auto_lan_frame(frame);
        if(g_test_buttons_until!=last) {
            last=g_test_buttons_until;assert(count<20);presses[count++]=g_test_buttons;
        }
    }
    if (!host) { assert(!count && !beacons); puts("PASS: guest sends no menu inputs"); return 0; }
    // Title -> selected Mii: no direction can move to a Player preset.
    assert(count>=9 && presses[0]==AUTO_A && presses[1]==AUTO_A);
    assert(presses[2]==(AUTO_L|AUTO_R));
    assert(presses[3]==(AUTO_L|AUTO_R|AUTO_LSTICK));
    assert(presses[4]==(AUTO_L|AUTO_R));
    assert(presses[5]==AUTO_DOWN && presses[6]==AUTO_DOWN && presses[7]==AUTO_DOWN);
    assert(presses[8]==AUTO_A);
    assert(mk8_auto_lan_requested() == !host);
    assert(count==10 && presses[9]==AUTO_A && beacons>0);
    mk8_auto_lan_frame(100000); assert(!mk8_auto_lan_requested());
    puts("PASS: active Mii kept, LAN navigation preserved, host creates room");
}
'''

@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class AutoLanTests(unittest.TestCase):
    def test_menu_ownership_focus_and_room_entry(self):
        with tempfile.TemporaryDirectory(prefix='mk8-auto-lan-') as directory:
            folder = Path(directory)
            (folder/'rt.h').write_text(HEADER)
            (folder/'kernel.h').write_text('#include "rt.h"\n')
            exe = folder/'test.exe'
            result = subprocess.run([
                str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                '-I'+str(folder), '-I'+str(ROOT/'runtime/mk8'), str(ROOT/'tests/native_auto_lan.c'),
                str(ROOT/'runtime/mk8/net/auto_lan_menu.c'), '-o', str(exe),
            ], env=compiler_env(), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            for role, case in [('guest','direct'), ('1','direct'), ('guest','boot'), ('guest','wrong-room'),
                               ('guest','first-ok'), ('guest','first-refused'), ('guest','first-busy'),
                               ('guest','boot-foreign-owner'), ('guest','boot-left'), ('guest','missing-controller'),
                               ('guest','foreign-controller'), ('guest','boot-invalid-nav'),
                               ('guest','first-preset'), ('guest','first-invalid'), ('guest','first-partial'),
                               ('guest','first-unselected'), ('guest','invalid-save'),
                               ('guest','boot-2p'), ('guest','boot-2p-cancel'), ('guest','boot-2p-invalid'),
                               ('guest','boot-2p-stalled'), ('guest','boot-2p-first'), ('guest','boot-2p-worker-cancel'),
                               ('guest','boot-2p-left-early')]:
                with self.subTest(role=role, case=case):
                    env = dict(os.environ, MK8_AUTO_LAN=role, LARECOMP_LAN_RELAY='127.0.0.1:47820',
                               MK8_LAN_PLAYERS='2' if case.startswith('boot-2p') else '1', LARECOMP_LAN_SLOT='1', LARECOMP_LAN_ROOM='MK8ONLINE')
                    result = subprocess.run([str(exe), case], env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_active_mii_is_confirmed_without_moving_to_a_preset(self):
        with tempfile.TemporaryDirectory(prefix='mk8-auto-lan-') as directory:
            folder=Path(directory);source=folder/'test.c';exe=folder/'test.exe'
            source.write_text(HARNESS)
            subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                            '-I'+str(ROOT/'runtime/mk8'),str(source),'-o',str(exe)],
                           env=compiler_env(),check=True,capture_output=True,timeout=120)
            for role in ('host','guest'):
                env=dict(os.environ,MK8_AUTO_LAN=role,LARECOMP_LAN_RELAY='127.0.0.1:47820')
                result=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertIn('PASS:',result.stdout)
