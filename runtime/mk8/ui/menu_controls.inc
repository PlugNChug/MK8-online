    header("Button prompts");
    {
        const Mk8MenuOption &prompts = *mk8_menu_option(MK8_MENU_PROMPTS);
        char auto_name[64];
        snprintf(auto_name, sizeof auto_name, "Auto (%s)", prompts.names[1 + input_pad_family() % 3]);
        const char *names[] = {auto_name, prompts.names[1], prompts.names[2], prompts.names[3]};
        choice(prompts.label, prompts.choice, names, prompts.count);
    }
    {
        const Mk8MenuOption &vibration = *mk8_menu_option(MK8_MENU_CONTROLLER_VIBRATION);
        bool enabled = *vibration.toggle;
        if (opt_toggle(vibration.label, &enabled, vibration.default_index, vibration.help))
            mk8_rumble_set_enabled(enabled);
    }
    ImGui::Spacing();
    header("System language");
    {
        const Mk8MenuOption &language = *mk8_menu_option(MK8_MENU_LANGUAGE);
        char auto_name[64];
        snprintf(auto_name, sizeof auto_name, "Auto (%s)", settings_language_name());
        const char *names[N_LANGUAGES];
        names[0] = auto_name;
        for (int i = 1; i < language.count; ++i) names[i] = language.names[i];
        ImGui::BeginDisabled(!mk8_menu_option_enabled(MK8_MENU_LANGUAGE, mk8_settings_values_main_menu()));
        if (choice(language.label, language.choice, names, language.count)) settings_save();
        ImGui::EndDisabled();
    }
