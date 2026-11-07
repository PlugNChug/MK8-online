"""Native integration using authored NSOs only. Requires scripts/setup-native.py."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_pipeline import ROOT, adapter, generator, module, synthetic_nso

native = module('mk8native', ROOT/'toolchain/native.py')


@unittest.skipUnless(native.DEFAULT_COMPILER.is_file(), 'Run scripts/setup-native.py')
class NativeTests(unittest.TestCase):
    def test_seven_modules_compile_link_and_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root/'runtime'
            adapter.prepare(ROOT/'upstream/smm2recomp', source)
            profile = json.loads((ROOT/'games/mk8d/profiles/development.json').read_text())
            exefs = root/'exefs'; exefs.mkdir()
            for name in profile['modules']:
                synthetic_nso(exefs/name)
            generator.generate(source, exefs, root/'generated', profile)
            result = native.compile_modules(source, root/'generated', root/'native', profile['modules'])
            self.assertEqual(result['objects'], 15)  # chunk + table for each module, and ABI entry
            stamps = {p:p.stat().st_mtime_ns for p in (root/'native').rglob('*.o')}
            native.compile_modules(source, root/'generated', root/'native', profile['modules'])
            self.assertEqual(stamps, {p:p.stat().st_mtime_ns for p in (root/'native').rglob('*.o')})
            # Native executable calls each compiled fixture; every module must return 42.
            harness = root/'harness.c'
            harness.write_text('''#include "gamelib.h"
#include <stdio.h>
#include <stdlib.h>
const GameLibTables *recomp_game_tables(void);
volatile int g_fnprof;
void fnprof_hit(uint64_t pc, uint64_t lr, uint64_t x0) { (void)pc; (void)lr; (void)x0; }
void rt_dispatch(Cpu *c) { (void)c; }
void rt_unwind_point(void) {}
int main(void) {
    const GameLibTables *g = recomp_game_tables();
    if (g->abi != 3 || g->module_count != 7) return 1;
    for (unsigned i=0; i<g->module_count; i++) {
        const GameLibModule *m = &g->modules[i];
        if (*m->count != 1 || m->addrs[0] != m->base+0x20) return 2;
        Cpu c = {0}; m->fns[0](&c);
        if (c.x[0] != 42) return 3;
        printf("%s: native return 42\\n", m->name);
    }
    return 0;
}
''', encoding='utf-8')
            exe = root/'native-test.exe'
            subprocess.run([str(native.DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-mcpu=x86_64_v2',
                            '-I'+str(source/'runtime'), str(harness), result['archive'], '-o', str(exe)], check=True)
            executed = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertEqual(executed.stdout.count('native return 42'), 7)
            # Compile the real loader and dispatch too, rather than validating only generated text.
            for relative in ['loader.c', 'dispatch.c', 'gamelib.c', 'gamebuild.c']:
                subprocess.run([str(native.DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-mcpu=x86_64_v2',
                                '-O1', '-D_FILE_OFFSET_BITS=64', '-I'+str(source/'runtime'), '-c',
                                str(source/'runtime'/relative), '-o', str(root/(relative+'.o'))], check=True)


if __name__ == '__main__':
    unittest.main()
