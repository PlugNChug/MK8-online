"""Save the Vulkan pipeline cache while the game runs, not only at exit (runtime/mk8/graphics/pipeline_saver.c).

Upstream writes cache/vk_pipelines.bin from the exit handler only. A session that crashed or was killed lost every
pipeline the driver had built, and the next session compiled them again during races: a manual session after such an
exit compiled 412 at one load screen and 17-223 per 2 s while racing (50-55 FPS where the same race ran at 60).

Every pipeline creation also counts in mk8_pipeline_built; a thread saves the cache when new pipelines exist, none was
built for 10 s and a minute passed since the last save. vk_pipeline_cache_save takes a lock, so that save and the exit
handler's never write the temporary file at once.
"""

COUNT = '    atomic_fetch_add(&stat_compiles, 1);\n'
COUNTED = '    atomic_fetch_add(&stat_compiles, 1);\n    mk8_pipeline_built();  // MK8 (toolchain/pipeline_cache_save.py)\n'
DECLARE = 'static _Atomic int stat_compiles;\n'
DECLARED = DECLARE + 'void mk8_pipeline_built(void);  // MK8 (toolchain/pipeline_cache_save.py)\n'

SAVE = '''void vk_pipeline_cache_save(void) {
    size_t sz = 0;
'''
LOCKED_SAVE = '''// MK8 (toolchain/pipeline_cache_save.py): also called while the game runs (runtime/mk8/graphics/pipeline_saver.c).
static void vk_pipeline_cache_save_locked(void);
void vk_pipeline_cache_save(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    vk_pipeline_cache_save_locked();
    pthread_mutex_unlock(&lock);
}

static void vk_pipeline_cache_save_locked(void) {
    size_t sz = 0;
'''
CREATED = '''    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
'''
STARTED = CREATED + '''    extern void mk8_pipeline_saver_start(void);  // MK8 (toolchain/pipeline_cache_save.py)
    mk8_pipeline_saver_start();
'''


def prepare(runtime):
    exec_c = runtime/'nvn/vulkan/exec.c'
    text = exec_c.read_text(encoding='utf-8')
    if text.count(COUNT) != 3 or text.count(DECLARE) != 1:
        raise ValueError('Pipeline creation boundary changed upstream')
    exec_c.write_text(text.replace(DECLARE, DECLARED).replace(COUNT, COUNTED), encoding='utf-8')
    device_c = runtime/'nvn/vulkan/device.c'
    text = device_c.read_text(encoding='utf-8')
    if text.count(SAVE) != 1 or text.count(CREATED) != 1:
        raise ValueError('Pipeline cache boundary changed upstream')
    if '#include <pthread.h>' not in text:
        text = '#include <pthread.h>  // MK8 (toolchain/pipeline_cache_save.py)\n' + text
    device_c.write_text(text.replace(SAVE, LOCKED_SAVE).replace(CREATED, STARTED), encoding='utf-8')
