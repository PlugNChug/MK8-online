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

    # The filesystem rebuilds its RomFS index on a soft reboot. Re-read the corresponding
    # preferences and overlay list at that same boundary, after guest threads have stopped.
    adapt('mods.c', [
        ('#include "rt.h"', '#include "rt.h"\n#include "hle.h"'),
        ('    static pthread_once_t once = PTHREAD_ONCE_INIT;\n', ''),
        ('static char g_overlay', '''static pthread_once_t once = PTHREAD_ONCE_INIT;
RT_ON_RESET(overlay_reset) {
    pthread_once_t fresh = PTHREAD_ONCE_INIT;
    once = fresh;
}
static char g_overlay'''),
        ('    snprintf(g_overlay', '    int length = snprintf(g_overlay'),
        ('? "\\n" : "");', '? "\\n" : "");\n    if (length < 0 || (size_t)length >= sizeof g_overlay) FATAL("[mk8:mods] overlay paths too long\\n");'),
    ])
    adapt('usermods.c', [
        ('#include "rt.h"', '#include "rt.h"\n#include "hle.h"'),
        ('static char g_started[4096];', 'static char g_started[MAX_MODS * (sizeof g_e[0].m.id + 33) + 1];'),
        ('static bool g_started_set;', '''static bool g_started_set;
RT_ON_RESET(installed_mods_reset) {
    pthread_mutex_lock(&g_lock);
    g_scanned = false;
    g_started_set = false;
    pthread_mutex_unlock(&g_lock);
}'''),
        ('    if (g_n >= MAX_MODS || find_id(id) >= 0) return;', '''    if (!*id || *id == '#' || !strcmp(id, ".") || !strcmp(id, "..") || strpbrk(id, "/\\\\:=\\r\\n") ||
        strlen(id) >= sizeof g_e[0].m.id || g_n >= MAX_MODS || find_id(id) >= 0) return;'''),
    ])
    # Videos open by path, outside the RomFS index: serve a mod's or pack's copy like every other file.
    adapt('hle/movie.c', [
        ('    snprintf(host, sizeof host, "%s/%s", g_romfs_dir, rel ? rel : "");',
         '    extern bool mods_romfs_override(const char *, char *, size_t);\n'
         '    if (!rel || !mods_romfs_override(rel, host, sizeof host))\n'
         '        snprintf(host, sizeof host, "%s/%s", g_romfs_dir, rel ? rel : "");'),
    ])
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
