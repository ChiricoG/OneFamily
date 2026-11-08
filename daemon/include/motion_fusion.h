/*
 * Orientation quaternion fusion for USB controllers (DualShock 4 mode, Switch Pro).
 *
 * Complementary filter with the same math as extended_sample() in bt_driver.c
 * (DualSense Bluetooth path). The Bluetooth path keeps its own copy on purpose:
 * it is covered by the anti-regression rules and must stay untouched.
 *
 * Inputs are expressed in the PlayStation frame: gyro in rad/s, accel in g.
 * Header-only (static inline) so no build script change is needed.
 */

#ifndef MOTION_FUSION_H
#define MOTION_FUSION_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float    q[4];        /* x, y, z, w */
    uint64_t last_us;     /* timestamp of the previous update, 0 = none yet */
} MotionFusionState;

static inline float motion_fusion_sqrt(float value) {
    if (value <= 0.0f) return 0.0f;
    float result = value > 1.0f ? value : 1.0f;
    for (unsigned i = 0; i < 12; ++i) {
        result = (result + value / result) * 0.5f;
    }
    return result;
}

static inline void motion_fusion_reset(MotionFusionState* s) {
    s->q[0] = 0.0f;
    s->q[1] = 0.0f;
    s->q[2] = 0.0f;
    s->q[3] = 1.0f;
    s->last_us = 0;
}

/*
 * Integrate one sample. The first call after a reset only records the
 * timestamp. Samples more than 100 ms apart are not integrated.
 */
static inline void motion_fusion_update(MotionFusionState* s,
                                        const float gyro[3], const float accel[3],
                                        uint64_t now_us) {
    float dt = 0.0f;
    if (s->last_us != 0 && now_us > s->last_us) {
        dt = (float)(now_us - s->last_us) * 0.000001f;
    }
    s->last_us = now_us;
    if (dt <= 0.0f || dt > 0.1f) {
        return;
    }

    float omega[3] = { gyro[0], gyro[1], gyro[2] };
    float x = s->q[0];
    float y = s->q[1];
    float z = s->q[2];
    float w = s->q[3];

    float magnitude = motion_fusion_sqrt(accel[0] * accel[0] +
                                         accel[1] * accel[1] +
                                         accel[2] * accel[2]);
    if (magnitude > 0.8f && magnitude < 1.2f) {
        float ax = accel[0] / magnitude;
        float ay = accel[1] / magnitude;
        float az = accel[2] / magnitude;
        float gx = 2.0f * (x * y + w * z);
        float gy = 1.0f - 2.0f * (x * x + z * z);
        float gz = 2.0f * (y * z - w * x);
        omega[0] += 2.0f * (ay * gz - az * gy);
        omega[1] += 2.0f * (az * gx - ax * gz);
        omega[2] += 2.0f * (ax * gy - ay * gx);
    }

    float half = dt * 0.5f;
    s->q[0] += ( w * omega[0] + y * omega[2] - z * omega[1]) * half;
    s->q[1] += ( w * omega[1] + z * omega[0] - x * omega[2]) * half;
    s->q[2] += ( w * omega[2] + x * omega[1] - y * omega[0]) * half;
    s->q[3] += (-x * omega[0] - y * omega[1] - z * omega[2]) * half;

    float norm = motion_fusion_sqrt(s->q[0] * s->q[0] + s->q[1] * s->q[1] +
                                    s->q[2] * s->q[2] + s->q[3] * s->q[3]);
    if (norm > 0.0001f) {
        s->q[0] /= norm;
        s->q[1] /= norm;
        s->q[2] /= norm;
        s->q[3] /= norm;
    }
}

#endif /* MOTION_FUSION_H */
