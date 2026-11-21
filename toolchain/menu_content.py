"""Expose each shared page's innermost Back action to Native pointer navigation."""
from settings_menu import ROOT, replace


def prepare_stock_rows(runtime):
    ui = runtime/'ui'
    for name in ('settings_children.h','native_rows_host.h','menu_row_labels.h','native_rows.inc'):
        (ui/name).write_bytes((ROOT/'runtime/mk8/ui'/name).read_bytes())
    declarations = ui/'option_rows.h'
    signature = 'void opt_section(const char *title);'
    declarations.write_text(replace(declarations.read_text(encoding='utf-8'), signature,
                                    signature + '\nvoid opt_heading(const char *title);'), encoding='utf-8')
    rows = ui/'option_rows.mm'
    text = '#include "native_rows_host.h"\n' + rows.read_text(encoding='utf-8')
    for signature, code in (
        ('void opt_begin(const char *id, float scale)', 'g_next_disabled=false; ImGui::PushID(id); return;'),
        ('void opt_end(void)', 'ImGui::PopID(); return;'),
        ('void opt_section(const char *title)', 'mk8_collect_section(title); return;'),
        ('bool opt_text_editing(const char *label)', 'return false;'),
        ('bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc)',
         'return mk8_collect_text(ImGui::GetID(label),label,buf,cap,desc);'),
    ):
        marker = signature + ' {'
        text = replace(text,marker,marker+'\n    if (mk8_collecting_rows()) { '+code+' }')
    marker = '               bool arrows = false) {'
    text = replace(text,marker,marker+'''
    if (mk8_collecting_rows()) {
        const bool enabled=!g_next_disabled;
        g_next_disabled=g_held=false;
        return mk8_collect_row(ImGui::GetID(label),label,value.c_str(),desc,enabled,arrows,upper);
    }''')
    rows.write_text(text,encoding='utf-8')

    menu = ui/'menu.mm'
    text = '#include "native_rows_host.h"\n' + menu.read_text(encoding='utf-8')
    text = replace(text,'#include "mk8_hub.h"','#include "mk8_hub.h"\n#include "native_rows.inc"')
    text = replace(text,'if (!open && !kbd && !g_settings.show_fps','if (!open && !kbd && !mk8_native_rows_active() && !g_settings.show_fps')
    text = replace(text,'    ImGui::NewFrame();','    ImGui::NewFrame();\n    if (!kbd && mk8_native_rows_active()) mk8_native_rows_pump(scale);')
    text = replace(text,'void menu_toggle(void) { if (!g_open.load()', 'void menu_toggle(void) { if (mk8_collecting_rows()) return; if (!g_open.load()')
    text = replace(text,'mk8_quit_consumed_back=ask &&','mk8_quit_consumed_back=!mk8_collecting_rows() && ask &&')
    # The same game keyboard can clear a search filter. Stock name entry continues to require a nonempty name.
    sig = 'extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap)'
    text = replace(text,sig+' {','extern "C" void nvn_represent(void);\nstatic int mk8_keyboard_run(const char *initial,int max_len,char *out,int cap,bool allow_empty) {')
    text = replace(text,'    extern void nvn_represent(void);  // runtime/nvn/objects.c\n','')
    text = replace(text,'bool ok = g_kbd.load() == 2 && g_kbd_text[0];','bool ok = g_kbd.load() == 2 && (allow_empty || g_kbd_text[0]);')
    marker = '// The tab bar and its pages,'
    text = replace(text,marker,sig+' { return mk8_keyboard_run(initial,max_len,out,cap,false); }\n'
                   'static int mk8_native_text_keyboard(const char *initial,int max_len,char *out,int cap) { return mk8_keyboard_run(initial,max_len,out,cap,true); }\n\n'+marker)
    menu.write_text(text,encoding='utf-8')

    mods = ui/'mods_page.mm'
    text = mods.read_text(encoding='utf-8')
    marker = 'bool mods_page_used_back(void)'
    text = replace(text,marker,'''static void mod_info(const char *label,const char *value,const char *help,bool upper) {
    opt_disable_next();
    opt_action(label,value,help,upper);
}
''' + marker)
    # Only these option rows discard activation. File selection and download cancellation stay interactive.
    for first_arg,count in (
        ('g_note',1), ('"By"',2), ('"Category"',2), ('"Licence"',2), ('"Game files"',1),
        ('"Code patches"',1), ('usermods_crash_note()',1), ('"None yet"',1), ('"Downloads"',1),
        ('"File"',1), ('"Not for this game"',1), ('msg[0] ? msg : "Nothing found"',1),
    ):
        call='opt_action('+first_arg+','
        if text.count(call)!=count:
            raise ValueError('Unexpected number of mod information rows: '+first_arg)
        text=text.replace(call,'mod_info('+first_arg+',')
    text = replace(text,'static bool back_pressed(void) {','static bool back_pressed(void) {\n    if (mk8_collecting_rows()) return false;')
    text = replace(text,'extern "C" bool mk8_mods_detail_back(void) {','''extern "C" unsigned mk8_mods_depth(void) {
    return g_page==1 && g_mod>=0 ? (g_ask_remove ? 2 : 1) : g_page==2 && g_gb!=0 ? 1 : 0;
}
extern "C" bool mk8_mods_detail_back(void) {''')
    text = replace(text,'    opt_action(msg, val, st == GB_BUSY', '    if (st!=GB_BUSY) opt_disable_next();\n    bool selected=opt_action(msg, val, st == GB_BUSY')
    text = replace(text,'st == GB_BUSY && ImGui::IsItemClicked()', 'st == GB_BUSY && (mk8_collecting_rows() ? selected : ImGui::IsItemClicked())')
    text = replace(text,'if (ImGui::IsItemFocused() && !list[i].details)', 'if (!mk8_collecting_rows() && ImGui::IsItemFocused() && !list[i].details)')
    text = replace(text,'if (!have) gb_details(id);','if (!have || !m.details) gb_details(id);')
    text = replace(text,'    if (g_mod >= 0) { page_mod(g_mod); return; }','''    if (g_mod >= 0) {
        UserMod target;
        if (!mk8_collecting_rows() || !usermods_get(g_mod,&target)) { page_mod(g_mod); return; }
        ImGui::PushID(target.id); page_mod(g_mod); ImGui::PopID();
        return;
    }''')
    text = replace(text,'            ImGui::PushID(i);','            ImGui::PushID(m.id);')
    marker = 'static void page_mod(int i) {'
    text = replace(text,marker,'''static void confirm_remove(int i,const UserMod &m) {
    if (opt_action("Yes, remove it",m.name,"Deletes the mod's files from this device.")) {
        usermods_remove(i); g_mod=-1; g_ask_remove=false;
    } else if (mk8_collecting_rows() && opt_button("Cancel","Keep the mod installed.")) g_ask_remove=false;
}
''' + marker)
    text = replace(text,'''    } else if (opt_button("Yes, remove it", "Deletes the mod's files from this device.")) {
        usermods_remove(i);
        g_mod = -1, g_ask_remove = false;
    }''','    } else confirm_remove(i,m);')
    text = replace(text,'    opt_section(m.name);','''    if (!mk8_collecting_rows()) opt_section(m.name);
    else {
        mod_info("Mod",m.name,desc,false);
        if (g_ask_remove) {
            confirm_remove(i,m);
            return;
        }
    }''')
    mods.write_text(text.replace('opt_section(', 'opt_heading('),encoding='utf-8')


