// Native Nextendo and Social share one snapshot and the stock scrolling list.
static void draw(Cpu *c);
static void start_exit(Cpu *c,bool resume);
static void menu_back_feedback(Cpu *c);

enum ServiceRowKind { LISTED_ROOM, CURRENT_ROOM, LISTED_FRIEND, SERVICE_REFRESH, SERVICE_ACCOUNT, SERVICE_RESUME };
struct ServiceRow { enum ServiceRowKind kind; bool enabled; };
static Mk8Social service_state,service_incoming;
static struct ServiceRow service_rows[sizeof service_state.friends/sizeof *service_state.friends+4];
static unsigned service_count;
static enum MenuLevel service_parent;
static bool service_connected,account_available,detail_current;
static char detail_id[sizeof service_state.lobby_id];
static Mk8Lobby detail_snapshot;
static unsigned listing_revision;
static uint64_t service_checked_at;

static bool friend_row_focused(void) {
    return menu_level==MENU_FRIENDS && focus<service_count && service_rows[focus].kind==LISTED_FRIEND;
}

static unsigned find_room_row(enum ServiceRowKind kind,const char *id) {
    if(kind!=LISTED_ROOM) for(unsigned i=0;i<service_count;i++)
        if(service_rows[i].kind==kind && (kind!=CURRENT_ROOM || !strcmp(service_state.lobby_id,id))) return i;
    if(service_connected && (kind==LISTED_ROOM || kind==CURRENT_ROOM)) {
        int index=mk8_lobby_index(&service_state,id);
        if(index>=0) return (unsigned)index;
    }
    // A vanished room must not turn a pending confirmation into a different room.
    for(unsigned i=0;i<service_count;i++) if(service_rows[i].kind==SERVICE_REFRESH) return i;
    return service_count-1;
}

static int unique_friend(uint64_t pid) {
    if(!pid) return -1;
    int match=-1;
    for(unsigned i=0;i<service_count && service_rows[i].kind==LISTED_FRIEND;i++) {
        if(service_state.friends[i].pid!=pid) continue;
        if(match>=0) return -1;
        match=(int)i;
    }
    return match;
}

static bool service_has_input;
struct ServicePosition { bool set, touched; unsigned offset, row, listing_revision; enum ServiceRowKind kind; uint64_t pid; char room[sizeof detail_id]; };
static struct ServicePosition positions[2],detail_position;
static enum MenuLevel account_source;

static struct ServicePosition service_position(void) {
    struct ServicePosition pos={0};
    if(focus>=service_count || service_header) return pos;
    enum ServiceRowKind kind=service_rows[focus].kind;
    // When the initial fetch finishes, reveal the first friend/room instead of chasing Refresh to the bottom.
    if(!service_has_input && (!service_rows[focus].enabled || kind>=SERVICE_REFRESH)) return pos;
    pos.touched=service_has_input;
    pos.set=true; pos.offset=offset; pos.kind=service_rows[focus].kind;
    if(pos.kind==LISTED_FRIEND && unique_friend(service_state.friends[focus].pid)==(int)focus)
        pos.pid=service_state.friends[focus].pid;
    if(pos.kind==LISTED_ROOM) {
        const char *id=service_state.lobbies[focus].id;
        if(mk8_lobby_index(&service_state,id)==(int)focus) snprintf(pos.room,sizeof pos.room,"%s",id);
        pos.row=focus; pos.listing_revision=listing_revision;
    }
    if(pos.kind==CURRENT_ROOM) snprintf(pos.room,sizeof pos.room,"%s",service_state.lobby_id);
    return pos;
}

static void restore_position(const struct ServicePosition *pos) {
    offset=pos->offset; service_has_input=pos->touched;
    if(!pos->set) { focus=0; return; }
    if(pos->kind==LISTED_FRIEND) {
        int match=unique_friend(pos->pid);
        focus=match>=0 ? (unsigned)match : find_room_row(SERVICE_REFRESH,"");
    } else if(pos->kind==LISTED_ROOM && !*pos->room && pos->listing_revision==listing_revision &&
              pos->row<service_count && service_rows[pos->row].kind==LISTED_ROOM) focus=pos->row;
    else focus=find_room_row(pos->kind,pos->room);
}

