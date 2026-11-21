#include "imgui_internal.h"
#include <cassert>
#include <string>

static std::string native_frame() {
    ImGui::NewFrame();
    ImGui::LogToBuffer();
    mk8_hub_draw(.65f);
    std::string text=ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::Render();
    return text;
}
static void visible(const char *label) {
    std::string output=native_frame();
    if (output.find(label)==std::string::npos) {
        fprintf(stderr,"Missing %s in %s\n",label,output.c_str()); abort();
    }
}
static void click(const char *label) {
    target=label; position=ImVec2(-1,-1); native_frame(); assert(position.x>=0);
    ImGuiIO &io=ImGui::GetIO();
    io.AddMousePosEvent(position.x,position.y); native_frame();
    io.AddMouseButtonEvent(0,true); native_frame(); io.AddMouseButtonEvent(0,false); native_frame();
    target=nullptr;
}
static void press(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key,true); native_frame();
    ImGui::GetIO().AddKeyEvent(key,false); native_frame();
}
static void check_native_pages() {
    ImGuiIO &io=ImGui::GetIO();
    for (Mk8MenuLanguage lang : {MK8_MENU_EN,MK8_MENU_PT,MK8_MENU_ES}) {
        for (int status : {401,403,429,200,503}) {
            const char *error=mk8_nx_service_error(lang,status);
            assert(*error && ImTextCountCharsFromUtf8(error,nullptr)<=40);
        }
        assert(ImTextCountCharsFromUtf8(mk8_menu_label(lang,mk8_nx_refresh_interval),nullptr)<=40);
    }
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags|=ImGuiBackendFlags_HasGamepad;
    click("Open Nextendo");
    visible("Choose Play on Nextendo");
    online=true; visible("Check your account connection.");
    state.state=NX_SIGNED_IN; strcpy(state.username,"Test racer");
    social.busy=true; visible("Updating Nextendo data");
    social.busy=false; social.listings_ok=social.lobby_ok=true;
    visible("No rooms are listed"); visible("joined a room yet");
    social.listings_ok=false; strcpy(social.listings_error,"Room list unavailable"); visible(social.listings_error);
    social.listings_ok=true; social.lobby_count=1;
    strcpy(social.lobbies[0].mode,"VS lobby"); social.lobbies[0].count=2; social.lobbies[0].capacity=12;
    click("VS lobby"); visible("To join a race");
    press(ImGuiKey_Escape); assert(!closed);
    static Mk8Social snapshot;
    bool connected,account_available;
    strcpy(social.lobbies[0].id,"room-2");
    social.lobby_count=2; strcpy(social.lobbies[1].id,"room-1");
    int before=refreshes;
    mk8_menu_nextendo_snapshot(&snapshot,&connected,&account_available);
    assert(connected && account_available && refreshes==before+1);
    assert(snapshot.lobby_count==2 && mk8_lobby_index(&snapshot,"room-1")==1);
    std::swap(social.lobbies[0],social.lobbies[1]);
    mk8_menu_nextendo_snapshot(&snapshot,&connected,&account_available);
    assert(mk8_lobby_index(&snapshot,"room-1")==0);
    strcpy(snapshot.lobbies[1].id,"room-1");
    assert(mk8_lobby_index(&snapshot,"room-1")==-1 && mk8_lobby_index(&snapshot,"")==-1);
    snapshot.lobby_count=129; assert(mk8_lobby_index(&snapshot,"room-1")==-1);
    snapshot.lobby_count=128; strcpy(snapshot.lobbies[127].id,"last"); assert(mk8_lobby_index(&snapshot,"last")==127);
    snapshot.listings_ok=false; assert(mk8_lobby_index(&snapshot,"last")==-1);
    state.state=NX_SIGNED_OUT; before=refreshes;
    mk8_menu_nextendo_snapshot(&snapshot,&connected,&account_available);
    assert(!connected && account_available && refreshes==before && snapshot.lobby_count==0 && !snapshot.lobbies[0].id[0]);
    online=false;
    mk8_menu_nextendo_snapshot(&snapshot,&connected,&account_available);
    assert(!connected && !account_available && refreshes==before);
    online=true; state.state=NX_SIGNED_IN;
    click("Friends");
    social.friends_ok=true; visible("Your friends list is empty");
    social.count=1; social.requests=2; strcpy(social.friends[0].name,"Local fixture"); social.friends[0].presence=2;
    visible("Local fixture"); visible("Playing");
    social.friends_ok=false; strcpy(social.friends_error,"Service unavailable"); visible(social.friends_error);
    click("Nextendo Account"); native_frame(); assert(account_frames);
    press(ImGuiKey_Escape); visible(social.friends_error); // Account → Friends, then Back to Play.
    press(ImGuiKey_Escape); visible("Back in the game, select Online Play.");
    visible("Back in the game, select Online Play."); visible("Account: Test racer"); visible("Nextendo active");
    click("Back to game"); assert(closed);
    online=false;
    visible("Local session"); visible("Account: Test racer"); visible("Use Play on Nextendo in the launcher.");
    memset(state.username,'W',sizeof state.username-1);
    state.username[sizeof state.username-1]=0;
    Mk8PlaySession session;
    mk8_menu_play_session(&session);
    assert(!session.connected && session.signed_in && session.modded);
    assert(!strcmp(session.username,state.username));
    visible("Content: CTGP / local mod");
    for (bool connected : {false,true}) {
        session.connected=connected;
        for (Mk8MenuLanguage lang : {MK8_MENU_EN,MK8_MENU_PT,MK8_MENU_ES}) {
            for (auto field : {PLAY_SESSION,PLAY_ACCOUNT,PLAY_CONTENT,PLAY_INSTRUCTION,PLAY_VERSION}) {
                const char *value=mk8_menu_play_text(lang,&session,field);
                assert(*value && strlen(value)<80); // Guest text pane input capacity.
                if (field==PLAY_INSTRUCTION) assert(strlen(value)<=40); // Stock icon tip holds 40 UTF-16 units.
            }
        }
    }
    state.state=NX_SIGNED_OUT;
    visible("Account: signed out of Nextendo"); visible("MK8 Deluxe 4.0.1");
    click("Open Nextendo"); visible("Choose Play on Nextendo");
    press(ImGuiKey_Q); // Return to Play for the next fixture.
}

static void check_quit_back() {
    closed=false;
    native_frame();
    press(ImGuiKey_Q);  // Play wraps back to Settings.
    click("Quit");
    for (bool use_nes : {false,true}) {
        nes=use_nes;
        closed=false;
        click("Quit to Desktop");
        visible("Yes, quit to Desktop");
        ImGuiKey key=use_nes ? ImGuiKey_GamepadFaceRight : ImGuiKey_Escape;
        press(key);
        assert(!closed && mk8_quit_choice==0);
        visible("Quit to Start Screen");
        press(key);
        assert(closed);
    }
}
