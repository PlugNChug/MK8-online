"""The native Yaz0 loop writes exactly what the game's own loop writes and leaves the same registers.

The game's loop is taken at test time from the local generated main_230.c (never copied into the repository) and
compiled next to runtime/mk8/support/yaz0.c; both decode the same streams, and an independent Python decoder gives the
expected output."""
import hashlib
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import yaz0_native


def generated_unit():
    builds = Path(os.environ.get('MK8_TEST_DATA', ROOT/'local'))/'builds'
    for unit in sorted(builds.glob('*/generated/main/' + yaz0_native.UNIT)):
        if hashlib.sha256(unit.read_bytes()).hexdigest() == yaz0_native.SHA256:
            return unit
    return None


def encode(data, rng):
    """Yaz0 with literals, short and long copies, overlapping ones (distance below length) and far ones."""
    out, ops, i = bytearray(), [], 0
    while i < len(data):
        best_len, best_dist = 0, 0
        for dist in range(1, min(i, 4096) + 1):
            n = 0
            while n < 273 and i + n < len(data) and data[i + n] == data[i + n - dist]:
                n += 1
            if n > best_len:
                best_len, best_dist = n, dist
        if best_len >= 3 and rng.random() < 0.95:
            ops.append(('copy', best_dist, best_len))
            i += best_len
        else:
            ops.append(('lit', data[i]))
            i += 1
    for g in range(0, len(ops), 8):
        group, flags, body = ops[g:g + 8], 0, bytearray()
        for k, op in enumerate(group):
            if op[0] == 'lit':
                flags |= 0x80 >> k
                body.append(op[1])
            else:
                d, n = op[1] - 1, op[2]
                if n <= 17:
                    body += bytes([((n - 2) << 4) | (d >> 8), d & 0xff])
                else:
                    body += bytes([d >> 8, d & 0xff, n - 0x12])
        out.append(flags)
        out += body
    return bytes(out)


def decode(stream, size, slack):
    """Independent reference: what the game's loop writes, overrun of the last copy included."""
    dst, pos, out, mask, code = bytearray(b'\xcd' * (size + slack)), 0, 0, 0, 0
    while True:
        if not mask:
            code, pos, mask = stream[pos], pos + 1, 0x80
        if code & mask:
            dst[out], pos, out = stream[pos], pos + 1, out + 1
        else:
            b0, b1 = stream[pos], stream[pos + 1]
            n = b0 >> 4
            if n:
                n, pos = n + 2, pos + 2
            else:
                n, pos = stream[pos + 2] + 0x12, pos + 3
            start = out - (b1 | (b0 & 0xf) << 8) - 1
            for k in range(n):
                dst[out + k] = dst[start + k]
            out += n
        mask >>= 1
        if out >= size:
            return bytes(dst)


HARNESS = r'''
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int mk8_yaz0_loop(Cpu *c);
void rt_log(const char *fmt, ...) { (void)fmt; }
uint64_t plat_now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static void guest_loop(Cpu *c) {
%s
L_7000a88b90:;
}
#define SLACK 512
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    static uint8_t src[1 << 22], a[1 << 22], b[1 << 22];
    size_t n = fread(src, 1, sizeof src, f);
    fclose(f);
    uint32_t size = (uint32_t)src[4] << 24 | src[5] << 16 | src[6] << 8 | src[7];
    memset(a, 0xcd, size + SLACK);
    memset(b, 0xcd, size + SLACK);
    Cpu g = {0}, h = {0};
    g.x[19] = h.x[19] = (uint64_t)(uintptr_t)src;
    g.x[20] = (uint64_t)(uintptr_t)a, h.x[20] = (uint64_t)(uintptr_t)b;
    g.x[12] = h.x[12] = 0x5a;  // (whatever the register held: the first group loads it)
    guest_loop(&g);             // (from L_7000a88ab8: the size from the header, then the loop)
    h.x[8] = size;              // (what the guest computed before the hook)
    if (!mk8_yaz0_loop(&h)) return 2;
    if (memcmp(a, b, size + SLACK)) return 3;
    for (int r = 8; r <= 13; r++) if (g.x[r] != h.x[r] && r != 9) { printf("x%%d %%llx %%llx\n", r, g.x[r], h.x[r]); return 4; }
    if (g.x[9] - (uint64_t)(uintptr_t)a != h.x[9] - (uint64_t)(uintptr_t)b) return 5;  // (dst - 1 of each)
    if (g.nzcv != h.nzcv) return 6;
    FILE *o = fopen(argv[2], "wb");
    fwrite(b, 1, size + SLACK, o);
    fclose(o);
    (void)n;
    return 0;
}
'''


