// shortcut: verified solo race and GP/no-team VS intro layouts only; other overlays need their own evidence.
#include "rt.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

double mk8_ultrawide_aspect(void);
uint8_t *mk8_ultrawide_memory(uint64_t address, size_t size, bool write);

typedef struct { uint8_t *pane; float x, scale, anchor; bool fade_left; uint8_t alpha[2]; } SavedPane;
static _Atomic bool rejected;

static uint64_t u64(const uint8_t *p, size_t offset) {
    uint64_t value;
    memcpy(&value, p + offset, sizeof value);
    return value;
}

static uint32_t u32(const uint8_t *p, size_t offset) {
    uint32_t value;
    memcpy(&value, p + offset, sizeof value);
    return value;
}

static float f32(const uint8_t *p, size_t offset) {
    float value;
    memcpy(&value, p + offset, sizeof value);
    return value;
}

static bool named(const uint8_t *pane, const char *name) {
    size_t n = strlen(name);
    return n < 24 && !memcmp(pane + 0xb8, name, n + 1);
}

static bool transform(const uint8_t *pane, bool identity, unsigned anchor) {
    if ((pane[0x58] & 0x60) || pane[0x5b] != anchor) return false;
    for (unsigned at = 0x30; at <= 0x44; at += 4) {
        float v = f32(pane, at);
        if (!isfinite(v) || fabsf(v) > 1280.f || ((identity || at >= 0x3c) && v != 0.f)) return false;
    }
    for (unsigned at = 0x48; at <= 0x4c; at += 4) {
        float v = f32(pane, at);
        if (!isfinite(v) || v <= 0.f || v > 4.f || (identity && v != 1.f)) return false;
    }
    return true;
}

// Follow only one direct-child list, checking its links and a fixed upper bound.
static uint8_t *child(const uint8_t *parent, const char *name) {
    uint64_t head = (uint64_t)(uintptr_t)parent + 0x20, previous = head, link = u64(parent, 0x28);
    uint8_t *found = NULL;
    for (unsigned i = 0; link != head; i++) {
        if (i == 32 || link < 8) return NULL;
        uint8_t *pane = mk8_ultrawide_memory(link - 8, 0xd0, false);
        if (!pane || u64(pane, 8) != previous || u64(pane, 0x18) != (uint64_t)(uintptr_t)parent) return NULL;
        if (named(pane, name)) {
            if (found) return NULL;
            found = pane;
        }
        previous = link;
        link = u64(pane, 0x10);
    }
    return u64(parent, 0x20) == previous ? found : NULL;
}

static uint8_t *current_manager(void) {
    uint8_t *slot = mk8_ultrawide_memory(MAIN_BASE + 0x135bb20, 8, false);
    uint8_t *ui = slot ? mk8_ultrawide_memory(u64(slot, 0), 0x18, false) : NULL;
    uint64_t address = ui ? u64(ui, 0x10) : 0;
    uint8_t *manager = mk8_ultrawide_memory(address, 0xc60, false);
    if (!manager || u64(manager, 0) != MAIN_BASE + 0x12d45a0 || u32(manager, 0x28) != 135 ||
        u32(manager, 0xb50) < 2 || u32(manager, 0xb50) > 32 || u32(manager, 0xb54) != 32 ||
        u64(manager, 0xb58) != address + 0xb60) return NULL;
    return manager;
}

static bool intro_children(const uint8_t *parent, const char *const names[], uint8_t *panes[], unsigned count) {
    uint64_t previous = (uint64_t)(uintptr_t)parent + 0x20;
    for (unsigned i = 0; i < count; i++) {
        panes[i] = child(parent, names[i]);
        if (!panes[i] || u64(panes[i], 8) != previous) return false;
        previous = (uint64_t)(uintptr_t)panes[i] + 8;
    }
    return u64(parent, 0x20) == previous;
}

