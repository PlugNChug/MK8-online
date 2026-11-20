"""Exercise real command recording, copying, and traversal with the smaller layout."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import compact_commands


def between(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class CompactCommandTests(unittest.TestCase):
    def test_record_arguments_payload_and_traversal(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            commands = (runtime/'nvn/cmdbuf.c').read_text(encoding='utf-8')
            header_before = (runtime/'nvn/nvn.h').read_bytes()
            with self.assertRaisesRegex(ValueError, 'boundary changed'):
                compact_commands.prepare(runtime)
            self.assertEqual((runtime/'nvn/nvn.h').read_bytes(), header_before)
            raw = between(commands, 'static CmdRec *raw_rec(', '// ---- recording.')
            rec = between(commands, 'static CmdRec *rec(', '// Debug: LARECOMP_TRACE_CALLS')
            generic = between(commands, 'typedef struct { uint64_t x[8]; float f[4]; } Snap;', '#define PLAIN')
            source = r'''
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#include "nvn/nvn.h"
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal_trace(void) { abort(); }
static NvnCmdBuf cb;
static NvnCmdBuf *cb_of(uint64_t guest) { (void)guest; return &cb; }
/*RECORDERS*/
int main(void) {
    _Static_assert(sizeof(CmdRec)==72 && offsetof(CmdRec,extra)==72, "compact size");
    _Static_assert(CMD_COUNT==62, "Review arguments for new commands");
    Cpu *cpu=calloc(1,sizeof *cpu); assert(cpu);
    uint64_t expected[7];
    for (int i=0;i<8;i++) cpu->x[i]=0x1234567800000000ull+i;
    for (int i=0;i<7;i++) expected[i]=cpu->x[i+1];
    float floats[4]={0.125f,-2.5f,3.25f,4.5f};
    for (int i=0;i<4;i++) memcpy(&cpu->v[i],&floats[i],sizeof(float));
    for (unsigned op=0;op<CMD_COUNT;op++) {
        Snap s=snap(cpu);
        CmdRec *r=record(cpu,op,13,&s);
        int has_float=op==CMD_SET_ALPHA_REF || op==CMD_SET_DEPTH_BOUNDS || op==CMD_SET_POLYGON_OFFSET ||
                      op==CMD_SET_LINE_WIDTH || op==CMD_SET_POINT_SIZE || op==CMD_CLEAR_DEPTH_STENCIL;
        assert(r->size==88 && !r->mk8_textures);
        for (int i=0;i<(has_float?5:7);i++) assert(r->x[i]==expected[i]);
        if (has_float) assert(!memcmp(r->f,floats,sizeof floats));
        memset(r->extra,(int)op,13);
    }
    unsigned char *copy=malloc(cb.buf_used); assert(copy); memcpy(copy,cb.buf,cb.buf_used);
    uint64_t at=0;
    for (unsigned op=0;op<CMD_COUNT;op++) {
        CmdRec *r=(CmdRec*)(copy+at);
        assert(r->op==op && r->size==88);
        for (unsigned j=0;j<13;j++) assert(r->extra[j]==op);
        at+=r->size;
    }
    assert(at==cb.buf_used);
    unsigned char payload[16]; memset(payload,0xa5,sizeof payload);
    cpu->x[3]=(uintptr_t)payload;
    CmdRec *r=record_copy(cpu,CMD_BIND_VERTEX_BUFFERS,3,sizeof payload);
    assert(!memcmp(r->extra,payload,sizeof payload));
    /* Indexed draws and copy flags still need all seven integer arguments. */
    Snap s=snap(cpu); r=record(cpu,CMD_DRAW_ELEMENTS,0,&s);
    assert(r->x[4]==expected[4] && r->x[5]==expected[5] && r->x[6]==expected[6]);
    free(copy); free(cb.buf); free(cpu);
    return 0;
}
'''.replace('/*RECORDERS*/', raw+rec+generic)
            c, exe = out/'compact.c', out/'compact.exe'
            c.write_text(source, encoding='utf-8')
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1',
                                     '-I'+str(runtime), '-I'+str(DEPS/'include'), str(c), '-o', str(exe)],
                                    env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