class Yaz0NativeTests(unittest.TestCase):
    def test_patch_inserts_one_hook_before_the_loop_and_checks_boundaries(self):
        text = ('void f_7000a88a44(Cpu *c) {\n' + yaz0_native.LOOP + '    loop body\n}\n')
        patched = yaz0_native.patch(text)
        self.assertEqual(patched.count('mk8_yaz0_loop(c)'), 1)
        self.assertLess(patched.index('if (COND_LT(c)) { goto L_7000a88b90; }'), patched.index('mk8_yaz0_loop(c)'))
        self.assertLess(patched.index('mk8_yaz0_loop(c)'), patched.index('c->x[11] = (uint32_t)(0u | 0u);'))
        self.assertIn(yaz0_native.DECLARATION, patched)
        for broken in (text.replace('L_7000a88af0', 'L_other'), text.replace('f_7000a88a44', 'f_other'), patched):
            with self.assertRaises(ValueError):
                yaz0_native.patch(broken)

    @unittest.skipUnless(DEFAULT_COMPILER.is_file() and generated_unit(), 'Native compiler and the verified generated unit required')
    def test_native_loop_matches_the_game_loop(self):
        text = generated_unit().read_text(encoding='utf-8')
        start = text.index('L_7000a88ab8:;')
        loop = text[start:text.index('L_7000a88b90:;', start)]
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            # (the build's own step on the real unit, generated with CRLF line ends)
            patched = yaz0_native.patch_unit(generated_unit(), out).read_text(encoding='utf-8')
            self.assertEqual(patched.count('if (mk8_yaz0_loop(c)) goto L_7000a88b90;'), 1)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            harness = out/'harness.c'
            harness.write_text(HARNESS % loop, encoding='utf-8')
            exe = out/'harness.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-I'+str(runtime),
                            str(harness), str(ROOT/'runtime/mk8/support/yaz0.c'), '-o', str(exe)],
                           env=compiler_env(), check=True, capture_output=True)
            rng = random.Random(14112026)
            cases = []
            for length in (1, 2, 3, 17, 18, 273, 274, 5000, 20000):
                words = [bytes(rng.choice(b'MK8yaz0 ') for _ in range(rng.randint(1, 12))) for _ in range(64)]
                data = b''.join(rng.choice(words) for _ in range(length // 4 + 1))[:length]
                cases.append((data, len(data)))
            runs = bytes([7]) * 600 + bytes(range(256)) * 20 + b'ab' * 300  # (overlapping copies of 1 and 2 bytes)
            cases.append((runs, len(runs)))
            # a header size short of what the stream holds: the last copy writes past it, as the game's loop does
            cases.append((runs, len(runs) - 5))
            for index, (data, size) in enumerate(cases):
                stream = encode(data, rng)
                path = out/f'case{index}.yaz0'
                path.write_bytes(b'Yaz0' + struct.pack('>I', size) + bytes(8) + stream)
                result = subprocess.run([str(exe), str(path), str(path.with_suffix('.out'))], capture_output=True, timeout=60)
                self.assertEqual(result.returncode, 0, (index, result.stdout[-200:]))
                written = path.with_suffix('.out').read_bytes()
                self.assertEqual(written, decode(stream, size, 512), index)
                if size == len(data):
                    self.assertEqual(written[:size], data, index)


if __name__ == '__main__':
    unittest.main()
