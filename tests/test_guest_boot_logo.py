"""The first automatic guest boot hides the logo without changing later title layouts."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from toolchain.native import DEFAULT_COMPILER, compiler_env

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'toolchain'))
import auto_lan
FIXTURE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) ((void)0)
#include "net/auto_lan.inc"

typedef struct { uint64_t x[31], sp; } Cpu;
#define LD64(address) (*(uint64_t *)(uintptr_t)(address))
#define ST64(address, value) (LD64(address) = (value))
static uint64_t page[64], stack[4];
static bool visible, has_logo;
static unsigned lookups;

static void f_70008ad8e8(Cpu *c) {
    assert(c->x[0] == 49);                      /* Boot layout handle. */
    assert(c->x[1] == (uintptr_t)&stack[1]);    /* Stock string object. */
    assert(LD64(c->x[1]) == 17 && LD64(c->x[1] + 8) == 0x7000ee914bull);
    ++lookups;
    c->x[0] = has_logo ? (uintptr_t)&visible : 0;
}

static void f_70008ade74(Cpu *c) {
    assert(c->x[1] == 0);
    assert(c->x[0] == (has_logo ? (uintptr_t)&visible : 0));
    if (c->x[0]) visible = false;
}

static void initialize_layout(bool present) {
    Cpu cpu = {0}, *c = &cpu;
    has_logo = present;
    visible = true;
    page[0x1d0 / 8] = 49;
    stack[1] = 17;
    stack[2] = 23;  /* The preceding stock lookup used a different name. */
    c->x[19] = (uintptr_t)page;
    c->sp = (uintptr_t)stack;
#include "net/guest_boot_logo.inc"
    assert(stack[1] == 17);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    bool guest = atoi(argv[1]) != 0, present = atoi(argv[2]) != 0;
    initialize_layout(present);
    assert(lookups == (unsigned)guest);
    assert(visible == !(present && guest));

    initialize_layout(true);  /* Return to the title in the same process. */
    assert(lookups == (unsigned)guest && visible && stack[2] == 23);
    return 0;
}
'''


class GuestBoundariesTests(unittest.TestCase):
    def test_version_and_both_boot_boundaries_fail_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            main=root/'main'
            main.mkdir()
            for name in ('main_099.c', 'main_189.c'):
                unit=main/name
                unit.write_text('void unrelated(void) {}\n')
                with self.assertRaisesRegex(ValueError,'verified MK8'):
                    auto_lan.patch_guest(unit,unit,root)
                with patch.object(auto_lan,'verified_bytes'), self.assertRaisesRegex(ValueError,'boundary|activation'):
                    auto_lan.patch_guest(unit,unit,root)
                self.assertFalse((root/'experimental'/name).exists())


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class GuestBootLogoTests(unittest.TestCase):
    def test_boot_layout_visibility_and_return_to_title(self):
        with tempfile.TemporaryDirectory(prefix='mk8-boot-logo-') as directory:
            folder = Path(directory)
            source = folder / 'boot.c'
            binary = folder / 'boot.exe'
            source.write_text(FIXTURE, encoding='utf-8')
            build = subprocess.run([
                str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                '-I' + str(ROOT / 'runtime/mk8'), str(source), '-o', str(binary),
            ], env=compiler_env(), capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr)
            for mode, relay, slot, guest, pane in (
                ('guest', True, '1', True, True), ('guest', True, '1', True, False),
                ('host', True, '0', False, True), ('manual', True, '1', False, True),
                (None, False, '0', False, True),
                ('guest', False, '1', False, True), ('1', True, '0', False, True),
                ('1', True, '1', True, True),
            ):
                with self.subTest(mode=mode, relay=relay, slot=slot, pane=pane):
                    env = dict(os.environ, LARECOMP_LAN_SLOT=slot, LARECOMP_LAN_ROOM='MK8ONLINE')
                    env.pop('MK8_AUTO_LAN', None)
                    env['MK8_LAN_PLAYERS'] = '1'
                    if mode is not None:
                        env['MK8_AUTO_LAN'] = mode
                    env.pop('LARECOMP_LAN_RELAY', None)
                    if relay:
                        env['LARECOMP_LAN_RELAY'] = '127.0.0.1:47820'
                    run = subprocess.run([str(binary), str(int(guest)), str(int(pane))],
                                         env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(run.returncode, 0, run.stderr)
            valid = dict(os.environ, MK8_AUTO_LAN='guest', LARECOMP_LAN_RELAY='127.0.0.1:47820',
                         LARECOMP_LAN_SLOT='1', LARECOMP_LAN_ROOM='MK8ONLINE', MK8_LAN_PLAYERS='2')
            for key, value in [('LARECOMP_LAN_SLOT', None), ('LARECOMP_LAN_SLOT','0'),
                               ('LARECOMP_LAN_ROOM', ''), ('LARECOMP_LAN_RELAY', ''),
                               ('MK8_LAN_PLAYERS', '0'), ('MK8_LAN_PLAYERS', '3')]:
                with self.subTest(key=key,value=value):
                    env = valid.copy()
                    if value is None: del env[key]
                    else: env[key] = value
                    subprocess.run([binary, '0', '1'],env=env,check=True,timeout=10)
