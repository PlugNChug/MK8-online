"""The translation thread destroys retired textures (runtime/mk8/graphics/texture_refs.inc).

toolchain/async_submit.py moved command translation to its own thread; the texture backends it uses have no lock, and
textures whose last reference dropped on a game thread were destroyed there, concurrently with the translator. Two
crashes of the 0.1.4 tests came from it: STATUS_HEAP_CORRUPTION in a menu, and an access violation inside the driver's
vkCreateImageView (mk8drecomp: objects.c -> exec.c -> textures.c vk_image_view) at the start of a race. The worker
now marks itself as the translator and destroys the queued textures before each item.
"""

WORKER = '''static void *mk8_q_worker(void *arg) {
    (void)arg;
    for (;;) {
'''
MARKED = '''static void *mk8_q_worker(void *arg) {
    (void)arg;
    extern void mk8_texture_translator_thread(void);  // MK8 (toolchain/texture_release_thread.py)
    extern void mk8_textures_drain(void);
    mk8_texture_translator_thread();
    for (;;) {
'''
ITEM = '''        pthread_mutex_unlock(&mk8_q_lock);
        if (it->kind == MK8_Q_SUBMIT) {
'''
DRAINED = '''        pthread_mutex_unlock(&mk8_q_lock);
        mk8_textures_drain();  // (textures retired on the game's threads: destroyed here, where their backends live)
        if (it->kind == MK8_Q_SUBMIT) {
'''


def prepare(runtime):
    objects = runtime/'nvn/objects.c'
    text = objects.read_text(encoding='utf-8')
    if text.count(WORKER) != 1 or text.count(ITEM) != 1 or 'void mk8_textures_drain(void) {' not in text:
        raise ValueError('Async translation worker boundary changed')
    objects.write_text(text.replace(WORKER, MARKED).replace(ITEM, DRAINED), encoding='utf-8')
