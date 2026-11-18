"""Exercises the real translator: level-0 texture sizes of a branching GLSL shader are read before its loop (#7)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from texture_size_hoist import prepare
from native import DEFAULT_COMPILER, TOOLS

DEPS = TOOLS/'deps/packages/ucrt64'
HARNESS = r'''
#include "shader.h"
#include <shaderc/shaderc.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTEX 11
static uint64_t words[64];

// SSY + SYNC (a branching program: the `switch (pc)` loop), then NTEX array lookups at the SSY target and EXIT.
static void program(bool branchy) {
    memset(words, 0, sizeof words);
    ((uint32_t *)words)[0x78 / 4] = 15;  // SPH: render target 0, RGBA
    int at[NTEX + 4], n = 0;
    for (int i = 17; n < NTEX + 4; i++) if ((i - 16) % 4) at[n++] = i;
    for (int i = 0; i < 3; i++) words[at[i]] = 0x50b0000000070f00ull;  // NOP
    uint32_t target = (uint32_t)(at[3] - 16) * 8;
    if (branchy) {
        words[at[0]] = 0xe290000000070000ull | (uint64_t)(target - 16) << 20;  // SSY target (from pc 8)
        words[at[1]] = 0xf0f800000007000full;                                  // SYNC
    }
    for (int k = 0; k < NTEX; k++)  // TEX.ARRAY 2D RGBA R0, R0 (layer R0, coordinates R1, R2), handle 10 + k
        words[at[3 + k]] = 0xc038000000070000ull | (uint64_t)(10 + k) << 36 | 15ull << 31 | 1ull << 29 | 1ull << 28 |
                           255ull << 20;
    words[at[3 + NTEX]] = 0xe30000000007000full;  // EXIT
}

static ShaderInfo *translate(bool branchy, int glsl) {
    program(branchy);
    ShaderKey key = {0}; key.glsl = glsl;
    ShaderInfo *si = maxwell_translate((uint8_t *)words, sizeof words, STAGE_FRAGMENT, &key);
    if (!si->msl) { fprintf(stderr, "%s\n", si->error); abort(); }
    assert(si->ntex == NTEX);
    return si;
}

static void compile_glsl(const ShaderInfo *si) {
    shaderc_compiler_t compiler = shaderc_compiler_initialize(); assert(compiler);
    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, si->msl, strlen(si->msl),
        shaderc_fragment_shader, "size_hoist.glsl", "main", options);
    if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
        fprintf(stderr, "%s\n%s\n", shaderc_result_get_error_message(result), si->msl); abort();
    }
    shaderc_result_release(result); shaderc_compile_options_release(options); shaderc_compiler_release(compiler);
}

static int count(const char *s, const char *what) {
    int n = 0;
    for (const char *p = s; (p = strstr(p, what)); p += strlen(what)) n++;
    return n;
}

int main(void) {
    ShaderInfo *si = translate(true, 1);
    const char *main_fn = strstr(si->msl, "void main() {"), *loop = strstr(si->msl, "  for (uint guard");
    assert(main_fn && loop);
    char call[32], decl[64], use[48];
    for (int k = 0; k < NTEX; k++) {
        snprintf(call, sizeof call, "tsize%d(0)", k);
        snprintf(decl, sizeof decl, "  ivec3 tsz%d = tsize%d(0);\n", k, k);
        snprintf(use, sizeof use, "uint(tsz%d.z) - 1u", k);
#ifdef BASELINE
        assert(!strstr(main_fn, decl) && count(loop, call) == 1);  // the layer clamp queries inside the loop
#else
        const char *d = strstr(main_fn, decl);
        assert(d && d < loop);                  // read once, before the dispatch loop
        assert(count(main_fn, call) == 1);      // (that read is the only query left)
        assert(count(loop, use) == 1);          // the layer clamp uses it
#endif
    }
    compile_glsl(si); maxwell_free(si);
    ShaderInfo *linear = translate(false, 1);  // no branches: no loop, nothing hoisted
    assert(!strstr(linear->msl, "for (uint guard") && !strstr(linear->msl, "tsz") && count(linear->msl, "uint(tsize0(0).z)") == 1);
    compile_glsl(linear); maxwell_free(linear);
    ShaderInfo *msl = translate(true, 0);  // MSL is unchanged
    assert(!strstr(msl->msl, "tsz") && strstr(msl->msl, "get_array_size()"));
    maxwell_free(msl);
    puts("texture sizes: hoisted before the dispatch loop, branch-free GLSL and MSL unchanged, GLSL compiles");
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS/'include/shaderc/shaderc.h').is_file(),
                     'Native shader toolchain required')
class TextureSizeHoistTests(unittest.TestCase):
    def test_native_translation_and_shaderc(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = root/'runtime'
            source = runtime/'nvn/maxwell'
            shutil.copytree(ROOT/'upstream/smm2recomp/recomp/runtime/nvn/maxwell', source)
            program = root/'size_hoist.c'
            program.write_text(HARNESS, encoding='utf-8')
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(TOOLS/'cache'))
            env['PATH'] = str(DEPS/'bin')+os.pathsep+env.get('PATH', '')
            command = [str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                       '-I'+str(source), '-I'+str(DEPS/'include'), str(program), str(source/'translate.c'),
                       str(DEPS/'lib/libshaderc_shared.dll.a')]
            for baseline in (True, False):
                exe = root/('before.exe' if baseline else 'after.exe')
                if not baseline:
                    prepare(runtime)
                subprocess.run(command+(['-DBASELINE'] if baseline else [])+['-o', str(exe)], env=env, check=True)
                subprocess.run([str(exe)], env=env, check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
