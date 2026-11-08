"""The "name this thread" exception (0x406D1388) is continued, never reported as a crash.

Windows tools learn a thread's name from an exception a program raises and expects a debugger to swallow; SDL3 raises
it while creating a thread in some conditions. Nothing handled it here, so it reached the runtime's crash filter and
the game stopped at boot with exit code 0xC0000409 (track battery, GCN Yoshi Circuit, 08/11/2026: one start in 26).
The vectored handler now continues it, as a debugger would.
"""

OLD = '''static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
'''
NEW = '''static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode == 0x406D1388) return EXCEPTION_CONTINUE_EXECUTION;  // thread name (toolchain/thread_name_exception.py)
'''


def prepare(runtime):
    source = runtime/'platform/windows.c'
    text = source.read_text(encoding='utf-8')
    if text.count(OLD) != 1:
        raise ValueError('Windows exception handler boundary changed upstream')
    source.write_text(text.replace(OLD, NEW), encoding='utf-8')
