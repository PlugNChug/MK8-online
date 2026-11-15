"""The controller support applet's end tells the game it is in focus again (MK8's frozen two-player screen).

The prepared ShowControllerSupport and am.c queue run in a native harness with kernel event calls as stand-ins;
the applet returns its player result and queues focus unless the diagnostic override disables it."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import controller_applet

HARNESS = r'''
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define RT_ON_RESET(name) static void name(void)
#define HLE_FUNC(symbol, name) static void name(Cpu *c)
#define LOG(...) ((void)0)
typedef struct Cpu { uint64_t x[31]; } Cpu;
static int input_players(void) { return 3; }
typedef struct KEvent { bool signaled; } KEvent;
pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static KEvent event;
static int created, signals;
static KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { *w = 1, *r = 2; created++; return &event; }
static void kernel_signal(KEvent *e) { e->signaled = true; signals++; }
/*QUEUE*/
/*SUPPORT*/
int main(void) {
    mk8_am_push_message(15);  // (the applet ends before anything else asked: the boot message comes first)
    if (created != 1 || msg_count != 2 || msg_queue[0] != 15 || msg_queue[1] != 15 || !event.signaled || signals != 2) return 1;
    mk8_am_push_message(15);
    if (created != 1 || msg_count != 3 || signals != 3) return 2;
    for (int i = 0; i < 40; i++) mk8_am_push_message(15);  // (a full queue keeps its 16 and still signals)
    if (msg_count != 16 || signals != 43) return 3;
    am_reset();
    mk8_am_push_message(15);  // (a reboot starts over)
    if (created != 2 || msg_count != 2) return 4;
    msg_count = 0;  // The game consumed its startup messages before opening controller support.
    event.signaled = false;
    int previous_signals = signals;
    struct { int8_t players; uint8_t pad[3]; uint32_t id; } result = { -1, {0}, UINT32_MAX };
    int8_t args[] = { 2, 2 };
    Cpu cpu = { .x = { (uintptr_t)&result, (uintptr_t)args } };
    hid_show_controller_support(&cpu);
    if (cpu.x[0] || result.players != 2 || result.id != 0) return 5;
    bool focus = getenv("MK8_NO_APPLET_FOCUS") == NULL;
    if (msg_count != (int)focus || (focus && msg_queue[0] != 15)) return 6;
    if (event.signaled != focus || signals != previous_signals + (int)focus) return 7;
    return 0;
}
'''


class ControllerAppletTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            (runtime/'services').mkdir()
            (runtime/'hle').mkdir()
            (runtime/'services/am.c').write_text('int x;\n', encoding='utf-8')
            (runtime/'hle/input.c').write_text('int y;\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                controller_applet.prepare(runtime)

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_applet_end_queues_focus_state_changed(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            hid = (runtime/'hle/input.c').read_text(encoding='utf-8')
            support = hid[hid.index('HLE_FUNC("_ZN2nn3hid21ShowControllerSupport'):]
            support = support[:support.index('\n}\n') + 3]
            am = (runtime/'services/am.c').read_text(encoding='utf-8')
            queue = am[am.index('static uint32_t msg_event_r'):am.index('static uint32_t new_event(void) {')]
            harness = out/'harness.c'
            harness.write_text(HARNESS.replace('/*QUEUE*/', queue).replace('/*SUPPORT*/', support), encoding='utf-8')
            exe = out/'harness.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-Wno-unused-variable',
                            '-Wno-unused-function', str(harness), '-o', str(exe)], env=compiler_env(), check=True,
                           capture_output=True)
            env = dict(os.environ)
            env.pop('MK8_NO_APPLET_FOCUS', None)
            for disabled in (False, True):
                with self.subTest(disabled=disabled):
                    if disabled:
                        env['MK8_NO_APPLET_FOCUS'] = '1'
                    result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
