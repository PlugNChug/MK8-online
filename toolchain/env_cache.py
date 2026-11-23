"""Read each environment option of the Vulkan renderer once instead of on every draw (#133).

The renderer reads its options with getenv at the point of use, and several of those points run per draw or per pass:
the FSR/DLSS/frame-generation checks (toolchain/fsr.py, fsr_fg.py, dlss_fg.py) alone made four to five calls in every
draw and four more at every pass end, whether or not an upscaler was on. On Windows getenv walks the whole environment
block under a lock, and a game started by the launcher, Steam or Nextendo carries a large one. A 12-racer Grand Prix on
an RX 580 + Xeon E5-2666 v3 spent 36% of the render thread inside getenv and ran at 27-45 FPS (23/11/2026); players
reported 24-30 FPS in races since 0.6.0 while menus stayed at 60.

The launcher sets these options before the game starts and nothing changes them while it runs, so each call site with
a literal name now reads its variable once (MK8_ENV). Names the process itself sets at run time keep the plain call.
Only nvn/vulkan/*.c is rewritten: the renderer starts after main() has loaded its environment file.
"""
from pathlib import Path
import re

MACRO = '''// MK8 (toolchain/env_cache.py): read an option once per call site; the launcher fixes them before the game starts.
#ifndef MK8_ENV
#define MK8_ENV(name) (__extension__({ \\
    static const char *mk8_env_value_; static int mk8_env_read_; \\
    if (!__atomic_load_n(&mk8_env_read_, __ATOMIC_ACQUIRE)) { \\
        mk8_env_value_ = getenv(name); __atomic_store_n(&mk8_env_read_, 1, __ATOMIC_RELEASE); } \\
    mk8_env_value_; }))
#endif
'''
MARKER = '#define MK8_ENV(name)'
HEADER = '#include <stdlib.h>\n' + MACRO
# Set by the process while it runs (main.c, host.py, net/embed_server.c, gamebuild.c, platform/*): never cached.
RUNTIME_SET = {'LARECOMP_GAME_LIB', 'LARECOMP_REEXEC', 'RECOMP_MOD_SERVER', 'RECOMP_MOD_TOKEN', 'RECOMP_MOD_100MARIO',
               'ZIG_GLOBAL_CACHE_DIR', 'ZIG_LOCAL_CACHE_DIR', 'LASRECOMP_LOG_APPEND'}
CALL = re.compile(r'(?<![A-Za-z0-9_])getenv\(\s*"([A-Za-z0-9_]+)"\s*\)')


def rewrite(text):
    """The text with cacheable getenv("NAME") calls turned into MK8_ENV("NAME"), and how many were turned."""
    count = 0

    def one(match):
        nonlocal count
        if match[1] in RUNTIME_SET:
            return match[0]
        count += 1
        return f'MK8_ENV("{match[1]}")'

    body = CALL.sub(one, text)
    # (A tree prepared before this placement has the macro after the last #include: it moves.)
    body = body.replace(HEADER, '').replace(MACRO + '\n', '')
    if 'MK8_ENV("' in body:
        lines = body.split('\n')
        # After the first #include: feature macros such as _GNU_SOURCE come before it, and some files have code
        # between later includes.
        first = next((i for i, line in enumerate(lines) if line.startswith('#include')), -1)
        lines.insert(first + 1, HEADER.rstrip('\n'))
        body = '\n'.join(lines)
    return body, count


def prepare(runtime: Path):
    total = 0
    for source in sorted((runtime/'nvn/vulkan').glob('*.c')):
        text = source.read_text(encoding='utf-8')
        new, count = rewrite(text)
        if new != text:
            source.write_text(new, encoding='utf-8')
        total += count
    return total
