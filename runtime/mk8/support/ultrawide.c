// shortcut: solo Time Trial, Grand Prix and no-team VS only; validate other cameras before widening them.
// The verified guest hook runs under the render-manager mutex.
#include "rt.h"
#include "settings.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#endif

static uint64_t changed_projection;
static float changed_aspect;
bool mk8_ultrawide_hud_ready(bool intro);
bool mk8_ultrawide_hud_handoff_ready(void);
bool mk8_settings_page_active(void);
static bool enabled;
// Zero preserves normal settings; each positive token is the aspect of that produced image.
static _Atomic double drawable, producer, presented;
#ifdef _WIN32
static DWORD page_size;
#endif

__attribute__((constructor)) static void mk8_ultrawide_init(void) {
    const char *value = getenv("MK8_ULTRAWIDE_TEST");
    enabled = value && !strcmp(value, "1");
    drawable = 16.0/9.0;
    producer = presented = enabled ? 16.0/9.0 : 0;
#ifdef _WIN32
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    page_size = system.dwPageSize;
#endif
}

void mk8_ultrawide_drawable(int width, int height) {
    double ratio = height > 0 ? (double)width/height : 0;
    atomic_store(&drawable, ratio >= 16.0/9.0 && ratio <= 32.0/9.0 ? ratio : 16.0/9.0);
}

double mk8_ultrawide_aspect(void) { return atomic_load(&producer); }
double mk8_ultrawide_take_present(void) {
    // Settings load after constructors; keep any positive decision made before a live preference change.
    double fallback = enabled || g_settings.aspect == ASPECT_ULTRAWIDE ? 16.0/9.0 : 0;
    double decision = atomic_exchange(&producer, fallback);
    return decision > 0 ? decision : fallback;
}
void mk8_ultrawide_present(double decision) { atomic_store(&presented, decision); }
double mk8_ultrawide_present_mode(void) { return atomic_load(&presented); }

uint8_t *mk8_ultrawide_memory(uint64_t address, size_t size, bool write) {
    // Camera list nodes observed in 4.0.1 are only four-byte aligned; reads below use memcpy.
    if ((address & 3) || address < GUEST_LO || address >= GUEST_HI || size > GUEST_HI - address) return NULL;
#ifdef _WIN32
    uint64_t end = address + size, at = address;
    DWORD allowed = write ? PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY :
        PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    // VirtualQuery scans multi-gigabyte guest regions; resident pages expose their current protection directly.
    if (size && page_size) {
        uint64_t first = address - address % page_size;
        uint64_t count = (end - 1 - first)/page_size + 1;
        if (count <= 2) {
            PSAPI_WORKING_SET_EX_INFORMATION pages[2] = {0};
            for (unsigned i = 0; i < count; i++)
                pages[i].VirtualAddress = (void *)(uintptr_t)(first + i*page_size);
            bool known = QueryWorkingSetEx(GetCurrentProcess(), pages, (DWORD)(count*sizeof *pages));
            for (unsigned i = 0; known && i < count; i++) known = pages[i].VirtualAttributes.Valid;
            if (known) {
                for (unsigned i = 0; i < count; i++) {
                    DWORD protection = pages[i].VirtualAttributes.Win32Protection;
                    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) || !(protection & allowed)) return NULL;
                }
                return (uint8_t *)(uintptr_t)address;
            }
        }
    }
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)(uintptr_t)at, &info, sizeof info) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return NULL;
        if (!(info.Protect & allowed)) return NULL;
        uint64_t next = (uint64_t)(uintptr_t)info.BaseAddress + info.RegionSize;
        if (next <= at) return NULL;
        at = next;
    }
    return (uint8_t *)(uintptr_t)address;
#else
    (void)write;
    return NULL;
#endif
}

static uint64_t u64(const uint8_t *p, size_t offset) {
    uint64_t value;
    memcpy(&value, p + offset, sizeof value);
    return value;
}

static uint32_t u32(const uint8_t *p, size_t offset) {
    uint32_t value;
    memcpy(&value, p + offset, sizeof value);
    return value;
}

static float f32(const uint8_t *p, size_t offset) {
    float value;
    memcpy(&value, p + offset, sizeof value);
    return value;
}

static bool valid_depth(float near_clip, float far_clip) {
    // Course cameras choose their clip planes; reject degenerate float projection math.
    if (!isnormal(near_clip) || near_clip <= 0.f || !isfinite(far_clip) || far_clip <= near_clip) return false;
    float inverse = 1.f/(far_clip - near_clip);
    float depth = -inverse*(far_clip + near_clip), offset = -inverse*((far_clip + far_clip)*near_clip);
    return isfinite(inverse) && inverse > 0.f && isfinite(depth) && isfinite(offset) && offset < 0.f;
}

static uint8_t *object(const uint8_t *p, size_t offset, size_t size) {
    return mk8_ultrawide_memory(u64(p, offset), size, false);
}

