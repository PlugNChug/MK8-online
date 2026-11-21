"""In-game panel text follows the launcher language (issues #76 and #77)."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from native import DEFAULT_COMPILER, compiler_env


class UiLanguageTest(unittest.TestCase):
    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_panel_text_follows_launcher_language(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'language.c'
            source.write_text('#include "social.h"\n#include <stdio.h>\n'
                              'int main(void) { fputs(mk8_ui_text("Ajustes", "Settings", "Configuración"), stdout); return 0; }\n',
                              encoding='utf-8')
            exe = root / 'language.exe'
            build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2',
                                    '-I' + str(ROOT / 'runtime/mk8/net'), str(source), '-o', str(exe)],
                                   env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-3000:])
            for language, expected in (('pt-BR', 'Ajustes'), ('en', 'Settings'), ('es', 'Configuración'),
                                       ('', 'Settings'), ('fr', 'Settings'), (None, 'Settings')):
                env = dict(os.environ)
                env.pop('MK8_UI_LANGUAGE', None)
                if language is not None:
                    env['MK8_UI_LANGUAGE'] = language
                result = subprocess.run([str(exe)], env=env, capture_output=True, timeout=10, check=True)
                self.assertEqual(result.stdout.decode('utf-8'), expected, language)

    def test_hub_has_no_untranslated_portuguese_literals(self):
        # Every user-visible hub text goes through hub_text(pt, en, es).
        import re
        hub = (ROOT / 'runtime/mk8/ui/hub.h').read_text(encoding='utf-8')
        accented = [line.strip() for line in hub.splitlines()
                    if re.search(r'"[^"]*[ãçéáíóúõêâ][^"]*"', line) and not re.search(r'\b(?:hub_text|tr)\(', line)
                    and not line.strip().startswith('"')]
        self.assertEqual(accented, [])


if __name__ == '__main__':
    unittest.main()