static unsigned collect_intro(const uint8_t *root, const uint8_t *all, SavedPane saved[8]) {
    const char *const all_names[] = {"W_BG_00", "N_BGPtn_00", "N_CupIcon_00", "N_Text_00", "N_Team_00"};
    const char *const bg_names[] = {"P_MtCheck_32x32", "P_CupIconBGDummy_00", "P_ModeIconBGDummy_00",
                                  "P_ClassIconBGDummy_00", "P_BattleIconBGDummy_00"};
    const char *const cup_names[] = {"N_CupIcon_01", "P_ModeIconTt_00", "P_ModeIconVs_00", "P_ModeIconBt_00", "N_CompeIcon_00"};
    const char *const text_names[] = {"T_CupName_00", "T_CompeName_00", "T_ClassicName_00", "T_CourseName_00", "N_Class_00", "N_Battle_00"};
    uint8_t *groups[5], *bg[5], *cup[5], *text[6], *badge[1];
    const char *const badge_name[] = {"P_ClassIconDummy_00"};
    uint64_t all_link = (uint64_t)(uintptr_t)all + 8;
    if (u64(root, 0x20) != all_link || u64(root, 0x28) != all_link ||
        f32(all, 0x50) != 30.f || f32(all, 0x54) != 40.f ||
        !intro_children(all, all_names, groups, 5) || !intro_children(groups[1], bg_names, bg, 5) ||
        !intro_children(groups[2], cup_names, cup, 5) || !intro_children(groups[3], text_names, text, 6) ||
        !intro_children(text[4], badge_name, badge, 1)) return 0;
    const uint8_t *identity[] = {groups[1], groups[3], text[4]};
    for (unsigned i = 0; i < 3; i++)
        if (u64(identity[i], 0) != MAIN_BASE + 0x12abd70 || !transform(identity[i], true, 0) ||
            f32(identity[i], 0x50) != 30.f || f32(identity[i], 0x54) != 40.f) return 0;
    bool gp = cup[0][0x58] & 1, vs = cup[2][0x58] & 1;
    if (gp == vs || !!(bg[1][0x58] & 1) != gp || !!(bg[2][0x58] & 1) != vs) return 0;
    const uint8_t *hidden[] = {groups[4], bg[4], cup[1], cup[3], cup[4], text[1], text[5]};
    for (unsigned i = 0; i < sizeof hidden/sizeof *hidden; i++)
        if (hidden[i][0x58] & 1) return 0;
    if (u64(cup[0], 0) != MAIN_BASE + 0x12abd70 || !transform(cup[0], true, 0) ||
        f32(cup[0], 0x50) != 140.f || f32(cup[0], 0x54) != 140.f ||
        u64(badge[0], 0) != MAIN_BASE + 0x12bf298 || !transform(badge[0], false, 0) ||
        f32(badge[0], 0x48) != .32f || f32(badge[0], 0x4c) != .32f ||
        f32(badge[0], 0x50) != 300.f || f32(badge[0], 0x54) != 300.f) return 0;
    if (vs && (u64(cup[2], 0) != MAIN_BASE + 0x12bf298 || !transform(cup[2], true, 0) ||
        f32(cup[2], 0x50) != 162.f || f32(cup[2], 0x54) != 124.f)) return 0;
    // Keep the animated band full-width; its separate decorations retain their proportions.
    const uint8_t *band = groups[0];
    if (u64(band, 0) != MAIN_BASE + 0x12bf8e8 || !transform(band, false, 0) ||
        f32(band, 0x30) != 0.f || f32(band, 0x34) != -260.f || f32(band, 0x38) != 0.f ||
        f32(band, 0x48) != 1.f || f32(band, 0x4c) != 1.f ||
        !isfinite(f32(band, 0x50)) || f32(band, 0x50) < 160.f || f32(band, 0x50) > 1440.f ||
        f32(band, 0x54) != 120.f) return 0;
    uint8_t *targets[] = {groups[2], text[0], text[2], text[3], vs ? bg[2] : bg[1], text[4], bg[3], bg[0]};
    const uint64_t vtables[] = {0x12abd70, 0x12bf708, 0x12bf708, 0x12bf708, 0x12bf298, 0x12abd70, 0x12bf298, 0x12bf298};
    for (unsigned i = 0; i < 8; i++) {
        uint8_t *pane = targets[i];
        if (u64(pane, 0) != MAIN_BASE + vtables[i] ||
            !mk8_ultrawide_memory((uint64_t)(uintptr_t)pane, i == 6 ? 0xf8 : 0xd0, true)) return 0;
        if (i == 7) {
            // The checker's X-axis rotation leaves its horizontal coordinates independent.
            if ((pane[0x58] & 0x60) || pane[0x5b] != 8 ||
                !isfinite(f32(pane, 0x30)) || fabsf(f32(pane, 0x30)) > 1280.f ||
                f32(pane, 0x34) != -320.f || f32(pane, 0x38) != 0.f ||
                f32(pane, 0x3c) != -70.f || f32(pane, 0x40) != 0.f || f32(pane, 0x44) != 0.f ||
                f32(pane, 0x48) != 1.f || f32(pane, 0x4c) != 1.f ||
                f32(pane, 0x50) != 720.f || f32(pane, 0x54) != 120.f) return 0;
        } else {
            if (!transform(pane, false, i >= 1 && i <= 3 ? 1 : 0)) return 0;
            float scale = f32(pane, 0x48);
            if (scale != f32(pane, 0x4c) || (i == 0 ? scale < .8999f || scale > 1.5001f : scale != 1.f)) return 0;
            if (i >= 1 && i <= 3) {
                // Localized text changes glyph metrics; preserve its own origin and alignment offsets.
                float width = f32(pane, 0x50), height = f32(pane, 0x54);
                if (!isfinite(width) || width <= 0.f || width > 1280.f ||
                    !isfinite(height) || height <= 0.f || height > 720.f) return 0;
            } else if (i != 5 && (f32(pane, 0x50) != (i == 0 ? 140.f : i == 4 ? 400.f : 300.f) ||
                                  f32(pane, 0x54) != (i == 0 ? 140.f : 120.f))) return 0;
        }
        saved[i] = (SavedPane){pane, f32(pane, 0x30), f32(pane, 0x48), i < 5 ? -1.f : i < 7 ? 1.f : 0.f, i == 6, {0}};
        if (i == 6) {
            saved[i].alpha[0] = pane[0xeb];
            saved[i].alpha[1] = pane[0xf3];
        }
    }
    return 8;
}

