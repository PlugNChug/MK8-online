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
#define LOG(...) printf(__VA_ARGS__)
uint64_t g_test_buttons, g_test_buttons_until;
static int beacons;
int vlan_send(uint16_t a, uint32_t b, uint16_t c, const void *d, size_t n) {
    assert(a==47999 && c==47999 && b==0xffffffffu && n==8);
    assert(!memcmp(d,"MK8-ROOM",8));beacons++;return 0;
}
#include "net/auto_lan.inc"
int main(int argc, char **argv) {
    assert(argc==2);
    int title_timeout=!strcmp(argv[1],"title-timeout");
    int vlan_timeout=!strcmp(argv[1],"vlan-timeout");
    int vlan_deadline=!strcmp(argv[1],"vlan-deadline");
    int room_timeout=!strcmp(argv[1],"room-timeout");
    int room_deadline=!strcmp(argv[1],"room-deadline");
    uint64_t presses[20], frames[20], last=0, lobby_end=0;int count=0;
    uint64_t vlan_at=1500, room_at=UINT64_MAX, end=2500;
    if(title_timeout) { vlan_at=36003;room_at=36004;end=40000; }
    if(vlan_timeout || vlan_deadline) { vlan_at=UINT64_MAX;end=4000; }
    if(room_timeout || room_deadline) { room_at=1500+18000+(room_timeout?2:0);end=21000; }
    int host=!strcmp(getenv("MK8_AUTO_LAN"),"host");
    for(uint64_t frame=0;frame<end;frame++) {
        if(frame==(title_timeout?36002:20))mk8_auto_lan_movie("MovieMenu.mp4");
        if(frame==vlan_at)mk8_auto_lan_vlan_up();
        if(frame==room_at)mk8_auto_lan_beacon(0x0a0d2501,(const uint8_t*)"MK8-ROOM",8);
        if(frame==1600) {
            mk8_auto_lan_beacon(0x0a0d2501,(const uint8_t*)"MK8-ROOM",7);
            mk8_auto_lan_beacon(0x0a0d2501,(const uint8_t*)"bad-room",8);
        }
        // Repeated readiness signals cannot retry an already stopped/completed sequence.
        if(frame==end-100) {
            mk8_auto_lan_movie("MovieMenu.mp4");mk8_auto_lan_vlan_up();
            if(strcmp(argv[1],"normal"))
                mk8_auto_lan_beacon(0x0a0d2501,(const uint8_t*)"MK8-ROOM",8);
        }
        mk8_auto_lan_frame(frame);
        if(g_test_buttons_until!=last) {
            last=g_test_buttons_until;assert(count<20);
            frames[count]=frame;presses[count++]=g_test_buttons;
            if(count==9) {
                lobby_end=last;
                if(vlan_timeout || vlan_deadline) {
                    vlan_at=lobby_end+600+(vlan_timeout?2:0);
                    room_at=vlan_at+10;
                }
            }
        }
    }
    if(title_timeout) {
        assert(count==0 && beacons==0);
        puts("PASS: late title cannot restart timed-out navigation");return 0;
    }
    // Title -> selected Mii: no direction can move to a Player preset.
    assert(count>=9 && presses[0]==AUTO_A && presses[1]==AUTO_A);
    assert(presses[2]==(AUTO_L|AUTO_R));
    assert(presses[3]==(AUTO_L|AUTO_R|AUTO_LSTICK));
    assert(presses[4]==(AUTO_L|AUTO_R));
    assert(presses[5]==AUTO_DOWN && presses[6]==AUTO_DOWN && presses[7]==AUTO_DOWN);
    assert(presses[8]==AUTO_A);
    if(vlan_timeout || room_timeout) {
        assert(count==9 && beacons==0);
        printf("EXPECTED_STOP_FRAME=%llu\n",(unsigned long long)(vlan_timeout?lobby_end+601:1500+18001));
    } else if(host) {
        assert(count==10 && presses[9]==AUTO_A && beacons>0);
        assert(frames[9]==vlan_at+240);
    } else if(vlan_deadline || room_deadline) {
        assert(count==11 && presses[9]==AUTO_DOWN && presses[10]==AUTO_A && beacons==0);
        assert(frames[9]==room_at+180 && frames[10]==room_at+336);
    } else { assert(count==9 && beacons==0); } // no host beacon: never create a room as guest
    puts("PASS: active Mii kept, LAN navigation preserved, guest waits for host");
}
'''

@unittest.skipUnless(DEFAULT_COMPILER.is_file(),'Native compiler required')
class AutoLanTests(unittest.TestCase):
    def test_active_mii_and_navigation_readiness_deadlines(self):
        with tempfile.TemporaryDirectory(prefix='mk8-auto-lan-') as directory:
            folder=Path(directory);source=folder/'test.c';exe=folder/'test.exe'
            source.write_text(HARNESS)
            subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                            '-I'+str(ROOT/'runtime/mk8'),str(source),'-o',str(exe)],
                           env=compiler_env(),check=True,capture_output=True,timeout=120)
            cases=[(role,scenario) for role in ('host','guest')
                   for scenario in ('normal','title-timeout','vlan-timeout','vlan-deadline')]
            cases += [('guest','room-timeout'),('guest','room-deadline')]
            for role,scenario in cases:
                with self.subTest(role=role,scenario=scenario):
                    env=dict(os.environ,MK8_AUTO_LAN=role,LARECOMP_LAN_RELAY='127.0.0.1:47820')
                    result=subprocess.run([str(exe),scenario],env=env,capture_output=True,text=True,timeout=10)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    self.assertIn('PASS:',result.stdout)
                    if scenario.endswith('timeout'):
                        stop={'title-timeout':'title','vlan-timeout':'virtual LAN up',
                              'room-timeout':"lobby: the host's room"}[scenario]
                        frame=36001 if scenario=='title-timeout' else int(
                            result.stdout.split('EXPECTED_STOP_FRAME=')[1].splitlines()[0])
                        self.assertIn(f'stopped at "{stop}" (frame {frame}): continue by hand',result.stdout)
                        self.assertEqual(result.stdout.count('stopped at'),1)
                    else:
                        self.assertNotIn('stopped at',result.stdout)
