"""Run the prepared NVN recorder, Vulkan state/key/pipeline, and prefetch traversal."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


def between(text, start, end):
    if text.count(start) != 1:
        raise ValueError('Depth-clamp fixture boundary changed: '+start)
    at = text.index(start)
    return text[at:text.index(end, at)]


PREFIX = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "nvn/nvn.h"
#include "nvn/maxwell/shader.h"
#include "nvn/buildsync.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static jmp_buf fatal_return;
static bool expect_fatal;
static char fatal_message[256];
static _Noreturn void test_fatal(const char *format,...) {
    va_list ap; va_start(ap,format); vsnprintf(fatal_message,sizeof fatal_message,format,ap); va_end(ap);
    if (expect_fatal) longjmp(fatal_return,1);
    fprintf(stderr,"unexpected fatal: %s\n",fatal_message); exit(1);
}
#undef FATAL
#define FATAL(...) test_fatal(__VA_ARGS__)
#undef LOG
#define LOG(...) ((void)0)
/* The allocator is opaque here; VkG and feature assignments come from the prepared runtime. */
typedef struct VmaAllocator_T *VmaAllocator;
/*VK_GLOBAL*/
static VkG g_vk;
static void set_clamp_capability(VkBool32 supported) {
    VkPhysicalDeviceFeatures enabled={.depthClamp=!supported}, available={.depthClamp=supported};
    VkPhysicalDeviceFeatures *b=&enabled, *h=&available;
    g_vk.depth_clamp=!supported; /* Detect a missing assignment in either direction. */
/*CAPABILITY*/
    CHECK(enabled.depthClamp==supported && available.depthClamp==supported);
    CHECK(g_vk.depth_clamp==(supported!=VK_FALSE));
}
typedef struct { ShaderInfo *vi,*fi; VkShaderModule vs,fs,gs; VkPipelineLayout layout; } Prog;
static _Atomic int stat_compiles;
static unsigned creations,async_finishes;
static VkBool32 observed_clamp;
static void mk8_pipeline_built(void) {} /* Cache-save bookkeeping is outside this pipeline-state test. */
static VkResult vkCreateGraphicsPipelines(VkDevice dev,VkPipelineCache cache,uint32_t count,
    const VkGraphicsPipelineCreateInfo *ci,const VkAllocationCallbacks *alloc,VkPipeline *out) {
    (void)dev; (void)cache; (void)alloc;
    CHECK(count==1 && ci->pRasterizationState && ci->pRasterizationState->rasterizerDiscardEnable==VK_FALSE);
    const VkPipelineRenderingCreateInfo *render=ci->pNext;
    CHECK(render->colorAttachmentCount==0 && render->depthAttachmentFormat==VK_FORMAT_D16_UNORM);
    creations++; observed_clamp=ci->pRasterizationState->depthClampEnable;
    *out=(VkPipeline)(uintptr_t)creations;
    return VK_SUCCESS;
}
void build_finish(_Atomic int *state,int result) { async_finishes++; atomic_store(state,result); }
static VkFormat vertex_format(uint32_t value) { (void)value; CHECK(false); return VK_FORMAT_UNDEFINED; }
static VkFormat vertex_format_supported(VkFormat value) { CHECK(false); return value; }
static NvnCmdBuf command_buffer;
static NvnCmdBuf *cb_of(uint64_t address) { (void)address; return &command_buffer; }
/*RECORDERS*/
/*STATE*/
static void mk8_rt_retain(const RtState *rt) { CHECK(!rt->ncolor && !rt->depth); }
static void mk8_rt_release(const RtState *rt) { CHECK(!rt->ncolor && !rt->depth); }
NvnDescPool *mk8_descriptor_retain(NvnDescPool *pool) { CHECK(!pool); return NULL; }
void mk8_descriptor_release(NvnDescPool *pool) { CHECK(!pool); }
static void mk8_executor_owners_reset(void) {} /* No descriptor pools are owned by the fixture. */
static void vk_end_pass(void) {}
/*DEFAULTS*/
/*LAYOUT*/
/*PIPE_TYPES*/
/*HASH*/
/*BLEND*/
/*RT_KIND*/
/*DYNAMIC*/
/*BUILD*/
/*KEY*/
/*APPLY*/

static ShaderInfo vertex_info,fragment_info;
static Prog program={.vi=&vertex_info,.fi=&fragment_info};
static PassLayout layout={.depth=VK_FORMAT_D16_UNORM,.samples=1};
static PipeKey key_for(const ExecState *state) {
    PipeKey key; pipe_key(state,&layout,&program,2,&key); return key;
}
static uint64_t key_hash(const PipeKey *key) {
    uint64_t hash=hash_words(0xcbf29ce484222325ull,key,sizeof *key); return hash?hash:1;
}
static void check_pipeline(PipeKey key,bool enabled,bool async) {
    Pipe pipe={.k=key,.async=async}; unsigned before=creations,finished=async_finishes;
    build_pipeline(&pipe);
    CHECK(creations==before+1 && observed_clamp==(enabled?VK_TRUE:VK_FALSE));
    CHECK(atomic_load(&pipe.state)==BUILD_READY && pipe.pipe);
    CHECK(async_finishes==finished+(unsigned)async);
}
static CmdRec *append_clamp(Cpu *cpu,uint64_t value) {
    uint64_t offset=command_buffer.buf_used;
    cpu->x[0]=0x12340000; cpu->x[1]=value;
    hle_nvnCommandBufferSetDepthClamp(cpu);
    CmdRec *record=(CmdRec *)(command_buffer.buf+offset);
    CHECK(record->op==CMD_SET_DEPTH_CLAMP && record->x[0]==value);
    CHECK(record->size==sizeof(CmdRec));
    return record;
}
static void append_op(Cpu *cpu,unsigned op) {
    Snap values={0}; record(cpu,op,0,&values);
}
static ExecState g_shadow;
PrefetchStats g_prefetch;
static uint64_t g_cs_creates,cum_prefetch;
int shader_mode(void) { return SHADERS_PREFETCH; }
static uint64_t SDL_GetTicksNS(void) { static uint64_t tick; return ++tick; }
static void compute_program(NvnProgram *p) { (void)p; CHECK(false); }
static PipeKey prefetched[8];
static unsigned prefetched_count;
static void scan_draw(const CmdRec *r) {
    (void)r; CHECK(prefetched_count<8); prefetched[prefetched_count++]=key_for(&g_shadow);
}
/*PREFETCH*/
'''


CHECKS = r'''
int main(int argc,char **argv) {
    CHECK(argc==2);
    Cpu *cpu=calloc(1,sizeof *cpu); CHECK(cpu);
    set_clamp_capability(VK_TRUE);
    st_defaults();
    PipeKey off=key_for(&st);
    if (!strcmp(argv[1],"capability")) {
        const VkBool32 supported[]={VK_FALSE,VK_TRUE,VK_FALSE,VK_TRUE};
        for (unsigned i=0;i<sizeof supported/sizeof supported[0];i++) {
            set_clamp_capability(supported[i]);
            check_pipeline(off,false,false); /* An unsupported device can still use ordinary clipping. */
        }
    } else if (!strcmp(argv[1],"state")) {
        CHECK(apply_state(&st,append_clamp(cpu,0))); /* Baseline drops this recorded command. */
        const uint64_t values[]={1,1,3,2,0,UINT64_MAX,0x100000000ull};
        PipeKey on={0};
        for (unsigned i=0;i<sizeof values/sizeof values[0];i++) {
            CHECK(apply_state(&st,append_clamp(cpu,values[i])));
            PipeKey key=key_for(&st);
            if (i==0) { on=key; CHECK(memcmp(&off,&on,sizeof off)); CHECK(key_hash(&off)!=key_hash(&on)); }
            CHECK(!memcmp(&key,(values[i]&1)?&on:&off,sizeof key));
            ExecState before=st;
            CHECK(apply_state(&st,append_clamp(cpu,values[i])));
            CHECK(!memcmp(&before,&st,sizeof st));
        }
        CHECK(apply_state(&st,append_clamp(cpu,1)));
        CmdRec unrelated={.op=CMD_SET_POLYGON_OFFSET}; unrelated.f[0]=2; unrelated.f[1]=3;
        CHECK(apply_state(&st,&unrelated));
        CHECK(st.poly_offset[0]==2 && st.poly_offset[1]==3);
        PipeKey after=key_for(&st); CHECK(!memcmp(&after,&on,sizeof on));
        CHECK(apply_state(&st,append_clamp(cpu,0)));
        CHECK(st.poly_offset[0]==2 && st.poly_offset[1]==3);
        after=key_for(&st); CHECK(!memcmp(&after,&off,sizeof off));
        CHECK(apply_state(&st,append_clamp(cpu,1)));
        st_defaults();
        after=key_for(&st); CHECK(!memcmp(&after,&off,sizeof off));
        check_pipeline(after,false,false);
    } else if (!strcmp(argv[1],"pipeline")) {
        check_pipeline(off,false,false);
        apply_state(&st,append_clamp(cpu,1));
        PipeKey on=key_for(&st);
        check_pipeline(on,true,true); /* Actual VkGraphicsPipelineCreateInfo, not a modeled flag. */
        uint64_t off_hash=key_hash(&off),on_hash=key_hash(&on);
        CHECK(off_hash!=on_hash);
        Pipe p0={.k=off},p1={.k=on};
        uint32_t s0=mk8_pipe_tab_slot(off_hash);
        pipe_tab[s0].key=off_hash; pipe_tab[s0].pipe=&p0; pipe_tab_count++;
        uint32_t s1=mk8_pipe_tab_slot(on_hash);
        CHECK(!pipe_tab[s1].key && s0!=s1);
        pipe_tab[s1].key=on_hash; pipe_tab[s1].pipe=&p1; pipe_tab_count++;
        CHECK(pipe_tab[mk8_pipe_tab_slot(off_hash)].pipe==&p0);
        CHECK(pipe_tab[mk8_pipe_tab_slot(on_hash)].pipe==&p1);
        apply_state(&st,append_clamp(cpu,2));
        PipeKey masked=key_for(&st); CHECK(!memcmp(&off,&masked,sizeof off));
        check_pipeline(masked,false,false);
        free(pipe_tab);
    } else if (!strcmp(argv[1],"prefetch")) {
        const uint64_t values[]={1,3,2,0};
        for (unsigned i=0;i<4;i++) { append_clamp(cpu,values[i]); append_op(cpu,CMD_DRAW_ARRAYS); }
        append_op(cpu,CMD_END);
        uint64_t handle=(uintptr_t)command_buffer.buf;
        backend_prefetch(&handle,1);
        CHECK(prefetched_count==4 && g_prefetch.submits==1);
        PipeKey executor[4]; unsigned draws=0;
        for (uint64_t offset=0;offset<command_buffer.buf_used;) {
            CmdRec *r=(CmdRec *)(command_buffer.buf+offset);
            if (r->op==CMD_END) break;
            if (r->op==CMD_DRAW_ARRAYS) executor[draws++]=key_for(&st);
            else CHECK(apply_state(&st,r));
            offset+=r->size;
        }
        CHECK(draws==4 && !memcmp(&st,&g_shadow,sizeof st));
        for (unsigned i=0;i<4;i++) {
            CHECK(!memcmp(&executor[i],&prefetched[i],sizeof(PipeKey)));
            check_pipeline(prefetched[i],(values[i]&1)!=0,i&1);
        }
        /* The next submission starts from the previous executor state, without resetting it. */
        command_buffer.buf_used=0;
        CHECK(apply_state(&st,append_clamp(cpu,1)));
        command_buffer.buf_used=0; append_op(cpu,CMD_DRAW_ARRAYS); append_op(cpu,CMD_END);
        handle=(uintptr_t)command_buffer.buf; backend_prefetch(&handle,1);
        CHECK(prefetched_count==5); check_pipeline(prefetched[4],true,false);
    } else if (!strcmp(argv[1],"unsupported")) {
        set_clamp_capability(VK_FALSE);
        check_pipeline(off,false,false);
        apply_state(&st,append_clamp(cpu,1));
        Pipe pipe={.k=key_for(&st)};
        unsigned before=creations;
        expect_fatal=true;
        if (!setjmp(fatal_return)) {
            build_pipeline(&pipe);
            fprintf(stderr,"requested depth clamp on an unsupported device was not rejected\n");
            return 1;
        }
        expect_fatal=false;
        CHECK(creations==before && strstr(fatal_message,"depth"));
    } else CHECK(false);
    free(command_buffer.buf); free(cpu);
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class DepthClampTests(unittest.TestCase):
    def test_recorded_clamp_reaches_pipeline_and_prefetch(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)
            runtime=prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',out/'host')
            text=(runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            commands=(runtime/'nvn/cmdbuf.c').read_text(encoding='utf-8')
            header=(runtime/'nvn/vulkan/vk.h').read_text(encoding='utf-8')
            device=(runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            chunks={
                'VK_GLOBAL': between(header,'typedef struct {\n    VkInstance inst;','extern VkG g_vk;'),
                'CAPABILITY': between(device,'    b->depthClamp = h->depthClamp;','    b->largePoints = h->largePoints;'),
                'RECORDERS': between(commands,'static CmdRec *raw_rec(','// ---- recording.')
                    +between(commands,'static CmdRec *rec(','// Debug: LARECOMP_TRACE_CALLS')
                    +between(commands,'typedef struct { uint64_t x[8]; float f[4]; } Snap;','#define PLAIN')
                    +between(commands,'NVN(nvnCommandBufferSetDepthClamp)','NVN(nvnCommandBufferSetRasterizerDiscard)'),
                'STATE': between(text,'typedef struct {\n    NvnTexture *color[8];','uint32_t nvn_window_origin(void);'),
                'DEFAULTS': between(text,'static void st_defaults(void)','void vk_exec_init(void)'),
                'LAYOUT': between(text,'typedef struct { VkFormat color[8], depth, stencil;', '\n'),
                'PIPE_TYPES': between(text,'#define ZERO_BINDING 16','// 8 bytes a step'),
                'HASH': between(text,'static uint64_t hash_words(','static uint64_t hmix('),
                'BLEND': between(text,'static VkBlendFactor blend_factor(','static VkCompareOp compare_fn('),
                'RT_KIND': between(text,'static int rt_kind(','// ================================================================== graphics pipelines'),
                'DYNAMIC': between(text,'static const VkDynamicState DYN[]','// Fixed-function state'),
                'BUILD': between(text,'static VkPrimitiveTopology topo_class_base(','// ================================================================== feedback snapshots'),
                'KEY': between(text,'static void pipe_key(','\n}\n')+'\n}\n',
                'APPLY': between(text,'static bool apply_state(','static void exec_list('),
                'PREFETCH': between(text,'static void scan_list(','// LARECOMP_PREFETCH_VERIFY=1:'),
            }
            source=PREFIX
            for name,chunk in chunks.items():
                source=source.replace('/*'+name+'*/',chunk)
            path,exe=out/'depth_clamp.c',out/'depth_clamp.exe'
            path.write_text(source+CHECKS,encoding='utf-8')
            built=subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                '-I'+str(runtime),'-I'+str(DEPS/'include'),str(path),'-o',str(exe)],
                env=compiler_env(),capture_output=True,text=True,timeout=120)
            self.assertEqual(built.returncode,0,built.stderr)
            for case in ('capability','state','pipeline','prefetch','unsupported'):
                with self.subTest(case=case):
                    run=subprocess.run([str(exe),case],capture_output=True,text=True,timeout=15)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)


if __name__=='__main__':
    unittest.main()
