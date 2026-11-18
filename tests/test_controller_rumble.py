"""Exercise the prepared vibration service through real SDL virtual gamepads."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'upstream/smm2recomp/recomp'
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


PREFIX = r'''
#include <SDL3/SDL.h>
#include <windows.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#undef IN
#undef OUT
#include "ipc.h"
#include "settings.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s (%s)\n",__LINE__,#x,SDL_GetError()); exit(1); } } while(0)
Settings g_settings;
static bool menu_open;
bool menu_is_open(void) { return menu_open; }
static bool native_settings_open;
bool mk8_settings_page_active(void) { return native_settings_open; }
static bool clock_controlled;
static Uint64 clock_ms;
static unsigned rumble_requests;
static Uint32 last_duration;
static Uint64 test_ticks(void) { return clock_controlled ? clock_ms : SDL_GetTicks(); }
static bool test_rumble(SDL_Gamepad *p,Uint16 low,Uint16 high,Uint32 duration) {
    rumble_requests++;
    last_duration=duration;
    return SDL_RumbleGamepad(p,low,high,duration);
}
#define EXTRA_PADS 3
static SDL_Gamepad *pad, *extra_pad[EXTRA_PADS];
KEvent *kernel_create_event_pair(uint32_t *w,uint32_t *r) { (void)w;(void)r;CHECK(false);return NULL; }
KSharedMem *kernel_create_shared_memory(uint64_t size,void (*fn)(KSharedMem *),uint32_t *handle) {
    (void)size;(void)fn;(void)handle;CHECK(false);return NULL;
}
Service *service_new(const char *name,ServiceHandler h,void *state) {
    (void)name;(void)h;(void)state;CHECK(false);return NULL;
}
void ipc_return_object(IpcResp *r,Service *s) { (void)r;(void)s;CHECK(false); }
'''

CHECKS = r'''
typedef struct { unsigned calls; Uint16 low,high; } Pulse;
static Pulse pulse[2];
static uint8_t *guest;
static bool SDLCALL rumble(void *userdata,Uint16 low,Uint16 high) {
    Pulse *p=userdata;
    p->calls++;
    p->low=low;
    p->high=high;
    return true;
}
static SDL_JoystickID attach(unsigned i,bool capable) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes=SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    d.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    d.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    d.name="Authored vibration regression";
    d.userdata=&pulse[i];
    d.Rumble=capable?rumble:NULL;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&d);
    CHECK(id);
    return id;
}
static void handles(unsigned player,uint32_t out[2]) {
    Cpu c={0};
    c.x[0]=(uintptr_t)out;
    c.x[1]=2;
    c.x[2]=(uintptr_t)&player;
    c.x[3]=1;
    hid_vibration_handles(&c);
    CHECK(c.x[0]==2);
}
static void send_one(uint32_t handle,float low,float high) {
    uint8_t payload[32]={0};
    float value[4]={low,160.f,high,320.f};
    memcpy(payload,&handle,4);
    memcpy(payload+4,value,16);
    IpcReq req={.cmd=201,.data=payload,.data_size=sizeof payload};
    IpcResp resp={0};
    hid(NULL,&req,&resp);
    CHECK(resp.result==0);
}
static void send_pair(const uint32_t handles[2],float low,float high) {
    float values[2][4]={{low,160.f,high,320.f},{low,160.f,high,320.f}};
    memcpy(guest,handles,8);
    memcpy(guest+128,values,sizeof values);
    uint64_t aruid=0;
    IpcReq req={.cmd=206,.data=(const uint8_t *)&aruid,.data_size=sizeof aruid,.nx=2};
    req.x[0]=(IpcBuf){(uintptr_t)guest,8};
    req.x[1]=(IpcBuf){(uintptr_t)(guest+128),sizeof values};
    IpcResp resp={0};
    hid(NULL,&req,&resp);
    CHECK(resp.result==0);
}
static void readback(uint32_t handle,float low,float high) {
    IpcReq req={.cmd=202,.data=(const uint8_t *)&handle,.data_size=4};
    IpcResp resp={0};
    hid(NULL,&req,&resp);
    CHECK(!resp.result && resp.data_size==16);
    float value[4];
    memcpy(value,resp.data,sizeof value);
    CHECK(value[0]==low && value[1]==160.f && value[2]==high && value[3]==320.f);
}
static void malformed(const uint32_t handles[2]) {
    float values[2][4]={{1,160,1,320},{1,160,1,320}};
    memcpy(guest,handles,8);
    memcpy(guest+128,values,sizeof values);
    for(unsigned i=0;i<11;i++) {
        uint64_t aruid=0;
        IpcReq req={.cmd=206,.data=(const uint8_t *)&aruid,.data_size=8,.nx=2};
        req.x[0]=(IpcBuf){(uintptr_t)guest,8};
        req.x[1]=(IpcBuf){(uintptr_t)(guest+128),32};
        if(i==0) req.x[0].size=7;
        if(i==1) req.x[1].size=31;
        if(i==2) req.x[1].size=16;
        if(i==3) req.nx=1;
        if(i==4) req.x[0].addr=GUEST_LO-4;
        if(i==5) req.x[1].addr=GUEST_HI-8;
        if(i==6) {
            req.cmd=201;
            req.data=NULL;
            req.data_size=32;
        }
        if(i==7) {
            req.cmd=201;
            req.data=guest;
            req.data_size=19;
        }
        if(i==8) {
            req.a[0]=req.x[0];
            req.a[1]=req.x[1];
            req.na=2;
            req.nx=0;
        }
        if(i==9) {
            req.x[0].size=16384;
            req.x[1]=(IpcBuf){(uintptr_t)(guest+16384),65536};
        }
        if(i==10) {
            req.cmd=204;
            req.data=NULL;
            req.data_size=1;
        }
        unsigned before0=pulse[0].calls,before1=pulse[1].calls;
        IpcResp resp={0};
        hid(NULL,&req,&resp);
        CHECK(resp.result!=0);
        CHECK(pulse[0].calls==before0 && pulse[1].calls==before1);
        readback(handles[0],0,0);
        readback(handles[1],0,0);
    }
}
static void permission(bool enabled) {
    uint8_t value=enabled;
    IpcReq req={.cmd=204,.data=&value,.data_size=1};
    IpcResp resp={0};
    hid(NULL,&req,&resp);
    CHECK(!resp.result);
    req=(IpcReq){.cmd=205};
    resp=(IpcResp){0};
    hid(NULL,&req,&resp);
    CHECK(!resp.result && resp.data_size==1 && resp.data[0]==enabled);
}
int main(void) {
    guest=VirtualAlloc((void *)(GUEST_LO+0x100000),0x20000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(guest==(void *)(GUEST_LO+0x100000));
    CHECK(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_JoystickID ids[2]={attach(0,true),attach(1,true)};
    pad=mk8_rumble_open(0,ids[0]);
    extra_pad[0]=mk8_rumble_open(1,ids[1]);
    CHECK(pad && extra_pad[0]);
    /* Check SDL first so a missing production pulse is not a broken virtual driver. */
    CHECK(SDL_RumbleGamepad(pad,123,456,1000));
    CHECK(pulse[0].low==123 && pulse[0].high==456);
    CHECK(SDL_RumbleGamepad(pad,0,0,0));
    memset(pulse,0,sizeof pulse);
    uint32_t p1[2],p2[2];
    handles(0,p1);
    handles(1,p2);
    send_one(p1[0],.4f,.7f);
    CHECK(pulse[0].calls>0 && pulse[0].low>0 && pulse[0].high>0);
    CHECK(!pulse[1].calls);
    CHECK(p1[0]==3 && p1[1]==0x10003 && p2[0]==0x103 && p2[1]==0x10103);
    readback(p1[0],.4f,.7f);
    readback(p1[1],0,0);
    uint32_t info[2]={0};
    Cpu c={0};
    c.x[0]=(uintptr_t)info;
    c.x[1]=(uintptr_t)&p1[1];
    hid_vibration_info(&c);
    CHECK(info[0]==1 && info[1]==2);
    send_pair(p2,.2f,.6f);
    CHECK(pulse[1].calls>0 && pulse[1].low>0 && pulse[1].high>0);
    unsigned second=pulse[1].calls;
    send_one(p1[0],0,0);
    CHECK(!pulse[0].low && !pulse[0].high && pulse[1].calls==second);
    send_pair(p2,0,0);
    CHECK(!pulse[1].low && !pulse[1].high);
    readback(p1[0],0,0);
    readback(p2[0],0,0);
    readback(p2[1],0,0);
    send_one(p1[0],1.5518f,2.f);
    CHECK(pulse[0].low==65535 && pulse[0].high==65535);
    readback(p1[0],1.f,1.f);
    send_one(p1[0],-1.f,NAN);
    CHECK(!pulse[0].low && !pulse[0].high);
    readback(p1[0],0,0);
    send_one(p1[1],.2f,.8f);
    Uint16 right_low=pulse[0].low,right_high=pulse[0].high;
    send_one(p1[0],.7f,.1f);
    send_one(p1[0],0,0);
    CHECK(pulse[0].low==right_low && pulse[0].high==right_high && right_low>0 && right_high>0);
    readback(p1[0],0,0);
    readback(p1[1],.2f,.8f);
    send_one(p1[1],0,0);
    CHECK(!pulse[0].low && !pulse[0].high);
    malformed(p1);
    send_one(p1[0],.5f,.4f);
    send_pair(p2,.3f,.2f);
    menu_open=true;
    mk8_rumble_frame();
    CHECK(!pulse[0].low && !pulse[0].high && !pulse[1].low && !pulse[1].high);
    send_pair(p1,.8f,.8f);
    CHECK(!pulse[0].low && !pulse[0].high);
    readback(p1[0],0,0);
    readback(p1[1],0,0);
    menu_open=false;
    mk8_rumble_frame();
    CHECK(!pulse[0].low && !pulse[0].high);
    send_pair(p1,.3f,.4f);
    native_settings_open=true;
    mk8_rumble_frame();
    CHECK(!pulse[0].low && !pulse[0].high);
    send_pair(p1,.8f,.8f);
    readback(p1[0],0,0);
    readback(p1[1],0,0);
    native_settings_open=false;
    mk8_rumble_frame();
    CHECK(!pulse[0].low && !pulse[0].high);
    send_pair(p1,.3f,.4f);
    send_pair(p2,.5f,.6f);
    mk8_rumble_reset();
    CHECK(!pulse[0].low && !pulse[0].high && !pulse[1].low && !pulse[1].high);
    readback(p1[0],0,0);
    readback(p2[1],0,0);
    for(unsigned focus=0;focus<2;focus++) {
        send_pair(p1,.5f,.6f);
        send_pair(p2,.6f,.7f);
        if(focus) mk8_rumble_focus(false);
        else permission(false);
        CHECK(!pulse[0].low && !pulse[0].high && !pulse[1].low && !pulse[1].high);
        send_pair(p1,.7f,.8f);
        send_pair(p2,.8f,.9f);
        CHECK(!pulse[0].low && !pulse[0].high && !pulse[1].low && !pulse[1].high);
        readback(p1[0],0,0);
        readback(p2[1],0,0);
        if(focus) mk8_rumble_focus(true);
        else permission(true);
        mk8_rumble_frame();
        CHECK(!pulse[0].low && !pulse[0].high && !pulse[1].low && !pulse[1].high);
        send_pair(p1,.3f,.4f);
        CHECK(pulse[0].low>0 && pulse[0].high>0);
        send_pair(p1,0,0);
    }
    /* Only the clock is controlled: real frame logic and real SDL output execute at each boundary. */
    clock_controlled=true;
    clock_ms=1000;
    send_one(p1[0],.8f,.9f);
    unsigned renewals=rumble_requests;
    CHECK(last_duration==250);
    clock_ms=1124;
    mk8_rumble_frame();
    CHECK(rumble_requests==renewals);
    clock_ms=1125;
    mk8_rumble_frame();
    CHECK(rumble_requests==renewals+1 && last_duration==250);
    clock_ms=1250;
    mk8_rumble_frame();
    CHECK(rumble_requests==renewals+2 && last_duration==250);
    clock_ms=1500;
    mk8_rumble_frame();
    CHECK(!pulse[0].low && !pulse[0].high && last_duration==0);
    readback(p1[0],0,0);
    clock_controlled=false;
    mk8_rumble_close(1,extra_pad[0]);
    extra_pad[0]=NULL;
    CHECK(SDL_DetachVirtualJoystick(ids[1]));
    second=pulse[1].calls;
    send_pair(p2,.5f,.5f);
    CHECK(pulse[1].calls==second);
    ids[1]=attach(1,false);
    extra_pad[0]=mk8_rumble_open(1,ids[1]);
    CHECK(extra_pad[0]);
    second=pulse[1].calls;
    send_pair(p2,.5f,.5f);
    CHECK(pulse[1].calls==second);
    mk8_rumble_close(1,extra_pad[0]);
    extra_pad[0]=NULL;
    CHECK(SDL_DetachVirtualJoystick(ids[1]));
    ids[1]=attach(1,true);
    extra_pad[0]=mk8_rumble_open(1,ids[1]);
    CHECK(extra_pad[0]);
    CHECK(!pulse[1].low && !pulse[1].high); /* New hardware must not inherit an unavailable device's pulse. */
    send_pair(p2,.3f,.4f);
    CHECK(pulse[1].low>0 && pulse[1].high>0);
    send_pair(p2,0,0);
    CHECK(!pulse[1].low && !pulse[1].high);
    send_one(p1[0],.8f,.3f);
    CHECK(pulse[0].low>0 && pulse[0].high>0);
    send_one(p1[0],0,0);
    CHECK(!pulse[0].low && !pulse[0].high);
    send_one(p1[0],.4f,.4f);
    mk8_rumble_close(0,pad);
    pad=NULL;
    CHECK(!pulse[0].low && !pulse[0].high);
    mk8_rumble_close(1,extra_pad[0]);
    extra_pad[0]=NULL;
    CHECK(SDL_DetachVirtualJoystick(ids[0]));
    CHECK(SDL_DetachVirtualJoystick(ids[1]));
    SDL_Quit();
    CHECK(VirtualFree(guest,0,MEM_RELEASE));
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class ControllerRumbleTests(unittest.TestCase):
    def test_hid_values_reach_independent_virtual_pads_and_stop(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)
            runtime=prepare_host_sources(SOURCE,out/'host')
            text=(runtime/'hle/input.c').read_text(encoding='utf-8')
            start=text.index('// Vibration devices:')
            end=text.index('// Controller support (',start)
            vibration=text[start:end]
            service=(runtime/'services/hid_set.c').read_text(encoding='utf-8')
            start=service.index('static uint32_t new_event_handle(void)')
            end=service.index('SERVICE("hid:sys"',start)
            source=PREFIX+'\n#define SDL_GetTicks test_ticks\n#define SDL_RumbleGamepad test_rumble\n'+vibration
            source+='\n#undef SDL_GetTicks\n#undef SDL_RumbleGamepad\n'+service[start:end]+'\n'+CHECKS
            (out/'rumble.c').write_text(source,encoding='utf-8')
            exe=out/'rumble.exe'
            build=subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                '-I'+str(runtime),'-I'+str(DEPS/'include'),str(out/'rumble.c'),str(DEPS/'lib/libSDL3.dll.a'),
                '-o',str(exe)],env=compiler_env(),capture_output=True,text=True,timeout=90)
            self.assertEqual(build.returncode,0,build.stderr[-5000:])
            env=dict(os.environ,PATH=str(DEPS/'bin')+os.pathsep+os.environ.get('PATH',''))
            run=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=20)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)


if __name__=='__main__': unittest.main()