static bool update_service(void) {
    bool list=menu_level==MENU_ROOMS || menu_level==MENU_FRIENDS;
    struct ServicePosition pos={0};
    if(list) pos=service_position();
    bool connected,account;
    mk8_menu_nextendo_snapshot(&service_incoming,&connected,&account);
    service_checked_at=plat_now_ns();
    if(service_count && service_connected==connected && account_available==account &&
       !memcmp(&service_incoming,&service_state,sizeof service_state)) return false;
    // A list offset can restore focus only while that exact listing is still displayed.
    if(connected!=service_connected || service_incoming.listings_ok!=service_state.listings_ok ||
       service_incoming.lobby_count!=service_state.lobby_count ||
       memcmp(service_incoming.lobbies,service_state.lobbies,sizeof service_state.lobbies)) ++listing_revision;
    service_connected=connected; account_available=account; service_state=service_incoming; service_count=0;
    if(connected && menu_level==MENU_FRIENDS) {
        if(service_state.friends_ok && service_state.count>=0 && (size_t)service_state.count<=sizeof service_state.friends/sizeof *service_state.friends)
            for(int i=0;i<service_state.count;i++) service_rows[service_count++]=(struct ServiceRow){LISTED_FRIEND,false};
    } else if(connected) {
        if(service_state.listings_ok && service_state.lobby_count>=0 && service_state.lobby_count<=128) {
            for(int i=0;i<service_state.lobby_count;i++) service_rows[service_count++]=(struct ServiceRow){LISTED_ROOM,true};
        }
        if(service_state.lobby_ok && service_state.in_lobby && *service_state.lobby_id)
            service_rows[service_count++]=(struct ServiceRow){CURRENT_ROOM,true};
    }
    if(connected) service_rows[service_count++]=(struct ServiceRow){SERVICE_REFRESH,true};
    if(account) service_rows[service_count++]=(struct ServiceRow){SERVICE_ACCOUNT,true};
    service_rows[service_count++]=(struct ServiceRow){SERVICE_RESUME,true};
    if(!connected && menu_level==MENU_ROOM_DETAILS) {
        menu_level=MENU_ROOMS; *detail_id=0; focus=offset=0;
    } else if(list) restore_position(&pos);
    return true;
}

static void enter_service(enum MenuLevel parent,enum MenuLevel level) {
    service_parent=parent; menu_level=level; service_count=0; focus=offset=0; *detail_id=0;
    update_service();
    memset(positions,0,sizeof positions); service_header=service_has_input=false;
}

static void switch_service(void) {
    positions[menu_level==MENU_FRIENDS]=service_position();
    menu_level=menu_level==MENU_ROOMS ? MENU_FRIENDS : MENU_ROOMS;
    service_count=0; update_service();
    restore_position(&positions[menu_level==MENU_FRIENDS]);
}

static void return_to_service(void) {
    menu_level=account_source; service_count=0; update_service();
    restore_position(&positions[menu_level==MENU_FRIENDS]); service_header=false;
}

static bool own_detail(void) {
    return detail_current && service_state.lobby_ok && service_state.in_lobby && !strcmp(detail_id,service_state.lobby_id);
}

static const Mk8Lobby *listed_detail(void) {
    if(!detail_current && !*detail_id) return &detail_snapshot;
    int index=mk8_lobby_index(&service_state,detail_id);
    return index>=0 ? &service_state.lobbies[index] : NULL;
}

