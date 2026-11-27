#define COBJMACROS
#include <windows.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shldisp.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include "steam_motion_windows.h"

typedef struct {
    ULONGLONG heartbeat, received;
    DWORD reader, generation, sequence;
    float accel[3], gyro[3];
} MotionMailbox;

typedef struct {
    HANDLE mapping, lock;
    MotionMailbox *data;
} MotionChannel;

static MotionChannel channel;
static WCHAR channel_id[39];
static SDL_JoystickID target, active;
static Uint64 next_scan, next_start, last_source;
static DWORD last_sequence;

static void close_channel(MotionChannel *pipe) {
    if (pipe->data) UnmapViewOfFile(pipe->data);
    if (pipe->lock) CloseHandle(pipe->lock);
    if (pipe->mapping) CloseHandle(pipe->mapping);
    *pipe = (MotionChannel){0};
}

static bool open_channel(MotionChannel *pipe, const WCHAR *id, bool create) {
    WCHAR mapping[80], lock[80];
    swprintf(mapping, SDL_arraysize(mapping), L"Local\\MK8.SteamMotion.%ls", id);
    swprintf(lock, SDL_arraysize(lock), L"Local\\MK8.SteamMotion.%ls.lock", id);
    pipe->mapping = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                              sizeof(MotionMailbox), mapping)
                           : OpenFileMappingW(FILE_MAP_WRITE, FALSE, mapping);
    if (!pipe->mapping || (create && GetLastError() == ERROR_ALREADY_EXISTS)) goto failed;
    pipe->lock = create ? CreateMutexW(NULL, FALSE, lock)
                        : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, lock);
    if (!pipe->lock || (create && GetLastError() == ERROR_ALREADY_EXISTS)) goto failed;
    pipe->data = MapViewOfFile(pipe->mapping, FILE_MAP_WRITE, 0, 0, sizeof(MotionMailbox));
    if (pipe->data) return true;
failed:
    close_channel(pipe);
    return false;
}

static bool lock_channel(const MotionChannel *pipe, DWORD timeout) {
    DWORD result = WaitForSingleObject(pipe->lock, timeout);
    if (result == WAIT_ABANDONED) {
        // A writer may have exited in the middle of copying a sample.
        *pipe->data = (MotionMailbox){0};
        return true;
    }
    return result == WAIT_OBJECT_0;
}

// Ask the desktop shell to launch the reader in its own process tree. A game child would
// inherit Steam's gamepad redirection and lose access to the physical controller again.
static HRESULT desktop_shell(IShellDispatch2 **result) {
    IShellWindows *windows = NULL;
    IDispatch *desktop = NULL, *background = NULL, *application = NULL;
    IServiceProvider *provider = NULL;
    IShellBrowser *browser = NULL;
    IShellView *view = NULL;
    IShellFolderViewDual *folder = NULL;
    VARIANT location = {.vt = VT_I4, .lVal = CSIDL_DESKTOP}, empty = {0};
    LONG window = 0;
    HRESULT hr = CoCreateInstance(&CLSID_ShellWindows, NULL, CLSCTX_LOCAL_SERVER, &IID_IShellWindows, (void **)&windows);
    if (SUCCEEDED(hr))
        hr = IShellWindows_FindWindowSW(windows, &location, &empty, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, &desktop);
    if (hr == S_OK) hr = IDispatch_QueryInterface(desktop, &IID_IServiceProvider, (void **)&provider);
    if (hr == S_OK) hr = IServiceProvider_QueryService(provider, &SID_STopLevelBrowser, &IID_IShellBrowser, (void **)&browser);
    if (hr == S_OK) hr = IShellBrowser_QueryActiveShellView(browser, &view);
    if (hr == S_OK) hr = IShellView_GetItemObject(view, SVGIO_BACKGROUND, &IID_IDispatch, (void **)&background);
    if (hr == S_OK) hr = IDispatch_QueryInterface(background, &IID_IShellFolderViewDual, (void **)&folder);
    if (hr == S_OK) hr = IShellFolderViewDual_get_Application(folder, &application);
    if (hr == S_OK) hr = IDispatch_QueryInterface(application, &IID_IShellDispatch2, (void **)result);
    if (application) IDispatch_Release(application);
    if (folder) IShellFolderViewDual_Release(folder);
    if (background) IDispatch_Release(background);
    if (view) IShellView_Release(view);
    if (browser) IShellBrowser_Release(browser);
    if (provider) IServiceProvider_Release(provider);
    if (desktop) IDispatch_Release(desktop);
    if (windows) IShellWindows_Release(windows);
    return hr;
}

