"""Execute the automatic LAN input state machine; no commercial game inputs."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from toolchain.native import DEFAULT_COMPILER, compiler_env

ROOT=Path(__file__).resolve().parents[1]
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
    for(uint64_t frame=0;frame<2500;frame++) {
        if(frame==20)mk8_auto_lan_movie("MovieMenu.mp4");
        if(frame==1500)mk8_auto_lan_vlan_up();
        mk8_auto_lan_frame(frame);
        if(g_test_buttons_until!=last) {
            last=g_test_buttons_until;assert(count<20);presses[count++]=g_test_buttons;
        }
    }
    // Title -> selected Mii: no direction can move to a Player preset.
    assert(count>=9 && presses[0]==AUTO_A && presses[1]==AUTO_A);
    assert(presses[2]==(AUTO_L|AUTO_R));
    assert(presses[3]==(AUTO_L|AUTO_R|AUTO_LSTICK));
    assert(presses[4]==(AUTO_L|AUTO_R));
    assert(presses[5]==AUTO_DOWN && presses[6]==AUTO_DOWN && presses[7]==AUTO_DOWN);
    assert(presses[8]==AUTO_A);
    if(host) { assert(count==10 && presses[9]==AUTO_A && beacons>0); }
    else { assert(count==9 && beacons==0); } // no host beacon: never create a room as guest
    puts("PASS: active Mii kept, LAN navigation preserved, guest waits for host");
}
'''

@unittest.skipUnless(DEFAULT_COMPILER.is_file(),'Native compiler required')
class AutoLanTests(unittest.TestCase):
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
                self.assertIn('active Mii kept',result.stdout)
