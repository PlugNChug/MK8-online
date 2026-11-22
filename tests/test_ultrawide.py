"""Authored camera objects exercise the opt-in runtime and the bounded build adapter."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER, compiler_env
import ultrawide
from host import prepare_host_sources

RT_H = '''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define MAIN_BASE 0x7000000000ull
#define GUEST_LO MAIN_BASE
#define GUEST_HI (MAIN_BASE + 0x320000000ull)
uint64_t plat_now_ns(void);
'''
SETTINGS_H = '''enum { ASPECT_UNLOCKED, ASPECT_ORIGINAL, ASPECT_ULTRAWIDE };
#define TEST_CAMERA_SETTINGS
int mk8_settings_camera_distance(void);
enum { FRAMEGEN_OFF, FRAMEGEN_120, FRAMEGEN_REPLAY };
typedef struct { int aspect, frame_gen; } Settings;
extern Settings g_settings;
'''
HARNESS = r'''
#include "rt.h"
#include "settings.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#undef K32QueryWorkingSetEx
BOOL WINAPI K32QueryWorkingSetEx(HANDLE, PVOID, DWORD);
#ifdef NDEBUG
#error Camera checks require assertions
#endif
// Failed regressions must exit, rather than wait for Windows crash reporting.
#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed: %s, line %d\n", #condition, __LINE__); \
        exit(1); \
    } \
} while (0)
Settings g_settings;
static int camera_percent=100;
#ifdef TEST_CAMERA_SETTINGS
int mk8_settings_camera_distance(void) { return camera_percent; }
#endif
static uint64_t test_now=1000000;
uint64_t plat_now_ns(void) { return test_now; }
float mk8_driver_camera_distance(uint64_t owner, float length);
static bool hud_ready = true, hud_intro, hud_fade;
static bool settings_active;
bool mk8_settings_page_active(void) { return settings_active; }
bool mk8_ultrawide_hud_ready(bool intro) { return hud_ready && intro == hud_intro; }
bool mk8_ultrawide_hud_handoff_ready(void) { return hud_ready && !hud_intro && hud_fade; }
void mk8_ultrawide_camera(uint64_t);
double mk8_ultrawide_aspect(void);
double mk8_ultrawide_present_mode(void);
double mk8_ultrawide_take_present(void);
void mk8_ultrawide_present(double);
void mk8_ultrawide_drawable(int, int);
static bool wide_active(void) { return mk8_ultrawide_aspect()>16.0/9.0; }
#define ARENA 0x7240000000ull
#define SIZE 0x20000
static unsigned char *arena, *slot, before[SIZE];
static unsigned ws_mode, ws_calls;
BOOL WINAPI mk8_test_working_set(HANDLE process, PVOID buffer, DWORD size) {
    ws_calls++;
    if (ws_mode == 1) return FALSE;
    if (ws_mode == 2) {
        PSAPI_WORKING_SET_EX_INFORMATION *pages=buffer;
        for (unsigned i=0;i<size/sizeof *pages;i++) pages[i].VirtualAttributes.Flags=0;
        return TRUE;
    }
    return K32QueryWorkingSetEx(process, buffer, size);
}
static unsigned M=0x3000;
static const unsigned R=0, C=0x1000, E=0x2000, L=0x5000, V=0x6000,
    N=0x7000, S=0x8000, F=0x9000, Q=0xa000, Y=0xb000, W=0xc000,
    G=0xd000, K=0xe000, O=0x10000, A=0x11000, T=0x12000;
static void w64(unsigned p, uint64_t v) { memcpy(arena+p, &v, 8); }
static void w32(unsigned p, uint32_t v) { memcpy(arena+p, &v, 4); }
static void wf(unsigned p, float v) { memcpy(arena+p, &v, 4); }
static float rf(unsigned p) { float v; memcpy(&v, arena+p, 4); return v; }
static void link(unsigned p, unsigned target) { w64(p, ARENA+target); }
static void run(void) { mk8_ultrawide_camera(ARENA+M); }
static void setup(void) {
    hud_intro=false; hud_fade=false;
    mk8_ultrawide_drawable(1680,720);
    memset(arena, 0, SIZE);
    uint64_t root=ARENA+R; memcpy(slot, &root, 8);
    link(R+0x28,C); link(C+0x1a0,E); link(E+0x420,M);
    w32(M+0x120,1); link(M+0x128,L); link(L,V); link(V+0x70,M+0x238);
    w64(M+0x238, MAIN_BASE+0x12b0050);
    wf(V+0x38,1280); wf(V+0x3c,720);
    unsigned p=M+0x238;
    for (unsigned i=0xc; i<0x98; i++) arena[p+i]=(unsigned char)(i*7);
    wf(p+0x98,1); wf(p+0x9c,100000); wf(p+0xa0,0.977384388f);
    wf(p+0xa4,sinf(rf(p+0xa0)*.5f)); wf(p+0xa8,cosf(rf(p+0xa0)*.5f));
    wf(p+0xac,tanf(rf(p+0xa0)*.5f)); wf(p+0xb0,16.f/9.f);
    arena[M+0x9d4]=1;
    link(R+0x20,N); link(N+8,S); w32(S+0x180,4); w32(S+0x184,4); w32(S+0x17c,3);
    link(C+0x240,F); link(F+0x10,Q); w32(Q+8,2); w32(Q+0x180,1); w32(Q+0x188,1);
    link(C+0x190,Y); link(Y+0x7f8,W); w32(W+0xd8,3); w32(W+0x40,1);
    for (unsigned i=1;i<4;i++) w32(W+0xb0+4*i,UINT32_MAX);
    link(S+0x1c0,G); link(G+0x1f8,K); w64(K,MAIN_BASE+0x11b88e0);
    link(K+0x60,M+0x958); link(K+0x68,M+0x978);
    link(M+0x958,K+0x5b0); link(M+0x978,K+0x730); link(V+0x68,K+0x5b0);
    w32(K+0x80,1); link(K+0x78,T+0x10); link(T,O+8); link(K+0x3d0,O+8);
    w64(O,MAIN_BASE+0x11b9c30); w64(O+8,MAIN_BASE+0x11b9c50); w32(O+0x10,1);
    link(O+0xb0,A);
    g_settings.aspect=ASPECT_UNLOCKED; g_settings.frame_gen=FRAMEGEN_OFF;
}
static void unchanged(void) {
    memcpy(before,arena,SIZE); run(); assert(!memcmp(before,arena,SIZE));
}
static void only_aspect(float target) {
    memcpy(before,arena,SIZE); run();
    memcpy(before+M+0x238+0xb0,&target,4); before[M+0x238+8]=1;
    assert(!memcmp(before,arena,SIZE));
}
static void rear_setup(unsigned kind) {
    setup(); w32(Q+8,kind); w32(Q+0x180,kind==2 ? 2 : 12);
    link(K+0x3d8,O+0x40); link(K+0x3e8,O+0xca);
    w64(O+0x40,MAIN_BASE+0x11b9c50); w32(O+0x48,1); arena[O+0xca]=1;
    link(T,O+0x40);
}
static void check_course_depth(void) {
    const float clips[][2]={{4,50000},{1,100000},{2,10000}};
    const unsigned kinds[]={1,0,2};
    for (unsigned c=0;c<sizeof clips/sizeof *clips;c++)
        for (unsigned k=0;k<sizeof kinds/sizeof *kinds;k++)
            for (unsigned rear=0;rear<2;rear++) {
                rear_setup(kinds[k]); if (!rear) link(T,O+8);
                g_settings.aspect=ASPECT_ULTRAWIDE;
                mk8_ultrawide_drawable(1827,720);
                wf(M+0x2d0,clips[c][0]); wf(M+0x2d4,clips[c][1]);
                // The real VS course differs only in clip depth; every other guest byte must survive.
                only_aspect((float)(1827.0/720.0)); assert(wide_active());
                assert(mk8_ultrawide_take_present()==1827.0/720.0);
                arena[M+0x240]=0; unchanged(); assert(wide_active());
                g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);
                assert(!wide_active()); unchanged();
            }
    const float invalid[][2]={
        {NAN,50000},{4,NAN},{INFINITY,50000},{4,INFINITY},
        {-INFINITY,50000},{4,-INFINITY},{0,50000},{-1,50000},
        {4,0},{4,-1},{4,4},{4,1},{1,FLT_MAX},
        {FLT_MAX/4,FLT_MAX/2},{FLT_TRUE_MIN,1},{FLT_TRUE_MIN,2*FLT_TRUE_MIN},{FLT_MIN,2*FLT_MIN}
    };
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++) {
        rear_setup(1); link(T,O+8); g_settings.aspect=ASPECT_ULTRAWIDE;
        wf(M+0x2d0,invalid[i][0]); wf(M+0x2d4,invalid[i][1]);
        unchanged(); assert(!wide_active());
        wf(M+0x2d0,4); wf(M+0x2d4,50000); only_aspect(21.f/9.f);
        wf(M+0x2d0,invalid[i][0]); wf(M+0x2d4,invalid[i][1]);
        only_aspect(16.f/9.f); assert(!wide_active()); unchanged();
        wf(M+0x2d0,4); wf(M+0x2d4,50000); only_aspect(21.f/9.f);
        g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);
    }
}
static void check_rear(void) {
    for (unsigned kind=0;kind<=2;kind++) {
        rear_setup(kind);
        // Primary/rear identity does not depend on the intro registration metadata.
        w32(T+8,20); w32(T+12,2);
        only_aspect(21.f/9.f); assert(wide_active());
        for (unsigned i=0;i<100;i++) {
            arena[M+0x240]=0;
            link(T,O+8); unchanged(); assert(wide_active());
            link(T,O+0x40); unchanged(); assert(wide_active());
        }
        arena[S+0x234]=1; only_aspect(16.f/9.f); assert(!wide_active());
        unchanged(); arena[S+0x234]=0; only_aspect(21.f/9.f);
        assert(wide_active());
        arena[O+0xca]=0; only_aspect(16.f/9.f); assert(!wide_active());
        unchanged(); arena[O+0xca]=1; only_aspect(21.f/9.f);
        // Existing primary acceptance does not depend on the alternate's flag or registration.
        link(T,O+8); w64(K+0x3d8,0); w64(K+0x3e8,0); arena[O+0xca]=0;
        arena[M+0x240]=0; unchanged(); assert(wide_active());
        g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);
        rear_setup(kind); link(K+0x78,T+4+0x10); link(T+4,O+0x40);
        only_aspect(21.f/9.f); assert(wide_active());

        const struct { unsigned at; uint32_t value; } guards[] = {
            {O+0xca,0},{O+0xca,2},{O+0xca,255},{O+0x10,2},{O+0x48,0},{O+0x48,2},
            {A+0x40,1},{K+0x80,0},{K+0x80,17},{Q+0x188,2},{W+0xb0,1}
        };
        for (unsigned i=0;i<sizeof guards/sizeof *guards;i++) {
            rear_setup(kind); w32(guards[i].at,guards[i].value);
            unchanged(); assert(!wide_active());
        }
        const unsigned pointers[] = {K+0x3d0,K+0x3d8,K+0x3e8,T,O,O+8,O+0x40,O+0xb0};
        for (unsigned i=0;i<sizeof pointers/sizeof *pointers;i++) {
            rear_setup(kind); w64(pointers[i],0); unchanged(); assert(!wide_active());
            rear_setup(kind); w64(pointers[i],GUEST_HI-8); unchanged(); assert(!wide_active());
        }
        // A readable true flag at a different address is not the registered owner's flag.
        rear_setup(kind); link(K+0x3e8,O+0xcb); arena[O+0xcb]=1;
        unchanged(); assert(!wide_active());
        // Neither another valid-looking parameter nor the third embedded parameter is rear view.
        rear_setup(kind); link(K+0x3d8,T+0x40); link(T,T+0x40);
        w64(T+0x40,MAIN_BASE+0x11b9c50); w32(T+0x48,1);
        unchanged(); assert(!wide_active());
        rear_setup(kind); memcpy(arena+0x13000,arena+O,0xcc); link(K+0x3d0,0x13008);
        unchanged(); assert(!wide_active());  // same vtables, but another primary owner
        rear_setup(kind); link(K+0x3e0,O+0x78); link(T,O+0x78);
        w64(O+0x78,MAIN_BASE+0x11b9c50); w32(O+0x80,1);
        unchanged(); assert(!wide_active());
        link(K+0x3d8,O+0x78); unchanged(); assert(!wide_active());

        // The owner/flag may cross a page; read-only is valid, inaccessible bytes must reject.
        rear_setup(kind);
        const unsigned crossing=0x18f40;
        memcpy(arena+crossing,arena+O,0xcc);
        link(K+0x3d0,crossing+8); link(K+0x3d8,crossing+0x40);
        link(K+0x3e8,crossing+0xca); link(T,crossing+0x40);
        DWORD old; assert(VirtualProtect(arena+0x19000,0x1000,PAGE_READONLY,&old));
        only_aspect(21.f/9.f); assert(wide_active());
        g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);
        g_settings.aspect=ASPECT_UNLOCKED;
        memcpy(before,arena,SIZE);
        assert(VirtualProtect(arena+0x19000,0x1000,PAGE_NOACCESS,&old)); run();
        assert(VirtualProtect(arena+0x19000,0x1000,PAGE_READWRITE,&old));
        assert(!memcmp(before,arena,SIZE) && !wide_active());
        assert(VirtualProtect(arena+0x19000,0x1000,PAGE_READWRITE|PAGE_GUARD,&old)); run();
        MEMORY_BASIC_INFORMATION info;
        assert(VirtualQuery(arena+0x19000,&info,sizeof info) && (info.Protect&PAGE_GUARD));
        assert(VirtualProtect(arena+0x19000,0x1000,PAGE_READWRITE,&old));
        assert(!memcmp(before,arena,SIZE) && !wide_active());
        // Primary view still needs only the original owner span, not the alternate flag page.
        link(T,crossing+8); memcpy(before,arena,SIZE);
        assert(VirtualProtect(arena+0x19000,0x1000,PAGE_NOACCESS,&old)); run();
        assert(VirtualProtect(arena+0x19000,0x1000,PAGE_READWRITE,&old));
        float wide=21.f/9.f; memcpy(before+M+0x2e8,&wide,4); before[M+0x240]=1;
        assert(!memcmp(before,arena,SIZE) && wide_active());
    }
}

static void check_teams(void) {
    const unsigned flags[] = {1,2,255};
    for (unsigned kind=0;kind<=2;kind++)
        for (unsigned rear=0;rear<2;rear++) {
            rear_setup(kind);
            if (!rear) link(T,O+8);
            only_aspect(21.f/9.f); assert(wide_active());
            for (unsigned i=0;i<sizeof flags/sizeof *flags;i++) {
                // Team VS shares the rules kind; its byte is independent of racer count.
                arena[Q+0x27]=(unsigned char)flags[i];
                if (kind==1) {
                    only_aspect(16.f/9.f); assert(!wide_active());
                    unchanged();
                } else {
                    arena[M+0x240]=0; unchanged(); assert(wide_active());
                }
                arena[Q+0x27]=0;
                if (kind==1) only_aspect(21.f/9.f); else unchanged();
                assert(wide_active());
                arena[M+0x240]=0; unchanged();
            }
        }
}

// Flyover and close-up share this external owner, with separately registered subcameras.
static const unsigned IE=0x13000, IR=0x14000, IO=0x15000, IP=0x16000,
    IT=0x17000, IS=0x18000, IA=0x14100;
static void intro_setup_at(unsigned kind, unsigned mode, unsigned owner, unsigned source,
                           unsigned table, unsigned subcamera) {
    setup(); hud_intro=mode==2; w32(Q+8,kind); w32(Q+0x180,kind==2 ? 1 : 12);
    w32(W+0xd8,mode==2 ? 4 : 3); w32(W+0x44,mode==2 ? 2 : 1);
    link(S+0x1b0,IE); link(IE+0x228,IR); w64(IR,MAIN_BASE+0x11b8db0);
    w32(IR+0x38,4); w32(IR+0x3c,4); link(IR+0x40,IA); link(IA,owner);
    w64(owner,MAIN_BASE+0x11b8d50); w32(owner+8,0);
    w64(owner+0x10,MAIN_BASE+0x12c7778); w32(owner+0x18,0);
    link(owner+0x20,owner+0x30); link(owner+0x28,source);
    w64(owner+0x30,MAIN_BASE+0x12afde8);
    w32(owner+0xd8,9); link(owner+0xe0,table);
    w32(owner+0xe8,mode); w32(owner+0xec,mode);
    link(table+mode*8,subcamera); link(owner+(mode==2 ? 0x120 : 0x130),subcamera);
    w64(subcamera,MAIN_BASE+(mode==2 ? 0x11b8ed8 : 0x12c7e68)); link(subcamera+8,owner);
    memcpy(arena+source,arena+M+0x238,0xc0); wf(source+0xb0,4.f/3.f);
    link(M+0x958,owner+0x30); link(M+0x978,source); link(V+0x68,owner+0x30);
    w32(K+0x80,2); link(T,owner+0x10); w32(T+8,20); w32(T+12,2);
    arena[M+0x9d7]=mode==2 || kind==2;
}
static void intro_setup(unsigned kind, unsigned mode) { intro_setup_at(kind,mode,IO,IP,IT,IS); }
static void check_distance(void) {
    const uint64_t owner=ARENA+O;
    for (unsigned kind=0;kind<3;kind++) {
        for (int aspect=ASPECT_ORIGINAL;aspect<=ASPECT_ULTRAWIDE;aspect++) {
            camera_percent=117;
            intro_setup(kind,3); g_settings.aspect=aspect; run();
            assert(mk8_driver_camera_distance(owner,100.f)==100.f);
            rear_setup(kind); link(T,O+8); g_settings.aspect=aspect; run();
            assert(mk8_driver_camera_distance(owner,100.f)==100.f);
            float last=100.f;
            for (int frame=0;frame<60;frame++) {
                test_now+=16666667; run();
                float next=mk8_driver_camera_distance(owner,100.f);
                assert(next>=last && next-last<5.f); last=next;
            }
            assert(fabsf(last-117.f)<.001f);
            assert(mk8_driver_camera_distance(owner+8,100.f)==100.f);
            link(T,O+0x40); run(); assert(fabsf(mk8_driver_camera_distance(owner,100.f)-117.f)<.001f);
            camera_percent=75;
            for (int i=0;i<65;i++) { test_now+=16666667; run(); }
            assert(mk8_driver_camera_distance(owner,100.f)==75.f);
            assert(mk8_driver_camera_distance(owner,-1.f)==-1.f);
            assert(isnan(mk8_driver_camera_distance(owner,NAN)));
            assert(isinf(mk8_driver_camera_distance(owner,INFINITY)));
            camera_percent=150;
            for (int i=0;i<40;i++) { test_now+=16666667; run(); }
            assert(mk8_driver_camera_distance(owner,100.f)==150.f);
            assert(mk8_driver_camera_distance(owner,FLT_MAX)==FLT_MAX);
            w32(W+0x40+0x18,1); run(); assert(mk8_driver_camera_distance(owner,100.f)==100.f);
            w32(W+0x40+0x18,0); run(); assert(mk8_driver_camera_distance(owner,100.f)==100.f);
            test_now+=16666667; run(); assert(mk8_driver_camera_distance(owner,100.f)>100.f);
            mk8_ultrawide_camera(UINT64_MAX); assert(mk8_driver_camera_distance(owner,100.f)==100.f);
            camera_percent=100; setup(); run(); assert(mk8_driver_camera_distance(owner,100.f)==100.f);
        }
    }
}
static void check_intro_case(unsigned kind, unsigned mode) {
    unsigned registration=IO+(mode==2 ? 0x120 : 0x130);
    intro_setup(kind,mode);
    only_aspect(21.f/9.f); assert(wide_active());
    assert(rf(IP+0xb0)==4.f/3.f);  // only the final render projection is widened
    mk8_ultrawide_drawable(2560,720); only_aspect(32.f/9.f); assert(wide_active());
    mk8_ultrawide_drawable(1680,720); only_aspect(21.f/9.f);
    arena[M+0x240]=0; unchanged(); assert(wide_active());
    w32(IO+0xdc,UINT32_MAX); unchanged(); assert(wide_active());  // the adjacent raw DWORD is not a capacity
    hud_intro=mode!=2; only_aspect(16.f/9.f); assert(!wide_active());
    unchanged(); hud_intro=mode==2; only_aspect(21.f/9.f);
    // A sampled fade or unsupported page makes the matching HUD class unready.
    hud_ready=false; only_aspect(16.f/9.f); assert(!wide_active());
    hud_ready=true; only_aspect(21.f/9.f);
    settings_active=true; only_aspect(16.f/9.f); assert(!wide_active());
    settings_active=false; only_aspect(21.f/9.f);
    // Vulkan frame generation interpolates the placed image: the wide camera stays (Metal's replay keeps 16:9).
    g_settings.frame_gen=FRAMEGEN_120; unchanged(); assert(wide_active());
    g_settings.frame_gen=FRAMEGEN_REPLAY; unchanged(); assert(wide_active());
    g_settings.frame_gen=FRAMEGEN_OFF; unchanged(); assert(wide_active());
    w32(IO+0xe8,1); only_aspect(16.f/9.f); assert(!wide_active());
    unchanged(); w32(IO+0xe8,mode); only_aspect(21.f/9.f);
    const unsigned teams[] = {1,2,255};
    for (unsigned i=0;i<sizeof teams/sizeof *teams;i++) {
        arena[Q+0x27]=(unsigned char)teams[i];
        if (kind==1) { only_aspect(16.f/9.f); assert(!wide_active()); unchanged(); }
        else { arena[M+0x240]=0; unchanged(); assert(wide_active()); }
        arena[Q+0x27]=0;
        if (kind==1) only_aspect(21.f/9.f); else unchanged();
        assert(wide_active());
    }
    w32(S+0x184,3); only_aspect(16.f/9.f); assert(!wide_active());
    unchanged(); w32(S+0x184,4); only_aspect(21.f/9.f);
    // Returning to the primary camera keeps the current final projection and producer decision.
    link(T,O+8); link(M+0x958,K+0x5b0); link(M+0x978,K+0x730); link(V+0x68,K+0x5b0);
    w32(K+0x80,1); w32(T+8,10); w32(T+12,1); hud_intro=false;
    w32(W+0xd8,3); w32(W+0x44,0);
    arena[M+0x240]=0; unchanged(); assert(wide_active());
    assert(rf(IP+0xb0)==4.f/3.f);
    g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);

    const struct { unsigned at; uint32_t value; } guards[] = {
        {IR+0x38,0},{IR+0x38,3},{IR+0x38,5},{IR+0x3c,0},{IR+0x3c,3},{IR+0x3c,5},
        {IO+8,1},{IO+0x18,1},{IO+0x18,2},{IO+0xd8,0},{IO+0xd8,8},{IO+0xd8,10},
        {IO+0xe8,0},{IO+0xe8,1},{IO+0xe8,4},{IO+0xe8,8},{IO+0xe8,UINT32_MAX},
        {IO+0xec,0},{IO+0xec,1},{IO+0xec,mode==2 ? 3 : 2},{IO+0xec,4},
        {T+8,10},{T+8,30},{T+12,1},{T+12,3},{Q+8,3},{Q+8,UINT32_MAX},{Q+0x188,2},
        {W+0xd8,mode==2 ? 3 : 4},{W+0xd8,5},{W+0x44,0},{W+0x44,mode==2 ? 1 : 2},{W+0x48,1},
        {W+0xb4,0},{W+0x58,1},{M+0x9bc,1},{M+0x9d7,mode==2 || kind==2 ? 0 : 1},{M+0x9d7,2}
    };
    for (unsigned i=0;i<sizeof guards/sizeof *guards;i++) {
        intro_setup(kind,mode); w32(guards[i].at,guards[i].value); unchanged(); assert(!wide_active());
    }
    const unsigned pointers[] = {S+0x1b0,IE+0x228,IR,IR+0x40,IA,IO,IO+0x10,
        IO+0x20,IO+0x28,IO+0x30,IO+0xe0,registration,IT+mode*8,IS,IS+8,IP,
        M+0x958,M+0x978,V+0x68};
    for (unsigned i=0;i<sizeof pointers/sizeof *pointers;i++) {
        intro_setup(kind,mode); w64(pointers[i],0); unchanged(); assert(!wide_active());
        intro_setup(kind,mode); w64(pointers[i],GUEST_HI-8); unchanged(); assert(!wide_active());
    }
    const float source_aspects[] = {16.f/9.f,21.f/9.f,0,NAN,INFINITY};
    for (unsigned i=0;i<sizeof source_aspects/sizeof *source_aspects;i++) {
        intro_setup(kind,mode); wf(IP+0xb0,source_aspects[i]); unchanged(); assert(!wide_active());
    }
    // A registered object in another slot is not viewport zero's owner.
    intro_setup(kind,mode); w64(IA,0); link(IA+8,IO); unchanged(); assert(!wide_active());
    // Identical vtables and a readable parameter do not establish registry ownership.
    intro_setup(kind,mode); memcpy(arena+0x1a000,arena+IO,0x190);
    link(0x1a020,0x1a030); link(T,0x1a010); link(IS+8,0x1a000);
    link(M+0x958,0x1a030); link(V+0x68,0x1a030);
    unchanged(); assert(!wide_active());
    // A different readable subcamera with the same vtable/backlink is not the registered table entry.
    intro_setup(kind,mode); memcpy(arena+IS+0x100,arena+IS,16); link(registration,IS+0x100);
    unchanged(); assert(!wide_active());
    // Registering the other known class at the same slot must not borrow this mode's eligibility.
    intro_setup(kind,mode); w64(IS,MAIN_BASE+(mode==2 ? 0x12c7e68 : 0x11b8ed8));
    unchanged(); assert(!wide_active());
    intro_setup(kind,mode); w32(IO+0xe8,mode==2 ? 3 : 2); w32(IO+0xec,mode==2 ? 3 : 2);
    unchanged(); assert(!wide_active());

    DWORD old;
    intro_setup(kind,mode);
    assert(VirtualProtect(arena+IP,0x1000,PAGE_READONLY,&old));
    only_aspect(21.f/9.f); assert(wide_active() && rf(IP+0xb0)==4.f/3.f);
    g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);
    assert(VirtualProtect(arena+IP,0x1000,PAGE_READWRITE,&old));
    // Each required object tail is checked across pages; read-only inputs are accepted.
    const struct { unsigned owner, source, table, subcamera, page; } crossing[] = {
        {mode==2 ? 0x1af80 : 0x1aed0,IP,IT,IS,0x1b000},
        {IO,0x1cf80,IT,IS,0x1d000},
        {IO,IP,0x1eff0,IS,0x1f000},
        {IO,IP,IT,0x1bff8,0x1c000}
    };
    for (unsigned i=0;i<sizeof crossing/sizeof *crossing;i++) {
        // Mode 3's +0x130 registration alone crosses the first case's page boundary.
        intro_setup_at(kind,mode,crossing[i].owner,crossing[i].source,crossing[i].table,crossing[i].subcamera);
        unsigned page=crossing[i].page;
        assert(VirtualProtect(arena+page,0x1000,PAGE_READONLY,&old));
        only_aspect(21.f/9.f); assert(wide_active());
        g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);
        g_settings.aspect=ASPECT_UNLOCKED;
        memcpy(before,arena,SIZE);
        assert(VirtualProtect(arena+page,0x1000,PAGE_NOACCESS,&old)); run();
        assert(VirtualProtect(arena+page,0x1000,PAGE_READWRITE,&old));
        assert(!memcmp(before,arena,SIZE) && !wide_active());
        assert(VirtualProtect(arena+page,0x1000,PAGE_READWRITE|PAGE_GUARD,&old)); run();
        MEMORY_BASIC_INFORMATION info;
        assert(VirtualQuery(arena+page,&info,sizeof info) && (info.Protect&PAGE_GUARD));
        assert(VirtualProtect(arena+page,0x1000,PAGE_READWRITE,&old));
        assert(!memcmp(before,arena,SIZE) && !wide_active());
    }
    // A race camera cannot borrow intro HUD readiness, including after a previously wide image.
    setup(); hud_intro=true; unchanged(); assert(!wide_active());
    hud_intro=false; only_aspect(21.f/9.f); assert(wide_active());
    hud_intro=true; only_aspect(16.f/9.f); assert(!wide_active());
}
static void check_intro(void) {
    for (unsigned kind=0;kind<=1;kind++)
        for (unsigned mode=2;mode<=3;mode++) check_intro_case(kind,mode);
    check_intro_case(2,3);
    // TT has the registered close-up with flag 1, but no verified flyover.
    intro_setup(2,2); unchanged(); assert(!wide_active());
    puts("flyover/close-up ownership, final-only projection, fallback and page guards passed");
}

static void handoff_setup(unsigned kind) {
    intro_setup(kind,2); w32(W+0xd4,3);
    hud_intro=false; hud_fade=true;
}
static void check_handoff(void) {
    for (unsigned kind=0;kind<=1;kind++) {
        const int widths[]={1680,2560};
        for (unsigned i=0;i<sizeof widths/sizeof *widths;i++) {
            double ratio=(double)widths[i]/720;
            handoff_setup(kind); mk8_ultrawide_drawable(widths[i],720);
            only_aspect((float)ratio); assert(wide_active() && rf(IP+0xb0)==4.f/3.f);
            arena[M+0x240]=0; unchanged(); assert(wide_active());
            assert(mk8_ultrawide_take_present()==ratio);
            assert(mk8_ultrawide_take_present()==16.0/9.0);
            unchanged(); assert(wide_active());
            // The race HUD alone cannot keep a flyover wide after the fade disappears.
            hud_fade=false; only_aspect(16.f/9.f); assert(!wide_active());
            unchanged(); hud_fade=true; only_aspect((float)ratio);
            hud_ready=false; only_aspect(16.f/9.f); assert(!wide_active());
            hud_ready=true; only_aspect((float)ratio);
            w32(W+0xd4,0); only_aspect(16.f/9.f); assert(!wide_active());
            unchanged(); w32(W+0xd4,3); only_aspect((float)ratio);
            // The normal close-up takes over only after all mode and viewport state agrees.
            w32(IO+0xe8,3); w32(IO+0xec,3); w32(W+0xd8,3); w32(W+0xd4,0);
            w32(W+0x44,1); arena[M+0x9d7]=0;
            link(IT+3*8,IS); link(IO+0x130,IS); w64(IS,MAIN_BASE+0x12c7e68);
            hud_fade=false; arena[M+0x240]=0; unchanged(); assert(wide_active());
        }
        const struct { unsigned at; uint32_t value; } guards[]={
            {W+0xd4,0},{W+0xd4,1},{W+0xd4,2},{W+0xd4,4},{W+0xd4,UINT32_MAX},
            {IO+0xec,3},{IO+0xe8,3},{W+0xd8,3},{W+0x44,1},{M+0x9d7,0},
            {Q+8,2},{Q+8,3},{Q+0x188,2},{IR+0x38,3}
        };
        for (unsigned i=0;i<sizeof guards/sizeof *guards;i++) {
            handoff_setup(kind); w32(guards[i].at,guards[i].value);
            unchanged(); assert(!wide_active());
        }
        handoff_setup(kind); w64(IA,0); unchanged(); assert(!wide_active());
        handoff_setup(kind); link(IO+0x120,IS+0x100); unchanged(); assert(!wide_active());
        handoff_setup(kind); hud_fade=false; unchanged(); assert(!wide_active());
        handoff_setup(kind); hud_ready=false; unchanged(); assert(!wide_active()); hud_ready=true;
        if (kind==1) {
            handoff_setup(kind); arena[Q+0x27]=1; unchanged(); assert(!wide_active());
        }
        // A pending request does not invalidate the still-active ordinary intro HUD.
        intro_setup(kind,2); w32(W+0xd4,3); only_aspect(21.f/9.f); assert(wide_active());
    }
    setup(); w32(W+0xd4,3); hud_intro=true; hud_fade=true;
    unchanged(); assert(!wide_active());
    puts("stable flyover handoff requires pending viewport three and validated race HUD with fade");
}

static void check_ratios(void) {
    const int sizes[][2]={{1280,720},{1680,720},{2560,1080},{3440,1440},{3840,1600},{3840,1080},{5120,1440}};
    for (unsigned kind=0;kind<=2;kind++) {
        setup(); w32(Q+8,kind); w32(Q+0x180,kind==2 ? 1 : 12);
        for (unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
            double ratio=(double)sizes[i][0]/sizes[i][1];
            double produced=mk8_ultrawide_aspect(), presented=mk8_ultrawide_present_mode();
            mk8_ultrawide_drawable(sizes[i][0],sizes[i][1]);
            assert(mk8_ultrawide_aspect()==produced && mk8_ultrawide_present_mode()==presented);
            if (i==0) unchanged(); else only_aspect((float)ratio);
            assert(mk8_ultrawide_aspect()==ratio);
            arena[M+0x240]=0; unchanged();  // no cumulative transform or redundant dirty bit
            double image=mk8_ultrawide_take_present();
            assert(image==ratio && mk8_ultrawide_aspect()==16.0/9.0);
            mk8_ultrawide_present(image);
            arena[S+0x234]=1;
            if (ratio>16.0/9.0) only_aspect(16.f/9.f); else unchanged();
            assert(mk8_ultrawide_aspect()==16.0/9.0 && mk8_ultrawide_present_mode()==image);
            arena[S+0x234]=0;
            if (ratio>16.0/9.0) only_aspect((float)ratio); else unchanged();
        }
        // Restore a previous non-21:9 write when the drawable disappears or leaves the supported range.
        const int invalid[][2]={{0,0},{1680,0},{0,720},{-1,720},{1680,-1},{1280,960},{5121,1440},{1,1},
                                {INT32_MAX,1},{1,INT32_MAX}};
        for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++) {
            mk8_ultrawide_drawable(3440,1440); only_aspect((float)(3440.0/1440.0));
            double produced=mk8_ultrawide_aspect();
            mk8_ultrawide_drawable(invalid[i][0],invalid[i][1]);
            assert(mk8_ultrawide_aspect()==produced);
            only_aspect(16.f/9.f); assert(mk8_ultrawide_aspect()==16.0/9.0);
            unchanged();
        }
        mk8_ultrawide_drawable(5120,1440); only_aspect(32.f/9.f);
        mk8_ultrawide_drawable(1280,720); only_aspect(16.f/9.f);
        assert(mk8_ultrawide_aspect()==16.0/9.0);
        arena[M+0x240]=0; unchanged();
    }
}
int main(int argc, char **argv) {
    assert(argc==2);
    arena=VirtualAlloc((void *)(uintptr_t)ARENA,SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(arena==(void *)(uintptr_t)ARENA);
    unsigned char *module=VirtualAlloc((void *)(uintptr_t)(MAIN_BASE+0x1350000),0x10000,
                                      MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(module==(void *)(uintptr_t)(MAIN_BASE+0x1350000));
    slot=module+0x8c70;
    setup();
    if (!strcmp(argv[1],"course-depth")) { check_course_depth(); return 0; }
    if (!strcmp(argv[1],"distance")) { check_distance(); return 0; }
    if (!strcmp(argv[1],"off")) {
        assert(mk8_ultrawide_aspect()==0 && mk8_ultrawide_present_mode()==0);
        assert(mk8_ultrawide_take_present()==0);
        unchanged(); mk8_ultrawide_camera(UINT64_MAX); assert(!ws_calls); return 0;
    }
    if (!strncmp(argv[1],"rear",4)) {
        ws_mode=!strcmp(argv[1],"rear-api-error") ? 1 : !strcmp(argv[1],"rear-nonresident") ? 2 : 0;
        check_rear(); return 0;
    }
    if (!strncmp(argv[1],"intro",5)) {
        ws_mode=!strcmp(argv[1],"intro-api-error") ? 1 : !strcmp(argv[1],"intro-nonresident") ? 2 : 0;
        check_intro(); return 0;
    }
    if (!strncmp(argv[1],"handoff",7)) {
        ws_mode=!strcmp(argv[1],"handoff-api-error") ? 1 : !strcmp(argv[1],"handoff-nonresident") ? 2 : 0;
        check_handoff(); return 0;
    }
    if (!strcmp(argv[1],"ratios")) { check_ratios(); return 0; }
    assert(!wide_active() && mk8_ultrawide_present_mode()==16.0/9.0);
    assert(mk8_ultrawide_take_present()==16.0/9.0);
    run(); assert(wide_active());
    double wide=mk8_ultrawide_take_present();
    assert(wide==21.0/9.0 && !wide_active());
    assert(mk8_ultrawide_take_present()==16.0/9.0);  // a new image needs a fresh camera decision
    mk8_ultrawide_present(wide); assert(mk8_ultrawide_present_mode()==21.0/9.0);
    w32(S+0x184,3); run();
    assert(!wide_active() && mk8_ultrawide_present_mode()==21.0/9.0); // producer cannot change the current image
    double fallback=mk8_ultrawide_take_present(); assert(fallback==16.0/9.0);
    w32(S+0x184,4); run(); assert(wide_active());
    mk8_ultrawide_present(fallback); assert(mk8_ultrawide_present_mode()==16.0/9.0); // queued image keeps its own decision
    assert(wide_active());
    g_settings.aspect=ASPECT_ORIGINAL; run();
    assert(!wide_active());
    for (unsigned kind=0;kind<=2;kind++) {
        setup(); w32(Q+8,kind); w32(Q+0x180,kind==2 ? 1 : 12);
        hud_ready=false; unchanged();
        hud_ready=true; only_aspect(21.f/9.f);
        settings_active=true; only_aspect(16.f/9.f); assert(!wide_active());
        settings_active=false; only_aspect(21.f/9.f);
        hud_ready=false; only_aspect(16.f/9.f);
        assert(!wide_active());
        hud_ready=true;
    }
    ws_mode=!strcmp(argv[1],"api-error") ? 1 : !strcmp(argv[1],"nonresident") ? 2 : 0;
    check_teams();
    setup(); w32(Q+8,0); w32(Q+0x180,12);  // Grand Prix: twelve racers, one local player
    only_aspect(21.f/9.f); assert(wide_active());
    arena[M+0x240]=0; unchanged();
    arena[S+0x234]=1; only_aspect(16.f/9.f); assert(!wide_active());
    arena[S+0x234]=0; only_aspect(21.f/9.f);
    w32(Q+8,3); only_aspect(16.f/9.f); assert(!wide_active());
    w32(Q+8,0); only_aspect(21.f/9.f);
    w32(Q+0x188,2); only_aspect(16.f/9.f); unchanged();
    setup();
    link(K+0x78,T+4+0x10); link(T+4,O+8); only_aspect(21.f/9.f);
    g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);
    setup();
    only_aspect(21.f/9.f);
    arena[M+0x240]=0; unchanged();  // repeated calls do not dirty an unchanged projection
    g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f); unchanged();
    g_settings.aspect=ASPECT_UNLOCKED; only_aspect(21.f/9.f);
    g_settings.frame_gen=FRAMEGEN_120; unchanged();
    g_settings.frame_gen=FRAMEGEN_REPLAY; unchanged();
    g_settings.frame_gen=FRAMEGEN_OFF; unchanged();
    w32(S+0x184,3); only_aspect(16.f/9.f); unchanged();  // menu fallback
    w32(S+0x184,4); only_aspect(21.f/9.f);
    arena[S+0x234]=1; only_aspect(16.f/9.f); unchanged();  // pause
    arena[S+0x234]=0; only_aspect(21.f/9.f);
    w32(Q+0x188,2); only_aspect(16.f/9.f); unchanged();  // split screen
    w32(Q+0x188,1); only_aspect(21.f/9.f);
    link(T,O+0x40); only_aspect(16.f/9.f); unchanged();  // another selected camera
    setup();
    // Each broken identity, scene, player and camera bound rejects before any guest write.
    const struct { unsigned at; uint32_t value; } guards[] = {
        {M+0x120,0},{M+0x120,17},{M+0x9c0,1},{M+0x9bc,1},{M+0x9d4,0},
        {S+0x180,0},{S+0x184,7},{S+0x17c,4},{S+0x238,1},{S+0x240,1},
        {Q+8,3},{Q+8,4},{Q+8,5},{Q+8,UINT32_MAX},
        {Q+0x188,0},{Q+0x188,2},{W+0xcc,1},{W+0xd8,4},
        {W+0xb0,1},{W+0xb4,0},{W+0xb8,0},{W+0xbc,0},{W+0x40,3},{W+0x4c,1},
        {W+0x58,1},{W+0x70,1},{W+0x88,1},{W+0xa0,1},
        {K+0x80,0},{K+0x80,17},{O+0x10,2},{A+0x40,1}
    };
    for (unsigned kind=0;kind<=2;kind++)
        for (unsigned i=0;i<sizeof guards/sizeof *guards;i++) {
            setup(); w32(Q+8,kind); w32(Q+0x180,kind==2 ? 1 : 12);
            w32(guards[i].at,guards[i].value); unchanged();
        }
    const unsigned pointers[] = {R+0x28,C+0x1a0,E+0x420,M+0x128,L,V+0x70,M+0x238,
        R+0x20,N+8,C+0x240,F+0x10,C+0x190,Y+0x7f8,S+0x1c0,G+0x1f8,K,
        K+0x60,K+0x68,M+0x958,M+0x978,V+0x68,K+0x78,K+0x3d0,T,O,O+8,O+0xb0};
    for (unsigned kind=0;kind<=2;kind++)
        for (unsigned i=0;i<sizeof pointers/sizeof *pointers;i++) {
            setup(); w32(Q+8,kind); w32(Q+0x180,kind==2 ? 1 : 12); w64(pointers[i],0); unchanged();
            setup(); w32(Q+8,kind); w32(Q+0x180,kind==2 ? 1 : 12); w64(pointers[i],GUEST_HI-8); unchanged();
        }
    const struct { unsigned at; float value; } floats[] = {
        {V+0x30,NAN},{V+0x34,INFINITY},{V+0x38,0},{V+0x3c,0},{V+0x38,3841},
        {V+0x3c,2161},{V+0x38,1279},{M+0x2d0,0},{M+0x2d4,0},
        {M+0x2d8,NAN},{M+0x2d8,0.49f},{M+0x2d8,1.61f},{M+0x2ec,1},{M+0x2f0,1},
        {M+0x2e8,NAN},{M+0x2e8,2},{M+0x2dc,NAN},{M+0x2e0,INFINITY},{M+0x2e4,0}
    };
    for (unsigned kind=0;kind<=2;kind++)
        for (unsigned i=0;i<sizeof floats/sizeof *floats;i++) {
            setup(); w32(Q+8,kind); w32(Q+0x180,kind==2 ? 1 : 12);
            wf(floats[i].at,floats[i].value); unchanged();
        }
    setup();
    DWORD old;
    assert(VirtualProtect(arena+M,0x1000,PAGE_READONLY,&old)); unchanged();
    assert(VirtualProtect(arena+M,0x1000,PAGE_READWRITE,&old));
    assert(VirtualProtect(arena+O,0x1000,PAGE_NOACCESS,&old));
    run();  // inaccessible objects fail closed without touching their bytes
    assert(VirtualProtect(arena+O,0x1000,PAGE_READWRITE,&old));
    assert(rf(M+0x2e8)==16.f/9.f);
    assert(VirtualProtect(arena+O,0x1000,PAGE_READWRITE|PAGE_GUARD,&old));
    run();
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery(arena+O,&info,sizeof info) && (info.Protect&PAGE_GUARD));
    assert(VirtualProtect(arena+O,0x1000,PAGE_READWRITE,&old));
    assert(rf(M+0x2e8)==16.f/9.f);
    // An owner spanning two pages requires readable protection on both, even if only its tail is blocked.
    setup();
    const unsigned crossing=0x18f80;
    memcpy(arena+crossing,arena+O,0xb8); link(K+0x3d0,crossing+8); link(T,crossing+8);
    only_aspect(21.f/9.f);
    g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);
    g_settings.aspect=ASPECT_UNLOCKED;
    assert(VirtualProtect(arena+0x19000,0x1000,PAGE_READONLY,&old));
    only_aspect(21.f/9.f);  // read-only input is sufficient; projection writes remain separately checked
    g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);
    g_settings.aspect=ASPECT_UNLOCKED;
    assert(VirtualProtect(arena+0x19000,0x1000,PAGE_NOACCESS,&old));
    run();
    assert(VirtualProtect(arena+0x19000,0x1000,PAGE_READWRITE,&old));
    assert(rf(M+0x2e8)==16.f/9.f);
    assert(VirtualProtect(arena+0x19000,0x1000,PAGE_READWRITE|PAGE_GUARD,&old));
    run();
    assert(VirtualQuery(arena+0x19000,&info,sizeof info) && (info.Protect&PAGE_GUARD));
    assert(VirtualProtect(arena+0x19000,0x1000,PAGE_READWRITE,&old));
    assert(rf(M+0x2e8)==16.f/9.f);
    assert(VirtualFree(arena+0x19000,0x1000,MEM_DECOMMIT));
    run();  // an uncommitted second page must also fail without a dereference
    assert(VirtualAlloc(arena+0x19000,0x1000,MEM_COMMIT,PAGE_READWRITE)==arena+0x19000);
    assert(rf(M+0x2e8)==16.f/9.f);
    M=0x3dc0; setup();  // the projection's dirty byte and aspect now lie in the second page
    only_aspect(21.f/9.f);
    g_settings.aspect=ASPECT_ORIGINAL; only_aspect(16.f/9.f);
    g_settings.aspect=ASPECT_UNLOCKED;
    assert(VirtualProtect(arena+0x4000,0x1000,PAGE_READONLY,&old)); unchanged();
    assert(VirtualProtect(arena+0x4000,0x1000,PAGE_NOACCESS,&old)); run();
    assert(VirtualProtect(arena+0x4000,0x1000,PAGE_READWRITE,&old));
    assert(rf(M+0x2e8)==16.f/9.f);
    assert(VirtualProtect(arena+0x4000,0x1000,PAGE_READWRITE|PAGE_GUARD,&old)); run();
    assert(VirtualQuery(arena+0x4000,&info,sizeof info) && (info.Protect&PAGE_GUARD));
    assert(VirtualProtect(arena+0x4000,0x1000,PAGE_READWRITE,&old));
    assert(rf(M+0x2e8)==16.f/9.f);
    M=0x3000;
    setup(); only_aspect(21.f/9.f);
    // If the old manager disappears, its pointer is never revisited by the next invocation.
    assert(VirtualProtect(arena+M,0x1000,PAGE_NOACCESS,&old));
    mk8_ultrawide_camera(ARENA+0x18000);
    assert(VirtualProtect(arena+M,0x1000,PAGE_READWRITE,&old));
    setup(); w32(S+0x184,3); wf(M+0x2e8,21.f/9.f);
    unchanged();  // a reused address with a wide aspect is not our old camera
    assert(ws_calls);
    puts("camera guards, field preservation and fallback passed");
    return 0;
}
'''


SAVED_SETTINGS = r'''
#undef main
#include "nvn/output_scale.h"
const char *const g_tunic_colours[N_TUNIC_COLOURS] = {0};
const char *const g_heart_colours[N_HEART_COLOURS] = {0};
void rt_log(const char *fmt, ...) { (void)fmt; }
int input_pad_family(void) { return 0; }
static void image_width(double token, double width) {
    OutScale output;
    mk8_ultrawide_present(token);
    output_scale(1680,720,false,0,-1,&output);
    assert(output.vw==width && output.vh==720 && output.vx==(1680-width)/2 && output.vy==0);
    assert(output.sx==output.sy && output.shape==1);
}
int main(void) {
    assert(!getenv("MK8_ULTRAWIDE_TEST"));
    arena=VirtualAlloc((void *)(uintptr_t)ARENA,SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(arena==(void *)(uintptr_t)ARENA);
    unsigned char *module=VirtualAlloc((void *)(uintptr_t)(MAIN_BASE+0x1350000),0x10000,
                                      MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(module==(void *)(uintptr_t)(MAIN_BASE+0x1350000)); slot=module+0x8c70;
    setup(); settings_load();
    double first=mk8_ultrawide_take_present();
    only_aspect(21.f/9.f); assert(wide_active()); // loaded INI, not an environment flag, must activate the real camera
    assert(g_settings.aspect==2 && first==16.0/9.0);
    image_width(first,1280);
    for (int ordinary=0;ordinary<=1;ordinary++) {
        g_settings.aspect=ordinary;
        double queued=mk8_ultrawide_take_present(); assert(queued==21.0/9.0);
        only_aspect(16.f/9.f); assert(!wide_active());
        image_width(queued,1680); // changing the preference cannot relabel an already-produced wide image
        image_width(mk8_ultrawide_take_present(),ordinary==0 ? 1680 : 1280);
        g_settings.aspect=2; only_aspect(21.f/9.f); assert(wide_active());
    }
    g_settings.frame_gen=FRAMEGEN_120; unchanged();
    image_width(mk8_ultrawide_take_present(),1680); assert(g_settings.frame_gen==FRAMEGEN_120);
    g_settings.frame_gen=FRAMEGEN_OFF; unchanged();
    settings_active=true; only_aspect(16.f/9.f); image_width(mk8_ultrawide_take_present(),1280);
    settings_active=false; only_aspect(21.f/9.f);
    hud_ready=false; only_aspect(16.f/9.f); image_width(mk8_ultrawide_take_present(),1280);
    hud_ready=true; only_aspect(21.f/9.f);
    mk8_ultrawide_drawable(960,720); only_aspect(16.f/9.f); image_width(mk8_ultrawide_take_present(),1280);
    mk8_ultrawide_drawable(3000,720); unchanged(); image_width(mk8_ultrawide_take_present(),1280);
    mk8_ultrawide_drawable(1680,720); only_aspect(21.f/9.f);
    assert(mk8_driver_camera_distance(ARENA+O,100.f)==100.f);
    for (int i=0;i<32;i++) { test_now+=16666667; run(); }
    assert(fabsf(mk8_driver_camera_distance(ARENA+O,100.f)-117.f)<.001f);
    settings_save();
    assert(VirtualFree(module,0,MEM_RELEASE) && VirtualFree(arena,0,MEM_RELEASE));
    return 0;
}
'''


class UltrawideTests(unittest.TestCase):
    def test_driver_distance_requires_both_verified_camera_writers(self):
        functions = [f'void {name}(Cpu *c) {{\n{boundary}}}\n'
                     for name, boundary, _, _ in ultrawide.DISTANCE_SITES]
        original = ''.join(functions)
        adapted = ultrawide.patch_distance(original)
        self.assertEqual(adapted.count('= mk8_driver_camera_distance('), 2)
        for name, boundary, register, caller in ultrawide.DISTANCE_SITES:
            function = adapted.split(f'void {name}(Cpu *c) {{', 1)[1].split('\n}', 1)[0]
            self.assertIn(f'const int driver_camera = {caller};', function)
            self.assertIn(boundary + f'    if (driver_camera) VSF32({register}) = ', function)
            for bad in (original.replace(boundary, ''), original.replace(boundary, boundary*2),
                        original.replace(boundary, '') + boundary, adapted):
                with self.assertRaises(ValueError):
                    ultrawide.patch_distance(bad)
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder)/'main'/ultrawide.DISTANCE_UNIT
            src.parent.mkdir()
            src.write_text(original, encoding='utf-8')
            with self.assertRaises(ValueError):
                ultrawide.patch_unit(src, Path(folder)/'out')
            self.assertFalse((Path(folder)/'out').exists())
            crlf = original.replace('\n', '\r\n').encode()
            with mock.patch.object(ultrawide, 'DISTANCE_SHA256', hashlib.sha256(crlf).hexdigest()):
                for raw in (crlf, original.encode()):
                    src.write_bytes(raw)
                    patched = ultrawide.patch_unit(src, Path(folder)/'out')
                    self.assertEqual(patched.read_bytes(), adapted.replace('\n', '\r\n').encode())

    def fixture(self):
        return '#include "cpu.h"\n' + ultrawide.ENTRY + '\n' + ultrawide.BOUNDARY + '}\n'

    def test_scoped_hook_and_boundary_rejection(self):
        original = self.fixture()
        patched = ultrawide.patch(original)
        self.assertEqual(patched.count(ultrawide.HOOK), 1)
        self.assertLess(patched.index('f_70003cc8ac(c);'), patched.index(ultrawide.HOOK))
        self.assertLess(patched.index(ultrawide.HOOK), patched.index('f_70003ccb1c(c);'))
        for broken in (patched, original.replace(ultrawide.ENTRY, 'void other(Cpu *c) {'),
                       original.replace(ultrawide.BOUNDARY, ''), original+original,
                       original.replace(ultrawide.BOUNDARY, ultrawide.BOUNDARY*2),
                       ultrawide.ENTRY+'\n}\nvoid other(Cpu *c) {\n'+ultrawide.BOUNDARY+'}\n'):
            with self.subTest(broken=broken[:60]), self.assertRaises(ValueError):
                ultrawide.patch(broken)

    def test_unit_hash_checked_before_writing(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            src = base/'main'/ultrawide.UNIT
            src.parent.mkdir()
            raw = self.fixture().encode()
            src.write_bytes(raw)
            output = base/'output'
            with self.assertRaises(ValueError):
                ultrawide.patch_unit(src, output)
            self.assertFalse(output.exists())
            with mock.patch.object(ultrawide, 'SHA256', hashlib.sha256(raw).hexdigest()):
                result = ultrawide.patch_unit(src, output)
            self.assertEqual(src.read_bytes(), raw)
            self.assertEqual(result.read_text(), ultrawide.patch(self.fixture()))
            other = base/'main_001.c'
            self.assertEqual(ultrawide.patch_unit(other, output), other)

    @unittest.skipUnless(os.name == 'nt' and DEFAULT_COMPILER.is_file(), 'Windows native compiler required')
    def test_saved_preference_drives_real_camera_and_presentation(self):
        tools = Path(os.environ.get('MK8_NATIVE_TOOLS', ROOT/'local/native-tools'))
        deps = tools/'deps/packages/ucrt64'
        with tempfile.TemporaryDirectory(prefix='mk8-ultrawide-settings-') as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'prepared')
            (out/'rt.h').write_text(RT_H)
            (out/'settings.h').write_bytes((runtime/'settings.h').read_bytes())
            self.assertEqual(HARNESS.count('Settings g_settings;'), 1)
            source = '#define main camera_fixture_main\n'+HARNESS.replace('Settings g_settings;', '')+SAVED_SETTINGS
            (out/'harness.c').write_text(source, encoding='utf-8')
            exe = out/'saved-camera.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1',
                '-UNDEBUG', '-DK32QueryWorkingSetEx=mk8_test_working_set', '-I'+str(out), '-I'+str(runtime),
                '-I'+str(deps/'include'), str(out/'harness.c'), str(ROOT/'runtime/mk8/support/ultrawide.c'),
                str(runtime/'settings.c'), str(runtime/'hle/bindings.c'), str(runtime/'nvn/output_scale.c'),
                str(deps/'lib/libSDL3.dll.a'), '-o', str(exe)], env=compiler_env(), capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            ini = out/'settings.ini'
            ini.write_bytes(b'aspect = ultrawide\r\nframe_gen = off\r\ncamera_distance = 117\r\n')
            env = dict(os.environ, LARECOMP_SETTINGS=str(ini),
                       PATH=str(deps/'bin')+os.pathsep+os.environ.get('PATH', ''))
            for key in ('MK8_ULTRAWIDE_TEST', 'LARECOMP_SCALE', 'LARECOMP_WIDE_TEST'):
                env.pop(key, None)
            for run in ('load', 'reload-saved'):
                result = subprocess.run([str(exe)], env=env, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, (run, result.stdout, result.stderr))
                self.assertIn('aspect = ultrawide\n', ini.read_text())
                self.assertIn('frame_gen = off\n', ini.read_text())
                self.assertIn('camera_distance = 117\n', ini.read_text())

    @unittest.skipUnless(os.name == 'nt' and DEFAULT_COMPILER.is_file(), 'Windows native compiler required')
    def test_real_runtime_guards_and_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            (out/'rt.h').write_text(RT_H)
            (out/'settings.h').write_text(SETTINGS_H)
            (out/'harness.c').write_text(HARNESS)
            exe = out/'camera.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1',
                                     '-DK32QueryWorkingSetEx=mk8_test_working_set',
                                     '-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-I'+str(out), str(out/'harness.c'),
                                     str(ROOT/'runtime/mk8/support/ultrawide.c'), '-o', str(exe)],
                                    env=compiler_env(), capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            for setting, mode in ((None, 'distance'), (None, 'course-depth'), (None, 'off'), ('', 'off'), ('0', 'off'), ('true', 'off'),
                                  ('1', 'on'), ('1', 'api-error'), ('1', 'nonresident'),
                                  ('1', 'rear'), ('1', 'rear-api-error'), ('1', 'rear-nonresident'), ('1', 'ratios'),
                                  ('1', 'intro'), ('1', 'intro-api-error'), ('1', 'intro-nonresident'),
                                  ('1', 'handoff'), ('1', 'handoff-api-error'), ('1', 'handoff-nonresident')):
                env = dict(os.environ)
                env.pop('MK8_ULTRAWIDE_TEST', None)
                if setting is not None:
                    env['MK8_ULTRAWIDE_TEST'] = setting
                result = subprocess.run([str(exe), mode], env=env,
                                        capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, (setting, mode, result.stdout, result.stderr))


if __name__ == '__main__':
    unittest.main()
