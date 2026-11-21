"""Exercise prepared Quit, Mods and Account handlers with isolated service doubles."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER,compiler_env
from host import prepare_host_sources


@unittest.skipUnless(DEFAULT_COMPILER.is_file(),'Native compiler required')
class MenuContentTests(unittest.TestCase):
    def test_confirmations_back_navigation_and_failed_saves(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            runtime=prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',root/'host')
            menu=(runtime/'ui/menu.mm').read_text(encoding='utf-8')
            mods=(runtime/'ui/mods_page.mm').read_text(encoding='utf-8')
            account=(runtime/'ui/online_rows.mm').read_text(encoding='utf-8')
            start=account.index('extern "C" void online_rows(void) {')
            (root/'prepared_actions.inc').write_text(
                menu[menu.index('static int mk8_quit_choice;'):menu.index('static void tab_about(void) {')] +
                mods[mods.index('static bool g_back_used;'):mods.index('// ---------------------------------------------------------------- built-in')] +
                mods[mods.index('static void credit('):mods.index('// ---------------------------------------------------------------- GameBanana')] +
                mods[mods.index('static void install_status_row('):mods.index('static void page_gb_mod(')] +
                account[start:account.index('\n}',start)+2],encoding='utf-8')
            exe=root/'actions.exe'
            for mode in ([],['-DNX_SHEET']):
                with self.subTest(sign_in='sheet' if mode else 'browser'):
                    subprocess.run([DEFAULT_COMPILER,'c++','-std=c++17','-O1','-UNDEBUG',*mode,'-I'+str(root),'-I'+str(runtime),
                                    ROOT/'tests/fixtures/native_menu_actions.cpp','-o',exe],env=compiler_env(),check=True)
                    subprocess.run([exe],cwd=root,check=True,timeout=15)