static const Mk8MenuLabel room_refresh={"Refresh","Atualizar","Actualizar"};
static const Mk8MenuLabel room_unavailable={"Room data unavailable.","Dados da sala indisponíveis.","Datos de la sala no disponibles."};
static const Mk8MenuLabel room_closed={"This room is no longer listed.","Esta sala não está mais na lista.","Esta sala ya no está en la lista."};
// The stock heading holds 50 UTF-16 units. Extremely large request counts use the footer.
static bool friends_summary(Mk8MenuLanguage lang,char summary[80]) {
    const char *fmt=mk8_menu_text(lang,"Friends: %d · Requests: %d in / %d sent",
        "Amigos: %d · Pedidos: %d receb. / %d env.","Amigos: %d · Solicitudes: %d rec. / %d env.");
    snprintf(summary,80,fmt,service_state.count,service_state.requests,service_state.sent);
    uint16_t sized[51];
    return utf16_line(sized,50,summary,false)<50;
}
static const char *nextendo_tip(Mk8MenuLanguage lang) {
    if(menu_level==MENU_ROOM_DETAILS) return mk8_menu_label(lang,mk8_nx_online_play);
    if(!service_connected) return account_available ? mk8_menu_label(lang,mk8_nx_check_account) :
        mk8_menu_text(lang,"Then sign in through your browser.","Depois, entre pelo navegador.","Después, entra desde tu navegador.");
    if(menu_level==MENU_FRIENDS) {
        if(service_state.busy) return mk8_menu_label(lang,mk8_nx_loading);
        if(!service_state.friends_ok) return service_state.friends_error;
        char summary[80];
        if(!friends_summary(lang,summary)) {
            static char requests[64];
            snprintf(requests,sizeof requests,mk8_menu_text(lang,"In: %d · Sent: %d","Receb.: %d · Env.: %d","Rec.: %d · Env.: %d"),service_state.requests,service_state.sent);
            return requests;
        }
        if(!service_state.count) return mk8_menu_label(lang,mk8_nx_friends_empty);
        if(friend_row_focused()) return mk8_menu_label(lang,mk8_nx_invites_unavailable);
        return mk8_menu_label(lang,mk8_nx_refresh_interval);
    }
    if(!service_state.listings_ok && *service_state.listings_error) return service_state.listings_error;
    if(!service_state.lobby_ok && *service_state.lobby_error) return service_state.lobby_error;
    if(focus<service_count && service_rows[focus].kind==SERVICE_REFRESH) return mk8_menu_label(lang,mk8_nx_refresh_interval);
    if(service_state.lobby_ok && !service_state.in_lobby) return mk8_menu_label(lang,mk8_nx_no_current_room);
    return mk8_menu_label(lang,mk8_nx_online_play);
}

static void draw_nextendo_header(Cpu *c) {
    Mk8MenuLanguage lang=mk8_menu_language();
    const char *status;
    char summary[80];
    if(menu_level==MENU_ROOM_DETAILS) {
        if(own_detail()) status=mk8_menu_label(lang,mk8_nx_current_room);
        else if(!*detail_id) status=mk8_menu_text(lang,"Public listing at selection","Listagem ao selecionar","Listado al seleccionar");
        else if(listed_detail()) status=mk8_menu_text(lang,"Public listing","Listagem pública","Listado público");
        else if(detail_current && service_state.lobby_ok)
            status=mk8_menu_text(lang,"You are no longer in this room.","Você não está mais nesta sala.","Ya no estás en esta sala.");
        else if(!detail_current && service_state.listings_ok) {
            status=mk8_menu_label(lang,room_closed);
            for(unsigned i=0;i<service_count && service_rows[i].kind==LISTED_ROOM;i++)
                if(!strcmp(service_state.lobbies[i].id,detail_id)) { status=mk8_menu_label(lang,room_unavailable); break; }
        } else status=mk8_menu_label(lang,service_state.busy ? mk8_nx_loading : room_unavailable);
    } else if(!service_connected) {
        status=mk8_menu_play_text(lang,&(Mk8PlaySession){0},account_available ? PLAY_SESSION : PLAY_INSTRUCTION);
    } else if(menu_level==MENU_FRIENDS && service_state.friends_ok) {
        if(!friends_summary(lang,summary))
            snprintf(summary,sizeof summary,mk8_menu_text(lang,"Friends: %d","Amigos: %d","Amigos: %d"),service_state.count);
        status=summary;
    } else if(service_state.busy) status=mk8_menu_label(lang,mk8_nx_loading);
    else if(menu_level==MENU_FRIENDS) status=mk8_menu_text(lang,"Friends list unavailable.","Lista de amigos indisponível.","Lista de amigos no disponible.");
    else if(!service_state.listings_ok) status=mk8_menu_label(lang,room_unavailable);
    else if(!service_state.lobby_count) status=mk8_menu_label(lang,mk8_nx_empty);
    else {
        const char *format=mk8_menu_text(lang,"Players online: %d  ·  Rooms: %d","Jogadores online: %d  ·  Salas: %d","Jugadores en línea: %d  ·  Salas: %d");
        snprintf(summary,sizeof summary,format,service_state.online_players,service_state.lobby_count); status=summary;
    }
    text(c,panes->values[0],status);
    visible(c,panes->rows[0]+0x30,"L_Btn_LR",menu_level!=MENU_ROOM_DETAILS);
}

