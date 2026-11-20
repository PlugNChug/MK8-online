"""Authored HUD objects exercise the real wrapper, preflight and scoped guest adapter."""
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
import ultrawide_hud

RT_H = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#define MAIN_BASE 0x7000000000ull
typedef struct { uint64_t x[31], pc, sp; unsigned char other[512]; } Cpu;
void rt_dispatch(Cpu *c);
extern unsigned diagnostics;
#define LOG(...) (diagnostics++, fprintf(stderr, __VA_ARGS__))
'''

HARNESS = r'''
#include "rt.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error HUD checks require assertions
#endif
// Failed regressions must exit, rather than wait for Windows crash reporting.
#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed: %s, line %d\n", #condition, __LINE__); \
        exit(1); \
    } \
} while (0)
bool mk8_ultrawide_hud_ready(bool intro);
bool mk8_ultrawide_hud_handoff_ready(void);
void mk8_ultrawide_hud(Cpu *, uint64_t);
static _Alignas(8) unsigned char arena[0x18000], before[sizeof arena], expected[sizeof arena];
static uint64_t singleton_slot;
static bool active, compensate, change_count, intro_case;
static double aspect=21.0/9.0;
static unsigned aspect_reads;
static uint64_t denied, unreadable;
static bool fade_readonly;
static unsigned class_color_access;
static float picture_colors[2][16];
static unsigned calls, reads, first, second, third, fourth, fifth, sixth, seventh, compensated;
unsigned diagnostics;
static Cpu input;
enum { UI=0, MANAGER=0x100, TABLE=0xe00, SOLO=0x1400, COMMON=0x5000,
       LAYOUT=0x200, ROOTP=0x400, PAUSE=0x600, FIRST=0x800, SECOND=0xa00,
       FADE=0xc00, WORLD=0xe00, RANK=0x1000, RANK_CHILD=0x1200,
       GO=0x1000, COUNT=0x1200, COUNT1=0x1400, GO_PICTURE=0x1600,
       RACE_NUM=0x1800, RACE_ALL=0x1a00, RACE_BASE=0x1c00,
       ALARM0=0x1800, ALARM_ALL0=0x1900, ALARM_SCOPE0=0x1a00, ALARM_INNER0=0x1b00,
       ALARM1=0x1c00, ALARM_ALL1=0x1d00, ALARM_SCOPE1=0x1e00, ALARM_INNER1=0x1f00,
       ALARM_PIC0=0x2000, ALARM_PIC1=0x2300,
       RIVAL0=0x2600, RIVAL_INNER0=0x2700, RIVAL_ART0=0x2800, RIVAL_MII0=0x2900,
       RIVAL1=0x2a00, RIVAL_INNER1=0x2b00, RIVAL_ART1=0x2c00, RIVAL_MII1=0x2d00,
        OTHER=0x8000, FINISH=0xc000, INTRO=0xf000,
        I_BG=0x800, I_PATTERN=0xa00, I_CUP=0xc00, I_TEXT=0xe00, I_TEAM=0x1000,
        I_CHECK=0x1200, I_CUP_BG=0x1400, I_CLASS_BG=0x1600, I_MODE_BG=0x1800,
        I_BATTLE_BG=0x1a00, I_CUP_INNER=0x1c00, I_TT=0x2000, I_VS=0x2200,
        I_BT=0x2400, I_COMPE=0x2600, I_CUP_TEXT=0x2800, I_CLASSIC=0x2a00,
        I_COURSE=0x2c00, I_CLASS=0x2e00, I_BADGE=0x3000, I_COMPE_TEXT=0x3200,
        I_BATTLE=0x3400, I_WINDOW_PAGE=0x3600, I_CURSOR_PAGE=0x3800,
        I_CUP_PICTURE=0x3a00, I_EXTRA=0x3e00,
        FADER=0x14000, F_PART0=0x800, F_PART1=0x1400, F_LOADING=0x2000,
        F_ALL=0x100, F_BG=0x200, F_BLACK=0x300, F_WHITE=0x400, F_LOGO=0x500 };
