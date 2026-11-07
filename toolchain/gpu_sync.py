"""Guest fence waits in the pinned Vulkan runtime: wait for the submission that holds the fence.

nvnSyncWait waited only for work already submitted when its fence belonged to a submission another thread was
still recording, so the game saw the fence signalled and rewrote memory the GPU had yet to read (MK8 4.0.1 boots
56-59: green/yellow blocks and blown-out frames that LARECOMP_SYNC_SUBMIT / LARECOMP_SYNC_FLUSH removed).
"""

STATE = '''
// MK8: which thread is recording the open submission (exec.c records on the thread that submits)
static pthread_cond_t mk8_seq_cond = PTHREAD_COND_INITIALIZER;
static pthread_t mk8_seq_recorder;
static bool mk8_seq_recording;
'''

WAIT = '''void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    // A fence in a submission still being recorded is satisfied once that submission is submitted and done:
    // another thread's open submission is waited for here; this thread's own is submitted first.
    pthread_mutex_lock(&seq_lock);
    uint64_t t0 = plat_now_ns();
    bool warned = false;
    while (seq > submit_seq) {
        if (!mk8_seq_recording || pthread_equal(mk8_seq_recorder, pthread_self())) {
            pthread_mutex_unlock(&seq_lock);
            wait_why = "SyncWait (open submission)";
            renderer_flush();
            return;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&mk8_seq_cond, &seq_lock, &ts);
        if (!warned && plat_now_ns() - t0 > 2000000000ull) {
            warned = true;
            LOG("[vk] SyncWait: submission %llu still open after 2 s (submitted %llu)\\n", (unsigned long long)seq,
                (unsigned long long)submit_seq);
        }
    }
    pthread_mutex_unlock(&seq_lock);
    wait_why = "SyncWait";
    vk_wait_seq(seq);
}
'''

OLD_WAIT = '''void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = seq > s ? "SyncWait (fence not submitted yet)" : "SyncWait";
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}
'''


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Fence wait boundary changed: {old[:90]}')
    return text.replace(old, new)


def prepare(runtime):
    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    lock = 'static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;\n'
    text = replace(text, lock, lock + STATE)
    text = replace(text, '#include <pthread.h>\n', '#include <pthread.h>\n#include <time.h>\n')
    begun = '    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));\n    *begun = true;\n'
    text = replace(text, begun, begun + '    pthread_mutex_lock(&seq_lock);\n    mk8_seq_recorder = pthread_self();\n'
                                        '    mk8_seq_recording = true;\n    pthread_mutex_unlock(&seq_lock);\n')
    submitted = '    pthread_mutex_lock(&seq_lock);\n    submit_seq++;\n    pthread_mutex_unlock(&seq_lock);\n'
    text = replace(text, submitted, '    pthread_mutex_lock(&seq_lock);\n    submit_seq++;\n    mk8_seq_recording = false;\n'
                                    '    pthread_cond_broadcast(&mk8_seq_cond);\n    pthread_mutex_unlock(&seq_lock);\n')
    text = replace(text, OLD_WAIT, WAIT)
    # SMM2's pacing raises the game's GPU timing counters to 20 ms so that SMM2 takes its two-steps-per-frame 30 fps
    # mode. MK8 4.0.1 has no such mode: Time Trial boot 67 measured 25.33 s of race timer over 1520 frames (16.66 ms
    # per frame, 48% speed at 29 fps) with pacing on, and the forged counters coincided with flashes. Opt-in only.
    text = replace(text, 'const char *e = getenv("LARECOMP_PACE"); on = !e || atoi(e) != 0;',
                   'const char *e = getenv("LARECOMP_PACE"); on = e && atoi(e) != 0; /* MK8: off unless asked */')
    # (Test) MK8_TIMESTAMP_TOP_GPU=1: TIMESTAMP_TOP counters (type 16) get the GPU's time too, like type 0, instead of
    # the host clock at translation: MK8 reports both, and mixing the two clocks can make an interval negative.
    text = replace(text, '            if (mode == 2 && type == 0) {  // (type 0: timestamp)\n',
                   '            static int mk8_top_gpu = -1;\n'
                   '            if (mk8_top_gpu < 0) mk8_top_gpu = getenv("MK8_TIMESTAMP_TOP_GPU") != NULL;\n'
                   '            if (mode == 2 && (type == 0 || (type == 16 && mk8_top_gpu))) {  // (type 0: timestamp; 16: top of pipe)\n')
    text = replace(text, '                    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->pool, q->n);\n                    q->out[q->n++] = out;\n',
                   '                    vkCmdWriteTimestamp2(cb, type == 16 ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,\n'
                   '                                         q->pool, q->n);\n                    q->out[q->n++] = out;\n')
    execute.write_text(text, encoding='utf-8')
    objects = runtime/'nvn/objects.c'
    text = objects.read_text(encoding='utf-8')
    text = replace(text, '''        uint64_t d = next - now;
        struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
        nanosleep(&ts, NULL);
''', '''        // Use the host's high-resolution deadline wait; Windows nanosleep truncates to milliseconds.
        plat_sleep_until_ns(next);
''')
    objects.write_text(text, encoding='utf-8')
