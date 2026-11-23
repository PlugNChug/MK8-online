"""Read each environment option of the Vulkan renderer once instead of on every draw (#133).

The renderer reads its options with getenv at the point of use, and several of those points run per draw or per pass:
the FSR/DLSS/frame-generation checks (toolchain/fsr.py, fsr_fg.py, dlss_fg.py) alone made four to five calls in every
draw and four more at every pass end, whether or not an upscaler was on. On Windows getenv walks the whole environment
block under a lock, and a game started by the launcher, Steam or Nextendo carries a large one. A 12-racer Grand Prix on
an RX 580 + Xeon E5-2666 v3 spent 36% of the render thread inside getenv and ran at 27-45 FPS (23/11/2026); players
reported 24-30 FPS in races since 0.6.0 while menus stayed at 60.

The launcher sets these options before the game starts and nothing changes them while it runs, so each call site with
a literal name now reads its variable once. The cache is written out at the call site (a statement expression around
the original `getenv("NAME")`), not as a macro: tests that compile single functions taken from the prepared sources
keep compiling, and searches for `getenv("NAME")` still find it. Names the process itself sets at run time keep the
plain call. Only nvn/vulkan/*.c is rewritten: the renderer starts after main() has loaded its environment file.
"""
from pathlib import Path
import re

# Set by the process while it runs (main.c, host.py, net/embed_server.c, gamebuild.c, platform/*): never cached.
RUNTIME_SET = {'LARECOMP_GAME_LIB', 'LARECOMP_REEXEC', 'RECOMP_MOD_SERVER', 'RECOMP_MOD_TOKEN', 'RECOMP_MOD_100MARIO',
               'ZIG_GLOBAL_CACHE_DIR', 'ZIG_LOCAL_CACHE_DIR', 'LASRECOMP_LOG_APPEND'}
READ = 'mk8_env_value_ = '  # (marks a call already inside the cache)
CALL = re.compile(r'(?<![A-Za-z0-9_])(?<!' + READ + r')getenv\(\s*"([A-Za-z0-9_]+)"\s*\)')
# An earlier form of this patch: MK8_ENV("NAME") with a macro at the top of the file.
OLD_CALL = re.compile(r'(?<![A-Za-z0-9_])MK8_ENV\("([A-Za-z0-9_]+)"\)')
OLD_HEADER = re.compile(r'(?:#include <stdlib\.h>\n)?// MK8 \(toolchain/env_cache\.py\): read an option once per call site;'
                        r'.*?\n#endif\n\n?', re.S)


def cached(name):
    """An expression with getenv's value for `name`, read the first time it is evaluated."""
    return ('(__extension__({ static const char *mk8_env_value_; static int mk8_env_read_; '
            'if (!__atomic_load_n(&mk8_env_read_, __ATOMIC_ACQUIRE)) { '
            f'{READ}getenv("{name}"); __atomic_store_n(&mk8_env_read_, 1, __ATOMIC_RELEASE); }} '
            'mk8_env_value_; }))')


CACHED = re.compile(re.escape(cached('NAME_HERE')).replace('NAME_HERE', '([A-Za-z0-9_]+)'))


def plain(text):
    """The text with every cached read turned back into its getenv("NAME") call (for checks on other patches)."""
    return CACHED.sub(lambda m: f'getenv("{m[1]}")', text)


def rewrite(text):
    """The text with cacheable getenv("NAME") calls read once per call site, and how many were turned."""
    count = 0

    def one(match):
        nonlocal count
        if match[1] in RUNTIME_SET:
            return match[0]
        count += 1
        return cached(match[1])

    body = OLD_HEADER.sub('', text)
    body = OLD_CALL.sub(lambda m: f'getenv("{m[1]}")', body)
    return CALL.sub(one, body), count


def prepare(runtime: Path):
    total = 0
    for source in sorted((runtime/'nvn/vulkan').glob('*.c')):
        text = source.read_text(encoding='utf-8')
        new, count = rewrite(text)
        if new != text:
            source.write_text(new, encoding='utf-8')
        total += count
    return total