static int SDLCALL launch_reader(void *argument) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr)) {
        IShellDispatch2 *shell = NULL;
        hr = desktop_shell(&shell);
        if (hr == S_OK) {
            WCHAR path[32768];
            DWORD length = GetModuleFileNameW(NULL, path, SDL_arraysize(path));
            BSTR executable = length && length < SDL_arraysize(path) ? SysAllocString(path) : NULL;
            VARIANT args = {.vt = VT_BSTR, .bstrVal = SysAllocString(argument)};
            VARIANT empty = {0}, hidden = {.vt = VT_I4, .lVal = SW_HIDE};
            hr = executable && args.bstrVal ? IShellDispatch2_ShellExecute(shell, executable, args, empty, empty, hidden)
                                            : E_FAIL;
            SysFreeString(args.bstrVal);
            SysFreeString(executable);
            IShellDispatch2_Release(shell);
        }
        CoUninitialize();
    }
    if (hr != S_OK) SDL_Log("[input] Could not start the Steam motion reader (0x%08lx)", (unsigned long)hr);
    SDL_free(argument);
    return hr != S_OK;
}

static void start_reader(void) {
    WCHAR *args = SDL_malloc(96 * sizeof(*args));
    if (!args) return;
    swprintf(args, 96, L"--steam-motion-reader %lu %ls", (unsigned long)GetCurrentProcessId(), channel_id);
    SDL_Thread *thread = SDL_CreateThread(launch_reader, "Steam motion startup", args);
    if (thread) SDL_DetachThread(thread);
    else SDL_free(args);
}

static SDL_JoystickID find_controller(bool steam) {
    SDL_JoystickID found = 0;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        Uint16 vendor = 0, product = 0;
        SDL_GetJoystickGUIDInfo(SDL_GetGamepadGUIDForID(ids[i]), &vendor, &product, NULL, NULL);
        if (steam ? vendor != 0x28de || product != 0x11ff : vendor != 0x057e || product != 0x2009) continue;
        if (steam) {
            SDL_Gamepad *mapped = SDL_OpenGamepad(ids[i]);
            if (!mapped) continue;
            bool pro = SDL_GetGamepadSteamHandle(mapped) &&
                       SDL_GetGamepadVendor(mapped) == 0x057e && SDL_GetGamepadProduct(mapped) == 0x2009;
            bool native_motion = SDL_GamepadHasSensor(mapped, SDL_SENSOR_ACCEL) && SDL_GamepadHasSensor(mapped, SDL_SENSOR_GYRO);
            SDL_CloseGamepad(mapped);
            if (!pro || native_motion) continue;
        }
        if (found) { found = 0; break; } // The two processes can associate a single Pro Controller.
        found = ids[i];
    }
    SDL_free(ids);
    return found;
}

SDL_JoystickID mk8_poll_steam_motion(void) {
    Uint64 now = GetTickCount64();
    if (now >= next_scan || (target && !SDL_IsGamepad(target))) {
        target = find_controller(true);
        next_scan = now + 250;
    }
    if (!target) goto lost;
    if (!channel.data) {
        if (now < next_start) return target;
        GUID id;
        if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(&id, channel_id, SDL_arraysize(channel_id)) ||
            !open_channel(&channel, channel_id, true)) {
            next_start = now + 10000;
            return target;
        }
    }
    if (!lock_channel(&channel, 0)) return target;
    MotionMailbox sample = *channel.data;
    ReleaseMutex(channel.lock);
    now = GetTickCount64();
    if (now >= next_start && (!sample.heartbeat || now - sample.heartbeat > 4000)) {
        next_start = now + 10000;
        start_reader();
    }
    if (!sample.received || now - sample.received > 150) goto lost;
    Uint64 source = ((Uint64)sample.reader << 32) | sample.generation;
    if (target == active && source == last_source && sample.sequence == last_sequence) return target;
    if (active && active != target) mk8_motion_steam_lost(active);
    if (active != target) SDL_Log("[input] Steam motion reader connected");
    active = target;
    last_source = source;
    last_sequence = sample.sequence;
    mk8_motion_steam_sample(active, source, sample.accel, sample.gyro);
    return target;
