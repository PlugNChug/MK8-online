"""Close the host file or directory when the game closes its IFile / IDirectory (runtime/services/fs.c).

Upstream opens a host descriptor for every OpenFile / OpenDirectory and never closes it: the objects had no on_close, so
each save written left its file open until the process ended. macOS and Linux delete open files; Windows does not.
In a Grand Prix, MK8 creates a file in its save (rp*.dat, after reading a texture back), closes it and deletes it:
DeleteFile failed with EACCES, which the service returned as an unknown result (0x1f4002), and the game aborted
(nn::diag AbortImpl, svcBreak) at the same point of two automated runs (~500 s). on_close is upstream's own hook for
the last reference of an object going away (audio.c, ssl.c).
"""

ANCHOR = 'static void filesystem(Service *s, IpcReq *req, IpcResp *resp) {\n'
CLOSERS = '''// MK8 (toolchain/save_file_close.py): the descriptor goes with the game's object (Windows keeps open files undeletable).
static void host_file_close(Service *s) {
    HostFile *f = s->state;
    if (!f) return;
    s->state = NULL;
    close(f->fd);
    free(f);
}
static void host_dir_close(Service *s) {
    HostDir *d = s->state;
    if (!d) return;
    s->state = NULL;
    closedir(d->d);
    free(d);
}

''' + ANCHOR
FILE = '        ipc_return_object(resp, service_new("IFile", file, f));\n'
FILE_CLOSED = ('        Service *opened = service_new("IFile", file, f);\n'
               '        opened->on_close = host_file_close;  // MK8 (toolchain/save_file_close.py)\n'
               '        ipc_return_object(resp, opened);\n')
DIR = '        ipc_return_object(resp, service_new("IDirectory", directory, hd));\n'
DIR_CLOSED = ('        Service *opened = service_new("IDirectory", directory, hd);\n'
              '        opened->on_close = host_dir_close;  // MK8 (toolchain/save_file_close.py)\n'
              '        ipc_return_object(resp, opened);\n')


def prepare(runtime):
    fs = runtime/'services/fs.c'
    text = fs.read_text(encoding='utf-8')
    if text.count(ANCHOR) != 1 or text.count(FILE) != 1 or text.count(DIR) != 1 or 'on_close' in text:
        raise ValueError('Filesystem object boundary changed upstream')
    fs.write_text(text.replace(ANCHOR, CLOSERS).replace(FILE, FILE_CLOSED).replace(DIR, DIR_CLOSED), encoding='utf-8')