static bool solo_camera(const uint8_t *root, const uint8_t *context, const uint8_t *manager) {
    uint8_t *node = object(root, 0x20, 0x10);
    uint8_t *scene = node ? object(node, 8, 0x248) : NULL;
    if (!scene || u32(scene, 0x180) != 4 || u32(scene, 0x184) != 4 || u32(scene, 0x17c) != 3 ||
        scene[0x234] || u32(scene, 0x238) || u32(scene, 0x240)) return false;
    // The persistent root's rules and SystemEngine's viewport configuration are distinct objects.
    uint8_t *config = object(context, 0x240, 0x18);
    uint8_t *rules = config ? object(config, 0x10, 0x1a0) : NULL;
    uint8_t *system = object(context, 0x190, 0x800);
    uint8_t *view = system ? object(system, 0x7f8, 0xe0) : NULL;
    uint32_t kind = rules ? u32(rules, 8) : UINT32_MAX;
    if (!rules || (kind != 0 && kind != 1 && kind != 2) || (kind == 1 && rules[0x27]) ||
        u32(rules, 0x188) != 1 || !view ||
        u32(view, 0xcc) != 0 || u32(view, 0xb0) != 0 ||
        u32(view, 0x40) != 1 || u32(view, 0x4c) != 0) return false;
    for (unsigned i = 1; i < 4; i++)
        if (u32(view, 0xb0 + 4*i) != UINT32_MAX || u32(view, 0x40 + 0x18*i)) return false;
    if (u32(view, 0xa0)) return false;
    uint8_t *engine = object(scene, 0x1c0, 0x200);
    uint8_t *controller = engine ? object(engine, 0x1f8, 0xb00) : NULL;
    if (!controller || u64(controller, 0) != MAIN_BASE + 0x11b88e0 ||
        u64(controller, 0x60) != (uint64_t)(uintptr_t)manager + 0x958 ||
        u64(controller, 0x68) != (uint64_t)(uintptr_t)manager + 0x978 ||
        u32(controller, 0x80) < 1 || u32(controller, 0x80) > 16) return false;
    uint64_t tail = u64(controller, 0x78);
    uint8_t *selection = tail >= 0x10 ? mk8_ultrawide_memory(tail - 0x10, 0x20, false) : NULL;
    if (!selection) return false;
    uint64_t selected = u64(selection, 0);
    uint8_t *external = mk8_ultrawide_memory(selected, 0xc, false);
    if (external && u64(external, 0) == MAIN_BASE + 0x12c7778 && u32(external, 8) == 0) {
        // This owner serves other sequences; only the solo GP/VS flyover and close-up are verified.
        uint8_t *race = object(scene, 0x1b0, 0x230);
        uint8_t *registry = race ? object(race, 0x228, 0x48) : NULL;
        if ((kind != 0 && kind != 1) || u32(view, 0x48) != 0 ||
            u32(selection, 8) != 20 || u32(selection, 0xc) != 2 ||
            !registry || u64(registry, 0) != MAIN_BASE + 0x11b8db0 ||
            u32(registry, 0x38) != 4 || u32(registry, 0x3c) != 4) return false;
        uint8_t *owners = object(registry, 0x40, 4*8);
        uint8_t *owner = owners ? object(owners, 0, 0x128) : NULL;
        uint64_t address = (uint64_t)(uintptr_t)owner;
        if (!owner || u64(owner, 0) != MAIN_BASE + 0x11b8d50 || u32(owner, 8) != 0 ||
            selected != address + 0x10 || u64(owner, 0x10) != MAIN_BASE + 0x12c7778 ||
            u32(owner, 0x18) != 0 || u64(owner, 0x20) != address + 0x30 ||
            u64(owner, 0x30) != MAIN_BASE + 0x12afde8 ||
            u64(manager, 0x958) != address + 0x30 || u64(manager, 0x978) != u64(owner, 0x28) ||
            u32(owner, 0xd8) != 9) return false;
        uint32_t mode = u32(owner, 0xe8);
        if ((mode != 2 && mode != 3) || u32(owner, 0xec) != mode) return false;
        bool flyover = mode == 2;
        if (u32(view, 0xd8) != (flyover ? 4u : 3u) ||
            u32(view, 0x44) != (flyover ? 2u : 1u) || manager[0x9d7] != (flyover ? 1 : 0) ||
            (!flyover && !mk8_ultrawide_memory(address, 0x138, false))) return false;
        uint8_t *modes = object(owner, 0xe0, 9*8);
        uint8_t *camera = modes ? object(modes, mode*8, 0x10) : NULL;
        uint8_t *source = object(owner, 0x28, 0xc0);
        return camera && (uint64_t)(uintptr_t)camera == u64(owner, flyover ? 0x120 : 0x130) &&
            u64(camera, 0) == MAIN_BASE + (flyover ? 0x11b8ed8 : 0x12c7e68) && u64(camera, 8) == address &&
            source && u64(source, 0) == MAIN_BASE + 0x12b0050 && f32(source, 0xb0) == 4.f/3.f &&
            (mk8_ultrawide_hud_ready(flyover) ||
             (flyover && u32(view, 0xd4) == 3 && mk8_ultrawide_hud_handoff_ready()));
    }
    uint64_t parameter = u64(controller, 0x3d0);
    if (u32(view, 0xd8) != 3 || parameter < 8 ||
        u64(manager, 0x958) != (uint64_t)(uintptr_t)controller + 0x5b0 ||
        u64(manager, 0x978) != (uint64_t)(uintptr_t)controller + 0x730) return false;
    uint8_t *owner = mk8_ultrawide_memory(parameter - 8, 0xb8, false);
    uint8_t *racer = owner ? object(owner, 0xb0, 0x48) : NULL;
    if (!(owner && u64(owner, 0) == MAIN_BASE + 0x11b9c30 &&
        u64(owner, 8) == MAIN_BASE + 0x11b9c50 && u32(owner, 0x10) == 1 &&
        racer && u32(racer, 0x40) == 0)) return false;
    if (selected == parameter) return mk8_ultrawide_hud_ready(false);
    // Rear view uses another registered parameter inside the same racer-camera owner.
    return selected == parameter + 0x38 && u64(controller, 0x3d8) == selected &&
        u64(controller, 0x3e8) == parameter - 8 + 0xca &&
        mk8_ultrawide_memory(parameter - 8, 0xcb, false) && owner[0xca] == 1 &&
        u64(owner, 0x40) == MAIN_BASE + 0x11b9c50 && u32(owner, 0x48) == 1 &&
        mk8_ultrawide_hud_ready(false);
}

