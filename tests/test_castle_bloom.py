"""Compile the paired-sample extraction and the original single-sample path to Vulkan SPIR-V."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from castle_bloom import prepare
from native import DEFAULT_COMPILER, TOOLS, compiler_env

DEPS = TOOLS/'deps/packages/ucrt64'
HARNESS = r'''
#include "translate.c"
#include <assert.h>
#include <shaderc/shaderc.h>
#include "reduce_axis.h"

static void check_area(int src, int dst) {
    float values[1024];
    for (int i=0; i<src; i++) values[i] = (i*17%19)/18.f;
    for (int i=0; i<dst; i++) {
        float width=(float)src/dst, left=i*width, right=(i+1)*width, exact=0;
        Reduction r=mk8_reduce_axis((i+.5f)*width,width);
        for (int j=(int)floorf(left); j<(int)ceilf(right) && j<src; j++)
            exact+=values[j]*(fminf(right,j+1.f)-fmaxf(left,j))/width;
        float sampled[2], centers[2]={r.x,r.y};
        for (int j=0; j<2; j++) {
            float x=fminf(src-1.f,fmaxf(0,centers[j]-.5f));
            int k=(int)x;
            sampled[j]=values[k]*(1-(x-k))+values[k+1<src?k+1:k]*(x-k);
        }
        assert(fabsf(sampled[0]*(1-r.z)+sampled[1]*r.z-exact)<.0002f);
        if (src==2*dst) assert(fabsf(exact-(values[2*i]+values[2*i+1])*.5f)<.0002f);
    }
}

static float smooth(float low, float high, float x) {
    float t = fminf(1, fmaxf(0, (x-low)/(high-low)));
    return t*t*(3-2*t);
}

static void check_spread(const char *source) {
    const char *expr = strstr(source,"float spread = smoothstep(");
    float a,b,c,d;
    assert(expr && sscanf(expr,"float spread = smoothstep(%f, %f, min(footprint.x, footprint.y)) "
                          "* (1.0 - smoothstep(%f, %f, max(footprint.x, footprint.y)))", &a,&b,&c,&d)==4);
    assert(a==1 && b<=270.0f/136.0f && b>a && c>=2 && d>c);
    assert(strstr(source,"dx *= 0.25 * spread; dy *= 0.25 * spread;"));
    // Sweep both independently changing axes, including all four curve boundaries.
    for (int axis=0;axis<2;axis++) {
        float prev=0;
        for (int i=0;i<=1400;i++) {
            float variable=0.5f+i/400.0f, other=axis ? 270.0f/136.0f : 2.0f;
            float spread=smooth(a,b,fminf(variable,other))*(1-smooth(c,d,fmaxf(variable,other)));
            assert(spread>=0 && spread<=1 && fabsf(spread-prev)<0.01f);
            if (variable<=1 || variable>=3) assert(spread==0);
            if (variable>=270.0f/136.0f && variable<=2) assert(spread==1);
            prev=spread;
        }
    }
}

static void check(ShaderInfo *si, bool filtered, bool reduce) {
    assert(si && si->msl && si->uses_rscale == (filtered || reduce));
    assert((strstr(si->msl,"mk8_reduce_sample(")!=NULL)==reduce);
    if (filtered) check_spread(si->msl);
    shaderc_compiler_t compiler=shaderc_compiler_initialize();
    shaderc_compile_options_t options=shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(options,shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t result=shaderc_compile_into_spv(compiler,si->msl,strlen(si->msl),
        shaderc_fragment_shader,"bloom.frag","main",options);
    if (shaderc_result_get_compilation_status(result)!=shaderc_compilation_status_success) {
        fprintf(stderr,"%s\n",shaderc_result_get_error_message(result)); abort();
    }
    const uint32_t *words=(const uint32_t *)shaderc_result_get_bytes(result);
    size_t len=shaderc_result_get_length(result)/4;
    unsigned fn=0, calls=0, explicit_samples=0;
    for (size_t i=5;i<len;) {
        unsigned op=words[i]&65535, n=words[i]>>16;
        assert(n && n<=len-i);
        if (op==5 && !strncmp((const char *)(words+i+2),"mk8_castle_sample(",18)) fn=words[i+1];
        if (op==57 && fn && words[i+3]==fn) calls++;
        if (op==88) explicit_samples++;
        i+=n;
    }
    assert(calls==(filtered?5:0)); // Four shifted evaluations and the original center fallback.
    if (reduce) assert(explicit_samples==4);
    shaderc_result_release(result);shaderc_compile_options_release(options);shaderc_compiler_release(compiler);
    maxwell_free(si);
}

static ShaderInfo *fixture(bool filtered, bool reduce) {
    Ctx *c=calloc(1,sizeof *c);
    ShaderInfo *si=calloc(1,sizeof *si);
    ShaderKey key={0};key.glsl=true;
    c->key=&key;c->glsl=true;c->info=si;c->stage=STAGE_FRAGMENT;
    c->mk8_bloom_reduce=reduce;c->uses_rscale=reduce;
    c->in_attr_mask=1;c->omap_target=15;
    for (int i=0;i<4;i++) c->reg_used[i]=true;
    si->stage=STAGE_FRAGMENT;si->ntex=1;si->tex[0].type=TEXTYPE_2D;
    sp(&c->body,"    vec4 color = texture(t0,v0.xy);\n"
        "    r0=as_u(color.r); r1=as_u(color.g); r2=as_u(color.b); r3=as_u(color.a);\n");
    return glsl_finish(c,false,false,filtered);
}

int main(int argc,char **argv) {
    check_area(400,200);check_area(405,202);check_area(885,442);check_area(498,248);
    uint8_t blob[4096]={0};
    assert(!mk8_castle_seed(blob,sizeof blob,30) && !mk8_castle_seed(blob,sizeof blob,31));
    assert(!mk8_castle_seed(blob,16,31));
    check(fixture(false,false),false,false);check(fixture(true,false),true,false);
    check(fixture(false,true),false,true);
    if (argc==2) {
        // Optional private check uses an operator-supplied 4.0.1 shader archive.
        FILE *f=fopen(argv[1],"rb");assert(f);ShaderKey key={0};key.glsl=true;key.vs_out_mask=~0u;
        const long offsets[]={0x12e500,0x136100};
        for (int i=0;i<2;i++) {
            assert(!fseek(f,offsets[i],SEEK_SET) && fread(blob,1,sizeof blob,f)==sizeof blob);
            assert(mk8_castle_seed(blob,sizeof blob,31)==(i==0));
            check(maxwell_translate(blob,sizeof blob,STAGE_FRAGMENT,&key),i==0,false);
        }
        assert(!fseek(f,0x146200,SEEK_SET) && fread(blob,1,sizeof blob,f)==sizeof blob);
        assert(mk8_castle_reduce(blob,sizeof blob,11) && !mk8_castle_reduce(blob,sizeof blob,12));
        check(maxwell_translate(blob,sizeof blob,STAGE_FRAGMENT,&key),false,true);
        blob[0x37]^=1;assert(!mk8_castle_reduce(blob,sizeof blob,11));
        fclose(f);
    }
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS/'include/shaderc/shaderc.h').is_file(), 'Native shader tools required')
class CastleBloomTests(unittest.TestCase):
    def test_fragment_variants(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            original = Path(os.environ.get('MK8_TEST_RUNTIME_SOURCE', ROOT/'upstream/smm2recomp/recomp'))/'runtime'
            runtime = out/'runtime'
            shutil.copytree(original/'nvn/maxwell', runtime/'nvn/maxwell')
            (runtime/'nvn/vulkan').mkdir()
            shutil.copyfile(original/'nvn/vulkan/exec.c', runtime/'nvn/vulkan/exec.c')
            prepare(runtime)
            src = out/'check.c'
            shader=(ROOT/'runtime/mk8/graphics/bloom_reduce.glsl').read_text(encoding='utf-8')
            axis=shader[shader.index('vec3 mk8_reduce_axis('):shader.index('\n}',shader.index('vec3 mk8_reduce_axis('))+2]
            (out/'reduce_axis.h').write_text(
                'typedef struct { float x,y,z; } Reduction;\n'
                '#define vec3(x,y,z) (Reduction){x,y,z}\n#define clamp(x,l,h) fminf(fmaxf(x,l),h)\n'
                '#define min fminf\n#define max fmaxf\n#define floor floorf\n'+axis.replace('vec3 mk8_', 'Reduction mk8_')+
                '\n#undef vec3\n#undef clamp\n#undef min\n#undef max\n#undef floor\n',encoding='utf-8')
            src.write_text(HARNESS, encoding='utf-8')
            env = dict(compiler_env(), PATH=str(DEPS/'bin')+os.pathsep+os.environ['PATH'])
            exe = out/'check.exe'
            subprocess.run([DEFAULT_COMPILER,'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                            '-I'+str(runtime/'nvn/maxwell'),'-I'+str(DEPS/'include'),src,
                            DEPS/'lib/libshaderc_shared.dll.a','-o',exe],env=env,check=True)
            private = os.environ.get('MK8_TEST_CASTLE_SHADER_ARCHIVE')
            subprocess.run([exe, *([private] if private else [])],env=env,check=True)
