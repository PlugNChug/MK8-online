"""Exercita TMML.B sintético no tradutor real e compila o GLSL produzido."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from maxwell_mip_query import prepare
from native import DEFAULT_COMPILER, TOOLS

DEPS = TOOLS/'deps/packages/ucrt64'
HARNESS = r'''
#include "shader.h"
#include <shaderc/shaderc.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t words[44];
static const uint64_t query = 0xdf60000120670406ull;
static uint64_t mov(int reg, int cb, int offset) {
    return 0x4c98078000070000ull | (uint64_t)cb << 34 | (uint64_t)(offset / 4) << 20 | reg;
}
static void reset(int reg, int offset) {
    memset(words, 0, sizeof words);
    ((uint32_t *)words)[0x78 / 4] = 15;
    words[17] = mov(reg, 2, offset);
    words[18] = 0xe003ff87cff7ff08ull;  // IPA R8
    words[19] = 0x0103f8000007f00full;  // MOV32I R15
    words[21] = 0x5080000000470808ull;  // MUFU R8
    words[22] = 0xe043ff898087ff04ull;  // IPA R4
    words[23] = 0xe043ff89c087ff05ull;  // IPA R5
    words[25] = (query & ~((255ull << 20) | 255ull)) | (uint64_t)reg << 20 | reg;
    words[26] = 0xe30000000007000full;
}
static ShaderInfo *translate(int glsl, int stage) {
    ShaderKey key = {0}; key.glsl = glsl;
    return maxwell_translate((uint8_t *)words, sizeof words, stage, &key);
}
static void rejected(void) {
    ShaderInfo *si = translate(1, STAGE_FRAGMENT);
    assert(!si->msl && strstr(si->error, "unsupported")); maxwell_free(si);
}
static void compile_glsl(ShaderInfo *si) {
    shaderc_compiler_t compiler = shaderc_compiler_initialize(); assert(compiler);
    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, si->msl, strlen(si->msl),
        shaderc_fragment_shader, "tmml.glsl", "main", options);
    if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
        fprintf(stderr, "%s\n", shaderc_result_get_error_message(result)); abort();
    }
    assert(shaderc_result_get_length(result) > 20);
    shaderc_result_release(result); shaderc_compile_options_release(options); shaderc_compiler_release(compiler);
}
int main(int argc, char **argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            FILE *f = fopen(argv[i], "rb"); assert(f);
            uint8_t code[65536]; size_t n = fread(code, 1, sizeof code, f); fclose(f);
            ShaderKey key = {0}; key.glsl = 1;
            ShaderInfo *si = maxwell_translate(code, n, STAGE_FRAGMENT, &key);
            if (!si->msl) { fprintf(stderr, "%s: %s\n", argv[i], si->error); return 1; }
            compile_glsl(si); printf("%s: translated+compiled, textures=%d\n", argv[i], si->ntex);
            maxwell_free(si);
        }
        return 0;
    }
    reset(6, 0x28);
#ifdef EXPECT_UNSUPPORTED
    ShaderInfo *before = translate(1, STAGE_FRAGMENT);
    assert(!before->msl && strstr(before->error, "unknown instruction df60000120670406 at 0x48"));
    maxwell_free(before); return 0;
#endif
    for (int glsl = 0; glsl < 2; glsl++) for (int reg = 6; reg < 8; reg++) {
        for (int offset = 0x20; offset <= 0x118; offset += 8) {
            reset(reg, offset);
            ShaderInfo *si = translate(glsl, STAGE_FRAGMENT);
            if (!si->msl) { fprintf(stderr, "%s\n", si->error); return 1; }
            assert(si->ntex == 1 && si->tex[0].type == TEXTYPE_2D);
            assert(si->tex[0].handle_offset == (unsigned)offset && !si->tex[0].bindless);
            // Igualdade com a instrução bound equivalente cobre emissor, destino e metadados.
            words[25] = (words[25] & 0x0000000fffffffffull) | 0xdf58000000000000ull | (uint64_t)(offset / 4) << 36;
            ShaderInfo *bound = translate(glsl, STAGE_FRAGMENT);
            assert(bound->msl && !strcmp(si->msl, bound->msl));
            if (glsl && offset == 0x28) compile_glsl(si);
            maxwell_free(bound); maxwell_free(si);
        }
    }
    reset(6, 0x28); words[17] = mov(6, 3, 0x28); rejected();
    reset(6, 0x28); words[17] &= ~(7ull << 16); rejected();  // load predicado
    reset(6, 0x28); words[17] &= ~(1ull << 39); rejected();  // máscara parcial
    reset(6, 0x18); rejected();
    reset(6, 0x24); rejected();
    reset(6, 0x120); rejected();
    reset(6, 0x28); words[19] = (words[19] & ~255ull) | 6; rejected();  // sobrescrita
    reset(6, 0x28); words[19] = 0xef9000000007ff06ull; rejected();  // LDC dinâmico/não suportado
    reset(6, 0x28); words[19] = 0x5c5800000007000full; rejected();  // operação fora da prova
    reset(6, 0x28); words[19] = 0xe24000000107000full; rejected();  // fluxo de controle
    reset(6, 0x28); words[26] = 0xe300000000070002ull;  // EXIT com condição CC EQ
    words[27] = 0xe24000000007000full | (0x1000000ull - 0x48) << 20; rejected();
    for (int pred = 0; pred <= 15; pred += 15) {  // EXIT P0 / !PT não encerra todos os caminhos
        reset(6, 0x28); words[26] = 0xe30000000000000full | (uint64_t)pred << 16;
        words[27] = 0xe24000000007000full | (0x1000000ull - 0x48) << 20; rejected();
    }
    reset(6, 0x28); words[25] |= 1ull << 31; rejected();  // XY: alias exige outro caso
    reset(6, 0x28); words[25] |= 1ull << 28; rejected();  // array
    reset(6, 0x28); words[25] |= 1ull << 35; rejected();  // NDV
    reset(6, 0x28); words[25] |= 1ull << 50; rejected();  // reservado
    reset(6, 0x28); words[25] = (words[25] & ~(255ull << 8)) | 254ull << 8; rejected();
    reset(6, 0x28); words[25] |= 255ull << 20; rejected();  // handle RZ
    reset(6, 0x28); ShaderInfo *vertex = translate(1, STAGE_VERTEX);
    assert(!vertex->msl && strstr(vertex->error, "unsupported")); maxwell_free(vertex);
    reset(6, 0x28); words[25] |= 255ull; ShaderInfo *discard = translate(1, STAGE_FRAGMENT);
    assert(discard->msl && discard->ntex == 0); maxwell_free(discard);
    reset(6, 0x28); words[26] = words[21]; words[27] = 0xe30000000007000full;
    words[29] = 0xe2400fffff07000full;  // padding self-branch após EXIT
    ShaderInfo *padding = translate(1, STAGE_FRAGMENT);
    assert(padding->msl && padding->ntex == 1); compile_glsl(padding); maxwell_free(padding);
    reset(6, 0x28); words[25] |= 1ull << 49;  // NODEP não altera resultado
    ShaderInfo *nodep = translate(1, STAGE_FRAGMENT);
    words[25] &= ~(1ull << 49); ShaderInfo *normal = translate(1, STAGE_FRAGMENT);
    assert(nodep->msl && normal->msl && !strcmp(nodep->msl, normal->msl));
    maxwell_free(nodep); maxwell_free(normal);
    // Segunda consulta após recarregar outro handle e calcular o descarte do efeito.
    reset(6, 0x28); words[26] = mov(12, 2, 0x20);
    words[27] = 0x5c68100000470504ull;  // FMUL R4, sem tocar R12
    words[29] = 0xe043ff898087ff06ull;  // IPA R6
    words[30] = 0x4bb1038c0007040cull;  // FSETP: low8=12 não é destino GPR
    words[31] = 0xe33000000000000full;  // KIL P0
    words[33] = (query & ~((255ull << 20) | (255ull << 8) | 255ull)) | 12ull << 20 | 6ull << 8 | 12ull;
    words[34] = 0xe30000000007000full;
    ShaderInfo *second = translate(1, STAGE_FRAGMENT);
    assert(second->msl && second->ntex == 2 && second->tex[1].handle_offset == 0x20);
    compile_glsl(second); maxwell_free(second);
    words[27] = (words[27] & ~255ull) | 12; rejected();  // FMUL sobrescreve o handle
    // Handle preservado através da primeira consulta e da amostragem TEXS RGBA.
    reset(6, 0x28); words[19] = mov(12, 2, 0x20);
    words[26] = 0x5cb8000000670a00ull;  // I2F32 R0
    words[27] = 0x3868103b80070000ull;  // FMUL imediato R0
    words[29] = 0x36b483c00007000cull;  // FSETP imediato, somente predicados
    words[30] = 0x010400000000f000ull;
    words[31] = 0xd87000a020070400ull;  // TEXS.F32 R0/R1 e R2/R3
    words[33] = 0xe043ff898087ff06ull;
    words[34] = 0x5c68100000470504ull;
    words[35] = 0xe043ff89c087ff07ull;
    words[37] = 0x4bb1038c0007040cull;
    words[38] = 0xe33000000000000full;
    words[39] = (query & ~((255ull << 20) | (255ull << 8) | 255ull)) | 12ull << 20 | 6ull << 8 | 12ull;
    words[41] = 0xe30000000007000full;
    ShaderInfo *preserved = translate(1, STAGE_FRAGMENT);
    assert(preserved->msl && preserved->ntex == 2); compile_glsl(preserved); maxwell_free(preserved);
    uint64_t saved[44]; memcpy(saved, words, sizeof words);
    for (int i = 25; i <= 27; i++) {  // TMML.B, I2F e FMUL não podem sobrescrever R12
        memcpy(words, saved, sizeof words); words[i] = (words[i] & ~255ull) | 12; rejected();
    }
    for (int pair = 0; pair < 2; pair++) for (int dest = 11; dest <= 12; dest++) {
        memcpy(words, saved, sizeof words);
        int shift = pair ? 28 : 0;
        words[31] = (words[31] & ~(255ull << shift)) | (uint64_t)dest << shift; rejected();
    }
    // A two-component sample and scalar arithmetic can precede the mip query.
    // Use a synthetic instruction stream and compare against the bound form.
    memcpy(words, saved, sizeof words);
    words[26] = 0x4c68000000070000ull;  // FMUL cbuf, R0
    words[27] = 0x32a0000000070001ull;  // FFMA immediate, R1
    words[30] = 0x59a0000000070002ull;  // FFMA register, R2
    words[31] = 0xd834000ff0070004ull;  // TEXS.F32, R/A into R4 and R5
    ShaderInfo *effect = translate(1, STAGE_FRAGMENT);
    if (!effect->msl) fprintf(stderr, "%s\n", effect->error);
    assert(effect->msl);
    uint64_t bindless = words[39];
    words[39] = (bindless & 0x0002000fffffffffull) | 0xdf58000000000000ull | (8ull << 36);
    ShaderInfo *reference = translate(1, STAGE_FRAGMENT);
    assert(reference->msl && reference->ntex == effect->ntex && !strcmp(effect->msl, reference->msl));
    compile_glsl(effect); maxwell_free(reference); maxwell_free(effect);
    words[39] = bindless;
    for (int i = 26; i <= 31; i++) {
        if (i == 28 || i == 29) continue;
        uint64_t original = words[i];
        words[i] = (original & ~255ull) | 12; rejected();
        if (i == 31) { words[i] = (original & ~255ull) | 11; rejected(); }
        words[i] = original;
    }
    puts("TMML.B: baseline, equivalent bindings, GLSL and rejected provenance verified");
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS/'include/shaderc/shaderc.h').is_file(),
                     'Native shader toolchain required')
class MipQueryTests(unittest.TestCase):
    def test_native_translation_and_shaderc(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = root/'runtime'
            source = runtime/'nvn/maxwell'
            shutil.copytree(ROOT/'upstream/smm2recomp/recomp/runtime/nvn/maxwell', source)
            program = root/'mip_query.c'
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
                subprocess.run(command+(['-DEXPECT_UNSUPPORTED'] if baseline else [])+['-o', str(exe)], env=env, check=True)
                subprocess.run([str(exe)], env=env, check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
