// Included in the verified nextendo.c build copy to keep OAuth secrets private.
#include "mk8_social.h"
#include "../ui/menu_nextendo.h"

static pthread_mutex_t social_lock = PTHREAD_MUTEX_INITIALIZER;
static Mk8Social social;
static unsigned social_session;
static bool social_running;
static time_t social_attempt;

static void social_error(char *out, size_t cap, int status) {
    snprintf(out,cap,"%s",mk8_nx_service_error(mk8_menu_language(),status));
}

static bool social_friends(Json *root, Mk8Social *out) {
    Json *list = json_get(root,"friends"), *requests = json_get(root,"requests"), *sent = json_get(root,"sent");
    if (!list || list->type != JSON_ARR || json_len(list)>300 ||
        !requests || requests->type != JSON_ARR || !sent || sent->type != JSON_ARR) return false;
    out->requests = (int)json_len(requests); out->sent = (int)json_len(sent);
    for (size_t i=0;i<json_len(list);++i) {
        Json *entry=json_at(list,i), *pid=json_get(entry,"pid");
        const char *name=json_str(json_get(entry,"name"),"");
        if (!*name) name=json_str(json_get(entry,"username"),"");
        if (!pid || pid->type!=JSON_NUM || pid->num<1 || pid->num>9007199254740991.0 ||
            (double)(uint64_t)pid->num!=pid->num || !*name) return false;
        Mk8Friend *friend_entry=&out->friends[out->count++];
        friend_entry->pid=(uint64_t)pid->num;
        snprintf(friend_entry->name,sizeof friend_entry->name,"%s",name);
        snprintf(friend_entry->code,sizeof friend_entry->code,"%s",json_str(json_get(entry,"friend_code"),""));
        Json *status=json_get(json_get(entry,"presence"),"status");
        friend_entry->presence = status && status->type==JSON_NUM &&
            (status->num==0 || status->num==1 || status->num==2) ? (int)status->num : -1;
    }
    return true;
}

static bool social_lobby(Json *root, Mk8Social *out) {
    Json *present=json_get(root,"in_lobby");
    if (!present || present->type!=JSON_BOOL) return false;
    out->in_lobby=json_bool(present,false);
    if (!out->in_lobby) return true;
    Json *lobby=json_get(root,"lobby"), *id=json_get(lobby,"id");
    double count=json_num(json_get(lobby,"count"),-1), max=json_num(json_get(lobby,"max"),-1);
    if (!id || count<0 || max<1 || max>64 || count>max || count!=(int)count || max!=(int)max) return false;
    if (id->type==JSON_STR && id->str[0] && strlen(id->str)<sizeof out->lobby_id)
        snprintf(out->lobby_id,sizeof out->lobby_id,"%s",id->str);
    else if (id->type==JSON_NUM && id->num>=0 && id->num<=9007199254740991.0 && (double)(uint64_t)id->num==id->num)
        snprintf(out->lobby_id,sizeof out->lobby_id,"%llu",(unsigned long long)id->num);
    else return false;
    out->players=(int)count; out->capacity=(int)max;
    snprintf(out->lobby_mode,sizeof out->lobby_mode,"%s",json_str(json_get(lobby,"mode"),mk8_menu_text(mk8_menu_language(), "Unknown mode", "Modo não informado", "Modo desconocido")));
    return true;
}

static bool social_listings(Json *root, Mk8Social *out) {
    Json *games=json_get(root,"games");
    if (!games || games->type!=JSON_ARR) return false;
    Json *game=NULL;
    for (size_t i=0;i<json_len(games);++i) {
        Json *entry=json_at(games,i), *titles=json_get(entry,"titles");
        for (size_t t=0;t<json_len(titles);++t) {
            if (!strcmp(json_str(json_at(titles,t),""),"0100152000022000")) {
                if (game) return false;
                game=entry;
            }
        }
    }
    // A missing game / unavailable feed is not evidence of zero players.
    if (!game || !json_bool(json_get(game,"roomsAvailable"),true)) return false;
    Json *rooms=json_get(game,"lobbies"), *players=json_get(game,"players");
    if (!rooms || rooms->type!=JSON_ARR || json_len(rooms)>128 ||
        !players || players->type!=JSON_ARR || json_len(players)>100000) return false;
    out->online_players=(int)json_len(players);
    for (size_t i=0;i<json_len(rooms);++i) {
        Json *entry=json_at(rooms,i), *id=json_get(entry,"id");
        double count=json_num(json_get(entry,"count"),-1), max=json_num(json_get(entry,"max"),-1);
        if (count<0 || max<1 || max>64 || count>max || count!=(int)count || max!=(int)max) return false;
        Mk8Lobby *room=&out->lobbies[out->lobby_count++];
        room->count=(int)count; room->capacity=(int)max;
        snprintf(room->mode,sizeof room->mode,"%s",json_str(json_get(entry,"mode"),mk8_menu_text(mk8_menu_language(), "Unknown mode", "Modo não informado", "Modo desconocido")));
        snprintf(room->kind,sizeof room->kind,"%s",json_str(json_get(entry,"kind"),""));
        snprintf(room->state,sizeof room->state,"%s",json_str(json_get(entry,"state"),mk8_menu_text(mk8_menu_language(), "Unknown", "Não informado", "Desconocido")));
        if (id && id->type==JSON_STR && strlen(id->str)<sizeof room->id) snprintf(room->id,sizeof room->id,"%s",id->str);
        else if (id && id->type==JSON_NUM && id->num>=0 && id->num<=9007199254740991.0 && (double)(uint64_t)id->num==id->num)
            snprintf(room->id,sizeof room->id,"%llu",(unsigned long long)id->num);
        // The display index must never be used as a NEX gathering ID.
    }
    return true;
}

