# Technical log — Mii

[Index](README.md) · [Português](../../pt-BR/log/mii.md)

## Transfer a saved Mii between builds (#143)

The launcher exposes its existing 88-byte Switch CharInfo import and export in the Mii editor. A browser test using the compiled launcher checked the downloaded bytes, duplicate protection and selection of an imported profile with its original identity and appearance.

## Launcher frozen in the Mii creator (issues #15 and #16) — 2026-11-14

The launcher's HTTP server handles one request at a time. The creator requested the AFL library (4.5 MB) and handed the not-yet-read response to `FFL.initWithResource`, which first waits for the module to download `ffl-emscripten.wasm` from that same server. When the body exceeded the WebView/TCP buffers, the loop got stuck writing the library and never served the `.wasm`: the screen stayed on "Loading Mii parts…" and **all** other calls stopped (Browse, Play, status). This explains both reports, including "Play solo does nothing" after opening the creator. It depended on the buffer sizes of each machine; here the creator tests passed.

Fix: responses above 32 KiB are written in a dedicated thread (`binary_response` in `apps/launcher/src/server.rs`), and the creator reads the whole AFL before starting the FFL module. Reproduction with the real `Launcher.exe` and the verified AFL: with the library response left unread, the `.wasm` request waited 5 s without a response in the previous binary; in the fixed one it returned immediately. The new `tests/test_launcher_unread_response.py` does the same with a large public resource (`three.core.js`), without private files: it fails on the previous launcher and passes on the new one.

Validation: formatting, Clippy with `-D warnings` and 66 Rust tests; browser with the release launcher: e2e (21, 1 skipped for relay), languages (3), Mii (1) and the new test; `scripts/test-mii-composer.py` with the private AFL passed (library, thumbnails, body, saving and game resources). No new setup or release.

## Mii wait and controller through the USB receiver — 2026-11-13

On the reported installation, Mii generation finished in approximately 4.3 s and the resources were enabled. The global warning to wait for conversion remained after an early attempt to play. The overall status now tracks/finalizes the Mii jobs; the Play screen reports preparation/completion/failure, disables the buttons during the work and replaces only the corresponding old warning. The creator also marks preparation before the first await to avoid two simultaneous requests. Existing resources continue to be reused; no converter speedup was applied or claimed.

The GameSir T4n Lite with USB receiver was recognized by SDL3 and sent buttons/trigger in an isolated test, but did not appear in the log of the game started by the launcher. The launcher empties `SDL_GAMECONTROLLER_IGNORE_DEVICES` and `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT`, preserving mapping and Steam rooms. The user's earlier confirmation in the menu did not validate Steam API initialization: with the SDK connected, the failure came back even with empty filters. The reported installation had the expected setup hash, ruling out an old package. A native C test started by the launcher after connecting to Steam returned XInput 1167 and zero SDL gamepads; outside that context, the same controller was detected.

The Steam interface confirmed **Enable Steam Input** forced for Spacewar (AppID 480). After the user selected **Disable Steam Input** for that game only, the same test with the SDK connected returned XInput 0 in two DLLs and opened `XInput Controller #1` (VID 3537, PID 1040) via SDL3. The result was repeated with additional SDK initialization in the child. Reopening only the game in the old launcher session was not enough. After closing both and opening a new session, connecting the Steam API before starting MK8, the log registered the controller and the user confirmed L + R/analog working with Steam open. This is a per-game setting plus a launcher restart, not an automatic fix; the interface explains these steps in Multiplayer → How it works, in both languages. There was no change to runtime/ABI, driver or global Steam preferences. The tests that removed Steam variables from the child were discarded because they did not resolve the failure. Multiplayer rooms/races on two machines remain unqualified.

Validation: 38 Rust tests, formatting and Clippy passed; browser with 16 tests passed and one skipped, including preparation/completion/failure and translation of the warning; creator with the real AFL, thumbnails, body, saving, conversion and completion tracked by the overall status passed. Three process tests also passed with the packaged launcher: the child process receives the empty filters and keeps the custom mapping, preserves saves and keeps the usage locks while needed. The controller confirmation is menu navigation, not a full race. `gameplay_verified` and `multiplayer_verified` remain false.

**Setup 0.2.0 approved and published:** 57,975,296 bytes, SHA-256 `356381687927b3c93a83ee6b91bb4a6c5e22876cb23d48bb5d39f3c9efa314d3`, at `target/dist/MK8-online-0.2.0-setup.exe` of the configured Cargo. It replaces the package `f792f663…`, including the Steam Input guidance in Portuguese and English. Rust distribution build completed; unchanged runtime and pipeline were reused with hashes checked. Payload with 935 files, with no personal paths/private files detected; 23 tracked personal files preserved. Previous title and description kept, changing only the generated SHA-256 of the setup. In the final package, the three process tests and 16 browser tests passed (one skipped); controller guidance visible in both languages, with no JavaScript errors and with a reviewed capture. The Steam Input configuration remains a per-game choice in Steam and is not changed by the installer.

## Automatic Mii library — 2026-11-13, fix-chore

When opening **Mii profile** without a valid library, the launcher downloads the copy of the old Miitomo CDN from the Internet Archive (origin stated in the interface). It uses HTTPS, size/time limits and a fixed SHA-256 of the ZIP before extracting only the expected AFL; it checks the size and hash of the AFL again before publishing the file and the configuration. It does not execute downloaded content. `mii-library/AFLResHigh_2_3.dat` stays in the private data folder, with offline reuse. An already configured library is preserved; a manual selection made during the download takes precedence. There is a retry and local selection when the network fails. No resource was embedded in GitHub or in the installer.

Validation: debug Rust build completed, `cargo fmt`, Clippy with `-D warnings` and 37 Rust tests passed. Browser suites: 18 passed and 1 skipped (relay not configured). `scripts/test-mii-library-download.py` exercised the real download, size/hash, authentication, absence of download before opening the creator, real connection failure/retry, cleanup of temporary files, translation and cache after restart without HTTPS access, including recreation of the configuration from the cache. `scripts/test-mii-composer.py` passed with manual selection, real thumbnails, game body by gender/height/color, saving, generation of the game resources and reload. Private captures in `local/mii-composer-check/`.

Local change after the public release 0.2.0. At the user's request, a **new 0.2.0 distribution setup for testing before commit/merge/republication** was generated, using `scripts/release.py --tag v0.2.0`, recompiled runtime (213 units) and optimized Rust launcher. `MK8-online-0.2.0-setup.exe`: 57,948,672 bytes, SHA-256 `1d6c33d85734f27aa97d3845364b84cb0b03a44dae291648102635efb3e85ce1`. The package has 935 files; current sources and host/FFmpeg hashes checked, with no private resources or personal paths detected, and 23 tracked personal files preserved. Real download/offline cache and the three creation/editing and language tests passed again using the packaged launcher. Previous title and description preserved byte for byte, except for the generated SHA-256 of the setup. Evidence in `local/rebuild-020-build.log` and `local/release-artifacts-0.2.0.json`; artifacts in `target/dist/` of the configured Cargo. No commit, merge, push or publication in this step; awaiting user testing.

Future availability of the Internet Archive is not guaranteed. Runtime/guest sources unchanged and no new race run; `gameplay_verified` and `multiplayer_verified` remain false.

## Mii creator in the launcher (Mii profile screen) — 2026-11-12

The **Mii profile** screen was rebuilt as a **creator**. Removed:

- the approximate procedural preview;
- the old forms;
- import/export of `.charinfo`;
- the GLB inspector;
- the 3D modal;
- the "Models inside the game" and "Head in the game" cards.

The APIs for these features remain on the server. What stayed on the screen:

