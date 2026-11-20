"""Exercise save mounts and cleanup with real filesystem handlers and synthetic files."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER, compiler_env
import save_data
import save_file_close

SOURCE = ROOT/'upstream/smm2recomp/recomp/runtime'

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <windows.h>
#include "platform/win_compat.h"
#define LOG(...) ((void)0)
#define MAKE_RESULT(m,d) ((m) | ((d)<<9))
#define FS_PATH_NOT_FOUND MAKE_RESULT(2,1)
#define FS_PATH_EXISTS MAKE_RESULT(2,2)
#define FS_NOT_EMPTY MAKE_RESULT(2,8)
typedef struct { uint64_t addr,size; } IpcBuf;
typedef struct { uint32_t cmd; const uint8_t *data; uint32_t data_size; IpcBuf x[1],a[1],b[1]; int nx,na,nb; } IpcReq;
typedef struct Service Service;
typedef struct { uint32_t result; uint64_t data[64]; unsigned data_size,nobjects; Service *objects[1]; } IpcResp;
struct Service { void (*handler)(Service *,IpcReq *,IpcResp *); void *state; void (*on_close)(Service *); };
#undef IN
#undef OUT
#define IN(t,o) (*(const t *)(req->data+(o)))
#define OUT(t,o) (*(t *)((uint8_t *)resp->data+(o)))
#define ENV(n) getenv(n)
static void resp_size(IpcResp *r,unsigned n) { r->data_size=n; }
static IpcBuf *out_buf(IpcReq *r,int i) { return &r->b[i]; }
static IpcBuf *in_buf(IpcReq *r,int i) { return &r->a[i]; }
static const char *mods_dir(void) { return NULL; }
static const char *g_save_dir;
static Service *service_new(const char *name,void (*fn)(Service *,IpcReq *,IpcResp *),void *state) {
    (void)name; Service *s=calloc(1,sizeof *s); assert(s); s->handler=fn; s->state=state; return s;
}
static void ipc_return_object(IpcResp *r,Service *s) { r->objects[r->nobjects++]=s; }
void flags_ext_save_written(const char *p,uint64_t a,int64_t o,int64_t n) { (void)p;(void)a;(void)o;(void)n; }
/*HOST_IO*/
/*HOST_FS*/
static void mount_dispatch(IpcReq *req,IpcResp *resp) { switch(req->cmd) { /*MOUNT*/ } }
static Service *mount_save(unsigned type,unsigned cmd,unsigned bytes) {
    _Alignas(8) uint8_t payload[0x48]={0};
    payload[0]=1; /* User save-data space. */
    payload[0x28]=(uint8_t)type;
    IpcReq req={.cmd=cmd,.data=payload,.data_size=bytes}; IpcResp resp={0};
    mount_dispatch(&req,&resp);
    if(bytes<sizeof payload || type>6) { assert(resp.result && !resp.nobjects); return NULL; }
    assert(!resp.result && resp.nobjects==1); return resp.objects[0];
}
static IpcResp fs_call(Service *fs,unsigned cmd,const char *path,uint64_t first,uint64_t size) {
    uint64_t data[2]={first,size}; IpcReq req={.cmd=cmd,.data=(uint8_t *)data,.data_size=sizeof data,.nx=1};
    req.x[0]=(IpcBuf){(uintptr_t)path,strlen(path)+1}; IpcResp r={0}; fs->handler(fs,&req,&r); return r;
}
static void put(Service *fs,const char *name,const char *value) {
    assert(!fs_call(fs,0,name,0,strlen(value)).result);
    IpcResp opened=fs_call(fs,8,name,3,0); assert(!opened.result && opened.nobjects==1);
    Service *f=opened.objects[0]; uint64_t data[3]={1,0,strlen(value)};
    IpcReq req={.cmd=1,.data=(uint8_t *)data,.data_size=sizeof data,.na=1};
    req.a[0]=(IpcBuf){(uintptr_t)value,strlen(value)}; IpcResp r={0};
    f->handler(f,&req,&r); assert(!r.result); f->on_close(f); free(f);
}
static void expect(Service *fs,const char *name,const char *value) {
    IpcResp opened=fs_call(fs,8,name,1,0); assert(!opened.result && opened.nobjects==1);
    Service *f=opened.objects[0]; char output[100]={0}; uint64_t data[3]={0,0,strlen(value)};
    IpcReq req={.cmd=0,.data=(uint8_t *)data,.data_size=sizeof data,.nb=1};
    req.b[0]=(IpcBuf){(uintptr_t)output,sizeof output}; IpcResp r={0}; f->handler(f,&req,&r);
    assert(!r.result && r.data[0]==strlen(value) && !strcmp(output,value)); f->on_close(f); free(f);
}
int main(int argc,char **argv) {
    assert(argc==3); g_save_dir=argv[1]; _fmode=_O_BINARY;
    Service *account=mount_save(1,51,0x48), *common=mount_save(3,51,0x48), *temporary=mount_save(4,51,0x48);
    Service *cache=mount_save(5,51,0x48), *system=mount_save(0,51,0x48);
    Service *all[]={account,common,temporary,cache,system};
    assert(strstr(((HostFs *)account->state)->root,"/save_0"));
    for(unsigned i=0;i<5;i++) for(unsigned j=0;j<i;j++)
        assert(strcmp(((HostFs *)all[i]->state)->root,((HostFs *)all[j]->state)->root));
    if(!strcmp(argv[2],"write")) {
        put(account,"/userdata.dat","earned progress");
        for(unsigned i=1;i<5;i++) {
            put(all[i],"/userdata.dat","cleanup target");
            assert(!fs_call(all[i],1,"/userdata.dat",0,0).result);
            expect(account,"/userdata.dat","earned progress");
        }
        put(temporary,"/rp_test.dat","new replay and thumbnail");
        expect(temporary,"/rp_test.dat","new replay and thumbnail");
        put(account,"/rp_test.dat","new replay and thumbnail");
        assert(!fs_call(temporary,1,"/rp_test.dat",0,0).result);
    }
    expect(account,"/userdata.dat","earned progress"); expect(account,"/rp_test.dat","new replay and thumbnail");
    Service *read_only=mount_save(1,53,0x48); expect(read_only,"/userdata.dat","earned progress");
    assert(fs_call(common,8,"/userdata.dat",1,0).result==FS_PATH_NOT_FOUND);
    mount_save(1,51,0x28); mount_save(7,51,0x48);
    free(read_only->state);free(read_only);
    for(unsigned i=0;i<5;i++) { free(all[i]->state);free(all[i]); }
    puts("Account progress and replay survived other-save cleanup and reopen");
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class SaveDataTests(unittest.TestCase):
    def test_mount_cleanup_and_second_process_keep_account_files(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = out/'runtime'
            (runtime/'services').mkdir(parents=True)
            fs = runtime/'services/fs.c'
            fs.write_bytes((SOURCE/'services/fs.c').read_bytes())
            save_file_close.prepare(runtime)
            save_data.prepare(runtime)
            prepared = fs.read_text(encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'layout changed'):
                save_data.prepare(runtime)
            self.assertEqual(fs.read_text(encoding='utf-8'), prepared)
            host = prepared[prepared.index('typedef struct { char root[1024];'):prepared.index('// ------------------------------------------------------------------ fsp-srv')]
            mount = prepared[prepared.index('    case 51: case 53:'):prepared.index('    case 200:')]
            windows = (SOURCE/'platform/windows.c').read_text(encoding='utf-8')
            io = windows[windows.index('static ssize_t prw('):windows.index('// ---- child processes')]
            source = HARNESS.replace('/*HOST_IO*/', io).replace('/*HOST_FS*/', host).replace('/*MOUNT*/', mount)
            c, exe = out/'save.c', out/'save.exe'
            c.write_text(source, encoding='utf-8')
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                                     '-D_FILE_OFFSET_BITS=64', '-I'+str(SOURCE), str(c), '-o', str(exe)],
                                    env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for phase in ('write', 'reload'):
                result = subprocess.run([str(exe), str(out/'saves'), phase], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
