"""Exercise the prepared menu widgets, binding actions and persisted audio values."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolchain"))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), "Native compiler required")
class MenuAudioTests(unittest.TestCase):
    def test_video_and_controls_cover_catalog_and_binding_actions(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT / "upstream/smm2recomp/recomp", root / "host")
            menu = (runtime / "ui/menu.mm").read_text(encoding="utf-8")
            video_start = menu.rindex('extern "C" bool mk8_settings_values_main_menu(void);', 0, menu.index("static void tab_display("))
            video = menu[video_start:menu.index("static void tab_audio(")]
            controls = menu.index("static void tab_about(")
            (root / "catalog_pages.inc").write_text(video + menu[controls:menu.index("\n}", controls) + 2], encoding="utf-8")
            imgui = ROOT / "upstream/smm2recomp/recomp/third_party/imgui"
            exe = root / "widgets.exe"
            command = [DEFAULT_COMPILER, "c++", "-target", "x86_64-windows-gnu", "-O1", "-UNDEBUG",
                       "-I" + str(root), "-I" + str(runtime), "-I" + str(DEPS / "include"), "-I" + str(imgui),
                       ROOT / "tests/fixtures/menu_settings_catalog.cpp",
                       *(imgui / name for name in ("imgui.cpp","imgui_draw.cpp","imgui_widgets.cpp","imgui_tables.cpp")),
                       "-x", "c", runtime / "settings.c", runtime / "hle/bindings.c", ROOT / "runtime/mk8/support/joycon_modes.c", "-x", "none",
                       DEPS / "lib/libSDL3.dll.a", "-o", exe]
            env = dict(os.environ, LARECOMP_SETTINGS=str(root / "mk8d.ini"), PATH=str(DEPS / "bin") + os.pathsep + os.environ["PATH"])
            for missing in (False, True):
                if missing:
                    header = runtime / "settings_catalog.h"
                    text = header.read_text(encoding="utf-8")
                    marker = '    T(FULLSCREEN, VIDEO,'
                    self.assertEqual(text.count(marker),1)
                    header.write_text(text.replace(marker, '    T(SYNTHETIC, VIDEO, "Synthetic field", fullscreen, false, ALWAYS, "Test field.") \\\n' + marker), encoding="utf-8")
                subprocess.run(command, env=compiler_env(), check=True)
                result = subprocess.run([exe], cwd=root, env=env, text=True, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, int(missing), result.stderr)
                if missing:
                    self.assertIn("missing/duplicate field: Synthetic field", result.stderr)

    def test_quick_and_audio_widgets_share_the_native_value_and_range(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT / "upstream/smm2recomp/recomp", root / "host")
            hub = (runtime / "ui/mk8_hub.h").read_text(encoding="utf-8")
            start = hub.index("if (settings_page == MENU_QUICK) {") + len("if (settings_page == MENU_QUICK) {")
            quick = hub[start:hub.index("ImGui::TextWrapped", start)]
            menu = (runtime / "ui/menu.mm").read_text(encoding="utf-8")
            start = menu.index("static void tab_audio(")
            audio = menu[start:menu.index("\n}", start) + 2]
            rows = (runtime / "ui/option_rows.mm").read_text(encoding="utf-8")
            start = rows.index("bool opt_int(")
            integer_row = rows[start:rows.index("\n}", start) + 2]
            source = root / "menu.cpp"
            source.write_text(r'''
#include "settings_catalog.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
const char *const g_tunic_colours[N_TUNIC_COLOURS]={};
const char *const g_heart_colours[N_HEART_COLOURS]={};
void rt_log(const char *,...) {}
void mk8_settings_pads_changed(void) {}
int input_pad_family(void) { return 0; }
}
static bool g_want_fullscreen_apply;
static int seen[MK8_MENU_FIELD_COUNT],edit,shown;
static bool g_held;
static const char *tr(const char *en,const char *,const char *) { return en; }
static void toggle(const char *label,bool *value) {
    for(int id=0;id<MK8_MENU_FIELD_COUNT;id++) {
        const Mk8MenuOption *o=mk8_menu_option(id);
        if(o->toggle==value && !strcmp(o->label,label)) { ++seen[id]; return; }
    }
    assert(false);
}
static bool number(const char *label,int *value,int lo,int hi) {
    assert(!strcmp(label,mk8_menu_option(MK8_MENU_VOLUME)->label));
    assert(lo==0 && hi==200);
    ++seen[MK8_MENU_VOLUME]; shown=*value;
    if(edit<lo || edit>hi) return false;
    *value=edit; return true;
}
enum { ImGuiSliderFlags_AlwaysClamp=1 };
namespace ImGui {
static float GetFontSize() { return 34; }
static void SetNextItemWidth(float) {}
static bool Checkbox(const char *label,bool *value) { toggle(label,value); return false; }
static bool SliderInt(const char *label,int *value,int lo,int hi,const char *,int flags) {
    assert(flags&ImGuiSliderFlags_AlwaysClamp); return number(label,value,lo,hi);
}
}
static void opt_begin(const char *,float) {}
static void opt_heading(const char *label) { assert(!strcmp(label,"Sound")); }
static void opt_end() {}
static bool opt_toggle(const char *label,bool *value,bool,const char *) { toggle(label,value); return false; }
static int row(const char *label,const char *display,bool,const char *,bool,bool) {
    assert(!strcmp(label,mk8_menu_option(MK8_MENU_VOLUME)->label));
    ++seen[MK8_MENU_VOLUME]; shown=atoi(display);
    return edit;
}
''' + integer_row + "\nstatic void quick(float scale) {\n" + quick + "\n}\n" + audio + r'''
int main(int argc,char **argv) {
    assert(argc==3);
    bool advanced=!strcmp(argv[1],"audio"); edit=atoi(argv[2]);
    settings_load();
    const int before=mk8_settings_volume_percent();
    if(advanced) tab_audio(); else quick(1);
    assert(shown==before);
    for(int id=0;id<MK8_MENU_FIELD_COUNT;id++) {
        bool expected=advanced ? mk8_menu_option(id)->group==MK8_MENU_AUDIO :
            (id==MK8_MENU_SHOW_FPS || id==MK8_MENU_SHOW_MEMORY || id==MK8_MENU_FULLSCREEN ||
             id==MK8_MENU_MUTE_IN_BACKGROUND || id==MK8_MENU_VOLUME);
        assert(seen[id]==(int)expected);
    }
    printf("%d -> %d\n",shown,mk8_settings_volume_percent());
    settings_save();
}
''', encoding="utf-8")
            exe = root / "menu.exe"
            subprocess.run([DEFAULT_COMPILER, "c++", "-target", "x86_64-windows-gnu", "-O1", "-UNDEBUG",
                            "-I" + str(runtime), "-I" + str(DEPS / "include"), source, "-x", "c",
                            runtime / "settings.c", runtime / "hle/bindings.c", ROOT / "runtime/mk8/support/joycon_modes.c", "-x", "none",
                            DEPS / "lib/libSDL3.dll.a", "-o", exe], env=compiler_env(), check=True)
            ini = root / "mk8d.ini"
            ini.write_text("volume = 0.80\n", encoding="utf-8")
            env = dict(os.environ, LARECOMP_SETTINGS=str(ini), PATH=str(DEPS / "bin") + os.pathsep + os.environ["PATH"])
            for page, edit, expected in (("quick", 150, "80 -> 150"), ("audio", 0, "150 -> 150"),
                                         ("audio", 1, "150 -> 155"), ("quick", 137, "155 -> 137"),
                                         ("audio", 1, "137 -> 142"), ("quick", 200, "142 -> 200"),
                                         ("audio", 1, "200 -> 200"), ("quick", 145, "200 -> 145"),
                                         ("audio", 1, "145 -> 150"), ("quick", -1, "150 -> 150")):
                result = subprocess.check_output([exe, page, str(edit)], env=env, cwd=root, text=True)
                self.assertEqual(result.strip(), expected)
