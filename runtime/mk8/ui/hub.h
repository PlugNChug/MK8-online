// Included by the verified menu build copy. Prototype only; never fabricates service data.
extern "C" {
#include "net/nextendo.h"
#include "net/netcfg.h"
#include "net/mk8_social.h"
}

static bool mk8_hub_enabled() {
    const char *value = getenv("MK8_MENU_PROTOTYPE");
    return value && !strcmp(value, "1");
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

static void mk8_hub_track(float scale) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 185*scale;
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p,ImVec2(p.x+w,p.y+h),IM_COL32(24,29,33,255),16*scale);
    // A schematic circuit, not a preview of a commercial track.
    ImVec2 pts[97];
    for (int i=0;i<=96;++i) {
        float a = i*6.2831853f/96;
        pts[i] = ImVec2(p.x+w*.63f+cosf(a)*w*.26f,p.y+h*.5f+sinf(2*a)*h*.31f);
    }
    d->AddPolyline(pts,97,IM_COL32(53,61,68,255),0,22*scale);
    d->AddPolyline(pts,97,IM_COL32(255,119,74,255),0,2*scale);
    d->AddCircleFilled(pts[10],6*scale,IM_COL32(255,119,74,255));
    d->AddCircle(pts[10],11*scale,IM_COL32(255,119,74,130),24,2*scale);
    d->AddText(menu_font_big(),44*scale,ImVec2(p.x+24*scale,p.y+42*scale),IM_COL32(242,242,245,255),"De volta");
    d->AddText(menu_font_big(),44*scale,ImVec2(p.x+24*scale,p.y+91*scale),IM_COL32(242,242,245,255),"à pista.");
    ImGui::Dummy(ImVec2(w,h));
}

