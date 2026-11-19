"""Bound every draw's pipeline wait to the shader budget (MK8_INLINE_PIPE=1 restores the full wait).

toolchain/shader_budget.py already waits at most 4 ms per frame for a pending pipeline when the draw's targets are
screen-sized, but for a non-scalable target (shadow map, probe, cube map) it ran the build inline / waited for it:
one first-visit pipeline cost 131 ms in a race frame. Take the same budgeted path for those targets too; if the build
is not ready the existing skip path drops the draw for that frame and the next one adopts the pipeline. Truly one-shot
targets can therefore miss a frame of content - MK8_INLINE_PIPE=1 restores the old unbounded wait.
"""
from pathlib import Path

OLD = '    if (budget < 0 || !mk8_targets_redrawn()) {\n'
NEW = '''    static int inline_pipe = -1;  // MK8 (toolchain/prefetch_nowait.py)
    if (inline_pipe < 0) inline_pipe = getenv("MK8_INLINE_PIPE") != NULL;
    if (budget < 0 || (inline_pipe && !mk8_targets_redrawn())) {
'''


def prepare(runtime: Path):
    ex = runtime/'nvn/vulkan/exec.c'
    text = ex.read_text(encoding='utf-8')
    if 'toolchain/prefetch_nowait.py' in text:
        return
    if text.count(OLD) != 1:
        raise ValueError('shader budget full-wait boundary changed')
    ex.write_text(text.replace(OLD, NEW, 1), encoding='utf-8')
