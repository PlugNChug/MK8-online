"""O bloco Vulkan real conserva o afastamento de planos autorais ao mudar a escala."""
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
import depth_bias


def section(text, start, end):
    if text.count(start) != 1:
        raise ValueError('Depth-bias native fixture boundary changed')
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef uint32_t VkBool32;
typedef int VkCommandBuffer;
typedef struct { bool scalable; float sx, sy; } TexBackend;
typedef struct { TexBackend *backend; } NvnTexture;
static struct { uint8_t poly[4]; float poly_offset[3]; } st;
static struct { uint32_t known, vb_known; VkBool32 dbe; float db[3]; } ss;
enum { SS_DBE = 1 << 12, SS_DB = 1 << 13 };
static int ss_on = 1, bias_calls, enable_calls;
static float pass_sx = 1, pass_sy = 1, applied[3];
static VkBool32 enabled;
static void vkCmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 on) {
    (void)cb; enabled = on; enable_calls++;
}
static void vkCmdSetDepthBias(VkCommandBuffer cb, float constant, float clamp, float slope) {
    (void)cb; applied[0] = slope; applied[1] = constant; applied[2] = clamp; bias_calls++;
}
/* Actual renderer_texture_scale, ss_need and depth-bias block are inserted here. */
@PRODUCTION@
#define CHECK(test, code) do { if (!(test)) { fprintf(stderr, "%s (line %d)\n", #test, __LINE__); return code; } } while (0)
static void reset(void) {
    memset(&st, 0, sizeof st); memset(&ss, 0, sizeof ss);
    st.poly[3] = 4; bias_calls = enable_calls = 0; ss_on = 1;
    memset(applied, 0, sizeof applied); enabled = 0; pass_sx = pass_sy = 1;
}
static void target(float sx, float sy, bool scalable) {
    TexBackend backend = {.scalable=scalable, .sx=sx, .sy=sy};
    NvnTexture texture = {.backend=&backend};
    renderer_texture_scale(&texture, &pass_sx, &pass_sy);
}
static int planes(void) {
    /* Public synthetic planes: no captured geometry, shaders or constants. */
    const float gradients[][2] = {{1.f/256, 1.f/512}, {1.f/1024, -1.f/128}};
    const float scales[] = {1, 2, .5f, .6f, 1.5f};
    for (int g = 0; g < 2; g++) for (int sign = -1; sign <= 1; sign += 2) {
        const float triangle[3][3] = {{0,0,.25f}, {1,0,.25f+gradients[g][0]}, {0,1,.25f+gradients[g][1]}};
        float dx = triangle[1][2] - triangle[0][2], dy = triangle[2][2] - triangle[0][2];
        float native_slope = fmaxf(fabsf(dx), fabsf(dy));
        float receiver = triangle[0][2] + .25f*dx + .25f*dy;
        float decal = receiver - sign*2*native_slope;
        for (int s = 0; s < 5; s++) {
            reset(); st.poly_offset[0] = sign*3.f; target(scales[s], scales[s], true); apply(false);
            float raster_slope = fmaxf(fabsf(dx/pass_sx), fabsf(dy/pass_sy));
            float bias = applied[0]*raster_slope;
            bool accepted = sign < 0 ? decal+bias <= receiver : decal+bias >= receiver;
            if (!accepted) fprintf(stderr, "Decal lost at %.3gx scale, slope %.6g, bias %.6g\n", scales[s], raster_slope, bias);
            CHECK(accepted, 41);
            CHECK(fabsf(bias - sign*3*native_slope) < 1e-7f, 42);
            CHECK(st.poly_offset[0] == sign*3.f && applied[1] == 0 && applied[2] == 0, 43);
        }
    }
    return 0;
}
static int state(void) {
    reset(); st.poly_offset[0]=3; st.poly_offset[1]=7; st.poly_offset[2]=9;
    const float original[3] = {3,7,9};
    target(2,2,true); apply(false);
    CHECK(enabled && applied[0] == 6 && applied[1] == 7 && applied[2] == 9, 51);
    CHECK(!memcmp(st.poly_offset, original, sizeof original), 52);
    CHECK(bias_calls == 1 && enable_calls == 1, 53);
    apply(false); CHECK(bias_calls == 1 && enable_calls == 1, 54);
    target(1,1,true); apply(false); CHECK(applied[0] == 3 && bias_calls == 2 && enable_calls == 1, 55);
    target(2,2,true); apply(false); CHECK(applied[0] == 6 && bias_calls == 3, 56);
    /* A shadow/offscreen target stays 1x even when the scalable scene target was 2x. */
    target(2,2,false); apply(false); CHECK(pass_sx == 1 && pass_sy == 1 && applied[0] == 3 && bias_calls == 4, 57);
    apply(false); CHECK(bias_calls == 4, 58);
    ss.known = 0; apply(false); CHECK(bias_calls == 5 && enable_calls == 2, 59);
    ss_on = 0; apply(false); apply(false); CHECK(bias_calls == 7 && enable_calls == 4, 60);
    ss_on = 1;
    st.poly[3] = 0; target(2,2,true); apply(false);
    CHECK(!enabled && applied[0] == 0 && applied[1] == 0 && applied[2] == 0, 61);
    st.poly[3] = 3; apply(false); CHECK(!enabled && applied[0] == 0, 62);
    apply(true); CHECK(enabled && applied[0] == 0 && applied[1] == -8 && applied[2] == 0, 63);
    st.poly[3] = 4; apply(true); CHECK(enabled && applied[0] == 6 && applied[1] == -1 && applied[2] == 9, 64);
    CHECK(!memcmp(st.poly_offset, original, sizeof original), 65);
    st.poly_offset[0] = 0; apply(false); CHECK(applied[0] == 0 && applied[1] == 7 && applied[2] == 9, 66);
    const float unchanged[][2] = {{2,1}, {1,2}, {0,0}, {-1,-1}, {INFINITY,INFINITY}, {NAN,NAN}};
    st.poly_offset[0] = -3;
    for (int i = 0; i < 6; i++) {
        target(unchanged[i][0], unchanged[i][1], true); apply(false);
        CHECK(applied[0] == -3 && applied[1] == 7 && applied[2] == 9, 67);
    }
    target(4,4,true); st.poly_offset[0] = 1e38f; apply(false);
    CHECK(applied[0] == 1e38f && isfinite(applied[0]), 68);
    return 0;
}
static int disabled(void) {
    reset(); st.poly_offset[0]=3; st.poly_offset[1]=7; st.poly_offset[2]=9;
    target(2,2,true); apply(false);
    CHECK(!enabled && applied[0] == 0 && applied[1] == 0 && applied[2] == 0, 71);
    apply(true); CHECK(enabled && applied[0] == 0 && applied[1] == -8 && applied[2] == 0, 72);
    CHECK(st.poly_offset[0] == 3 && st.poly_offset[1] == 7 && st.poly_offset[2] == 9, 73);
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 2) return 99;
    if (!strcmp(argv[1], "planes")) return planes();
    if (!strcmp(argv[1], "state")) return state();
    if (!strcmp(argv[1], "disabled")) return disabled();
    return 98;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class DepthBiasTests(unittest.TestCase):
    def test_prepared_bias_preserves_plane_acceptance_and_dynamic_state(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            execute = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            textures = (runtime/'nvn/vulkan/textures.c').read_text(encoding='utf-8')
            scale = section(textures, 'void renderer_texture_scale(', '\n}')+'\n}\n'
            cache = section(execute, 'static inline bool ss_need(', '\n}')+'\n}\n'
            block = section(execute, '    {\n        float f[3] = {0, 0, 0};', '\n    // ---- textures first')
            production = scale+cache+'static void apply(bool equal_tol) {\n    VkCommandBuffer cb = 1;\n'+block+'\n}\n'
            source = out/'bias.c'
            source.write_text(HARNESS.replace('@PRODUCTION@', production), encoding='utf-8')
            exe = out/'bias.exe'
            build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                                    '-O2', str(source), '-o', str(exe)],
                                   env=compiler_env(), capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stderr[-3000:])
            clean = {k: v for k, v in os.environ.items() if k != 'LARECOMP_NO_DEPTH_BIAS'}
            for mode in ('planes', 'state', 'disabled'):
                with self.subTest(mode=mode):
                    env = dict(clean, LARECOMP_NO_DEPTH_BIAS='1') if mode == 'disabled' else clean
                    result = subprocess.run([str(exe), mode], env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


class DepthBiasBoundaryTests(unittest.TestCase):
    def test_refuses_changed_duplicate_and_already_prepared_source(self):
        original = (ROOT/'upstream/smm2recomp/recomp/runtime/nvn/vulkan/exec.c').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            path = runtime/'nvn/vulkan/exec.c'
            path.parent.mkdir(parents=True)
            for changed in (original.replace('memcpy(f, st.poly_offset, sizeof f)', 'memcpy(f, st.poly_offset, 12)'),
                            original.replace('if (equal_tol) { f[1] -= 8;', 'if (equal_tol) { f[1] -= 4;'),
                            original+original):
                path.write_text(changed, encoding='utf-8')
                before = path.read_bytes()
                with self.assertRaisesRegex(ValueError, 'Depth-bias scale boundary changed'):
                    depth_bias.prepare(runtime)
                self.assertEqual(path.read_bytes(), before)
            path.write_text(original, encoding='utf-8')
            depth_bias.prepare(runtime)
            prepared = path.read_bytes()
            with self.assertRaisesRegex(ValueError, 'Depth-bias scale boundary changed'):
                depth_bias.prepare(runtime)
            self.assertEqual(path.read_bytes(), prepared)


if __name__ == '__main__':
    unittest.main()
