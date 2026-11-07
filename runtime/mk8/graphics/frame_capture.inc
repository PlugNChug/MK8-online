// Diagnostic GPU readback, off unless a local output directory is explicitly supplied.
static void mk8_capture_frame(uint64_t frame) {
    const char *directory = getenv("MK8_CAPTURE_DIR");
    if (!directory || !*directory || frame == 0 || frame > 600 || frame % 60) return;
    extern char g_window_shot_request[256];
    if (g_window_shot_request[0]) return;
    int n = snprintf(g_window_shot_request, 256, "%s/mk8-frame-%llu.png", directory, (unsigned long long)frame);
    if (n < 0 || n >= 256) {
        g_window_shot_request[0] = 0;
        return;
    }
}
