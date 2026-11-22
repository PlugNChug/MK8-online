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
from native import DEFAULT_COMPILER, TOOLS


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class TextureMemoTests(unittest.TestCase):
    def test_shadow_array_copies_selected_real_values(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            exe = out/'shadow-array.exe'
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(TOOLS/'cache'))
            subprocess.run([str(DEFAULT_COMPILER),'cc','-O1','-UNDEBUG','-I'+str(runtime),
                            '-I'+str(DEPS/'include'),str(ROOT/'tests/shadow_array.c'),'-o',str(exe)],
                           check=True,env=env,timeout=90)
            subprocess.run([str(exe)],check=True,timeout=10)

    def test_dummy_image_format_dimensions_and_initial_contents(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            textures = (runtime/'nvn/vulkan/textures.c').read_text()
            view_types = textures[textures.index('static VkImageViewType view_type_for('):textures.index('// render: attachment view')]
            factory = textures[textures.index('// 1x1 black textures'):textures.index('// ================================================================== samplers')]
            (out/'dummy_factory.inc').write_text(view_types+factory)
            exe = out/'dummy.exe'
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(TOOLS/'cache'))
            subprocess.run([str(DEFAULT_COMPILER),'cc','-O1','-UNDEBUG','-I'+str(out),'-I'+str(runtime),
                            '-I'+str(DEPS/'include'),str(ROOT/'tests/shadow_dummy.c'),'-o',str(exe)],check=True,env=env,timeout=90)
            subprocess.run([str(exe)],check=True,timeout=10)


    def test_resolved_views_and_samplers(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            text = (runtime/'nvn/vulkan/exec.c').read_text()
            function = text[text.index('static void resolved_texture_info('):text.index('// Writes the descriptor set of one stage.')]
            pre = text[text.index('    VkDescriptorImageInfo fii[64];'):text.index('    snap_reason = "draw-depthwrite";')]
            late = text[text.index('    for (int i = 0;', text.index('uint64_t prof_tx_ =')):text.index('    if (prof) prof_tex +=')]
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
typedef struct { VkImageView depth;bool failed; } Mk8ShadowArrayResult;
static void mk8_shadow_array_written(VkImage image) { (void)image; }
static NvnTexture *feedback_tex[8];
typedef struct { uint32_t levels; VkFormat format; VkImageAspectFlags aspect; VkImage image; bool gpu_owned; } TexBackend;
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
static NvnTexture *alias_target;
static NvnTexture *alias_read(NvnTexture *t) { return alias_target ? alias_target : t; }
static bool is_attachment(NvnTexture *t) { return nfeedback && feedback_tex[0]==t; }
static VkFormat vk_format(uint32_t f) { return (VkFormat)f; }
static NvnTexture *copied_source;
static Mk8ShadowArrayResult mk8_shadow_array_prepare(NvnTexture *t,const NvnTextureView *v,bool has) {
 copied_source=t;
 return (Mk8ShadowArrayResult){.depth=(VkImageView)(uintptr_t)(999+(has?v->min_layer:0))};
}
static void vk_tex(NvnTexture *t,bool render) {
 (void)t;(void)render;
 services++;
 if(pending) { contents=pending;pending=0; }
 if(recreate) { recreate=0;vk_view_gen++;image_view++; }
}
static VkImageView vk_view(NvnTexture *t,const NvnTextureView *v,bool has,bool render,int type) {
 (void)v;(void)has;(void)type;lookups++;vk_tex(t,render);return (VkImageView)image_view;
}
static VkImageView shadow_copy;
static VkImageView vk_shadow_view(NvnTexture *t) { (void)t;return shadow_copy; }
static void renderer_texture_scale(NvnTexture *t,float *x,float *y) { (void)t;*x=*y=1; }
static VkImageView vk_dummy_view(int type) { return (VkImageView)(uintptr_t)(99+type); }
static VkImageView vk_shadow_dummy_view(int type) { return (VkImageView)(uintptr_t)(199+type); }
static bool vk_is_color_dummy_view(VkImageView view) {
 for(int t=TEXTYPE_1D;t<=TEXTYPE_1D_ARRAY;t++) if(view==vk_dummy_view(t)) return true;
 return false;
}
#include "nvn/castle_blur.h"
static uint64_t g_cur_fs_hash; static bool mk8_reduce_linear; static unsigned mk8_blur_target; static int mk8_blur_lod;
static VkSampler vk_sampler(NvnSamplerDesc *s,int *locked_lod) { (void)s;if(locked_lod) *locked_lod=-1;return (VkSampler)(uintptr_t)456; }
static VkSampler vk_default_sampler(bool linear) { (void)linear;return (VkSampler)(uintptr_t)457; }
static VkSampler vk_shadow_default_sampler(void) { return (VkSampler)(uintptr_t)458; }
void rt_log(const char *fmt, ...) { (void)fmt; }
const NvnTexEntry *mk8_texpool_cube(NvnDescPool *d, uint32_t id) { (void)d;(void)id;return NULL; }
'''
            draw_slots = r'''
static float tex_sc[2][64][2];static uint32_t tex_wh[2][64][2];
static bool draw_slots(ShaderInfo *fi,VkDescriptorImageInfo out[64]) {
 struct { ShaderInfo *fi; } program={fi}, *pr=&program;
 #define SKIP_DRAW() return false
''' + pre + r'''
 /* A busy view cache replaces the color view between preparation and binding. */
 image_view++;vk_view_gen++;
''' + late + r'''
 memcpy(out,fii,fi->ntex*sizeof *out);
 return true;
}
'''
            checks = r'''
static void test_shadow_fallback(void) {
 ShaderInfo si={0};si.ntex=1;si.tex[0].handle_offset=0x20;
 VkDescriptorImageInfo descriptor={0};float scale[2];uint32_t wh[2];
 for(int t=TEXTYPE_1D;t<=TEXTYPE_1D_ARRAY;t++) {
  if(t==TEXTYPE_3D) continue; /* No 3D comparison texture in the shader language. */
  si.tex[0].type=t;si.tex[0].shadow=false;
  texture_info(0,&si,0,&descriptor,scale,wh);
  assert(descriptor.imageView==vk_dummy_view(t) && descriptor.sampler==vk_default_sampler(true));
  si.tex[0].shadow=true;
  texture_info(0,&si,0,&descriptor,scale,wh);
  assert(descriptor.imageView==vk_shadow_dummy_view(t) && descriptor.sampler==vk_shadow_default_sampler());
 }

 /* Live views and the guest's real sampler must survive selecting a fallback for another slot. */
 NvnTexture texture={.width=64,.height=32};
 NvnTexEntry entries[2]={{0},{.tex=&texture}};NvnDescPool images={.count=2,.entries=entries};
 NvnSamplerDesc sampler={.compare_mode=1};NvnDescPool samplers={.count=1,.entries=&sampler};
 st.tpool=&images;st.spool=&samplers;st.tex[0][0]=1;si.tex[0].type=TEXTYPE_2D;
 image_view=(uintptr_t)vk_dummy_view(TEXTYPE_2D); /* vk_view rejected incompatible dimensions. */
 texture_info(0,&si,0,&descriptor,scale,wh);
 assert(descriptor.imageView==vk_shadow_dummy_view(TEXTYPE_2D) && descriptor.sampler==vk_sampler(&sampler,NULL));
 image_view=321;vk_view_gen++; /* vk_view resolved a live native depth view. */
 texture_info(0,&si,0,&descriptor,scale,wh);
 assert(descriptor.imageView==(VkImageView)image_view && descriptor.sampler==vk_sampler(&sampler,NULL));
 shadow_copy=(VkImageView)(uintptr_t)322; /* A supported R32/R16 source with a depth copy. */
 texture_info(0,&si,0,&descriptor,scale,wh);
 assert(descriptor.imageView==shadow_copy);
 si.tex[0].shadow=false;
 texture_info(0,&si,0,&descriptor,scale,wh);
 assert(descriptor.imageView==(VkImageView)image_view);
 st.tpool=st.spool=NULL;st.tex[0][0]=0;shadow_copy=VK_NULL_HANDLE;image_view=123;vk_view_gen++;
 lookups=services=0;
}
int main(void) {
 test_shadow_fallback();
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
 ShaderInfo mixed={.ntex=6};VkDescriptorImageInfo sets[64]={0};
 NvnTexture actual={.width=4,.height=4,.format=VK_FORMAT_R32_SFLOAT};
 NvnSamplerDesc sampler={.compare_mode=1};NvnDescPool samplers={.count=1,.entries=&sampler};st.spool=&samplers;
 naliases=1;alias_target=&actual;entries[1].has_view=1;entries[1].view.min_layer=2;
 for(int i=3;i<=5;i++) {mixed.tex[i].handle_offset=0x20;mixed.tex[i].type=TEXTYPE_2D_ARRAY;mixed.tex[i].shadow=i!=3;}
 assert(draw_slots(&mixed,sets));assert(copied_source==&actual);
 assert(sets[3].imageView==(VkImageView)image_view);
 assert(sets[4].imageView==(VkImageView)(uintptr_t)1001 && sets[5].imageView==sets[4].imageView);
 assert(sets[4].sampler==vk_sampler(&sampler,NULL) && tex_wh[1][4][0]==64);
 naliases=0;alias_target=NULL;entries[1].has_view=0;st.spool=NULL;
 assert(draw_slots(&mixed,sets));assert(sets[4].imageView==(VkImageView)image_view);
 si.tex[0].type=TEXTYPE_2D;
 TexBackend snapshot={.levels=5,.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32,.aspect=VK_IMAGE_ASPECT_COLOR_BIT};
 nfeedback=1;feedback_tex[0]=&texture;feedback_copy[0]=&snapshot;
 texture.format=VK_FORMAT_R32_SFLOAT;assert(draw_slots(&mixed,sets));
 assert(sets[4].imageView==vk_shadow_dummy_view(TEXTYPE_2D_ARRAY));
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
 assert(descriptor.imageView==vk_dummy_view(TEXTYPE_2D) && wh[0]==0);
 return 0;
}
'''
            code=out/'memo.c'
            code.write_text(fixture+hashing+snapshots+lookup+function+draw_slots+checks)
            exe=out/'memo.exe'
            env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(TOOLS/'cache'))
            env.pop('LARECOMP_VK_NO_TEX_MEMO',None)
            build=subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG','-I'+str(runtime),'-I'+str(DEPS/'include'),str(code),'-o',str(exe)],env=env,capture_output=True,text=True,timeout=90)
            self.assertEqual(build.returncode,0,build.stderr)
            result=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)


if __name__ == '__main__':
    unittest.main()
