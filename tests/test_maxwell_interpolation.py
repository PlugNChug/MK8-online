"""Interpola entradas sintéticas conforme o SPH e compila o GLSL resultante."""
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
import interpolation


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS/'include/shaderc/shaderc.h').is_file(),
                     'Native shader toolchain required')
class InterpolationTests(unittest.TestCase):
    def test_header_interpolation_and_multiply(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', root)
            program = root/'interpolation.c'
            program.write_text(r'''
#include "shader.h"
#include <shaderc/shaderc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 uint64_t words[19]={0};uint8_t *blob=(uint8_t *)words;
 *(uint32_t *)(blob+0x78)=1;words[18]=0xe30000000007000full;
 ShaderKey key={0};key.glsl=1;key.vs_out_mask=1;
 shaderc_compiler_t compiler=shaderc_compiler_initialize();assert(compiler);
 shaderc_compile_options_t options=shaderc_compile_options_initialize();
 shaderc_compile_options_set_target_env(options,shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_3);
 for(int type=1;type<=3;type++)for(int multiply=0;multiply<=1;multiply++) {
  // Apenas Y está em uso: o modo deve vir do primeiro componente não vazio.
  blob[0x48]=(uint8_t)(type<<2);
  words[17]=0xe003ff884ff7ff00ull;
  if(multiply)words[17]=(words[17]&~(255ull<<20))|(1ull<<54)|(1ull<<20);
  ShaderInfo *si=maxwell_translate(blob,sizeof words,STAGE_FRAGMENT,&key);
  if(si->error[0]){fprintf(stderr,"%s\n",si->error);return 1;}
  const char *qual=type==1?"flat ":type==3?"noperspective ":"";
  char declaration[100];snprintf(declaration,sizeof declaration,"layout(location = 0) %sin vec4 v0;",qual);
  if(!strstr(si->msl,declaration)){fprintf(stderr,"tipo %d: qualificador incorreto\n",type);return 2;}
  // A multiplicação por 1/W só pertence às entradas perspective.
  if((strstr(si->msl,"v0[1] * gl_FragCoord.w")!=NULL)!=(type==2))return 3;
  if(multiply && !strstr(si->msl,"* as_f(r1)"))return 4;
  shaderc_compilation_result_t result=shaderc_compile_into_spv(compiler,si->msl,strlen(si->msl),shaderc_fragment_shader,"interpolation.glsl","main",options);
  if(shaderc_result_get_compilation_status(result)!=shaderc_compilation_status_success){
   fprintf(stderr,"%s\n",shaderc_result_get_error_message(result));return 5;
  }
  // Conferir a decoração produzida pelo compilador, além da expressão traduzida.
  const uint32_t *spv=(const uint32_t *)shaderc_result_get_bytes(result);
  size_t size=shaderc_result_get_length(result)/4;int flat=0,linear=0;
  for(size_t i=5;i<size;i+=spv[i]>>16){
   assert((spv[i]>>16)>0);
   if((spv[i]&65535)==71 && (spv[i]>>16)>=3){flat|=spv[i+2]==14;linear|=spv[i+2]==13;}
  }
  assert(flat==(type==1) && linear==(type==3));
  shaderc_result_release(result);maxwell_free(si);
 }
 shaderc_compile_options_release(options);shaderc_compiler_release(compiler);return 0;
}
''', encoding='utf-8')
            exe = root/'interpolation.exe'
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            env['PATH'] = str(DEPS/'bin')+os.pathsep+env.get('PATH', '')
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                            '-O1', '-UNDEBUG', '-I'+str(runtime/'nvn/maxwell'),
                            '-I'+str(DEPS/'include'), str(program),
                            str(runtime/'nvn/maxwell/translate.c'),
                            str(DEPS/'lib/libshaderc_shared.dll.a'), '-o', str(exe)],
                           env=env, check=True)
            subprocess.run([str(exe)], env=env, check=True, timeout=20)

    def test_changed_boundary_preserves_source(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            path = runtime/'nvn/maxwell/translate.c'
            path.parent.mkdir(parents=True)
            for text in ('changed upstream',
                         '        c->omap_target = *(const uint32_t *)(sph + 0x48);\n'*2):
                path.write_text(text)
                with self.assertRaisesRegex(ValueError, 'boundary changed'):
                    interpolation.prepare(runtime)
                self.assertEqual(path.read_text(), text)


if __name__ == '__main__':
    unittest.main()
