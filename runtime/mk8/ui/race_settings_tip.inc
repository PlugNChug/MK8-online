// Draw on the presentation layer so this passive hint also works between race scenes.
static void mk8_draw_race_settings_tip(ImVec2 display, float scale, const char *text, float alpha) {
    if (alpha <= 0 || display.x <= 0 || display.y <= 0) return;
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    ImFont *font = ImGui::GetFont();
    const float font_size = 30 * scale;
    const float stroke = fmaxf(1.f, roundf(scale));
    const float max_width = fminf(display.x * 0.8f, 1100 * scale);
    const float pad_x = fminf(44 * scale, max_width / 8), pad_y = 11 * scale;
    const float wrap = fmaxf(1.f, max_width - 2 * pad_x - stroke);
    ImVec2 label = font->CalcTextSizeA(font_size, FLT_MAX, wrap, text);
    const float width = fminf(max_width, ceilf(label.x + stroke) + 2 * pad_x);
    const float left = floorf((display.x - width) / 2), right = left + width;
    const float bottom = floorf(display.y * (5.f / 6.f)), top = bottom - label.y - 2 * pad_y;
    auto color = [alpha](int r, int g, int b, float opacity) {
        return IM_COL32(r, g, b, (int)(alpha * opacity + 0.5f));
    };
    const ImU32 upper = color(49, 54, 61, 185), lower = color(22, 25, 31, 206);
    const float x[] = {left, left + pad_x, right - pad_x, right};
    for (int i = 0; i < 3; ++i) {
        draw->AddRectFilledMultiColor(ImVec2(x[i], top), ImVec2(x[i+1], bottom),
            i == 0 ? 0 : upper, i == 2 ? 0 : upper, i == 2 ? 0 : lower, i == 0 ? 0 : lower);
    }
    // The Settings hint's check squares are 8.8 pixels at a 720-pixel output height.
    const float cell = 13.2f * scale;
    auto checker = [&](float px) {
        float edge = fminf(1.f, fminf(px - left, right - px) / pad_x);
        return color(224, 228, 234, 17 * edge);
    };
    for (int row = 0; top + row * cell < bottom; ++row) {
        float y = top + row * cell;
        for (float px = left + (row % 2) * cell; px < right; px += 2 * cell) {
            float end = fminf(right, px + cell);
            ImU32 a = checker(px), b = checker(end);
            draw->AddRectFilledMultiColor(ImVec2(px, y), ImVec2(end, fminf(y + cell, bottom)), a, b, b, a);
        }
    }
    const char *end = text + strlen(text);
    float y = top + pad_y;
    while (text < end) {
        const char *line = font->CalcWordWrapPositionA(font_size / font->FontSize, text, end, wrap);
        float w = font->CalcTextSizeA(font_size, FLT_MAX, 0, text, line).x;
        ImVec2 p(floorf((display.x - w - stroke) / 2), floorf(y));
        draw->AddText(font, font_size, ImVec2(p.x + stroke, p.y + stroke), color(0, 0, 0, 240), text, line);
        // Give the available font the game tip's heavier edge without loading another face.
        draw->AddText(font, font_size, ImVec2(p.x + stroke, p.y), color(255, 255, 255, 170), text, line);
        draw->AddText(font, font_size, p, color(255, 255, 255, 255), text, line);
        text = line;
        while (text < end && *text == ' ') ++text;
        y += font_size;
    }
}
