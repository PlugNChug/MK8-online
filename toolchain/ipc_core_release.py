"""A guest thread gives its emulated core up while a service handles its SendSyncRequest (svc 0x21).

runtime/kernel.c keeps Horizon's rule that the threads of one core never run at once: a thread holds its core while it
runs and gives it up while blocked in the kernel. A service request (IPC) is handled on the calling thread, so a slow
one (file reads at a load screen) kept the core for its whole duration: in a manual session, threads of core 0 waited
0.26-2.1 s for a thread whose last svc was 0x21, in menu transitions. On the console the caller is blocked during IPC
and its core runs other threads. The services are already reached at once from threads of different cores, so this adds
no new kind of concurrency to them, and none runs guest code. kernel_host_wait_begin/end is upstream's own helper for a
service that waits in the host (sockets, TLS, the present's pacing).

The stall warning also says whether the holder is inside that svc or already back in guest code after it.
"""

DISPATCH = '    case 0x21: x[0] = ipc_handle_request(c, (uint32_t)x[0]); break;  // SendSyncRequest\n'
RELEASED = ('    case 0x21:  // SendSyncRequest: the core is free while the service handles it (toolchain/ipc_core_release.py)\n'
            '        kernel_host_wait_begin();\n'
            '        x[0] = ipc_handle_request(c, (uint32_t)x[0]);\n'
            '        kernel_host_wait_end();\n'
            '        break;\n')
WARNING = '''                LOG("[kernel] thread %llu waits %llums for core %d (held by thread %llu, last svc %02x)\\n", (unsigned long long)t->id,
                    (unsigned long long)((plat_now_ns() - t0) / 1000000), t->core, (unsigned long long)c->owner->id,
                    c->owner->last_svc & 0xFF);
'''
WARNED = '''                LOG("[kernel] thread %llu waits %llums for core %d (held by thread %llu, %s svc %02x)\\n", (unsigned long long)t->id,
                    (unsigned long long)((plat_now_ns() - t0) / 1000000), t->core, (unsigned long long)c->owner->id,
                    (c->owner->last_svc & 0x100) ? "inside" : "after", c->owner->last_svc & 0xFF);  // MK8 (ipc_core_release.py)
'''


def prepare(runtime):
    kernel = runtime/'kernel.c'
    text = kernel.read_text(encoding='utf-8')
    if text.count(DISPATCH) != 1 or text.count(WARNING) != 1 or 'void kernel_host_wait_begin(void)' not in text:
        raise ValueError('Kernel SendSyncRequest boundary changed upstream')
    kernel.write_text(text.replace(DISPATCH, RELEASED).replace(WARNING, WARNED), encoding='utf-8')