def prepare(runtime):
    header = ROOT/'runtime/mk8/ui/menu_exit.h'
    (runtime/'ui/menu_exit.h').write_bytes(header.read_bytes())
    menu = runtime / 'ui/menu.mm'
    text = menu.read_text(encoding='utf-8')
    start = text.index('static void tab_quit(float scale, bool in_game) {')
    end = text.index('    const char *lost', start)
    original = text[start:end]
    marker = '    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))'
    if original.count(marker) != 1:
        raise ValueError('Quit confirmation Back handling changed')
    text = replace(text, original, """#include "menu_exit.h"
static int mk8_quit_choice;
static bool mk8_quit_save_failed;
static bool mk8_quit_consumed_back;
static bool mk8_quit_detail_back(void) {
    bool pending=mk8_quit_choice!=0 || mk8_quit_consumed_back;
    mk8_quit_choice=0; mk8_quit_consumed_back=mk8_quit_save_failed=false;
    return pending;
}
static void tab_quit(float scale, bool in_game) {
    int &ask=mk8_quit_choice;
    mk8_quit_consumed_back=ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight,false) || ImGui::IsKeyPressed(ImGuiKey_Escape,false));
    if (mk8_quit_consumed_back) ask=0;
    if (!ask) mk8_quit_save_failed=false;
""")
    text = replace(text, 'if (opt_action(ask == 3 ?', 'if (mk8_menu_exit_ready(opt_action(ask == 3 ?')
    text = replace(text, 'if (opt_action("Cancel", "", "Stays where you are.")) ask = 0;',
                   'if (opt_action("Cancel", "", "Stays where you are.")) { ask=0; mk8_quit_consumed_back=true; }')
    text = replace(text, 'ask == 1 ? d1 : d2)) {\n            settings_save();',
                   'ask == 1 ? d1 : d2), mk8_quit_save_failed)) {')
    text = replace(text, '''    opt_section("About");
    opt_action("Version", app_version(), "This program's version (and build number, in an app). Please include it "
               "in bug reports.", false);
''', '')
    start = text.index('// "v0.5.3" and, in an app bundle')
    end = text.index('#include "menu_exit.h"',start)
    text = text[:start] + text[end:]
    menu.write_text(text,encoding='utf-8')

    mods = runtime / 'ui/mods_page.mm'
    text = mods.read_text(encoding='utf-8')
    marker = 'static bool g_ask_remove;'
    text = replace(text, marker, marker + """
extern "C" bool mk8_mods_detail_back(void) {
    if (g_page==1 && g_mod>=0) {
        if (g_ask_remove) g_ask_remove=false;
        else g_mod=-1;
        return true;
    }
    if (g_page==2 && g_gb) { g_gb=0; return true; }
    return false;
}""")
    for original in ('g_mod = -1, g_ask_remove = false, g_back_used = true;', 'g_gb = 0, g_back_used = true;'):
        text = replace(text, 'if (back_pressed()) { '+original+' return; }',
                       'if (back_pressed() && mk8_mods_detail_back()) { g_back_used=true; return; }')
    for action in ('g_mod = -1, g_ask_remove = false;', 'g_gb = 0;'):
        marker = '"Back to the list (B).", false)) { ' + action
        text = replace(text, marker, marker + ' g_back_used=true;')
    text = replace(text, '#include "option_rows.h"', '#include "option_rows.h"\n#include "menu_exit.h"')
    text = replace(text, '    if (opt_button("Restart the game to apply",',
                   '    static bool failed;\n    if (mk8_menu_exit_ready(opt_button("Restart the game to apply",')
    text = replace(text, 'since your last save is lost."))\n        rt_request_reboot(2), menu_close();',
                   'since your last save is lost."), failed))\n        rt_request_reboot(2), menu_close();')
    mods.write_text(text,encoding='utf-8')

    online = runtime/'ui/online_rows.mm'
    text = online.read_text(encoding='utf-8')
    text = replace(text,'''    if (opt_action("Nextendo account", val, ad)) {
#ifdef NX_SHEET
        if (st.state != NX_SIGNED_IN && st.state != NX_WAITING_BROWSER) sign_in();  // (the sheet may be up)
#else
        if (st.state != NX_SIGNED_IN) sign_in();
#endif
    }''','''    bool can_sign_in=st.state!=NX_SIGNED_IN;
#ifdef NX_SHEET
    can_sign_in=can_sign_in && st.state!=NX_WAITING_BROWSER;
#endif
    if (!can_sign_in) opt_disable_next();
    if (opt_action("Nextendo account",val,ad) && can_sign_in) sign_in();''')
    text = replace(text, '#include "option_rows.h"', '#include "option_rows.h"\n#include "imgui.h"\n#include "menu_exit.h"')
    text = replace(text, '        if (opt_action("Restart the game now",',
                   '        static bool failed;\n        if (mk8_menu_exit_ready(opt_action("Restart the game now",')
    text = replace(text, 'last save is lost."))\n            rt_request_reboot(1 /* RT_REBOOT_GAME */), menu_toggle();',
                   'last save is lost."), failed))\n            rt_request_reboot(1 /* RT_REBOOT_GAME */), menu_toggle();')
    online.write_text(text,encoding='utf-8')
    prepare_stock_rows(runtime)