lost:
    if (active) mk8_motion_steam_lost(active);
    active = 0;
    return target;
}

void mk8_stop_steam_motion(void) {
    close_channel(&channel);
}

static bool reader_alive(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

static bool claim_reader(const MotionChannel *pipe) {
    if (!lock_channel(pipe, 250)) return false;
    bool claimed = !pipe->data->reader || !reader_alive(pipe->data->reader);
    if (claimed) {
        *pipe->data = (MotionMailbox){.reader = GetCurrentProcessId(), .heartbeat = GetTickCount64()};
    }
    ReleaseMutex(pipe->lock);
    return claimed;
}

static void read_controller(const MotionChannel *pipe, HANDLE owner) {
    SDL_Gamepad *pad = NULL;
    SDL_JoystickID id = 0;
    Uint64 next_scan = 0, stamps[2] = {0};
    float pending[2][3];
    MotionMailbox sample = {.reader = GetCurrentProcessId()};
    Uint64 published = 0;
    while (WaitForSingleObject(owner, 0) == WAIT_TIMEOUT) {
        Uint64 now = GetTickCount64();
        bool changed = false;
        if (now >= next_scan || (pad && !SDL_GamepadConnected(pad))) {
            next_scan = now + 250;
            SDL_JoystickID found = pad && !SDL_GamepadConnected(pad) ? 0 : find_controller(false);
            if (found != id) {
                if (pad) SDL_CloseGamepad(pad);
                pad = found ? SDL_OpenGamepad(found) : NULL;
                if (pad && (!SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true) ||
                            !SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true))) {
                    SDL_CloseGamepad(pad);
                    pad = NULL;
                }
                id = pad ? found : 0;
                stamps[0] = stamps[1] = 0;
                sample.received = 0;
                ++sample.generation;
                changed = true;
            }
        }
        SDL_Event event;
        for (unsigned i = 0; i < 128 && SDL_PollEvent(&event); ++i) {
            if (!id || event.type != SDL_EVENT_GAMEPAD_SENSOR_UPDATE || event.gsensor.which != id) continue;
            bool gyro = event.gsensor.sensor == SDL_SENSOR_GYRO;
            if (!gyro && event.gsensor.sensor != SDL_SENSOR_ACCEL) continue;
            stamps[gyro] = event.gsensor.sensor_timestamp ? event.gsensor.sensor_timestamp : event.gsensor.timestamp;
            SDL_memcpy(pending[gyro], event.gsensor.data, sizeof pending[gyro]);
        }
        Uint64 delta = stamps[0] > stamps[1] ? stamps[0] - stamps[1] : stamps[1] - stamps[0];
        bool paired = stamps[0] && stamps[1] && delta <= SDL_MS_TO_NS(20);
        if ((paired || changed || now - published >= 1000) && lock_channel(pipe, 0)) {
            sample.heartbeat = now;
            if (paired) {
                sample.received = now;
                ++sample.sequence;
                SDL_memcpy(sample.accel, pending[0], sizeof sample.accel);
                SDL_memcpy(sample.gyro, pending[1], sizeof sample.gyro);
                stamps[0] = stamps[1] = 0;
            }
            *pipe->data = sample;
            ReleaseMutex(pipe->lock);
            published = now;
        }
        SDL_Delay(4);
    }
    if (pad) SDL_CloseGamepad(pad);
}

int mk8_run_steam_motion_reader(const char *owner, const char *session) {
    char *end = NULL;
    unsigned long long pid = strtoull(owner, &end, 10);
    if (!pid || pid > UINT32_MAX || *end || SDL_strlen(session) != 38 || *session != '{') return 1;
    WCHAR id[39];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, session, -1, id, SDL_arraysize(id))) return 1;
    GUID guid;
    if (FAILED(CLSIDFromString(id, &guid))) return 1;
    StringFromGUID2(&guid, id, SDL_arraysize(id));
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!process) return 1;
    MotionChannel pipe = {0};
    int result = 1;
    if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && open_channel(&pipe, id, false) && claim_reader(&pipe)) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (SDL_Init(SDL_INIT_GAMEPAD)) {
            read_controller(&pipe, process);
            SDL_Quit();
            result = 0;
        }
    }
    close_channel(&pipe);
    CloseHandle(process);
    return result;
}
