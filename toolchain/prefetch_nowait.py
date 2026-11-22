"""Optionally bound every draw's pipeline wait to the shader budget (MK8_PIPE_NOWAIT=1).

toolchain/shader_budget.py already waits at most 4 ms per frame for a pending pipeline when the draw's targets are
screen-sized, but for a non-scalable target (shadow map, probe, cube map) it runs the build inline / waits for it:
one first-visit pipeline cost 131 ms in a race frame. MK8_PIPE_NOWAIT=1 takes the same budgeted path for those
targets too; if the build is not ready the existing skip path drops the draw for that frame.

That is opt-in because truly one-shot targets then miss their content for good. With a cold driver cache on an
RX 580 (Windows, 22/11/2026) the first Mario Kart Stadium entry skipped about 33,000 draws and the race showed only
the HUD over a black scene until the next launch; the full wait drew it normally. MK8_INLINE_PIPE (the earlier
opt-out) keeps its meaning: the full wait.
"""
from pathlib import Path

OLD = '    if (budget < 0 || !mk8_targets_redrawn()) {\n'
NEW = '''    static int nowait = -1;  // MK8 (toolchain/prefetch_nowait.py)
    if (nowait < 0) nowait = getenv("MK8_PIPE_NOWAIT") && !getenv("MK8_INLINE_PIPE");
    if (budget < 0 || (!nowait && !mk8_targets_redrawn())) {
'''


def prepare(runtime: Path):
    ex = runtime/'nvn/vulkan/exec.c'
    text = ex.read_text(encoding='utf-8')
    if 'toolchain/prefetch_nowait.py' in text:
        return
    if text.count(OLD) != 1:
        raise ValueError('shader budget full-wait boundary changed')
    ex.write_text(text.replace(OLD, NEW, 1), encoding='utf-8')
