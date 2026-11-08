"""A submission ring that holds the work of three frames, so command translation does not wait on the GPU.

Upstream keeps 8 submissions in flight. Since compute dispatches are submitted on their own
(compute_flush.py), an MK8 race frame makes about 9 submissions: 887 per 2 s in the Grand Prix of 07/11/2026,
with 248 ms of "ring slot" waits in that interval. Reusing a slot waits for the submission it held, so the
translating thread, which is the game's own submitting thread, could not get even one frame ahead of the GPU.
The swapchain already limits how far ahead the CPU runs (three frames in flight), and 32 slots cover them.
Per-slot resources are created on first use and tracked by submission number: transient memory, descriptor
pools, counter and occlusion queries.
"""

OLD = '#define SUBS 8\n'
NEW = '#define SUBS 32  // MK8 (toolchain/submit_ring.py): ~9 submissions per frame, three frames in flight\n'


def prepare(runtime):
    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    if text.count(OLD) != 1:
        raise ValueError('Submission ring boundary changed')
    execute.write_text(text.replace(OLD, NEW), encoding='utf-8')
