"""Adapt the pinned runtime's virtual LAN for MK8's network requests and guest scheduling."""


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'LAN adaptation boundary changed: {old[:80]}')
    return text.replace(old, new)


def prepare(runtime):
    platform = runtime/'platform/windows.c'
    text = platform.read_text(encoding='utf-8')
    text = replace(text, '''    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);''', '''    CloseHandle(pi.hThread);
    DWORD exit_code = 0;
    if (!argv) {
        // Address-reservation retry happens before boot. Keep the launcher's child alive
        // until the actual game exits, so it cannot close the room or reopen the same save.
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (!GetExitCodeProcess(pi.hProcess, &exit_code)) exit_code = 1;
    }
    CloseHandle(pi.hProcess);
    ExitProcess(exit_code);''')
    platform.write_text(text, encoding='utf-8')
    nifm = runtime/'services/nifm.c'
    text = nifm.read_text(encoding='utf-8')
    # A configured virtual LAN is a usable local interface. Do not enable the game's Internet backend.
    text = replace(text, 'if (net_online() && req->cmd != 2)',
                   'if ((net_online() || vlan_active()) && req->cmd != 2)')
    text = replace(text, 'if (net_online()) switch (req->cmd)',
                   'if (net_online() || vlan_active()) switch (req->cmd)')
    nifm.write_text(text, encoding='utf-8')

    bsd = runtime/'services/bsd.c'
    text = bsd.read_text(encoding='utf-8')
    text = replace(text, 's->nonblock ? 4 : 0', '(s->nonblock ? 0x800 : 0) | 2')
    text = replace(text, '(fl & 4) != 0', '(fl & (0x800 | 4)) != 0')
    boundary = '        int n = vlan_send(sport, ip, port, d ? (const void *)(uintptr_t)d->addr : "", d ? d->size : 0);'
    text = replace(text, boundary, '''        if (getenv("MK8_TRACE_LAN"))
            LOG("[lan-send] fd=%d src=%u dst=%08x:%u bytes=%llu\\n", fd, sport, ip, port,
                (unsigned long long)(d ? d->size : 0));
''' + boundary)
    text = replace(text, 'static void client(Service *svc, IpcReq *req, IpcResp *resp) {',
                   'static void lan_client(Service *svc, IpcReq *req, IpcResp *resp) {')
    boundary = 'SERVICE("bsd:u", bsd_u)'
    wrapper = '''// Waiting for LAN packets must not prevent the game's other threads from running.
// Release/reacquire the virtual core outside the socket mutex to preserve lock ordering.
static void client(Service *svc, IpcReq *req, IpcResp *resp) {
    bool wait = vlan_active() && (req->cmd == 5 || req->cmd == 6 || req->cmd == 8 ||
                                 req->cmd == 9 || req->cmd == 25);
    if (wait) kernel_host_wait_begin();
    lan_client(svc, req, resp);
    if (wait) kernel_host_wait_end();
    if (vlan_active() && getenv("MK8_TRACE_LAN") && req->cmd >= 2 && req->cmd != 5 && req->cmd != 6) {
        IpcBuf *data = in_buf(req, 0);
        LOG("[lan-call] cmd=%u fd=%d arg=%u bytes=%llu ret=%d errno=%u\\n", req->cmd,
            req->data_size >= 4 ? IN(int32_t, 0) : -1,
            req->data_size >= 8 ? IN(uint32_t, 4) : 0,
            (unsigned long long)(data ? data->size : 0), OUT(int32_t, 0), OUT(uint32_t, 4));
    }
}

'''
    text = replace(text, boundary, wrapper + boundary)
    bsd.write_text(text, encoding='utf-8')
