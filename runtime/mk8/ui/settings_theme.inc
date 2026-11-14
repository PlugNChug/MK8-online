extern "C" bool mk8_settings_style_active(void);
static void mk8_settings_style_extras(ImGuiStyle &s) {
    if (!mk8_settings_style_active()) return;
    s.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.72f,0.84f,0.93f,1);
    s.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1,1,1,0.30f);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.33f,0.47f,0.56f,0.8f);
}
static const ThemePalette &mk8_settings_palette(const ThemePalette &base) {
    static ThemePalette p = base;
    p.window = {0.96f,0.98f,1,0.97f}; p.child = {0.88f,0.94f,0.98f,0.94f}; p.popup = {0.97f,0.98f,1,1};
    p.text = {0.10f,0.16f,0.22f,1}; p.text_dim = {0.30f,0.38f,0.44f,1};
    p.frame = {1,1,1,1}; p.button = {0.90f,0.94f,0.97f,1}; p.tab = p.button;
    p.hover = {1,0.84f,0.12f,1}; p.active = {1,0.76f,0.04f,1}; p.pressed = p.active;
    p.tab_selected = p.hover; p.tab_alt = {0.78f,0.88f,0.95f,1};
    p.line = {0.60f,0.73f,0.83f,1}; p.muted = p.text_dim;
    p.accent = {0.08f,0.28f,0.43f,1}; p.accent_dim = {0.15f,0.52f,0.78f,1};
    p.window_round = 14; p.child_round = 8; p.frame_round = 7; p.tab_round = 7;
    p.window_border = 2; p.frame_border = 1; p.row_round = 7; p.row_border = 1; p.row_notch = 0;
    return p;
}

static void mk8_settings_backdrop(ImVec2 size) {
    if (!mk8_settings_style_active()) return;
    ImDrawList *d = ImGui::GetBackgroundDrawList();
    d->AddRectFilledMultiColor(ImVec2(0,0), size, IM_COL32(26,156,216,255), IM_COL32(48,184,225,255),
                              IM_COL32(12,95,168,255), IM_COL32(11,119,190,255));
    float cell = size.y / 18;
    float shift = fmodf((float)ImGui::GetTime() * 9, cell * 2);
    for (int y = -1; y < 20; y++) for (int x = -2; x < (int)(size.x / cell) + 2; x++)
        if ((x + y) % 2 == 0) d->AddRectFilled(ImVec2(x*cell+shift,y*cell),ImVec2((x+1)*cell+shift,(y+1)*cell),IM_COL32(255,255,255,12));
    float s = size.y / 720;
    d->AddRectFilled(ImVec2(0,0),ImVec2(size.x,39*s),IM_COL32(255,216,34,255));
    d->AddText(ImGui::GetFont(),23*s,ImVec2(30*s,7*s),IM_COL32(24,46,64,255),"PC SETTINGS");
    d->AddRectFilled(ImVec2(0,size.y-37*s),size,IM_COL32(12,37,65,245));
    d->AddText(ImGui::GetFont(),17*s,ImVec2(30*s,size.y-28*s),IM_COL32(255,255,255,255),
               "B / Esc: Back     A / Enter: Select     Left / Right: Change     L / R or Q / E: Tabs");
}