static unsigned intro_targets[]={I_CUP,I_CUP_TEXT,I_CLASSIC,I_COURSE,I_CLASS,I_CUP_BG,I_CLASS_BG,I_CHECK};
static uint64_t addr(unsigned at) { return (uint64_t)(uintptr_t)(arena+at); }
static void w64(unsigned at, uint64_t value) { memcpy(arena+at,&value,8); }
static void w32(unsigned at, uint32_t value) { memcpy(arena+at,&value,4); }
static void wf(unsigned at, float value) { memcpy(arena+at,&value,4); }
static float rf(unsigned at) { float value; memcpy(&value,arena+at,4); return value; }
double mk8_ultrawide_aspect(void) { aspect_reads++; return active ? aspect : 16.0/9.0; }
uint8_t *mk8_ultrawide_memory(uint64_t address, size_t size, bool write) {
    reads++;
    if (address == MAIN_BASE+0x135bb20 && size == 8 && !write) return (uint8_t *)&singleton_slot;
    if ((address&3) || address<addr(0) || address>addr(sizeof arena) || size>addr(sizeof arena)-address ||
        address==unreadable || (write && (address==denied || (fade_readonly && address>=addr(FADER))))) return NULL;
    if (address==addr(INTRO+I_CLASS_BG) && size>0xd0 &&
        (class_color_access==2 || (class_color_access==1 && write))) return NULL;
    if (change_count && address==addr(TABLE) && size==135*8 && !write) {
        // Simulate changed metadata after the manager check, not an observed live thread race.
        change_count=false; w32(MANAGER+0xb50,33);
        memcpy(before+MANAGER+0xb50,arena+MANAGER+0xb50,4);
    }
    return (uint8_t *)(uintptr_t)address;
}
static void pane(unsigned p, const char *name, uint64_t vt) {
    w64(p,MAIN_BASE+vt); w64(p+0x20,addr(p+0x20)); w64(p+0x28,addr(p+0x20));
    wf(p+0x48,1); wf(p+0x4c,1); arena[p+0x58]=0x81; arena[p+0x59]=255;
    strcpy((char *)arena+p+0xb8,name);
}
static void children(unsigned p, const unsigned *items, unsigned n) {
    w64(p+0x20,n ? addr(items[n-1]+8) : addr(p+0x20));
    w64(p+0x28,n ? addr(items[0]+8) : addr(p+0x20));
    for (unsigned i=0;i<n;i++) {
        w64(items[i]+8,i ? addr(items[i-1]+8) : addr(p+0x20));
        w64(items[i]+0x10,i+1<n ? addr(items[i+1]+8) : addr(p+0x20));
        w64(items[i]+0x18,addr(p));
    }
}
static void alarm(unsigned p, unsigned index) {
    unsigned outer=p+ALARM0+index*0x400, all=outer+0x100, scope=outer+0x200, inner=outer+0x300;
    pane(outer,index ? "L_Alarm_01" : "L_Alarm_00",0x12bf1a8);
    wf(outer+0x34,-320); wf(outer+0x50,50); wf(outer+0x54,170);
    pane(all,"N_All_00",0x12abd70); wf(all+0x34,60); wf(all+0x50,30); wf(all+0x54,40);
    pane(scope,"N_Alarm_Scope_00",0x12abd70); wf(scope+0x50,50); wf(scope+0x54,50);
    pane(inner,"N_Alarm_00",0x12abd70); wf(inner+0x50,50); wf(inner+0x54,50);
    wf(inner+0x48,.2f); wf(inner+0x4c,.2f); arena[inner+0x59]=0; arena[inner+0x5b]=8;
    unsigned a[]={all},s[]={scope},n[]={inner},pictures[3];
    children(outer,a,1); children(all,s,1); children(scope,n,1);
    const char *names[]={"P_Base_00","P_Item_00","P_Light_00"};
    const float widths[]={88,72,120};
    for (unsigned i=0;i<3;i++) {
        pictures[i]=p+ALARM_PIC0+index*0x300+i*0x100;
        pane(pictures[i],names[i],0x12bf298);
        wf(pictures[i]+0x50,widths[i]); wf(pictures[i]+0x54,widths[i]);
    }
    children(inner,pictures,3);
}
static void rivals(unsigned p) {
    wf(p+WORLD+0x50,30); wf(p+WORLD+0x54,40);
    unsigned slots[]={p+RIVAL0,p+RIVAL1};
    for (unsigned i=0;i<2;i++) {
        unsigned parts=slots[i],inner=parts+0x100,art=parts+0x200,mii=parts+0x300;
        pane(parts,i ? "L_RivalName_01" : "L_RivalName_00",0x12bf1a8);
        wf(parts+0x50,746); wf(parts+0x54,144);
        pane(inner,"N_All_00",0x12abd70); wf(inner+0x50,30); wf(inner+0x54,40);
        pane(art,"Glyph",0); wf(art+0x50,270); wf(art+0x54,40);
        wf(art+0x30,23); wf(art+0x34,-9);
        pane(mii,"Mii",0); wf(mii+0x50,40); wf(mii+0x54,40); wf(mii+0x30,80);
        unsigned a[]={inner},b[]={art,mii}; children(parts,a,1); children(inner,b,2);
        if (!i) { arena[parts+0x58]=0x02; arena[inner+0x59]=0; }
    }
    children(p+WORLD,slots,2);
}
static void page(unsigned p, bool solo) {
    unsigned id=solo ? 101 : 105;
    w64(p,MAIN_BASE+(solo ? 0x1289240 : 0x1287160)); w64(p+0x20,addr(MANAGER));
    w64(p+0x28,addr(p+LAYOUT)); w32(p+0xa0,id); w32(p+0xe8,id); w64(TABLE+id*8,addr(p));
    w64(p+LAYOUT,MAIN_BASE+0x12d5ee0); w64(p+LAYOUT+0x18,addr(p+ROOTP));
    w64(p+LAYOUT+0x80,addr(p));
    pane(p+ROOTP,"RootPane",0x12bf390); wf(p+ROOTP+0x50,1280); wf(p+ROOTP+0x54,720);
    pane(p+PAUSE,"N_Pause_00",0x12abd70);
    pane(p+FIRST,solo ? "N_ItemBoxPos_00" : "L_TVMap",solo ? 0x12abd70 : 0x12bf1a8);
    pane(p+SECOND,solo ? "L_LapCoin_00" : "L_Timer_00",0x12bf1a8);
    wf(p+FIRST+0x30,solo ? -528 : 450); wf(p+FIRST+0x34,solo ? 261 : -190);
    wf(p+FIRST+0x48,solo ? 1 : .6f); wf(p+FIRST+0x4c,solo ? 1 : .6f);
    wf(p+SECOND+0x30,solo ? -460 : 0); wf(p+SECOND+0x34,solo ? -310 : 0);
    wf(p+SECOND+0x48,solo ? .9f : 1); wf(p+SECOND+0x4c,solo ? .9f : 1);
    pane(p+FADE,"L_Fade_00",0x12bf1a8); pane(p+WORLD,"N_RivalName_00",0x12abd70);
    if (solo) {
        pane(p+RANK,"L_Rank_00",0x12bf1a8);
        wf(p+RANK+0x30,509); wf(p+RANK+0x34,-268);
        wf(p+RANK+0x48,.9f); wf(p+RANK+0x4c,.9f);
        wf(p+RANK+0x50,180); wf(p+RANK+0x54,128);
        arena[p+RANK+0x58]=0x02; arena[p+RANK+0x5a]=255;  // observed hidden Time Trial rank
        pane(p+RANK_CHILD,"N_All_00",0x12abd70);
        unsigned nested[]={p+RANK_CHILD}; children(p+RANK,nested,1);
        alarm(p,0); alarm(p,1); rivals(p);
    } else {
        pane(p+GO,"L_Go_00",0x12bf1a8); arena[p+GO+0x58]=0x83;
        pane(p+COUNT,"N_Count_00",0x12abd70); wf(p+COUNT+0x34,50);
        pane(p+COUNT1,"P_Count1",0x12bf298); wf(p+COUNT1+0x50,126); wf(p+COUNT1+0x54,148);
        pane(p+GO_PICTURE,"P_Go_00",0x12bf298); wf(p+GO_PICTURE+0x50,690); wf(p+GO_PICTURE+0x54,148);
        arena[p+COUNT1+0x59]=arena[p+GO_PICTURE+0x59]=0;
        unsigned nested[]={p+COUNT},pictures[]={p+COUNT1,p+GO_PICTURE};
        children(p+GO,nested,1); children(p+COUNT,pictures,2);
        pane(p+RACE_NUM,"L_RaceNum_00",0x12bf1a8); wf(p+RACE_NUM+0x34,-250);
        wf(p+RACE_NUM+0x50,1000); wf(p+RACE_NUM+0x54,156);
        pane(p+RACE_ALL,"N_All_00",0x12abd70);
        pane(p+RACE_BASE,"W_Base_00",0);
        wf(p+RACE_BASE+0x50,995); wf(p+RACE_BASE+0x54,76);
        unsigned banner[]={p+RACE_ALL},base[]={p+RACE_BASE};
        children(p+RACE_NUM,banner,1); children(p+RACE_ALL,base,1);
    }
    unsigned a[]={p+PAUSE,p+FADE},b[]={p+FIRST,p+SECOND,p+WORLD,p+RANK,
                                    p+(solo ? ALARM0 : RACE_NUM),p+ALARM1};
    children(p+ROOTP,a,2); children(p+PAUSE,b,solo ? 6 : 5);
}
static void setup(void) {
    memset(arena,0,sizeof arena); singleton_slot=addr(UI); w64(UI+0x10,addr(MANAGER));
    w64(MANAGER,MAIN_BASE+0x12d45a0); w32(MANAGER+0x28,135); w64(MANAGER+0x30,addr(TABLE));
    w32(MANAGER+0xb50,2); w32(MANAGER+0xb54,32); w64(MANAGER+0xb58,addr(MANAGER+0xb60));
    w64(MANAGER+0xb60,addr(SOLO)); w64(MANAGER+0xb68,addr(COMMON));
    page(SOLO,true); page(COMMON,false); active=true; denied=unreadable=0; fade_readonly=false; intro_case=false;
    class_color_access=0;
}
static void intro_pane(unsigned offset, const char *name, uint64_t vt,
                       float x, float y, float width, float height) {
    unsigned p=INTRO+offset; pane(p,name,vt);
    wf(p+0x30,x); wf(p+0x34,y); wf(p+0x50,width); wf(p+0x54,height);
    arena[p+0x5a]=255;
}
static void intro_setup(bool vs) {
    setup(); intro_case=true;
    intro_targets[5]=vs ? I_MODE_BG : I_CUP_BG;
    w64(INTRO,MAIN_BASE+0x1287d40); w64(INTRO+0x20,addr(MANAGER));
    w64(INTRO+0x28,addr(INTRO+LAYOUT)); w32(INTRO+0xa0,110); w32(INTRO+0xe8,110);
    w64(TABLE+110*8,addr(INTRO)); w64(INTRO+LAYOUT,MAIN_BASE+0x12d5ee0);
    w64(INTRO+LAYOUT+0x18,addr(INTRO+ROOTP)); w64(INTRO+LAYOUT+0x80,addr(INTRO));
    w32(MANAGER+0xb50,3); w64(MANAGER+0xb60,addr(INTRO));
    const unsigned pages[]={I_WINDOW_PAGE,I_CURSOR_PAGE},ids[]={7,13};
    const uint64_t vtables[]={0x12723e0,0x1271df0};
    for (unsigned i=0;i<2;i++) {
        unsigned p=INTRO+pages[i]; w64(p,MAIN_BASE+vtables[i]); w64(p+0x20,addr(MANAGER));
        w32(p+0xa0,ids[i]); w32(p+0xe8,ids[i]); w64(TABLE+ids[i]*8,addr(p));
        w64(MANAGER+0xb68+8*i,addr(p));
    }
    intro_pane(ROOTP,"RootPane",0x12bf390,0,0,1280,720);
    intro_pane(PAUSE,"N_All_00",0x12abd70,0,0,30,40);
    intro_pane(I_BG,"W_BG_00",0x12bf8e8,0,-260,1440,120);
    intro_pane(I_PATTERN,"N_BGPtn_00",0x12abd70,0,0,30,40);
    intro_pane(I_CUP,"N_CupIcon_00",0x12abd70,-480,-258,140,140);
    intro_pane(I_TEXT,"N_Text_00",0x12abd70,0,0,30,40);
    intro_pane(I_TEAM,"N_Team_00",0x12abd70,0,0,30,40);
    arena[INTRO+I_TEAM+0x58]=0;  // local visibility wins over stale effective alpha255
    intro_pane(I_CHECK,"P_MtCheck_32x32",0x12bf298,60,-320,720,120);
    wf(INTRO+I_CHECK+0x3c,-70); arena[INTRO+I_CHECK+0x5b]=8;
    intro_pane(I_CUP_BG,"P_CupIconBGDummy_00",0x12bf298,-440,-260,400,120);
    intro_pane(I_MODE_BG,"P_ModeIconBGDummy_00",0x12bf298,-440,-260,400,120);
    intro_pane(I_CLASS_BG,"P_ClassIconBGDummy_00",0x12bf298,490,-260,300,120);
    intro_pane(I_BATTLE_BG,"P_BattleIconBGDummy_00",0x12bf298,490,-260,300,120);
    arena[INTRO+I_MODE_BG+0x58]=arena[INTRO+I_BATTLE_BG+0x58]=0;
    intro_pane(I_CUP_INNER,"N_CupIcon_01",0x12abd70,0,0,140,140);
    intro_pane(I_TT,"P_ModeIconTt_00",0x12bf298,0,0,122,140);
    intro_pane(I_VS,"P_ModeIconVs_00",0x12bf298,0,0,162,124);
    intro_pane(I_BT,"P_ModeIconBt_00",0x12bf298,0,0,136,128);
    intro_pane(I_COMPE,"N_CompeIcon_00",0x12abd70,0,0,30,40);
    arena[INTRO+I_TT+0x58]=arena[INTRO+I_VS+0x58]=arena[INTRO+I_BT+0x58]=arena[INTRO+I_COMPE+0x58]=0x10;
    const char *pictures[]={"P_CupIconSh_00","P_CupIconDummy_00","P_CupIconNml_00"};
    for (unsigned i=0;i<3;i++) intro_pane(I_CUP_PICTURE+i*0x100,pictures[i],0x12bf298,0,0,140,140);
    arena[INTRO+I_CUP_PICTURE+0x200+0x59]=128;
    intro_pane(I_CUP_TEXT,"T_CupName_00",0x12bf708,-390,-232,820,42);
    intro_pane(I_COMPE_TEXT,"T_CompeName_00",0x12bf708,-390,-232,820,40);
    intro_pane(I_CLASSIC,"T_ClassicName_00",0x12bf708,-380,-288,77,36);
    intro_pane(I_COURSE,"T_CourseName_00",0x12bf708,-290,-280,730,66);
    for (unsigned i=0;i<4;i++) {
        const unsigned texts[]={I_CUP_TEXT,I_COMPE_TEXT,I_CLASSIC,I_COURSE};
        arena[INTRO+texts[i]+0x5b]=1;
    }
    arena[INTRO+I_COMPE_TEXT+0x58]=0;
    intro_pane(I_CLASS,"N_Class_00",0x12abd70,0,0,30,40);
    intro_pane(I_BADGE,"P_ClassIconDummy_00",0x12bf298,520,-258,300,300);
    wf(INTRO+I_BADGE+0x48,.32f); wf(INTRO+I_BADGE+0x4c,.32f);
    const unsigned pictures_rgba[]={I_CLASS_BG,I_BADGE};
    for (unsigned i=0;i<2;i++) {
        unsigned p=INTRO+pictures_rgba[i];
        memset(arena+p+0xd0,0xa7+i,0x50); // authored material/UV sentinels
        memset(arena+p+0xe8,255,16);
    }
    arena[INTRO+I_CLASS_BG+0xeb]=arena[INTRO+I_CLASS_BG+0xef]=15;
    arena[INTRO+I_CLASS_BG+0xf3]=arena[INTRO+I_CLASS_BG+0xf7]=5;
    intro_pane(I_BATTLE,"N_Battle_00",0x12abd70,0,0,30,40); arena[INTRO+I_BATTLE+0x58]=2;
    unsigned root[]={INTRO+PAUSE},all[]={INTRO+I_BG,INTRO+I_PATTERN,INTRO+I_CUP,INTRO+I_TEXT,INTRO+I_TEAM};
    unsigned pattern[]={INTRO+I_CHECK,INTRO+I_CUP_BG,INTRO+I_MODE_BG,INTRO+I_CLASS_BG,INTRO+I_BATTLE_BG};
    unsigned cup[]={INTRO+I_CUP_INNER,INTRO+I_TT,INTRO+I_VS,INTRO+I_BT,INTRO+I_COMPE};
    unsigned art[]={INTRO+I_CUP_PICTURE,INTRO+I_CUP_PICTURE+0x100,INTRO+I_CUP_PICTURE+0x200};
    unsigned texts[]={INTRO+I_CUP_TEXT,INTRO+I_COMPE_TEXT,INTRO+I_CLASSIC,INTRO+I_COURSE,INTRO+I_CLASS,INTRO+I_BATTLE};
    unsigned badge[]={INTRO+I_BADGE};
    children(INTRO+ROOTP,root,1); children(INTRO+PAUSE,all,5); children(INTRO+I_PATTERN,pattern,5);
    children(INTRO+I_CUP,cup,5); children(INTRO+I_CUP_INNER,art,3);
    children(INTRO+I_TEXT,texts,6); children(INTRO+I_CLASS,badge,1);
    if (vs) {
        arena[INTRO+I_CUP_INNER+0x58]=arena[INTRO+I_CUP_BG+0x58]=0x80;
        arena[INTRO+I_VS+0x58]=arena[INTRO+I_MODE_BG+0x58]=0x81;
    }
}
static void fade_setup(void) {
    unsigned p=FADER;
    w64(p,MAIN_BASE+0x12733d8); w64(p+0x20,addr(MANAGER)); w64(p+0x28,addr(p+LAYOUT));
    w32(p+0xa0,4); w32(p+0xe8,4); w64(TABLE+4*8,addr(p));
    w64(p+LAYOUT,MAIN_BASE+0x12d5ee0); w64(p+LAYOUT+0x18,addr(p+ROOTP)); w64(p+LAYOUT+0x80,addr(p));
    pane(p+ROOTP,"RootPane",0x12bf390); wf(p+ROOTP+0x50,1280); wf(p+ROOTP+0x54,720);
    unsigned parts[]={p+F_PART0,p+F_PART1,p+F_LOADING};
    for (unsigned i=0;i<3;i++) {
        unsigned at=parts[i];
        pane(at,i==0 ? "cm_Fade_00" : i==1 ? "cm_Fade_01" : "cm_LoadScreen_00",0x12bf1a8);
        wf(at+0x50,i==2 ? 6100 : 1280); wf(at+0x54,720); arena[at+0x58]=i ? 2 : 0x83;
        pane(at+F_ALL,"N_All_00",0x12abd70); wf(at+F_ALL+0x50,30); wf(at+F_ALL+0x54,40);
        unsigned all[]={at+F_ALL}; children(at,all,1);
        pane(at+F_BG,"P_FadeBG_00",0x12bf298); wf(at+F_BG+0x50,1280); wf(at+F_BG+0x54,720);
        // Authored material/picture bytes are sent to the callback unchanged.
        memset(arena+at+F_BG+0xd0,0xa7,0x30);
        unsigned groups[]={at+F_BG,at+F_BLACK,at+F_WHITE,at+F_LOGO};
        if (i<2) {
            const char *names[]={"N_Load_B","N_Load_W","N_Logo_00"};
            const char *nested[]={"N_Bar_B","N_Bar_W","P_TitleLogo_00"};
            for (unsigned j=0;j<3;j++) {
                unsigned g=groups[j+1],art=at+0x600+j*0x100;
                pane(g,names[j],0x12abd70); wf(g+0x50,30); wf(g+0x54,40);
                if (j<2) { wf(g+0x30,550.99994f); wf(g+0x34,-304); }
                arena[g+0x58]=(i ? 0x80 : 0) | (j==2 ? 2 : 0);
                pane(art,nested[j],j==2 ? 0x12bf298 : 0x12abd70);
                unsigned child[]={art}; children(g,child,1);
            }
        }
        children(at+F_ALL,groups,i==2 ? 1 : 4);
    }
    children(p+ROOTP,parts,3);
    unsigned n=intro_case ? 3 : 2;
    w32(MANAGER+0xb50,n+1); w64(MANAGER+0xb60+n*8,addr(p));
    fade_readonly=true;
}
static void finish_page(unsigned p) {
    w64(p,MAIN_BASE+0x1289538); w64(p+0x20,addr(MANAGER));
    w64(p+0x28,addr(p+LAYOUT)); w32(p+0xa0,106); w32(p+0xe8,106); w64(TABLE+106*8,addr(p));
    w64(p+LAYOUT,MAIN_BASE+0x12d5ee0); w64(p+LAYOUT+0x18,addr(p+ROOTP));
    w64(p+LAYOUT+0x80,addr(p));
    pane(p+ROOTP,"RootPane",0x12bf390); wf(p+ROOTP+0x50,1280); wf(p+ROOTP+0x54,720);
    pane(p+PAUSE,"N_All_00",0x12abd70);
    pane(p+FIRST,"L_Finish_00",0x12bf1a8); wf(p+FIRST+0x50,1725); wf(p+FIRST+0x54,470);
    pane(p+SECOND,"N_Finish_00",0x12abd70); wf(p+SECOND+0x34,50);
    wf(p+SECOND+0x48,2.5f); wf(p+SECOND+0x4c,2.5f); arena[p+SECOND+0x59]=0;
    pane(p+RANK,"P_Finish_00",0x12bf298); wf(p+RANK+0x50,690); wf(p+RANK+0x54,148);
    pane(p+FADE,"L_Fade_00",0x12bf1a8); pane(p+WORLD,"N_RivalName_00",0x12abd70);
    unsigned root[]={p+PAUSE,p+FADE},all[]={p+FIRST,p+WORLD},inner[]={p+SECOND},picture[]={p+RANK};
    children(p+ROOTP,root,2); children(p+PAUSE,all,2);
    children(p+FIRST,inner,1); children(p+SECOND,picture,1);
}
static void finish_setup(void) {
    finish_page(FINISH); w32(MANAGER+0xb50,3); w64(MANAGER+0xb70,addr(FINISH));
}
static void damage_second_warning(unsigned test) {
    switch (test) {
    case 0: strcpy((char *)arena+SOLO+ALARM1+0xb8,"Missing_Alarm"); break;
    case 1: strcpy((char *)arena+SOLO+ALARM_INNER1+0xb8,"Missing_Inner"); break;
    case 2: denied=addr(SOLO+ALARM_INNER1); break;
    case 3: arena[SOLO+ALARM_INNER1+0x5b]=0; break;
    case 4: arena[SOLO+ALARM1+0x5b]=1; break;
    case 5: arena[SOLO+ALARM_ALL1+0x5b]=1; break;
    case 6: arena[SOLO+ALARM_SCOPE1+0x5b]=1; break;
    case 7: wf(SOLO+ALARM1+0x30,1); break;
    case 8: wf(SOLO+ALARM_ALL1+0x34,59); break;
    case 9: wf(SOLO+ALARM_SCOPE1+0x4c,.9f); break;
    case 10: wf(SOLO+ALARM_INNER1+0x44,1); break;
    case 11: wf(SOLO+ALARM_INNER1+0x50,51); break;
    case 12: wf(SOLO+ALARM1+0x54,171); break;
    case 13: wf(SOLO+ALARM_SCOPE1+0x50,51); break;
    case 14: wf(SOLO+ALARM_ALL1+0x50,31); break;
    case 15: wf(SOLO+ALARM_SCOPE1+0x30,NAN); break;
    case 16: wf(SOLO+ALARM_INNER1+0x48,NAN); break;
    case 17: wf(SOLO+ALARM_INNER1+0x48,.19f); wf(SOLO+ALARM_INNER1+0x4c,.19f); break;
    case 18: wf(SOLO+ALARM_INNER1+0x48,1.21f); wf(SOLO+ALARM_INNER1+0x4c,1.21f); break;
    case 19: wf(SOLO+ALARM_INNER1+0x48,.4f); wf(SOLO+ALARM_INNER1+0x4c,.5f); break;
    case 20: w64(SOLO+ALARM_INNER1+0x18,addr(SOLO+ALARM_SCOPE0)); break;
    case 21: w64(SOLO+ALARM_INNER1,MAIN_BASE+0x12bf1a8); break;
    case 22: wf(SOLO+ALARM_SCOPE1+0x30,701); break;
    case 23: wf(SOLO+ALARM_SCOPE1+0x34,1); break;
    case 24: wf(SOLO+ALARM_INNER1+0x30,1); break;
    default: assert(!"unknown warning damage");
    }
}
static void damage_second_label(unsigned test) {
    switch (test) {
    case 0: strcpy((char *)arena+SOLO+WORLD+0xb8,"Missing_Rivals"); break;
    case 1: strcpy((char *)arena+SOLO+RIVAL1+0xb8,"Missing_Parts"); break;
    case 2: strcpy((char *)arena+SOLO+RIVAL_INNER1+0xb8,"Missing_Inner"); break;
    case 3: denied=addr(SOLO+RIVAL_INNER1); break;
    case 4: wf(SOLO+WORLD+0x48,.9f); break;
    case 5: arena[SOLO+RIVAL1+0x5b]=1; break;
    case 6: wf(SOLO+RIVAL1+0x44,1); break;
    case 7: wf(SOLO+RIVAL1+0x50,745); break;
    case 8: wf(SOLO+RIVAL1+0x48,.8f); wf(SOLO+RIVAL1+0x4c,.9f); break;
    case 9: wf(SOLO+RIVAL1+0x48,.79f); wf(SOLO+RIVAL1+0x4c,.79f); break;
    case 10: wf(SOLO+RIVAL1+0x48,1.01f); wf(SOLO+RIVAL1+0x4c,1.01f); break;
    case 11: wf(SOLO+RIVAL1+0x30,NAN); break;
    case 12: wf(SOLO+RIVAL1+0x34,INFINITY); break;
    case 13: wf(SOLO+RIVAL1+0x38,1); break;
    case 14: wf(SOLO+RIVAL_INNER1+0x30,1); break;
    case 15: wf(SOLO+RIVAL_INNER1+0x48,.9f); break;
    case 16: arena[SOLO+RIVAL_INNER1+0x5b]=1; break;
    case 17: wf(SOLO+RIVAL_INNER1+0x54,41); break;
    case 18: w64(SOLO+RIVAL_INNER1+0x18,addr(SOLO+RIVAL0)); break;
    case 19: w64(SOLO+RIVAL_INNER1,MAIN_BASE+0x12bf1a8); break;
    case 20: w64(SOLO+RIVAL1,MAIN_BASE+0x12abd70); break;
    case 21: arena[SOLO+WORLD+0x58]|=0x40; break;
    case 22: arena[SOLO+RIVAL1+0x58]|=0x20; break;
    default: assert(!"unknown label damage");
    }
}
static float old_float(unsigned at) { float value; memcpy(&value,before+at,4); return value; }
static void intro_draw(void) {
    memcpy(expected,before,sizeof expected);
    const double stretch=aspect/(16.0/9.0),width=720.0*aspect;
    const float correction=(float)((16.0/9.0)/aspect);
    const int edges[]={-1,-1,-1,-1,1,-1,1,0};
    if (compensate) {
        for (unsigned i=0;i<8;i++) {
            unsigned p=INTRO+intro_targets[i];
            float x=old_float(p+0x30),scale=old_float(p+0x48);
            double origin=(640.0+rf(p+0x30))*stretch;
            double wanted=(edges[i]<0 ? 640.0 : edges[i]>0 ? width-640.0 : width/2)+x;
            assert(fabs(origin-wanted)<.001);
            assert(fabs(rf(p+0x50)*rf(p+0x48)*stretch-old_float(p+0x50)*scale)<.001);
            assert(rf(p+0x4c)==old_float(p+0x4c));
            if (i>=1 && i<=3) {
                // Anchor1 places the pane origin at its left edge. Glyph/alignment offsets are not that origin.
                const double offsets[]={-13.5,17.25,61.0};
                for (unsigned q=0;q<3;q++) {
                    double glyph=origin+offsets[q]*rf(p+0x48)*stretch;
                    assert(fabs(glyph-(wanted+offsets[q]*scale))<.001);
                    assert(fabs(37.0*rf(p+0x48)*stretch-37.0*scale)<.001);
                }
                assert(arena[p+0x5b]==1);
            }
            if (i==0) {
                assert(fabs(origin-160.0)<.001);
                assert(fabs(140.0*rf(p+0x48)*stretch-140.0*scale)<.001);
                if (intro_targets[5]==I_MODE_BG) {
                    unsigned icon=INTRO+I_VS;
                    assert(fabs(origin+rf(icon+0x30)*rf(p+0x48)*stretch-160.0)<.001);
                    assert(fabs(rf(icon+0x50)*rf(icon+0x48)*rf(p+0x48)*stretch-162.0*scale)<.001);
                    assert(fabs(rf(icon+0x54)*rf(icon+0x4c)*rf(p+0x4c)-124.0*scale)<.001);
                }
            }
            if (i==4) {
                unsigned badge=INTRO+I_BADGE;
                assert(fabs(origin+rf(badge+0x30)*rf(p+0x48)*stretch-(width-120.0))<.001);
                assert(fabs(300.0*rf(badge+0x48)*rf(p+0x48)*stretch-96.0)<.001);
            }
            if (i==5) assert(fabs(origin-200.0)<.001 && fabs(origin-200.0*rf(p+0x48)*stretch)<.001);
            if (i==6) assert(fabs(origin+150.0*rf(p+0x48)*stretch-width)<.001);
            if (i==7) {
                assert(fabs(origin-(width/2+60.0))<.001);
                assert(rf(p+0x3c)==-70 && rf(p+0x40)==0 && rf(p+0x44)==0 && arena[p+0x5b]==8);
                // The X rotation does not mix the artwork's horizontal axis with its vertical axis.
                assert(fabs(720.0*rf(p+0x48)*stretch-720.0)<.001);
                assert(rf(p+0x54)*rf(p+0x4c)==120.f);
            }
            x=x*correction+edges[i]*640.f*(1.f-correction); scale*=correction;
            memcpy(expected+p+0x30,&x,4); memcpy(expected+p+0x48,&scale,4); expected[p+0x58]|=0x10;
        }
        unsigned bar=INTRO+I_BG;
        assert(!memcmp(arena+bar,before+bar,0xd0));
        double half=rf(bar+0x50)*stretch/2;
        if (rf(bar+0x50)==1440) assert(width/2-half<0 && width/2+half>width);
    }
    if (compensate) expected[INTRO+I_CLASS_BG+0xeb]=expected[INTRO+I_CLASS_BG+0xf3]=0;
    // Picture preparation copies RGBA into float buffers inside the recursive layout callback.
    const unsigned pictures[]={INTRO+I_CLASS_BG,INTRO+I_BADGE};
    for (unsigned i=0;i<2;i++)
        for (unsigned channel=0;channel<16;channel++)
            picture_colors[i][channel]=arena[pictures[i]+0xe8+channel]/255.f;
    assert(!memcmp(arena,expected,sizeof arena));  // only class-background left alphas may differ
    if (compensate)
        for (unsigned i=0;i<8;i++) arena[INTRO+intro_targets[i]+0x58]&=~0x10;
}
void rt_dispatch(Cpu *c) {
    calls++;
    assert(!memcmp(c,&input,sizeof input));
    if (intro_case) {
        intro_draw(); c->x[5]=0x12345678; c->pc=0xabcdef; return;
    }
    memcpy(expected,before,sizeof expected);
    const double stretch=aspect/(16.0/9.0);
    const float correction=aspect>16.0/9.0 ? (float)((16.0/9.0)/aspect) : 1;
    if (compensate) {
        unsigned panes[]={first,second,third,fourth,fifth,sixth,seventh};
        for (unsigned i=0;i<compensated;i++) {
            unsigned p=panes[i]; float x,scale;
            memcpy(&x,before+p+0x30,4); memcpy(&scale,before+p+0x48,4);
            bool finish=compensated==1;
            bool solo=!finish && first!=COMMON+FIRST;
            bool warning=solo && (p==first-FIRST+ALARM_INNER0 || p==first-FIRST+ALARM_INNER1);
            bool label=solo && (p==first-FIRST+RIVAL_INNER0 || p==first-FIRST+RIVAL_INNER1);
            bool rank=solo && p==first-FIRST+RANK;
            bool left=solo && !rank && !warning && !label;
            bool centered=finish || warning || label || (first==COMMON+FIRST && i>=2);
            double screen_x=(640.f+rf(p+0x30))*stretch;
            double expected_x=(centered ? 360.0*aspect : left ? 640.0 : 720.0*aspect-640.0)+x;
            assert(fabs(screen_x-expected_x)<.001f);
            if (p==COMMON+GO) {
                assert(fabs(126.f*rf(p+0x48)*stretch-126.f)<.001f);
                assert(fabs(screen_x-360.0*aspect)<.001f);
            }
            if (p==COMMON+RACE_NUM) {
                float nested_scale=rf(COMMON+RACE_ALL+0x48);
                assert(fabs(995.f*rf(p+0x48)*stretch*nested_scale-995.f*nested_scale)<.001f);
            }
            if (finish) {
                float nested_scale=rf(FINISH+SECOND+0x48);
                assert(fabs(690.f*rf(p+0x48)*stretch*nested_scale-690.f*nested_scale)<.001f);
            }
            if (warning) {
                float scope_x;
                memcpy(&scope_x,before+p-0x100+0x30,4);
                assert(fabs(screen_x+rf(p-0x100+0x30)*stretch-(360.0*aspect+scope_x*stretch))<.001f);
                const float widths[]={88,72,120};
                for (unsigned n=0;n<3;n++)
                    assert(fabs(widths[n]*rf(p+0x48)*stretch-widths[n]*scale)<.001f);
                assert(rf(p+0x4c)==scale);  // only horizontal scale is corrected
            }
            if (label) {
                unsigned parts=p-0x100;
                float world_x,world_y,distance;
                memcpy(&world_x,before+parts+0x30,4); memcpy(&world_y,before+parts+0x34,4);
                memcpy(&distance,before+parts+0x48,4);
                double attachment_x=(640.f+rf(parts+0x30))*stretch;
                assert(attachment_x==(640.f+world_x)*stretch);
                assert(360.f-rf(parts+0x34)==360.f-world_y);
                assert(rf(parts+0x48)==distance && rf(parts+0x4c)==distance);
                assert(fabs(746.f*distance*rf(p+0x48)*stretch-746.f*distance)<.001f);
                assert(144.f*distance*rf(p+0x4c)==144.f*distance);
                // Glyph and Mii offsets keep their original pixel sizes relative to the world attachment.
                const float widths[]={270,40};
                for (unsigned n=0;n<2;n++) {
                    unsigned art=p+0x100+n*0x100;
                    float child_x,child_y;
                    memcpy(&child_x,before+art+0x30,4); memcpy(&child_y,before+art+0x34,4);
                    double picture_x=attachment_x+rf(art+0x30)*rf(parts+0x48)*rf(p+0x48)*stretch;
                    assert(fabs(picture_x-(attachment_x+child_x*distance))<.001f);
                    assert(rf(art+0x34)*rf(parts+0x4c)*rf(p+0x4c)==child_y*distance);
                    assert(fabs(rf(art+0x50)*rf(parts+0x48)*rf(p+0x48)*stretch-widths[n]*distance)<.001f);
                    assert(rf(art+0x54)*rf(parts+0x4c)*rf(p+0x4c)==40.f*distance);
                }
            }
            assert(fabs(rf(p+0x48)*stretch-scale)<.00001f);
            // The nested timer animation keeps its offset and dimensions inside the moved group.
            if (p==COMMON+SECOND)
                assert(fabs(screen_x+494.f*rf(p+0x48)*stretch-(expected_x+494.f*scale))<.001f);
            if (rank) {
                assert(fabs(screen_x-(720.0*aspect-131.0))<.001f);
                float nested_scale=rf(p-RANK+RANK_CHILD+0x48);
                assert(fabs(180.f*rf(p+0x48)*stretch*nested_scale-162.f*nested_scale)<.001f);
            }
            x=x*correction+(centered ? 0.f : left ? -640.f : 640.f)*(1.f-correction);
            scale*=correction;
            memcpy(expected+p+0x30,&x,4); memcpy(expected+p+0x48,&scale,4);
            expected[p+0x58]|=0x10;
        }
    }
    assert(!memcmp(arena,expected,sizeof arena));  // includes fades, projected parent transforms and nested animation
    if (compensate) {
        unsigned panes[]={first,second,third,fourth,fifth,sixth,seventh};
        for (unsigned i=0;i<compensated;i++) arena[panes[i]+0x58]&=~0x10;
    }
    c->x[5]=0x12345678; c->pc=0xabcdef;  // legitimate callee changes must survive the wrapper
}
static void run(unsigned p, bool apply) {
    memset(&input,0xa5,sizeof input); input.pc=MAIN_BASE+0x592808; input.x[0]=addr(p+ROOTP);
    Cpu c=input; compensate=apply; first=p+FIRST; second=p+SECOND; third=p+RANK; fourth=p+RACE_NUM;
    fifth=sixth=seventh=0; calls=0;
    compensated=apply ? (p==FINISH ? 1 : 4) : 0;
    if (p!=FINISH && p!=COMMON) {
        fourth=p+ALARM_INNER1;
        if (arena[p+RANK+0x58]&1) {
            fourth=p+ALARM_INNER0; fifth=p+ALARM_INNER1;
            sixth=p+RIVAL_INNER0; seventh=p+RIVAL_INNER1;
            if (apply) compensated=7;
        } else {
            third=p+ALARM_INNER0; fifth=p+RIVAL_INNER0; sixth=p+RIVAL_INNER1;
            if (apply) compensated=6;
        }
    }
    unsigned reads_before=aspect_reads;
    memcpy(before,arena,sizeof arena); mk8_ultrawide_hud(&c,addr(p+LAYOUT));
    assert(aspect_reads==reads_before+1);
    assert(calls==1); input.x[5]=0x12345678; input.pc=0xabcdef; assert(!memcmp(&c,&input,sizeof c));
    unsigned panes[]={first,second,third,fourth,fifth,sixth,seventh};
    for (unsigned i=0;i<compensated;i++) before[panes[i]+0x58]|=0x10;
    assert(!memcmp(arena,before,sizeof arena));
}
static void run_intro(bool apply) {
    memset(&input,0xa5,sizeof input); input.pc=MAIN_BASE+0x592808; input.x[0]=addr(INTRO+ROOTP);
    Cpu c=input; compensate=apply; calls=0;
    unsigned reads_before=aspect_reads;
    memcpy(before,arena,sizeof arena); mk8_ultrawide_hud(&c,addr(INTRO+LAYOUT));
    assert(calls==1 && aspect_reads==reads_before+1);
    input.x[5]=0x12345678; input.pc=0xabcdef; assert(!memcmp(&c,&input,sizeof c));
    if (apply)
        for (unsigned i=0;i<8;i++) before[INTRO+intro_targets[i]+0x58]|=0x10;
    assert(!memcmp(arena,before,sizeof arena));
    const unsigned pictures[]={INTRO+I_CLASS_BG,INTRO+I_BADGE};
    for (unsigned i=0;i<2;i++)
        for (unsigned channel=0;channel<16;channel++) {
            float wanted=apply && i==0 && (channel==3 || channel==11) ? 0.f :
                         before[pictures[i]+0xe8+channel]/255.f;
            assert(picture_colors[i][channel]==wanted); // retained draw copy, after source restoration
        }
}
static void run_fade(unsigned bad_entry) {
    bool was_intro=intro_case; intro_case=false;
    memset(&input,0xa5,sizeof input);
    input.pc=MAIN_BASE+(bad_entry==1 ? 0x592804 : 0x592808);
    input.x[0]=addr(bad_entry==2 ? SOLO+ROOTP : FADER+ROOTP);
    Cpu c=input; compensate=false; calls=0;
    memcpy(before,arena,sizeof arena); mk8_ultrawide_hud(&c,addr(FADER+LAYOUT));
    assert(calls==1); input.x[5]=0x12345678; input.pc=0xabcdef; assert(!memcmp(&c,&input,sizeof c));
    assert(!memcmp(arena,before,sizeof arena));  // Includes material bytes and the real callback's inputs.
    intro_case=was_intro;
}
static void damage_fade(unsigned test) {
    unsigned p=FADER, part=p+F_PART1, bg=part+F_BG;
    switch (test) {
    case 0: w64(p+0x20,addr(OTHER)); break;
    case 1: w64(TABLE+4*8,addr(OTHER)); break;
    case 2: w32(p+0xe8,5); break;
    case 3: w64(p+LAYOUT+0x80,addr(OTHER)); break;
    case 4: w64(p+LAYOUT+0x88,addr(OTHER)); break;
    case 5: w64(p+ROOTP+0x18,addr(OTHER)); break;
    case 6: wf(p+ROOTP+0x48,.9f); break;
    case 7: wf(p+ROOTP+0x50,1279); break;
    case 8: w64(part+0x18,addr(p+F_PART0)); break;
    case 9: wf(part+0x34,1); break;
    case 10: wf(part+0x54,719); break;
    case 11: wf(part+F_ALL+0x4c,.9f); break;
    case 12: wf(bg+0x30,1); break;
    case 13: wf(bg+0x44,1); break;
    case 14: wf(bg+0x50,1279); break;
    case 15: arena[bg+0x5b]=8; break;
    case 16: w64(bg,MAIN_BASE+0x12abd70); break;
    case 17: unreadable=addr(bg); break;
    case 18: arena[p+F_LOADING+0x58]|=1; break;
    case 19: arena[p+F_PART0+F_BLACK+0x58]|=1; break;
    case 20: arena[p+F_PART0+F_WHITE+0x58]|=1; break;
    case 21: arena[p+F_PART0+F_LOGO+0x58]|=1; break;
    case 22: arena[part+F_BLACK+0x58]|=1; break;
    case 23: arena[part+F_WHITE+0x58]|=1; break;
    case 24: arena[part+F_LOGO+0x58]|=1; break;
    case 25: strcpy((char *)arena+part+0xb8,"cm_Fade_00"); break;
    case 26: strcpy((char *)arena+bg+0xb8,"Missing_Background"); break;
    case 27: w64(part+F_ALL+0x20,addr(bg+8)); break;
    case 28: w64(bg+8,addr(bg+8)); break;
    case 29: wf(bg+0x48,NAN); break;
    case 30: w32(MANAGER+0xb50,4); w64(MANAGER+0xb78,addr(p)); break;
    case 31: w64(bg+0x28,addr(p+ROOTP+8)); break;
    case 32: arena[part+F_ALL+0x58]|=0x40; break;
    case 33: w64(part,MAIN_BASE+0x12abd70); break;
    default: assert(!"unknown fade damage");
    }
}
static void fade_tests(const char *name) {
    if (!strcmp(name,"fade-guards")) {
        for (unsigned test=0;test<34;test++) {
            setup(); fade_setup(); damage_fade(test); memcpy(before,arena,sizeof arena);
            assert(!mk8_ultrawide_hud_ready(false)); assert(!memcmp(before,arena,sizeof arena));
            active=false; run_fade(0); assert(!diagnostics);
        }
        setup(); fade_setup(); assert(mk8_ultrawide_hud_ready(false)); return;
    }
    if (!strncmp(name,"fade-late-",10)) {
        setup(); fade_setup(); assert(mk8_ultrawide_hud_ready(false));
        unsigned bad_entry=!strcmp(name+10,"pc") ? 1 : !strcmp(name+10,"root") ? 2 : 0;
        if (!bad_entry) damage_fade((unsigned)atoi(name+10));
        run_fade(bad_entry);
        assert(active && diagnostics==1 && !mk8_ultrawide_hud_ready(false));
        run_fade(bad_entry); run(COMMON,true); assert(diagnostics==1); return;
    }
    assert(!strcmp(name,"fade"));
    const unsigned alphas[]={0,90,228,220,78,255};
    for (unsigned kind=0;kind<3;kind++) {
        if (kind) intro_setup(kind==2); else setup();
        assert(mk8_ultrawide_hud_ready(kind!=0));
        fade_setup();
        for (unsigned i=0;i<sizeof alphas/sizeof *alphas;i++) {
            unsigned bg=FADER+F_PART0+F_BG;
            arena[bg+0x59]=(unsigned char)alphas[i];
            arena[bg+0x5a]=(unsigned char)(i ? alphas[i] : 1); // previous traversal may differ
            memcpy(before,arena,sizeof arena);
            assert(mk8_ultrawide_hud_ready(kind!=0)); assert(!memcmp(before,arena,sizeof arena));
            // Alpha/visibility can change after camera preflight; neither changes full-screen coverage geometry.
            arena[bg+0x59]^=255; arena[FADER+F_PART1+0x58]^=1;
            run_fade(0);
            assert(fabs(rf(bg+0x50)*(aspect/(16.0/9.0))-720.0*aspect)<.001);
            if (kind) run_intro(true); else { run(SOLO,true); run(COMMON,true); }
            assert(mk8_ultrawide_hud_ready(kind!=0) && !diagnostics);
        }
        // Normal narrow loading and removal/reentry do not permanently reject later valid framing.
        arena[FADER+F_LOADING+0x58]|=1; active=false; run_fade(0);
        assert(!mk8_ultrawide_hud_ready(kind!=0) && !diagnostics);
        w32(MANAGER+0xb50,kind ? 3 : 2); active=true;
        assert(mk8_ultrawide_hud_ready(kind!=0));
        arena[FADER+F_LOADING+0x58]&=~1; w32(MANAGER+0xb50,kind ? 4 : 3);
        assert(mk8_ultrawide_hud_ready(kind!=0)); run_fade(0); assert(!diagnostics);
    }
}
static void handoff_setup(void) {
    setup(); fade_setup(); finish_page(FINISH);
    const unsigned ids[]={118,7,13}, vtables[]={0x1287458,0x12723e0,0x1271df0};
    for (unsigned i=0;i<3;i++) {
        unsigned p=OTHER+i*0x100;
        w64(p,MAIN_BASE+vtables[i]); w64(p+0x20,addr(MANAGER));
        w32(p+0xa0,ids[i]); w32(p+0xe8,ids[i]); w64(TABLE+ids[i]*8,addr(p));
    }
    // The observed handoff has the six race pages followed by the valid fade page.
    const unsigned pages[]={SOLO,COMMON,FINISH,OTHER,OTHER+0x100,OTHER+0x200,FADER};
    w32(MANAGER+0xb50,7);
    for (unsigned i=0;i<7;i++) w64(MANAGER+0xb60+i*8,addr(pages[i]));
}
static void handoff_tests(void) {
    setup(); assert(mk8_ultrawide_hud_ready(false) && !mk8_ultrawide_hud_handoff_ready());
    handoff_setup(); memcpy(before,arena,sizeof arena);
    assert(mk8_ultrawide_hud_handoff_ready() && !mk8_ultrawide_hud_ready(true));
    assert(!memcmp(before,arena,sizeof arena));
    for (unsigned i=0;i<3;i++) {
        run_fade(0); run(SOLO,true); run(COMMON,true); run(FINISH,true);
        assert(active && !diagnostics && mk8_ultrawide_hud_handoff_ready());
    }
    // Losing the fade prevents only this handoff, without poisoning ordinary race readiness.
    w32(MANAGER+0xb50,6);
    assert(!mk8_ultrawide_hud_handoff_ready() && mk8_ultrawide_hud_ready(false));
    w32(MANAGER+0xb50,7); assert(mk8_ultrawide_hud_handoff_ready());
    arena[FADER+F_LOADING+0x58]|=1; memcpy(before,arena,sizeof arena);
    assert(!mk8_ultrawide_hud_handoff_ready() && !memcmp(before,arena,sizeof arena));
    active=false; run_fade(0); assert(!diagnostics);
    arena[FADER+F_LOADING+0x58]&=~1; active=true;
    assert(mk8_ultrawide_hud_handoff_ready());
    // Both intro variants with a fade still belong to the intro class, not the race handoff.
    for (unsigned vs=0;vs<2;vs++) {
        intro_setup(vs); fade_setup(); memcpy(before,arena,sizeof arena);
        assert(mk8_ultrawide_hud_ready(true) && !mk8_ultrawide_hud_handoff_ready());
        assert(!memcmp(before,arena,sizeof arena));
    }
    for (unsigned test=0;test<34;test++) {
        setup(); fade_setup(); damage_fade(test); memcpy(before,arena,sizeof arena);
        assert(!mk8_ultrawide_hud_handoff_ready() && !memcmp(before,arena,sizeof arena));
    }
    for (unsigned test=0;test<7;test++) {
        handoff_setup();
        if (test==0) w64(TABLE+101*8,addr(OTHER));
        else if (test==1) w64(COMMON+0x20,addr(OTHER));
        else if (test==2) denied=addr(SOLO+FIRST);
        else if (test==3) strcpy((char *)arena+COMMON+RACE_NUM+0xb8,"Missing_RaceNum");
        else if (test==4) w64(OTHER,MAIN_BASE+0x1287459); // unknown active page
        else if (test==5) w64(MANAGER+0xb78,addr(SOLO)); // duplicate active page
        else w64(MANAGER+0xb60,addr(OTHER)); // missing required solo HUD
        memcpy(before,arena,sizeof arena);
        assert(!mk8_ultrawide_hud_handoff_ready() && !memcmp(before,arena,sizeof arena));
    }
    handoff_setup(); assert(mk8_ultrawide_hud_handoff_ready() && !diagnostics);
}
static void damage_intro(unsigned test) {
    unsigned p=INTRO;
    switch (test) {
    case 0: strcpy((char *)arena+p+I_CHECK+0xb8,"Missing_Checker"); break;
    case 1: denied=addr(p+I_CHECK); break;
    case 2: wf(p+I_CHECK+0x3c,-69); break;
    case 3: wf(p+I_CHECK+0x40,1); break;
    case 4: arena[p+I_CHECK+0x5b]=0; break;
    case 5: wf(p+I_CHECK+0x50,721); break;
    case 6: wf(p+I_CHECK+0x48,NAN); break;
    case 7: arena[p+I_CUP_TEXT+0x5b]=0; break;
    case 8: wf(p+I_TEXT+0x48,.9f); break;
    case 9: wf(p+I_PATTERN+0x30,1); break;
    case 10: wf(p+I_BADGE+0x50,301); break;
    case 11: wf(p+I_BADGE+0x48,.33f); break;
    case 12: wf(p+I_CUP+0x48,.9f); break;
    case 13: wf(p+I_CUP+0x48,.89f); wf(p+I_CUP+0x4c,.89f); break;
    case 14: wf(p+I_CUP+0x48,1.51f); wf(p+I_CUP+0x4c,1.51f); break;
    case 15: wf(p+I_COURSE+0x30,NAN); break;
    case 16: wf(p+I_COURSE+0x50,0); break;
    case 17: w64(p,MAIN_BASE+0x1289538); break;
    case 18: w32(p+0xe8,105); break;
    case 19: w64(p+0x20,addr(OTHER)); break;
    case 20: w64(TABLE+110*8,addr(OTHER)); break;
    case 21: w64(p+LAYOUT+0x80,addr(OTHER)); break;
    case 22: w64(p+LAYOUT+0x88,addr(OTHER+LAYOUT)); break;
    case 23: w64(p+ROOTP+0x18,addr(OTHER+ROOTP)); break;
    case 24: w32(MANAGER+0xb50,2); break;
    case 25: w64(MANAGER+0xb70,addr(p+I_WINDOW_PAGE)); break;
    case 26: w32(MANAGER+0xb50,4); w64(MANAGER+0xb78,addr(COMMON)); break;
    case 27: w64(MANAGER+0xb60,addr(SOLO)); break;
    case 28: arena[p+I_TEAM+0x58]|=1; break;
    case 29: arena[p+I_COMPE_TEXT+0x58]|=1; break;
    case 30: arena[p+I_BATTLE+0x58]|=1; break;
    case 31: arena[p+I_TT+0x58]|=1; break;
    case 32: arena[p+(intro_targets[5]==I_MODE_BG ? I_CUP_INNER : I_VS)+0x58]|=1; break;
    case 33: arena[p+I_BT+0x58]|=1; break;
    case 34: arena[p+I_COMPE+0x58]|=1; break;
    case 35: arena[p+(intro_targets[5]==I_MODE_BG ? I_CUP_BG : I_MODE_BG)+0x58]|=1; break;
    case 36: arena[p+I_BATTLE_BG+0x58]|=1; break;
    case 37: {
        intro_pane(I_EXTRA,"Unknown",0x12abd70,0,0,30,40);
        unsigned items[]={p+I_BG,p+I_PATTERN,p+I_CUP,p+I_TEXT,p+I_TEAM,p+I_EXTRA};
        children(p+PAUSE,items,6); break;
    }
    case 38: {
        intro_pane(I_EXTRA,"Unknown",0x12abd70,0,0,30,40);
        unsigned items[]={p+I_CUP_TEXT,p+I_COMPE_TEXT,p+I_CLASSIC,p+I_COURSE,p+I_CLASS,p+I_BATTLE,p+I_EXTRA};
        children(p+I_TEXT,items,7); break;
    }
    case 39: {
        intro_pane(I_EXTRA,"Unknown",0x12abd70,0,0,30,40);
        unsigned items[]={p+I_BADGE,p+I_EXTRA}; children(p+I_CLASS,items,2); break;
    }
    case 40: w64(p+I_WINDOW_PAGE,MAIN_BASE+0x1271df0); break;
    case 41: w32(MANAGER+0xb50,33); break;
    case 42: {
        intro_pane(I_EXTRA,"Unknown",0x12abd70,0,0,30,40);
        unsigned items[]={p+PAUSE,p+I_EXTRA}; children(p+ROOTP,items,2); break;
    }
    case 43: w64(p+I_CHECK+0x18,addr(p+I_TEXT)); break;
    case 44: wf(p+I_BG+0x50,159); break;
    case 45: wf(p+I_BG+0x50,1441); break;
    case 46: wf(p+I_BG+0x44,1); break;
    case 47: wf(p+I_CLASSIC+0x54,INFINITY); break;
    case 48: arena[p+(intro_targets[5]==I_MODE_BG ? I_VS : I_CUP_INNER)+0x58]&=~1; break;
    case 49: arena[p+intro_targets[5]+0x58]&=~1; break;
    case 50:  // A page ID/vtable without the validated fade layout must remain rejected.
        w64(OTHER,MAIN_BASE+0x12733d8); w64(OTHER+0x20,addr(MANAGER));
        w32(OTHER+0xa0,4); w32(OTHER+0xe8,4); w64(TABLE+4*8,addr(OTHER));
        w32(MANAGER+0xb50,4); w64(MANAGER+0xb78,addr(OTHER)); break;
    case 51: w64(p+I_VS,MAIN_BASE+0x12abd70); break;
    case 52: wf(p+I_VS+0x30,1); break;
    case 53: wf(p+I_VS+0x38,1); break;
    case 54: wf(p+I_VS+0x44,1); break;
    case 55: wf(p+I_VS+0x48,.9f); break;
    case 56: wf(p+I_VS+0x4c,1.1f); break;
    case 57: wf(p+I_VS+0x50,163); break;
    case 58: wf(p+I_VS+0x54,125); break;
    case 59: arena[p+I_VS+0x5b]=1; break;
    case 60: arena[p+I_VS+0x58]|=0x20; break;
    case 61: wf(p+I_MODE_BG+0x50,401); break;
    case 62: w64(p+I_MODE_BG,MAIN_BASE+0x12abd70); break;
    case 63: denied=addr(p+I_MODE_BG); break;
    case 64: strcpy((char *)arena+p+I_MODE_BG+0xb8,"Missing_ModeBackground"); break;
    case 65: wf(p+I_VS+0x48,NAN); break;
    case 66:  // Exactly one icon and one background, but they belong to different variants.
        arena[p+intro_targets[5]+0x58]&=~1;
        arena[p+(intro_targets[5]==I_MODE_BG ? I_CUP_BG : I_MODE_BG)+0x58]|=1; break;
    case 67: class_color_access=1; break; // base pane writable, corner-color tail read-only
    case 68: class_color_access=2; break; // base pane readable, corner-color tail unavailable
    default: assert(!"unknown intro damage");
    }
}
static void intro_tests(const char *name) {
    bool vs=!strncmp(name,"intro-vs",8);
    const char *mode=name+(vs ? 8 : 5);
    intro_setup(vs);
    if (!strcmp(mode,"-kind")) {
        setup(); memcpy(before,arena,sizeof arena);
        assert(mk8_ultrawide_hud_ready(false) && !mk8_ultrawide_hud_ready(true));
        assert(!memcmp(before,arena,sizeof arena));
        intro_setup(vs); memcpy(before,arena,sizeof arena);
        assert(mk8_ultrawide_hud_ready(true) && !mk8_ultrawide_hud_ready(false));
        assert(!memcmp(before,arena,sizeof arena));
        // Combining the two individually supported page sets does not qualify either camera kind.
        w32(MANAGER+0xb50,5); w64(MANAGER+0xb78,addr(SOLO)); w64(MANAGER+0xb80,addr(COMMON));
        memcpy(before,arena,sizeof arena);
        assert(!mk8_ultrawide_hud_ready(false) && !mk8_ultrawide_hud_ready(true));
        assert(!memcmp(before,arena,sizeof arena));
        w32(MANAGER+0xb50,3); w64(MANAGER+0xb60,addr(SOLO));
        w64(MANAGER+0xb68,addr(COMMON)); w64(MANAGER+0xb70,addr(INTRO));
        memcpy(before,arena,sizeof arena);
        assert(!mk8_ultrawide_hud_ready(false) && !mk8_ultrawide_hud_ready(true));
        assert(!memcmp(before,arena,sizeof arena));
        return;
    }
    if (!strcmp(mode,"-draw")) { run_intro(true); return; }
    if (!strcmp(mode,"-edge")) {
        const unsigned char alpha[][4]={{15,15,5,5},{127,201,3,99},{255,255,255,255},{0,99,0,3}};
        for (unsigned i=0;i<sizeof alpha/sizeof *alpha;i++) {
            for (unsigned corner=0;corner<4;corner++) {
                arena[INTRO+I_CLASS_BG+0xeb+4*corner]=alpha[i][corner];
                arena[INTRO+I_CLASS_BG+0xe8+4*corner]=(unsigned char)(17+corner);
            }
            assert(mk8_ultrawide_hud_ready(true)); run_intro(true);
            active=false; run_intro(false); active=true;
        }
        aspect=16.0/9.0; run_intro(false); // ordinary proportions do not acquire the fade
        return;
    }
    if (!strcmp(mode,"-edge-guards")) {
        for (unsigned test=67;test<=68;test++) {
            intro_setup(vs); damage_intro(test); memcpy(before,arena,sizeof arena);
            assert(!mk8_ultrawide_hud_ready(true) && !memcmp(before,arena,sizeof arena));
            active=false; run_intro(false); assert(!diagnostics);
        }
        return;
    }
    if (!strcmp(mode,"-guards")) {
        for (unsigned test=0;test<(vs ? 67u : 51u);test++) {
            intro_setup(vs); damage_intro(test); memcpy(before,arena,sizeof arena);
            assert(!mk8_ultrawide_hud_ready(true)); assert(!memcmp(before,arena,sizeof arena));
            active=false; run_intro(false); assert(!diagnostics);
        }
        intro_setup(vs); assert(mk8_ultrawide_hud_ready(true)); return;
    }
    if (!strncmp(mode,"-late-",6)) {
        unsigned test=(unsigned)atoi(mode+6);
        damage_intro(test); run_intro(false);
        assert(active && !mk8_ultrawide_hud_ready(true));
        if (test==21) {
            // An unknown layout owner cannot identify a supported page for sticky rejection.
            assert(!diagnostics); run_intro(false); assert(!diagnostics);
            w64(INTRO+LAYOUT+0x80,addr(INTRO));
            assert(mk8_ultrawide_hud_ready(true)); run_intro(true);
            assert(!diagnostics); return;
        }
        assert(diagnostics==1);
        run_intro(false); assert(diagnostics==1); return;
    }
    assert(!strcmp(mode,""));
    assert(mk8_ultrawide_hud_ready(true));
    assert(640.f+rf(INTRO+I_CUP_TEXT+0x30)==250.f);
    assert(640.f+rf(INTRO+I_CLASSIC+0x30)==260.f);
    assert(640.f+rf(INTRO+I_COURSE+0x30)==350.f);
    denied=addr(INTRO+(vs ? I_CUP_BG : I_MODE_BG));
    assert(mk8_ultrawide_hud_ready(true)); run_intro(true); denied=0;
    for (unsigned i=0;i<100;i++) run_intro(true);
    for (unsigned i=0;i<3;i++) {
        const float scales[]={.9f,1.2f,1.5f},bar_widths[]={160,800,1440};
        wf(INTRO+I_CUP+0x48,scales[i]); wf(INTRO+I_CUP+0x4c,scales[i]);
        wf(INTRO+I_BG+0x50,bar_widths[i]);
        arena[INTRO+I_CUP+0x59]=(unsigned char)(127*i);
        arena[INTRO+I_TEXT+0x59]=(unsigned char)(255-100*i);
        arena[INTRO+I_PATTERN+0x59]=(unsigned char)(80*i);
        assert(mk8_ultrawide_hud_ready(true)); run_intro(true);
    }
    // A registered hidden card may become visible after preflight; fresh animation values survive restoration.
    arena[INTRO+I_CUP+0x58]=arena[INTRO+I_TEXT+0x58]=0x82;
    arena[INTRO+I_CUP+0x59]=arena[INTRO+I_TEXT+0x59]=0;
    assert(mk8_ultrawide_hud_ready(true));
    arena[INTRO+I_CUP+0x58]=arena[INTRO+I_TEXT+0x58]=0x83;
    arena[INTRO+I_CUP+0x59]=201; arena[INTRO+I_TEXT+0x59]=213;
    wf(INTRO+I_CUP+0x48,1.2f); wf(INTRO+I_CUP+0x4c,1.2f); run_intro(true);
    // Authored localized/retro text positioning: do not pin English text-box metrics.
    wf(INTRO+I_CLASSIC+0x30,-410); wf(INTRO+I_CLASSIC+0x50,110);
    wf(INTRO+I_COURSE+0x30,-275); wf(INTRO+I_COURSE+0x50,680);
    assert(mk8_ultrawide_hud_ready(true)); run_intro(true);
    active=false; reads=0; run_intro(false); assert(!reads && !diagnostics);
}
int main(int argc, char **argv) {
    assert(argc==3); aspect=atof(argv[2])/9.0; setup();
    if (!strcmp(argv[1],"handoff")) { handoff_tests(); return 0; }
    if (!strncmp(argv[1],"fade",4)) { fade_tests(argv[1]); return 0; }
    if (!strncmp(argv[1],"intro",5)) { intro_tests(argv[1]); return 0; }
    if (!strncmp(argv[1],"finish",6)) finish_setup();
    assert(mk8_ultrawide_hud_ready(false));
    if (!strcmp(argv[1],"ratios")) {
        const double ratios[]={16.0/9.0,21.0/9.0,2560.0/1080.0,3440.0/1440.0,3840.0/1600.0,32.0/9.0,16.0/9.0};
        for (unsigned i=0;i<sizeof ratios/sizeof *ratios;i++) {
            aspect=ratios[i]; reads=0;
            run(SOLO,aspect>16.0/9.0); run(COMMON,aspect>16.0/9.0);
            if (aspect==16.0/9.0) assert(!reads);
        }
        aspect=0; reads=0; run(SOLO,false); assert(!reads && !diagnostics);
        return 0;
    }
    if (!strcmp(argv[1],"normal")) {
        for (unsigned i=0;i<100;i++) { run(SOLO,true); run(COMMON,true); }
        assert(rf(SOLO+FIRST+0x30)==-528 && rf(COMMON+FIRST+0x48)==.6f);
        // A legitimate post-preflight animation is consumed fresh and restored, never accumulated.
        assert(mk8_ultrawide_hud_ready(false)); wf(SOLO+FIRST+0x30,-500); run(SOLO,true);
        assert(rf(SOLO+FIRST+0x30)==-500 && mk8_ultrawide_hud_ready(false));
        wf(COMMON+FIRST+0x30,425); wf(COMMON+SECOND+0x30,30); run(COMMON,true);
        assert(rf(COMMON+FIRST+0x30)==425 && rf(COMMON+SECOND+0x30)==30 && mk8_ultrawide_hud_ready(false));
        page(OTHER,true); w64(OTHER,MAIN_BASE+0x1287458); w64(TABLE+101*8,addr(SOLO));
        run(OTHER,false); assert(!diagnostics && mk8_ultrawide_hud_ready(false));
        active=false; reads=0; run(SOLO,false); assert(!reads && !diagnostics);
        return 0;
    }
    if (!strcmp(argv[1],"guards")) {
        for (unsigned test=0;test<34;test++) {
            setup();
            switch (test) {
            case 0: singleton_slot=0; break;
            case 1: w64(MANAGER,0); break;
            case 2: w32(MANAGER+0x28,136); break;
            case 3: w32(MANAGER+0xb50,33); break;
            case 4: w32(MANAGER+0xb54,64); break;
            case 5: w64(MANAGER+0xb58,addr(TABLE)); break;
            case 6: w64(MANAGER+0xb68,addr(SOLO)); break;
            case 7: w64(TABLE+101*8,addr(COMMON)); break;
            case 8: w64(SOLO+0x20,0); break;
            case 9: w32(SOLO+0xe8,105); break;
            case 10: w64(SOLO+LAYOUT+0x80,addr(COMMON)); break;
            case 11: w64(SOLO+LAYOUT+0x88,addr(COMMON+LAYOUT)); break;
            case 12: w64(SOLO+ROOTP+0x18,addr(COMMON+ROOTP)); break;
            case 13: wf(SOLO+ROOTP+0x50,1920); break;
            case 14: wf(SOLO+PAUSE+0x48,.5f); break;
            case 15: arena[SOLO+SECOND+0x58]|=0x20; break;
            case 16: arena[SOLO+SECOND+0x58]|=0x40; break;
            case 17: arena[SOLO+SECOND+0x5b]=1; break;
            case 18: wf(SOLO+SECOND+0x3c,1); break;
            case 19: wf(SOLO+SECOND+0x48,NAN); break;
            case 20: w64(SOLO+WORLD+0x10,addr(SOLO+FIRST+8)); break;
            case 21: w64(SOLO+SECOND+8,addr(SOLO+PAUSE+0x20)); break;
            case 22: strcpy((char *)arena+SOLO+WORLD+0xb8,"N_ItemBoxPos_00"); break;
            case 23: denied=addr(SOLO+SECOND); break;
            case 24: w64(COMMON,MAIN_BASE+0x1289538); break;
            case 25: strcpy((char *)arena+SOLO+RANK+0xb8,"Missing_Rank"); break;
            case 26: denied=addr(SOLO+RANK); break;
            case 27: wf(SOLO+RANK+0x44,1); break;
            case 28: strcpy((char *)arena+COMMON+GO+0xb8,"Missing_Go"); break;
            case 29: denied=addr(COMMON+GO); break;
            case 30: wf(COMMON+GO+0x44,1); break;
            case 31: strcpy((char *)arena+COMMON+RACE_NUM+0xb8,"Missing_RaceNum"); break;
            case 32: denied=addr(COMMON+RACE_NUM); break;
            case 33: wf(COMMON+RACE_NUM+0x44,1); break;
            }
            memcpy(before,arena,sizeof arena); assert(!mk8_ultrawide_hud_ready(false));
            assert(!memcmp(arena,before,sizeof arena));
            active=false; run(SOLO,false); assert(!diagnostics);
        }
        setup(); assert(mk8_ultrawide_hud_ready(false));  // ordinary transition rejection is not sticky
        return 0;
    }
    if (!strcmp(argv[1],"rank")) {
        arena[SOLO+RANK+0x58]=0x83; assert(mk8_ultrawide_hud_ready(false));
        for (unsigned i=0;i<100;i++) { run(SOLO,true); run(COMMON,true); }
        // Nested rank pulses/spins are left intact; the outer wrapper alone corrects the image.
        for (unsigned i=0;i<3;i++) {
            const float scales[]={.1f,1.1f,.8f};
            wf(SOLO+RANK_CHILD+0x48,scales[i]); wf(SOLO+RANK_CHILD+0x4c,scales[i]);
            wf(SOLO+RANK_CHILD+0x44,-180.f*i);
            assert(mk8_ultrawide_hud_ready(false)); run(SOLO,true);
        }
        arena[SOLO+RANK+0x58]=0x02; run(SOLO,true);
        assert(arena[SOLO+RANK+0x58]==0x02 && !diagnostics);
        return 0;
    }
    if (!strcmp(argv[1],"countdown")) {
        for (unsigned i=0;i<100;i++) run(COMMON,true);
        for (unsigned i=0;i<3;i++) {
            const float scales[]={3.6f,1.2f,1.f};
            arena[COMMON+COUNT1+0x59]=i ? 0 : 255;
            arena[COMMON+GO_PICTURE+0x59]=i ? 255 : 0;
            wf(COMMON+COUNT1+0x34,50.f*(i%2));
            wf(COMMON+GO_PICTURE+0x48,scales[i]); wf(COMMON+GO_PICTURE+0x4c,scales[i]);
            assert(mk8_ultrawide_hud_ready(false)); run(COMMON,true);
        }
        // The wrapper remains visible after its animated pictures fade out.
        arena[COMMON+COUNT1+0x59]=arena[COMMON+GO_PICTURE+0x59]=0;
        assert(mk8_ultrawide_hud_ready(false)); run(COMMON,true); assert(!diagnostics);
        return 0;
    }
    if (!strncmp(argv[1],"countdown-",10)) {
        if (!strcmp(argv[1],"countdown-missing")) strcpy((char *)arena+COMMON+GO+0xb8,"Missing_Go");
        else if (!strcmp(argv[1],"countdown-readonly")) denied=addr(COMMON+GO);
        else { assert(!strcmp(argv[1],"countdown-rotation")); wf(COMMON+GO+0x44,1); }
        run(COMMON,false); assert(diagnostics==1 && active && !mk8_ultrawide_hud_ready(false));
        return 0;
    }
    if (!strcmp(argv[1],"racenum")) {
        active=false; run(COMMON,false);
        assert(640.f+rf(COMMON+RACE_NUM+0x30)==640.f);
        assert(rf(COMMON+RACE_BASE+0x50)*rf(COMMON+RACE_NUM+0x48)==995.f);
        active=true;
        for (unsigned i=0;i<100;i++) run(COMMON,true);
        // The banner's own out animation rotates, scales, translates and fades its descendants.
        for (unsigned i=0;i<3;i++) {
            assert(mk8_ultrawide_hud_ready(false));
            wf(COMMON+RACE_NUM+0x30,12.f*i);
            wf(COMMON+RACE_ALL+0x38,75.f*i); wf(COMMON+RACE_ALL+0x3c,-30.f*i);
            wf(COMMON+RACE_ALL+0x48,1.f+.1f*i); wf(COMMON+RACE_ALL+0x4c,1.f+.1f*i);
            arena[COMMON+RACE_ALL+0x59]=(unsigned char)(255-100*i);
            run(COMMON,true);
        }
        // Visibility may change after camera preflight; the wrapper must use this traversal's state.
        arena[COMMON+RACE_NUM+0x58]=0x82; assert(mk8_ultrawide_hud_ready(false));
        arena[COMMON+RACE_NUM+0x58]=0x83; run(COMMON,true);
        arena[COMMON+RACE_NUM+0x58]=0x82; run(COMMON,true); assert(!diagnostics);
        return 0;
    }
    if (!strncmp(argv[1],"racenum-",8)) {
        if (!strcmp(argv[1],"racenum-missing")) strcpy((char *)arena+COMMON+RACE_NUM+0xb8,"Missing_RaceNum");
        else if (!strcmp(argv[1],"racenum-readonly")) denied=addr(COMMON+RACE_NUM);
        else { assert(!strcmp(argv[1],"racenum-rotation")); wf(COMMON+RACE_NUM+0x44,1); }
        run(COMMON,false); assert(diagnostics==1 && active && !mk8_ultrawide_hud_ready(false));
        return 0;
    }
    if (!strcmp(argv[1],"warnings")) {
        active=false; run(SOLO,false); active=true;
        for (unsigned i=0;i<100;i++) run(SOLO,true);
        const float positions[]={-661.283f,-300.f,0.f,300.f,661.283f};
        const float scales[]={.2f,.65f,1.2f,.85f,1.f};
        for (unsigned i=0;i<5;i++) {
            assert(mk8_ultrawide_hud_ready(false));
            wf(SOLO+ALARM_SCOPE0+0x30,positions[i]); wf(SOLO+ALARM_SCOPE1+0x30,-positions[i]);
            for (unsigned n=0;n<2;n++) {
                unsigned inner=SOLO+ALARM_INNER0+n*0x400;
                wf(inner+0x48,scales[i]); wf(inner+0x4c,scales[i]);
                arena[inner+0x59]=i==0 || i==4 ? 0 : i==2 ? 255 : 128;
            }
            run(SOLO,true);
        }
        // The first visible warning can follow camera preflight in the same frame.
        arena[SOLO+ALARM_INNER0+0x59]=arena[SOLO+ALARM_INNER1+0x59]=0;
        assert(mk8_ultrawide_hud_ready(false));
        arena[SOLO+ALARM_INNER0+0x59]=arena[SOLO+ALARM_INNER1+0x59]=255;
        run(SOLO,true);
        arena[SOLO+RANK+0x58]=0x83; run(SOLO,true);
        arena[SOLO+ALARM0+0x58]=arena[SOLO+ALARM1+0x58]=0x82;
        assert(mk8_ultrawide_hud_ready(false)); run(SOLO,true); assert(!diagnostics);
        return 0;
    }
    if (!strcmp(argv[1],"warning-guards")) {
        for (unsigned test=0;test<25;test++) {
            setup(); damage_second_warning(test);  // both inner warning alphas are still zero
            memcpy(before,arena,sizeof arena); assert(!mk8_ultrawide_hud_ready(false));
            assert(!memcmp(arena,before,sizeof arena));
        }
        setup(); assert(mk8_ultrawide_hud_ready(false) && !diagnostics);
        return 0;
    }
    if (!strncmp(argv[1],"warning-invalid-",16)) {
        unsigned test; assert(sscanf(argv[1]+16,"%u",&test)==1);
        damage_second_warning(test);
        // Even the valid first warning and earlier HUD targets must remain untouched.
        run(SOLO,false); assert(diagnostics==1 && active && !mk8_ultrawide_hud_ready(false));
        run(SOLO,false); run(COMMON,true); assert(diagnostics==1);
        return 0;
    }
    if (!strcmp(argv[1],"labels")) {
        active=false; reads=0; run(SOLO,false); assert(!reads); active=true;
        for (unsigned i=0;i<100;i++) run(SOLO,true);
        const float x[]={-2000.f,18.41271f,2000.f},y[]={-1500.f,72.65293f,1900.f};
        const float distance[]={.8f,.97299176f,1.f};
        for (unsigned i=0;i<3;i++) {
            assert(mk8_ultrawide_hud_ready(false));
            for (unsigned n=0;n<2;n++) {
                unsigned parts=SOLO+RIVAL0+n*0x400,inner=parts+0x100;
                wf(parts+0x30,n ? x[i] : -x[i]); wf(parts+0x34,n ? y[i] : -y[i]);
                wf(parts+0x48,distance[i]); wf(parts+0x4c,distance[i]);
                arena[parts+0x58]=0x83; arena[inner+0x59]=(unsigned char)(i*127);
                wf(parts+0x300+0x30,i==1 ? 48 : 80);
                arena[parts+0x200+0x59]=(unsigned char)(255-i*50);
            }
            // The projection writer does not clamp offscreen XY; those Parts remain valid.
            assert(mk8_ultrawide_hud_ready(false)); run(SOLO,true);
        }
        // Only the inner panes need write access; projected parents are read-only inputs.
        const unsigned parents[]={SOLO+WORLD,SOLO+RIVAL0,SOLO+RIVAL1};
        for (unsigned i=0;i<3;i++) {
            denied=addr(parents[i]); assert(mk8_ultrawide_hud_ready(false)); run(SOLO,true);
        }
        denied=0;
        arena[SOLO+RIVAL0+0x58]=arena[SOLO+RIVAL1+0x58]=0x02;
        arena[SOLO+RIVAL_INNER0+0x59]=arena[SOLO+RIVAL_INNER1+0x59]=0;
        assert(mk8_ultrawide_hud_ready(false));
        arena[SOLO+RIVAL1+0x58]=0x83; arena[SOLO+RIVAL_INNER1+0x59]=255; run(SOLO,true);
        arena[SOLO+RANK+0x58]=0x83; run(SOLO,true); assert(!diagnostics);
        return 0;
    }
    if (!strcmp(argv[1],"label-guards")) {
        for (unsigned test=0;test<23;test++) {
            setup(); arena[SOLO+RIVAL1+0x58]=0x02; arena[SOLO+RIVAL_INNER1+0x59]=0;
            damage_second_label(test);
            memcpy(before,arena,sizeof arena); assert(!mk8_ultrawide_hud_ready(false));
            assert(!memcmp(arena,before,sizeof arena));
        }
        setup(); assert(mk8_ultrawide_hud_ready(false) && !diagnostics);
        return 0;
    }
    if (!strncmp(argv[1],"label-invalid-",14)) {
        unsigned test; assert(sscanf(argv[1]+14,"%u",&test)==1);
        damage_second_label(test);
        // An invalid second chain must prevent writes to the first label, warnings and fixed HUD.
        run(SOLO,false); assert(diagnostics==1 && active && !mk8_ultrawide_hud_ready(false));
        run(SOLO,false); run(COMMON,true); assert(diagnostics==1);
        return 0;
    }
    if (!strcmp(argv[1],"finish")) {
        active=false; run(FINISH,false);
        assert(640.f+rf(FINISH+FIRST+0x30)==640.f);
        assert(690.f*rf(FINISH+FIRST+0x48)==690.f);
        active=true;
        for (unsigned i=0;i<100;i++) run(FINISH,true);
        for (unsigned i=0;i<3;i++) {
            const float scales[]={2.5f,1.f,1.1f};
            assert(mk8_ultrawide_hud_ready(false));
            wf(FINISH+SECOND+0x48,scales[i]); wf(FINISH+SECOND+0x4c,scales[i]);
            arena[FINISH+SECOND+0x59]=i==1 ? 255 : 0;
            run(FINISH,true);
        }
        // Finish can appear after this frame's camera preflight without a page change.
        arena[FINISH+FIRST+0x58]=0x82; assert(mk8_ultrawide_hud_ready(false));
        arena[FINISH+FIRST+0x58]=0x83; arena[FINISH+SECOND+0x59]=255; run(FINISH,true);
        arena[FINISH+FIRST+0x58]=0x82; run(FINISH,true);
        active=false; reads=0; run(FINISH,false); assert(!reads && !diagnostics);
        return 0;
    }
    if (!strcmp(argv[1],"finish-guards")) {
        for (unsigned test=0;test<7;test++) {
            setup(); finish_setup();
            switch (test) {
            case 0: wf(FINISH+PAUSE+0x48,.5f); break;
            case 1: strcpy((char *)arena+FINISH+FIRST+0xb8,"Missing_Finish"); break;
            case 2: denied=addr(FINISH+FIRST); break;
            case 3: wf(FINISH+FIRST+0x44,1); break;
            case 4: w64(TABLE+106*8,addr(OTHER)); break;
            case 5: w64(FINISH+0x20,addr(OTHER)); break;
            case 6: w64(FINISH+LAYOUT+0x80,addr(COMMON)); break;
            }
            memcpy(before,arena,sizeof arena); assert(!mk8_ultrawide_hud_ready(false));
            assert(!memcmp(arena,before,sizeof arena));
        }
        setup(); finish_setup(); assert(mk8_ultrawide_hud_ready(false) && !diagnostics);
        return 0;
    }
    if (!strncmp(argv[1],"finish-",7)) {
        if (!strcmp(argv[1],"finish-parent")) wf(FINISH+PAUSE+0x48,.5f);
        else if (!strcmp(argv[1],"finish-missing")) strcpy((char *)arena+FINISH+FIRST+0xb8,"Missing_Finish");
        else if (!strcmp(argv[1],"finish-readonly")) denied=addr(FINISH+FIRST);
        else if (!strcmp(argv[1],"finish-rotation")) wf(FINISH+FIRST+0x44,1);
        else if (!strcmp(argv[1],"finish-retired")) finish_page(OTHER);
        else if (!strcmp(argv[1],"finish-manager")) w64(FINISH+0x20,addr(OTHER));
        else { assert(!strcmp(argv[1],"finish-inactive")); w32(MANAGER+0xb50,2); }
        run(FINISH,false); assert(diagnostics==1 && active && !mk8_ultrawide_hud_ready(false));
        run(FINISH,false); run(COMMON,true); assert(diagnostics==1);
        return 0;
    }
    if (!strncmp(argv[1],"rank-",5)) {
        arena[SOLO+RANK+0x58]=0x83; assert(mk8_ultrawide_hud_ready(false));
        if (!strcmp(argv[1],"rank-missing")) strcpy((char *)arena+SOLO+RANK+0xb8,"Missing_Rank");
        else if (!strcmp(argv[1],"rank-readonly")) denied=addr(SOLO+RANK);
        else { assert(!strcmp(argv[1],"rank-rotation")); wf(SOLO+RANK+0x44,1); }
        run(SOLO,false); assert(diagnostics==1 && active && !mk8_ultrawide_hud_ready(false));
        return 0;
    }
    if (!strcmp(argv[1],"late")) {
        wf(SOLO+SECOND+0x3c,1); run(SOLO,false); assert(diagnostics==1 && active);
        run(SOLO,false); assert(diagnostics==1 && active);
        wf(SOLO+SECOND+0x3c,0); assert(!mk8_ultrawide_hud_ready(false));
        // Keep this frame's producer choice: the other HUD can still match the already widened world.
        run(COMMON,true); active=false; run(SOLO,false); assert(diagnostics==1);
        return 0;
    }
    if (!strcmp(argv[1],"retired")) {
        // A copied traversal can retain the old page after its same-vtable replacement is registered.
        page(OTHER,true); w64(MANAGER+0xb60,addr(OTHER));
        run(SOLO,false); assert(diagnostics==1 && active);
        run(OTHER,true); run(COMMON,true); assert(!mk8_ultrawide_hud_ready(false));
        return 0;
    }
    if (!strcmp(argv[1],"metadata")) {
        change_count=true; run(SOLO,false);
        assert(!change_count && diagnostics==1 && active && !mk8_ultrawide_hud_ready(false));
        return 0;
    }
    if (!strcmp(argv[1],"manager") || !strcmp(argv[1],"inactive")) {
        if (!strcmp(argv[1],"manager")) w64(SOLO+0x20,addr(OTHER));
        else w64(MANAGER+0xb60,addr(OTHER));
        run(SOLO,false); assert(diagnostics==1 && active);
        run(COMMON,true); assert(!mk8_ultrawide_hud_ready(false));
        return 0;
    }
    assert(!"unknown case");
}
'''


class UltrawideHudTests(unittest.TestCase):
    def fixture(self):
        return '#include "cpu.h"\n' + ultrawide_hud.ENTRY + '\n' + ultrawide_hud.BOUNDARY + '}\n'

    def test_scoped_traversal_and_boundary_rejection(self):
        original = self.fixture()
        patched = ultrawide_hud.patch(original)
        self.assertEqual(patched.count(ultrawide_hud.HOOK), 1)
        self.assertIn('c->pc = c->x[8]; c->x[30] = 0x7000732668ull;', patched)
        for broken in (patched, original.replace(ultrawide_hud.ENTRY, 'void other(Cpu *c) {'),
                       original.replace(ultrawide_hud.BOUNDARY, ''), original+original,
                       original.replace(ultrawide_hud.BOUNDARY, ultrawide_hud.BOUNDARY*2),
                       ultrawide_hud.ENTRY+'\n}\nvoid other(Cpu *c) {\n'+ultrawide_hud.BOUNDARY+'}\n'):
            with self.subTest(broken=broken[:60]), self.assertRaises(ValueError):
                ultrawide_hud.patch(broken)

    def test_unit_hash_checked_before_writing(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            src = base/'main'/ultrawide_hud.UNIT
            src.parent.mkdir()
            raw = self.fixture().encode()
            src.write_bytes(raw)
            output = base/'output'
            with self.assertRaises(ValueError):
                ultrawide_hud.patch_unit(src, output)
            self.assertFalse(output.exists())
            with mock.patch.object(ultrawide_hud, 'SHA256', hashlib.sha256(raw).hexdigest()):
                result = ultrawide_hud.patch_unit(src, output)
            self.assertEqual(src.read_bytes(), raw)
            self.assertEqual(result.read_text(), ultrawide_hud.patch(self.fixture()))
            other = base/'main_001.c'
            self.assertEqual(ultrawide_hud.patch_unit(other, output), other)

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_real_hud_wrapper_preflight_and_restoration(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            (out/'rt.h').write_text(RT_H, encoding='utf-8')
            (out/'harness.c').write_text(HARNESS, encoding='utf-8')
            exe = out/'hud.exe'
            built = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1',
                                    '-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-I'+str(out),
                                    str(out/'harness.c'), str(ROOT/'runtime/mk8/support/ultrawide_hud.c'),
                                    '-o', str(exe)], env=compiler_env(), capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout+built.stderr)
            cases = ('handoff', 'fade', 'fade-guards', 'normal', 'ratios', 'guards', 'late', 'retired', 'manager', 'inactive', 'metadata',
                         'rank', 'rank-missing', 'rank-readonly', 'rank-rotation',
                         'countdown', 'countdown-missing', 'countdown-readonly', 'countdown-rotation',
                         'racenum', 'racenum-missing', 'racenum-readonly', 'racenum-rotation',
                         'finish', 'finish-guards', 'finish-parent', 'finish-missing', 'finish-readonly',
                         'finish-rotation', 'finish-retired', 'finish-manager', 'finish-inactive',
                          'warnings', 'warning-guards', 'labels', 'label-guards',
                          'intro', 'intro-draw', 'intro-guards', 'intro-kind',
                          'intro-vs', 'intro-vs-draw', 'intro-vs-guards', 'intro-vs-kind',
                          'intro-edge', 'intro-vs-edge', 'intro-edge-guards', 'intro-vs-edge-guards')
            cases += tuple(f'intro-late-{i}' for i in (*range(7), 19, 20, 21, 22, 23, 27))
            cases += ('intro-late-66', 'fade-late-pc', 'fade-late-root')
            cases += tuple(f'{prefix}-late-{i}' for prefix in ('intro', 'intro-vs') for i in (67, 68))
            cases += tuple(f'fade-late-{i}' for i in (0, 1, 4, 12, 18, 19, 24, 30))
            cases += tuple(f'intro-vs-late-{i}' for i in (19, 20, 21, 22, 23, 27, *range(28, 37), 48, 49, *range(51, 67)))
            cases += tuple(f'warning-invalid-{i}' for i in range(25))
            cases += tuple(f'label-invalid-{i}' for i in range(23))
            for ratio in ('21', '32'):
                for case in cases:
                    with self.subTest(case=case, ratio=ratio):
                        result = subprocess.run([str(exe), case, ratio], capture_output=True, text=True, timeout=20)
                        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
