"""Honor the presentation setting without changing MK8 camera or render-target shape."""


def prepare(runtime):
    path = runtime/'nvn/output_scale.c'
    text = path.read_text(encoding='utf-8')
    old = '    o->vx = (dw - o->vw) / 2; o->vy = (dh - o->vh) / 2;'
    if text.count(old) != 1:
        raise ValueError('Output aspect boundary changed')
    text = text.replace(old, '''    // MK8 has no widened camera hook; fill means stretch the final 16:9 image.
    if (!reshape && wide_test <= 0 && g_settings.aspect == ASPECT_UNLOCKED) {
        o->vw = dw; o->vh = dh;
    }
'''+old)
    path.write_text(text, encoding='utf-8')
    path = runtime/'ui/menu.mm'
    text = path.read_text(encoding='utf-8')
    old = '''    const char *aspects[] = {"Unlocked", "Original 16:9"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               "Unlocked: the view fills the window, so wider (or taller) windows show more of the island - up to "
               "ultrawide; houses and dungeons keep their framing. Original 16:9: the game's own framing, with bars.");'''
    if text.count(old) != 1:
        raise ValueError('Aspect menu boundary changed')
    text = text.replace(old, '''    const char *aspects[] = {"Stretch to window", "Original 16:9"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               "Stretch to window: fills the window by stretching the image; camera field of view is unchanged. "
               "Original 16:9: preserves proportions with bars when needed.");''')
    path.write_text(text, encoding='utf-8')