void mk8_ultrawide_camera(uint64_t manager_address) {
    bool requested = g_settings.aspect == ASPECT_ULTRAWIDE || (enabled && g_settings.aspect == ASPECT_UNLOCKED);
    atomic_store(&producer, requested || enabled ? 16.0/9.0 : 0);
    if (!requested && !changed_projection) return;
    double wide = atomic_load(&drawable);
    uint64_t previous = changed_projection;
    float previous_aspect = changed_aspect;
    changed_projection = 0;
    uint8_t *slot = mk8_ultrawide_memory(MAIN_BASE + 0x1358c70, 8, false);
    uint8_t *root = slot ? object(slot, 0, 0x30) : NULL;
    uint8_t *context = root ? object(root, 0x28, 0x248) : NULL;
    uint8_t *engine = context ? object(context, 0x1a0, 0x428) : NULL;
    uint8_t *manager = mk8_ultrawide_memory(manager_address, 0xc78, false);
    if (!engine || !manager || u64(engine, 0x420) != manager_address ||
        u32(manager, 0x120) < 1 || u32(manager, 0x120) > 16 || u32(manager, 0x9c0) != 0) return;
    uint8_t *layers = object(manager, 0x128, u32(manager, 0x120)*8);
    uint8_t *layer = layers ? object(layers, 0, 0x80) : NULL;
    uint64_t projection_address = manager_address + 0x238;
    uint8_t *projection = mk8_ultrawide_memory(projection_address, 0xc0, true);
    if (!layer || !projection || u64(layer, 0x70) != projection_address ||
        u64(projection, 0) != MAIN_BASE + 0x12b0050) return;
    float x0 = f32(layer, 0x30), y0 = f32(layer, 0x34), x1 = f32(layer, 0x38), y1 = f32(layer, 0x3c);
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) ||
        x0 != 0.f || y0 != 0.f || x1 < 1.f || x1 > 3840.f || y1 < 1.f || y1 > 2160.f) return;
    float baseline = (x1 - x0)/(y1 - y0), aspect = f32(projection, 0xb0);
    bool eligible = requested && !mk8_settings_page_active() && wide > 16.0/9.0 && g_settings.frame_gen == FRAMEGEN_OFF &&
        x1 == 1280.f && y1 == 720.f && u32(manager, 0x9bc) == 0 && manager[0x9d4] == 1 &&
        u64(layer, 0x68) == u64(manager, 0x958) &&
        valid_depth(f32(projection, 0x98), f32(projection, 0x9c)) &&
        isfinite(f32(projection, 0xa0)) && f32(projection, 0xa0) >= 0.5f && f32(projection, 0xa0) <= 1.6f &&
        f32(projection, 0xa4) > 0.f && f32(projection, 0xa4) <= 1.f &&
        f32(projection, 0xa8) > 0.f && f32(projection, 0xa8) <= 1.f &&
        f32(projection, 0xac) > 0.f && f32(projection, 0xac) <= 2.f &&
        f32(projection, 0xb4) == 0.f && f32(projection, 0xb8) == 0.f &&
        (aspect == baseline || (previous == projection_address && aspect == previous_aspect)) &&
        solo_camera(root, context, manager);
    // Only the currently validated owner can be restored; a prior scene's pointer is never dereferenced.
    bool restore = previous == projection_address && aspect == previous_aspect;
    float target = eligible ? (float)wide : baseline;
    if ((eligible || restore) && aspect != target) {
        memcpy(projection + 0xb0, &target, sizeof target);
        projection[8] = 1;
    }
    changed_projection = eligible ? projection_address : 0;
    changed_aspect = eligible ? target : 0.f;
    if (eligible) atomic_store(&producer, wide);
}