static bool pane_shown(const uint8_t *pane) {
    return pane && (pane[0x58]&1) && pane[0x59];
}

static uint8_t *fader_layout_root(uint64_t address) {
    uint8_t *page=mk8_ultrawide_memory(address,0xf0,false);
    if(!page || u64(page,0)!=MAIN_BASE+0x12733d8 || u32(page,0xe8)!=4) return NULL;
    uint8_t *layout=mk8_ultrawide_memory(u64(page,0x28),0x90,false);
    if(!layout || u64(layout,0)!=MAIN_BASE+0x12d5ee0 || u64(layout,0x80)!=address) return NULL;
    uint8_t *root=mk8_ultrawide_memory(u64(layout,0x18),0xd0,false);
    return root && named(root,"RootPane") ? root : NULL;
}

static const char *const fader_parts[] = {"cm_Fade_00","cm_Fade_01","cm_LoadScreen_00","L_SettingsRoot"};

uint64_t mk8_settings_fader_root(uint64_t address) {
    uint8_t *root=fader_layout_root(address), *parts[4];
    if(!root || !intro_children(root,fader_parts,parts,4) || u64(parts[3],0)!=MAIN_BASE+0x12bf1a8) return 0;
    return (uint64_t)(uintptr_t)parts[3];
}

// The stock always-resident fader page also exists during normal racing. Its membership
// alone says nothing about whether the guest Menu is behind a transition.
bool mk8_settings_fader_covers(uint64_t address) {
    uint8_t *root=fader_layout_root(address);
    if(!pane_shown(root)) return false;
    const char *names[]={"cm_LoadScreen_00","cm_Fade_00","cm_Fade_01"};
    for(unsigned i=0;i<3;i++) {
        uint8_t *part=child(root,names[i]);
        if(!pane_shown(part)) continue;
        uint8_t *all=child(part,"N_All_00");
        if(!pane_shown(all)) continue;
        if(i==0) return true;
        uint8_t *bg=child(all,"P_FadeBG_00");
        if(pane_shown(bg)) {
            bg=mk8_ultrawide_memory((uint64_t)(uintptr_t)bg,0xf8,false);
            if(bg && (bg[0xeb] || bg[0xef] || bg[0xf3] || bg[0xf7])) return true;
        }
    }
    return false;
}

