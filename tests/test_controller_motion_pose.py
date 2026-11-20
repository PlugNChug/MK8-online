"""Native SDL-to-Npad units, orientation and sampling continuity."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolchain"))
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), "Native compiler required")
class ControllerPoseTests(unittest.TestCase):
    def test_real_units_held_tilt_and_stream_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            program = Path(folder) / "pose.c"
            program.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "motion_pose.h"
static uint64_t generation = 1, stamp = 100;
static float a[3] = {0, 9.80665f, 0}, w[3];
static bool connected = true;
bool mk8_motion_read(unsigned p, uint64_t *g, uint64_t *t, float accel[3], float gyro[3]) {
    if (!connected || p != 0) return false;
    *g = generation; *t = stamp;
    memcpy(accel, a, sizeof a); memcpy(gyro, w, sizeof w); return true;
}
static void near(float actual, float expected) { assert(fabsf(actual - expected) < 0.002f); }
int main(void) {
    Mk8MotionState s;
    assert(mk8_motion_state(1, &s) == -1);
    assert(mk8_motion_state(0, &s) == 1);
    near(s.acceleration[2], -1); near(s.direction[0][0], 1); near(s.direction[2][2], 1);
    assert(s.attributes == 1 && s.delta_time_ns == 0);
    assert(mk8_motion_state(0, &s) == 0);
    // A quarter turn about guest Z in half a second, with no gravity correction in free fall.
    a[1] = 0; w[1] = 3.14159265f;
    for (int i = 0; i < 100; ++i) {
        stamp += 5000000;
        assert(mk8_motion_state(0, &s) == 1 && s.delta_time_ns == 5000000);
    }
    near(s.angular_velocity[2], 0.5f); near(s.angle[2], 0.25f);
    near(s.direction[0][0], 0); near(s.direction[0][1], 1); near(s.direction[1][0], -1);
    // Reconnect already tilted left/right. Its orientation must agree with gravity at rest.
    for (int sign = -1; sign <= 1; sign += 2) {
        ++generation; stamp = 10; w[1] = 0;
        a[0] = sign * 4.903325f; a[1] = 8.492808f;
        assert(mk8_motion_state(0, &s) == 1);
        near(s.acceleration[0], -sign * 0.5f); near(s.direction[0][2], sign * 0.5f);
        near(s.direction[2][2], 0.8660254f); near(s.angle[2], 0);
    }
    // A held bank still resolves when its rotation fell between polls.
    a[0] *= -1;
    for (int i = 0; i < 1500; ++i) { stamp += 5000000; assert(mk8_motion_state(0, &s) == 1); }
    near(s.direction[0][2], -0.5f);
    w[0] = 1; w[2] = 2;
    stamp += 1000000000; assert(mk8_motion_state(0, &s) == 1);
    assert(s.delta_time_ns == 0); near(s.angle[0], 0);
    near(s.angular_velocity[0], 0.159155f); near(s.angular_velocity[1], -0.318310f);
    a[0] = NAN; stamp += 5000000; assert(mk8_motion_state(0, &s) == -1);
    a[0] = 0; a[1] = -9.80665f; connected = false; assert(mk8_motion_state(0, &s) == -1);
    connected = true; assert(mk8_motion_state(0, &s) == 1); near(s.direction[2][2], -1);
    assert(s.delta_time_ns == 0); assert(mk8_motion_state(4, &s) == -1);
}
''', encoding="utf-8")
            exe = Path(folder) / "pose.exe"
            subprocess.run([str(DEFAULT_COMPILER), "cc", "-std=c11", "-O2", "-UNDEBUG",
                            "-I", str(ROOT / "runtime/mk8/support"), str(program), "-o", str(exe)],
                           env=compiler_env(), check=True)
            subprocess.run([str(exe)], check=True, timeout=15)
