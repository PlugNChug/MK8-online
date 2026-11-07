"""Run the private MK8 SDK's recompiled verifier against every editor face combination.

No generated game code is stored in this test. Requires this machine's generated SDK.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER


class MiiSdkTests(unittest.TestCase):
    def test_editor_appearance_passes_actual_sdk_verifier(self):
        generation = ROOT/'local/generation.json'
        if not generation.is_file() or not DEFAULT_COMPILER.is_file():
            self.skipTest('Private generated MK8 SDK and native compiler required')
        identity = json.loads(generation.read_text(encoding='utf-8'))['identity']
        source = ROOT/'local/builds'/identity/'generated/sdk/sdk_042.c'
        if not source.is_file():
            self.skipTest('Generated SDK unit unavailable')
        text = source.read_text(encoding='utf-8')
        functions = []
        for address, expected in [
            ('70041b6324', '12f64799cf29998b065504cbd6eac0b04b1a1e7d0107460afd91785e05708a28'),
            ('70041b5c90', '23efad68fa08abd6b551a3dc063263c870e6d288eefe388c0e0cb3ff4a8e26a4'),
            ('70041b68c8', 'd9aa071c8049fdf922eafc2e34e86798c936d2082fe755c2947b6e58ec1e6c83'),
        ]:
            body = re.search(r'void f_'+address+r'\(Cpu \*c\) \{.*?\n\}', text, re.S)
            self.assertIsNotNone(body, 'SDK verifier address changed')
            body = body.group()
            self.assertEqual(hashlib.sha256(body.encode()).hexdigest(), expected, 'SDK verifier changed; review private harness')
            functions.append(body.replace('f_70043fe490(c)', 'f_70041b6324(c)').replace('f_70043fe4a0(c)', 'f_70041b5c90(c)'))
        runtime = ROOT/'upstream/smm2recomp/recomp/runtime'
        service = (runtime/'services/mii.c').read_text(encoding='utf-8')
        default = service[service.index('void mii_default_charinfo'):service.index('static bool one_mii')]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            program = root/'verify.c'
            program.write_text('''#include "cpu.h"
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
volatile int g_fnprof;
void fnprof_hit(uint64_t a,uint64_t b,uint64_t d) {}
'''+default+'\n#include "mii/local_mii.inc"\n'+'\n'.join(functions)+r'''
static uint64_t verify(uint8_t *info) {
 _Alignas(16) uint8_t stack[4096];Cpu c={0};c.sp=(uintptr_t)(stack+sizeof stack);c.x[0]=(uintptr_t)info;
 f_70041b68c8(&c);return c.x[0];
}
int main(void) {
 uint8_t face[9]={0},info[0x58],copy[0x58];unsigned cases=0;
 const uint8_t limits[9]={6,3,4,8,3,6,2,1,1};
 for(;;) {
  for(int edge=0;edge<3;edge++) {
   mii_default_charinfo(info,"Player");face[7]=face[8]=edge==2?127:edge==1?64:0;
   info[0x27]=edge==2?11:0;info[0x28]=edge&1;mk8_face_charinfo(info,face);
   memcpy(copy,info,sizeof info);uint64_t result=verify(info);
   if(result) { fprintf(stderr,"case %u SDK result %llu\n",cases,(unsigned long long)result);return 1; }
   assert(!memcmp(copy,info,sizeof info));cases++;
  }
  int digit=0;for(;digit<7;digit++) { if(++face[digit]<limits[digit])break;face[digit]=0; }
  if(digit==7)break;
 }
 info[0x2e]=255;assert(verify(info)!=0);
 printf("%u appearances accepted by native SDK verifier\n",cases);return 0;
}
''', encoding='utf-8')
            exe = root/'verify.exe'
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-UNDEBUG', '-I'+str(runtime), '-I'+str(ROOT/'runtime/mk8'), str(program), '-o', str(exe)], env=env, check=True)
            subprocess.run([str(exe)], check=True, timeout=15)
