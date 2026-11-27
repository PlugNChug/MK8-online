"""Exercise the real renderer's H/V dispatch, locked sampler, and translated fragment programs."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
import castle_bloom
import castle_blur
from native import DEFAULT_COMPILER, TOOLS, compiler_env

DEPS = TOOLS/'deps/packages/ucrt64'
HARNESS = r'''
#include "translate.c"
#include "kernel_weights.h"
#include <assert.h>
#include <shaderc/shaderc.h>

static void check_program(ShaderInfo *info, int axis) {
    assert(info && info->msl && info->uses_rscale == (axis != 0));
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, info->msl, strlen(info->msl),
        shaderc_fragment_shader, "blur.frag", "main", opts);
    if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
        fprintf(stderr,"%s\n",shaderc_result_get_error_message(result)); abort();
    }
    const uint32_t *p = (const uint32_t *)shaderc_result_get_bytes(result);
    size_t end = shaderc_result_get_length(result)/4; unsigned explicit_lod = 0, original = 0;
    for (size_t i=5; i<end;) {
        unsigned op=p[i]&65535, words=p[i]>>16; assert(words && words <= end-i);
        if (op==88) explicit_lod++;  /* OpImageSampleExplicitLod */
        if (op==87) original++;      /* OpImageSampleImplicitLod, retained fallback */
        i+=words;
    }
    assert(original && (explicit_lod != 0) == (axis != 0));
    if (axis==MK8_BLUR_REDUCTION) assert(explicit_lod==4);
    else if (axis) assert(strstr(info->msl, axis==1 ? "horizontal = true" : "horizontal = false"));
    shaderc_result_release(result); shaderc_compile_options_release(opts); shaderc_compiler_release(compiler);
    maxwell_free(info);
}

static ShaderInfo *fixture(unsigned axis) {
    Ctx *c=calloc(1,sizeof *c); ShaderInfo *info=calloc(1,sizeof *info);
    ShaderKey key={0}; key.glsl=true; c->info=info; c->key=&key; c->glsl=true;
    c->mk8_blur_program=axis; c->uses_rscale=axis==MK8_BLUR_REDUCTION;
    c->stage=info->stage=STAGE_FRAGMENT; c->in_attr_mask=3;
    c->omap_target=15; info->cbuf_mask=2; info->cbuf_size[1]=16;
    info->ntex=1; info->tex[0].type=TEXTYPE_2D;
    for (int i=0;i<4;i++) c->reg_used[i]=true;
    sp(&c->body, "    vec4 col = texture(t0,v0.xy);\n"
                 "    r0=as_u(col.r); r1=as_u(col.g); r2=as_u(col.b); r3=as_u(col.a);\n");
    return glsl_finish(c,false,false,false);
}

int main(int argc,char **argv) {
    /* Compare the actual GLSL axis packing to a direct binomial convolution
       of a unit impulse, including texel centers and non-half DRS phases. */
    for (unsigned phase=0;phase<256;phase++) {
        float q=phase/256.f;
        TestAxis a=mk8_first_down_axis(5.5f+q);
        for (int i=-1;i<=2;i++) {
            float center=5.5f+i;
            float got=(1-a.z)*fmaxf(0.f,1-fabsf(a.x-center))+a.z*fmaxf(0.f,1-fabsf(a.y-center));
            float expected[]={.25f*(1-q),.25f*(2-q),.25f*(1+q),.25f*q};
            assert(fabsf(got-expected[i+1])<1e-6f);
        }
    }
    assert(mk8_blur_kernel(0x3d8fe9dcu,0x3e6879c6u,0x3ea1e718u));
    assert(!mk8_blur_kernel(0u,0x3f000000u,0u));  /* positive, asymmetric stock gain */
    assert(!mk8_blur_kernel(0x3d8fe9dcu,0x3e6879c7u,0x3ea1e718u));
    assert(!mk8_blur_kernel(0x3d8fe9dcu,0x3e6879c6u,0x3ea1e717u));
    Mk8BlurSequence s={0};
    uint64_t seed=0x051303030c68828full, down=0x75ee7f36aed0f6f0ull;
    uint64_t h=0xb91aecdd33397b59ull, v=0xa0b620fe821bd4bfull;
    Mk8BlurSequence submitted={0}, pending=mk8_blur_begin(&submitted);
    assert(!mk8_blur_draw(&pending,1,seed,false));   /* seed pipeline skipped after lookup */
    pending=mk8_blur_begin(&submitted);
    assert(!mk8_blur_draw(&pending,1,down,false));
    pending=mk8_blur_begin(&submitted);
    assert(!mk8_blur_draw(&pending,1,seed,false));
    submitted=pending;                       /* seed recorded; intervening draw skipped before lookup */
    (void)mk8_blur_begin(&submitted);
    pending=mk8_blur_begin(&submitted);
    assert(!mk8_blur_draw(&pending,1,down,false));
    assert(!mk8_blur_draw(&s,1,h,false) && !mk8_blur_draw(&s,1,v,false));
    assert(!mk8_blur_draw(&s,1,seed,false) && !mk8_blur_draw(&s,2,down,false));
    assert(!mk8_blur_draw(&s,1,seed,false) && !mk8_blur_draw(&s,1,123,false) && !mk8_blur_draw(&s,1,down,false));
    assert(!mk8_blur_draw(&s,1,seed,false) && mk8_blur_draw(&s,1,down,false)==MK8_BLUR_REDUCTION);
    assert(mk8_blur_draw(&s,1,h,false)==MK8_BLUR_HORIZONTAL && mk8_blur_draw(&s,1,v,false)==MK8_BLUR_VERTICAL);
    assert(!mk8_blur_draw(&s,1,down,false) && !mk8_blur_draw(&s,1,h,false) && !mk8_blur_draw(&s,1,v,false));
    assert(!mk8_blur_draw(&s,1,seed,false) && mk8_blur_draw(&s,1,down,false)==MK8_BLUR_REDUCTION && !mk8_blur_draw(&s,2,h,false));
    assert(!mk8_blur_draw(&s,2,seed,false) && mk8_blur_draw(&s,2,down,false)==MK8_BLUR_REDUCTION &&
           !mk8_blur_draw(&s,2,123,false) && !mk8_blur_draw(&s,2,h,false));
    uint64_t vancouver_seed=0x0e1fa9b3719cf15bull;
    assert(!mk8_blur_draw(&s,3,vancouver_seed,false) && !mk8_blur_draw(&s,3,down,false));
    assert(!mk8_blur_draw(&s,4,vancouver_seed,true) && mk8_blur_draw(&s,4,down,false)==MK8_BLUR_REDUCTION);
    assert(mk8_blur_draw(&s,4,h,false)==MK8_BLUR_HORIZONTAL && mk8_blur_draw(&s,4,v,false)==MK8_BLUR_VERTICAL);
    assert(!mk8_blur_draw(&s,4,down,false) && !mk8_blur_draw(&s,4,h,false) && !mk8_blur_draw(&s,4,v,false));
    for (int i=0;i<=15;i++) assert(mk8_blur_locked_lod(i,i,true)==i);
    assert(mk8_blur_locked_lod(0,1,true)<0 && mk8_blur_locked_lod(-1,-1,true)<0);
    assert(mk8_blur_locked_lod(.5f,.5f,true)<0 && mk8_blur_locked_lod(1,1,false)<0);
    assert(mk8_blur_locked_lod(16,16,true)<0 && mk8_blur_locked_lod(NAN,NAN,true)<0);
    uint8_t blob[4096]={0};
    assert(!mk8_blur_program(blob,sizeof blob,50) && !mk8_blur_program(blob,16,50));
    for (unsigned axis=0; axis<=3;axis++) check_program(fixture(axis),axis);
    if (argc==2) {
        FILE *f=fopen(argv[1],"rb"); assert(f);
        const long offsets[]={0x145400,0x145b00,0x146100};
        const unsigned words[]={50,50,16};
        ShaderKey key={0}; key.glsl=true; key.vs_out_mask=~0u;
        for (unsigned axis=1;axis<=3;axis++) {
            assert(!fseek(f,offsets[axis-1],SEEK_SET) && fread(blob,1,sizeof blob,f)==sizeof blob);
            assert(mk8_blur_program(blob,sizeof blob,words[axis-1])==axis &&
                   !mk8_blur_program(blob,sizeof blob,words[axis-1]-1));
            check_program(maxwell_translate(blob,sizeof blob,STAGE_FRAGMENT,&key),axis);
            blob[0x31]^=1;
            assert(!mk8_blur_program(blob,sizeof blob,words[axis-1]));
        }
        fclose(f);
    }
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS/'include/shaderc/shaderc.h').is_file(), 'Native shader tools required')
class CastleBlurTests(unittest.TestCase):
    def test_filters_and_first_pair(self):
        upstream = Path(os.environ.get('MK8_TEST_RUNTIME_SOURCE', ROOT/'upstream/smm2recomp/recomp'))/'runtime'
        with tempfile.TemporaryDirectory() as d:
            temp = Path(d)
            runtime = temp/'runtime'
            shutil.copytree(upstream/'nvn/maxwell', runtime/'nvn/maxwell')
            vulkan = runtime/'nvn/vulkan'
            vulkan.mkdir()
            for name in ('exec.c','textures.c','vk.h'):
                shutil.copyfile(upstream/'nvn/vulkan'/name, vulkan/name)
            castle_bloom.prepare(runtime)
            castle_blur.prepare(runtime)
            src = temp/'check.c'
            # This GLSL predicate uses only unsigned comparisons, so the native test
            # can also call the exact predicate executed by the translated shader.
            shader = (castle_blur.GRAPHICS/'castle_blur.glsl').read_text(encoding='utf-8')
            start = shader.index('bool mk8_blur_kernel(')
            end = shader.index('\n}',start)+2
            down = (castle_blur.GRAPHICS/'bloom_first_down.glsl').read_text(encoding='utf-8')
            axis = down[down.index('vec3 mk8_first_down_axis('):down.index('\n}')+2]
            (temp/'kernel_weights.h').write_text(shader[start:end].replace('uint ', 'uint32_t ') +
                '\ntypedef struct {float x,y,z;} TestAxis;\n#define vec3(x,y,z) ((TestAxis){x,y,z})\n' +
                axis.replace('vec3 mk8_', 'TestAxis mk8_'),
                encoding='utf-8')
            src.write_text(HARNESS, encoding='utf-8')
            env = dict(compiler_env(), PATH=str(DEPS/'bin')+os.pathsep+os.environ['PATH'])
            exe = temp/'check.exe'
            subprocess.run([DEFAULT_COMPILER,'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                            '-I'+str(runtime/'nvn/maxwell'),'-I'+str(DEPS/'include'),src,
                            DEPS/'lib/libshaderc_shared.dll.a','-o',exe],env=env,check=True)
            archive = os.environ.get('MK8_TEST_CASTLE_SHADER_ARCHIVE')
            subprocess.run([exe,*([archive] if archive else [])],env=env,check=True)
