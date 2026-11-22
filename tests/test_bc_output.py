"""The reflection block encoder exports four raw Maxwell registers to its integer target."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start)+2] + '\n'


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native shader tools required')
class BcOutputTests(unittest.TestCase):
    def test_block_encoding_preserves_all_register_words(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',out)
            execute = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            key = function(execute,'static bool program_keys(')
            code = '''
#include <assert.h>
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>
#include "translate.c"
typedef struct { VkFormat format; VkImage image; } TexBackend;
typedef struct { uint32_t format; TexBackend *backend; } Texture;
typedef Texture NvnTexture;
typedef struct { unsigned setmask,format; } NvnTextureView;
typedef struct { struct { Texture *color[8]; int ncolor; bool has_cview[8]; NvnTextureView cview[8]; } rt; } ExecState;
static VkFormat vk_format(unsigned f) {
    switch(f) {
    case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x2f: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x2e: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}
static TexBackend allocated;
static unsigned allocations;
static VkFormat mk8_bc_backing_format(NvnTexture *t) {
    if (!t->backend || !t->backend->image) { allocations++; t->backend=&allocated; allocated.image=(VkImage)(uintptr_t)1; }
    return t->backend->format;
}
''' + function(execute,'static uint32_t mk8_bc_block_bytes(') + function(execute,'static uint32_t mk8_bc_texel_bytes(') + function(execute,'static VkFormat mk8_bc_format(') + function(execute,'static int rt_kind(')
            code += function(execute,'static int mk8_bc_output_kind(')
            select = key[key.index('    for (int i = 0; i < s->rt.ncolor'):key.rindex('}')]
            code += 'static bool output_keys(ExecState *s, ShaderKey *fkey,bool scan) {\n'+select+'}\n'
            code += r'''
int main(void) {
    Texture t={.format=0x44}; ExecState live={0}; live.rt.ncolor=1;live.rt.color[0]=&t;
    live.rt.has_cview[0]=true;live.rt.cview[0]=(NvnTextureView){.setmask=4,.format=0x2f};
    ShaderKey k={.glsl=true};
    assert(!output_keys(&live,&k,true) && allocations==0);
    allocated.format=VK_FORMAT_R8G8B8A8_UNORM;assert(output_keys(&live,&k,false) && allocations==1 && k.rt_kind[0]==0);
    assert(output_keys(&live,&k,true) && allocations==1 && k.rt_kind[0]==0);
    allocated.image=NULL;allocated.format=VK_FORMAT_BC3_UNORM_BLOCK;
    assert(!output_keys(&live,&k,true) && allocations==1);
    assert(output_keys(&live,&k,false) && allocations==2 && k.rt_kind[0]==2);
    assert(output_keys(&live,&k,true) && k.rt_kind[0]==2);
    live.rt.cview[0].format=0x30;output_keys(&live,&k,false);assert(k.rt_kind[0]==1);
    live.rt.cview[0].format=0x2e;output_keys(&live,&k,false);assert(k.rt_kind[0]==0);
    live.rt.cview[0].format=0x2f;live.rt.cview[0].setmask=0;output_keys(&live,&k,false);assert(k.rt_kind[0]==0);
    live.rt.cview[0].setmask=4;output_keys(&live,&k,false);
    const uint32_t words[4]={0x7fc12345u,0xff800000u,0x80000000u,0xdeadbeefu};
    Ctx *c=calloc(1,sizeof *c);
    c->key=&k;c->glsl=true;c->stage=STAGE_FRAGMENT;c->omap_target=15;c->info=calloc(1,sizeof *c->info);
    for(unsigned i=0;i<4;i++) {c->reg_used[i]=1;sp(&c->body,"  r%u = 0x%08xu;\n",i,words[i]);}
    ShaderInfo *info=glsl_finish(c,false,false);
    assert(info && info->msl && strstr(info->msl,"layout(location = 0) out uvec4 c0;"));
    shaderc_compiler_t cc=shaderc_compiler_initialize();
    shaderc_compile_options_t opts=shaderc_compile_options_initialize();
    shaderc_compile_options_set_optimization_level(opts,shaderc_optimization_level_performance);
    shaderc_compilation_result_t r=shaderc_compile_into_spv(cc,info->msl,strlen(info->msl),shaderc_fragment_shader,"block.frag","main",opts);
    if(shaderc_result_get_compilation_status(r)!=shaderc_compilation_status_success) fprintf(stderr,"%s\n",shaderc_result_get_error_message(r));
    assert(shaderc_result_get_compilation_status(r)==shaderc_compilation_status_success);
    const uint32_t *p=(const uint32_t*)shaderc_result_get_bytes(r);size_t end=shaderc_result_get_length(r)/4;
    uint32_t uint_type=0,vec_type=0,out_ptr=0,output=0,literals[4]={0},offsets[4]={0},dest[4]={0},literal_vec=0,mask=0;
    for(size_t i=5;i<end;i+=p[i]>>16) {
        unsigned op=p[i]&65535,n=p[i]>>16;assert(n && n<=end-i);
        if(op==21 && n==4 && p[i+2]==32 && p[i+3]==0) uint_type=p[i+1];
        if(op==23 && n==4 && p[i+2]==uint_type && p[i+3]==4) vec_type=p[i+1];
        if(op==32 && n==4 && p[i+2]==3 && p[i+3]==vec_type) out_ptr=p[i+1];
        if(op==59 && n>=4 && p[i+1]==out_ptr && p[i+3]==3) output=p[i+2];
        if(op==43 && n==4) {
            for(unsigned j=0;j<4;j++) if(p[i+1]==uint_type && p[i+3]==words[j]) literals[j]=p[i+2];
            if(p[i+3]<4) offsets[p[i+3]]=p[i+2];
        }
        if(op==44 && n==7 && p[i+1]==vec_type && !memcmp(p+i+3,literals,sizeof literals)) literal_vec=p[i+2];
        if(op==65 && n==5 && p[i+3]==output)
            for(unsigned j=0;j<4;j++) if(p[i+4]==offsets[j]) dest[j]=p[i+2];
        if(op==62 && n==3) {
            if(literal_vec && p[i+1]==output && p[i+2]==literal_vec) mask=15;
            for(unsigned j=0;j<4;j++) if(dest[j] && p[i+1]==dest[j] && p[i+2]==literals[j]) mask|=1u<<j;
        }
    }
    assert(output && mask==15);
    shaderc_result_release(r);shaderc_compile_options_release(opts);shaderc_compiler_release(cc);maxwell_free(info);
}
'''
            src=out/'block.c'
            src.write_text(code,encoding='utf-8')
            env=dict(compiler_env(),PATH=str(DEPS/'bin')+os.pathsep+os.environ['PATH'])
            exe=out/'block.exe'
            subprocess.run([DEFAULT_COMPILER,'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                            '-I'+str(runtime/'nvn/maxwell'),'-I'+str(DEPS/'include'),src,
                            DEPS/'lib/libshaderc_shared.dll.a','-o',exe],env=env,check=True,timeout=120)
            subprocess.run([exe],env=env,check=True,timeout=20)
