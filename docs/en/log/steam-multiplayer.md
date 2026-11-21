# Technical log — Steam and multiplayer

[Index](README.md) · [Português](../../pt-BR/log/steam-multiplayer.md)

## SteamFriends and overlay in solo mode — issue #71

The public 0.3.0 executable, checked against the release SHA-256, reproduced the failure of [issue #71](https://github.com/Vnnc1/MK8-online/issues/71) with the x64 Counter-Strike 2 DLL in the data folder. It offers `SteamAPI_SteamFriends_v018`; the launcher looks for `SteamAPI_SteamFriends_v017` or the legacy export `SteamFriends`. The tested Brawlhalla DLL offers `v017` and 0.3.0 managed to list friends with it.

[PR #33](https://github.com/Vnnc1/MK8-online/pull/33) went into 0.3.0. The attempt to validate several candidates only came later, in [commit 26a6dce](https://github.com/Vnnc1/MK8-online/commit/26a6dce). On `fix-chore` at `a9aa55b`, the `v018` DLL in the data folder still prevented the search in the game libraries, even with Brawlhalla installed. In new processes, the friends list opened both in the automatic search with no local DLL and with the incompatible DLL in the data folder and the compatible one next to the executable. The ZIP from the reporter's PC did not accompany the issue.

In two solo runs with the same runtime, the user confirmed that Shift+Tab failed after the DLL error and opened the overlay when the launcher listed the friends before starting the game. The loading of `gameoverlayrenderer64.dll` in the MK8 process matched that result. On the public 0.3.0, `MK8_STEAM_DLL` pointing to the compatible copy also allowed listing friends even with the incompatible DLL present in the data folder.

The tested copies of Counter-Strike 2 and Deadlock have the same SHA-256. An isolated proof initialized that DLL, obtained the `v018` interface and read the identity and the friend count using functions exported by the DLL itself. The launcher still has no explicit support for that version. Rooms, invites and transport with `v018` remain unvalidated.

## CI of PR #59 — socket release on Windows

The [run at `fe69983`](https://github.com/Vnnc1/MK8-online/actions/runs/44372501134) failed again only in the Rust test of Steam port release, with WinSock 10048; the Python stage was skipped. The socket fix already reproduced and reviewed at `4e34d8a` now makes up this PR as a separate change. The session file from before the integration was identical to the one from the original reproduction, and the integrated file is identical to the reviewed fix.

The gateway shares its two original sockets with `Arc` between the workers. This avoids the inheritable handles created by `UdpSocket::try_clone`, which a child process could keep alive after the gateway ended. The regression uses the real gateway and confirms that both ports are released while the child remains alive, with the child cleaned up before the assertions. Endpoints, timeouts, joins, authentication and replay protection were kept; independent review of the integration approved. The exact process involved in the hosted failure has not yet been observed.

On the integrated source, 96 Rust tests passed with toolchain 1.100.0, including the 45 of the launcher and the new regression; one live Steam test that was already ignored remained ignored. Formatting, Clippy with `-D warnings`, the launcher debug build and nine Python pipeline tests with TEMP on an 8.3 alias passed. Private receipt: `local/uw-pr18/pr59-socket-integrated-checks.json`. The 489 protected files and the installed launcher kept their hashes; no test/build process remained active. New CI pending; multiplayer on two machines and `gameplay_verified` remain unqualified.

[PR #59](https://github.com/Vnnc1/MK8-online/pull/59) is open against `fix-chore`. The [CI repetition at `aa472b1`](https://github.com/Vnnc1/MK8-online/actions/runs/44358328861) passed formatting, Clippy and Rust tests, including the Steam port release. The Python pipeline failed afterwards: the synthetic runtime-update fixture did not provide the menu files required by the Settings preparation. The records below that mention the absence of publication describe earlier steps.

The same absence of `UI/cmn/menu.sarc` was reproduced with Python 3.12.10, as in CI. Only the fixture of `test_runtime_refresh_records_new_dll_without_changing_game_or_saves` was fixed: the preparation boundary supplies two authored outputs; the real update verifies their records in the manifest, the preparation arguments and the preservation of the guest library, save and native receipt. The real derivation and the rejection of hashes remain covered by `test_settings_layout.py`; no production validation was removed. The nine pipeline tests and the seven layout tests passed, with no skips, with independent review approved.

The [next CI at `0ee287a`](https://github.com/Vnnc1/MK8-online/actions/runs/44371038014) also passed the Rust gates and revealed a second difference in the fixture: TEMP used `RUNNER~1`, while `file_record` resolves the executable to `runneradmin`. The same error was reproduced with a real local 8.3 alias. The temporary root is now resolved before building the paths, as in the production caller; all assertions remain. The focused test and the nine pipeline tests passed using that same alias, with no skips, with independent review approved. The full run needed permission to create the test's already existing temporary junction. New CI pending; no runtime/launcher code, private data or gameplay qualification changed.

## Continuation of the fixes — Steam, textures and presentation

On `fix-chore` after `07126fd`, Steam selection now tries to load/validate the exports of each candidate DLL until it finds a compatible one, instead of failing on the first library discovered. Priority and scope of explicit/local paths preserved; required ABI versions were not relaxed. Failures include the path and reason (up to four details). A behavioral test covers an incompatible candidate followed by a compatible one, and the absence or error of all. 16 session tests passed and the optional real Steam test remained ignored. There was no Steam connection between two machines; #45 still needs a retest with the reporter's DLLs.

Code of PRs #43/#44 applied locally after review: cubemap history requires an existing GPU image, avoiding re-upload of reused guest memory; CmdRec header reduced from 88 to 72 bytes. All host consumers recompiled together. 14 texture tests, 19 submit/descriptor/compute/occlusion tests and 1 exercising the 62 compacted commands passed. No remote merge of the PRs in this step. Do not attribute to these changes the solution of white Miis, headlights #49 or lack of memory #52 without reproduction.

Investigation #46 confirmed that the MK8 hook does not enable a wide camera and the presentation kept bars regardless of `aspect=unlocked`. The own adaptation `output_aspect.py` now uses that choice to stretch only the final presentation; internal resolution and camera shape remain the same. The native menu clarifies the stretching, with no promise of a larger field of view. Test C runs the prepared calculation at 3440×1440, 1280×1024 and 1920×1080 to fill/preserve the aspect ratio, passed. Native smoke `local/race-time-trial-7gpln4nc` opened a 1280×540 swapchain and ended with code 0; the title capture was inspected, but the hook captures the game image at 1920×1080, so it does not visually prove the final stretching of the window. Race/split-screen and the final presentation on the monitor still need to be checked.

Launcher debug compiled, 36 tests passed; Clippy of launcher/session with `-D warnings` and formatting passed. Final runtime SHA-256 `ce7e59c1dd07c72205fcd0a0e1b22465bb5af98ac176596f6d1b192c57ecf65c`. No new setup/publication, no change to personal saves. Priority pending items kept: error 193/SDL #47, GPU memory #52, controller #50, split-screen lighting #49; DLC #36 awaits the reporter's metadata. Save import #41/#48, direct start #37 and portability #35 are not yet implemented. #51 requires proving physical space, since the pipeline already uses hard links.

## Contribution: Steam, navigation and Mii on fix-chore — 2026-11-16

Integration base: `d443092`. The contribution preserves verified DLC, settings, Spanish, CTGP and runtime fixes already present on that branch. The equivalent fix of the Mii freeze already incorporated by the maintainer was kept.

- **Steam:** resolves versioned C accessors with a legacy fallback; searches registered/additional libraries and internal layouts without a cut at 40 games; keeps all friends searchable, sorted by availability/name, including offline ones. It does not identify invisible friends. No Valve DLL is distributed; the self-contained dependency remains pending.
- **Transport:** encrypted datagrams above 1200 bytes use Reliable; smaller ones remain UnreliableNoDelay. The simulated backend enforces the real limit and tests the boundaries in both directions. Retransmission/ordering may increase latency under loss; the effect in a real race still needs to be measured.
- **Session:** leaving the game ends workers and releases lobby/gateway; an idle guest can accept another room without restarting the launcher, but a live game remains protected. Entry validates LobbyEnter, identity, DLC and CTGP; failures leave the lobby. Invites through the client and through the launcher use the same routine.
- **Navigation:** the host chooses automatic or manual; the guest chooses the navigation before accepting the invite. Both host buttons create a Steam room; merely opening manually does not connect to another host's room. The guest's preference persists per computer.
- **Optional DLC:** the field in Game files follows the preparation. Since the current importer requires a ready candidate, the import happens after the successful build, in the same guided operation. An empty field preserves the preference; failure/cancellation does not trigger the pending import. The importer and the upstream manifests are reused; there is no second receipt format. A later import remains in Install → Additional content, reachable by a shortcut in Play, with activation in Settings.
- **Mii:** automation confirms the active profile without DOWN for a preset; AFL RG8 glasses textures swap coverage/luminance to the NX order, with review of the converter. Both adjustments require regenerating Mii resources and recompiling the runtime; updating only the launcher does not repair old derived resources.

Local validation of the integration: `cargo test --workspace --locked` (77 passed, one optional real Steam test ignored), formatting, Clippy with `-D warnings` and the launcher release build. Mii converter: eight tests passed; LAN automation: one C test compiled/run on host and guest; supervision/navigation: four tests with real processes and authored executable passed. Browser: 24 tests passed and one optional external relay ignored; languages: four passed. Desktop/mobile captures inspected. No commercial file was needed for these tests. The diagnostic artifacts remain private and are not part of the contribution.

Limits: no full race between machines, real reconnection between two clients, intercontinental test or new end-to-end commercial import in this integration. Earlier tests reached track selection, but the host ended with a Vulkan device loss also reproduced in solo; that computer was removed from the next round of tests, without attributing a physical cause. `gameplay_verified` and `multiplayer_verified` remain false. Installer, tag and release version are left to the maintainer's decision after review.

## Steam rooms require the same DLC state — 2026-11-13

The host publishes `mk8_dlc` (`1` with the Booster Course Pass installed and enabled; `0` without it) in the lobby data, next to `mk8_code`. The state is the same one the game receives at startup (`dlc::launch_root`). On joining, `SteamRoom::join_live` reads the key **before opening the transport** and refuses differing states with an actionable message ("Import and enable the DLC…" / "Disable the DLC…"), leaving the Steam lobby. A lobby without the key, from a launcher without DLC support, counts as without DLC; an unknown value is refused. The decision lives in `steam::check_dlc` (test `dlc_state_must_match_the_host_lobby`: four combinations, old host and invalid value). Automatic entry through `+connect_lobby` now shows the error instead of silently ignoring it. The DLC card in **Settings** states the rule; texts in PT/EN. Validation: formatting, Clippy, 66 Rust tests, e2e (21), languages (3) and DLC in the launcher (4). **No proof with two real Steam accounts.** P2P rooms (`MK8P-…`), invites and relay remain without this check; today they are used only by scripts/API, since the interface offers only Steam rooms.

## Multiplayer through Steam only, with the Mii and automatic room entry — 2026-11-12

**Multiplayer tab:** offers rooms through Steam only. Removed from the screen:

- creating a room on the local network, over the internet or with a P2P code;
- `mk8://` invites, code field, room search and alternative address;
- the "Username" field.

These APIs remain on the server (`scripts/test-multiplayer.py` and the tests use them). The screen shows:

- the active Mii ("You join rooms as …", with **Change Mii**/**Create Mii now**, which leads to Mii profile);
- **Create room**;
- received invites (**Accept**/**Decline**) and friends' open rooms (**Join**);
- the list of online Steam friends, with **Invite** when the room is yours;
- **Invite via Steam** (overlay) and **Close room**.

**Mii and name:** creating or joining a room (`/api/steam/host`, `/api/steam/join`, `/api/session/host-steam`) requires a saved Mii. The Mii's name becomes the account name in the game (`RECOMP_USER_NAME`, `username.json`), which appears in the room list and in the lobby.

**Steam (`crates/session/src/steam.rs`):**

- New functions:
  - friends list (`GetFriendCount/ByIndex/PersonaName/PersonaState/GamePlayed/RichPresence`);
  - `InviteUserToLobby`.
- The host's room publishes the rich presence `mk8_room=1`. A friend appears in "friends' rooms" when in the same AppID, in a lobby and with that mark.
- **Callbacks** registered with `SteamAPI_RegisterCallback`, with a `CCallbackBase` object assembled in Rust (vtable in MSVC order):
  - `GameLobbyJoinRequested_t` (333): invite accepted in the Steam chat or overlay;
  - `GameRichPresenceJoinRequested_t` (337): "Join game" from the friends list;
  - `LobbyInvite_t` (503): received invite, shown in the launcher;
  - `P2PSessionRequest_t` (1202): accepts the guest's P2P session. The old code only accepted after reading a packet, and a packet from a session that is not accepted never gets read;
  - `LobbyEnter_t` (504): diagnostic that the callbacks arrive.
- `SteamAPI_RunCallbacks` is now called under a lock (rooms and a dispatch thread every 100 ms).
- After the first use of Steam on the PC (`local/steam-enabled`), the launcher reconnects on its own when opened, to receive invites.

**Flow:**

1. When creating or joining the room, the launcher opens the game on its own once the transport confirms the connection.
2. Whoever accepts an invite through Steam with the launcher open joins directly: the launcher comes to the front, joins the room and opens the game.
3. Errors on this path appear as a warning (`steam_error`).

**Automatic entry inside the game (experimental):** `runtime/mk8/net/auto_lan.inc`, applied by `toolchain/auto_lan.py`.

- Enabled with `MK8_AUTO_LAN=host|guest`, which the launcher sets only for Steam rooms (`game_session::Request.auto_lan`), or with `1` (slot 0 hosts).
- Presses the buttons through the game's own input (`g_test_buttons`, added to the player's). Waits for **observed milestones**, not fixed frames:
  - the menu video (`MovieMenu.mp4`) opening;
  - the virtual network (`vlan`) coming up, which shows the game entered the LAN Lobby.
- Sequence: title → menu → L+R+stick click → Lobby.
- **Host:** presses **Create Room** and broadcasts over `vlan`, once per second, a notice on a port the game does not use (47999). `vlan.c` consumes that notice and never delivers it to the game.
- **Guest:** only goes down to the room and presses A **after receiving the notice**. Without it, it never presses A (on an empty list, A would create a room).
- Any wait that times out ends the sequence and the player continues by hand. Character and rules are left to the player.
- A first attempt used "first unicast packet" as the room signal. The packet arrived before the room existed, so it was discarded.

**Evidence:**

- **Live Steam, on this PC (user's account):**
  - Steam connected through the `steam_api64.dll` of another installed game and listed 89 friends;
  - friends-only room created and host transport connected;
  - `LobbyEnter_t` delivered (`callbacks=true`), which proves the callback mechanism;
  - without a Mii, the room is refused; with a Mii, the name becomes the Mii's;
  - no invite was sent.
- **Two real game instances on the same PC** (local transport; `scripts/test-multiplayer.py --env MK8_AUTO_LAN=1`):
  - `local/multiplayer-rf8o8bsu` (first signal) and `local/multiplayer-perc57wm` (room notice);
  - both reached the LAN Lobby on their own, and the host created the room and went to the garage;
  - the guest received the notice, saw "Host 1/12", joined and stayed on character selection inside the room (captures 1800 and 2400 inspected);
  - the final "communication error" is the host exiting at the end of the script.

**Not validated:**

- the guest side through Steam (another account: `JoinLobby`, `MK8J`/`MK8I` exchange, P2P tunnel and acceptance through the chat);
- the Steam overlay over the launcher window;
- slower PCs (the intervals between milestones are fixed);
- saves with another menu state;
- a full race between two machines.

**Limits:**

- There is no public installer with `steam_api64.dll`: the launcher looks for a copy on the PC (variable, data folder, launcher folder or `steamapps/common`).
- The launcher appears as "Spacewar" (AppID 480).
- Anyone without the launcher open who accepts an invite sees Steam try to open Spacewar.
- `gameplay_verified=false` and `multiplayer_verified=false`.

**Validation:**

- Formatting, Clippy `-D warnings` and all Rust tests, including `steam_callbacks_become_join_and_invite_events` (events and ABI of `FriendGameInfo_t`/`CCallbackBase`).
- `tests/test_launcher_e2e.py`, `tests/test_launcher_languages.py` and `tests/test_mii_launcher.py`: 18 passed and 1 skipped (relay not configured). The transport tests use the API; the new screen is verified (without a Mii, refusal; with a Mii, name; Steam only).
- `tests/test_runtime_stalls.py` checks the automatic-entry patch on the prepared copy.
- `scripts/test-mii-composer.py`.
- Fix found by the tests: the creator did not read the body of error responses, and the request stayed open in the browser.

## `fix-multiplayer` integrated into `fix-chore` — 2026-11-12

Merge of `fix-multiplayer` (P2P rooms with unique code `MK8P-…` + STUN, Steam integration `MK8S-…`, per-host TLS pin separation in Nextendo, automatic OAuth opening). Only conflict: translations in `apps/launcher/web/en.json`, those from both sides kept. Formatting, workspace Clippy with `-D warnings` and all Rust tests passed. `scripts/test-multiplayer.py` brings together `--p2p`/`--mii-profile` (multiplayer) and `--mii-resources`/`--host-profile`/`--guest-profile` (fix-chore). Combined evidence (`local/multiplayer-5u3b89qo`, `--p2p --mii-resources`, 200.4 s): P2P room by unique code with **both animated 3D-body Miis in the garage**, portraits on the voting cards and a host race with the Mii icon on the minimap. The guest's "A communication error has occurred." screen in `1/frame-10000.png` was recorded 7.6 s after the host ended following its last capture (`0/frame-10000.png` at 19:28:28, `1/frame-10000.png` at 19:28:35): it is an effect of the end of the script, as in the earlier P2P run, and not a room failure. The static head (phases 4–6) remains blocked in rooms; the path for online Miis is the original renderer with generated resources. A full race between two PCs remains pending.

## P2P unique-code rooms + STUN and Steam integration (`fix-multiplayer`) — 2026-11-10

Also implemented in `crates/session/src/p2p.rs` and integrated into `crates/session/src/lib.rs`, `apps/launcher/src/server.rs` and the web interface, **Option 1 (reusable unique code `MK8P-…` + NAT traversal via STUN)** next to the Steam option (`MK8S-…`), keeping both available for comparative tests:

- **STUN RFC 5389 on the host's socket (`p2p::stun_discover_on_socket`):** when starting a room, the `Host` queries public STUN servers (`stun.cloudflare.com:3478`, `stun.l.google.com:19302`) through the session's own UDP socket to discover its external `IP:port` (`XOR-MAPPED-ADDRESS`) and keeps the NAT mapping alive with keepalives every 20 s (can be disabled in hermetic tests with `MK8_DISABLE_STUN=1`).
- **Reusable unique ticket (`MK8P-…`) and slot auto-allocation (`MK8-JOIN-1`):** encodes the candidate endpoints (explicit/alternative endpoint, STUN public endpoint and detected LAN IP) + a 16-byte `room_token`. Up to 11 guests can paste the **same `MK8P-…` code** into **Join** (or click the room found directly in **Find rooms** on the local network); `p2p::join_with_ticket` fires probes in parallel, receives its authenticated individual `Invite` (`XChaCha20-Poly1305`) and starts the `Gateway` on the same punched UDP socket.

## Experimental integration with Steam Lobbies and P2P Relay (`fix-multiplayer`) — 2026-11-10

Implemented in `crates/session/src/steam.rs` and integrated into the launcher (`apps/launcher/src/server.rs`, `apps/launcher/src/main.rs` and the web interface), an optional dynamic adapter for Steamworks (`ISteamMatchmaking` + `ISteamNetworking` P2P Relay), without distributing proprietary Valve binaries in the repository:

- **Dynamic loading (`LoadLibraryExW`):** looks for `steam_api64.dll` in `MK8_STEAM_DLL`, in the data folder (`local/steam_api64.dll`), next to the executable or in local Steam installations (`steamapps/common`), setting `SteamAppId=480` (or `MK8_STEAM_APPID`) for tests.
- **Unique code `MK8S-…` and automatic slot negotiation:** when clicking **Create room via Steam**, the host creates a Steam lobby of up to 12 players and pre-allocates the 11 authenticated slots (`XChaCha20-Poly1305`) on the local `Host` at `127.0.0.1:0`. Guests who join with the same `MK8S-…` code or via a Steam invite (`+connect_lobby <id>`) automatically negotiate their individual invite over the P2P control channel (`P2P_CHANNEL_CONTROL`) and tunnel the `MMKDVL1` packets over the data channel (`P2P_CHANNEL_DATA`) via Steam Datagram Relay, with no need for a VPN, without opening UDP port `47820` in the firewall and without generating individual invites by hand.
- **Invite via Steam Overlay:** the **Invite friends on Steam** button triggers `SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog` and publishes Rich Presence (`+connect_lobby`).
- **Validation run:** formatting (`cargo fmt --all -- --check`), Clippy (`--workspace --all-targets -- -D warnings`), the 50 Rust unit tests (including the STUN RFC 5389 roundtrip, `RoomTicket` `MK8P-…` roundtrip, multiple simultaneous guests with a single `RoomTicket`, `SteamCode` `MK8S-…` roundtrip and a deterministic test of bidirectional `MMKDVL1` packet exchange over a simulated P2P bus) and the 18 end-to-end browser tests (`test_launcher_e2e.py` and `test_launcher_languages.py`, with `mk8-relay` configured, 0 skips) passed.
- **Real in-game evidence (`local/multiplayer-p2r5om9x`):** ran `scripts/test-multiplayer.py --p2p` with two real instances of the launcher and MK8 connected exclusively through the unique code `MK8P-…` (312.8 s, exit 0, protected data intact in `protected-data.json`). The captures confirm: rules/kart selection at frame 4000 (`0/frame-04000.png`, `1/frame-04000.png`), both players and track votes in the LAN garage at frame 6600 (`0/frame-06600.png`, `1/frame-06600.png`) and an active race on Mario Kart Stadium at frame 9200 (`0/frame-09200.png` shows Mario in 2nd place seeing the kart and the `Guest` tag ahead on the track; `1/frame-09200.png` shows the guest in 1st place at the moment the host ended the capture at 9200). A full race between two separate PCs remains pending qualification; `gameplay_verified` remains `false`.

## Guest log confirms crash in LAN voting — 2026-11-09

The `mk8drecomp.previous.log` sent by the guest records the LAN bridge on slot 1 (`10.13.37.2`) and, at around 30 s, `exception c0000005 accessing 0` in `f_700080f4a4+34c`, guest LR `700080f564`; frames `70000d51fc → 700016e13c → 700016ee54 → 700016f100`. Registers x8=0 and x22/x25=0xffffffff. Inspection of the consumer confirms reading of the list at +0x548 before dereferencing the selected pointer. The signature matches the pending resources/portraits problem in the Nextendo test; there is now evidence in LAN too, in the voting screen reported by the user. This confirms a crash of the recompiled guest, but does not qualify the VPN's stability nor prove the exact order of the disconnection messages on the two PCs.

`previous2.log` and `previous3.log` end with `window closed`, without the same exception; the third includes an Online Play attempt without a service. The repeated warning `0 member(s)` does not prove an empty room: the current gateway answers `MMKW\x00` to the local handshake, separately from the transport's real participant count. Fix of the initialization/consumption of the visual resources still pending; no fictitious pointer, success return or save change was applied.

## Reports of the disconnection during voting — 2026-11-09

The local reports 1794260543 (host) and 1794260549 (guest), six seconds apart, record respectively a connected transport with two participants/active game and a disconnected guest with exit `0xc0000409`. This does not determine the order between the transport drop and the game's exit. The hashes of the runtime, main/sdk/subsdk0 entries and content identity match; the individually recompiled binaries differ, which alone does not prove incompatibility. The only historical log included by the host ends with a normal window close and does not demonstrate the cause of this voting.

Confirmed failure in collection: `game.log` is empty in both ZIPs; the Windows runtime redirects output to `mk8drecomp.log` next to the executable, but the report only collected stdout and histories from the data folder. Fixed in `apps/launcher/src/report.rs` and in the manual/automatic report callers: it includes only the four fixed names of the runtime log and their rotations, keeping the size limit and redaction, without copying saves or other build files. Tests verify the real ZIP, log content with empty stdout and exclusion of an adjacent private file; two tests passed, Clippy with no warnings. This change is not yet in the VPN setup delivered earlier. To resolve the exit during voting, the guest's `mk8drecomp.log` is missing (and rotations if the game was restarted); it was not attributed to firewall, Mii or VPN without evidence.

## Local LAN/VPN room test — 2026-11-09

Nextendo paused at the maintainer's request. The interface distinguishes the host's connection from the guests', confirms acceptance by the transport without claiming lobby entry, and gives guidance after 20 seconds without confirmation. The alternative address field belongs to the host: it is kept when the panel is collapsed and editing it invalidates the displayed invite. New invites must contain the host's Radmin IP, with UDP 47820 allowed. Wireless Play/LDN remains unimplemented; the test path is Play in the room → LAN Play (L + R + left stick click on the main menu).

The runtime preparation replaces the inherited Course World/TGRCode guidance with LAN/VPN instructions, preserving the error code and the adaptation's bounds check. No network return was converted into success. A browser test with two launchers and real UDP confirmed acceptance, the count of one guest, an invite with an explicit address even with the panel collapsed, and guidance for an unanswered destination. Languages and adaptation limits passed. Local setup requested for a test on two machines; a race between PCs remains unqualified and `gameplay_verified` remains false. No publication on GitHub. Local installer completed at `target/dist/vpn-test-20261109/MK8-online-0.1.8-vpn-test-setup.exe` (55,697,920 bytes; SHA-256 `7b884652fcff06b15c398d6cd6aea2e9483b42c4007ff32ab6b3d70332c67a79`), with sharing package `target/dist/MK8-online-0.1.8-vpn-test.zip` including FFmpeg sources/notices and instructions. The version remains 0.1.8; the new internal message requires preparing the build again, but the launcher adjustments do not. Release build and precompiled runtime completed; 15 E2E tests passed (relay not configured, 1 skip), 2 language tests, 2 adaptation tests, 3 native transport tests and 4 installer tests passed. The check of the 12 protected private files found no changes; the 916-file payload does not include private input paths.
