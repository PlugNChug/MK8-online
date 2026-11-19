"""Skip an empty submission instead of paying a vkQueueSubmit2 for it (MK8_SKIP_EMPTY=0 disables).

The consensus found 5,696 zero-draw/zero-pass submits in 5,594 frames (~28% of submit time). A submission is
skippable only when it carries no semaphores (the frame-end WSI one does), no upload command buffer (texture/vertex
copies), no profiling timestamp, and no draw or pass has been recorded since the previous submission. The command
buffer is left open, so the next submit carries its bookkeeping (counter query resets, gp marks) in order.
"""
from pathlib import Path

GLOBALS_OLD = 'static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;\n'
GLOBALS_NEW = GLOBALS_OLD + 'static uint64_t mk8_last_draws, mk8_last_passes;  // MK8 (toolchain/skip_empty.py)\n'

SKIP_OLD = '''    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    vc_flush();
'''
SKIP_NEW = '''    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    vc_flush();
    if (!wait && !signal && !extra && !s->upload_begun &&  // (a merged profiling timestamp stays in the CB)
        stat_draws == mk8_last_draws && stat_passes == mk8_last_passes) {  // MK8 (toolchain/skip_empty.py)
        static int on = -1;
        if (on < 0) { const char *e = getenv("MK8_SKIP_EMPTY"); on = !e || atoi(e) != 0; }
        if (on) return;  // nothing recorded: the next submit carries this slot's bookkeeping
    }
'''

UPDATE_OLD = '''    cur_sub = (cur_sub + 1) % SUBS;
    stat_flushes++;
'''
UPDATE_NEW = '''    mk8_last_draws = stat_draws;  // MK8 (toolchain/skip_empty.py)
    mk8_last_passes = stat_passes;
    cur_sub = (cur_sub + 1) % SUBS;
    stat_flushes++;
'''


def prepare(runtime: Path):
    ex = runtime/'nvn/vulkan/exec.c'
    text = ex.read_text(encoding='utf-8')
    if 'toolchain/skip_empty.py' in text:
        return
    for old, new in ((GLOBALS_OLD, GLOBALS_NEW), (SKIP_OLD, SKIP_NEW), (UPDATE_OLD, UPDATE_NEW)):
        if text.count(old) != 1:
            raise ValueError('skip empty boundary changed: ' + old.strip()[:50])
        text = text.replace(old, new, 1)
    ex.write_text(text, encoding='utf-8')
