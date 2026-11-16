"""Tell the game it is in focus again when the controller support applet "closes".

On the Switch, nn::hid::ShowControllerSupport runs a system applet: the game leaves the foreground and, when the
applet closes, the applet manager queues FocusStateChanged (the state, read with GetCurrentFocusState, is in focus
again). The pinned runtime answers the call at once and queues nothing. MK8 4.0.1's two-player character select (local
multiplayer, after "2 player(s)") then stayed frozen: the game kept rendering the same frame, byte for byte, while its
threads ran at 60 Hz and ignored both players' buttons (14/11/2026). The answer now also queues FocusStateChanged, as
the applet's end does. MK8_NO_APPLET_FOCUS=1: off.

With fewer players than requested, return ControllerSupportCanceled (2202-3101), not success with an invalid
count: MK8 otherwise reopens the applet every few frames and its character screen appears frozen.
"""

AM_BOUNDARY = 'static uint32_t new_event(void) {\n'
AM_PUSH = '''// MK8 (toolchain/controller_applet.py): a message for the game, as the applet manager queues them (signals the event).
void mk8_am_push_message(uint32_t message) {
    am_init_once();
    pthread_mutex_lock(&g_klock);
    if (msg_count < (int)(sizeof msg_queue / sizeof msg_queue[0])) msg_queue[msg_count++] = message;
    kernel_signal(msg_event);
    pthread_mutex_unlock(&g_klock);
}

'''
SUPPORT_LOG = ('    LOG("[hid] controller support (players %d..%d): %d player(s)%s\\n", min, max, n,\n'
               '        n < min ? " - fewer than the game asks for" : "");\n')
SUPPORT_FOCUS = SUPPORT_LOG + '''    if (!getenv("MK8_NO_APPLET_FOCUS")) {  // MK8 (toolchain/controller_applet.py): the applet closed, the game is in focus
        extern void mk8_am_push_message(uint32_t message);
        mk8_am_push_message(15);  // FocusStateChanged
    }
'''


def prepare(runtime):
    am = runtime/'services/am.c'
    text = am.read_text(encoding='utf-8')
    if text.count(AM_BOUNDARY) != 1 or 'static void am_init_once(void) {' not in text or 'mk8_am_push_message' in text:
        raise ValueError('Applet message boundary changed: services/am.c')
    am.write_text(text.replace(AM_BOUNDARY, AM_PUSH + AM_BOUNDARY), encoding='utf-8')
    hid = runtime/'hle/input.c'
    text = hid.read_text(encoding='utf-8')
    if text.count(SUPPORT_LOG) != 1 or 'mk8_am_push_message' in text:
        raise ValueError('Controller support boundary changed: hle/input.c')
    boundary = '         hid_show_controller_support) {\n'
    if text.count(boundary) != 1:
        raise ValueError('Controller support function boundary changed')
    start = text.index(boundary)
    end = text.index('\n}\n', start)
    support = text[start:end]
    if support.count('    if (out) {') != 1 or support.count('    c->x[0] = 0;') != 1:
        raise ValueError('Controller support result boundary changed')
    support = support.replace('    if (out) {', '    if (out && n >= min) {')
    support = support.replace('    c->x[0] = 0;',
        '    c->x[0] = n < min ? (3101u << 9) | 202u : 0;  // ControllerSupportCanceled (2202-3101)')
    text = text[:start] + support + text[end:]
    hid.write_text(text.replace(SUPPORT_LOG, SUPPORT_FOCUS), encoding='utf-8')
