#ifndef MK8_MOTION_POSE_H
#define MK8_MOTION_POSE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

bool mk8_motion_read(unsigned slot, uint64_t *generation, uint64_t *timestamp_ns,
                     float acceleration[3], float gyro[3]);

typedef struct {
    uint64_t delta_time_ns, sampling_number;
    float acceleration[3], angular_velocity[3], angle[3], direction[3][3];
    uint32_t attributes, reserved;
} Mk8MotionState;
_Static_assert(sizeof(Mk8MotionState) == 0x60, "Six-axis state layout changed");

typedef struct {
    bool initialized;
    uint64_t generation, timestamp;
    float q[4], angle[3];
} Mk8MotionPose;

// The HID reader serializes pose updates, including StopSixAxisSensor.
static Mk8MotionPose mk8_motion_poses[4];

static void mk8_motion_pose_reset(unsigned slot) {
    if (slot < 4) mk8_motion_poses[slot] = (Mk8MotionPose){0};
}

static void mk8_motion_direction(const float q[4], float d[3][3]) {
    // Each Npad column gives a controller axis in the reference frame.
    float w = q[0], x = q[1], y = q[2], z = q[3];
    d[0][0] = 1 - 2*(y*y + z*z); d[0][1] = 2*(x*y + w*z); d[0][2] = 2*(x*z - w*y);
    d[1][0] = 2*(x*y - w*z); d[1][1] = 1 - 2*(x*x + z*z); d[1][2] = 2*(y*z + w*x);
    d[2][0] = 2*(x*z + w*y); d[2][1] = 2*(y*z - w*x); d[2][2] = 1 - 2*(x*x + y*y);
}

// Return -1 for no usable device, 0 for a repeat, 1 for a new state.
static int mk8_motion_state(unsigned slot, Mk8MotionState *out) {
    uint64_t generation, stamp;
    float accel[3], gyro[3];
    if (slot >= 4 || !mk8_motion_read(slot, &generation, &stamp, accel, gyro)) {
        mk8_motion_pose_reset(slot);
        return -1;
    }
    Mk8MotionPose *pose = &mk8_motion_poses[slot];
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(accel[i]) || !isfinite(gyro[i])) {
            mk8_motion_pose_reset(slot);
            return -1;
        }
    }
    bool same_stream = pose->initialized && pose->generation == generation;
    if (same_stream && stamp <= pose->timestamp) return 0;
    uint64_t delta = same_stream ? stamp - pose->timestamp : 0;
    // A suspended or replaced device cannot describe rotation during the gap.
    if (delta > 250000000) delta = 0;
    *out = (Mk8MotionState){.delta_time_ns = delta, .attributes = 1};
    const float tau = 6.28318530718f;
    float rate[3] = {gyro[0], -gyro[2], gyro[1]};
    float up[3] = {accel[0] / 9.80665f, -accel[2] / 9.80665f, accel[1] / 9.80665f};
    float gravity = hypotf(up[0], hypotf(up[1], up[2]));
    // Translation is indistinguishable from tilt during strong acceleration.
    bool settled = gravity > 0.8f && gravity < 1.2f;
    for (unsigned i = 0; i < 3; ++i) {
        out->angular_velocity[i] = rate[i] / tau;
        out->acceleration[i] = -up[i];
        if (settled) up[i] /= gravity;
    }
    float *q = pose->q;
    if (!delta) {
        *pose = (Mk8MotionPose){.initialized = true, .generation = generation, .q = {1, 0, 0, 0}};
        // Shortest rotation from the sensed vertical to reference +Z; yaw starts relative.
        if (settled) {
            if (up[2] < -0.999999f) { q[0] = 0; q[1] = 1; }
            else {
                q[0] = sqrtf((1 + up[2]) * 0.5f);
                q[1] = up[1] / (2 * q[0]);
                q[2] = -up[0] / (2 * q[0]);
            }
        }
    } else {
        float dt = (float)delta * 1e-9f;
        for (unsigned i = 0; i < 3; ++i) pose->angle[i] += out->angular_velocity[i] * dt;
        if (settled) {
            mk8_motion_direction(q, out->direction);
            float vertical[3] = {out->direction[0][2], out->direction[1][2], out->direction[2][2]};
            // Gravity corrects pitch and roll drift; it carries no heading information.
            for (unsigned i = 0; i < 3; ++i) {
                unsigned j = (i + 1) % 3, k = (i + 2) % 3;
                rate[i] += 2 * (up[j] * vertical[k] - up[k] * vertical[j]);
            }
        }
        float speed = hypotf(rate[0], hypotf(rate[1], rate[2]));
        if (!isfinite(speed)) {
            mk8_motion_pose_reset(slot);
            return -1;
        }
        if (speed > 0) {
            float half = 0.5f * speed * dt, scale = sinf(half) / speed;
            float c = cosf(half), dx = rate[0]*scale, dy = rate[1]*scale, dz = rate[2]*scale;
            float w = q[0], x = q[1], y = q[2], z = q[3];
            q[0] = w*c - x*dx - y*dy - z*dz;
            q[1] = x*c + w*dx + y*dz - z*dy;
            q[2] = y*c + w*dy + z*dx - x*dz;
            q[3] = z*c + w*dz + x*dy - y*dx;
            float norm = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
            for (unsigned i = 0; i < 4; ++i) q[i] /= norm;
        }
    }
    pose->timestamp = stamp;
    for (unsigned i = 0; i < 3; ++i) out->angle[i] = pose->angle[i];
    mk8_motion_direction(q, out->direction);
    return 1;
}

#endif
