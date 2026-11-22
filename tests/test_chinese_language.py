"""Check the game's language IPC reply without changing the saved or system language."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(os.name == 'nt' and DEFAULT_COMPILER.is_file(), 'Windows native compiler required')
class GameLanguageTests(unittest.TestCase):
    def test_persisted_language_and_ipc_replies(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', out)
            am = (runtime / 'services/am.c').read_text(encoding='utf-8').split('static void application_functions(')[1]
            desired = am[am.index('case 21:'):am.index('case 23:')]
            system = (runtime / 'services/hid_set.c').read_text(encoding='utf-8')
            system = system[system.index('static const char *const languages[]'):system.index('SERVICE("set:sys"')]
            source = out / 'language.c'
            source.write_text('''#include "hle.h"
#include "ipc.h"
#include "settings.h"
#include "input_bind.h"
#include <string.h>
#include <assert.h>
const char *const g_tunic_colours[N_TUNIC_COLOURS]={"Default"};
const char *const g_heart_colours[N_HEART_COLOURS]={"Default"};
bool bind_settings_load(const char *k,const char *v) { return false; }
void bind_settings_save(FILE *f) {}
void rt_log(const char *fmt,...) {}
static void desired_language(unsigned cmd,IpcResp *resp) { switch(cmd) {
''' + desired + '} }\n' + system + '''
int main(int argc,char **argv) {
    assert(argc==3);
    settings_load();
    IpcReq req={0};
    IpcResp sys={0}, app={0};
    char system_code[8]={0}, game_code[8]={0};
    assert(strlen(argv[1])<8 && strlen(argv[2])<8);
    strcpy(system_code,argv[1]); strcpy(game_code,argv[2]);
    set(NULL,&req,&sys);
    desired_language(21,&app);
    assert(sys.data_size==8 && !memcmp(sys.data,system_code,8));
    assert(app.data_size==8 && !memcmp(app.data,game_code,8));
    settings_save();
}
''', encoding='utf-8')
            exe = out / 'language.exe'
            subprocess.run([DEFAULT_COMPILER,'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                            '-I'+str(runtime),'-I'+str(DEPS/'include'),source,runtime/'settings.c',
                            DEPS/'lib/libSDL3.dll.a','-o',exe],
                           env=compiler_env(),check=True)
            ini = out / 'settings.ini'
            env = dict(os.environ, LARECOMP_SETTINGS=str(ini), PATH=str(DEPS/'bin')+os.pathsep+os.environ['PATH'])
            for saved, system_code, game_code in [('zh-CN','zh-CN','zh-Hans'),('zh-TW','zh-TW','zh-Hant'),
                                                  ('ja','ja','ja'),('auto','en-US','en-US')]:
                with self.subTest(saved=saved):
                    ini.write_text(f'language = {saved}\n', encoding='utf-8')
                    result = subprocess.run([exe,system_code,game_code], env=env,
                                            capture_output=True,text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('language = '+saved, ini.read_text(encoding='utf-8').splitlines())
