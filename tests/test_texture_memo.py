"""A view-cache hit must still service texture contents and image recreation."""
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


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class TextureMemoTests(unittest.TestCase):
    def test_update_and_reallocation_within_one_frame(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            text = (runtime/'nvn/vulkan/exec.c').read_text()
            function = text[text.index('static void texture_info('):text.index('// Writes the descriptor set of one stage.')]
            hashing = text[text.index('static uint64_t hash_words('):text.index('static uint64_t hmix(')]
            snapshots = text[text.index('static NvnTexture *snap_tex[8];'):text.index('typedef struct { NvnTexture *key;')]
            # (toolchain/texture_pool.py) the pool entry a sampler reads; no cube history in this fixture
            lookup = text[text.index('// MK8 (toolchain/texture_pool.py): the entry a sampler reads'):text.index('static NvnTexture *shader_texture_object(')]
            fixture = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "nvn/nvn.h"
#include "nvn/maxwell/shader.h"
static struct { uint64_t tex[6][32]; NvnDescPool *tpool,*spool; } st;
static int naliases,nfeedback;
static NvnTexture *feedback_tex[8];
typedef struct { uint32_t levels; VkFormat format; VkImageAspectFlags aspect; VkImage image; } TexBackend;
static TexBackend *feedback_copy[8];
static uint32_t snapshot_base, snapshot_levels;
static VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t base, uint32_t count,
                                 uint32_t layer, uint32_t layers, VkComponentMapping sw, VkImageAspectFlags aspect) {
 (void)b;(void)type;(void)fmt;(void)layer;(void)layers;(void)sw;(void)aspect;
 snapshot_base=base;snapshot_levels=count;return (VkImageView)(uintptr_t)555;
}
uint32_t vk_view_gen;
static int pending,contents,recreate,lookups,services;
static uintptr_t image_view=123;
uint64_t vk_frame_counter(void) { return 42; }
static NvnTexture *alias_read(NvnTexture *t) { return t; }
static void vk_tex(NvnTexture *t,bool render) {
 (void)t;(void)render;
 services++;
 if(pending) { contents=pending;pending=0; }
 if(recreate) { recreate=0;vk_view_gen++;image_view++; }
}
static VkImageView vk_view(NvnTexture *t,const NvnTextureView *v,bool has,bool render,int type) {
 (void)v;(void)has;(void)type;lookups++;vk_tex(t,render);return (VkImageView)image_view;
}
static VkImageView vk_shadow_view(NvnTexture *t) { (void)t;return VK_NULL_HANDLE; }
static void renderer_texture_scale(NvnTexture *t,float *x,float *y) { (void)t;*x=*y=1; }
static VkImageView vk_dummy_view(int type) { (void)type;return (VkImageView)(uintptr_t)99; }
static VkSampler vk_sampler(NvnSamplerDesc *s) { (void)s;return (VkSampler)(uintptr_t)456; }
static VkSampler vk_default_sampler(bool linear) { (void)linear;return (VkSampler)(uintptr_t)456; }
void rt_log(const char *fmt, ...) { (void)fmt; }
const NvnTexEntry *mk8_texpool_cube(NvnDescPool *d, uint32_t id) { (void)d;(void)id;return NULL; }
'''
            checks = r'''
int main(void) {
 NvnTexture texture={.width=64,.height=32};
 NvnTexEntry entries[2]={{0},{.tex=&texture}};
 NvnDescPool pool={.count=2,.entries=entries};st.tpool=&pool;st.tex[1][0]=1;
 ShaderInfo si={0};si.tex[0].handle_offset=0x20;si.tex[0].type=TEXTYPE_2D;
 VkDescriptorImageInfo descriptor={0};float scale[2];uint32_t wh[2];
 pending=10;texture_info(1,&si,0,&descriptor,scale,wh);
 assert(contents==10 && lookups==1 && services==1 && wh[0]==64 && wh[1]==32);
 VkImageView first=descriptor.imageView;
 pending=20;texture_info(1,&si,0,&descriptor,scale,wh);
 assert(contents==20 && descriptor.imageView==first && lookups==1 && services==2);
 recreate=1;texture_info(1,&si,0,&descriptor,scale,wh);
 assert(descriptor.imageView!=first && lookups==2);
 texture_info(1,&si,0,&descriptor,scale,wh);assert(lookups==2);
 TexBackend snapshot={.levels=5,.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32,.aspect=VK_IMAGE_ASPECT_COLOR_BIT};
 nfeedback=1;feedback_tex[0]=&texture;feedback_copy[0]=&snapshot;
 entries[1].has_view=1;entries[1].view.setmask=1;
 entries[1].view.base_level=2;entries[1].view.num_levels=1;
 texture_info(1,&si,0,&descriptor,scale,wh);
 assert(descriptor.imageView==(VkImageView)(uintptr_t)555 && snapshot_base==2 && snapshot_levels==1);
 snapshot.image=(VkImage)(uintptr_t)123;
 NvnTexture alias={.backend=&snapshot};
 snap_tex[0]=&texture;snap_ok[0]=true;mk8_snap_sources[0]=snapshot.image;
 snap_ok[1]=true;mk8_snap_sources[1]=(VkImage)(uintptr_t)456;
 snapshots_invalidate(&alias);
 assert(!snap_ok[0] && snap_ok[1]);
 st.tex[1][0]=0;texture_info(1,&si,0,&descriptor,scale,wh);
 assert(descriptor.imageView==(VkImageView)(uintptr_t)99 && wh[0]==0);
 return 0;
}
'''
            code=out/'memo.c'
            code.write_text(fixture+hashing+snapshots+lookup+function+checks)
            exe=out/'memo.exe'
            env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            env.pop('LARECOMP_VK_NO_TEX_MEMO',None)
            build=subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG','-I'+str(runtime),'-I'+str(DEPS/'include'),str(code),'-o',str(exe)],env=env,capture_output=True,text=True,timeout=90)
            self.assertEqual(build.returncode,0,build.stderr)
            result=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)


if __name__ == '__main__':
    unittest.main()
