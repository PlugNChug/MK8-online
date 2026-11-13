"""Compatibilidade do esquema do launcher com o leitor e os bindings nativos."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolchain"))
from native import DEFAULT_COMPILER, compiler_env

HARNESS = r'''
#include <stdio.h>
#include "settings.h"
#include "input_bind.h"
const char *const g_tunic_colours[N_TUNIC_COLOURS] = {0};
const char *const g_heart_colours[N_HEART_COLOURS] = {0};
void rt_log(const char *fmt, ...) { (void)fmt; }
int input_pad_family(void) { return 0; }
int main(void) {
    settings_load();
    printf("%.2f %d %d %d %d %d %d %d\n", g_settings.volume, g_settings.res_mode,
           g_settings.fullscreen, g_settings.show_fps, g_settings.language,
           g_settings.upscaler, g_settings.antialias, g_settings.frame_gen);
    printf("%d %d %d %d %d %d %d %d %d %d\n", g_settings.aspect, g_settings.theme,
           g_settings.reverb, g_settings.movie_audio, g_settings.mute_in_background,
           g_settings.skip_intro, g_settings.show_memory, g_settings.ahead_fast,
           g_settings.prompts, g_settings.pads_from_p2);
    bind_settings_save(stdout);
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), "Compilador nativo necessário")
class RuntimeSettingsTests(unittest.TestCase):
    def test_shared_ini_values_and_remapping_are_loaded_by_the_runtime(self):
        tools = Path(os.environ.get("MK8_NATIVE_TOOLS", ROOT / "local/native-tools"))
        base = ROOT / "upstream/smm2recomp/recomp/runtime"
        deps = tools / "deps/packages/ucrt64"
        schema = json.loads((ROOT / "apps/launcher/game-settings.json").read_text(encoding="utf-8"))
        defaults = {f["key"]: f["default"] for g in schema["groups"] for f in g["fields"]}
        with tempfile.TemporaryDirectory(prefix="mk8-settings-native-") as temporary:
            folder = Path(temporary)
            harness = folder / "main.c"
            harness.write_text(HARNESS)
            exe = folder / "settings.exe"
            result = subprocess.run([DEFAULT_COMPILER, "cc", "-target", "x86_64-windows-gnu", "-O1",
                "-I" + str(base), "-I" + str(deps / "include"), harness, base / "settings.c",
                base / "hle/bindings.c", deps / "lib/libSDL3.dll.a", "-o", exe],
                capture_output=True, env=compiler_env())
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            env = dict(os.environ, LARECOMP_SETTINGS=str(folder / "mk8d.ini"),
                       PATH=str(deps / "bin") + os.pathsep + os.environ.get("PATH", ""))
            def run(values):
                (folder / "mk8d.ini").write_text("".join(f"{k} = {v}\n" for k,v in values.items()))
                result = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout
            baseline = run(defaults)
            self.assertEqual(baseline, run({}))
            self.assertEqual(baseline.splitlines()[0], "1.00 0 0 0 0 2 0 0")
            self.assertNotIn("bind_", baseline)
            changed = dict(defaults, volume="0.65", resolution="75%", fullscreen="1",
                           show_fps="1", language="fr", upscaler="bilinear", antialias="smaa",
                           frame_gen="120", bind_key_a="Space", bind_pad_a="south")
            output = run(changed)
            self.assertEqual(output.splitlines()[0], "0.65 10 1 1 4 0 1 1")
            self.assertIn("bind_key_a = Space", output)
            self.assertIn("bind_pad_a = south", output)