static bool collect_fade(const uint8_t *root) {
    const char *const all_name[] = {"N_All_00"};
    const char *const groups[] = {"P_FadeBG_00", "N_Load_B", "N_Load_W", "N_Logo_00"};
    uint8_t *parts[4], *all[1], *panes[4];
    if (!intro_children(root, fader_parts, parts, 3) &&
        (!intro_children(root, fader_parts, parts, 4) || u64(parts[3],0)!=MAIN_BASE+0x12bf1a8)) return false;
    for (unsigned i = 0; i < 3; i++) {
        if (u64(parts[i], 0) != MAIN_BASE + 0x12bf1a8 || !transform(parts[i], true, 0) ||
            f32(parts[i], 0x50) != (i == 2 ? 6100.f : 1280.f) || f32(parts[i], 0x54) != 720.f ||
            !intro_children(parts[i], all_name, all, 1) || u64(all[0], 0) != MAIN_BASE + 0x12abd70 ||
            !transform(all[0], true, 0) || f32(all[0], 0x50) != 30.f || f32(all[0], 0x54) != 40.f) return false;
        if (i == 2) {
            if (parts[i][0x58] & 1) return false;  // Loading artwork has not been qualified.
            continue;
        }
        if (!intro_children(all[0], groups, panes, 4)) return false;
        const uint8_t *bg = panes[0];
        uint64_t head = (uint64_t)(uintptr_t)bg + 0x20;
        if (u64(bg, 0) != MAIN_BASE + 0x12bf298 || !transform(bg, true, 0) ||
            f32(bg, 0x50) != 1280.f || f32(bg, 0x54) != 720.f ||
            u64(bg, 0x20) != head || u64(bg, 0x28) != head) return false;
        for (unsigned j = 1; j < 4; j++) {
            const uint8_t *pane = panes[j];
            if ((pane[0x58] & 1) || u64(pane, 0) != MAIN_BASE + 0x12abd70 || !transform(pane, false, 0) ||
                f32(pane, 0x30) != (j == 3 ? 0.f : 550.99994f) ||
                f32(pane, 0x34) != (j == 3 ? 0.f : -304.f) || f32(pane, 0x38) != 0.f ||
                f32(pane, 0x48) != 1.f || f32(pane, 0x4c) != 1.f ||
                f32(pane, 0x50) != 30.f || f32(pane, 0x54) != 40.f) return false;
        }
    }
    // Both uniform-color fade quads retain full-window coverage, including their animated alpha.
    return true;
}

