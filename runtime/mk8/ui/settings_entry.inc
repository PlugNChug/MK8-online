#include <float.h>
extern "C" bool mk8_settings_take_open(void);
extern "C" bool mk8_settings_entry_visible(void);
extern "C" bool mk8_settings_entry_focused(void);

static float mk8_settings_phase(bool open) {
    static bool was_open;
    static Uint64 began;
    Uint64 now = SDL_GetTicks();
    if (open && !was_open) began = now;
    was_open = open;
    float t = fminf(1, (now - began) / 160.0f);
    return 1 - (1-t)*(1-t)*(1-t);
}

// An additional host-drawn tile extends native controller navigation without replacing the DLC action.
static void mk8_settings_entry_draw(ImVec2 display) {
    if (!mk8_settings_entry_visible()) return;
    float height = fminf(display.y, display.x * 9.0f / 16.0f), width = height * 16.0f / 9.0f;
    float ox = (display.x - width) * 0.5f, oy = (display.y - height) * 0.5f, scale = height / 720.0f;
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    bool selected = mk8_settings_entry_focused();
    ImVec2 shop(ox + width * 0.5125f, oy + height * 0.8472f);
    if (selected) {
        // Cover the native cursor while its focus anchors the extra runtime slot.
        draw->AddRectFilled(ImVec2(shop.x-48*scale,shop.y-45*scale), ImVec2(shop.x+48*scale,shop.y+28*scale), IM_COL32(247,249,250,255),4*scale);
        draw->AddRectFilled(ImVec2(shop.x-37*scale,shop.y-15*scale),ImVec2(shop.x+37*scale,shop.y+15*scale),IM_COL32(225,48,32,255),3*scale);
        draw->AddRect(ImVec2(shop.x-9*scale,shop.y-22*scale),ImVec2(shop.x+9*scale,shop.y-7*scale),IM_COL32(255,255,255,255),5*scale,0,2*scale);
    }
    ImVec2 centre(ox + width * 0.586f, oy + height * 0.8472f);
    draw->AddRectFilled(ImVec2(centre.x-43*scale,centre.y-23*scale),ImVec2(centre.x+43*scale,centre.y+23*scale), selected ? IM_COL32(255,211,28,255) : IM_COL32(220,227,232,255),5*scale);
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
    if (!selected) return;
    draw->AddRectFilled(ImVec2(shop.x-102*scale,oy+height*0.883f),ImVec2(shop.x+102*scale,oy+height*0.923f),IM_COL32(248,248,248,255),5*scale);
    ImVec2 label(centre.x, oy + height * 0.9028f);
    draw->AddRectFilled(ImVec2(label.x - 102 * scale, label.y - 14 * scale),
                        ImVec2(label.x + 102 * scale, label.y + 14 * scale), IM_COL32(248, 248, 248, 255), 5 * scale);
    const char *name = "Settings";
    float size = 19 * scale;
    ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, name);
    draw->AddText(ImGui::GetFont(), size, ImVec2(label.x - extent.x * 0.5f, label.y - extent.y * 0.5f),
                  IM_COL32(65, 65, 65, 255), name);
}
