#include "rt.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include "platform.h"
extern char **environ;
#endif

// The launcher supplies its own executable, including when the game outlives its window.
int mk8_manual_run(void) {
#ifdef _WIN32
    wchar_t executable[4096], command[4200];
    DWORD n = GetEnvironmentVariableW(L"MK8_MANUAL_VIEWER", executable, 4096);
    if (!n || n >= 4096) { LOG("[mk8:manual] viewer unavailable\n"); return 0; }
    if (swprintf(command, 4200, L"\"%ls\" --how-to-play", executable) < 0) return 0;
    STARTUPINFOW start = {.cb = sizeof start};
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &start, &process)) {
        LOG("[mk8:manual] cannot open guide: %lu\n", (unsigned long)GetLastError());
        return 0;
    }
    CloseHandle(process.hThread);
    LOG("[mk8:manual] local guide opened\n");
    DWORD wait;
    do { wait = WaitForSingleObject(process.hProcess, 50); } while (wait == WAIT_TIMEOUT && !g_rt_unwind);
    DWORD code = 1;
    bool ok = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code) && code == 0;
    CloseHandle(process.hProcess);
    LOG("[mk8:manual] local guide closed (%s)\n", ok ? "ok" : "interrupted");
    return ok;
#else
    const char *executable = getenv("MK8_MANUAL_VIEWER");
    if (!executable || !*executable) return 0;
    char *const argv[] = {(char *)executable, "--how-to-play", NULL};
    pid_t pid;
    int error = posix_spawn(&pid, executable, NULL, NULL, argv, environ);
    if (error) {
        LOG("[mk8:manual] cannot open guide: %d\n", error);
        return 0;
    }
    int status;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (done < 0 && errno != EINTR) return 0;
        if (g_rt_unwind) return 0;
        plat_sleep_until_ns(plat_now_ns() + 50 * 1000 * 1000);
    }
#endif
}
