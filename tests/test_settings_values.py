"""Exercise real settings, bindings and INI persistence through the native menu bridge."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER,compiler_env
from host import DEPS,prepare_host_sources


@unittest.skipUnless(DEFAULT_COMPILER.is_file(),'Native compiler required')
class SettingsValuesTests(unittest.TestCase):
    def test_real_settings_remaps_checked_save_and_reload(self):
        deps=DEPS
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            runtime=prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',root/'host')
            (root/'test.c').write_text(r'''
#include "rt.h"
#include "settings.h"
#include "input_bind.h"
#include "settings_values.h"
#include "nvn/shader_ahead.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
const char *const g_tunic_colours[N_TUNIC_COLOURS]={"Default"};
const char *const g_heart_colours[N_HEART_COLOURS]={"Default"};
static int windows,pads,reboots,notices;
void mk8_publish_settings(bool overlay,uint64_t ticks);
void menu_notice(const char *message,double seconds) {
    assert(!strcmp(message,"Settings could not be saved. Check the settings folder, then reopen Settings and go Back to retry."));
    assert(seconds>0); ++notices;
}
void rt_request_reboot(int kind) { assert(kind==RT_REBOOT_GAME); reboots++; }
static int family_queries;
int input_pad_family(void) { family_queries++; return 0; }
bool mk8_settings_style_active(void) { return true; }
void rt_log(const char *fmt,...) { (void)fmt; }
void mk8_settings_window_changed(void) { windows++; }
void mk8_settings_pads_changed(void) { pads++; }
AheadStatus shader_ahead_status(void) { return (AheadStatus){2,10,10}; }
void rt_on_reset(void (*fn)(void)) { (void)fn; }
static Mk8Setting find(unsigned group,const char *label) {
    for(int i=0;i<mk8_settings_values_count(group);i++) {
        Mk8Setting r; assert(mk8_settings_values_row(group,i,&r));
        if(!strcmp(r.label,label)) return r;
    }
    assert(0); return (Mk8Setting){0};
}
static void request(int op,int id,int value) {
    assert(mk8_settings_values_request(op,id,value)); mk8_settings_values_sync();
}
static void check_tips(void) {
    for(unsigned g=0;g<3;g++) for(int i=0;i<mk8_settings_values_count(g);i++) {
        Mk8Setting r; assert(mk8_settings_values_row(g,i,&r));
        assert(strlen(r.help)<=40); // Native footer text capacity, excluding the terminator.
    }
}
int main(int argc,char **argv) {
    assert(argc==3 || argc==4); settings_load(); mk8_settings_values_context(true); mk8_settings_values_sync();
    if(argc==4) {
        assert(g_settings.aspect==ASPECT_ULTRAWIDE);
        assert(g_settings.joycon_mode==1);
        assert(g_settings.volume==.65f && g_settings.show_fps && g_settings.fullscreen);
        assert(g_settings.res_mode==RES_Q75 && g_settings.antialias==AA_SMAA);
        assert(g_bind_key[BIND_ZL]==SDL_SCANCODE_SPACE && g_bind_key[BIND_ZR]==SDL_SCANCODE_LALT);
        return 0;
    }
    assert(mk8_settings_values_count(0)==11 && mk8_settings_values_count(1)==4 && mk8_settings_values_count(2)==22);
    check_tips();
    assert(!strcmp(find(0,"Aspect ratio").value,"Stretch"));
    // The launcher may read the INI while the overlay remains open. A drag
    // publishes its settled value, and an idle menu never rewrites the file.
    mk8_publish_settings(true,100);
    g_settings.volume=1.50f;
    mk8_publish_settings(true,120);
    g_settings.volume=1.75f;
    mk8_publish_settings(true,200);
    mk8_publish_settings(true,550);
    FILE *live=fopen(argv[1],"rb"); assert(live);
    char live_text[4096]={0}; assert(fread(live_text,1,sizeof live_text-1,live)>0);
    fclose(live); assert(strstr(live_text,"volume = 1.75"));
    g_settings.volume=1.0f;
    mk8_publish_settings(false,570);
    assert(!mk8_settings_values_row(3,0,&(Mk8Setting){0}));
    assert(!mk8_settings_values_request(SETTINGS_SET,11,201));
    assert(!mk8_settings_values_request(SETTINGS_SET,1,0));
    assert(!mk8_settings_values_request(SETTINGS_ACTIVATE,999,0));
    int before=family_queries;
    assert(before>0);
    mk8_settings_values_sync();
    assert(family_queries==before);
    request(SETTINGS_BEGIN,0,0);
    assert(family_queries>before);
    int aspect=find(0,"Aspect ratio").id;
    int scaler=find(0,"Upscaler").id;
    assert(find(0,"Upscaler").maximum==1);
    assert(!_putenv_s("MK8_READY_FSR3","1"));
    request(SETTINGS_STEP,scaler,1); assert(g_settings.upscaler==UPSCALE_FSR3);
    assert(!strcmp(find(0,"Upscaler").value,"AMD FSR 3.1.6"));
    request(SETTINGS_STEP,scaler,1); assert(g_settings.upscaler==UPSCALE_FSR3);
    request(SETTINGS_STEP,scaler,-1); assert(g_settings.upscaler==UPSCALE_FSR1);
    assert(!_putenv_s("MK8_READY_FSR3",""));
    request(SETTINGS_STEP,aspect,1); assert(g_settings.aspect==ASPECT_ORIGINAL);
    request(SETTINGS_STEP,aspect,1); assert(g_settings.aspect==ASPECT_ULTRAWIDE);
    assert(find(0,"Aspect ratio").maximum==2 && !strcmp(find(0,"Aspect ratio").value,"Ultrawide (experimental)"));
    request(SETTINGS_STEP,aspect,1); assert(g_settings.aspect==ASPECT_ULTRAWIDE);
    check_tips();
    mk8_settings_values_context(false); mk8_settings_values_sync();
    assert(!find(0,"Skip intro").enabled && !find(0,"Full-speed shaders").enabled);
    assert(!find(2,"Game language").enabled);
    assert(!strcmp(find(2,"Game language").help,"Exit the race to modify."));
    int language=g_settings.language;
    request(SETTINGS_STEP,find(2,"Game language").id,1); assert(g_settings.language==language);
    request(SETTINGS_RESTART,0,1); assert(!reboots);
    mk8_settings_values_context(true);
    assert(mk8_settings_values_request(SETTINGS_STEP,find(2,"Game language").id,1));
    mk8_settings_values_context(false); mk8_settings_values_sync(); assert(g_settings.language==language);
    mk8_settings_values_context(true); mk8_settings_values_sync();
    request(SETTINGS_STEP,find(0,"Show FPS").id,1); assert(g_settings.show_fps);
    request(SETTINGS_STEP,find(0,"Fullscreen").id,1); assert(g_settings.fullscreen && windows==1);
    request(SETTINGS_STEP,find(0,"Internal resolution").id,-1); assert(g_settings.res_mode==RES_Q75);
    assert(find(0,"Upscaler").enabled);
    request(SETTINGS_STEP,find(0,"Upscaler").id,-1); assert(g_settings.upscaler==UPSCALE_BILINEAR);
    request(SETTINGS_STEP,find(0,"Anti-aliasing").id,1); assert(g_settings.antialias==AA_SMAA);
    request(SETTINGS_STEP,find(0,"Frame generation").id,1); assert(g_settings.frame_gen==FRAMEGEN_120);
    request(SETTINGS_SET,find(1,"Master Volume").id,200); assert(g_settings.volume==2.0f);
    request(SETTINGS_SET,find(1,"Master Volume").id,80); assert(g_settings.volume==.8f);
    request(SETTINGS_STEP,find(1,"Movie audio").id,1); assert(!g_settings.movie_audio);
    request(SETTINGS_STEP,find(2,"Controllers start at P2").id,1); assert(g_settings.pads_from_p2 && pads==1);
    int joycons=find(2,"Joy-Con mode").id;
    assert(g_settings.joycon_mode==0);
    request(SETTINGS_STEP,joycons,1);
    assert(g_settings.joycon_mode==1 && !SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS,true));
    assert(pads==2);
    assert(mk8_set_joycon_mode(1) && pads==2);
    assert(!SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS,true));
    request(SETTINGS_STEP,joycons,-1);
    assert(g_settings.joycon_mode==0 && SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS,false));
    assert(pads==3);
    assert(mk8_set_joycon_mode(0) && pads==3);
    request(SETTINGS_STEP,joycons,1);
    request(SETTINGS_STEP,find(2,"Game language").id,1); assert(g_settings.language==1);
    request(SETTINGS_STEP,find(2,"Button icons").id,1); assert(g_settings.prompts==1);
    request(SETTINGS_STEP,find(2,"Mapping device").id,1); assert(mk8_settings_values_count(2)==30);
    check_tips();
    assert(g_bind_key[BIND_ZL]==SDL_SCANCODE_LALT && g_bind_key[BIND_ZR]==SDL_SCANCODE_SPACE);
    request(SETTINGS_ACTIVATE,find(2,"ZL").id,0); assert(mk8_settings_values_capturing());
    SDL_Event e={0}; e.type=SDL_EVENT_KEY_DOWN; e.key.scancode=SDL_SCANCODE_SPACE;
    assert(bind_capture_event(&e)); mk8_settings_values_sync();
    assert(!mk8_settings_values_capturing());
    assert(g_bind_key[BIND_ZL]==SDL_SCANCODE_SPACE && g_bind_key[BIND_ZR]==SDL_SCANCODE_LALT);
    request(SETTINGS_FINISH,0,0); assert(mk8_settings_values_saved()==2);
    request(SETTINGS_BEGIN,0,0);
    request(SETTINGS_SET,find(1,"Master Volume").id,65);
    assert(!_putenv_s("LARECOMP_SETTINGS",argv[2]));
    request(SETTINGS_FINISH,0,0); assert(mk8_settings_values_saved()==-1 && g_settings.volume==.65f);
    assert(!_putenv_s("LARECOMP_SETTINGS",argv[1]));
    FILE *held=fopen(argv[1],"rb"); assert(held);
    request(SETTINGS_FINISH,0,0); assert(mk8_settings_values_saved()==-1);
    char previous[4096]={0}; assert(fread(previous,1,sizeof previous-1,held)>0);
    assert(strstr(previous,"volume = 0.80")); fclose(held);
    request(SETTINGS_FINISH,0,0); assert(mk8_settings_values_saved()==2);
    request(SETTINGS_BEGIN,0,0);
    for(int i=0;i<32;i++) assert(mk8_settings_values_request(SETTINGS_STEP,find(2,"Mapping device").id,1));
    assert(!mk8_settings_values_request(SETTINGS_FINISH,0,0));
    mk8_settings_values_sync(); request(SETTINGS_FINISH,0,0); assert(mk8_settings_values_saved()==2);
    // Start Race can end a lobby edit while every ordinary queue slot is occupied.
    assert(!notices);
    request(SETTINGS_BEGIN,0,0);
    int volume=find(1,"Master Volume").id;
    for(int i=0;i<32;i++) assert(mk8_settings_values_request(SETTINGS_SET,volume,69));
    assert(!mk8_settings_values_request(SETTINGS_FINISH,0,0));
    assert(mk8_settings_values_request(SETTINGS_FINISH,0,SETTINGS_DETACHED));
    assert(mk8_settings_values_request(SETTINGS_FINISH,0,SETTINGS_DETACHED));
    assert(mk8_settings_values_saved()==1);
    held=fopen(argv[1],"rb"); assert(held);
    mk8_settings_values_sync();
    assert(mk8_settings_values_saved()==-1 && notices==1 && g_settings.volume==.69f);
    memset(previous,0,sizeof previous);
    assert(fread(previous,1,sizeof previous-1,held)>0 && strstr(previous,"volume = 0.65"));
    fclose(held);
    // The retry must persist the unsaved value even without another edit.
    request(SETTINGS_BEGIN,0,0); request(SETTINGS_FINISH,0,0);
    assert(mk8_settings_values_saved()==2 && notices==1);
    held=fopen(argv[1],"rb"); assert(held); memset(previous,0,sizeof previous);
    assert(fread(previous,1,sizeof previous-1,held)>0 && strstr(previous,"volume = 0.69"));
    fclose(held);
    request(SETTINGS_BEGIN,0,0);
    request(SETTINGS_SET,find(1,"Master Volume").id,66);
    assert(!_putenv_s("LARECOMP_SETTINGS",argv[2]));
    request(SETTINGS_FINISH,0,1); assert(!reboots && mk8_settings_values_saved()==-1);
    request(SETTINGS_RESTART,0,1); assert(!reboots && mk8_settings_values_saved()==-1);
    assert(!_putenv_s("LARECOMP_SETTINGS",argv[1]));
    request(SETTINGS_SET,find(1,"Master Volume").id,65);
    request(SETTINGS_FINISH,0,1); assert(!reboots && mk8_settings_values_saved()==3);
    request(SETTINGS_RESTART,0,0); assert(!reboots && mk8_settings_values_saved()==2);
    request(SETTINGS_BEGIN,0,0); request(SETTINGS_FINISH,0,1); assert(mk8_settings_values_saved()==3);
    mk8_settings_values_context(false); request(SETTINGS_RESTART,0,1); assert(!reboots && mk8_settings_values_saved()==2);
    mk8_settings_values_context(true); request(SETTINGS_BEGIN,0,0);
    request(SETTINGS_STEP,find(2,"Game language").id,-1);
    request(SETTINGS_FINISH,0,1); assert(mk8_settings_values_saved()==2); // Reverting to the running settings needs no restart.
    request(SETTINGS_BEGIN,0,0); request(SETTINGS_STEP,find(2,"Game language").id,1);
    request(SETTINGS_FINISH,0,1); assert(mk8_settings_values_saved()==3);
    request(SETTINGS_RESTART,0,1); assert(reboots==1 && mk8_settings_values_saved()==4);
}
''',encoding='utf-8')
            exe=root/'test.exe'
            result=subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O2','-UNDEBUG',
                '-I'+str(runtime),'-I'+str(ROOT/'runtime/mk8/support'),'-I'+str(deps/'include'),
                str(root/'test.c'),str(ROOT/'runtime/mk8/support/settings_values.c'),str(runtime/'settings.c'),
                str(ROOT/'runtime/mk8/support/joycon_modes.c'),str(runtime/'hle/bindings.c'),str(deps/'lib/libSDL3.dll.a'),'-o',str(exe)],
                env=compiler_env(),capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr[-4000:])
            ini=root/'settings.ini'
            ini.write_text('bind_key_zl = Left Alt\r\nbind_key_zr = Space\r\n',encoding='utf-8',newline='')
            env=dict(os.environ,LARECOMP_SETTINGS=str(ini),
                     PATH=str(deps/'bin')+os.pathsep+os.environ.get('PATH',''))
            env.pop('MK8_GAMEPAD_LAYOUT', None)
            result=subprocess.run([str(exe),str(ini),str(root/'missing/settings.ini')],env=env,capture_output=True,text=True,timeout=25)
            self.assertEqual(result.returncode,0,result.stderr[-2000:])
            reload=subprocess.run([str(exe),str(ini),str(root/'missing/settings.ini'),'reload'],env=env,capture_output=True,text=True,timeout=25)
            self.assertEqual(reload.returncode,0,reload.stderr[-2000:])
            text=ini.read_text()
            self.assertIn('aspect = ultrawide',text)
            self.assertIn('volume = 0.65',text)
            self.assertIn('joycon_mode = horizontal',text)
            self.assertIn('bind_key_zl = Space',text)
            self.assertIn('bind_key_zr = Left Alt',text)


if __name__=='__main__': unittest.main()
