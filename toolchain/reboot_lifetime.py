"""Drena a tradução e preserva backends compartilhados durante o reboot."""

CHANGES = (
    ('main.c', '    kernel_stop_threads(reboot_idle);\n    backend_clear_window();',
     '''    kernel_stop_threads(reboot_idle);
    // Sem produtores guest: aguardar também o consumidor host antes de alterar seu estado.
    extern void mk8_async_drain_hook(void);
    extern void mk8_textures_drain(void);
    mk8_async_drain_hook();
    mk8_textures_drain();
    backend_clear_window();'''),
    ('main.c', '    backend_reset();\n    const ResetFn *start',
     '    backend_reset();\n    mk8_textures_drain();\n    const ResetFn *start'),
    ('nvn/vulkan/textures.c', '        if (b) b->refs = 1;  // (its aliases go with it)\n', ''),
    ('nvn/vulkan/textures.c', '''    for (unsigned k = 0; k < ALIAS_BUCKETS; k++)  // (backends whose listed owner had gone)
        while (g_alias[k]) backend_free(g_alias[k]->b);''',
     '''    for (unsigned k = 0; k < ALIAS_BUCKETS; k++) {
        while (g_alias[k]) {
            TexBackend *b = g_alias[k]->b;
            // Listas antigas ainda podem liberar outros owners depois do reset.
            alias_remove(b);
            destroy_image(b);
            free(b->pending_rgba);
            b->pending_rgba = NULL;
        }
    }'''),
)


def prepare(runtime):
    texts = {name: (runtime/name).read_text(encoding='utf-8') for name, _, _ in CHANGES}
    for name, old, _ in CHANGES:
        if texts[name].count(old) != 1:
            raise ValueError('Reboot texture lifetime boundary changed')
    for name, old, new in CHANGES:
        texts[name] = texts[name].replace(old, new)
    for name, text in texts.items():
        (runtime/name).write_text(text, encoding='utf-8')