// Keep the full ID visible, splitting on a character boundary when it spans two rows.
static size_t first_id_bytes(void) {
    size_t i=0;
    for(unsigned units=0;detail_id[i] && units<28;units++) {
        unsigned ch=(unsigned char)detail_id[i++];
        if(ch>=0xf0) units++;
        if(ch>=0xc2) while(((unsigned char)detail_id[i]&0xc0)==0x80) i++;
    }
    return i;
}
static unsigned room_detail_row_count(void) {
    if(!*detail_id) return 3;
    return (own_detail() || listed_detail() ? 4u : 1u)+(detail_id[first_id_bytes()] ? 1 : 0);
}

static void draw_nextendo_row(Cpu *c,unsigned slot,unsigned index) {
    Mk8MenuLanguage lang=mk8_menu_language();
    bool detail=menu_level==MENU_ROOM_DETAILS;
    const char *left="",*right="";
    char buffer[80];
    if(detail) {
        bool own=own_detail();
        const Mk8Lobby *room=listed_detail();
        unsigned meta=own || room ? 3 : 0;
        if(index>=meta) {
            size_t split=first_id_bytes();
            left=index==meta ? "ID" : "";
            if(index==meta) { memcpy(buffer,detail_id,split); buffer[split]=0; right=buffer; }
            else right=detail_id+split;
        } else if(index==0) {
            left=mk8_menu_label(lang,mk8_nx_mode); right=own ? service_state.lobby_mode : room->mode;
        } else if(index==1) {
            left=mk8_menu_label(lang,mk8_nx_status); right=room ? room->state : mk8_menu_label(lang,mk8_nx_current_room);
        } else {
            left=mk8_menu_label(lang,mk8_nx_players);
            snprintf(buffer,sizeof buffer,"%d / %d",own ? service_state.players : room->count,own ? service_state.capacity : room->capacity); right=buffer;
        }
    } else {
        switch(service_rows[index].kind) {
        case LISTED_ROOM: {
            const Mk8Lobby *room=&service_state.lobbies[index]; left=room->mode;
            snprintf(buffer,sizeof buffer,"%d / %d",room->count,room->capacity); right=buffer; break;
        }
        case LISTED_FRIEND: {
            const Mk8Friend *person=&service_state.friends[index]; left=person->name;
            right=mk8_nx_presence(lang,person->presence); break;
        }
        case CURRENT_ROOM:
            left=mk8_menu_label(lang,mk8_nx_current_room);
            snprintf(buffer,sizeof buffer,"%d / %d",service_state.players,service_state.capacity); right=buffer; break;
        case SERVICE_REFRESH: left=mk8_menu_label(lang,room_refresh); break;
        case SERVICE_ACCOUNT: left=mk8_menu_label(lang,mk8_nx_account); break;
        case SERVICE_RESUME: left=mk8_menu_label(lang,mk8_play_back); break;
        }
    }
    controller_place(c,panes->labels[slot],detail || *right ? -245 : 0,0);
    list_text(c,panes->labels[slot],left,24); list_text(c,panes->values[slot],right,36);
    call(c,!detail && !service_header && index==focus ? 0x700044ed20ull : 0x700044ee50ull,panes->rows[slot],1,0,0,0);
    bool actionable=!detail && service_rows[index].enabled;
    row_ink(panes->labels[slot],actionable); row_ink(panes->values[slot],actionable);
    visible(c,panes->rows[slot]+0x30,"L_Btn_LR",false);
}

