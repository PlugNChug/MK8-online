# MK8-specific candidate; only validated game adapters are enabled.
GAME_ID := mk8d
GAME_TITLE := Mario Kart 8 Deluxe
GAME_BUNDLE_ID := org.mk8online.mk8d
GAME_TITLE_ID := 0x0100152000022000
# Development metadata only; release profiles must read the user's matching NACP.
GAME_DISPLAY_VERSION := development
GAME_MOVIES := 1
GAME_SRC := $(wildcard games/mk8d/*.c)
GAME_HLE := games/mk8d/hle_symbols.txt
GAME_PATCHES := games/mk8d/patches.py
GAME_PATCH_DEPS :=
