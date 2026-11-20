// MK8 asks the SDK for the latest 16 states of each started FullKey sensor.
#include "mk8_motion_pose.h"

enum { MK8_MOTION_HISTORY = 16 };
typedef struct {
    bool started;
    unsigned count;
    uint64_t sequence;
    Mk8MotionState samples[MK8_MOTION_HISTORY];  // newest first
} Mk8MotionHistory;

static Mk8MotionHistory mk8_motion_history[1 + EXTRA_PADS];
static pthread_mutex_t mk8_motion_lock = PTHREAD_MUTEX_INITIALIZER;

static int mk8_motion_slot(uint32_t handle) {
    unsigned id = (handle >> 8) & 0xff;
    // The SDK encodes FullKey style 3 and the single motion device index 2.
    return (handle & 0xffff00ffu) == 0x00020003u && id <= EXTRA_PADS ? (int)id : -1;
}

uint32_t mk8_motion_control(const uint8_t *data, size_t size, bool start) {
    uint32_t handle;
    if (!data || size < 16) return MAKE_RESULT(11, 420);  // InvalidCmifRequest
    memcpy(&handle, data, sizeof handle);
    int slot = mk8_motion_slot(handle);
    if (slot < 0) return MAKE_RESULT(11, 420);

    pthread_mutex_lock(&mk8_motion_lock);
    Mk8MotionHistory *history = &mk8_motion_history[slot];
    if (history->started != start) {
        history->started = start;
        history->count = 0;
        mk8_motion_pose_reset(slot);
    }
    pthread_mutex_unlock(&mk8_motion_lock);
    return 0;
}

RT_ON_RESET(mk8_motion_guest_reset) {
    memset(mk8_motion_history, 0, sizeof mk8_motion_history);
    for (unsigned i = 0; i <= EXTRA_PADS; ++i) mk8_motion_pose_reset(i);
}

HLE_FUNC("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",
         mk8_get_motion_states) {
    uint8_t *out = (void *)(uintptr_t)c->x[0];
    int32_t capacity = (int32_t)c->x[1];
    const void *handle_address = (const void *)(uintptr_t)c->x[2];
    c->x[0] = 0;
    if (!out || !handle_address || capacity <= 0) return;
    uint32_t handle;
    memcpy(&handle, handle_address, sizeof handle);
    int slot = mk8_motion_slot(handle);
    if (slot < 0) {
        memset(out, 0, sizeof(Mk8MotionState));
        return;
    }

    pthread_mutex_lock(&mk8_motion_lock);
    Mk8MotionHistory *history = &mk8_motion_history[slot];
    Mk8MotionState next;
    int status = history->started ? mk8_motion_state(slot, &next) : -1;
    if (status < 0) {
        history->count = 0;
        // MK8 checks the newest sampling number, then uses acceleration, angular
        // velocity and the vertical direction components even if attributes=0.
        // A zero sampling number lets the game keep ordinary stick steering.
        memset(out, 0, sizeof next);
    } else {
        if (status > 0) {
            if (!next.delta_time_ns) history->count = 0;
            if (history->count < MK8_MOTION_HISTORY) history->count++;
            memmove(history->samples + 1, history->samples,
                    (history->count - 1) * sizeof next);
            next.sampling_number = ++history->sequence;
            history->samples[0] = next;
        }
        unsigned n = history->count;
        if (n > (unsigned)capacity) n = (unsigned)capacity;
        memcpy(out, history->samples, n * sizeof next);
        c->x[0] = n;
    }
    pthread_mutex_unlock(&mk8_motion_lock);
}
