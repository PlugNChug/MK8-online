// Included by the verified in-game menu build copy.
#include "menu_language.h"
#include "settings_catalog.h"
#include "menu_launcher_actions.h"
#include "menu_play.h"
#include "menu_nextendo.h"
extern "C" {
#include "net/nextendo.h"
#include "net/netcfg.h"
#include "net/mk8_social.h"
}

extern "C" void mk8_menu_play_session(Mk8PlaySession *out) {
    NxStatus account;
    nextendo_status(&account);
    out->signed_in = account.state == NX_SIGNED_IN;
    out->connected = out->signed_in && net_backend() == NET_BACKEND_NEXTENDO;
    const char *overlay = getenv("MK8_ROMFS_OVERLAY");
    out->modded = overlay && *overlay;
    snprintf(out->username, sizeof out->username, "%s", out->signed_in ? account.username : "");
}

static void mk8_read_social(Mk8Social *out,bool connected) {
    if (connected) {
        mk8_social_refresh();
        mk8_social_snapshot(out);
    } else memset(out,0,sizeof *out);
}

extern "C" void mk8_menu_nextendo_snapshot(Mk8Social *out,bool *connected,bool *account_available) {
    Mk8PlaySession session;
    mk8_menu_play_session(&session);
    *connected=session.connected;
    *account_available=online_rows_available();
    mk8_read_social(out,*connected);
}

static bool mk8_hub_enabled() {
    const char *value = getenv("MK8_MENU_PROTOTYPE");
    return !value || strcmp(value, "0");
}

static const char *hub_text(const char *pt, const char *en, const char *es) { return mk8_ui_text(pt, en, es); }

static void mk8_nextendo_start_notice() {
    static bool checked;
    if (checked) return;
    checked = true;
    const char *mii = getenv("MK8_MII_NAME");
    char account[64], message[256];
    if (!mii || !*mii || !nextendo_pid() || net_backend() != NET_BACKEND_NEXTENDO) return;
    bool named = nextendo_username(account, sizeof account);
    snprintf(message, sizeof message, "%s%s%s  ·  Mii: %s",
             hub_text("Jogando via Nextendo", "Playing on Nextendo", "Jugando en Nextendo"),
             named ? "  ·  " : "", named ? account : "", mii);
    menu_notice(message, 7);
}

static void mk8_draw_launcher_actions(Mk8MenuLanguage language) {
    for (const Mk8MenuLauncherAction &action : mk8_menu_launcher_actions)
        if (action.available() && ImGui::Button(mk8_menu_label(language,action.label))) action.open();
}

static void mk8_hub_icon(ImDrawList *d, ImVec2 p, float s, int kind, ImU32 ink) {
    const float t = 2 * s;
    if (kind == 0) {
        d->AddTriangle(ImVec2(p.x-7*s,p.y-10*s), ImVec2(p.x+10*s,p.y), ImVec2(p.x-7*s,p.y+10*s),ink,t);
    } else if (kind == 1) {
        d->AddCircle(p,12*s,ink,24,t);
        d->AddEllipse(p,ImVec2(5*s,12*s),ink,0,24,t);
        d->AddLine(ImVec2(p.x-11*s,p.y),ImVec2(p.x+11*s,p.y),ink,t);
    } else if (kind == 2) {
        d->AddCircle(ImVec2(p.x,p.y-6*s),5*s,ink,16,t);
        d->AddBezierCubic(ImVec2(p.x-11*s,p.y+11*s),ImVec2(p.x-11*s,p.y-3*s),ImVec2(p.x+11*s,p.y-3*s),ImVec2(p.x+11*s,p.y+11*s),ink,t);
    } else {
        for (int i=0;i<3;++i) {
            float y=p.y+(i-1)*9*s, x=p.x+(i==1?5:-4)*s;
            d->AddLine(ImVec2(p.x-12*s,y),ImVec2(p.x+12*s,y),ink,t);
            d->AddCircleFilled(ImVec2(x,y),3*s,ink);
        }
    }
}

