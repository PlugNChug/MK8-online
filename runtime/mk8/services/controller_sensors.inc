// SDL's event thread owns the devices and pending readings. The renderer reads one complete pair at a time.
typedef struct {
    uint64_t generation, stamp, received;
    float accel[3], gyro[3];
} Mk8MotionPair;

typedef struct {
    SDL_JoystickID id;
    bool enabled, device_clock;
    uint64_t assigned_at, last_received, stamp[2];
    float pending[2][3];
    bool fresh[2];
    Mk8MotionPair pair;
} Mk8SensorPad;

static Mk8SensorPad mk8_sensors[1 + EXTRA_PADS];
static SDL_SpinLock mk8_sensor_lock;
static bool mk8_sensor_focused = true;

static void mk8_reset_sensor_pair(Mk8SensorPad *slot) {
    slot->last_received = 0;
    memset(slot->stamp, 0, sizeof slot->stamp);
    memset(slot->fresh, 0, sizeof slot->fresh);
    SDL_LockSpinlock(&mk8_sensor_lock);
    ++slot->pair.generation;
    slot->pair.received = 0;
    SDL_UnlockSpinlock(&mk8_sensor_lock);
}

// Called after the normal player assignment changes and when the game window loses or regains focus.
static void mk8_sync_sensors(void) {
    bool listen = mk8_sensor_focused && !input_user_blocked(NULL);
    for (unsigned i = 0; i < SDL_arraysize(mk8_sensors); ++i) {
        Mk8SensorPad *slot = &mk8_sensors[i];
        SDL_Gamepad *device = i ? extra_pad[i - 1] : pad;
        SDL_JoystickID id = device ? SDL_GetGamepadID(device) : 0;
        SDL_JoystickID wanted = listen ? id : 0;
        if (slot->id == wanted) continue;
        if (slot->enabled && slot->id == id) {
            SDL_SetGamepadSensorEnabled(device, SDL_SENSOR_ACCEL, false);
            SDL_SetGamepadSensorEnabled(device, SDL_SENSOR_GYRO, false);
        }
        slot->enabled = false;
        slot->id = wanted;
        slot->assigned_at = SDL_GetTicksNS();
        mk8_reset_sensor_pair(slot);
        if (!wanted || !SDL_GamepadHasSensor(device, SDL_SENSOR_ACCEL) || !SDL_GamepadHasSensor(device, SDL_SENSOR_GYRO)) continue;
        if (!SDL_SetGamepadSensorEnabled(device, SDL_SENSOR_ACCEL, true)) continue;
        slot->enabled = SDL_SetGamepadSensorEnabled(device, SDL_SENSOR_GYRO, true);
        if (!slot->enabled) SDL_SetGamepadSensorEnabled(device, SDL_SENSOR_ACCEL, false);
    }
}

static void mk8_sensor_event(const SDL_GamepadSensorEvent *e) {
    if (e->sensor != SDL_SENSOR_ACCEL && e->sensor != SDL_SENSOR_GYRO) return;
    for (unsigned i = 0; i < SDL_arraysize(mk8_sensors); ++i) {
        Mk8SensorPad *slot = &mk8_sensors[i];
        if (!slot->enabled || slot->id != e->which) continue;
        // Reject events queued before reassignment/focus and samples that cannot represent physical motion.
        if (e->timestamp < slot->assigned_at || e->timestamp < slot->last_received) return;
        for (unsigned axis = 0; axis < 3; ++axis)
            if (!isfinite(e->data[axis])) return;
        bool device_clock = e->sensor_timestamp != 0;
        if (slot->last_received && (slot->device_clock != device_clock ||
            e->timestamp - slot->last_received > SDL_MS_TO_NS(250))) mk8_reset_sensor_pair(slot);
        slot->last_received = e->timestamp;
        slot->device_clock = device_clock;
        unsigned k = e->sensor == SDL_SENSOR_GYRO;
        uint64_t stamp = device_clock ? e->sensor_timestamp : e->timestamp;
        if (stamp <= slot->stamp[k]) return;
        slot->stamp[k] = stamp;
        memcpy(slot->pending[k], e->data, sizeof slot->pending[k]);
        slot->fresh[k] = true;
        if (!slot->fresh[1 - k]) return;
        uint64_t other = slot->stamp[1 - k];
        if ((stamp > other ? stamp - other : other - stamp) > SDL_MS_TO_NS(20)) return;
        slot->fresh[0] = slot->fresh[1] = false;
        SDL_LockSpinlock(&mk8_sensor_lock);
        slot->pair.stamp = slot->stamp[1];
        slot->pair.received = e->timestamp;
        memcpy(slot->pair.accel, slot->pending[0], sizeof slot->pair.accel);
        memcpy(slot->pair.gyro, slot->pending[1], sizeof slot->pair.gyro);
        SDL_UnlockSpinlock(&mk8_sensor_lock);
        return;
    }
}

// Timestamps share a clock only within one stream generation; SDL's timestamps need not track wall time.
bool mk8_motion_read(unsigned player, uint64_t *generation, uint64_t *timestamp_ns, float accel[3], float gyro[3]) {
    extern bool menu_input_captured(void), game_overlay_captured(void);
    if (player >= SDL_arraysize(mk8_sensors) || menu_input_captured() || mk8_settings_page_active() ||
        game_overlay_captured() || padscript_active()) return false;
    SDL_LockSpinlock(&mk8_sensor_lock);
    Mk8MotionPair pair = mk8_sensors[player].pair;
    SDL_UnlockSpinlock(&mk8_sensor_lock);
    if (!pair.received || SDL_GetTicksNS() - pair.received > SDL_MS_TO_NS(250)) return false;
    *generation = pair.generation;
    *timestamp_ns = pair.stamp;
    memcpy(accel, pair.accel, sizeof pair.accel);
    memcpy(gyro, pair.gyro, sizeof pair.gyro);
    return true;
}
