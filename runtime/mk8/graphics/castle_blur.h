#ifndef MK8_CASTLE_BLUR_H
#define MK8_CASTLE_BLUR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MK8_BLUR_HORIZONTAL = 1, MK8_BLUR_VERTICAL, MK8_BLUR_REDUCTION };
#define MK8_VANCOUVER_BLOOM_MASK 0x0e1fa9b3719cf15bull

/* Hash the SPH and all instructions of the three known 4.0.1 programs. */
static inline unsigned mk8_blur_program(const uint8_t *blob, size_t size, size_t words) {
    if ((words != 16 && words != 50) || size < 0x80 + words * 8) return 0;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0x30; i < 0x80 + words * 8; i++)
        hash = (hash ^ blob[i]) * 0x100000001b3ull;
    if (hash == 0x586d1caf71a13a16ull) return MK8_BLUR_HORIZONTAL;
    if (hash == 0x7421943955b18154ull) return MK8_BLUR_VERTICAL;
    if (hash == 0xb4006180b30e53c6ull) return MK8_BLUR_REDUCTION;
    return 0;
}

typedef struct {
    uint64_t frame;
    unsigned phase;
} Mk8BlurSequence;

/* A skipped draw breaks the sequence. Publish the proposed phase only after
 * recording a nonempty Vulkan draw. */
static inline Mk8BlurSequence mk8_blur_begin(Mk8BlurSequence *submitted) {
    Mk8BlurSequence pending = *submitted;
    submitted->phase = 0;
    return pending;
}

/* Only the first reduction and its H/V pair after this extraction were validated. */
static inline unsigned mk8_blur_draw(Mk8BlurSequence *s, uint64_t frame, uint64_t shader, bool enlarged) {
    if (frame != s->frame) s->phase = 0;
    s->frame = frame;
    bool horizontal = shader == 0xb91aecdd33397b59ull && s->phase == 2;
    bool vertical = shader == 0xa0b620fe821bd4bfull && s->phase == 3;
    // The Vancouver variant needs coverage correction only above native scale.
    if (shader == 0x051303030c68828full || (enlarged && shader == MK8_VANCOUVER_BLOOM_MASK)) s->phase = 1;
    else if (shader == 0x75ee7f36aed0f6f0ull && s->phase == 1) {
        s->phase = 2;
        return MK8_BLUR_REDUCTION;
    }
    else s->phase = horizontal ? 3 : 0;
    return horizontal ? MK8_BLUR_HORIZONTAL : vertical ? MK8_BLUR_VERTICAL : 0;
}

static inline int mk8_blur_locked_lod(float min, float max, bool linear) {
    if (!linear || min != max || !(min >= 0 && min <= 15)) return -1;
    int level = (int)min;
    return level == min ? level : -1;
}

#endif
