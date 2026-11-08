#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

NvnTexture *mk8_texture_retain(NvnTexture *t) {
    if (t && !__atomic_fetch_add(&t->mk8_refs, 1, __ATOMIC_RELAXED))
        FATAL("Retaining a retired texture\n");
    return t;
}

// The texture backends (nvn/vulkan/textures.c: images, views, the alias table, the live list, the deferred-destruction
// list) have no lock: they belong to the thread that translates commands. With async translation
// (toolchain/async_submit.py) that is its own thread, while the last reference of a texture drops on the game's threads
// (nvnTextureFinalize, command lists released when recording). Destroying the backend there raced with the
// translation: a realloc of the deferred list under the translator (STATUS_HEAP_CORRUPTION in a menu) and a view created
// over an image already destroyed (an access violation inside the driver's vkCreateImageView at the start of a race).
// Such textures now wait in this list; the translator destroys them before its next item (toolchain/texture_release_thread.py).
// Before the translator starts (and with MK8_ASYNC_SUBMIT=0) there is only one thread translating: destroyed at once.
static pthread_mutex_t mk8_retired_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnTexture **mk8_retired;
static size_t mk8_nretired, mk8_cap_retired;
static _Atomic bool mk8_translator_running;
static __thread bool mk8_on_translator;

static void mk8_texture_destroy_now(NvnTexture *t) {
    backend_texture_destroyed(t);
    free(t);
}

void mk8_texture_translator_thread(void) {
    mk8_on_translator = true;
    atomic_store(&mk8_translator_running, true);
}

void mk8_textures_drain(void) {
    pthread_mutex_lock(&mk8_retired_lock);
    NvnTexture **list = mk8_retired;
    size_t n = mk8_nretired;
    mk8_retired = NULL;
    mk8_nretired = mk8_cap_retired = 0;
    pthread_mutex_unlock(&mk8_retired_lock);
    for (size_t i = 0; i < n; i++) mk8_texture_destroy_now(list[i]);
    free(list);
}

void mk8_texture_release(NvnTexture *t) {
    if (!t) return;
    unsigned previous = __atomic_fetch_sub(&t->mk8_refs, 1, __ATOMIC_ACQ_REL);
    if (!previous) FATAL("Texture reference underflow\n");
    if (previous != 1) return;
    if (mk8_on_translator || !atomic_load(&mk8_translator_running)) {
        mk8_texture_destroy_now(t);
        return;
    }
    pthread_mutex_lock(&mk8_retired_lock);
    if (mk8_nretired == mk8_cap_retired) {
        size_t cap = mk8_cap_retired ? mk8_cap_retired * 2 : 1024;
        NvnTexture **grown = realloc(mk8_retired, cap * sizeof *grown);
        if (!grown) FATAL("Retired texture list allocation failed\n");
        mk8_retired = grown;
        mk8_cap_retired = cap;
    }
    mk8_retired[mk8_nretired++] = t;
    pthread_mutex_unlock(&mk8_retired_lock);
}
