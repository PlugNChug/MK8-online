// Relative shared-font files are redirected only after a launcher font selection.
    char mk8_font_path[4096];
    if (!strncmp(path, "fonts/", 6)) {
        const char *mode = getenv("MK8_FONT_MODE");
        if (mode && !strcmp(mode, "system")) return NULL;
        const char *dir = getenv("MK8_FONT_DIR");
        if (mode && !strcmp(mode, "console") && dir && *dir) {
            // Verbatim Windows paths (\\?\) do not normalize forward slashes.
#ifdef _WIN32
            const char *separator = "\\";
#else
            const char *separator = "/";
#endif
            int n = snprintf(mk8_font_path, sizeof mk8_font_path, "%s%s%s", dir, separator, path + 6);
            if (n < 0 || (size_t)n >= sizeof mk8_font_path) return NULL;
            path = mk8_font_path;
        }
    }