static void mk8_hub_draw(float scale) {
    static const Mk8MenuLanguage language = mk8_menu_language();
    auto tr = [](const char *en, const char *pt, const char *es) {
        return mk8_menu_text(language, en, pt, es);
    };
    static int page = MENU_PLAY, settings_page = MENU_QUICK;
    static int account_return=-1;
    static bool nextendo_from_play,focus_account,focus_play,reset_room_scroll,reset_friend_scroll;
    auto open_home_page=[&](int destination) {
        page=destination; account_return=-1; nextendo_from_play=focus_account=focus_play=false;
        reset_room_scroll=reset_friend_scroll=true;
    };
    static bool initialized;
    if (!initialized) {
        const char *initial = getenv("MK8_MENU_PROTOTYPE_PAGE");
        if (initial) page = atoi(initial);
        if (page < 0 || page >= MENU_PAGE_COUNT) page = 0;
        initialized = true;
    }
    ImGuiIO &io = ImGui::GetIO();
    ImGuiStyle saved = ImGui::GetStyle();
    ImGuiStyle &style = ImGui::GetStyle();
    const bool light = mk8_menu_light();
    const ImVec4 online = light ? ImVec4(.08f,.36f,.21f,1) : ImVec4(.48f,.82f,.65f,1);
    const ImVec4 accent = (light || nes_theme()) ? theme_palette().accent : ImVec4(1, .47f, .29f, 1);
    const ImVec4 muted = (light || nes_theme()) ? theme_palette().muted : ImVec4(.65f, .66f, .69f, 1);
    style.WindowPadding = ImVec2(32 * scale, 26 * scale);
    style.FramePadding = ImVec2(18 * scale, 12 * scale);
    style.ItemSpacing = ImVec2(14 * scale, 14 * scale);
    if (!nes_theme() && !light) {
        style.WindowRounding = 20 * scale;
        style.FrameRounding = 8 * scale;
        style.ChildRounding = 12 * scale;
        style.WindowBorderSize = 1;
        style.Colors[ImGuiCol_WindowBg] = ImVec4(.043f,.055f,.063f,.98f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0,0,0,0);
        style.Colors[ImGuiCol_Text] = ImVec4(.96f,.96f,.97f,1);
        style.Colors[ImGuiCol_TextDisabled] = muted;
        style.Colors[ImGuiCol_Border] = ImVec4(.21f,.21f,.22f,1);
        style.Colors[ImGuiCol_Separator] = ImVec4(.17f,.20f,.22f,1);
        style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(.10f,.12f,.14f,1);
        style.Colors[ImGuiCol_Button] = ImVec4(.15f,.15f,.16f,1);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.36f,.22f,.18f,1);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(.48f,.26f,.19f,1);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(.15f,.15f,.16f,1);
        style.Colors[ImGuiCol_Header] = ImVec4(.26f,.17f,.14f,1);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(.34f,.21f,.16f,1);
        style.Colors[ImGuiCol_CheckMark] = accent;
        style.Colors[ImGuiCol_SliderGrab] = accent;
        style.Colors[ImGuiCol_NavCursor] = accent;
    }
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0,0), io.DisplaySize, IM_COL32(0,0,0,125));
    const float w = fminf(io.DisplaySize.x * .92f, 1560 * scale);
    const float h = fminf(io.DisplaySize.y * .88f, 900 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x/2,io.DisplaySize.y/2), ImGuiCond_Always, ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(w,h), ImGuiCond_Always);
    ImGui::PushFont(!nes_theme() ? g_font : menu_font());
    ImGui::Begin("MK8 online##hub", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    const bool typing = ui_text_busy();
    float body_h=0;
    Mk8PlaySession session;
    mk8_menu_play_session(&session);
    const bool connected = session.connected;
    auto play_text = [&](Mk8PlayField field) { return mk8_menu_play_text(language,&session,field); };
    ImVec2 brand = ImGui::GetCursorScreenPos();
    const ImVec2 digit = ImGui::CalcTextSize("8");
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(brand.x+21*scale,brand.y+22*scale),21*scale,IM_COL32(255,119,74,255));
    ImGui::GetWindowDrawList()->AddText(ImVec2(brand.x+21*scale-digit.x*.5f,brand.y+22*scale-digit.y*.5f),IM_COL32(18,21,24,255),"8");
    ImGui::Dummy(ImVec2(48*scale,44*scale)); ImGui::SameLine();
    ImGui::TextUnformatted("MK8 online");
    ImGui::Separator();
    if (!popup && !typing) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1,false) || ImGui::IsKeyPressed(ImGuiKey_E,false)) open_home_page(mk8_home_pages[(mk8_home_row(page)+1)%HOME_COUNT]);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1,false) || ImGui::IsKeyPressed(ImGuiKey_Q,false)) open_home_page(mk8_home_pages[(mk8_home_row(page)+HOME_COUNT-1)%HOME_COUNT]);
    }
    body_h=ImGui::GetContentRegionAvail().y - 72*scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10*scale,8*scale));
    float navigation_width=fminf(280*scale,w*.24f);
    if (nes_theme()) {
        float needed=ImGui::CalcTextSize(play_text(PLAY_SESSION)).x+22*scale;
        for (int i: mk8_home_pages)
            needed=fmaxf(needed,ImGui::CalcTextSize(mk8_menu_label(language,mk8_menu_pages[i])).x+92*scale);
        navigation_width=fminf(w*.34f,fmaxf(navigation_width,needed));
    }
    ImGui::BeginChild("hub-navigation", ImVec2(navigation_width,body_h),ImGuiChildFlags_AlwaysUseWindowPadding);
    for (unsigned row=0; row<HOME_COUNT; ++row) {
        const int i=mk8_home_pages[row];
        const bool current=row==mk8_home_row(page);
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button,current ? style.Colors[ImGuiCol_Header] : ImVec4(0,0,0,0));
        if (ImGui::Button("##section", ImVec2(ImGui::GetContentRegionAvail().x,66 * scale))) open_home_page(i);
        const ImVec2 pos = ImGui::GetItemRectMin();
        ImDrawList *d = ImGui::GetWindowDrawList();
        const ImVec4 active_ink = nes_theme() && !light ? theme_palette().child : accent;
        ImU32 ink = ImGui::GetColorU32(current ? active_ink : muted);
        mk8_hub_icon(d,ImVec2(pos.x+32*scale,pos.y+33*scale),scale,i,ink);
        d->AddText(ImVec2(pos.x+62*scale,pos.y+(66*scale-ImGui::GetFontSize())*.5f),ink,mk8_menu_label(language,mk8_menu_pages[i]));
        if (current) d->AddRectFilled(ImVec2(pos.x+7*scale,pos.y+16*scale),ImVec2(pos.x+10*scale,pos.y+50*scale),ink,2*scale);
        if (current && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::Spacing(); ImGui::Separator();
    ImGui::SetCursorPosY(fmaxf(ImGui::GetCursorPosY(),body_h-180*scale));
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(connected ? online : muted,"%s",play_text(PLAY_SESSION));
    ImGui::PopTextWrapPos();
    ImGui::TextWrapped("%s",session.signed_in ? session.username : tr("Your racer", "Seu piloto", "Tu piloto"));
    ImGui::PushTextWrapPos(0);
        const char *version=play_text(PLAY_VERSION), *number=strrchr(version,' ');
        if (number && ImGui::CalcTextSize(version).x>ImGui::GetContentRegionAvail().x)
            ImGui::TextColored(muted,"%.*s\n%s",int(number-version),version,number+1);
        else ImGui::TextColored(muted,"%s",version);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SameLine(0,28*scale);
    const char *body_id=page==MENU_NEXTENDO ? "hub-rooms" : page==MENU_SOCIAL ? "hub-friends" : "hub-content";
    ImGui::BeginChild(body_id, ImVec2(0,body_h));
    static int last_page = -1;
    static float reveal = 1;
    if (last_page != page) { last_page = page; reveal = .55f; }
    reveal = fminf(1, reveal + io.DeltaTime*3);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, reveal);
    ImGui::PushFont(nes_theme() ? menu_font_big() : g_font_big);
    ImGui::TextUnformatted(mk8_menu_label(language,mk8_menu_pages[mk8_home_pages[mk8_home_row(page)]]));
    ImGui::PopFont();
    if (page == MENU_PLAY) {
        ImGui::TextWrapped("%s",play_text(PLAY_INSTRUCTION));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, light ? style.Colors[ImGuiCol_Header] : accent);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.08f,.08f,.08f,1));
        if (ImGui::Button(mk8_menu_label(language,mk8_play_back), ImVec2(300*scale,64*scale))) set_open(false);
        ImGui::PopStyleColor(2);
        if (ImGui::GetContentRegionAvail().x > 630*scale) ImGui::SameLine();
        if(focus_play) { ImGui::SetKeyboardFocusHere(); focus_play=false; }
        if (ImGui::Button(mk8_menu_label(language,mk8_play_open_nextendo), ImVec2(300*scale,64*scale))) {
            open_home_page(MENU_NEXTENDO); nextendo_from_play=true;
        }
        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextColored(muted,"%s",tr("This session", "Nesta sessão", "Esta sesión"));
        ImGui::Text("%s: %s",mk8_menu_label(language,mk8_play_account_label),play_text(PLAY_ACCOUNT));
        ImGui::Text("%s: %s",mk8_menu_label(language,mk8_play_content_label),play_text(PLAY_CONTENT));
        mk8_draw_launcher_actions(language);
    } else if (page == MENU_NEXTENDO || page == MENU_SOCIAL) {
        for (int view=0;view<2;view++) {
            if(view) ImGui::SameLine();
            int destination=view ? MENU_SOCIAL : MENU_NEXTENDO;
            ImGui::PushStyleColor(ImGuiCol_Button,page==destination ? style.Colors[ImGuiCol_Header] : style.Colors[ImGuiCol_Button]);
            if(ImGui::Button(mk8_menu_label(language,mk8_nx_views[view]))) page=destination;
            ImGui::PopStyleColor();
        }
        static Mk8Social social_state;
        mk8_read_social(&social_state,connected);
        if (!connected) ImGui::TextWrapped("%s",mk8_menu_label(language,online_rows_available() ? mk8_nx_check_account : mk8_nx_launcher_signin));
        else if (social_state.busy) ImGui::TextColored(muted,"%s",mk8_menu_label(language,mk8_nx_loading));
        else ImGui::TextColored(muted,"%s",mk8_menu_label(language,mk8_nx_refresh_interval));
        ImGui::Spacing();
        if (page == MENU_NEXTENDO && connected) {
            if (!social_state.listings_ok) ImGui::TextWrapped("%s",social_state.listings_error);
            else {
                ImGui::Text(tr("%d players online   |   %d rooms", "%d jogadores online   |   %d salas", "%d jugadores en línea   |   %d salas"),social_state.online_players,social_state.lobby_count);
                if (!social_state.lobby_count) ImGui::TextUnformatted(mk8_menu_label(language,mk8_nx_empty));
                static Mk8Lobby selected_room;
                bool details=false;
                if (reset_room_scroll && social_state.lobby_count) {
                    ImGui::SetNextWindowScroll(ImVec2(0,0)); reset_room_scroll=false;
                }
                if (social_state.lobby_count && ImGui::BeginTable("live-lobbies",3,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,ImVec2(0,210*scale))) {
                    ImGui::TableSetupColumn(mk8_menu_label(language,mk8_nx_mode),ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn(mk8_menu_label(language,mk8_nx_status),ImGuiTableColumnFlags_WidthFixed,220*scale);
                    ImGui::TableSetupColumn(mk8_menu_label(language,mk8_nx_players),ImGuiTableColumnFlags_WidthFixed,180*scale);
                    ImGui::TableHeadersRow();
                    for (int i=0;i<social_state.lobby_count;++i) {
                        const Mk8Lobby &room=social_state.lobbies[i];
                        ImGui::PushID(i);
                        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                        if (ImGui::Selectable(room.mode,false,ImGuiSelectableFlags_SpanAllColumns,ImVec2(0,48*scale))) {
                            selected_room=room; details=true;
                        }
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(room.state);
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%d / %d",room.count,room.capacity);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                const char *room_title=mk8_menu_label(language,mk8_nx_room);
                if (details) ImGui::OpenPopup(room_title);
                if (ImGui::BeginPopupModal(room_title,nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted(selected_room.mode);
                    ImGui::Text(tr("%d / %d players", "%d / %d jogadores", "%d / %d jugadores"),selected_room.count,selected_room.capacity);
                    ImGui::TextUnformatted(mk8_menu_label(language,mk8_nx_online_play));
                    if (ImGui::Button(tr("Close", "Fechar", "Cerrar")) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight,false) || ImGui::IsKeyPressed(ImGuiKey_Escape,false)) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
            ImGui::Separator();
            if (!social_state.lobby_ok) ImGui::TextWrapped("%s",social_state.lobby_error);
            else if (!social_state.in_lobby) ImGui::TextUnformatted(mk8_menu_label(language,mk8_nx_no_current_room));
            else {
                ImGui::TextColored(accent,"%s",mk8_menu_label(language,mk8_nx_current_room));
                ImGui::Text(tr("Room %s", "Sala %s", "Sala %s"),social_state.lobby_id);
                ImGui::Text("%s",social_state.lobby_mode);
                ImGui::Text(tr("Players: %d / %d", "Jogadores: %d / %d", "Jugadores: %d / %d"),social_state.players,social_state.capacity);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram,accent);
                ImGui::ProgressBar((float)social_state.players/social_state.capacity,ImVec2(360*scale,8*scale),"");
                ImGui::PopStyleColor();
            }
            ImGui::Spacing(); ImGui::Separator();
            ImGui::TextWrapped("%s",tr("Nextendo is connected. Return to the game and select Online Play to find a race. You can browse rooms and check your account here.", "Nextendo conectado: volte ao jogo e escolha Online Play para encontrar uma corrida. Este painel permite consultar as salas e sua conta.", "Nextendo está conectado. Vuelve al juego y selecciona Juego en línea para buscar una carrera. Aquí puedes consultar las salas y tu cuenta."));
        } else if (page == MENU_SOCIAL && connected) {
            if (!social_state.friends_ok) ImGui::TextWrapped("%s",social_state.friends_error);
            else {
                ImGui::Text(tr("%d friends   |   %d incoming requests   |   %d sent", "%d amigos   |   %d pedidos recebidos   |   %d enviados", "%d amigos   |   %d solicitudes recibidas   |   %d enviadas"),social_state.count,social_state.requests,social_state.sent);
                if (!social_state.count) ImGui::TextUnformatted(mk8_menu_label(language,mk8_nx_friends_empty));
                if (reset_friend_scroll) { ImGui::SetNextWindowScroll(ImVec2(0,0)); reset_friend_scroll=false; }
                if (ImGui::BeginTable("friends",2,ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,ImVec2(0,300*scale))) {
                    ImGui::TableSetupColumn(tr("Racer", "Piloto", "Piloto"),ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn(tr("Presence", "Presença", "Presencia"),ImGuiTableColumnFlags_WidthFixed,260*scale);
                    ImGui::TableHeadersRow();
                    for (int i=0;i<social_state.count;++i) {
                        const Mk8Friend &f=social_state.friends[i];
                        ImGui::TableNextRow(0,72*scale); ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(f.name);
                        ImGui::TableSetColumnIndex(1);
                        const char *presence=mk8_nx_presence(language,f.presence);
                        ImGui::TextColored(f.presence>0?online:muted,"%s",presence);
                    }
                    ImGui::EndTable();
                }
                ImGui::TextWrapped("%s",tr("Race invites and joining a friend's game are currently unavailable in MK8.", "Convites de corrida e entrada na partida de um amigo ainda não estão integrados ao MK8.", "Las invitaciones a carreras y la opción de unirte a la partida de un amigo todavía no están disponibles en MK8."));
            }
        }
        ImGui::Spacing();
        if (online_rows_available()) {
            if(focus_account) { ImGui::SetKeyboardFocusHere(); focus_account=false; }
            if (ImGui::Button(mk8_menu_label(language,mk8_nx_account))) {
                account_return=page; settings_page=MENU_ACCOUNT; page=MENU_SETTINGS;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(mk8_menu_label(language,mk8_play_back))) set_open(false);
    } else {
        for (int i=0;i<MENU_GROUP_COUNT;++i) {
            const char *name=mk8_menu_label(language,mk8_menu_groups[i]);
            if (i && ImGui::GetItemRectMax().x + style.ItemSpacing.x + ImGui::CalcTextSize(name).x + 2*style.FramePadding.x <
                ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Button,style.Colors[settings_page==i ? ImGuiCol_Header : ImGuiCol_Button]);
            const ImVec4 active_ink = nes_theme() && !light ? theme_palette().child : accent;
            ImGui::PushStyleColor(ImGuiCol_Text,settings_page==i ? active_ink : muted);
            if (ImGui::Button(name)) { settings_page=i; if(i!=MENU_ACCOUNT) account_return=-1; }
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextColored(muted,"%s",tr("Settings are saved when you close the menu.", "Ajustes reais · salvos ao fechar o painel", "Los ajustes se guardan al cerrar el menú."));
        if (settings_page == MENU_QUICK) {
            for (const auto &row : mk8_menu_quick) {
                const Mk8MenuOption *o=mk8_menu_option(row.id);
                const char *label=tr(o->label,row.pt,row.es);
                if (row.id==MK8_MENU_VOLUME) {
                    int volume=mk8_settings_volume_percent();
                    ImGui::SetNextItemWidth(400*scale);
                    if (ImGui::SliderInt(label,&volume,MK8_VOLUME_MIN,MK8_VOLUME_MAX,"%d%%",ImGuiSliderFlags_AlwaysClamp))
                        mk8_settings_set_volume_percent(volume);
                } else if (ImGui::Checkbox(label,o->toggle) && row.id==MK8_MENU_FULLSCREEN)
                    g_want_fullscreen_apply=true;
            }
            ImGui::TextWrapped("%s",tr("Open Video, Audio or Controls for more settings.", "Vídeo, áudio e controle preservam os ajustes completos do menu atual.", "Abre Vídeo, Audio o Controles para ver más ajustes."));
        } else if (settings_page == MENU_VIDEO) tab_display();
        else if (settings_page == MENU_AUDIO) tab_audio();
        else if (settings_page == MENU_CONTROLS) tab_about();
        else if (settings_page == MENU_MODS) tab_mods();
        else if (settings_page == MENU_QUIT) tab_quit(scale,true);
        else if (online_rows_available()) tab_online();
        else ImGui::TextWrapped("%s",tr("Choose Play on Nextendo in the launcher, then sign in through your browser.", "No launcher, selecione Jogar no Nextendo e faça login pelo navegador.", "Selecciona Jugar en Nextendo en el lanzador e inicia sesión en el navegador."));
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(muted,"%s",tr("LB / RB or Q / E: switch tabs     A: select     B / Esc: back to game", "LB / RB ou Q / E: abas     A: selecionar     B / Esc: voltar ao jogo", "LB / RB o Q / E: cambiar pestaña     A: seleccionar     B / Esc: volver al juego"));
    ImGui::PopTextWrapPos();
    const bool back_used = mods_page_used_back() ||
        (page==MENU_SETTINGS && settings_page==MENU_QUIT && mk8_quit_consumed_back);
    const bool back_key=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight,false) || ImGui::IsKeyPressed(ImGuiKey_Escape,false));
    if (!popup && !typing && !ui_text_busy() && !back_used && back_key) {
        if (!(page==MENU_SETTINGS && settings_page==MENU_QUIT && mk8_quit_detail_back())) {
            if(page==MENU_SETTINGS && settings_page==MENU_ACCOUNT && account_return>=0) {
                page=account_return; account_return=-1; focus_account=true;
            } else if((page==MENU_NEXTENDO || page==MENU_SOCIAL) && nextendo_from_play) {
                open_home_page(MENU_PLAY); focus_play=true;
            } else set_open(false);
        }
    }
    ImGui::End(); ImGui::PopFont();
    ImGui::GetStyle() = saved;
    osk_draw(menu_font_big(),scale);
}
