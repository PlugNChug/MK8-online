"""The prepared GetDesiredLanguage reply for every saved game language (#99)."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *saved;
static const char *settings_language(void) { return saved; }
HELPER
static void reply(char lang[8]) {
REPLY
}
int main(void) {
    const char *cases[][2] = {{"zh-CN", "zh-Hans"}, {"zh-TW", "zh-Hant"}, {"ja", "ja"}, {"en-US", "en-US"},
                              {"ko", "ko"}, {"es", "es"}, {"es-419", "es-419"}, {"zh", "zh"}};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        char lang[8] = {0};
        saved = cases[i][0];
        reply(lang);
        assert(!strcmp(lang, cases[i][1]));
        assert(!lang[7]);  // the eight-byte LanguageCode keeps its terminator
    }
    puts("desired language ok");
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class DesiredLanguageTests(unittest.TestCase):
    def test_prepared_reply_uses_application_language_codes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', root/'host')
            am = (runtime/'services/am.c').read_text(encoding='utf-8')
            start = am.index('static const char *mk8_desired_language(const char *system) {')
            helper = am[start:am.index('\n}\n', start) + 3]
            case = am.index('    case 21: {  // GetDesiredLanguage')
            line = am.index('        snprintf(lang, sizeof lang, "%s", mk8_desired_language(settings_language()));', case)
            self.assertLess(line, am.index('    case 23:', case))
            self.assertLess(start, case)
            source = root/'desired.c'
            source.write_text(HARNESS.replace('HELPER', helper).replace(
                'REPLY', am[line:am.index('\n', line)]), encoding='utf-8')
            exe = root/'desired.exe'
            build = subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                                    source, '-o', exe], env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([exe], capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('desired language ok', run.stdout)


if __name__ == '__main__':
    unittest.main()
