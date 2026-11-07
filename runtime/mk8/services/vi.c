// MK8 uses the same host-owned layers as the base VI HLE. Do not call the guest
// VI service for scaling: Initialize/CreateLayer intentionally have no guest IPC object.
#include "hle.h"

HLE_FUNC("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", mk8_vi_scaling) {
    if (c->x[0] < GUEST_LO || c->x[0] >= GUEST_HI)
        FATAL("VI scaling received an invalid host layer: %llx\n", c->x[0]);
    // shortcut: the host compositor already fills its layer; add other scaling
    // modes to the presentation backend when a game actually requests them.
    if ((uint32_t)c->x[1] != 2)
        FATAL("VI scaling mode not implemented: %llu\n", c->x[1]);
    LOG("[vi] host layer scaling mode %llu\n", c->x[1]);
    c->x[0] = 0;
}
