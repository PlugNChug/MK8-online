"""Translate synthetic TLD4S programs and compile their actual GLSL to SPIR-V."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS/'include/shaderc/shaderc.h').is_file(), 'Native shader toolchain required')
class GatherTests(unittest.TestCase):
    def test_plain_gather_all_components_and_unsupported_variants(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', root)
            program = root/'gather.c'
            program.write_text(r'''
#include "shader.h"
#include <shaderc/shaderc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 uint64_t words[19]={0};uint8_t *code=(uint8_t *)words;
 *(uint32_t *)(code+0x78)=15;words[18]=0xe30000000007000full;
 ShaderKey key={0};key.glsl=1;
 shaderc_compiler_t compiler=shaderc_compiler_initialize();assert(compiler);
 shaderc_compile_options_t options=shaderc_compile_options_initialize();
 shaderc_compile_options_set_target_env(options,shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_3);
 for(int comp=0;comp<4;comp++)for(int discard=0;discard<2;discard++) {
  words[17]=(0xdf020080c0b7010eull & ~(3ull<<52)) | ((uint64_t)comp<<52);
  if(discard)words[17]=(words[17]&~(255ull<<28))|(255ull<<28);
  ShaderInfo *si=maxwell_translate(code,sizeof words,STAGE_FRAGMENT,&key);
  if(si->error[0]){fprintf(stderr,"%s\n",si->error);return 1;}
  assert(si->ntex==1 && si->tex[0].type==TEXTYPE_2D && si->tex[0].handle_offset==0x20);
  assert(strstr(si->msl,"textureGather(") && !strstr(si->msl,"uint r255"));
  shaderc_compilation_result_t result=shaderc_compile_into_spv(compiler,si->msl,strlen(si->msl),shaderc_fragment_shader,"gather.glsl","main",options);
  if(shaderc_result_get_compilation_status(result)!=shaderc_compilation_status_success) {
   fprintf(stderr,"%s\n",shaderc_result_get_error_message(result));return 2;
  }
  assert(shaderc_result_get_length(result)>20);shaderc_result_release(result);maxwell_free(si);
 }
 for(int bit=50;bit<=51;bit++) {
  words[17]=0xdf020080c0b7010eull|(1ull<<bit);
  ShaderInfo *si=maxwell_translate(code,sizeof words,STAGE_FRAGMENT,&key);
  assert(strstr(si->error,"unsupported"));maxwell_free(si);
 }
 shaderc_compile_options_release(options);shaderc_compiler_release(compiler);return 0;
}
''', encoding='utf-8')
            exe = root/'gather.exe'
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            env['PATH'] = str(DEPS/'bin')+os.pathsep+env.get('PATH','')
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG', '-I'+str(runtime/'nvn/maxwell'), '-I'+str(DEPS/'include'), str(program), str(runtime/'nvn/maxwell/translate.c'), str(DEPS/'lib/libshaderc_shared.dll.a'), '-o', str(exe)], env=env, check=True)
            subprocess.run([str(exe)], env=env, check=True, timeout=20)
