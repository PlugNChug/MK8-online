// Return one current sample through the existing SDL touch backend, not the guest ring buffer.
HLE_FUNC("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", mk8_touch_states16) {
    int32_t capacity = (int32_t)c->x[1];
    if (capacity <= 0 || c->x[0] < GUEST_LO || c->x[0] > GUEST_HI - (16 + 0x28 * 16)) {
        c->x[0] = 0;
        return;
    }
    fill_touch(c, 16);
    c->x[0] = 1;
}
