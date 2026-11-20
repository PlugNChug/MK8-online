#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t pid;
    char name[128], code[32];
    int presence; // -1 unknown, 0 offline, 1 online, 2 playing
} Mk8Friend;
typedef struct {
    char id[64], mode[96], kind[48], state[48];
    int count, capacity;
} Mk8Lobby;
typedef struct {
    bool busy, friends_ok, lobby_ok, in_lobby, listings_ok;
    int count, requests, sent, players, capacity;
    int lobby_count, online_players;
    Mk8Lobby lobbies[128];
    Mk8Friend friends[300];
    char listings_error[160];
    char friends_error[160], lobby_error[160], lobby_id[64], lobby_mode[64];
} Mk8Social;

// Nonblocking UI API. Refresh is throttled; account data never leaves memory.
void mk8_social_snapshot(Mk8Social *out);
void mk8_social_refresh(void);