static unsigned collect(uint64_t address, SavedPane saved[8], uint64_t *root_address, bool *fade) {
    *fade = false;
    uint8_t *layout = mk8_ultrawide_memory(address, 0x90, false);
    if (!layout || u64(layout, 0) != MAIN_BASE + 0x12d5ee0 || u64(layout, 0x88)) return 0;
    uint8_t *page = mk8_ultrawide_memory(u64(layout, 0x80), 0xf0, false);
    if (!page || u64(page, 0x28) != address) return 0;
    bool solo = u64(page, 0) == MAIN_BASE + 0x1289240;
    bool finish = u64(page, 0) == MAIN_BASE + 0x1289538;
    bool intro = u64(page, 0) == MAIN_BASE + 0x1287d40;
    bool fader = u64(page, 0) == MAIN_BASE + 0x12733d8;
    if (!solo && !finish && !intro && !fader && u64(page, 0) != MAIN_BASE + 0x1287160) return 0;
    unsigned id = solo ? 101 : finish ? 106 : intro ? 110 : fader ? 4 : 105;
    if (u32(page, 0xa0) != id || u32(page, 0xe8) != id) return 0;
    uint8_t *manager = current_manager();
    uint8_t *registered = manager ? mk8_ultrawide_memory(u64(manager, 0x30), 135*8, false) : NULL;
    uint64_t page_address = (uint64_t)(uintptr_t)page;
    if (!registered || u64(page, 0x20) != (uint64_t)(uintptr_t)manager ||
        u64(registered, 8*id) != page_address) return 0;
    uint32_t count = u32(manager, 0xb50);
    if (count < 2 || count > 32) return 0;
    // The guest traverses a copied active list, which may still contain a retired page.
    unsigned membership = 0;
    for (unsigned i = 0; i < count; i++)
        membership += u64(manager, 0xb60 + 8*i) == page_address;
    if (membership != 1) return 0;
    uint8_t *root = mk8_ultrawide_memory(u64(layout, 0x18), 0xd0, false);
    if (!root || u64(root, 0) != MAIN_BASE + 0x12bf390 || u64(root, 0x18) ||
        !named(root, "RootPane") || !transform(root, true, 0) ||
        f32(root, 0x50) != 1280.f || f32(root, 0x54) != 720.f) return 0;
    if (fader) {
        *root_address = (uint64_t)(uintptr_t)root;
        *fade = collect_fade(root);
        return 0;  // Validated coverage needs no compensation targets or guest writes.
    }
    uint8_t *pause = child(root, finish || intro ? "N_All_00" : "N_Pause_00");
    if (!pause || u64(pause, 0) != MAIN_BASE + 0x12abd70 || !transform(pause, true, 0)) return 0;
    if (intro) {
        *root_address = (uint64_t)(uintptr_t)root;
        return collect_intro(root, pause, saved);
    }
    const char *names[4] = {solo ? "N_ItemBoxPos_00" : "L_TVMap", solo ? "L_LapCoin_00" : "L_Timer_00",
                          solo ? "L_Rank_00" : "L_Go_00", "L_RaceNum_00"};
    if (finish) names[0] = "L_Finish_00";
    unsigned collected = 0;
    for (unsigned i = 0; i < (finish ? 1u : solo ? 3u : 4u); i++) {
        uint8_t *pane = child(pause, names[i]);
        uint64_t vtable = solo && i == 0 ? MAIN_BASE + 0x12abd70 : MAIN_BASE + 0x12bf1a8;
        if (!pane || u64(pane, 0) != vtable || !transform(pane, false, 0) ||
            !mk8_ultrawide_memory((uint64_t)(uintptr_t)pane, 0xd0, true)) return 0;
        if (solo && i == 2 && !(pane[0x58] & 1)) continue;  // Time Trial keeps its rank pane hidden and untouched.
        float anchor = finish ? 0.f : i >= 2 ? (solo ? 1.f : 0.f) : (solo ? -1.f : 1.f);
        saved[collected++] = (SavedPane){pane, f32(pane, 0x30), f32(pane, 0x48), anchor, false, {0}};
    }
    if (solo) {
        for (unsigned alarm = 0; alarm < 2; alarm++) {
            const char *names[] = {alarm ? "L_Alarm_01" : "L_Alarm_00", "N_All_00",
                                   "N_Alarm_Scope_00", "N_Alarm_00"};
            const float y[] = {-320.f, 60.f, 0.f, 0.f};
            const float width[] = {50.f, 30.f, 50.f, 50.f}, height[] = {170.f, 40.f, 50.f, 50.f};
            uint8_t *pane = pause;
            for (unsigned step = 0; step < 4; step++) {
                pane = child(pane, names[step]);
                uint64_t vtable = MAIN_BASE + (step ? 0x12abd70 : 0x12bf1a8);
                if (!pane || u64(pane, 0) != vtable || !transform(pane, false, step == 3 ? 8 : 0) ||
                    f32(pane, 0x34) != y[step] || f32(pane, 0x38) != 0.f ||
                    f32(pane, 0x50) != width[step] || f32(pane, 0x54) != height[step]) return 0;
                float x = f32(pane, 0x30), sx = f32(pane, 0x48), sy = f32(pane, 0x4c);
                if (step == 2 ? fabsf(x) > 700.f : x != 0.f) return 0;
                if (step < 3 && (sx != 1.f || sy != 1.f)) return 0;
                if (step == 3 && (sx < .2f || sx > 1.2f || sx != sy)) return 0;
            }
            if (!mk8_ultrawide_memory((uint64_t)(uintptr_t)pane, 0xd0, true)) return 0;
            // Scale only the artwork; its ancestor carries the full-width warning trajectory.
            saved[collected++] = (SavedPane){pane, f32(pane, 0x30), f32(pane, 0x48), 0.f, false, {0}};
        }
        uint8_t *rivals = child(pause, "N_RivalName_00");
        if (!rivals || u64(rivals, 0) != MAIN_BASE + 0x12abd70 || !transform(rivals, true, 0) ||
            f32(rivals, 0x50) != 30.f || f32(rivals, 0x54) != 40.f) return 0;
        for (unsigned rival = 0; rival < 2; rival++) {
            uint8_t *parts = child(rivals, rival ? "L_RivalName_01" : "L_RivalName_00");
            if (!parts || u64(parts, 0) != MAIN_BASE + 0x12bf1a8 ||
                (parts[0x58] & 0x60) || parts[0x5b] ||
                !isfinite(f32(parts, 0x30)) || !isfinite(f32(parts, 0x34)) ||
                f32(parts, 0x50) != 746.f || f32(parts, 0x54) != 144.f) return 0;
            for (unsigned at = 0x38; at <= 0x44; at += 4)
                if (f32(parts, at) != 0.f) return 0;
            float scale = f32(parts, 0x48);
            if (!isfinite(scale) || scale < .8f || scale > 1.f || f32(parts, 0x4c) != scale) return 0;
            uint8_t *pane = child(parts, "N_All_00");
            if (!pane || u64(pane, 0) != MAIN_BASE + 0x12abd70 || !transform(pane, true, 0) ||
                f32(pane, 0x50) != 30.f || f32(pane, 0x54) != 40.f ||
                !mk8_ultrawide_memory((uint64_t)(uintptr_t)pane, 0xd0, true)) return 0;
            // Preserve the parent's unclamped world projection and distance scale; shrink only its artwork.
            saved[collected++] = (SavedPane){pane, f32(pane, 0x30), f32(pane, 0x48), 0.f, false, {0}};
        }
    }
    *root_address = (uint64_t)(uintptr_t)root;
    return collected;
}