static Json *social_get(const char *path, unsigned session, int *status) {
    char token[sizeof S.access], url[512], headers[1400];
    pthread_mutex_lock(&lock);
    bool current=S.session==session && S.state==NX_SIGNED_IN;
    bool fresh=current && S.access[0] && S.access_exp-time(NULL)>ACCESS_MIN_LEFT_S;
    pthread_mutex_unlock(&lock);
    if (!current || (!fresh && !refresh_access(5000,session))) { *status=401; return NULL; }
    pthread_mutex_lock(&lock);
    current=S.session==session && S.state==NX_SIGNED_IN;
    snprintf(token,sizeof token,"%s",current?S.access:"");
    pthread_mutex_unlock(&lock);
    if (!*token) { *status=401; return NULL; }
    snprintf(url,sizeof url,"%s%s",api_base(),path);
    snprintf(headers,sizeof headers,"Authorization: Bearer %s\r\nAccept: application/json\r\nX-Nextendo-Client-Id: " CLIENT_ID "\r\n",token);
    HttpResponse response={0};
    *status=http_request("GET",url,headers,NULL,0,5000,&response);
    mbedtls_platform_zeroize(token,sizeof token);
    mbedtls_platform_zeroize(headers,sizeof headers);
    Json *root=*status==200 && response.len<=1024*1024 ? json_parse(response.body,response.len) : NULL;
    http_free(&response);
    return root;
}

static void *social_worker(void *arg) {
    unsigned session=*(unsigned *)arg; free(arg);
    Mk8Social *next=calloc(1,sizeof *next);
    if (next) {
        pthread_mutex_lock(&net_lock);
        int status=0;
        Json *root=social_get("/api/friends?images=0",session,&status);
        next->friends_ok=social_friends(root,next);
        if (!next->friends_ok) { next->count=0; social_error(next->friends_error,sizeof next->friends_error,status); }
        json_free(root);
        root=social_get("/api/my-lobby",session,&status);
        next->lobby_ok=social_lobby(root,next);
        if (!next->lobby_ok) social_error(next->lobby_error,sizeof next->lobby_error,status);
        json_free(root);
        root=social_get("/api/status/live",session,&status);
        next->listings_ok=social_listings(root,next);
        if (!next->listings_ok) {
            next->lobby_count=0; next->online_players=0;
            social_error(next->listings_error,sizeof next->listings_error,status);
        }
        json_free(root);
        pthread_mutex_unlock(&net_lock);
        pthread_mutex_lock(&lock);
        pthread_mutex_lock(&social_lock);
        if (S.session==session && S.state==NX_SIGNED_IN) { social=*next; social_session=session; }
        social_running=false;
        pthread_mutex_unlock(&social_lock);
        pthread_mutex_unlock(&lock);
        free(next);
    } else {
        pthread_mutex_lock(&social_lock); social_running=false; pthread_mutex_unlock(&social_lock);
    }
    return NULL;
}

void mk8_social_snapshot(Mk8Social *out) {
    pthread_mutex_lock(&lock);
    pthread_mutex_lock(&social_lock);
    if (S.session==social_session && S.state==NX_SIGNED_IN) *out=social;
    else memset(out,0,sizeof *out);
    out->busy=social_running;
    pthread_mutex_unlock(&social_lock);
    pthread_mutex_unlock(&lock);
}

void mk8_social_refresh(void) {
    if (!mk8_nextendo_enabled()) return;
    pthread_mutex_lock(&lock);
    pthread_mutex_lock(&social_lock);
    if (S.state==NX_SIGNED_IN && !social_running && time(NULL)-social_attempt>=30) {
        unsigned *session=malloc(sizeof *session);
        if (session) {
            *session=S.session; pthread_t worker;
            social_running=true; social_attempt=time(NULL);
            if (!pthread_create(&worker,NULL,social_worker,session)) pthread_detach(worker);
            else { social_running=false; free(session); }
        }
    }
    pthread_mutex_unlock(&social_lock);
    pthread_mutex_unlock(&lock);
}