static void mk8_hub_draw(float scale) {
    static int page = 0, settings_page = 0;
    static bool initialized;
    if (!initialized) {
        const char *initial = getenv("MK8_MENU_PROTOTYPE_PAGE");
        if (initial) page = atoi(initial);
        if (page < 0 || page > 3) page = 0;
        initialized = true;
    }
    ImGuiIO &io = ImGui::GetIO();
    ImGuiStyle saved = ImGui::GetStyle();
    ImGuiStyle &style = ImGui::GetStyle();
    const ImVec4 orange(1.0f, .47f, .29f, 1), muted(.65f, .66f, .69f, 1);
    style.WindowPadding = ImVec2(32 * scale, 26 * scale);
    style.FramePadding = ImVec2(18 * scale, 12 * scale);
    style.ItemSpacing = ImVec2(14 * scale, 14 * scale);
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
    style.Colors[ImGuiCol_CheckMark] = orange;
    style.Colors[ImGuiCol_SliderGrab] = orange;
    style.Colors[ImGuiCol_NavCursor] = orange;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0,0), io.DisplaySize, IM_COL32(0,0,0,125));
    const float w = fminf(io.DisplaySize.x * .92f, 1560 * scale);
    const float h = fminf(io.DisplaySize.y * .88f, 900 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x/2,io.DisplaySize.y/2), ImGuiCond_Always, ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(w,h), ImGuiCond_Always);
    ImGui::PushFont(menu_font());
    ImGui::Begin("MK8 online##hub", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImVec2 brand = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(brand.x+21*scale,brand.y+22*scale),21*scale,IM_COL32(255,119,74,255));
    ImGui::GetWindowDrawList()->AddText(ImVec2(brand.x+12*scale,brand.y+1*scale),IM_COL32(18,21,24,255),"8");
    ImGui::Dummy(ImVec2(48*scale,44*scale)); ImGui::SameLine();
    ImGui::TextUnformatted("MK8 online");
    ImGui::SameLine(w-370*scale); ImGui::TextColored(muted, "Prévia do menu");
    ImGui::Separator();
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (!popup && !ui_text_busy()) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_E)) page = (page+1)%4;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_Q)) page = (page+3)%4;
    }
    const float body_h = ImGui::GetContentRegionAvail().y - 72 * scale;
    NxStatus account; nextendo_status(&account);
    const bool connected = account.state == NX_SIGNED_IN && net_backend() == NET_BACKEND_NEXTENDO;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10*scale,8*scale));
    ImGui::BeginChild("hub-navigation", ImVec2(280 * scale,body_h),ImGuiChildFlags_AlwaysUseWindowPadding);
    const char *tabs[] = {"Jogar", "Nextendo", "Social", "Ajustes"};
    for (int i=0; i<4; ++i) {
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button,page==i ? style.Colors[ImGuiCol_Header] : ImVec4(0,0,0,0));
        if (ImGui::Button("##section", ImVec2(ImGui::GetContentRegionAvail().x,66 * scale))) page=i;
        const ImVec2 pos = ImGui::GetItemRectMin();
        ImDrawList *d = ImGui::GetWindowDrawList();
        ImU32 ink = ImGui::GetColorU32(page == i ? orange : muted);
        mk8_hub_icon(d,ImVec2(pos.x+32*scale,pos.y+33*scale),scale,i,ink);
        d->AddText(ImVec2(pos.x+62*scale,pos.y+(66*scale-ImGui::GetFontSize())*.5f),ink,tabs[i]);
        if (page==i) d->AddRectFilled(ImVec2(pos.x+7*scale,pos.y+16*scale),ImVec2(pos.x+10*scale,pos.y+50*scale),ink,2*scale);
        if (i == page && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::Spacing(); ImGui::Separator();
    ImGui::SetCursorPosY(fmaxf(ImGui::GetCursorPosY(),body_h-180*scale));
    ImGui::TextColored(connected ? ImVec4(.48f,.82f,.65f,1) : muted,connected ? "Nextendo ativo" : "Sessão local");
    ImGui::TextWrapped("%s",account.state == NX_SIGNED_IN ? account.username : "Seu piloto");
    ImGui::TextColored(muted,"MK8 Deluxe 4.0.1");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SameLine(0,28*scale);
    ImGui::BeginChild("hub-content", ImVec2(0,body_h));
    static int last_page = -1;
    static float reveal = 1;
    if (last_page != page) { last_page = page; reveal = .55f; }
    reveal = fminf(1, reveal + io.DeltaTime*3);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, reveal);
    ImGui::PushFont(menu_font_big()); ImGui::TextUnformatted(tabs[page]); ImGui::PopFont();
    if (page == 0) {
        ImGui::TextColored(muted,"Sua partida, sem sair do jogo.");
        mk8_hub_track(scale);
        ImGui::TextWrapped(connected ? "Nextendo pronto nesta sessão. Acesse Online Play no menu do MK8 para encontrar uma corrida." : "Continue sua partida ou abra Nextendo para conferir a conexão.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, orange);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.08f,.08f,.08f,1));
        if (ImGui::Button("Voltar ao jogo", ImVec2(300*scale,64*scale))) set_open(false);
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Abrir Nextendo", ImVec2(300*scale,64*scale))) page=1;
        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextColored(muted,"Nesta sessão");
        ImGui::Text("Conta: %s", account.state == NX_SIGNED_IN ? account.username : "sem login Nextendo");
        ImGui::Text("Conteúdo: %s", getenv("MK8_ROMFS_OVERLAY") && *getenv("MK8_ROMFS_OVERLAY") ? "CTGP / mod local" : "jogo original");
    } else if (page == 1 || page == 2) {
        static Mk8Social social_state;
        if (connected) mk8_social_refresh();
        mk8_social_snapshot(&social_state);
        if (!connected) ImGui::TextWrapped("Entre no Nextendo pela aba Conta para consultar seus dados.");
        else if (social_state.busy) ImGui::TextColored(muted,"Atualizando dados do Nextendo...");
        else ImGui::TextColored(muted,"Dados do serviço. Atualização a cada 30 segundos nesta tela.");
        ImGui::Spacing();
        if (page == 1 && connected) {
            if (!social_state.listings_ok) ImGui::TextWrapped("%s",social_state.listings_error);
            else {
                ImGui::Text("%d jogadores online   |   %d salas",social_state.online_players,social_state.lobby_count);
                if (!social_state.lobby_count) ImGui::TextUnformatted("Nenhuma sala publicada neste momento.");
                static Mk8Lobby selected_room;
                bool details=false;
                if (social_state.lobby_count && ImGui::BeginTable("live-lobbies",3,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,ImVec2(0,210*scale))) {
                    ImGui::TableSetupColumn("Modo",ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Estado",ImGuiTableColumnFlags_WidthFixed,220*scale);
                    ImGui::TableSetupColumn("Jogadores",ImGuiTableColumnFlags_WidthFixed,180*scale);
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
                if (details) ImGui::OpenPopup("Sala Nextendo");
                if (ImGui::BeginPopupModal("Sala Nextendo",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted(selected_room.mode);
                    ImGui::Text("%d / %d jogadores",selected_room.count,selected_room.capacity);
                    ImGui::TextUnformatted("Listagem informativa. Para jogar, use Online Play no MK8.");
                    if (ImGui::Button("Fechar") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight,false) || ImGui::IsKeyPressed(ImGuiKey_Escape,false)) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
            ImGui::Separator();
            if (!social_state.lobby_ok) ImGui::TextWrapped("%s",social_state.lobby_error);
            else if (!social_state.in_lobby) ImGui::TextUnformatted("Você ainda não está em uma sala.");
            else {
                ImGui::TextColored(orange,"Sua sala atual");
                ImGui::Text("Sala %s",social_state.lobby_id);
                ImGui::Text("%s",social_state.lobby_mode);
                ImGui::Text("Jogadores: %d / %d",social_state.players,social_state.capacity);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram,orange);
                ImGui::ProgressBar((float)social_state.players/social_state.capacity,ImVec2(360*scale,8*scale),"");
                ImGui::PopStyleColor();
            }
            ImGui::Spacing(); ImGui::Separator();
            ImGui::TextWrapped("Nextendo conectado: volte ao MK8 e escolha Online Play para encontrar uma corrida. Este painel permite consultar as salas e sua conta.");
        } else if (page == 2 && connected) {
            if (!social_state.friends_ok) ImGui::TextWrapped("%s",social_state.friends_error);
            else {
                ImGui::Text("%d amigos   |   %d pedidos recebidos   |   %d enviados",social_state.count,social_state.requests,social_state.sent);
                if (!social_state.count) ImGui::TextUnformatted("Sua lista de amigos está vazia.");
                if (ImGui::BeginTable("friends",2,ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,ImVec2(0,300*scale))) {
                    ImGui::TableSetupColumn("Piloto",ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Presença",ImGuiTableColumnFlags_WidthFixed,260*scale);
                    ImGui::TableHeadersRow();
                    for (int i=0;i<social_state.count;++i) {
                        const Mk8Friend &f=social_state.friends[i];
                        ImGui::TableNextRow(0,72*scale); ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(f.name);
                        ImGui::TableSetColumnIndex(1);
                        const char *presence=f.presence==0?"Offline":f.presence==1?"Online":f.presence==2?"Jogando":"Não informada";
                        ImGui::TextColored(f.presence>0?ImVec4(.48f,.82f,.65f,1):muted,"%s",presence);
                    }
                    ImGui::EndTable();
                }
                ImGui::TextWrapped("Convites de corrida e entrada na partida de um amigo ainda não estão integrados ao MK8.");
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("Conta Nextendo")) settings_page=4, page=3;
        ImGui::SameLine(); if (ImGui::Button("Voltar ao MK8")) set_open(false);
    } else {
        const char *groups[] = {"Rápidos", "Vídeo", "Áudio", "Controle", "Conta"};
        for (int i=0;i<5;++i) {
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Button,settings_page==i ? ImVec4(.34f,.20f,.15f,1) : ImVec4(.11f,.13f,.15f,1));
            ImGui::PushStyleColor(ImGuiCol_Text,settings_page==i ? orange : muted);
            if (ImGui::Button(groups[i])) settings_page=i;
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextColored(muted,"Ajustes reais · salvos ao fechar o painel");
        if (settings_page == 0) {
            ImGui::Checkbox("Mostrar FPS", &g_settings.show_fps);
            ImGui::Checkbox("Mostrar memória utilizada", &g_settings.show_memory);
            if (ImGui::Checkbox("Tela cheia", &g_settings.fullscreen)) g_want_fullscreen_apply = true;
            ImGui::Checkbox("Silenciar em segundo plano", &g_settings.mute_in_background);
            float volume = g_settings.volume * 100;
            ImGui::SetNextItemWidth(400*scale);
            if (ImGui::SliderFloat("Volume", &volume,0,100,"%.0f%%")) g_settings.volume=volume/100;
            ImGui::TextWrapped("Vídeo, áudio e controle preservam os ajustes completos do menu atual.");
        } else if (settings_page == 1) tab_display();
        else if (settings_page == 2) tab_audio();
        else if (settings_page == 3) tab_about();
        else if (online_rows_available()) tab_online();
        else ImGui::TextWrapped("Nextendo não foi habilitado nesta execução. Abra pelo launcher de teste.");
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::TextColored(muted,"LB / RB ou Q / E: abas     A: selecionar     B / Esc: voltar ao jogo");
    if (!popup && !ui_text_busy() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight,false) || ImGui::IsKeyPressed(ImGuiKey_Escape,false))) set_open(false);
    ImGui::End(); ImGui::PopFont();
    ImGui::GetStyle() = saved;
    osk_draw(menu_font_big(),scale);
}
