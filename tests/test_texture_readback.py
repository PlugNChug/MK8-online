"""NVN readback entrypoints and bounded copy state, exercised as native code."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import texture_readback


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class ReadbackTests(unittest.TestCase):
    def test_entrypoints_preserve_strides_view_and_reject_invalid_memory(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', out / 'host')
            text = (runtime / 'nvn/objects.c').read_text(encoding='utf-8')
            wrappers = text[text.index('NVN(nvnTextureReadTexels) {'):
                            text.index('// ================================================================== samplers')]
            # Queue ordering is exercised separately against the actual worker in test_async_submit.
            wrappers = wrappers.replace('mk8_readback_sync(', 'mk8_texture_readback(')
            own = (ROOT / 'runtime/mk8/graphics/texture_readback.inc').read_text(encoding='utf-8')
            source = out / 'readback.c'
            source.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define LOG(...) ((void)0)
static jmp_buf failed;
#define FATAL(...) longjmp(failed,1)
static uint8_t memory[256];
#define HEAP_BASE ((uint64_t)(uintptr_t)memory)
#define GUEST_HI (HEAP_BASE+sizeof memory)
typedef struct { uint32_t setmask;uint16_t base_level,num_levels,min_layer,num_layers; } NvnTextureView;
typedef struct { uint32_t width,height,depth,samples,format; } NvnTexture;
typedef struct { unsigned bw,bh,bpb; } FormatInfo;
static FormatInfo fmt={1,1,4};
static const FormatInfo *nvn_format_info(uint32_t n) { return &fmt; }
static bool renderer_format_is_depth(uint32_t n) { return n==99; }
typedef struct { uint64_t tex;NvnTextureView view;uint8_t has_view;int32_t region[6]; } TexRefRec;
static struct { uint64_t copy_row_stride,copy_image_stride; } st={111,222};
static unsigned copies;
static void copy_texture_to_buffer(const TexRefRec *ref,uint64_t dst) {
    assert(ref->region[3]==4 && ref->region[4]==3); copies++;
    for(unsigned y=0;y<3;y++)for(unsigned x=0;x<16;x++)
        ((uint8_t*)(uintptr_t)dst)[y*st.copy_row_stride+x]=(uint8_t)(y*16+x);
}
''' + own + r'''
typedef struct { uint64_t x[8]; } Cpu;
#define NVN(name) static void name(Cpu *c)
#define A(i) c->x[i]
#define AP(i) ((void*)(uintptr_t)c->x[i])
#define HOST(T,g) ((T*)(uintptr_t)(*(uint64_t*)(uintptr_t)(g)))
''' + wrappers + r'''
int main(void) {
    NvnTexture texture={4,3,1,1,1};uint64_t guest=(uintptr_t)&texture;
    int32_t region[6]={0,0,0,4,3,1};NvnTextureView view={0};
    Cpu c={{(uintptr_t)&guest,(uintptr_t)&view,(uintptr_t)region,HEAP_BASE,24,72}};
    memset(memory,0xcc,sizeof memory);nvnTextureReadTexelsStrided(&c);
    assert(copies==1 && memory[24]==16 && memory[48]==32 && memory[16]==0xcc);
    assert(st.copy_row_stride==111 && st.copy_image_stride==222);
    memset(memory,0xcc,sizeof memory);nvnTextureReadTexels(&c);
    assert(copies==2 && memory[16]==16 && memory[32]==32 && memory[48]==0xcc);
    assert(st.copy_row_stride==111 && st.copy_image_stride==222);
    for(unsigned invalid=0;invalid<9;invalid++) {
        c.x[0]=(uintptr_t)&guest;c.x[3]=HEAP_BASE;c.x[4]=24;c.x[5]=72;
        texture.format=1;view.base_level=0;view.setmask=0;region[3]=4;
        if(invalid==0)c.x[0]=0;
        if(invalid==1)c.x[4]=(uint64_t)-1;
        if(invalid==2)c.x[4]=8;
        if(invalid==3)c.x[5]=16;
        if(invalid==4)c.x[3]=GUEST_HI-8;
        if(invalid==5)region[3]=5;
        if(invalid==6)texture.format=99;
        if(invalid==7){view.setmask=1;view.base_level=1;}
        if(invalid==8)c.x[4]=UINT64_MAX/2;
        if(!setjmp(failed)){nvnTextureReadTexelsStrided(&c);return 2;}
        assert(copies==2 && st.copy_row_stride==111 && st.copy_image_stride==222);
    }
}
''', encoding='utf-8')
            exe = out / 'readback.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                            '-O2', '-UNDEBUG', str(source), '-o', str(exe)],
                           env=compiler_env(), check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, timeout=30)

    def test_source_boundaries_fail_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'nvn').mkdir()
            (root / 'nvn/objects.c').write_text('changed', encoding='utf-8')
            with self.assertRaises(ValueError):
                texture_readback.prepare(root)


if __name__ == '__main__':
    unittest.main()
