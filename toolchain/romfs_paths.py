"""Install the Windows long-path reader into the verified host copy."""
from pathlib import Path
import shutil


def prepare(runtime):
    header = Path(__file__).resolve().parents[1]/'runtime/mk8/support/romfs_paths.h'
    shutil.copyfile(header, runtime/'mk8_romfs_paths.h')

    def adapt(file, changes, omit=None):
        path = runtime/file
        text = path.read_text(encoding='utf-8')
        if omit:
            first, last = omit
            if text.count(first) != 1 or text.count(last) != 1:
                raise ValueError(f'RomFS path adapter: unexpected source in {file}')
            text = text[:text.index(first)] + text[text.index(last, text.index(first)):]
        for old, new in changes:
            if text.count(old) != 1:
                raise ValueError(f'RomFS path adapter: unexpected source in {file}: {old[:70]}')
            text = text.replace(old, new)
        path.write_text(text, encoding='utf-8')

    adapt('romfs_build.c', [
        ('#include "platform.h"', '#include "mk8_romfs_paths.h"\n#include "platform.h"'),
        ('static Dir *scan(', 'static void free_tree(Dir *);\n\nstatic Dir *scan('),
        ('if (depth > 32) return NULL;', 'if (depth > 32) { errno = ELOOP; return NULL; }'),
        ('DIR *d = opendir(path);', 'Mk8RomfsDir *d = mk8_romfs_opendir(path);'),
        ('struct dirent *e;\n    while ((e = readdir(d))) {', 'const char *entry;\n    while ((entry = mk8_romfs_next(d))) {'),
        ("e->d_name[0] == '.'", "entry[0] == '.'"),
        ('strlen(e->d_name)', 'strlen(entry)'),
        ('path, e->d_name);', 'path, entry);'),
        ('if (stat(p, &st) != 0) { free(p); continue; }', 'if (mk8_romfs_stat(p, &st) != 0) { free(p); break; }'),
        ('scan(p, e->d_name,', 'scan(p, entry,'),
        ('if (!c) continue;', 'if (!c) break;'),
        ('strdup(e->d_name)', 'strdup(entry)'),
        ('    closedir(d);', '''    int error = errno;
    mk8_romfs_closedir(d);
    if (error) { free_tree(dir); errno = error; return NULL; }'''),
        ('if (!root) FAIL("can\'t read %s", dir);', 'if (!root) FAIL("cannot read %s: %s", dir, strerror(errno));'),
        ('if (o) merge(root, o);', '''if (o) merge(root, o);
        else if (n) {
            int error = errno;
            struct stat st;
            if (error != ENOENT || !mk8_romfs_stat(one, &st)) {
                if (why) snprintf(why, why_size, "cannot read overlay %s: %s", one, strerror(error));
                free(one);
                goto fail;
            }
        }'''),
        ('f->size = file_size(p, &st)', 'f->size = (uint64_t)st.st_size'),
        ('fopen(path, "rb")', 'mk8_romfs_input(path)'),
        ('fopen(src, "rb")', 'mk8_romfs_input(src)'),
    ], omit=("// A file's size.", 'static Dir *scan('))
    adapt('services/fs.c', [
        ('#include "../hle.h"', '#include "../mk8_romfs_paths.h"\n#include "../hle.h"'),
        ('open(f->host, O_RDONLY)', 'mk8_romfs_open(f->host)'),
        ('strerror(errno)); continue; }', 'strerror(errno)); return -1; }'),
        ('if (r < 0) LOG("[fs] read %s failed: %s\\n", f->host, strerror(errno));',
         'if (r != (ssize_t)(b - a)) { LOG("[fs] incomplete read of %s\\n", f->host); return -1; }'),
        ('if (n < 0) LOG("[fs] storage read failed at %llx: %s\\n", off, strerror(errno));',
         'if (n < 0) { LOG("[fs] storage read failed at %llx\\n", off); resp->result = MAKE_RESULT(2, 4000); break; }'),
        ('        rix_read(ix, (uint8_t *)(uintptr_t)b->addr, size, off);',
         '        if (rix_read(ix, (uint8_t *)(uintptr_t)b->addr, size, off) < 0) resp->result = MAKE_RESULT(2, 4000);'),
        ('romfs_fd = open(path, O_RDONLY);', 'romfs_fd = mk8_romfs_open(path);'),
        ('!stat(g_romfs_dir, &ds)', '!mk8_romfs_stat(g_romfs_dir, &ds)'),
        ('if (stat(dir, &st) || !S_ISDIR', 'if (mk8_romfs_stat(dir, &st) || !S_ISDIR'),
    ])
    adapt('nvn/shader_ahead.c', [
        ('#include "../platform.h"', '#include "../mk8_romfs_paths.h"\n#include "../platform.h"'),
        ('DIR *dir = depth <= 8 ? opendir(path)', 'Mk8RomfsDir *dir = depth <= 8 ? mk8_romfs_opendir(path)'),
        ('struct dirent *e;\n    while ((e = readdir(dir))) {', 'const char *entry;\n    while ((entry = mk8_romfs_next(dir))) {'),
        ("e->d_name[0] == '.'", "entry[0] == '.'"),
        ('e->d_name);', 'entry);'),
        ('stat(full, &sb)', 'mk8_romfs_stat(full, &sb)'),
        ('closedir(dir);', 'mk8_romfs_closedir(dir);'),
        ('open(path, O_RDONLY)', 'mk8_romfs_open(path)'),
    ])
