"""Exercise MK8's HID start/stop requests and actual six-axis state reader."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
import controller_motion
from native import DEFAULT_COMPILER, compiler_env


HARNESS = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define EXTRA_PADS 3
#define MAKE_RESULT(module,desc) ((module) | ((desc) << 9))
#define RT_ON_RESET(name) static void name(void)
typedef struct { uint64_t x[32]; } Cpu;
#define HLE_FUNC(symbol,name) static void name(Cpu *c)
#include "motion_guest.inc"

static bool connected[4], stale[4];
static uint64_t stamp[4], generation[4];
bool mk8_motion_read(unsigned slot, uint64_t *stream, uint64_t *ns, float a[3], float g[3]) {
    assert(slot < 4);
    *stream = generation[slot];
    *ns = stamp[slot];
    a[0] = 0; a[1] = 9.80665f; a[2] = 0;
    g[0] = slot + 1; g[1] = 0; g[2] = 0;
    return connected[slot] && !stale[slot];
}

typedef struct { uint32_t cmd; uint8_t *data; size_t data_size; } Request;
typedef struct { uint32_t result; } Response;
static uint32_t request(uint32_t cmd, uint32_t handle, size_t size) {
    uint8_t buffer[17] = {0};
    memcpy(buffer + 1, &handle, 4);
    Request message = { cmd, buffer + 1, size }, *req = &message;
    Response reply = {0}, *resp = &reply;
    switch (req->cmd) { /*DISPATCH*/ }
    return reply.result;
}
static unsigned read_states(uint32_t handle, int count, void *out) {
    uint8_t bytes[5];
    memcpy(bytes + 1, &handle, 4);  // exercise unaligned guest input
    Cpu c = {.x = {(uintptr_t)out, (uint32_t)count, (uintptr_t)(bytes + 1)}};
    mk8_get_motion_states(&c);
    return (unsigned)c.x[0];
}
int main(void) {
    const uint32_t p1 = 0x20003, p2 = 0x20103, p5 = 0x20403;
    Mk8MotionState out[17] = {0};
    out[16].sampling_number = 0xfeed;
    connected[0] = true;
    assert(read_states(p1, 16, out) == 0);
    assert(request(66, p1, 3) != 0);
    assert(request(66, p5, 16) != 0);
    assert(request(66, 0x20004, 16) != 0);
    assert(request(66, p1, 16) == 0);
    assert(read_states(p1, -1, out) == 0 && read_states(p1, 1, NULL) == 0);
    assert(read_states(p1, 16, out) == 1);
    assert(out[0].sampling_number == 1 && out[0].attributes == 1);
    assert(out[0].angular_velocity[0] > 0 && out[0].direction[0][0] == 1);
    assert(request(66, p1, 16) == 0);
    assert(read_states(p1, 1, out) == 1 && out[0].sampling_number == 1);
    for (unsigned i = 2; i <= 21; ++i) {
        stamp[0] += 10000000;
        assert(read_states(p1, 100, out) == (i > 16 ? 16 : i));
        assert(out[0].sampling_number == i && out[0].delta_time_ns == 10000000);
    }
    for (unsigned i = 0; i < 16; ++i) assert(out[i].sampling_number == 21 - i);
    assert(out[16].sampling_number == 0xfeed);
    assert(read_states(p1, 2, out) == 2);
    assert(request(66, p2, 16) == 0 && read_states(p2, 10, out) == 0);
    connected[1] = true;
    assert(read_states(p2, 10, out) == 1 && out[0].sampling_number == 1);
    stale[1] = true;
    assert(read_states(p2, 10, out) == 0 && !out[0].sampling_number);
    stale[1] = false;
    assert(read_states(p2, 10, out) == 1 && out[0].sampling_number == 2);
    assert(read_states(p1, 16, out) == 16 && out[0].sampling_number == 21);
    connected[0] = false;
    assert(read_states(p1, 16, out) == 0 && !out[0].attributes && out[0].angle[0] == 0);
    assert(out[0].sampling_number == 0 && out[0].angular_velocity[0] == 0);
    assert(read_states(p1, 16, out) == 0 && out[0].sampling_number == 0);
    connected[0] = true;
    assert(read_states(p1, 16, out) == 1 && out[0].attributes && !out[0].delta_time_ns);
    ++generation[0];  // replacement without an intervening disconnected read
    assert(read_states(p1, 16, out) == 1 && out[0].sampling_number == 23);
    assert(read_states(p5, 16, out) == 0 && !out[0].sampling_number);
    assert(read_states(p1, 16, out) == 1 && out[0].sampling_number == 23);
    assert(request(67, p1, 16) == 0 && read_states(p1, 16, out) == 0 && !out[0].sampling_number);
    assert(read_states(p2, 16, out) == 1 && out[0].attributes);
    mk8_motion_guest_reset();
    assert(read_states(p2, 16, out) == 0);
    assert(request(66, p2, 16) == 0);
    assert(read_states(p2, 16, out) == 1 && out[0].sampling_number == 1);
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class MotionGuestTests(unittest.TestCase):
    def test_started_fullkey_sensor_delivers_bounded_real_history(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            pinned = ROOT/'upstream/smm2recomp/recomp/runtime'
            for relative in ('hle/input.c', 'services/hid_set.c'):
                dest = runtime/relative
                dest.parent.mkdir()
                shutil.copyfile(pinned/relative, dest)
            controller_motion.prepare(runtime)
            hle = (runtime/'hle/input.c').read_text(encoding='utf-8')
            offset = hle.index('#include "mk8_motion_pose.h"')
            (runtime/'hle/motion_guest.inc').write_text(hle[offset:], encoding='utf-8')
            hid = (runtime/'services/hid_set.c').read_text(encoding='utf-8')
            dispatch = hid[hid.index('    case 66:'):hid.index('    case 101:')]
            harness = runtime/'hle/test.c'
            harness.write_text(HARNESS.replace('/*DISPATCH*/', dispatch), encoding='utf-8')
            exe = runtime/'test.exe'
            cmd = [str(DEFAULT_COMPILER), 'cc', '-O1', '-target', 'x86_64-windows-gnu', str(harness), '-o', str(exe)]
            build = subprocess.run(cmd, env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            test = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(test.returncode, 0, test.stderr)
