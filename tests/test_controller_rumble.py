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
#include <pthread.h>
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
Settings g_settings={.controller_vibration=true};
static bool menu_open;
bool menu_is_open(void) { return menu_open; }
static bool native_settings_open;
bool mk8_settings_page_active(void) { return native_settings_open; }
static bool clock_controlled;
static Uint64 clock_ms;
static SDL_JoystickID virtual_hidapi_id;
static Uint16 switch_product=0x2009;
static SDL_GUID fixture_guid(SDL_JoystickID id) {
    SDL_GUID guid=SDL_GetJoystickGUIDForID(id);
    if(id==virtual_hidapi_id) guid.data[14]='h';
    return guid;
}
static unsigned rumble_requests;
static Uint32 last_duration;
static SDL_Semaphore *basic_stop_entered,*release_basic_stop;
static SDL_Semaphore *active_entered,*release_active;
static _Atomic bool block_next_active;
static SDL_Semaphore *waiting_for_space,*producer_awake,*resume_producer;
static int wait_for_space(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *until) {
    if (waiting_for_space) SDL_SignalSemaphore(waiting_for_space);
    int rc=pthread_cond_timedwait(cond,mutex,until);
    if (resume_producer) {
        pthread_mutex_unlock(mutex);
        SDL_SignalSemaphore(producer_awake);
        SDL_WaitSemaphore(resume_producer);
        pthread_mutex_lock(mutex);
    }
    return rc;
}
static Uint64 test_ticks(void) { return clock_controlled ? clock_ms : SDL_GetTicks(); }
static bool test_rumble(SDL_Gamepad *p,Uint16 low,Uint16 high,Uint32 duration) {
    rumble_requests++;
    last_duration=duration;
    if ((low || high) && atomic_exchange(&block_next_active,false)) {
        SDL_SignalSemaphore(active_entered);
        SDL_WaitSemaphore(release_active);
    }
    if(basic_stop_entered && !low && !high) {
        SDL_SignalSemaphore(basic_stop_entered);
        SDL_WaitSemaphore(release_basic_stop);
    }
    return SDL_RumbleGamepad(p,low,high,duration);
}
#define EXTRA_PADS 3
static SDL_Gamepad *pad, *extra_pad[EXTRA_PADS];
static bool mk8_paired_slot[1 + EXTRA_PADS];
KEvent *kernel_create_event_pair(uint32_t *w,uint32_t *r) { (void)w;(void)r;CHECK(false);return NULL; }
KSharedMem *kernel_create_shared_memory(uint64_t size,void (*fn)(KSharedMem *),uint32_t *handle) {
    (void)size;(void)fn;(void)handle;CHECK(false);return NULL;
}
Service *service_new(const char *name,ServiceHandler h,void *state) {
    (void)name;(void)h;(void)state;CHECK(false);return NULL;
}
void ipc_return_object(IpcResp *r,Service *s) { (void)r;(void)s;CHECK(false); }
uint32_t mk8_motion_control(const uint8_t *data, size_t size, bool start) {
    (void)data; (void)size; (void)start; abort();  // Vibration commands must never dispatch to motion.
}
'''

PAIR_FIXTURE = r'''
/* SDL's virtual driver has no HID endpoints. Substitute only the discovery and raw-output boundary. */
static SDL_hid_device_info endpoints[3];
static _Atomic int live_halves, raw_writes[2], raw_active[2], rejected_side=-1;
static _Atomic bool reject_next_right;
static SDL_Semaphore *entered_write,*release_write,*polled;
static _Atomic bool block_next_left;
static SDL_Mutex *raw_lock;
static Uint64 raw_at[2], previous_at[2];
static Uint8 raw_packet[2][49];
static SDL_JoystickID *pair_ids(int *count) {
    SDL_JoystickID *ids=SDL_calloc(2,sizeof(*ids));
    CHECK(ids);
    ids[0]=virtual_hidapi_id;
    *count=1;
    return ids;
}
static SDL_hid_device_info *test_enumerate(Uint16 vid,Uint16 pid) {
    CHECK(vid==0x057e && !pid);
    return endpoints;
}
static SDL_hid_device *test_open(const char *path) {
    for(int i=0;i<2;i++) if(!strcmp(path,endpoints[i].path)) {
        live_halves++;
        return (SDL_hid_device *)&endpoints[i];
    }
    return NULL;
}
static int test_hid_write(SDL_hid_device *dev,const unsigned char *data,size_t size) {
    int side=dev==(SDL_hid_device *)&endpoints[1];
    CHECK(dev==(SDL_hid_device *)&endpoints[side] && size==49 && data[0]==0x10 && data[1]<16);
    for(size_t i=10;i<size;i++) CHECK(!data[i]);
    if(!side && block_next_left) {
        block_next_left=false;
        SDL_SignalSemaphore(entered_write);
        SDL_WaitSemaphore(release_write);
    }
    SDL_LockMutex(raw_lock);
    previous_at[side]=raw_at[side];raw_at[side]=SDL_GetTicks();
    bool fail=rejected_side==side || (side==1 && atomic_exchange(&reject_next_right,false));
    if(!fail) {
        memcpy(raw_packet[side],data,size);
        raw_writes[side]++;
        const Uint8 silent[8]={0,1,0x40,0x40,0,1,0x40,0x40};
        if(memcmp(data+2,silent,8)) raw_active[side]++;
    }
    SDL_UnlockMutex(raw_lock);
    return fail?-1:(int)size;
}
#define SDL_GetGamepads pair_ids
#define SDL_GetRealGamepadTypeForID(id) SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
#define SDL_GetGamepadPath(device) "nintendo_joycons_combined"
#define SDL_GetGamepadConnectionState(device) SDL_JOYSTICK_CONNECTION_WIRELESS
#define SDL_hid_enumerate test_enumerate
#define SDL_hid_free_enumeration(list) ((void)(list))
#define SDL_hid_open_path test_open
#define SDL_hid_close(dev) ((void)(dev),live_halves--)
#define SDL_hid_get_device_info(dev) ((SDL_hid_device_info *)(dev))
#define SDL_hid_write test_hid_write
'''

CHECKS = r'''
typedef struct { unsigned calls; Uint16 low,high; } Pulse;
static Pulse pulse[4];
static Uint8 last_packet[10];
static _Atomic unsigned packet_changes;
static _Atomic unsigned packet_writes;
static _Atomic unsigned neutral_writes;
static Uint64 last_write_ms;
static bool check_radio_interval;
static _Atomic bool reject_packet;
static uint8_t *guest;
static bool defer_output;
static bool SDLCALL rumble(void *userdata,Uint16 low,Uint16 high) {
    Pulse *p=userdata;
    p->calls++;
    p->low=low;
    p->high=high;
    return true;
}
static bool SDLCALL send_effect(void *userdata,const void *data,int size) {
    CHECK(userdata==&pulse[2] && size==sizeof last_packet);
    if(reject_packet) return false;
    Uint64 now=SDL_GetTicks();
    if(check_radio_interval && last_write_ms) CHECK(now-last_write_ms>=30);
    last_write_ms=now;
    ++packet_writes;
    bool changed=memcmp(last_packet+2,(const Uint8 *)data+2,8)!=0;
    memcpy(last_packet,data,sizeof last_packet);
    static const Uint8 silent_sides[8]={0,1,0x40,0x40,0,1,0x40,0x40};
    if(!memcmp(last_packet+2,silent_sides,sizeof silent_sides)) ++neutral_writes;
    if(changed) ++packet_changes;
    return true;
}
static bool motion_fixture;
static bool SDLCALL enable_motion(void *unused,bool enabled) { return true; }
static SDL_JoystickID attach(unsigned i,bool capable) {
    SDL_VirtualJoystickSensorDesc gyro={SDL_SENSOR_GYRO,200};
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
    if(i==2) {
        d.vendor_id=0x057e;
        d.product_id=switch_product;
        d.SendEffect=send_effect;
        if(motion_fixture) {
            d.nsensors=1; d.sensors=&gyro; d.SetSensorsEnabled=enable_motion;
        }
    }
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
static void send_tone(uint32_t handle,float low,float low_hz,float high,float high_hz) {
    uint8_t payload[32]={0};
    float value[4]={low,low_hz,high,high_hz};
    memcpy(payload,&handle,4);
    memcpy(payload+4,value,16);
    IpcReq req={.cmd=201,.data=payload,.data_size=sizeof payload};
    IpcResp resp={0};
    hid(NULL,&req,&resp);
    CHECK(!resp.result);
    if (!defer_output) mk8_rumble_pump();
}
static void send_one(uint32_t handle,float low,float high) {
    send_tone(handle,low,160.f,high,320.f);
}
static void send_batch(const uint32_t handles[2],const float values[2][4]) {
    memcpy(guest,handles,8);
    const size_t bytes=2*sizeof values[0];
    memcpy(guest+128,values,bytes);
    uint64_t aruid=0;
    IpcReq req={.cmd=206,.data=(const uint8_t *)&aruid,.data_size=sizeof aruid,.nx=2};
    req.x[0]=(IpcBuf){(uintptr_t)guest,8};
    req.x[1]=(IpcBuf){(uintptr_t)(guest+128),bytes};
    IpcResp resp={0};
    hid(NULL,&req,&resp);
    CHECK(resp.result==0);
    if (!defer_output) mk8_rumble_pump();
}
static void send_pair(const uint32_t handles[2],float low,float high) {
    const float values[2][4]={{low,160.f,high,320.f},{low,160.f,high,320.f}};
    send_batch(handles,values);
}
static void present_and_pump(void) { mk8_rumble_frame(); mk8_rumble_pump(); }
static int SDLCALL present_idle_frame(void *done) {
    mk8_rumble_frame();
    SDL_SignalSemaphore(done);
    return 0;
}
static void check_idle_frame(void) {
    SDL_Semaphore *done=SDL_CreateSemaphore(0);
    CHECK(done);
    SDL_LockJoysticks();  /* Discovery may hold this lock while a sensor is enabled. */
    SDL_Thread *frame=SDL_CreateThread(present_idle_frame,"idle-rumble-frame",done);
    CHECK(frame);
    bool finished=SDL_WaitSemaphoreTimeout(done,1000);
    SDL_UnlockJoysticks();
    SDL_WaitThread(frame,NULL);
    SDL_DestroySemaphore(done);
    CHECK(finished && "an idle presentation frame waited for the joystick lock");
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
static int SDLCALL four_player_output(void *done) {
    uint32_t handles4[8];
    for (unsigned i=0;i<4;++i) handles(i,handles4+2*i);
    memcpy(guest,handles4,sizeof handles4);
    float values[8][4];
    uint64_t aruid=0;
    IpcReq req={.cmd=206,.data=(const uint8_t *)&aruid,.data_size=8,.nx=2};
    req.x[0]=(IpcBuf){(uintptr_t)guest,sizeof handles4};
    req.x[1]=(IpcBuf){(uintptr_t)(guest+128),sizeof values};
    for (int active=1;active>=0;--active) {
        for (unsigned i=0;i<8;++i) {
            float value[]={active*(i+1)/8.f,160.f,0,320.f};
            memcpy(values[i],value,sizeof value);
        }
        memcpy(guest+128,values,sizeof values);
        IpcResp resp={0};
        hid(NULL,&req,&resp);
        CHECK(!resp.result);
        for (unsigned i=0;i<8;++i) readback(handles4[i],values[i][0],0);
    }
    mk8_rumble_frame();
    SDL_SignalSemaphore(done);
    return 0;
}
static void check_four_player_contention(void) {
    SDL_JoystickID extra[]={attach(2,true),attach(3,true)};
    for (unsigned i=0;i<2;++i) {
        extra_pad[i+1]=mk8_rumble_open(i+2,extra[i]);
        CHECK(extra_pad[i+1]);
    }
    unsigned before[4];
    for (unsigned i=0;i<4;++i) before[i]=pulse[i].calls;
    SDL_Semaphore *done=SDL_CreateSemaphore(0);
    SDL_LockJoysticks();
    SDL_Thread *guest_thread=SDL_CreateThread(four_player_output,"guest-rumble-ipc",done);
    CHECK(done && guest_thread);
    bool completed=SDL_WaitSemaphoreTimeout(done,1000);
    SDL_UnlockJoysticks();
    SDL_WaitThread(guest_thread,NULL);
    CHECK(completed && "guest readback, eight-effect writes or presentation waited for SDL discovery");
    for (unsigned i=0;i<4;++i) CHECK(pulse[i].calls==before[i]);
    mk8_rumble_pump();
    for (unsigned i=0;i<4;++i) CHECK(pulse[i].calls==before[i]+2 && !pulse[i].low && !pulse[i].high);
    SDL_DestroySemaphore(done);
    for (unsigned i=0;i<2;++i) {
        mk8_rumble_close(i+2,extra_pad[i+1]);
        extra_pad[i+1]=NULL;
        SDL_DetachVirtualJoystick(extra[i]);
    }
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
    if (!defer_output) mk8_rumble_pump();
}
static SDL_Semaphore *submitted;
static int SDLCALL submit_to_full_queue(void *arg) {
    send_one(*(uint32_t *)arg,.8f,.4f);
    SDL_SignalSemaphore(submitted);
    return 0;
}
static int SDLCALL pump_output(void *arg) {
    (void)arg;
    mk8_rumble_pump();
    return 0;
}
static void check_output_lifetime(uint32_t h) {
    defer_output=true;
    unsigned before=pulse[0].calls;
    send_one(h,.5f,.25f);
    readback(h,.5f,.25f);
    SDL_JoystickID id=SDL_GetGamepadID(pad);
    mk8_rumble_close(0,pad);
    pad=mk8_rumble_open(0,id);
    readback(h,0,0);
    mk8_rumble_pump();
    CHECK(pulse[0].calls==before);  /* The old request cannot reach the reopened device. */
    waiting_for_space=SDL_CreateSemaphore(0); submitted=SDL_CreateSemaphore(0);
    producer_awake=SDL_CreateSemaphore(0); resume_producer=SDL_CreateSemaphore(0);
    CHECK(waiting_for_space && submitted && producer_awake && resume_producer);
    for (int revoke=0;revoke<=1;++revoke) {
        for (unsigned i=0;i<MK8_OUTPUT_QUEUE;++i) send_one(h,.2f,.1f);
        SDL_Thread *producer=SDL_CreateThread(submit_to_full_queue,"full-rumble-queue",&h);
        CHECK(producer && SDL_WaitSemaphoreTimeout(waiting_for_space,1000));
        if (revoke) {
            permission(false);
        } else {
            mk8_rumble_close(0,pad);
            pad=mk8_rumble_open(0,id);
            mk8_rumble_pump();
        }
        CHECK(SDL_WaitSemaphoreTimeout(producer_awake,1000));
        if (revoke) permission(true);
        SDL_SignalSemaphore(resume_producer);
        CHECK(SDL_WaitSemaphoreTimeout(submitted,1000));
        SDL_WaitThread(producer,NULL);
        readback(h,0,0);
        mk8_rumble_pump();
        CHECK(pulse[0].calls==before);
    }
    SDL_DestroySemaphore(waiting_for_space); SDL_DestroySemaphore(submitted);
    SDL_DestroySemaphore(producer_awake); SDL_DestroySemaphore(resume_producer);
    waiting_for_space=resume_producer=NULL;
    uint32_t p2[2]; handles(1,p2);
    const uint32_t targets[]={h,p2[0]};
    const float motors[2][4]={{.6f,160,.1f,320},{.7f,160,.3f,320}};
    unsigned second_before=pulse[1].calls;
    active_entered=SDL_CreateSemaphore(0); release_active=SDL_CreateSemaphore(0);
    CHECK(active_entered && release_active);
    send_batch(targets,motors);
    block_next_active=true;
    SDL_Thread *owner=SDL_CreateThread(pump_output,"blocked-output-owner",NULL);
    CHECK(owner && SDL_WaitSemaphoreTimeout(active_entered,1000));
    permission(false); permission(true);
    SDL_SignalSemaphore(release_active);
    SDL_WaitThread(owner,NULL);
    mk8_rumble_pump();
    readback(h,0,0); readback(p2[0],0,0);
    CHECK(pulse[1].calls==second_before && !pulse[0].low && !pulse[0].high);
    SDL_DestroySemaphore(active_entered); SDL_DestroySemaphore(release_active);
    defer_output=false;
    send_one(h,.7f,.2f);
    before=pulse[0].calls;
    clock_ms=10000+SDL_GetTicks();
    clock_controlled=true;
    defer_output=true;
    /* During a two-second reconnect, 120 active writes must finish before the UI resumes.
       Once the last maneuver expires, the owner must deliver its stop without replaying it. */
    for (unsigned i=0;i<120;++i) {
        clock_ms+=16;
        send_one(h,.3f,.5f);
    }
    send_one(h,0,0);
    clock_ms+=251;
    mk8_rumble_pump();
    CHECK(pulse[0].calls==before+1 && !pulse[0].low && !pulse[0].high);
    readback(h,0,0);
    defer_output=false;
    clock_controlled=false;
}
static void expect_effect(const Uint8 left[4],const Uint8 right[4]) {
    Uint64 deadline=SDL_GetTicks()+1000;
    do {
        SDL_LockJoysticks();
        bool matched=last_packet[0]==0x10 && !memcmp(last_packet+2,left,4) && !memcmp(last_packet+6,right,4);
        SDL_UnlockJoysticks();
        if(matched) return;
        SDL_Delay(5);
    } while(SDL_GetTicks()<deadline);
    CHECK(false && "expected Nintendo actuator packet was not delivered");
}
static void expect_generic_pulse(void) {
    Uint64 deadline=SDL_GetTicks()+1000;
    do {
        SDL_LockJoysticks();
        bool active=pulse[2].low && pulse[2].high;
        SDL_UnlockJoysticks();
        if(active) return;
        SDL_Delay(5);
    } while(SDL_GetTicks()<deadline);
    CHECK(false && "generic rumble did not take over after rejected HD output");
}
static void hd_output(void) {
    /* Independent byte vectors from measured Switch frequency and amplitude tables. */
    const Uint8 neutral[4]={0x00,0x01,0x40,0x40};
    const Uint8 original[4]={0x00,0x49,0x40,0x62};
    const Uint8 retuned[4]={0x80,0x48,0x20,0x62};
    const Uint8 quiet[4]={0x80,0x22,0xa0,0x41};
    SDL_JoystickID id=attach(2,true);
    /* Only the driver's GUID marker is supplied; SDL owns both output callbacks. */
    virtual_hidapi_id=id;
    extra_pad[1]=mk8_rumble_open(2,id);
    CHECK(extra_pad[1] && SDL_IsJoystickVirtual(id));
    CHECK(SDL_GetGamepadVendor(extra_pad[1])==0x057e && SDL_GetGamepadProduct(extra_pad[1])==0x2009);
    Uint8 probe[10]={0x10,0};
    memcpy(probe+2,neutral,4);
    memcpy(probe+6,neutral,4);
    CHECK(SDL_SendGamepadEffect(extra_pad[1],probe,sizeof probe));
    CHECK(!memcmp(last_packet,probe,sizeof probe));
    packet_changes=0;
    last_write_ms=0;
    check_radio_interval=true;
    uint32_t p3[2];
    handles(2,p3);
    send_tone(p3[0],.501433f,160,.250715f,320);
    expect_effect(original,neutral);
    CHECK(packet_changes==1);
    /* Settings can reassign the same physical radio to another player immediately. */
    mk8_rumble_close(2,extra_pad[1]);
    extra_pad[1]=NULL;
    extra_pad[2]=mk8_rumble_open(3,id);
    CHECK(extra_pad[2]);
    uint32_t p4[2];
    handles(3,p4);
    send_tone(p4[1],.014061f,80,.122671f,160);
    expect_effect(neutral,quiet);
    mk8_rumble_close(3,extra_pad[2]);
    extra_pad[2]=NULL;
    extra_pad[1]=mk8_rumble_open(2,id);
    CHECK(extra_pad[1]);
    send_tone(p3[0],.501433f,160,.250715f,320);
    expect_effect(original,neutral);
    send_tone(p3[0],.501433f,80,.250715f,160);
    expect_effect(retuned,neutral);
    unsigned stops=neutral_writes;
    send_one(p3[0],0,0);
    send_tone(p3[0],.501433f,160,.250715f,320);
    expect_effect(original,neutral);
    CHECK(neutral_writes==stops+1);
    const float stereo[2][4]={{.501433f,160,.250715f,320},{.014061f,80,.122671f,160}};
    unsigned before_batch=packet_changes;
    send_batch(p3,stereo);
    expect_effect(original,quiet);
    CHECK(packet_changes==before_batch+1);
    /* Revocation stops an HD actuator without waiting for the UI queue or its 250 ms watchdog. */
    defer_output=true;
    Uint64 revoke_at=SDL_GetTicks();
    permission(false); permission(true);
    expect_effect(neutral,neutral);
    CHECK(SDL_GetTicks()-revoke_at<200);
    mk8_rumble_pump();
    defer_output=false;
    send_batch(p3,stereo);
    expect_effect(original,quiet);
    send_one(p3[0],0,0);
    expect_effect(neutral,quiet);
    mk8_rumble_focus(false);
    expect_effect(neutral,neutral);
    mk8_rumble_focus(true);
    send_tone(p3[0],9,9000,9,9000);
    expect_effect((Uint8[]){0xfc,0xc9,0x7f,0x72},neutral);
    send_tone(p3[0],1,NAN,.122671f,160);
    expect_effect((Uint8[]){0x80,0x22,0x40,0x40},neutral);
    mk8_rumble_reset();
    expect_effect(neutral,neutral);
    send_tone(p3[1],.014061f,80,.122671f,160);
    expect_effect(neutral,quiet);
    unsigned held_writes=packet_writes;
    Uint64 hold_until=SDL_GetTicks()+350;
    while(SDL_GetTicks()<hold_until) {
        present_and_pump();
        SDL_Delay(16);
    }
    expect_effect(neutral,quiet);
    CHECK(packet_writes>held_writes);
    /* A stalled presentation thread cannot call mk8_rumble_frame to stop the hardware. */
    expect_effect(neutral,neutral);
    CHECK(!pulse[2].calls);
    send_tone(p3[0],.501433f,160,.250715f,320);
    expect_effect(original,neutral);
    reject_packet=true;
    send_tone(p3[1],.501433f,160,.250715f,320);
    expect_generic_pulse();
    mk8_rumble_reset();
    CHECK(!pulse[2].low && !pulse[2].high);
    mk8_rumble_close(2,extra_pad[1]);
    extra_pad[1]=mk8_rumble_open(2,id);
    CHECK(extra_pad[1]);
    /* An unsupported effect on first use also falls back immediately. */
    send_tone(p3[1],.501433f,160,.250715f,320);
    expect_generic_pulse();
    reject_packet=false;
    mk8_rumble_close(2,extra_pad[1]);
    /* A Steam/virtual translation can retain the Nintendo VID/PID on a different driver. */
    virtual_hidapi_id=0;
    extra_pad[1]=mk8_rumble_open(2,id);
    CHECK(extra_pad[1]);
    unsigned before=packet_changes;
    send_tone(p3[0],.501433f,160,.250715f,320);
    CHECK(packet_changes==before && pulse[2].low && pulse[2].high);
    mk8_rumble_close(2,extra_pad[1]);
    extra_pad[1]=NULL;
    CHECK(SDL_DetachVirtualJoystick(id));
}
static void deliver_gyro(SDL_Gamepad *device, Uint64 sample) {
    const float data[]={(float)sample,0,0};
    CHECK(SDL_SendJoystickVirtualSensorData(SDL_GetGamepadJoystick(device),SDL_SENSOR_GYRO,sample,data,3));
    SDL_UpdateJoysticks();
    SDL_Event e;
    while(SDL_PollEvent(&e)) dispatch_sensor(&e);
}
static void motion_paces_hd_output(void) {
    motion_fixture=true;
    uint32_t h[2]; handles(0,h);
    const Uint16 products[]={0x2006,0x2007,0x2009};
    for(unsigned n=0;n<SDL_arraysize(products);n++) {
        switch_product=products[n];
        SDL_JoystickID id=attach(2,true);
        virtual_hidapi_id=id; pad=mk8_rumble_open(0,id);
        CHECK(pad && SDL_SetGamepadSensorEnabled(pad,SDL_SENSOR_GYRO,true));
        /* Hold the timer outside the test; guest updates still use the usual SDL output path. */
        SDL_LockJoysticks();
        SDL_Delay(55);
        unsigned before=packet_writes;
        send_one(h[0],.25f,0);
        CHECK(packet_writes==before+1);
        SDL_Delay(55);
        send_one(h[0],.5f,0);
        CHECK(packet_writes==before+1);
        deliver_gyro(pad,1);
        send_one(h[0],0,0);
        CHECK(packet_writes==before+2);
        SDL_Delay(55);
        send_one(h[0],.4f,0);
        CHECK(packet_writes==before+2);
        SDL_Event e={0};
        e.type=SDL_EVENT_GAMEPAD_SENSOR_UPDATE;
        e.gsensor.sensor=SDL_SENSOR_GYRO;
        e.gsensor.timestamp=SDL_GetTicksNS();
        e.gsensor.which=id+1; dispatch_sensor(&e);
        e.gsensor.which=id; e.gsensor.timestamp=1; dispatch_sensor(&e);
        send_one(h[0],.501433f,0);
        CHECK(packet_writes==before+2);
        deliver_gyro(pad,2);
        SDL_UnlockJoysticks();
        /* The existing timer sends the most recent staged value without another guest command. */
        const Uint8 low[]={0,1,0x40,switch_product==0x2009?0x62:0x52}, silent[]={0,1,0x40,0x40};
        expect_effect(switch_product==0x2007?silent:low,switch_product==0x2007?low:silent);
        SDL_LockJoysticks();
        CHECK(packet_writes==before+3);
        SDL_Delay(55);
        send_one(h[0],.7f,0);
        CHECK(packet_writes==before+3);
        CHECK(SDL_SetGamepadSensorEnabled(pad,SDL_SENSOR_GYRO,false));
        send_one(h[0],.8f,0);
        CHECK(packet_writes==before+4);
        SDL_UnlockJoysticks();
        if (switch_product==0x2009) {
            mk8_rumble_set_enabled(false);
            mk8_rumble_pump();
            expect_effect(silent,silent);
            send_one(h[0],.4f,0);
            readback(h[0],0,0);
            mk8_rumble_set_enabled(true);
        }
        mk8_rumble_close(0,pad); pad=NULL;
        CHECK(SDL_DetachVirtualJoystick(id));
    }
    motion_fixture=false;
}
static int SDLCALL submit_tone(void *arg) {
    send_tone(*(uint32_t *)arg,.2f,160,0,320);
    return 0;
}
static int SDLCALL poll_and_submit(void *arg) {
    SDL_LockJoysticks();
    CHECK(SDL_GetGamepadFromID(virtual_hidapi_id)==pad);
    SDL_UnlockJoysticks();
    send_tone(*(uint32_t *)arg,.4f,80,0,320);
    SDL_SignalSemaphore(polled);
    return 0;
}
static void expect_raw(int side,const Uint8 left[4],const Uint8 right[4]) {
    Uint64 until=SDL_GetTicks()+1000;
    do {
        SDL_LockMutex(raw_lock);
        bool same=raw_packet[side][0]==0x10 && !memcmp(raw_packet[side]+2,left,4) && !memcmp(raw_packet[side]+6,right,4);
        SDL_UnlockMutex(raw_lock);
        if(same)return;
        SDL_Delay(2);
    } while(SDL_GetTicks()<until);
    CHECK(false && "Joy-Con did not receive the expected report");
}
static void wait_pair_closed(void) {
    Uint64 until=SDL_GetTicks()+1500;
    while(SDL_GetAtomicInt(&mk8_pair_lease) && SDL_GetTicks()<until) SDL_Delay(2);
    CHECK(SDL_GetAtomicInt(&mk8_pair_lease)==MK8_PAIR_FREE && !live_halves);
}
static void joycon_output(void) {
    /* At half amplitude, .501433 maps to table entry 36 and .133774 maps to entry 12. */
    const Uint8 neutral[4]={0,1,0x40,0x40}, lo[4]={0,1,0x40,0x52}, hi[4]={0x80,0x18,0x40,0x40};
    const Uint8 mixed[4]={0x80,0x18,0x40,0x52};
    uint32_t h[2];
    handles(0,h);
    for(switch_product=0x2006;switch_product<=0x2007;switch_product++) {
        SDL_JoystickID id=attach(2,true);
        virtual_hidapi_id=id; pad=mk8_rumble_open(0,id);
        CHECK(pad && mk8_rumble_pads[0].hd);
        /* Both logical grips drive the one physical actuator, even for a right Joy-Con on P1. */
        const float bands[2][4]={{.501433f,160,0,320},{0,160,.133774f,160}};
        send_batch(h,bands);
        expect_effect(switch_product==0x2006?mixed:neutral,switch_product==0x2007?mixed:neutral);
        mk8_rumble_focus(false);
        expect_effect(neutral,neutral);
        mk8_rumble_focus(true);
        const float bad_frequency[2][4]={{.9f,NAN,.133774f,160},{.501433f,160,.9f,0}};
        send_batch(h,bad_frequency);
        expect_effect(switch_product==0x2006?mixed:neutral,switch_product==0x2007?mixed:neutral);
        mk8_rumble_close(0,pad);
        CHECK(!memcmp(last_packet+2,neutral,4) && !memcmp(last_packet+6,neutral,4));
        CHECK(SDL_DetachVirtualJoystick(id));
    }
    switch_product=0x2008;
    raw_lock=SDL_CreateMutex();CHECK(raw_lock);
    SDL_JoystickID id=attach(2,true);
    virtual_hidapi_id=id;
    for(int i=0;i<2;i++) endpoints[i]=(SDL_hid_device_info){.vendor_id=0x057e,.product_id=(Uint16)(0x2006+i),
        .path=i?"right":"left",.manufacturer_string=L"Nintendo",.usage_page=1,.usage=5,
        .bus_type=SDL_HID_API_BUS_BLUETOOTH,.next=i?NULL:&endpoints[1]};
    endpoints[2]=endpoints[0];endpoints[2].next=NULL;endpoints[1].next=&endpoints[2];
    pad=mk8_rumble_open(0,id);
    CHECK(pad && !mk8_rumble_pads[0].hd && live_halves==0);
    send_pair(h,.2f,.3f);
    CHECK(pulse[2].low==6554 && pulse[2].high==9830);
    readback(h[0],.2f,.3f);
    readback(h[1],.2f,.3f);
    quick_exit_handler();
    CHECK(fake_exit_called && !pulse[2].low && !pulse[2].high);
    /* Guest threads can still flush valid haptics while the host drains its GPU/cache. */
    unsigned after_stop=pulse[2].calls;
    send_pair(h,.8f,.4f);
    send_one(h[0],1,1);
    readback(h[0],0,0); readback(h[1],0,0);
    malformed(h);
    CHECK(pulse[2].calls==after_stop && !pulse[2].low && !pulse[2].high);
    /* The rendering process would terminate here. Reset the exit latch for the next fixture. */
    SDL_SetAtomicInt(&mk8_pair_exiting,0);mk8_output_exiting=false;fake_exit_called=false;
    send_pair(h,.2f,.3f);
    basic_stop_entered=SDL_CreateSemaphore(0);release_basic_stop=SDL_CreateSemaphore(0);
    Uint64 basic_exit=SDL_GetTicks();
    quick_exit_handler();
    CHECK(fake_exit_called && SDL_GetTicks()-basic_exit<450 && SDL_TryWaitSemaphore(basic_stop_entered));
    SDL_SignalSemaphore(release_basic_stop);
    Uint64 stop_deadline=SDL_GetTicks()+1000;
    while(!SDL_GetAtomicInt(&mk8_pair_stop_done) && SDL_GetTicks()<stop_deadline) SDL_Delay(2);
    CHECK(SDL_GetAtomicInt(&mk8_pair_stop_done) && !pulse[2].low && !pulse[2].high);
    SDL_DestroySemaphore(basic_stop_entered);SDL_DestroySemaphore(release_basic_stop);
    basic_stop_entered=NULL;release_basic_stop=NULL;
    SDL_SetAtomicInt(&mk8_pair_exiting,0);mk8_output_exiting=false;fake_exit_called=false;
    mk8_rumble_close(0,pad);
    endpoints[1].next=NULL;
    pad=mk8_rumble_open(0,id);
    CHECK(pad && mk8_rumble_pads[0].hd && live_halves==2);
    const float bands[2][4]={{.501433f,160,0,320},{0,160,.133774f,160}};
    send_batch(h,bands);
    for(int i=0;i<2;i++) expect_raw(i,lo,hi);
    defer_output=true;
    Uint64 revoke_at=SDL_GetTicks();
    permission(false); permission(true);
    for(int i=0;i<2;i++) expect_raw(i,neutral,neutral);
    CHECK(SDL_GetTicks()-revoke_at<200);
    mk8_rumble_pump();
    defer_output=false;
    send_batch(h,bands);
    for(int i=0;i<2;i++) expect_raw(i,lo,hi);
    mk8_rumble_focus(false);
    for(int i=0;i<2;i++) expect_raw(i,neutral,neutral);
    mk8_rumble_focus(true);
    /* First active report succeeds on left only; it must receive neutral before basic fallback. */
    rejected_side=1;
    send_batch(h,bands);
    Uint64 until=SDL_GetTicks()+1000;
    while(mk8_rumble_pads[0].hd && SDL_GetTicks()<until) {present_and_pump();SDL_Delay(3);}
    expect_generic_pulse();
    CHECK(!mk8_rumble_pads[0].hd && !live_halves);
    expect_raw(0,neutral,neutral);
    mk8_rumble_close(0,pad);
    wait_pair_closed();
    /* Close immediately after an active report; right rejects the first permitted neutral. */
    rejected_side=-1;
    pad=mk8_rumble_open(0,id);
    send_batch(h,bands);
    for(int i=0;i<2;i++) expect_raw(i,lo,hi);
    int before_close[2]={raw_writes[0],raw_writes[1]};
    reject_next_right=true;
    mk8_rumble_close(0,pad);
    wait_pair_closed();
    CHECK(!reject_next_right && raw_writes[0]==before_close[0]+2 && raw_writes[1]==before_close[1]+1);
    CHECK(raw_at[1]-previous_at[1]>=30);
    for(int i=0;i<2;i++) expect_raw(i,neutral,neutral);
    pad=mk8_rumble_open(0,id);
    int idle_writes=raw_writes[0]+raw_writes[1];
    mk8_rumble_close(0,pad);
    wait_pair_closed();
    CHECK(raw_writes[0]+raw_writes[1]==idle_writes);
    pad=mk8_rumble_open(0,id);
    entered_write=SDL_CreateSemaphore(0);release_write=SDL_CreateSemaphore(0);polled=SDL_CreateSemaphore(0);
    block_next_left=true;
    SDL_Thread *sender=SDL_CreateThread(submit_tone,"submit",&h[0]);
    CHECK(SDL_WaitSemaphoreTimeout(entered_write,1000));
    SDL_Thread *poller=SDL_CreateThread(poll_and_submit,"unrelated-poll",&h[0]);
    bool responsive=SDL_WaitSemaphoreTimeout(polled,200);
    int previous_right=raw_active[1];
    if(responsive) {
        mk8_rumble_close(0,pad);
        pad=mk8_rumble_open(0,id);
        CHECK(pad && mk8_rumble_pads[0].waiting_for_pair && live_halves==2);
        send_tone(h[0],.25f,160,0,320);
        mk8_rumble_close(0,pad);
        pad=mk8_rumble_open(0,id);
        CHECK(pad && mk8_rumble_pads[0].waiting_for_pair);
    }
    SDL_SignalSemaphore(release_write);
    SDL_WaitThread(poller,NULL);SDL_WaitThread(sender,NULL);
    CHECK(responsive && "HID write held joystick polling or a subsequent game submission");
    wait_pair_closed();
    CHECK(raw_active[1]==previous_right);
    for(int i=0;i<2;i++) expect_raw(i,neutral,neutral);
    /* A newly attached pair must finish discovery even without a new vibration request. */
    present_and_pump();
    CHECK(mk8_rumble_pads[0].pair && !mk8_rumble_pads[0].waiting_for_pair && live_halves==2);
    /* Without another presented frame, the queued tone expires while the first half is blocked. */
    int before_expiry=raw_writes[0];
    block_next_left=true;
    send_tone(h[0],.3f,160,0,320);
    CHECK(SDL_WaitSemaphoreTimeout(entered_write,1000));
    SDL_Delay(275);
    SDL_SignalSemaphore(release_write);
    until=SDL_GetTicks()+1000;
    while(raw_writes[0]<before_expiry+2 && SDL_GetTicks()<until) SDL_Delay(2);
    CHECK(raw_writes[0]>=before_expiry+2 && raw_active[1]==previous_right);
    expect_raw(0,neutral,neutral);
    mk8_rumble_close(0,pad);
    wait_pair_closed();
    /* Drive the production window-exit handler while a radio is stalled. */
    pad=mk8_rumble_open(0,id);
    block_next_left=true;
    send_tone(h[0],.3f,160,0,320);
    CHECK(SDL_WaitSemaphoreTimeout(entered_write,1000));
    previous_right=raw_active[1];
    Uint64 exit_start=SDL_GetTicks();
    quick_exit_handler();
    CHECK(fake_exit_called && SDL_GetTicks()-exit_start<450);
    send_tone(h[0],.5f,240,0,320);
    SDL_SignalSemaphore(release_write);
    wait_pair_closed();
    CHECK(raw_active[1]==previous_right);
    for(int i=0;i<2;i++) expect_raw(i,neutral,neutral);
    mk8_rumble_close(0,pad);
    SDL_DestroySemaphore(polled);SDL_DestroySemaphore(release_write);SDL_DestroySemaphore(entered_write);
    SDL_DestroyMutex(raw_lock);
    CHECK(!live_halves && SDL_DetachVirtualJoystick(id));
    memset(pulse,0,sizeof pulse);
    rejected_side=-1;virtual_hidapi_id=0;switch_product=0x2009;pad=NULL;
}
int main(void) {
    guest=VirtualAlloc((void *)(GUEST_LO+0x100000),0x20000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(guest==(void *)(GUEST_LO+0x100000));
    CHECK(SDL_Init(SDL_INIT_GAMEPAD));
    hd_output();
    motion_paces_hd_output();
    joycon_output();
    mk8_output_exiting=false;  /* The exit-handler test returns to this fixture. */
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
    defer_output=true;
    send_one(p1[0],.8f,.9f);
    defer_output=false;
    mk8_rumble_set_enabled(false);
    mk8_rumble_pump();
    CHECK(!pulse[0].low && !pulse[0].high && !pulse[1].low && !pulse[1].high);
    unsigned muted=rumble_requests;
    send_one(p1[0],.4f,.7f);
    send_pair(p2,.2f,.6f);
    CHECK(rumble_requests==muted);
    readback(p1[0],0,0);
    readback(p2[0],0,0);
    mk8_rumble_set_enabled(true);
    mk8_rumble_pump();
    CHECK(rumble_requests==muted);
    send_one(p1[0],.4f,.7f);
    send_pair(p2,.2f,.6f);
    CHECK(pulse[0].low && pulse[0].high && pulse[1].low && pulse[1].high);
    unsigned second=pulse[1].calls;
    send_one(p1[0],0,0);
    CHECK(!pulse[0].low && !pulse[0].high && pulse[1].calls==second);
    send_pair(p2,0,0);
    CHECK(!pulse[1].low && !pulse[1].high);
    check_idle_frame();
    check_four_player_contention();
    check_output_lifetime(p1[0]);
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
    present_and_pump();
    CHECK(!pulse[0].low && !pulse[0].high && !pulse[1].low && !pulse[1].high);
    send_pair(p1,.8f,.8f);
    CHECK(!pulse[0].low && !pulse[0].high);
    readback(p1[0],0,0);
    readback(p1[1],0,0);
    menu_open=false;
    present_and_pump();
    CHECK(!pulse[0].low && !pulse[0].high);
    send_pair(p1,.3f,.4f);
    native_settings_open=true;
    present_and_pump();
    CHECK(!pulse[0].low && !pulse[0].high);
    send_pair(p1,.8f,.8f);
    readback(p1[0],0,0);
    readback(p1[1],0,0);
    native_settings_open=false;
    present_and_pump();
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
        present_and_pump();
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
    present_and_pump();
    CHECK(rumble_requests==renewals);
    clock_ms=1125;
    present_and_pump();
    CHECK(rumble_requests==renewals+1 && last_duration==250);
    clock_ms=1250;
    present_and_pump();
    CHECK(rumble_requests==renewals+2 && last_duration==250);
    clock_ms=1500;
    present_and_pump();
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
            start=text.index('    if (e->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE)')
            end=text.index('    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST',start)
            dispatch=text[start:end]
            service=(runtime/'services/hid_set.c').read_text(encoding='utf-8')
            start=service.index('static uint32_t new_event_handle(void)')
            end=service.index('SERVICE("hid:sys"',start)
            source=PREFIX+PAIR_FIXTURE+'''\n#define SDL_GetTicks test_ticks
#define pthread_cond_timedwait wait_for_space
#define SDL_RumbleGamepad test_rumble
#define SDL_GetJoystickGUIDForID fixture_guid
'''+vibration
            source+='''\n#undef SDL_GetTicks
#undef pthread_cond_timedwait
#undef SDL_RumbleGamepad
#undef SDL_GetJoystickGUIDForID
'''+service[start:end]+'''
static void mk8_sensor_event(const SDL_GamepadSensorEvent *e) { (void)e; }
static void dispatch_sensor(const SDL_Event *e) {
'''+dispatch+'}\n'
            device=(runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            exit_start=device.index('static void quick_exit_handler(void) {')
            source+='''
#undef LOG
#define LOG(...) ((void)0)
static struct {void *dev;} g_vk;
static bool fake_exit_called;
void plat_terminate(void) { fake_exit_called=true; }
static void device_wait_idle(void) { abort(); }
static void vk_pipeline_cache_save(void) { abort(); }
'''+device[exit_start:device.index('\n}',exit_start)+2]+'\n'+CHECKS
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
