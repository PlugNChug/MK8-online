"""Point lists need an actual PointSize store, including when the guest VS omits it."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env

CHECK = r'''
#include "translate.c"
#include <assert.h>
#include <shaderc/shaderc.h>

// A gl_PerVertex declaration alone doesn't satisfy Vulkan. Check both the member
// decoration and an actual OpStore through its member pointer in compiled SPIR-V.
static void check_vertex(bool guest_size) {
    ShaderKey key = {.glsl=true};
    Ctx *ctx = calloc(1,sizeof *ctx);
    ShaderInfo *info = ctx->info = calloc(1,sizeof *info);
    ctx->key=&key;ctx->glsl=true;ctx->stage=STAGE_VERTEX;
    if(guest_size) attr_store(ctx,0,0x6c);
    info=glsl_finish(ctx,false,false,false);
    assert(info->msl && info->writes_point_size==guest_size);
    shaderc_compiler_t cc=shaderc_compiler_initialize();
    shaderc_compilation_result_t r=shaderc_compile_into_spv(cc,info->msl,strlen(info->msl),
        shaderc_vertex_shader,"point.vert","main",NULL);
    if(shaderc_result_get_compilation_status(r)!=shaderc_compilation_status_success)
        fprintf(stderr,"%s\n",shaderc_result_get_error_message(r));
    assert(shaderc_result_get_compilation_status(r)==shaderc_compilation_status_success);
    const uint32_t *p=(const uint32_t*)shaderc_result_get_bytes(r);
    size_t end=shaderc_result_get_length(r)/4;
    uint32_t block_type=0, output_type=0, block=0, member=~0u,index=0,ptr=0,spec=0,stores=0;
    bool spec_store=false;
    for (size_t i=5;i<end;) {
        unsigned words=p[i]>>16,op=p[i]&65535;assert(words && words<=end-i);
        if(op==72 && words==5 && p[i+3]==11 && p[i+4]==1) { block_type=p[i+1];member=p[i+2]; } // PointSize
        if(op==32 && words==4 && p[i+2]==3 && p[i+3]==block_type) output_type=p[i+1]; // Output pointer
        if(op==59 && words>=4 && p[i+1]==output_type && p[i+3]==3) block=p[i+2]; // gl_PerVertex
        if(op==71 && words==4 && p[i+2]==1 && p[i+3]==0) spec=p[i+1];   // SpecId 0
        if(op==43 && words==4 && p[i+3]==member) index=p[i+2];           // OpConstant
        if(op==65 && words==5 && index && block && p[i+3]==block && p[i+4]==index) ptr=p[i+2];     // OpAccessChain
        if(op==62 && ptr && p[i+1]==ptr) { stores++;spec_store|=spec && p[i+2]==spec; }
        i+=words;
    }
    assert(stores>0);
    assert(guest_size ? (!spec && stores>=2) : spec_store);
    shaderc_result_release(r);shaderc_compiler_release(cc);maxwell_free(info);
}
int main(void) { check_vertex(false); check_vertex(true); }
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS/'include/shaderc/shaderc.h').is_file(), 'Native shader tools required')
class PointSizeTests(unittest.TestCase):
    def test_point_size_store_in_spirv(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',out)
            src = out/'point.c'
            src.write_text(CHECK,encoding='utf-8')
            env=dict(compiler_env(),PATH=str(DEPS/'bin')+os.pathsep+os.environ['PATH'])
            exe=out/'point.exe'
            subprocess.run([DEFAULT_COMPILER,'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                            '-I'+str(runtime/'nvn/maxwell'),'-I'+str(DEPS/'include'),src,
                            DEPS/'lib/libshaderc_shared.dll.a','-o',exe],check=True,env=env)
            subprocess.run([exe],check=True,env=env,timeout=10)

    def test_shared_draw_and_prefetch_state(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',out)
            exec_c = (runtime/'nvn/vulkan/exec.c').read_text()
            key = exec_c[exec_c.index('static float fixed_point_key('):exec_c.index('static void pipe_key(')]
            set_size = exec_c[exec_c.index('case CMD_SET_POINT_SIZE:'):].splitlines()[0]
            initial = exec_c[exec_c.index('st.fixed_point_size ='):].splitlines()[0]
            vertex_spec = exec_c[exec_c.index('    VkSpecializationMapEntry point_entry'):exec_c.index('    VkVertexInputAttributeDescription va[32];')]
            src = out/'state.c'
            src.write_text(r'''#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>
typedef struct { float fixed_point_size; } State;
typedef struct {float f[4];} Command;
enum {CMD_SET_POINT_SIZE};
static bool apply(State *s, int op, const Command *r) {
    switch (op) {''' + set_size + r''' } return false;
}
''' + key + r'''
static void check_specialization(const State *k) {
    VkPipelineShaderStageCreateInfo stages[3]={0};
''' + vertex_spec + r'''
    assert(!stages[1].pSpecializationInfo && !stages[2].pSpecializationInfo);
    const VkSpecializationInfo *spec=stages[0].pSpecializationInfo;
    if (k->fixed_point_size==0) { assert(!spec); return; }
    assert(spec && spec->mapEntryCount==1 && spec->pMapEntries[0].constantID==0);
    assert(spec->pMapEntries[0].offset==0 && spec->pMapEntries[0].size==sizeof(float));
    assert(spec->dataSize==sizeof(float) && *(const float*)spec->pData==k->fixed_point_size);
}
int main(void) {
    State st={0};
    ''' + initial + r'''
    assert(st.fixed_point_size==1);
    Command cmd={.f={2.24f}};
    State scan=st;assert(apply(&st,CMD_SET_POINT_SIZE,&cmd) && apply(&scan,CMD_SET_POINT_SIZE,&cmd));
    assert(scan.fixed_point_size==st.fixed_point_size && st.fixed_point_size==cmd.f[0]);
    VkPhysicalDeviceLimits l={.pointSizeRange={1,64},.pointSizeGranularity=.25f};
    #define KEY(size) fixed_point_key(0,false,false,(size),true,&l)
    assert(KEY(st.fixed_point_size)==2.25f && KEY(2.26f)==2.25f && KEY(1)==0);
    assert(KEY(200)==64 && KEY(-1)==0 && KEY(NAN)==0 && KEY(INFINITY)==0);
    assert(fixed_point_key(2,false,false,20,true,&l)==0);
    assert(fixed_point_key(0,true,false,20,true,&l)==0);
    assert(fixed_point_key(0,false,true,20,true,&l)==0);
    assert(fixed_point_key(0,false,false,20,false,&l)==0);
    l.pointSizeGranularity=0;
    assert(KEY(2.24f)==2.24f);
    check_specialization(&(State){0});
    check_specialization(&(State){.fixed_point_size=KEY(st.fixed_point_size)});
}
''',encoding='utf-8')
            exe=out/'state.exe'
            env=compiler_env()
            subprocess.run([DEFAULT_COMPILER,'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                            '-I'+str(DEPS/'include'),src,'-o',exe],check=True,env=env)
            subprocess.run([exe],check=True,env=env,timeout=10)
