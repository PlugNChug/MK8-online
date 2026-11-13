#include <float.h>
extern "C" bool mk8_settings_take_open(void);
extern "C" bool mk8_settings_entry_visible(void);
extern "C" bool mk8_settings_entry_focused(void);

// Prototype artwork over the existing native tile; the game keeps focus, hit testing and navigation.
static void mk8_settings_entry_draw(ImVec2 display) {
    if (!mk8_settings_entry_visible()) return;
    float height = fminf(display.y, display.x * 9.0f / 16.0f), width = height * 16.0f / 9.0f;
    float ox = (display.x - width) * 0.5f, oy = (display.y - height) * 0.5f, scale = height / 720.0f;
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    ImVec2 centre(ox + width * 0.5125f, oy + height * 0.8472f);
    draw->AddRectFilled(ImVec2(centre.x - 36 * scale, centre.y - 14 * scale),
                        ImVec2(centre.x + 36 * scale, centre.y + 14 * scale), IM_COL32(248, 248, 248, 255), 2 * scale);
    for (int tooth = 0; tooth < 8; tooth++) {
        float a = tooth * 3.14159265f / 4;
        draw->AddLine(ImVec2(centre.x + cosf(a) * 11 * scale, centre.y + sinf(a) * 11 * scale),
                      ImVec2(centre.x + cosf(a) * 15 * scale, centre.y + sinf(a) * 15 * scale),
                      IM_COL32(85, 90, 96, 255), 6 * scale);
    }
    draw->AddCircleFilled(centre, 12 * scale, IM_COL32(85, 90, 96, 255), 24);
    draw->AddCircleFilled(centre, 5 * scale, IM_COL32(248, 248, 248, 255), 16);
    if (!mk8_settings_entry_focused()) return;
    ImVec2 label(ox + width * 0.5125f, oy + height * 0.9028f);
    draw->AddRectFilled(ImVec2(label.x - 102 * scale, label.y - 14 * scale),
                        ImVec2(label.x + 102 * scale, label.y + 14 * scale), IM_COL32(248, 248, 248, 255), 5 * scale);
    const char *name = "Settings";
    float size = 19 * scale;
    ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, name);
    draw->AddText(ImGui::GetFont(), size, ImVec2(label.x - extent.x * 0.5f, label.y - extent.y * 0.5f),
                  IM_COL32(65, 65, 65, 255), name);
}