- the list of saved Miis and **+ New Mii**;
- the 3D stage (**Full body**/**Face**), with name and **Save Mii**;
- the tabs Body, Face, Hair, Eyes, Eyebrows, Nose, Mouth, Beard, Glasses and Mole.

**How the options appear:**

- Each style and each color is a **button with the thumbnail of the Mii itself with that option**, drawn by FFL from the local AFL library. There are dedicated framings for head, face, eyes, nose and mouth, and the current option is marked.
- Skin tone and favorite color use FFL's own colors (`FFLGetFacelineColor`/`FFLGetFavoriteColor`).
- Type and flip use text buttons.
- Sizes and positions use a slider with −/+.
- The limits come from `/api/mii/fields`. Administrative fields (font region, type, origin region) are preserved without being shown.

**Flow:**

1. The user names the Mii and uses **Create Mii**. The profile is created in `local/mii-profile.json` and `local/profiles/<id>/`.
2. The screen opens the Face tab. Edits stay pending until **Save Mii** (`/api/mii/data`, which also saves the name).
3. Switching Miis with pending changes asks for confirmation.
4. The AFL library is requested only once, with a native `.dat` file picker. Without it, the options appear numbered and keep working.

**Body:** it is MK8's own, `romfs/UI3D/MiiBodyMiddle/MiiBodyMiddle.bfres` (the body of the rooms and MKTV, with MaleBody and FemaleBody). `crates/installer/src/mii_body.rs` reads it **in place**, from the prepared game, through `GET /api/mii/body`, and returns a GLB in memory; nothing derived is written.

- Only the verified file is accepted (SHA-256 `d7bc2730…7d93208`, BFRES 0.5.0.3).
- Offsets, counts, attribute formats (float position, normal 10:10:10:2, UV 16:16), indices, the skeleton and the `Mii_Alb` texture (BC3 sRGB 384², with its own Tegra deswizzle and BC3 decoding) are checked.
- The skeleton must have scale 1 and zero rotation in every bone; the rigid scarf is positioned by the `chest` bone.
- In the preview, the type chooses the body, and height/build apply the SDK scale (FFL's `getBodyScale`, the same formula as `nn::mii`).
- The outfit is tinted with the favorite color, as in the game, and the FFL head is attached to the `head` bone (scale 0.1).
- The body appears in a reference pose (arms out), because the preview does not apply animation.

**Path into the game:**

- When creating/saving a Mii, or when choosing the library, the launcher automatically generates and enables the MiiModel resources (`mii_resources`), if they are not yet active.
- Before that, it disables the converted static head (phase 4–6), which conflicts with the original renderer.
- The active profile still goes to the game at startup through `MK8_MII_CHARINFO`.
- **Mii profile** in the runtime's F1 menu (and the Mii editor applet) brings this screen to the front.

**Lobby with default Mii — hypothesis discarded:** with a native test and two LAN instances (`local/multiplayer-hfnd4lil`), a patch that wrote the profile's CreateId into `UserData.miiID` (`IProfile::Get`) had no effect: MK8 does not call `IProfile::Get` with a buffer in this flow. The patch was removed and the runtime recompiled without it.

With `MK8_TRACE_MII=1` (`local/multiplayer-r4oj5h08`) the following appear:

- `Get` with sources 1 and 2 at initialization;
- **13 `BuildRandom`** (cmd 6);
- repeated `IsFullDatabase`.

The upstream `BuildRandom` always returns the same default Mii, and the 13 calls match the 12+1 Mii instances of the rooms interface. The next step is to locate, in the guest code, the consumer of these results. Returning the profile in `BuildRandom` would be a fictitious result and was not done.

**Validation:**

- Formatting and Clippy of the workspace with `-D warnings`.
- All Rust tests, with three new ones in `mii_body`: BC3/normal/deswizzle/PNG decoding, refusal of a different file, and reading of the verified file when present (counts 808/678/175, `head` bone and texture).
- `tests/test_mii_launcher.py`, rewritten: without library and without game, creates, refuses an empty name, edits face/body/mouth through the buttons, saves, creates a second Mii, switches, confirms discard, reloads, 780 px width and English.
- `tests/test_launcher_languages.py`.
- `scripts/test-mii-composer.py`, rewritten: with library and prepared game, draws the 132 hairstyle thumbnails, changes eyes, female body, height 127 and color 5; saves, generates resources, checks the body GLB and the refusal without a token, reloads and makes no external requests. Captures in `local/mii-composer-check/creator*.png`, inspected.
- `scripts/test-mii-native-launcher.py` and `scripts/test-mii-resources-launcher.py` now use the API or the new flow in place of the removed buttons.

`gameplay_verified=false` and `multiplayer_verified=false`.

## Original Mii renderer with resources derived from the AFL — 2026-11-11

`toolchain/mii_resource_nx.py` generates, from the verified local AFL (`AFLResHigh_2_3.dat`, SHA-256 `4a4be71d…7722`), the six files in the MiiModel layout read by MK8 4.0.1: `ShapeMid/High.dat` and `NXTexture{Low,Mid}{Linear,SRGB}.dat`. The derived files stay private (`local/mii-resource-nx-v2`), like the AFL. With them, `scripts/test-race.py --mii-resources <folder>` sets `MK8_EXPERIMENTAL_NO_MII=0` and `MK8_MII_RESOURCE_DIR`, enabling the **game's original Mii renderer**, with no fictitious object and no change to the guest code.

**Conversion:**

- **Shapes:** NX header of 0x6c (`maxSize[12]` at +0x0c, `maxAlignment[12]` at +0x3c) and entries of 0x58, already described below. Position float4 BE → 4× half precision; UV float2 → 2× half precision; normal 10:10:10:2, tangent and color copied in little-endian order; u16 indices (in the AFL, element 5 stores the count, not bytes). Transformations in separate tables: hair normal 0x48 at 0x12704 and face 0x24 at 0x14c24. The data starts at 0x14dd4.
- **Textures:** the AFL stores **linear** level 0 (checked visually) with a BE footer. Level 0 is converted to Tegra block-linear with the same default block height and the same GOB addressing as `upstream/.../nvn/tegra.c`. For now there is a single mipmap level, because the AFL's mipmap layout has not yet been decoded.
- **Mapping:** NX texture types `[hat, eye 62, eyebrow 24, beard 2 (AFL 1–2), face, makeup, glasses, mole, mouth 37, mustache, nose line]`. Shape types 0–2 and 9 were confirmed by the code (`GetBeardType`, `GetFacelineType`, `GetHairType`). The hat/forehead/hair grouping of 3–8 and nose/nose line of 10–11 was inferred; the visual result below is compatible.
- The converter validates hash, limits, counts, indices and headers before writing, refuses existing output and keeps the 4 GiB reserve. There are six tests with their own data (`tests/test_mii_resource_nx.py`): swizzle checked against the runtime's GOB addressing, round trip, half precision, table offsets, refusals and hash.

**Evidence in the game (inspected):**

- `local/race-time-trial-xfzz9flt` (female profile C): exit 0. `frame-02100.png` shows the Mii in the kart menu with a complete face (eyes, eyebrows, mouth and nose) and the **original helmet**; `frame-03300.png` shows the same at the track presentation. `game.log`: `system data 0100000000000802: …mii-resource-nx-v2`, no exceptions.
- `local/race-time-trial-1rjufkt1` (profile A): exit 0. `frame-01300.png` shows the **Mii selection grid with portraits** of the 7 Miis (profile A with long hair and darker skin, and the six defaults); `frame-01550.png` shows the "Mii Suits" menu with 3D Miis in each outfit; `frame-01800/02050.png`, the driver with an animated expression.
- Correction of an earlier statement: in the original MK8, the Mii with Normal Suit **wears a helmet**. The blue ball on the ground was that model drawn out of place with the renderer disabled. With the generated resources, it appears on the head.

**Generation by the launcher (2026-11-11):** the **Models inside the game** card gained the **Generate from the local library** button. `crates/installer/src/mii_resources.rs` (`generate`/`finish`) checks again the AFL configured in the editor (size and SHA-256), runs `toolchain/mii_resource_nx.py` under the existing supervisor (tool locks; cancellation when the launcher closes) in a new folder `<data>/mii-resources-generated/<job>/files`, with `generation.log`, and only enables the resources after validating the manifest (schema, AFL hash, `rendering_verified=false`, hash of each of the six files) and passing the same validation as `configure`. Routes: `POST /api/mii/resources/generate` and `GET /api/mii/resources/job`; **Play** waits for generation to finish. Interface in PT/EN. New Rust test: manifest from another AFL, incomplete manifest and altered file are refused without enabling anything. `scripts/test-mii-resources-launcher.py` passed (`local/mii-resources-launcher-o6x17ct3`): without AFL, the button reports the requirement and nothing is enabled; with AFL, it generates and enables through the interface; the game opened by **Play solo** shows the portraits in the Mii grid and the Mii with face and helmet in the kart menu (`frame-01300/02100.png`); a second generation creates another folder and keeps the previous one. The launcher's `game.log` stays empty for the re-run runtime, so the captures are the evidence. Validation: formatting, Clippy, 33 Rust tests, `test_launcher_languages.py` and `test_mii_glb.py`. `tests/test_mii_launcher.py`, which looked for "Full Switch details" (text removed in commit `9fd7b3a`), now opens the 3D editor, edits/saves the Switch fields without AFL and closes the modal before exporting; it passes again.

**LAN room with two instances (2026-11-11):** `scripts/test-multiplayer.py --mii-resources local/mii-resource-nx-v2 --host-profile … --guest-profile …` configures the **Models inside the game** option of each launcher through the API itself (`POST /api/mii/resources`) and copies a different profile for each player. The first attempt stopped with `svcBreak` ("Mii data … missing"): the launcher passed the canonicalized folder with the Windows verbatim prefix (`\\?\`), which rejects the `/` separator appended by the runtime. This was fixed in `crates/installer/src/mii_resources.rs` with `runtime_path` (shared with `mii_native.rs`), and the test now checks the path with a slash. New run (`local/multiplayer-ve35ansp`): both final captures were reached in 197.7 s, with no exceptions in the logs. Inspected: **room garage with both 3D-body Miis** (Normal Suit) and faces; Mii portraits in the lobby list and on the voting cards; track voting without the previous crash in `f_700080f4a4`; the guest races as a Mii with the original helmet ("1st Race" intro and track). **Pending:** the garage Miis look like defaults (short hair) and not the A/C profiles; the lobby probably uses the account Mii, not `mii-profile.json`. It also remains to repeat this on two separate PCs.

**Map of the game's Mii areas (2026-11-11)**, from the strings and consumers in the 4.0.1 executable:

- **Validated with the original renderer and the generated resources:**
  - Mii selection with portraits (`MiiSele`);
  - Mii Suits (`MiiSuit`);
  - driver in the menu, at the presentation and in the race (`f_70000d4ffc` in `main_026.c`: `Driver`/`Head`/`Helmet`/`FaceAnim`);
  - LAN room garage with 3D bodies, plus portraits in the list and on the voting cards (`MiiWiFiMatchingHeap`, row `main_084.c`);
  - **Mii face icon on the minimap** of the race (`MiiMapHeap`; `local/multiplayer-ve35ansp/1/frame-09150.png` and `frame-10000.png`).
  - **Battle with Mii** (Balloon Battle, Battle Stadium/Wuhu, 1-minute rounds, 4 rounds, 11 CPUs): the script chooses Battle, the Mii and Normal Suit and reduces the round time (twice `LEFT`: 3 → 2 → 1 minute; the options wrap around). `local/race-time-trial-_x9uojkx` (475 s) and `local/race-time-trial-gjavz_tz` (529 s) finished with exit 0 and no exceptions. Inspected: **scoreboard of each round** with the Mii portrait on the "Player" row and the 3D Mii on the kart; face icon on the arena minimap; **final result** with the camera orbiting the 3D Mii, defeat expression (12th place) and cumulative ranking with portrait. The final result waits for `A`. Then the menu offers "View Results" / "View Highlight Reel" (`local/race-time-trial-3x8qk979`).
  - **MKTV (Highlight Reel)**: with `A` at the final result and then `DOWN` + `A` (`local/race-time-trial-dzqhx8k1`, 546 s, exit 0, no exceptions), the battle replay shows **close-ups of the 3D Mii** (face, helmet, Normal Suit) from several angles, with the caption "9th / Player" and the MKTV controls (Slo-mo, Menu).
- **Not yet reached by the scripts:**
  - ghost list with the author's icon (`GhostScrollList`, `P_GhostMiiDummy_00`, `UIHeap_MiiIconAuthor`): the `[input]` in `game.log` confirms that `DOWN`, `STICK_DOWN` and `A` reach the game. With the full cup/track sequence, `A` opens "Start Time Trial?" (`local/race-time-trial-rdrlazjk`), but the cursor never leaves "Solo Race". The test save has no ghost for the course, and the online options require a server; to have a ghost, a time trial must be completed. In that same run, the Mii face icon also appears on the time trial minimap. Note for scripts: the cup confirmation (2740) and track confirmation (2920) must remain before any menu navigation;
  - MKTV edit/save screens (`Theater_Remake`, `P_MKTV_Mii_00`) and race results (as opposed to battle): not yet traversed. The ghost list requires a completed time trial;
  - online screens (`WiFi_Mii`, `WiFi_MiiComment`, `MiiFriendCloseupHeap`): require a server.
- `main_108.c` (`f_70004c5664`, `mn_Background_00`) and `main_113.c` (Mii confirmation) request portraits through the same `f_70008104d4` already exercised by the lobby row.

**Limits:** a single mipmap level; Low/Mid and Linear/SRGB textures identical; no comparison with real MiiModel resources; the choice of eyes/eyebrows/mouths among the first N of the AFL was not compared piece by piece. LAN/online rooms, MKTV, a full race, restart/save and the launcher flow still need to be tested. The existing **Models inside the game** option accepts the folder, but its end-to-end test with this folder was not run. The static head overlay (phases 4–6) is unnecessary with the renderer active and conflicts with it. `gameplay_verified=false` and `multiplayer_verified=false`.


## Native female Mii — phase 6, 2026-11-11

The native conversion now accepts **female** Miis. The CharInfo gender (byte `0x28`) chooses the verified body: `MiiM.bfres` (SHA-256 `d2d4f099…568a`) for male and `MiiF.bfres` (SHA-256 `8fa6b46908450bb05d24a2a4607fdd205e51ee00f956abd33ea80368b5a4dfc8`) for female. A BfresLibrary probe confirmed that both have the same models (`Helmet`, `Mii`, `MiiFace`), 29 bones, `Head` at index 10 and identical transforms in the chain `Skl_Root → Spine1 → Spine2 → Head`. Therefore the calibrated transform `(x,y,z) → (y,z,x) * 0.1` holds for both.

**Finding in the game:** with `MK8_EXPERIMENTAL_NO_MII=1`, the game loads `Driver/MiiM.szs` for every Mii, even with a female profile. Proof: an overlay with only `MiiF.szs` (`local/race-time-trial-qz0_nyvm`) showed no head; an overlay with `MiiM.szs` of head A and `MiiF.szs` of head C, using the female profile (`local/race-time-trial-neb2uo1t`), showed head **A**. Thus, the female body with the converted head is served under the name `romfs/Driver/MiiM.szs`. The manifest records `body` (`MiiM`/`MiiF`) and `overlay_file="MiiM.szs"`. An active `MiiF.szs` file is refused by the launcher, since the game never reads it.

- `toolchain/mii_head_builder.cs` accepts both originals verified by hash and reports the body used. The local executable was recompiled; the previous one was kept as `local/mii-native-probe/MiiHeadBuilder-phase5.exe`.
- `toolchain/mii_native_head.py` chooses the body by gender and, before creating any output, refuses an original that does not match the gender. A gender other than 0/1 is refused.
- `crates/installer/src/mii_native.rs`: new optional field `original_female` in `mii-native-tools.json` (default `local/mii-native-probe/MiiF.bfres`). Old configurations remain valid, just without female support. `status` reports `body` and considers the tools ready according to the body of the selected profile. `publish` checks `body`, `overlay_file` and the hash of the corresponding original.
- The editor text (PT/EN) now reports male and female support.

**Native evidence inspected:**

- `local/race-time-trial-_kwzk43q`: female profile C (hair 12, skin 1), MiiF body served as `MiiM.szs`, exit 0 in 96.4 s, approximately 60 FPS. `frame-02000.png` shows the head in the kart menu; `frame-04800.png` shows the head on the track.
- `local/mii-native-launcher-cretj_th`: `scripts/test-mii-native-launcher.py --glb local/mii-composer-check/profile-C.glb`. Conversion through the editor button, real opening by **Play solo**, exit 0, head visible in `frame-03400.png`, plus the refusals of an outdated model, profile switch and unsupported part.
- `scripts/test-mii-composer.py` now also creates the female profile `Editor C` and exports `profile-C.glb`.

**Helmet/face on the ground (fixed):** the blue ball next to the rear wheel, seen at the presentation before the start, was the `Helmet` model of the BFRES. Behind it there was a black shape, which was the `MiiFace` model (`Mii_Dummy` textures). Both are defined in the space of the `Head` bone, but, with the Mii renderer disabled, the game draws them at the kart's origin, half buried in the asphalt. This also happens **without an overlay** (`local/race-time-trial-whybwj8w`). Diagnosis: collapsing only `Helmet` left the black shape (`local/race-time-trial-79ql9gdk`); collapsing `Helmet` and `MiiFace` showed the real side of the Standard Kart (`local/race-time-trial-1g0gt0_2`). The builder now collapses the vertices of these two models to a point, keeping models and materials for the game's queries, and checks the saved result. Evidence with builder `231a5bb5…`: female `local/race-time-trial-gfr9k06t` and male `local/race-time-trial-rf5baylc` (`frame-03300.png` side view, `frame-04800.png` track), plus `scripts/test-mii-native-launcher.py --glb …/profile-C.glb` approved again. From what is known of the original game, the Mii with Normal Suit races without a helmet; helmets appear in the special outfits, which are not handled here.

**Validation:** 13 Python tests of the converter/composition, Rust tests of installer/launcher (32), Clippy `-D warnings`, formatting, `test_mii_glb.py`, `test_launcher_languages.py` and the browser scripts above passed.

**Limits:** it remains a static replacement of the single resource the game loads. All Mii drivers in the session receive the same body and the same head; this also holds for the female body of a female profile. Bald heads, beard/glasses, expressions, portraits, per-player heads, multiplayer with an active head and a full race remain pending. `gameplay_verified=false` and `multiplayer_verified=false`. The shape of the female body is that of the original MiiF resource; there was no side-by-side comparison with the original Mii renderer.


## Substitute provider of Mii resources — mapping, 2026-11-11

**Chosen strategy:** do not fabricate the SDK's model object. Instead, generate **real resource files** in the Switch MiiModel format (`ShapeMid/High.dat`, `NXTexture{Low,Mid}{Linear,SRGB}.dat`) from the already verified local AFL (`AFLResHigh_2_3.dat`), and serve them through the existing path `MK8_MII_RESOURCE_DIR` with `MK8_EXPERIMENTAL_NO_MII=0`. That way, the game's original Mii renderer, with unchanged code and ABI, would assemble faces, bodies, portraits, rooms and MKTV for each player. The derived files remain private, like the AFL.

**Evidence in the 4.0.1 executable** (NSO decompressed locally; reference addresses, not qualified ABI):

- The Mii library is statically linked into `main`. Assembly `0x0100000000000802` (`f_70008e1e8c`); name table at `0x70012d6038` (`MiiResource:/NXTexture…`, `ShapeMid/High.dat`); full read by `f_70008e1fb0`/`f_70008e201c`; initialization `f_70008e1e08` → `f_70008ea6e0`, which only stores the buffers. No signature check was seen at load.
- **Shapes** (`f_70008ea9d4`): 12 types, entries of **0x58** bytes, in FFL order and with counts `[4 beard, 12 face, 12 mask, 132 normal hat, 132 cap hat, 132 normal forehead, 132 cap forehead, 132 normal hair, 132 cap hair, 1 glasses, 18 nose, 18 nose line]`. Tables start at 0x6c, 0x1cc, 0x5ec, 0xa0c, 0x376c, 0x64cc, 0x922c, 0xbf8c, 0xecec, 0x11a4c, 0x11aa4 and 0x120d4. There are two `u32[12]` vectors in the header, at +0x0c and +0x3c.
- **Textures** (`f_70008eabc8`): 11 types, entries of **0x1c** bytes, counts `[132, 62 eyes, 24 eyebrows, 2, 12 face, 12 makeup, 20 glasses, 2 moles, 37 mouths, 6 mustaches, 18 nose lines]`. Tables at 0x64, 0xed4, 0x159c, 0x183c, 0x1874, 0x19c4, 0x1b14, 0x1d44, 0x1d7c, 0x2188 and 0x2230. The header has `u32[11]` vectors at +0x0c and +0x38. `f_70008eab08` reads, from each entry, width u16 (+4), height u16 (+6), format u8 (+8, `nn::gfx` table at `0x7000f7668c`: 0x201, 0x901, 0xb01, 0x1d01, 0x1e01, 0x2001, 0x2d01) and mipmaps u8 (+9).
- **Local AFL confirmed:** 0x14-byte header (`FFRA`, big-endian); 0x10-byte entries (position, size, compressed size, level, window bits, memory, strategy); zlib data. Textures with 11 types `[3, 132, 80, 28, 12, 12, 20, 2, 37, 6, 18]`: the **232 non-empty parts decompress to the declared size**. Shapes in the same order as the 12 of the NX: 585 valid parts. The face entries 8–10 point to before the start of the data (0x4d00), that is, they are invalid in the AFL itself; they need explicit handling.

**Format decoded — continuation (2026-11-11):**

- Correction: in the table of counts, shapes and textures were swapped. Shapes: `f_70008ea9d4`, 12 types, entries of 0x58. Textures: `f_70008eabc8`, 11 types, entries of 0x1c, NX order `[hat 132, eye 62, eyebrow 24, beard 2, face 12, makeup 12, glasses 20, mole 2, mouth 37, mustache 6, nose line 18]`.
- NX entries: the first 0x10 bytes are position, size and compressed size. The data is zlib, decompressed by `nn::util::DecompressZlib`, with work area 0x1bf0, from the base of the file. **Shape (0x58):** five attribute offsets at +0x10 and sizes at +0x24; u16 index at +0x38/+0x3c (reversed when mirroring); bounding box at +0x40 (min.) and +0x4c (max.). Attribute 0 = position in 4× half precision (8 bytes); 1 = normal in 4 bytes; 2 = UV; 3 = 4 bytes (byte 0 is remapped when mirroring); 4 = not handled in the constructor. The face transforms (0x48 per face) are at +0x12704 and the hair transforms (0x24 per hair) at +0x14c24 of the shapes file. **Texture (0x1c):** at +0x10, width u16 (+4), height u16 (+6), format u8 (+8; indices 0/1/2 → `nn::gfx` 0x201 R8, 0x901 R8G8, 0xb01 RGBA8; 3–6 appear to be BC formats), mipmaps u8 (+9) and tiling mode u8 (+0xa → table `0x7000f20b00`: 0 optimal, 1 linear).
- Local AFL, with the layout confirmed by decompression: 0x14-byte header; texture `maxSize[11]`; **365 texture entries** in FFL order `[beard 3, hat 132, eye 80, eyebrow 28, face 12, makeup 12, glasses 20, mole 2, mouth 52, mustache 6, nose line 18]`, all with a 12-byte footer (mip, width, height, mipmaps, format 0 R8 / 1 RG8 / 2 RGBA8). At 0x1710 comes the shapes' `maxSize[12]`; the **shapes start at 0x1740**. Each AFL shape starts with a 0x48-byte header (six offsets, six sizes and bounding box), followed by float4 position, normal 10:10:10:2, UV float2, color, parameter and u16 indices.
- Differences to convert: position float4 → half precision; rearrange the AFL 0x48 header into the 0x58 entry; face/hair transforms in separate tables; for the textures, prefer linear mode, avoiding swizzle, if the runtime accepts it. The AFL has more eyes, eyebrows and mouths (extra expressions); the index mapping to the first N still needs visual verification.

**Next steps:** (1) decode the remaining fields of the NX entries (0x58 shape: attribute offsets/sizes, box and transforms; 0x1c texture: offset and mipmaps) from the consumers in `main`; (2) convert FFL attributes/indices to the NX layout and textures to the `nn::gfx` formats, including the Switch block swizzle, if required; (3) generate the six files with structural validation and tests; (4) test in the game with `MK8_EXPERIMENTAL_NO_MII=0`, first in the menu and solo race, then in LAN rooms with two instances. Until step 4 passes, no graphics consumer is reactivated.

## Mii with body in the game screens — investigation, 2026-11-11

The screens with a full-body Mii use `UI3D/MiiBodyMiddle/MiiBodyMiddle.bfres` (models `MaleBody`, `FemaleBody` and `MiiFace`; 16 bones, `head` at index 8, Y axis up, same scale as the driver's `MiiFace`). MKTV uses `UI3D/MiiBodyMiddleTheater/MiiBodyMiddleTheater.bfres`. In the decompressed 4.0.1 executable, the names are at `0x7000ee6751`/`0x7000ee6786` (body/theater) and `0x7000ee67db`/`0x7000ee6802` (`FemaleBody`/`MaleBody`). The loader of the resource is `f_70003e0d84` (`main_078.c`), called from `0x70003e08b0`. The 12+1 Mii instances of the interface (rooms/voting) are counted by the virtual methods +0x08/+0x10 in `main_085.c`. In no-Mii mode, `toolchain/experimental.py` forces these counts to zero and skips `f_70003e0ea0`, which avoids the known crashes in `f_700080f4a4`. Thus, these screens do not create the body; it is not only the head that is missing.

**Conclusion of the guest analysis (2026-11-11):** each Mii with a body is assembled by `f_70003e1310`, which uses one of the heaps `MiiWiFiMatchingHeap` (rooms), `MiiTheaterHeap` (MKTV) or `MiiFriendCloseupHeap`. This function obtains from the Mii manager the **SDK model object** through `f_700080f4a4` (stored at +0x2b0; it is the same function as the voting crash). Then `f_700081149c(model+0x10)` reads the gender through the SDK CharInfo accessors (`0x7000b53100`/`0x7000b54400`) to choose `FemaleBody`/`MaleBody` from the resource at +0x11a0. Finally, `f_7000810c90(model, "head")` attaches the face of that same model to the `head` bone, and the crown uses `/crown/Crown.bfres`. Therefore there is no "body only" path: creating these instances without the SDK model would require a fictitious object or empty pointers, which this project forbids. Legitimate paths: (1) MiiModel resources `0100000000000802` extracted from the console itself, with `MK8_EXPERIMENTAL_NO_MII=0` (path already implemented, not qualified); (2) a real substitute provider of the Mii model object (CharInfo + renderable meshes/textures with verified ABI), which is a large SDK reverse-engineering effort. No guest code was changed.

## Converted 3D Mii models in Multiplayer and Nextendo analysis (`fix-multiplayer`) — 2026-11-10

- **Audit of the Nextendo failure (`local/race-time-trial-cmcpfy8d`) and portrait protection (`f_700080f4a4`):** in the last live Nextendo test with a real login (`local/race-time-trial-cmcpfy8d`), the game authenticated OAuth + BAAS + NEX, went through `Worldwide` → `Race` → character/kart selection and entered the room at `"You're spectating. Get ready!"` (`frame-04500.png`), ending at frame ~4650 with `exception c0000005 accessing 0` in `f_700080f4a4+34c` called by `70000d51fc` (`main_026.c:316`). The full audit of `main_*.c` confirmed that there are only three call sites for `f_700080f4a4` in the entire game (`main_026.c:316`, `main_078.c:24271/25140` via `main_084.c:f_700041cb5c`, and `main_102.c:10515` in state 2 of `f_70004af884`), and all three are protected when `MK8_EXPERIMENTAL_NO_MII=1` in the current candidate (`1979f48120ee9d7035562438a4c2916901f57996275e47867b6187af8af8957b`, which includes `PORTRAIT_SCREEN_PATCHES` in `main_026.c`).
- **Interaction between converted 3D Mii heads (`MiiM.szs`) and Multiplayer/Nextendo:** the native conversion of phases 4 and 5 replaces the `Head` bone (index 10) in `romfs/Driver/MiiM.szs` while keeping `MK8_EXPERIMENTAL_NO_MII=1`. Thus, all the protections in `toolchain/experimental.py` against `MiiModel` firmware failures in portraits, globe and online menus remain active, while the 3D model of the Mii driver on the track shows the converted head of the profile.
- **Per-host TLS pin separation in Nextendo (`net_tls_pin_for_host` / `RECOMP_NEXTENDO_ZNC_TLS_PIN`):** the TLS inspection of `51.178.29.194:443` confirmed two distinct certificates served by SNI: `g2b309e01-lp1.s.n.srv.nintendo.net` (`CN=nintendo-private-server`, RSA-2048 SPKI SHA-256 `770551f13152f3d55370de6c039d3859cff0d39882f27c6c9fc6cfdb327d74b8`) and `api-lp1.znc.srv.nintendo.net` (`Issuer=Nextendo Network CA1`, `CN=accounts.nintendo.com`, RSA-2048 SPKI SHA-256 `eb22631902f276218368e59dfac73f656bf8391c356e6c37f079049db40d596f`). Implemented `net_tls_pin_for_host` in `toolchain/nextendo.py` (`net/netcfg.h`, `net/netcfg.c` and `services/ssl.c`) to require strict 32-byte equality against `RECOMP_NEXTENDO_ZNC_TLS_PIN` when configured for `api-lp1.znc.srv.nintendo.net`, preserving the default pin when omitted and without accepting generic certificates. Also added direct browser opening (`ShellExecuteA`) in `runtime/mk8/net/nextendo_probe.c` and `scripts/test-race.py`, recompiled the runtime (`--runtime-only`, candidate `74c68d423612d80b29de8d562d21a1a7b2ecfccb922c810f2479673b7bd2b047`) and validated the 9 tests in `tests/test_nextendo.py` (including the new native test `test_per_host_tls_pin_separation`).
- **Real in-game evidence (`local/multiplayer-uze8q270`):** added `--mii-profile` to `scripts/test-multiplayer.py` and ran a real test with two instances of the launcher and MK8 connected by the unique code `MK8P-…` using the profile and the converted 3D head of `local/mii-native-composer-a` (`330.7 s`, exit 0, `protected-data.json` with `unchanged: true`). The captures confirm: entry into the LAN garage without a portrait crash (`1/frame-05600.png`), synchronized track voting (`0/frame-07800.png`, `1/frame-07800.png`) and an active multiplayer race at Mario Kart Stadium with the guest driving the **converted 3D Mii (brown hair + Normal Suit)** right behind the host (`Host`, Mario) on the track (`1/frame-10000.png`). `gameplay_verified` and `multiplayer_verified` remain `false`.

## Conversion and application by the launcher — phase 5, 2026-11-10

The editor now offers **Save profile and model → Prepare and use in the game**. After the conversion finishes, **Play solo** applies the selected profile's head on the next launch. **Disable in the game** keeps the files; **Enable in the game** reuses a valid conversion. Changes made while the game is open only take effect on the next launch. The interface works in Portuguese and English.

`crates/installer/src/mii_native.rs` validates the saved GLB, full CharInfo, original BFRES, entry hashes and the output manifest. Each job uses a new private folder in `<data>/profiles/<id>/native/<job>/`, with `conversion.log` and output `model/romfs/Driver/MiiM.szs`. The profile's `native-head.json` receipt is only published after real success of the process and validation of the result. Switching/editing the profile during conversion, unsupported parts or converter failure do not enable an incorrect output. The existing supervision keeps the tool locks and cancels the job if the launcher exits; incomplete files are kept for diagnosis.

When the game is opened, the installer checks again the identity, integrity, location and exclusive content of the overlay. Outdated/corrupted active models prevent opening until they are prepared again or disabled. A profile without an active conversion does not inherit another's head. `RECOMP_MOD_DIR` is set only in the child process; an overlay already defined in the environment or active MiiModel resources cause an explicit conflict. `LARECOMP_SAVE_DIR` points to `<data>/user-no-mii/save`, keeping saves outside the overlay folder. The SDK Mii renderer remains disabled on this path.

**Local tools:** in this tree, the defaults are `local/python/Scripts/python.exe`, Pillow in `local/mii-library-research/python-libs`, `local/mii-native-probe/MiiM.bfres` and `local/mii-native-probe/MiiHeadBuilder.exe`. The builder must have been compiled from the current version of `toolchain/mii_head_builder.cs`, with the four BfresLibrary net48 DLLs next to the executable, as in phase 4. To use other paths, create `<data>/mii-native-tools.json` with absolute paths:

```json
{
  "python": "D:/Ferramentas/python/python.exe",
  "libraries": "D:/Ferramentas/python-libs",
  "original": "D:/DadosPrivados/MiiM.bfres",
  "original_female": "D:/DadosPrivados/MiiF.bfres",
  "builder": "D:/Ferramentas/mii/MiiHeadBuilder.exe"
}
```

`libraries` must contain Pillow's `PIL` package. The editor's AFL remains configured separately. The launcher does not download/extract these resources nor distribute private bodies, GLBs or tools. The CLI accepts `--python-libs` to load Pillow explicitly, since the supervisor removes inherited Python variables. The original BFRES remains restricted to SHA-256 `d2d4f0992ab12c0773e0a89add358576c2ceccff0450deb5053740f73c57568a`.

**Validation:** debug build, 31 Rust tests of installer/launcher, Clippy with `-D warnings`, formatting and 18 Python tests of conversion/composition/script passed. `python scripts/test-mii-composer.py` passed again. `python scripts/test-mii-native-launcher.py` exercised the compiled launcher with isolated data and saves: conversion button, real opening through the Play solo button, exit 0, refusal of an outdated model, deactivation, profile switch during conversion and a real unsupported-part error. Cache reactivation after a failure and the PT/EN controls passed in a complementary check, without creating another conversion job. The public script also includes the reactivation for future runs.

Private evidence in `local/mii-native-launcher-bs25s2jb`: `prepared.png`, `frame-02000.png`, `frame-03400.png`, `frame-04800.png`, `cached-english.png`, logs and `evidence.json`. The inspected images show the head in the menu and following the driver on the track. To repeat, first generate the GLBs with `scripts/test-mii-composer.py`, keep an already prepared local candidate and the private dependencies above; the native test copies the saves to its own test folder. No guest/runtime change was necessary in this phase.

**Current limits:** shared static replacement of `MiiM.szs`, male Normal Suit body only and parts accepted by the converter. Other drivers that use the same resource receive the same head. Launching into launcher multiplayer rooms is blocked while the head is active; this does not intercept manual entry into Online Play inside the game. There is no qualified support for female, bald heads without a hair mesh, additional beard/glasses parts, expressions, portraits, alternative outfits, independent per-player heads or switching during a match. The script reaches the barrier on the first lap: it does not qualify a full race, audio, physical controller, restart/save or global clock speed. `gameplay_verified=false` and `multiplayer_verified=false` remain. The next model step requires expanding the supported parts/bodies and validating each variant before per-player dynamic integration; the in-game modal remains pending.

## Editor heads in MK8 — phase 4, 2026-11-10

Two models created/saved by the launcher's editor were converted and drawn in the **kart menu and on the track of native MK8**. Profile A uses face 0, skin 2, hair 24; profile B uses face 4, skin 0, hair 33. They are distinct heads, with four and three opaque parts respectively (the second hair has no forehead piece). The bodies and animations still come from the male Normal Suit resource. The original Mii renderer was not enabled, and no guest/upstream code, original RomFS or personal profiles/saves were changed.

The new `toolchain/mii_native_head.py` reads the GLB generated by the launcher and identifies the parts through the FFL `modulateType` metadata. It validates CharInfo, envelope, buffers, indices, normals, limits, embedded PNGs and transforms; it refuses external files, extensions, animations and unsupported parts. It applies the scale/translation of the nose and nose line nodes before the conversion `(x,y,z) → (y,z,x) * 0.1` to the `Head` bone. It expands the mirrored face UV and composes mask/nose line into opaque maps, multiplying colors and blending transparency in linear space. It keeps sRGB maps, SNORM normal and neutral gloss from the previous path.

The C# builder was recompiled and now accepts the optional forehead, requiring face, nose and hair without weakening the existing validations. It still requires the hash of the original MK8 4.0.1 BFRES and `Head` at index 10. The packaging keeps the 4 MiB limit, the 4 GiB disk reserve and the refusal of existing outputs. The private output contains intermediates, the associated profile, BFRES, `romfs/Driver/MiiM.szs` and a manifest with hashes/scope; the converter also keeps the input GLB in new outputs. It is not an arbitrary GLB importer.

**Native evidence inspected:**

- A: overlay `local/mii-native-composer-a`, captures in `local/race-time-trial-8w4elf4s`, exit 0 in 97.7 s.
- B: overlay `local/mii-native-composer-b-short`, captures in `local/race-time-trial-g9zz2cok`, exit 0 in 97.3 s.
- In both, `frame-02000.png` shows face/hair in the menu; `frame-03400.png` and `frame-04800.png` show the head following the driver on the track. The final segments measured approximately 58–60 FPS, with earlier shader-compilation pauses. The long hair still shows uneven shading at the tips; there is no general visual equivalence with the FFL renderer.

The script accelerates to the barrier on the first lap: it is **not a full race**. Audio, physical controller, race save/restart, global clock speed or multiplayer were not qualified. `gameplay_verified=false` and `multiplayer_verified=false` remain in the manifests.

Additional validation: 18 Python tests passed (geometry, transforms, linear/sRGB color, composition, rejections, files without overwrite and script). The C# executable rejected an index outside the buffer before writing the BFRES. The launcher's Chromium test passed again and now exports both GLBs. `scripts/test-race.py --mii-profile` copies the given profile into the isolated session, making it possible to test each head without changing the active personal profile.

To reproduce with the private dependencies already available on this machine (the output folder must be new):

```powershell
$env:PYTHONPATH=(Resolve-Path local/mii-library-research/python-libs).Path
local/python/Scripts/python.exe toolchain/mii_native_head.py local/mii-composer-check/profile-B.glb local/nova-cabeca-nativa --original local/mii-native-probe/MiiM.bfres --builder local/mii-native-probe/MiiHeadBuilder.exe
local/python/Scripts/python.exe scripts/test-race.py time-trial --character mii --mii-profile local/nova-cabeca-nativa/mii-profile.json --last-frame 4800 --shots 3 --shot-step 1400 --timeout 180 --env "RECOMP_MOD_DIR=$((Resolve-Path local/nova-cabeca-nativa).Path)"
```

Before using the converter with a modified version of the builder, recompile `toolchain/mii_head_builder.cs` with the .NET Framework `csc.exe`, references `System.Web.Extensions`, `System.Core` and the four net48 DLLs of the private BfresLibrary; keep the DLLs next to the executable. The hash of the builder that was run is recorded in the manifest.

**Later integration:** the conversion and overlay selection by the launcher were implemented in phase 5 above. The replacement is still of the whole `MiiM.szs`: all drivers that share this resource may receive the same head. Female, bald heads without a hair mesh, beards/glasses with additional parts, animated expressions, portraits, alternative outfits and several simultaneous Miis require separate work. There is no model switching during a match and no modal inside the renderer.

## Mii editor and profiles — phase 3, 2026-11-10

The launcher now offers **Mii profile → Edit 3D Mii**. The user creates/selects an existing profile, provides the path of the verified local AFL, edits the 49 Switch fields with a preview of the real parts and saves profile + textured GLB. The head can be rotated, zoomed, reopened and exported. Creation/name remain in the main form; the modal does the full head editing. Height, build and administrative fields are preserved in the CharInfo, but do not change the isolated head preview. Interface in Portuguese/English. There is no modal inside the game's renderer.

`crates/installer/src/mii_composer.rs` keeps the configuration separate in `local/mii-composer.json`; it reads the AFL by size and known SHA-256 on every load/save. `local/mii-resources.json` and the original MiiModel consumers are not changed. The graphics resource is not embedded in the executable or in Git. Only FFL.js/WASM, its helpers/license and the GLTFExporter are public dependencies embedded in the launcher; provenance and hashes in `web/vendor/*/manifest.json` and `THIRD_PARTY.md`.

The models are kept in `local/profiles/<id>/assembled-<sha256>.glb`, with an `assembled-head.json` receipt containing full CharInfo, hashes and `native_integration_verified=false`. The previous procedural `head.glb` is preserved. Saving checks the expected profile, validates the GLB envelope, forbids external resources and requires a CharInfo link, normals and embedded images. Switching/editing the profile rejects a late save and invalidates the old receipt; previous files are kept. The derived file is written before the profile/receipt: a later failure is reported and may leave an orphan GLB, without replacing the previous model with partial content.

Validation: 30 Rust tests of installer/launcher passed; debug build and Clippy with `-D warnings` completed. `scripts/test-mii-composer.py` starts the real executable with a temporary `--data-dir`, exercises creation of two profiles, hair/skin/face, invalid range, saving, reopening, export, page reload, identity isolation, rejection of invalid GLB/resource and invalidation after editing. It checks languages, absence of external requests and browser errors. Private evidence in `local/mii-composer-check` (images, GLB and manifest). Personal profiles/saves are preserved.

**Next phase:** convert the heads generated per profile to the materials, coordinates and attachment of the native BFRES pipeline and test them in MK8. The previous native test covers only the static personal head; it was not repeated in this launcher change. There is no coverage of all styles/expressions, full body, switching during a match or multiplayer. `gameplay_verified` remains false.

## Mii head assembly — phase 2, 2026-11-10

Implemented a local prototype in `toolchain/mii_assembly.html`, separate from the launcher/runtime, which uses FFL.js/WASM and the AFL verified in phase 1. Two synthetic 88-byte CharInfo structures produce distinct heads, with selection of face shape, skin, hair, eyes and mouth. There is editing of the hair ID (0–131), texture composition, GLB export and reopening of the exported file. No personal profile was read or changed.

The assembly uses FFL's part selection and transforms, instead of treating the `shape_N.glb` sequence as editor style IDs. Fields exercised: shape at `0x2d`, skin `0x2e`, hair `0x31`, hair color `0x32`, eyes `0x34` and mouth `0x45`. Profile A: face 0, hair 33, eyes 2, mouth 23; profile B: face 4, hair 24, eyes 16, mouth 7. The additional edit to hair 12 passed; 132 was rejected. The API receives CharInfo directly, preserving the launcher's data contract. This still does not qualify all styles/fields.

The assembled heads include normals and materials, absent from the raw GLBs of phase 1. The masks are composed by the FFL shader and converted to RGBA textures for export. The auxiliary shader `TextureShaderMaterial` of the revision consulted failed to compile; `FFLShaderMaterial` was used for the composition, without modifying external sources. The linear→sRGB conversion of the pixels read from the render targets was fixed in the own adaptation, because the reopened GLB initially darkened the face details.

`scripts/test-mii-assembly.py` runs Chromium/Playwright with a temporary server limited to the test files on loopback, validates the FFL.js resource and revision and preserves the license next to the temporary copy. It exercises both profiles, normals, textures, distinct downloads, reopening by GLTFLoader, hair editing/rejection, console errors and absence of external requests. It compares the preview pixels with those of the reopened GLB, requiring a mean RGB difference below 0.1/255. The full test passed; the four library tests also passed.

Private evidence in `local/mii-assembly-check`: `profile-A.png`, `profile-B.png`, `export-A.png`, `export-B.png`, the two GLBs and `manifest.json` with hashes and pixel differences. Original/reopened images inspected. The manifest keeps `native_integration_verified=false`: **these two models have not yet been tested in MK8**. The earlier in-game test remains limited to the personal GLB, through the static overlay.

Reproduction on this machine: `python scripts/test-mii-assembly.py` (Python with Playwright/Chromium). Private prerequisites: the AFL from phase 1, a clean checkout `local/mii-library-research/FFL.js` at revision `4b62d7e6bb7b5ba3932b5236a2c05a2ef1b02fec` and `local/mii-library-research/GLTFExporter.js` from Three.js r183, hash recorded in `THIRD_PARTY.md` and verified by the test. The test does not download dependencies nor publish an external service.

**Next phase:** connect the composition to the launcher's editor/profile, with local import of the resource and model generation per identity; then adapt and test the generated heads in the native pipeline. The prototype is not a modal integrated into the game, does not enable loading during a match and does not include the library in the installer. Animated expressions, full style coverage and multiplayer remain pending. `gameplay_verified` remains false.

## Mii parts library — phase 1, 2026-11-10

Found and extracted a source of parts, without automating exports in the Mii Creator: `AFLResHigh_2_3.dat`, from Miitomo, obtained from the archived public address of the old CDN indicated by jaames/mii-assets and FFL-Testing. The file has 4,579,008 bytes and SHA-256 `4a4be71d75162c20b48720ef89cd3d4e6cd4e8e21bcbc2aed63ee08d96de7722`. It is not the NXTexture/Shape resource of the Switch SDK and must not be passed to the original firmware path as if it were equivalent.

Result in `local/mii-parts-library`: **585 GLB meshes with geometry, one empty entry (`shape_359.glb`) and 247 PNGs**. The file contains 859 mesh entries and 365 texture entries; entries without data produce no files. `manifest.json` records per-file hashes, counts, attributes, spatial bounds, dimensions and limitations. Headers/chunks, buffers, indices, finite positions and attribute counts were verified; all images were decoded. A sample of 48 textures was inspected in `texture-contact-sheet.png`, with eyes, eyebrows, glasses, mouths and other details. This validates the extraction/inventory, not the assembly or the drawing of these parts in MK8.

Added `toolchain/mii_library.py`: it receives an already obtained resource and external checkout, verifies hashes, the 4 GiB reserve and a new output; it adapts in memory two incompatibilities of the old extractor with current NumPy/Pillow, without editing the third-party code. The resource file and the six Python sources of the extractor are pinned by hash. The full test with the script ended with exit 0; four synthetic tests passed, covering geometry/bounds, truncation, invalid index and rejection of an unknown resource before creating the output. Dependencies used only in the private research folder: NumPy 2.5.4 and Pillow 13.0.0.

Local reproduction (choose an output folder that does not yet exist):

```powershell
$env:PYTHONPATH=(Resolve-Path local/mii-library-research/python-libs).Path
local/python/Scripts/python.exe toolchain/mii_library.py local/mii-library-research/AFLResHigh_2_3.dat local/mii-library-research/mii-assets local/nova-biblioteca-mii
```

**Phase 1 decision:** there is a source of parts and an offline extraction path; it is not necessary to export combinations in bulk from the site. Public access to `mii.nxw.pw/app` redirects to login, and the Creator's public repository identifies itself as an old version. No private endpoints were used and the user's account was not changed. FFL.js is a candidate for composing the parts from the profile data; it still requires the AFL/FFL resource.

**Next phase:** map IDs/variants, validate transform and materials, compose two different heads and test the result before connecting part selection to the launcher. The extracted GLBs include neither normals nor materials, and their UV convention needs to be handled. The current library remains private, with `editor_ready=false`, `native_integration_verified=false` and `distribution_license_verified=false`; it was not incorporated into the installer. The distribution license of the resources was not confirmed, nor was an explicit license for the archived extractor. References in `THIRD_PARTY.md`. The research also found a Fusion viewer, but the page identifies it as a demonstration, not a library; its code was not incorporated. No change to the runtime, saves or profiles; `gameplay_verified` remains false.


## Mii head in the native engine: fit and texture fixed — 2026-11-10

The personal export now appears in the **MK8 menu and on the track**, through the private overlay `local/mii-head-mod/romfs/Driver/MiiM.szs`. The head was incorporated into the male Mii body model, rigidly attached to the `Head` bone (index 10), keeping the original skeleton/animations. The original RomFS, profiles, saves, upstream and guest code were not changed; the policy without the original Mii renderer remains active. This is a static integration of one model, not qualification of a Mii library.

Fixes preserved in `toolchain/mii_head_bake.py` and `toolchain/mii_head_builder.cs`: local transform `(x,y,z) -> (x,z,-y)` with calibrated scale 7.14; expansion of the mirrored skin UVs from 0–2 to 0–1; static projection/composition of eyes, mouth, eyebrows and nose line into the opaque textures. The game's common material replaces the special Mii shaders that remained black. Color maps use sRGB, neutral normal uses SNORM and a neutral gloss mask avoids the metallic look inherited from the helmet. The original transparency was not qualified; the composition avoids depending on it for this fixed expression.

The C# builder validates the SHA-256 of the decompressed original BFRES (`d2d4f0992ab12c0773e0a89add358576c2ceccff0450deb5053740f73c57568a`), expected bone/model, names, dimensions, indices, normals and limits. The packager refuses a BFRES above 4 MiB and does not overwrite existing output. Textures are limited to 256 pixels; the private attempt with larger textures and a BFRES of approximately 9 MB failed when selecting the Mii, with no fully established internal cause. Do not treat the conservative limit as the engine's official budget. Conversion dependency/notices recorded in `THIRD_PARTY.md`.

**Final evidence:** `local/race-time-trial-4yvvjdnd`, exit 0 after 97.2 s; captures `frame-02000.png` (face in the menu), `frame-03400.png` and `frame-04800.png` (head on the track). Captures inspected: skin/eyes/mouth present and the head following the body; metallic gloss removed. The script accelerates to the barrier on the first lap, so it is **not a full race**. Final segments measured between 59.4–60 FPS, with a 40 ms peak in one interval; this does not qualify performance on other machines. `local/mii-head-mod/manifest.json` records hashes, scope and evidence, keeping `gameplay_verified=false` and `multiplayer_verified=false`.

Validation: C# builder compiled and its artifact run in the game; six geometry/composition/Yaz0 tests with synthetic data and the seven script tests passed. Reproducing the composition generates identical JSON and pixels; the builder executable rejected an index outside the buffer before creating the output. The script now forces `LARECOMP_SAVE_DIR` to the isolated copy, because a `RECOMP_MOD_DIR` could redirect saves into the overlay.

To repeat the already prepared local test:

```powershell
local/python/Scripts/python.exe scripts/test-race.py time-trial --character mii --last-frame 4800 --shots 3 --shot-step 1400 --timeout 180 --env "RECOMP_MOD_DIR=$((Resolve-Path local/mii-head-mod).Path)"
```

Reproduction of the conversion under development: the Python stage receives the private intermediate `head-small.json` (six parts, already scaled positions, GLB local axes and resized RGBA), not an arbitrary GLB. `python toolchain/mii_head_bake.py <intermediate.json> <new-folder>` produces `head.json` and composed maps. Compile `toolchain/mii_head_builder.cs` with the .NET Framework `csc.exe`, references `System.Web.Extensions`, `System.Core` and the four net48 DLLs of the verified BfresLibrary package; keep the DLLs next to the executable. Run `MiiHeadBuilder.exe <MiiM-original.bfres> <new-folder/head.json> <new-output.bfres>`; then `python toolchain/mii_head_bake.py --pack <new-output.bfres> <overlay/romfs/Driver/MiiM.szs>`. Output folders must exist for packaging; existing outputs are refused. The input files, intermediates, DLLs and derived resources remain private.

**Limits and next work:** it replaces the whole male Normal Suit body; other Miis that use this resource may receive the same head. There is no head choice by identity/profile, general importer in the launcher, dynamic expressions, integrated portraits or qualified female support. The GLB contains one finished Mii, not all hairs/eyes/shapes. The launcher already saves several profiles and the runtime menu can bring it to the front; it still lacks associating a native resource with each profile and implementing the requested modal editor. A complete editor needs its own parts library or one imported locally, composition of meshes/materials and the same conversion used by the game. The initial safe switch should occur at the next launch of the game; loading during a match and multiplayer require separate work and tests. Do not confuse the existing approximate procedural editor with this library.

## Mii integration: reference race and native consumers — 2026-11-10

Ran the real path of selecting the first Mii, Normal Suit and Time Trial at Mario Kart Stadium, with isolated copies of the saves/profile. `local/race-time-trial-ps9fgizm/frame-04200.png` shows the blue Mii body and kart on the track, **without a head**. This confirms that the driver's body/animation can be kept; it does not confirm GLB loading. The test accelerates without driving and ends next to the barrier, on the first lap; it is not a full race.

Fixed `scripts/test-race.py`: copying `mii-profile.json` alone did not provide the data to the Mii service. It now passes identity/appearance and, when present, CharInfo with the same name/color/type overlays as the launcher; it preserves imported fields and does not modify the original profile. `--character mii` reproduces the extra dialogs of this selection, limited for now to Time Trial/Stadium. The receipt records character, profile hash and `model_rendering_verified=false`. The graphics mode without Mii remains explicit; this script does not yet accept/install a GLB in the game.

Added `MK8_TRACE_MII_CONSUMERS=1` to the runtime: it records at most 64 distinct call sites of the no-models policy, without accessing guest structures, altering returns or blocking another drawing thread. The run identified preparation `0x700080fa44`, update `0x7000810278`, query `0x700041a834`, worker `0x700041a6b0`, portraits `0x700041b2f0` and the already protected selection/confirmation consumers. Reading the verified generated code also located `0x7000815724`, which prepares textures named `MiiCharModelTexture`, called by `0x7000814a04`. These addresses are investigation references for the 4.0.1 profile, **not a validated substitute rendering ABI**. Upstream and generated files were not changed.

Validation: runtime recompiled with `--runtime-only`, link and dependencies completed; 11 layout/host tests (including deduplication/limit of the diagnostic and preservation of the policy) and seven script tests passed. Repeat with the updated code: `local/race-time-trial-_ap1xdzt`, exit 0 after 98.2 s, capture `frame-04800.png` inspected, body present and head absent. There was no crash in this run. Build/test logs in `local/mii-integration-build.log` and `local/mii-integration-host-tests.log`.

Reproduction: `local/python/Scripts/python.exe scripts/test-race.py time-trial --character mii --last-frame 4800 --shots 3 --shot-step 1200 --timeout 180 --env MK8_TRACE_MII_CONSUMERS=1`.

**Pending objective:** connect the GLB's head/textures to the animated neck transform and to the native drawing pass. This contract and the lifetime of the graphics resources are still to be established; there is no GLB loader connected to the game's renderer at this stage. Do not re-enable the original preparation without the required resources, nor return an empty object as a substitute. Portraits, expressions, a full race and multiplayer remain pending; `gameplay_verified` remains false.

## Inspection of the personal Mii Creator export — 2026-11-10

The two most recent files provided in Downloads were `Test_all_body_2026-11-10T21_43_21.682Z.glb` (281,044 bytes) and `Test.miic` (140 bytes). The GLB 2.0 contains eight meshes, 3,423 triangles, three embedded PNG textures and a skeleton with 31 joints. The head parts are separate (`Faceline`, `Nose`, `Forehead`, `Hair`, `Mask`, `Noseline`); the body uses skinning. There are no animation clips. `asset.extras.miiData` matches the provided `.miic` byte for byte. This is real geometry of one character, not a library of all Mii parts.

Implemented in the launcher: **Mii profile → Inspect exported GLB**, with body/head, rotation/zoom, mesh, skeleton, inventory and comparison with the `.miic`. Three.js is packaged locally under the MIT license; there is no CDN and no data is sent. The viewer accepts a self-contained GLB 2.0 of up to 8 MiB, limits JSON, collections, hierarchy, accessors and PNG dimensions, and refuses external URIs and unsupported extensions. A texture error prevents announcing completed loading. It does not write profile/save, does not change the Downloads files and does not enable guest graphics consumers.

The initial inspection detected textures blocked by the content policy, even though the geometry loaded. The policy was fixed to allow local blobs; the synthetic test now includes a PNG texture and checks console errors. Final captures `local/mii-preview-ui/export-full.png`, `export-head.png` and `export-wire.png` were generated; body and head were inspected. Face, eyes, hair and clothing appear with the file's textures. The visible session uses the debug launcher and keeps the files only in the browser's memory.

Validation: debug and release builds completed; formatting and Clippy with warnings treated as errors passed. The new test `tests/test_mii_glb.py` renders self-authored geometry/PNG, toggles head/mesh/skeleton, compares equal/different data, refuses an external URI, a cycle, accessor/buffer limits and a truncated file, and reloads a valid model after errors. The Mii editor test and the two language tests also passed. The private check of the maintainer's GLB confirmed rendering and data match, without external requests. No personal model was added to fixtures or to the package.

The 44 Rust tests of the workspace also passed after the last change. When a test in MK8 was requested, the audit confirmed that `runtime/mk8` has no GLB consumer: `toolchain/host.py` only mounts the MiiModel title through `MK8_MII_RESOURCE_DIR`, and the guest graphics preparation remains conditioned on `mk8_no_mii_renderer()`. The browser's GLB is not passed to the game process. Thus, a race as a test of this model was neither announced nor run. The next necessary implementation is a native bridge for head/materials for a single profile in solo, with call sites and ABI verified before the guest recompilation; only then is it appropriate to move on to portraits, expressions and multiplayer.

Limit confirmed by public code: the old decoder uses another layout; the new `.miic` is not an 88-byte CharInfo and was not converted by assumption. The visualization does not start generating new styles from this export. Integration into the game still requires loading buffers/materials, calibrating the neck matrix and scale, adapting portraits/expressions and the lifetime of resources. The exported generic body does not automatically replace the MK8 driver's body/animations. No race with this model was run; `gameplay_verified` remains false. References and licenses in [THIRD_PARTY.md](../THIRD_PARTY.md).

## Mii profiles and experimental path for real models — 2026-11-10

Research on Tomodachi LTD Recomp, Mush8, FFL.js/FFL-Testing and Nextendo/NX Native documentation, recorded with revisions and limitations in [THIRD_PARTY.md](../THIRD_PARTY.md). Tomodachi uses another runtime and its own game's editor. Mush8 demonstrates the most useful separation for this base: CharInfo data in the Mii service, firmware resources for the head drawn by the SDK, and login/token/network for Nextendo. No component was found that, just by adding a GLB, enables MK8's online play.

**Implemented in the local sources:** the **Mii profile** tab restores creation, editing, selection and persistent identity. The simple editor offers name, color, type, face, skin, hair, eyes, glasses, height and build. "Full Switch details" exposes the 49 CharInfo fields, including nose, mouth, eyebrows, beard, mustache and positions/scales. Import/export of 88-byte `.charinfo`/`.ufsd` validates size, name, identity and domains before saving. Imported fields outside the simple editor remain intact when renaming or changing other controls; saving details keeps the ID and refuses a different active profile. `.storedata`, NFDB databases and QR codes are not accepted at this stage. The account/room name remains separate from the Mii name; there is no new Nintendo account or save migration.

The WebGL preview uses the project's original procedural geometry, with rotation, and is labeled **approximate**. It is not the Nintendo renderer and does not represent all imported styles. The derived GLB file remains a preview, not a system resource. The handoff to the game uses the full 88 bytes in `MK8_MII_CHARINFO`, revalidated by the runtime before answering the Mii data query. The database presents the active profile and the six existing presets. Old profiles without `charinfo` remain readable. Saves remain in `user-no-mii`; the historical name of the folder does not decide the graphics mode.

The runtime menu gained **Mii profile**, which brings the launcher to the front. The Mii applet can also request this window and returns cancellation, without inventing a result of completed editing or overwriting the fields imported by the legacy editor. Changes take effect on the **next launch of the game**: there is no update of models/account in an active match. The launcher window must remain open to receive the runtime's request.

**Real models:** the experimental option receives the folder extracted from the system title `0100000000000802`, with `ShapeMid.dat`, `ShapeHigh.dat`, `NXTextureLowLinear.dat`, `NXTextureLowSRGB.dat`, `NXTextureMidLinear.dat` and `NXTextureMidSRGB.dat`. It records the size and SHA-256 of each file and checks them before starting the game. It refuses absence, basic truncation and headers without parts; this screening is not a full validation of the format by the SDK. The runtime mounts this folder only for the MiiModel title, through the existing storage/RomFS service. When the configuration is enabled and intact, the launcher passes `MK8_EXPERIMENTAL_NO_MII=0`, allowing the existing conditional hooks to run the original preparation, portraits and models. Disabling the option restores the no-models mode. No empty file, fictitious pointer, firmware or resource from another game was provided.

### Validation performed in this implementation

Build of the development launcher (`target/debug/Launcher.exe`) and of the Windows host completed; the host compiled 212 units and linked. The 44 Rust tests of the workspace, `cargo fmt --all -- --check` and Clippy with warnings treated as errors passed. The three native tests of Mii data/storage mounting, the 12 tests of the experimental hooks and the 46 regression tests of `scripts/verify-graphics.py` also passed, with no skips in that suite.

The end-to-end test `tests/test_mii_launcher.py` passed with the real launcher and Chromium: creation, ID persistence, import preserving styles, detailed editing, byte-for-byte export, rejection of invalid data, refusal of missing resources and editing without WebGL. The capture `local/performance-validation/mii-editor.png` was inspected. The two language tests passed. In the general suite `tests/test_launcher_e2e.py`, 15 tests passed and one was skipped because the relay executable was not configured. These tests do not run matches and do not validate the commercial SDK's Mii renderer.

SHA-256 of the development launcher: `8aad4a7705126c4dfd4c2ff561c71b7b4b1cc8919210884549a73501c3219c11`. SHA-256 of the host `local/performance-validation/host/mk8drecomp.exe`: `4b25901c779d350d31a8695387e5ba2688ba767a4d7219c2cc71b5ec2c014a23`. The host is a development build without the recompiled game library; it is not an independent playable package. No installer was generated at this stage.

### Testing Mii on another machine

1. Recompile the launcher from these sources and the host with `--runtime-only` on the already prepared MK8 4.0.1 installation, following the procedure below. Do not swap only the EXE of an installation with an old runtime.
2. Open **Mii profile**, create a profile, save, close/reopen the launcher and check the ID. Export `.charinfo`; import a different Mii; verify that renaming and editing one field does not change the others. Importing an already existing ID is refused to preserve the previous profile.
3. Prepare the extraction of the MiiModel title from the firmware itself. This screen receives the already extracted folder, not a collection of NCAs or a GLB. Enter that folder in **Models inside the game — experimental**, check the option and save. The files are read in place, without replacing private data.
4. Start a new session, check the Miis in the menu, portraits, body, head, neck fit and animations, then play with the Mii in a solo race, return to the title and restart. Verify lighting, expressions, names, save and absence of null accesses. Test a profile change in a following session and the runtime menu's **Mii profile** button. If there is a graphics failure, disable the option in the launcher, preserve `game.log` and compare with the no-models mode.
5. Repeat on LAN/relay between two machines, with distinct profiles, observing the head and portrait of the other player, room entry/exit and track change. Record VRAM usage and frame time in long sessions: Mii rendering adds work and GPU resources not yet measured here.

**Current limits:** the user confirmed that no Mii firmware/resources or prepared installation is available for this test. Therefore heads/portraits/animations in MK8 and acceptance by the real game's SDK remain unqualified, even though the integration path is implemented. There is no integrated Nextendo authentication, NEX token, friends or matchmaking. Displaying/validating a Mii does not prove acceptance by the server; the client needs its own integration and authorized OAuth identity, not the identity of another project. `gameplay_verified` remains false. No release/installer was published.

## Local fix for the portrait request — 0.1.9-test.1

The screen in `main_026.c` requested `f_700080f4a4` even with the portrait manager without resources. The adaptation in `toolchain/experimental.py` validates the SHA-256 of the MK8 4.0.1 unit and two boundaries: in no-renderer mode, with the manager absent or the list count/pointer both zero, it omits the reservation/request block and keeps the portrait field null. The two presentation branches that consumed this portrait are also omitted for types 0x1d/0x34, proceeding to the screen's common path. The existing release already accepts a null portrait. No visual object or network success is fabricated; present resources and other types keep the original behavior.

A synthetic native test runs both types, another type, normal/experimental mode, manager absent/present and combinations of count/pointer, verifying request, consumption and continuation of the screen. Two voting/portrait tests and 12 tests of the other adaptations passed; real unit recompiled and consumer libraries relinked. Full compilation completed in 97.7 s after using the private Python with its dependencies (the initial attempt with the system Python stopped for lack of unicorn). Reports now include the runtime logs; two Rust tests, formatting and Clippy passed. The private version 0.1.9-test.1 distinguishes this build and requests a new preparation on the PCs with 0.1.8. No tag/release on GitHub. Qualification on two machines and `gameplay_verified` remain pending.

Real validation in `local/multiplayer-j2sdcvxw`: two launchers and two games with isolated copies of the saves. Captures 6600 show the voting of both participants; captures 9200 show Mario and Luigi on the same track, with the host visible to the guest, positions 1st/2nd and lap 1/3. The test ended after 211.1 s without the reported crash. It did not complete the race nor reproduce two machines/VPN. `protected-data.json` confirmed save preservation and the external check of the 12 protected files kept all hashes.

Setup completed: `target/dist/MK8-online-0.1.9-test.1-setup.exe` (55,750,656 bytes), SHA-256 `4b5f94c7b82c4f63c87e316a1a761012401ee8adf3739ac7d85e42c877112181`. Shareable package `MK8-online-0.1.9-test.1-test-package.zip` includes instructions, checksum and FFmpeg sources/notices. Payload verified with 916 files, portrait adaptation present and no private entries. Two browser tests against the packaged executable passed (invites/connection and report/updates). Installing and preparing the build again on both PCs is mandatory; do not delete data or saves.