static bool hud_ready(bool intro, bool require_fade) {
    if (atomic_load(&rejected)) return false;
    uint8_t *manager = current_manager();
    if (!manager) return false;
    uint64_t address = (uint64_t)(uintptr_t)manager;
    uint8_t *registered = mk8_ultrawide_memory(u64(manager, 0x30), 135*8, false);
    if (!registered) return false;
    uint32_t count = u32(manager, 0xb50);
    if (count < 2 || count > 32) return false;
    const uint32_t ids[] = {101, 105, 106, 118, 7, 13, 110, 4};
    const uint64_t vtables[] = {0x1289240, 0x1287160, 0x1289538, 0x1287458, 0x12723e0, 0x1271df0, 0x1287d40, 0x12733d8};
    unsigned seen = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t page_address = u64(manager, 0xb60 + 8*i);
        uint8_t *page = mk8_ultrawide_memory(page_address, 0xf0, false);
        if (!page || u64(page, 0x20) != address) return false;
        unsigned kind = 0;
        while (kind < 8 && u64(page, 0) != MAIN_BASE + vtables[kind]) kind++;
        if (kind == 8 || (intro ? kind < 4 : kind == 6) || (seen & (1u << kind)) || u32(page, 0xa0) != ids[kind] ||
            u32(page, 0xe8) != ids[kind] || u64(registered, 8*ids[kind]) != page_address) return false;
        seen |= 1u << kind;
        if (kind < 3 || kind >= 6) {
            SavedPane saved[8];
            uint64_t root;
            bool fade;
            if (!collect(u64(page, 0x28), saved, &root, &fade) && !fade) return false;
        }
    }
    if (require_fade && !(seen & (1u << 7))) return false;
    return intro ? (seen & ~(1u << 7)) == ((1u << 4) | (1u << 5) | (1u << 6)) : (seen & 3) == 3;
}

