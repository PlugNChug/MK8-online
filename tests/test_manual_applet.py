"""Exercise the adapted OfflineWeb accessor with synthetic IPC storage."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import manual_applet

@unittest.skipUnless(DEFAULT_COMPILER.is_file(),'Native compiler required')
class ManualTests(unittest.TestCase):
    def test_accessor_shows_guide_and_returns_one_valid_closed_reply(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)
            runtime=prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',out/'host')
            text=(runtime/'services/am.c').read_text(encoding='utf-8')
            at=text.index('typedef struct { uint32_t id, mii_mode;')
            applet=text[at:text.index('AmApplet;',at)+len('AmApplet;')]
            at=text.index('// MK8\'s OfflineWeb applet')
            accessor=text[at:text.index('static void library_applet_creator(',at)]
            source=out/'manual.c'
            source.write_text(r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define LOG(...) ((void)0)
#define AM_NO_DATA 0x580
#define APPLET_ERROR 0x0e
typedef struct Service Service;
typedef struct { uint32_t cmd; } IpcReq;
typedef struct { uint32_t result; uint64_t copy_handles[4]; unsigned ncopy; uint8_t bytes[16]; Service *object; } IpcResp;
struct Service { void (*handler)(Service*,IpcReq*,IpcResp*); void *state; };
typedef struct { uint8_t *data; uint64_t size; } AmStorage;
#define OUT(T,off) (*(T*)(resp->bytes+(off)))
static void resp_size(IpcResp*r,unsigned n) { assert(n<=sizeof r->bytes); }
static int new_signaled_event(void) { return 1; }
static int new_event(void) { return 2; }
static Service *input;
static Service *ipc_in_object(IpcReq*r,unsigned n) { return input; }
static void ipc_return_object(IpcResp*r,Service*s) { r->object=s; }
static void am_storage(Service*s,IpcReq*q,IpcResp*r) {}
static unsigned reply_size;
static uint8_t reply[0x1010];
static Service *new_am_storage(const void *p,size_t n) { assert(n<=sizeof reply);memcpy(reply,p,n);reply_size=n;static Service s;return &s; }
static void error_notice(const uint8_t*p,size_t n) {}
static int opened,closed=1;
int mk8_manual_run(void) { opened++;return closed; }
static int mk8_no_mii_renderer(void) { return 0; }
static size_t mk8_mii_editor(uint32_t m,uint8_t o[0x80]) { return 0; }
''' + applet + '\n' + accessor + r'''
static IpcResp request(AmApplet *ap,unsigned cmd) {
    Service s={.state=ap};IpcReq q={cmd};IpcResp r={0};applet_accessor(&s,&q,&r);return r;
}
int main(void) {
    AmApplet ap={.id=0x17};
    assert(request(&ap,101).result==AM_NO_DATA);
    assert(request(&ap,10).result==AM_NO_DATA && !opened && !ap.started);
    uint8_t data[0x2000]={1,0,0,0,3,0,0,0,6,0,8,0,0,0,0,0};
    memcpy(data+16,"manual",7);
    assert(mk8_manual_arguments(data,sizeof data));
    AmStorage storage={data,sizeof data};Service st={am_storage,&storage};input=&st;
    request(&ap,100);assert(ap.manual_args);
    assert(!request(&ap,10).result && ap.started && opened==1);
    assert(!request(&ap,10).result && opened==1); // no repeated guide
    assert(request(&ap,101).object && reply_size==0x1010 && reply[0]==1);
    uint64_t size;memcpy(&size,reply+0x1008,8);assert(size==1 && !reply[8]);
    assert(request(&ap,101).result==AM_NO_DATA);
    assert(request(&ap,104).result==AM_NO_DATA);
    assert(request(&ap,30).result==0);
    data[4]=5;assert(!mk8_manual_arguments(data,sizeof data));data[4]=3;
    assert(!mk8_manual_arguments(data,32));
    data[10]=0xff;data[11]=0xff;assert(!mk8_manual_arguments(data,sizeof data));data[10]=8;data[11]=0;
    memset(data+16,'x',8);assert(!mk8_manual_arguments(data,sizeof data));
    ap=(AmApplet){.id=0x17,.manual_args=true};closed=0;
    assert(request(&ap,10).result==AM_NO_DATA && !ap.started);
    assert(request(&ap,101).result==AM_NO_DATA); // interrupted display is not success
    ap=(AmApplet){.id=0x16};assert(!request(&ap,10).result && ap.started);
    assert(request(&ap,101).result==AM_NO_DATA); // other web applets unchanged
}
''',encoding='utf-8')
            exe=out/'manual.exe'
            subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O2','-UNDEBUG',str(source),'-o',str(exe)],env=compiler_env(),check=True,capture_output=True)
            subprocess.run([str(exe)],check=True,timeout=30)

    def test_host_boundaries_fail_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'services').mkdir()
            (root/'services/am.c').write_text('unrecognized source')
            with self.assertRaises(ValueError):manual_applet.prepare(root)

if __name__=='__main__':unittest.main()
