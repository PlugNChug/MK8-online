"""Execute the prepared allocation recovery with deterministic allocator failures."""
import subprocess
import tempfile
import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class SnapshotMemoryTests(unittest.TestCase):
    def test_allocation_retries_preserve_image_description_and_real_errors(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', root/'host')
            source = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            helper = source[source.index('static VkResult mk8_snapshot_allocate('):source.index('static TexBackend *snapshot_image(')]
            harness = r'''
#include <assert.h>
#include <stddef.h>
typedef int VkResult;
typedef unsigned long long VkImage,VmaAllocation;
typedef struct { int format,samples,width; } VkImageCreateInfo;
typedef struct { int usage,flags; } VmaAllocationCreateInfo;
#define VK_ERROR_OUT_OF_DEVICE_MEMORY -2
#define VMA_MEMORY_USAGE_AUTO_PREFER_HOST 2
#define LOG(...) ((void)0)
static struct { int vma; } g_vk;
static int calls,collections,errors[3];
static const VkImageCreateInfo *expected;
static void vk_collect_dead(void) { assert(calls==1); collections++; }
static int vmaCreateImage(int allocator,const VkImageCreateInfo *ci,const VmaAllocationCreateInfo *ai,
                         VkImage *image,VmaAllocation *memory,void *info) {
    assert(ci==expected && ci->format==99 && ci->samples==4 && ci->width==1920);
    assert(ai->usage==(calls==2?2:1) && ai->flags==7);
    int result=errors[calls++];
    if (!result) { *image=123; *memory=456; }
    return result;
}
''' + helper + r'''
int main(void) {
    VkImageCreateInfo ci={99,4,1920}; expected=&ci;
    VmaAllocationCreateInfo ai={1,7};
    int cases[][3]={{0,0,0},{-2,0,0},{-2,-2,0},{-2,-2,-2},{-1,0,0},{-4,0,0},{-2,-4,0}};
    int results[]={0,0,0,-2,-1,-4,-4}, counts[]={1,2,3,3,1,1,2};
    for(int n=0;n<7;n++) {
        calls=collections=0;
        for(int i=0;i<3;i++) errors[i]=cases[n][i];
        VkImage image=0; VmaAllocation memory=0;
        assert(mk8_snapshot_allocate(&ci,&ai,&image,&memory)==results[n]);
        assert(calls==counts[n] && collections==(cases[n][0]==-2));
        assert(ai.usage==1 && ai.flags==7);
        if(!results[n]) assert(image==123 && memory==456);
        else assert(!image && !memory);
    }
}
'''
            c, exe = root/'snapshot.c', root/'snapshot.exe'
            c.write_text(harness, encoding='utf-8')
            built = subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',str(c),'-o',str(exe)],
                env=compiler_env(),capture_output=True,text=True)
            self.assertEqual(built.returncode,0,built.stderr)
            run = subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
