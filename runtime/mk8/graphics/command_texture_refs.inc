struct Mk8TextureRefs { unsigned count; NvnTexture *textures[9]; NvnDescPool *pool; };

static void mk8_command_pool(CmdRec *r, NvnDescPool *pool) {
    if (!pool) return;
    if (!r->mk8_textures) {
        r->mk8_textures = calloc(1, sizeof *r->mk8_textures);
        if (!r->mk8_textures) FATAL("Command pool allocation failed\n");
    }
    if (r->mk8_textures->pool) FATAL("Command pool recorded twice\n");
    r->mk8_textures->pool = mk8_descriptor_retain(pool);
}

static void mk8_command_texture(CmdRec *r, uint64_t pointer) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)pointer;
    if (!t) return;
    if (!r->mk8_textures) {
        r->mk8_textures = calloc(1, sizeof *r->mk8_textures);
        if (!r->mk8_textures) FATAL("Command texture allocation failed\n");
    }
    struct Mk8TextureRefs *refs = r->mk8_textures;
    for (unsigned i = 0; i < refs->count; i++) if (refs->textures[i] == t) return;
    if (refs->count == 9) FATAL("Command texture capacity exceeded\n");
    refs->textures[refs->count++] = mk8_texture_retain(t);
}

static void mk8_copy_command_textures(CmdRec *destination, const CmdRec *source) {
    destination->mk8_textures = NULL;
    if (source->mk8_textures) mk8_command_pool(destination, source->mk8_textures->pool);
    if (source->mk8_textures)
        for (unsigned i = 0; i < source->mk8_textures->count; i++)
            mk8_command_texture(destination, (uintptr_t)source->mk8_textures->textures[i]);
}

// EndRecording transfers these references to the immutable list; abandoned recordings release them here.
static void mk8_release_records(uint8_t *mem, uint64_t bytes) {
    uint64_t offset = 0;
    while (offset < bytes) {
        if (bytes - offset < sizeof(CmdRec)) FATAL("Truncated owned command\n");
        CmdRec *r = (CmdRec *)(mem + offset);
        if (r->size < sizeof *r || r->size > bytes - offset) FATAL("Invalid owned command size\n");
        if (r->mk8_textures) {
            mk8_descriptor_release(r->mk8_textures->pool);
            for (unsigned i = 0; i < r->mk8_textures->count; i++)
                mk8_texture_release(r->mk8_textures->textures[i]);
            free(r->mk8_textures);
            r->mk8_textures = NULL;
        }
        if (r->op == CMD_END) return;
        offset += r->size;
    }
}

static void mk8_free_list(uint8_t *mem) {
    // Lists have an internal CMD_END; their existing graveyard governs when they may be reclaimed.
    mk8_release_records(mem, UINT64_MAX);
    free(mem);
}