bool mk8_ultrawide_hud_ready(bool intro) { return hud_ready(intro, false); }

// Race pages can activate under the fade before the deferred flyover camera switch.
bool mk8_ultrawide_hud_handoff_ready(void) { return hud_ready(false, true); }

void mk8_ultrawide_hud(Cpu *c, uint64_t layout) {
    SavedPane saved[8];
    uint64_t root;
    double aspect = mk8_ultrawide_aspect();
    bool active = aspect > 16.0/9.0;
    bool fade = false;
    unsigned count = active && c->pc == MAIN_BASE + 0x592808 ? collect(layout, saved, &root, &fade) : 0;
    bool valid = (count || fade) && c->x[0] == root;
    bool apply = count && valid;
    // World commands already use the chosen camera; reject later frames without changing this frame's presentation.
    if (active && !valid) {
        uint8_t *object = mk8_ultrawide_memory(layout, 0x88, false);
        uint8_t *page = object ? mk8_ultrawide_memory(u64(object, 0x80), 8, false) : NULL;
        if (page && (u64(page, 0) == MAIN_BASE + 0x1289240 || u64(page, 0) == MAIN_BASE + 0x1287160 ||
                     u64(page, 0) == MAIN_BASE + 0x1289538 || u64(page, 0) == MAIN_BASE + 0x1287d40 ||
                     u64(page, 0) == MAIN_BASE + 0x12733d8) &&
            !atomic_exchange(&rejected, true))
            LOG("[mk8:ultrawide] HUD changed after preflight; later frames use the original framing\n");
    }
    if (apply) {
        float factor = (float)((16.0/9.0)/aspect);
        for (unsigned i = 0; i < count; i++) {
            float x = saved[i].x * factor + saved[i].anchor * 640.f * (1.f-factor);
            float scale = saved[i].scale * factor;
            memcpy(saved[i].pane + 0x30, &x, sizeof x);
            memcpy(saved[i].pane + 0x48, &scale, sizeof scale);
            // PicturePane preparation copies these corner colors during this traversal.
            if (saved[i].fade_left) saved[i].pane[0xeb] = saved[i].pane[0xf3] = 0;
            saved[i].pane[0x58] |= 0x10;
        }
    }
    rt_dispatch(c);
    if (apply) {
        for (unsigned i = 0; i < count; i++) {
            memcpy(saved[i].pane + 0x30, &saved[i].x, sizeof saved[i].x);
            memcpy(saved[i].pane + 0x48, &saved[i].scale, sizeof saved[i].scale);
            if (saved[i].fade_left) {
                saved[i].pane[0xeb] = saved[i].alpha[0];
                saved[i].pane[0xf3] = saved[i].alpha[1];
            }
            // The current draw keeps its cached matrix/colors; the next traversal recalculates them.
            saved[i].pane[0x58] |= 0x10;
        }
    }
}
