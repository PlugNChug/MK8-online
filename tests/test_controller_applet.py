"""The controller support applet's end tells the game it is in focus again (MK8's frozen two-player screen).

The prepared am.c message queue (am_init_once, mk8_am_push_message) runs in a native harness with the kernel event
calls as stand-ins; the hook sits in the prepared ShowControllerSupport after its answer and can be turned off."""
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
#define RT_ON_RESET(name) static void name(void)
typedef struct KEvent { bool signaled; } KEvent;
pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static KEvent event;
static int created, signals;
static KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { *w = 1, *r = 2; created++; return &event; }
static void kernel_signal(KEvent *e) { e->signaled = true; signals++; }
/*QUEUE*/
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
            support = hid[hid.index('hid_show_controller_support) {'):]
            support = support[:support.index('\n}\n')]
            # after the answer is written and logged, before returning; MK8_NO_APPLET_FOCUS turns it off
            self.assertLess(support.index('player(s)'), support.index('mk8_am_push_message(15);'))
            self.assertLess(support.index('mk8_am_push_message(15);'), support.index('c->x[0] = n < min'))
            self.assertIn('if (!getenv("MK8_NO_APPLET_FOCUS")) {', support)
            am = (runtime/'services/am.c').read_text(encoding='utf-8')
            queue = am[am.index('static uint32_t msg_event_r'):am.index('static uint32_t new_event(void) {')]
            harness = out/'harness.c'
            harness.write_text(HARNESS.replace('/*QUEUE*/', queue), encoding='utf-8')
            exe = out/'harness.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-Wno-unused-variable',
                            '-Wno-unused-function', str(harness), '-o', str(exe)], env=compiler_env(), check=True,
                           capture_output=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout)

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_missing_players_cancel_without_publishing_a_successful_count(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            hid = (runtime/'hle/input.c').read_text(encoding='utf-8')
            start = hid.index('         hid_show_controller_support) {')
            body = hid[start:hid.index('\n}\n', start)].split('{', 1)[1]
            harness = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) ((void)0)
typedef struct { uint64_t x[32]; } Cpu;
static int players, focus, reported_min, reported_max, reported_players;
int input_players(void) { return players; }
void mk8_controller_applet_result(int min, int max, int current) {
    reported_min=min; reported_max=max; reported_players=current;
}
void mk8_am_push_message(uint32_t message) { if (message == 15) focus++; }
void support(Cpu *c) { /*BODY*/ }
int main(void) {
    const int cases[][4] = {{1,2,2,0}, {2,2,2,2}, {4,2,2,2}, {2,3,4,0}, {1,1,1,1}};
    for (int i = 0; i < 5; i++) {
        uint8_t output[8]; memset(output, 0xa5, sizeof output);
        int8_t arg[2] = {cases[i][1], cases[i][2]};
        Cpu c = {.x = {(uintptr_t)output, (uintptr_t)arg}};
        players = cases[i][0]; support(&c);
        if (focus != i + 1) return 1;
        if (reported_min!=arg[0] || reported_max!=arg[1] || reported_players!=(players>arg[1] ? arg[1] : players)) return 5;
        if (!cases[i][3]) {
            if (c.x[0] != ((3101u << 9) | 202u)) return 2;
            for (int k = 0; k < 8; k++) if (output[k] != 0xa5) return 3;
        } else if (c.x[0] || output[0] != cases[i][3] || *(uint32_t *)(output + 4)) return 4;
    }
    return 0;
}
'''
            source = out/'support.c'
            source.write_text(harness.replace('/*BODY*/', body + '\n'), encoding='utf-8')
            exe = out/'support.exe'
            built = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1',
                            str(source), '-o', str(exe)], env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