static void nextendo_input(Cpu *c,uint64_t edge,bool close,bool confirm,bool click,int x,int y) {
    int scroll=atomic_exchange(&wheel,0);
    close|=click && x>=20 && x<=340 && y>=600 && y<=680;
    if(menu_level==MENU_ROOM_DETAILS) {
        if(close || confirm) {
            menu_level=MENU_ROOMS; restore_position(&detail_position);
            menu_back_feedback(c); draw(c);
        }
        return;
    }
    unsigned before=focus; bool was_header=service_header;
    bool header_click=click && x>=200 && x<=1080 && y>=150 && y<=218;
    if(!close && (header_click || (service_header && confirm) || (edge&((1ull<<12)|(1ull<<14)|(1ull<<16)|(1ull<<18))))) {
        if(header_click || confirm) pulse_slot(c,0);
        switch_service(); menu_sound(c,header_click || confirm ? 0x7000ed87bfull : 0x7000ee9492ull);
        draw(c); return;
    }
    int direction=scroll;
    if(edge&((1ull<<13)|(1ull<<17))) direction=-1;
    if(edge&((1ull<<15)|(1ull<<19))) direction=1;
    if(direction) {
        service_has_input=true;
        int64_t next=(service_header ? -1 : (int)focus)+(int64_t)direction;
        service_header=next<0;
        focus=next<0 ? 0 : next>=(int)service_count ? service_count-1 : (unsigned)next;
    }
    if(click && x>=200 && x<=1080 && y>=230 && y<230+60*SETTINGS_VISIBLE) {
        unsigned index=offset+(unsigned)(y-230)/60;
        if(index<service_count) { service_header=false; focus=index; confirm=true; }
    }
    if(close) {
        focus=service_parent==MENU_HOME ? HOME_NEXTENDO : 1; service_header=false;
        menu_level=service_parent; offset=0;
        menu_back_feedback(c);
    } else if(confirm) {
        service_has_input=true;
        enum ServiceRowKind kind=service_rows[focus].kind;
        if(!service_rows[focus].enabled) {
            if(before!=focus || was_header!=service_header) { menu_sound(c,0x7000ee9492ull); draw(c); }
            return;
        }
        if(kind==SERVICE_RESUME) {
            start_exit(c,true);
            if(exiting) { pulse_slot(c,focus-offset+1); menu_sound(c,0x7000ed87bfull); }
            return;
        }
        pulse_slot(c,focus-offset+1); menu_sound(c,0x7000ed87bfull);
        if(kind==SERVICE_REFRESH) update_service();
        else if(kind==SERVICE_ACCOUNT) {
            positions[menu_level==MENU_FRIENDS]=service_position(); account_source=menu_level;
            enter_settings_category(c,MENU_ACCOUNT);
        }
        else {
            detail_current=kind==CURRENT_ROOM;
            detail_position=service_position();
            snprintf(detail_id,sizeof detail_id,"%s",detail_position.room);
            if(!*detail_id) detail_snapshot=service_state.lobbies[focus];
            menu_level=MENU_ROOM_DETAILS; focus=offset=0;
        }
    } else if(before==focus && was_header==service_header) return;
    else menu_sound(c,0x7000ee9492ull);
    draw(c);
}
