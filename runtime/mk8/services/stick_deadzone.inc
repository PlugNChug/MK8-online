static void mk8_apply_stick_deadzone(int32_t axes[4], int percent) {
    if (!percent) return;

    const float full = 32767.0f;
    const float dead = full * percent / 100.0f;
    for (unsigned i = 0; i < 4; i += 2) {
        float x = axes[i], y = axes[i + 1];
        float radius_squared = x * x + y * y;
        if (radius_squared <= dead * dead) {
            axes[i] = axes[i + 1] = 0;
        } else if (radius_squared < full * full) {
            // Rescale from the inner edge to full travel without changing the angle.
            float radius = sqrtf(radius_squared);
            float scale = (radius - dead) * full / ((full - dead) * radius);
            axes[i] = lroundf(x * scale);
            axes[i + 1] = lroundf(y * scale);
        }
    }
}
