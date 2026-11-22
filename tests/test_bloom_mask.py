"""Exercise live bloom tint routing and optionally validate the exact 4.0.1 mask family."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER, compiler_env

HARNESS = r'''#include "bloom_mask.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Check membership against every fragment entry in the pinned AGL post-effects archive.
// The two named families delimit the mask variants in the 4.0.1 program table.
static void check_mask_archive(FILE *f) {
    const long program_table = 0x2000 + 0x1158;
    const long code_base = 0x2000 + 0xe7000;
    uint32_t bounds[2], fields[5];
    const long names[] = {0xe7cad, 0xe7e13};
    const char *family[] = {"bloom_mask", "bloom_gaussian"};
    for (int i=0; i<2; i++) {
        char name[24] = {0};
        assert(fseek(f, names[i]-4, SEEK_SET)==0 && fread(&bounds[i],4,1,f)==1);
        assert(fread(name,strlen(family[i])+1,1,f)==1 && !strcmp(name,family[i]));
    }
    uint32_t count;
    assert(fseek(f,program_table-4,SEEK_SET)==0 && fread(&count,4,1,f)==1 && count==2632);
    long entry=program_table;
    unsigned masks=0, dual_output=0;
    for (unsigned i=0; i<count; i++) {
        assert(fseek(f,entry,SEEK_SET)==0 && fread(fields,sizeof fields,1,f)==1);
        assert(fields[0]>=sizeof fields && fields[0]<0x10000);
        entry+=fields[0];
        if (fields[1]!=1) continue;
        uint8_t bytes[512];
        assert(fseek(f,code_base+fields[4]+0x30,SEEK_SET)==0 && fread(bytes,sizeof bytes,1,f)==1);
        uint64_t hash=1469598103934665603ull;
        for (size_t j=0;j<sizeof bytes;j++) hash=(hash^bytes[j])*0x100000001b3ull;
        bool mask=i>=bounds[0] && i<bounds[1];
        assert(mk8_is_bloom_mask(hash)==mask);
        masks+=mask;
        if (mask) {
            uint32_t outputs;
            assert(!fseek(f,code_base+fields[4]+0x78,SEEK_SET) && fread(&outputs,4,1,f)==1);
            assert(outputs==0xf || outputs==0xff);
            dual_output+=outputs==0xff;
        }
    }
    assert(masks==256 && dual_output==128);
}

int main(int argc,char **argv) {
    const float neutral[8]={0.9f,0.1f,0.2f,1,0,0,0,1};
    const uint64_t mask=0x051303030c68828full, composite=0xdd475c2c10ddf381ull;
    assert(mk8_bloom_tint(true,mask,neutral)==neutral);
    assert(mk8_bloom_tint(false,composite,neutral)==neutral);
    const float *off=mk8_bloom_tint(false,mask,neutral);
    assert(off[3]==1 && off[7]==1);
    for (unsigned i=0;i<3;i++) assert(off[i]==0 && off[i+4]==0);
    assert(!mk8_is_bloom_mask(0) && !mk8_is_bloom_mask(UINT64_MAX));
    assert(mk8_is_bloom_mask(0x00cb4d81523f76a2ull) && mk8_is_bloom_mask(0xfeeae9ce74ab8af5ull));
    if (argc==2) { FILE *f=fopen(argv[1],"rb"); assert(f); check_mask_archive(f); fclose(f); }
}
'''

@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class BloomMaskTests(unittest.TestCase):
    def test_only_mask_programs_receive_the_black_tint(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder)/'check.c'
            source.write_text(HARNESS, encoding='utf-8')
            exe = source.with_suffix('.exe')
            subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                            '-I'+str(ROOT/'runtime/mk8/graphics'), source, '-o', exe], env=compiler_env(), check=True)
            archive = os.environ.get('MK8_TEST_CASTLE_SHADER_ARCHIVE')
            subprocess.run([exe, *([archive] if archive else [])], check=True)
