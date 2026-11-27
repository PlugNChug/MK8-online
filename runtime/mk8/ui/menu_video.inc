// Advanced pages keep their detailed help and dynamic readouts around the shared settings.
extern "C" bool mk8_settings_values_main_menu(void);

static bool mk8_video_toggle(Mk8MenuField field, const char *help) {
    const Mk8MenuOption &option = *mk8_menu_option(field);
    if (!mk8_menu_option_enabled(field, mk8_settings_values_main_menu())) opt_disable_next();
    bool value = __atomic_load_n(option.toggle, __ATOMIC_RELAXED);
    if (!opt_toggle(option.label, &value, option.default_index != 0, help)) return false;
    __atomic_store_n(option.toggle, value, __ATOMIC_RELAXED);
    return true;
}

static void mk8_video_choice(Mk8MenuField field, const char *help, bool wrap = true) {
    const Mk8MenuOption &option = *mk8_menu_option(field);
    int selected = mk8_menu_choice_index(field);
    if (selected < 0) selected = option.default_index;
    if (!mk8_menu_option_enabled(field, mk8_settings_values_main_menu())) opt_disable_next();
    if (opt_choice(option.label, &selected, option.names, option.count, option.default_index, help, wrap))
        *option.choice = option.values ? option.values[selected] : selected;
}
