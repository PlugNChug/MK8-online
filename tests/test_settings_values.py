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
            fallback=(runtime/'ui/menu.mm').read_text(encoding='utf-8')
            self.assertNotIn('Controllers start at player 2',fallback)
            self.assertNotIn('Joy-Con mode',fallback)
            (root/'test.c').write_text(r'''
#include "rt.h"
#include "settings.h"
#include "input_bind.h"
#include "settings_values.h"
#include "settings_catalog.h"
#include "nvn/shader_ahead.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
const char *const g_tunic_colours[N_TUNIC_COLOURS]={"Default"};
const char *const g_heart_colours[N_HEART_COLOURS]={"Default"};
static int windows,reboots,notices;
void mk8_publish_settings(bool overlay,uint64_t ticks);
void mk8_rumble_set_intensity(int percent) { g_settings.rumble_intensity=percent; }
void menu_notice(const char *message,double seconds) {
    assert(!strcmp(message,"Settings could not be saved. Check the settings folder, then reopen the Menu and close it to retry."));
    assert(seconds>0); ++notices;
}
void rt_request_reboot(int kind) { assert(kind==RT_REBOOT_GAME); reboots++; }
static int family_queries;
int input_pad_family(void) { family_queries++; return 0; }

void rt_log(const char *fmt,...) { (void)fmt; }
void mk8_settings_window_changed(void) { windows++; }
void renderer_get_scale(float *sx,float *sy) { *sx=.75f; *sy=.75f; }
double output_scale_dynamic_factor(void) { return .75; }
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
    for(unsigned g=0;g<SETTINGS_CATEGORIES;g++) for(int i=0;i<mk8_settings_values_count(g);i++) {
        Mk8Setting r; assert(mk8_settings_values_row(g,i,&r));
        assert(strlen(r.help)<=40); // Native footer text capacity, excluding the terminator.
    }
}
int main(int argc,char **argv) {
    assert(argc==3 || argc==4); settings_load();
    if(argc==4 && !strncmp(argv[3],"bloom:",6)) {
        assert(mk8_bloom_enabled()==(argv[3][6]=='1'));
        return 0;
    }
    if(argc==4 && !strncmp(argv[3],"deadzone:",9)) {
        assert(mk8_stick_deadzone_percent()==atoi(argv[3]+9));
        return 0;
    }
    assert(mk8_settings_input_native()==(g_settings.theme==THEME_NATIVE));
    mk8_settings_values_context(true); mk8_settings_values_sync();
    if(argc==4 && !strncmp(argv[3],"theme:",6)) {
        const char *name=argv[3]+6;
        assert(!strcmp(find(0,"Menu theme").value,name));
        assert(mk8_settings_style_active()==(g_settings.theme==THEME_NATIVE));
        return 0;
    }
    if(argc==4 && !strcmp(argv[3],"cross-platform")) {
        assert(g_settings.res_mode==4 && g_settings.frame_gen==FRAMEGEN_REPLAY);
        Mk8Setting fg=find(0,"Frame generation");
        assert(fg.current==1 && !strcmp(fg.value,"120 FPS"));
        assert(mk8_menu_choice_index(fg.id)==1);
        request(SETTINGS_BEGIN,0,0);
        request(SETTINGS_STEP,fg.id,1); // Already at Vulkan's highest choice.
        assert(g_settings.frame_gen==FRAMEGEN_REPLAY);
        request(SETTINGS_SET,find(1,"Master Volume").id,150);
        request(SETTINGS_FINISH,0,0); assert(mk8_settings_values_saved()==2);
        return 0;
    }
    assert(mk8_settings_values_count(SETTINGS_QUICK)==5);
    assert(find(SETTINGS_QUICK,"Master Volume").id==find(1,"Master Volume").id);
    assert(find(SETTINGS_QUICK,"Show FPS").id==find(0,"Show FPS").id);
    assert(g_bind_key[BIND_A]==SDL_SCANCODE_F10);
    if(argc==4) {
        assert(g_settings.aspect==ASPECT_ULTRAWIDE && g_settings.theme==THEME_NATIVE && mk8_settings_style_active());
        assert(g_settings.volume==1.5f && g_settings.show_fps && g_settings.fullscreen);
        Mk8Setting volume=find(1,"Master Volume");
        assert(volume.current==150 && !strcmp(volume.value,"150%"));
        assert(mk8_settings_camera_distance()==117);
        assert(mk8_stick_deadzone_percent()==17);
        assert(mk8_split_screen_60fps_enabled());
        assert(!mk8_bloom_enabled());
        assert(g_settings.res_mode==RES_Q75 && g_settings.antialias==AA_SMAA);
        assert(g_bind_key[BIND_ZL]==SDL_SCANCODE_SPACE && g_bind_key[BIND_ZR]==SDL_SCANCODE_LALT);
        return 0;
    }
    assert(mk8_settings_values_count(0)==15 && mk8_settings_values_count(1)==4 && mk8_settings_values_count(2)==22);
    Mk8Setting video_row;
    assert(mk8_settings_values_row(MK8_MENU_VIDEO,0,&video_row) && video_row.id==MK8_MENU_THEME);
    assert(mk8_settings_values_row(MK8_MENU_VIDEO,1,&video_row) && video_row.id==MK8_MENU_ASPECT);
    for(int id=0;id<MK8_MENU_FIELD_COUNT;id++) {
        const Mk8MenuOption *o=mk8_menu_option(id);
        Mk8Setting row=find(o->group,o->label);
        assert(row.id==id && row.enabled==mk8_menu_option_enabled(id,true));
    }
    Mk8Setting camera=find(0,"Driving camera distance");
    assert(camera.kind==SETTING_NUMBER && camera.minimum==75 && camera.maximum==150);
    assert(camera.current==100 && mk8_settings_camera_distance()==100 && !strcmp(camera.value,"100%"));
    for(int i=0;i<mk8_settings_values_count(2);i++) {
        Mk8Setting row; assert(mk8_settings_values_row(2,i,&row));
        assert(strcmp(row.label,"Controllers start at P2") && strcmp(row.label,"Joy-Con mode"));
    }
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
    assert(!mk8_settings_values_row(SETTINGS_CATEGORIES,0,&(Mk8Setting){0}));
    assert(!mk8_settings_values_request(SETTINGS_SET,find(1,"Master Volume").id,201));
    assert(!mk8_settings_values_request(SETTINGS_SET,1,0));
    assert(!mk8_settings_values_request(SETTINGS_ACTIVATE,999,0));
    int before=family_queries;
    assert(before>0);
    mk8_settings_values_sync();
    assert(family_queries==before);
    request(SETTINGS_BEGIN,0,0);
    assert(family_queries>before);
    assert(!strcmp(find(0,"Internal resolution").help,"Rendering 1440x810."));
    assert(*find(2,"Mapping device").help);
    for (unsigned i=0;i<sizeof mk8_menu_quick/sizeof *mk8_menu_quick;i++) {
        Mk8Setting shortcut; assert(mk8_settings_values_row(SETTINGS_QUICK,i,&shortcut));
        assert(shortcut.id==mk8_menu_quick[i].id);
        Mk8Setting full=find(mk8_menu_option(shortcut.id)->group,shortcut.label);
        assert(!memcmp(&shortcut,&full,sizeof full));
    }
    int theme=find(0,"Menu theme").id;
    const char *labels[]={"Native","Simple Dark","Simple Light","NES Dark","NES Light"};
    const char *tokens[]={"Native","Default","SimpleLight","NES","NESLight"};
    assert(g_settings.theme==THEME_NATIVE);
    request(SETTINGS_STEP,theme,1); request(SETTINGS_STEP,theme,-1);
    for(int i=0;i<5;i++) {
        if(i) request(SETTINGS_STEP,theme,1);
        assert(!strcmp(find(0,"Menu theme").value,labels[i]));
        assert(!strcmp(g_theme_names[g_settings.theme],tokens[i]));
        assert(mk8_settings_style_active()==(i==0));
        assert(mk8_menu_light()==(i==2 || i==4));
        request(SETTINGS_FINISH,0,0); assert(mk8_settings_values_saved()==2);
        FILE *saved=fopen(argv[1],"rt"); assert(saved);
        char contents[4096]={0},expected[48];
        assert(fread(contents,1,sizeof contents-1,saved)>0); fclose(saved);
        snprintf(expected,sizeof expected,"theme = %s\n",tokens[i]); assert(strstr(contents,expected));
        request(SETTINGS_BEGIN,0,0);
    }
    for(int i=4;i>0;i--) request(SETTINGS_STEP,theme,-1);
    Mk8Setting rate=find(0,"Split-screen 60 FPS");
    assert(rate.id==MK8_MENU_SPLIT_SCREEN_60FPS && !mk8_split_screen_60fps_enabled() && !strcmp(rate.value,"Off"));
    request(SETTINGS_STEP,rate.id,1);
    assert(mk8_split_screen_60fps_enabled() && !strcmp(find(0,"Split-screen 60 FPS").value,"On"));
    Mk8Setting bloom=find(0,"Bloom");
    assert(bloom.id==MK8_MENU_BLOOM && mk8_bloom_enabled() && !strcmp(bloom.value,"On"));
    request(SETTINGS_STEP,bloom.id,1);
    assert(!mk8_bloom_enabled() && !strcmp(find(0,"Bloom").value,"Off"));
    Mk8Setting rumble=find(2,"Rumble intensity");
    assert(rumble.id==MK8_MENU_RUMBLE_INTENSITY && rumble.kind==SETTING_NUMBER && !strcmp(rumble.value,"100%"));
    request(SETTINGS_STEP,rumble.id,-1);
    assert(g_settings.rumble_intensity==95);
    assert(!mk8_settings_values_request(SETTINGS_SET,rumble.id,101));
    request(SETTINGS_SET,rumble.id,37);
    assert(g_settings.rumble_intensity==37 && !strcmp(find(2,"Rumble intensity").value,"37%"));
    request(SETTINGS_SET,rumble.id,0);
    assert(!g_settings.rumble_intensity && !strcmp(find(2,"Rumble intensity").value,"0%"));
    Mk8Setting deadzone=find(2,"Stick dead zone");
    assert(deadzone.id==MK8_MENU_STICK_DEADZONE && deadzone.kind==SETTING_NUMBER);
    assert(deadzone.current==0 && deadzone.minimum==0 && deadzone.maximum==50);
    assert(!mk8_settings_values_request(SETTINGS_SET,deadzone.id,51));
    request(SETTINGS_STEP,deadzone.id,1);
    assert(mk8_stick_deadzone_percent()==1);
    request(SETTINGS_SET,deadzone.id,17);
    assert(!strcmp(find(2,"Stick dead zone").value,"17%"));
    int aspect=find(0,"Aspect ratio").id;
    int scaler=find(0,"Upscaler").id;
    assert(find(0,"Upscaler").maximum==1);
    assert(!_putenv_s("MK8_READY_FSR3","1"));
    request(SETTINGS_STEP,scaler,1); assert(g_settings.upscaler==UPSCALE_FSR3);
    assert(!strcmp(find(0,"Upscaler").value,"AMD FSR 3.1.6"));
    request(SETTINGS_STEP,scaler,1); assert(g_settings.upscaler==UPSCALE_FSR3);
    assert(!_putenv_s("MK8_READY_DLSS","1"));
    mk8_publish_settings(true,1000);
    request(SETTINGS_STEP,scaler,1); assert(g_settings.upscaler==UPSCALE_DLSS);
    mk8_publish_settings(true,1010);
    mk8_publish_settings(true,1400);
    live=fopen(argv[1],"rb"); assert(live);
    memset(live_text,0,sizeof live_text); assert(fread(live_text,1,sizeof live_text-1,live)>0);
    fclose(live); assert(strstr(live_text,"upscaler = dlss"));
    assert(!_putenv_s("MK8_READY_DLSS",""));
    mk8_settings_values_sync();
    assert(!strcmp(find(0,"Upscaler").value,"NVIDIA DLSS SR"));
    request(SETTINGS_STEP,scaler,-1); assert(g_settings.upscaler==UPSCALE_FSR3);
    assert(!_putenv_s("MK8_READY_FSR4","1"));
    request(SETTINGS_STEP,scaler,1); assert(g_settings.upscaler==UPSCALE_FSR4);
    request(SETTINGS_STEP,scaler,-1); assert(g_settings.upscaler==UPSCALE_FSR3);
    assert(!_putenv_s("MK8_READY_FSR4",""));
    request(SETTINGS_STEP,scaler,-1); assert(g_settings.upscaler==UPSCALE_FSR1);
    assert(!_putenv_s("MK8_READY_FSR3",""));
    request(SETTINGS_STEP,camera.id,1);
    assert(find(0,"Driving camera distance").current==110);
    request(SETTINGS_SET,camera.id,78);
    request(SETTINGS_STEP,camera.id,-1);
    assert(find(0,"Driving camera distance").current==75);
    request(SETTINGS_SET,camera.id,147);
    request(SETTINGS_STEP,camera.id,1);
    assert(find(0,"Driving camera distance").current==150);
    assert(!mk8_settings_values_request(SETTINGS_SET,camera.id,74));
    assert(!mk8_settings_values_request(SETTINGS_SET,camera.id,151));
    request(SETTINGS_SET,camera.id,117);
    assert(mk8_settings_camera_distance()==117);
    request(SETTINGS_STEP,aspect,1); assert(g_settings.aspect==ASPECT_ORIGINAL);
    request(SETTINGS_STEP,aspect,1); assert(g_settings.aspect==ASPECT_ULTRAWIDE);
    assert(find(0,"Aspect ratio").maximum==2 && !strcmp(find(0,"Aspect ratio").value,"Ultrawide (experimental)"));
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
    Mk8Setting master=find(1,"Master Volume");
    assert(master.minimum==0 && master.maximum==200 && master.current==100);
    request(SETTINGS_SET,master.id,198);
    request(SETTINGS_STEP,master.id,1); assert(g_settings.volume==2.0f);
    request(SETTINGS_SET,master.id,2);
    request(SETTINGS_STEP,master.id,-1); assert(g_settings.volume==0);
    request(SETTINGS_SET,find(1,"Master Volume").id,80); assert(g_settings.volume==.8f);
    request(SETTINGS_STEP,find(1,"Movie audio").id,1); assert(!g_settings.movie_audio);
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
    request(SETTINGS_SET,master.id,150);
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
                str(runtime/'hle/bindings.c'),str(deps/'lib/libSDL3.dll.a'),'-o',str(exe)],
                env=compiler_env(),capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr[-4000:])
            ini=root/'settings.ini'
            ini.write_text('pads_from_p2 = 1\r\njoycon_mode = horizontal\r\nbind_key_a = F10\r\nbind_key_zl = Left Alt\r\nbind_key_zr = Space\r\n',encoding='utf-8',newline='')
            env=dict(os.environ,LARECOMP_SETTINGS=str(ini),
                     PATH=str(deps/'bin')+os.pathsep+os.environ.get('PATH',''))
            env.pop('MK8_GAMEPAD_LAYOUT', None)
            result=subprocess.run([str(exe),str(ini),str(root/'missing/settings.ini')],env=env,capture_output=True,text=True,timeout=25)
            self.assertEqual(result.returncode,0,result.stderr[-2000:])
            reload=subprocess.run([str(exe),str(ini),str(root/'missing/settings.ini'),'reload'],env=env,capture_output=True,text=True,timeout=25)
            self.assertEqual(reload.returncode,0,reload.stderr[-2000:])
            probe = root/'bloom.ini'
            for value,expected in [('0',0), ('0  \t',0), ('1',1), ('',1), ('off',1), ('00',1), ('0junk',1)]:
                probe.write_bytes(f'bloom = {value}\r\n'.encode())
                subprocess.run([exe,probe,root/'missing/settings.ini',f'bloom:{expected}'],
                               env={**env,'LARECOMP_SETTINGS':str(probe)},check=True,timeout=25)
            for value,expected in [('50  \t',50), ('51',0), ('-1',0), ('17.5',0), ('',0)]:
                probe.write_bytes(f'stick_deadzone = {value}\r\n'.encode())
                subprocess.run([exe,probe,root/'missing/settings.ini',f'deadzone:{expected}'],
                               env={**env,'LARECOMP_SETTINGS':str(probe)},check=True,timeout=25)
            text=ini.read_text()
            self.assertIn('theme = Native',text)
            self.assertIn('aspect = ultrawide',text)
            self.assertIn('volume = 1.50',text)
            self.assertIn('split_screen_60fps = 1',text)
            self.assertIn('bloom = 0',text)
            self.assertIn('controller_vibration = 0',text)
            self.assertIn('rumble_intensity = 0',text)
            self.assertIn('camera_distance = 117',text)
            self.assertIn('stick_deadzone = 17',text)
            self.assertNotIn('joycon_mode',text)
            self.assertNotIn('pads_from_p2',text)
            self.assertIn('bind_key_zl = Space',text)
            self.assertIn('bind_key_a = F10',text)
            self.assertIn('bind_key_zr = Left Alt',text)
            check=root/'theme.ini'
            for token,label in [(None,'Native'), ('','Native'), ('Future','Simple Dark'), ('Native','Native'),
                                ('Default','Simple Dark'), ('SimpleLight','Simple Light'), ('NES','NES Dark'), ('NESLight','NES Light'),
                                ('NESLight  \t','NES Light'), ('NES junk','Simple Dark')]:
                with self.subTest(theme=token):
                    content=None if token is None else f'theme = {token}\n'.encode()
                    if content is not None: check.write_bytes(content)
                    subprocess.run([exe,str(check),str(root/'missing/settings.ini'),'theme:'+label],
                                   env={**env,'LARECOMP_SETTINGS':str(check)},check=True,timeout=25)
                    self.assertEqual(check.read_bytes() if check.exists() else None,content)
            legacy=root/'legacy.ini'
            legacy.write_text('resolution = 8x\nframe_gen = replay\n',encoding='utf-8')
            subprocess.run([exe,str(legacy),str(root/'missing/settings.ini'),'cross-platform'],
                           env={**env,'LARECOMP_SETTINGS':str(legacy)},check=True,timeout=25)
            text=legacy.read_text()
            self.assertIn('resolution = 4x',text)
            self.assertIn('frame_gen = replay',text)
            self.assertIn('volume = 1.50',text)


if __name__=='__main__': unittest.main()
