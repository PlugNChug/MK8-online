# Windows boot: missing sdk dispatch entry at 0x7004004c30 — 20/11/2026

## Result

| Check | Result | Evidence |
| --- | --- | --- |
| Failure reproduced under Wine 11.22 | **PASS** | `FATAL: no function at 7004004c30` before the first frame; `$HOME/mk8-win-boot/before-wine.stdout` |
| Root cause identified | **PASS** | `nndetailCryptoBignumMulAddWords` is a defined `STT_NOTYPE` symbol at sdk `0x4c30`; the generator only accepted `STT_FUNC` |
| General recompiler fix | **PASS** | `runtime/prepare.py` accepts sized `STT_NOTYPE` text symbols; synthetic NSO test fails on the pinned generator and passes on the prepared one |
| Windows runtime reaches the first rendered frame | **PASS** | `[vk] t=9.4 frame 6`, `t=103.3 frame 437`; exit 0 after capture |
| Windows runtime reaches an interactive screen | **PASS** | title `Press L R to start`, then `Select a Mii`; `win-shot-00450.png`, `menu-win2.png`, `menu-win.png` |
| Windows runtime reaches the in-game main menu | **UNVERIFIED** | the no-Mii data root asks for a Mii before the main menu; see below |
| Proton-GE 11-12 first frame and menu | **PASS** | `[vk] t=17.1 frame 4`; `Select a Mii` in `proton-menu2.png` |
| Linux regression (new generated table) | **PASS** | old and new `mk8dgame.so` both boot and reach `Select a Mii`; no FATAL |
| Real Windows/MSVC | **NOT RUN** | GNU target under Wine/Proton only |

`gameplay_verified` stays **false**. No race, audio, controls, restart/save or clock-speed qualification was attempted.

Commits: recompiler fix `4318c07` (`fix(recompiler): register sized STT_NOTYPE text symbols as
function entries`); this report and the `STATUS.md` update are the follow-up docs commit.

The old Windows DLL used for the "before" run is preserved at
`local/run-fresh/mk8d-code-sdk.dll`, 24,489,984 bytes, SHA-256
`4cdd239f63493afee0026a2a70b1ae0cf0329dfd3ca3983b7ac1d23b8b1e6da2` (with the host
`mk8drecomp.exe` `53e687472d0a18f1ecd9be6980b3e356480524f88a80441eb61feadd30591829`). The
`local/run-win` copy was rebuilt at 02:29 with the same fix (identical sdk DLL SHA-256) while this
session ran; the before log predates that rebuild.

## The failure

Same data, same runtime host, old Windows game library (preserved at
`local/run-fresh/mk8d-code-sdk.dll`, sdk function table 28228 entries):

```sh
cd /home/mitchell/Projects/MK8-online-windows-verify/local/run-fresh
MK8_EXPERIMENTAL_NO_MII=1 WINEDEBUG=-all timeout 45 wine ./mk8drecomp.exe --play
```

```text
[input] GetNpadStyleSet id 0x3
FATAL: no function at 7004004c30 (lr=7004002964 _ZN2nn6crypto6detail15bigint_mod_multEPjPKjS4_S4_i+d0)
```

The register dump printed by the runtime identifies the call mechanism:

```text
x16 00000070049872f0  x17 0000007004004c30
x30 0000007004002964
guest frames: 7004002c28 7004001e20 7000864308 7000864298 700086334c 7000862a5c
              7000862968 7000618d78 7000618610 7004091b48 7004095b24
```

## RE evidence

Inputs: private MK8 4.0.1 `exefs/sdk`, SHA-256
`427404f3218eaa790f611acd9e45fe0336efb9504f02949054b14de3578bd1c2`,
sdk base `0x7004000000`. Disassembly with Capstone 5.0.7 on the decompressed NSO image.

### What 0x7004004c30 is

`0x7004004c30` is **not** an interior branch target of a larger function. It is the entry of a real
leaf function, `nndetailCryptoBignumMulAddWords`, 172 bytes, ending at `0x4cdc`:

```text
7004004c30: mov   x15, xzr
7004004c34: cbz   x2, 0x7004004cd4
7004004c38: lsr   x6, x2, #2
7004004c3c: cbz   x6, 0x7004004c8c
7004004c40: sub   x2, x2, x6, lsl #2
7004004c44: stp   x16, x17, [sp, #-0x10]!
7004004c48: ldp   w4, w5, [x1], #8
...
7004004c58: umaddl x4, w3, w4, x8        ; a[i] = dest[i] + multiplier * src[i] + carry
...
7004004cd4: lsr   x0, x15, #0x20
7004004cd8: ret
```

After the fix, the preceding function `f_7004004ae8` ends at `0x4c30` (its last instruction is
`b 0x70043e9730` at `0x4c2c`), so the binary itself treats `0x4c30` as a separate entry.

### Why the call lands there

The call site in `nn::crypto::detail::bigint_mod_mult` is the module's own PLT thunk:

```text
7004002950: ldr  w3, [x25], #4
7004002954: mov  x0, x27
7004002958: mov  x1, x24
700400295c: mov  w2, w26
7004002960: bl   0x70043e95e0
7004002964: ldr  w8, [x27, x19]          ; lr = 0x7004002964 (+0xd0)
```

The thunk at `0x70043e95e0` loads a GOT slot and tail-branches through it:

```text
70043e95e0: adrp x16, 0x7004987000
70043e95e4: ldr  x17, [x16, #0x2f0]
70043e95e8: add  x16, x16, #0x2f0
70043e95ec: br   x17
```

That GOT slot is the `R_AARCH64_JUMP_SLOT` relocation at image offset `0x9872f0` for
`nndetailCryptoBignumMulAddWords`. The dynsym entry is:

```text
index 9184  name nndetailCryptoBignumMulAddWords  value 0x4c30  size 172
info 0x10 (STB_GLOBAL, STT_NOTYPE)  shndx 1 (.text)
```

The loader resolved the relocation to `base + 0x4c30`, which is why `x17` is `0x7004004c30`.
This is correct binding; the dispatch table simply had no entry at that address.

### Why the table lacked it

The pinned generator (`tools/gen.py`, `Module.boundaries`) accepted only `STT_FUNC` symbols:

```python
for name, info, shndx, value, size in n.syms:
    if shndx and (info & 0xF) == 2:      # STT_FUNC only
        starts.add(b + value)
```

The sdk's crypto helpers are `STT_NOTYPE` (assembly sources without `.type`). A scan of all defined
symbols in sdk text found exactly seven non-`STT_FUNC` entries:

| Address | Size | Type | Name |
| --- | --- | --- | --- |
| `0x7004004c30` | 172 | NOTYPE | `nndetailCryptoBignumMulAddWords` |
| `0x7004004cdc` | 148 | NOTYPE | `nndetailCryptoBignumMulWords` |
| `0x7004004d70` | 208 | NOTYPE | `nndetailCryptoBignumAddWords` |
| `0x7004004e40` | 212 | NOTYPE | `nndetailCryptoBignumSubWords` |
| `0x7004004f14` | 884 | NOTYPE | `nndetailCryptoBignum_mul_comba8` |
| `0x7004005288` | 772 | NOTYPE | `nndetailCryptoBignum_sqr_comba8` |
| `0x7004257c10` | 0 | NOTYPE | `__nnDetailNintendoSdkNsoFile` (marker, not a function) |

`main` and `subsdk0` have no such symbols. Because none of the six helpers is a direct `bl`
target (every call goes through the module PLT), the whole range
`0x4ae8..0x558c` was emitted as one function. `0x4c30` was not a label, so
`dispatch_lookup` could not enter it.

Generated table before (28228 entries):

```text
0x7004004a9c
0x7004004ae8     <- one function through 0x4c10 ... 0x558c
0x700400558c
```

After the fix (28234 entries):

```text
0x7004004a9c
0x7004004ae8
0x7004004c30     <- nndetailCryptoBignumMulAddWords
0x7004004cdc
0x7004004d70
0x7004004e40
0x7004004f14
0x7004005288
0x700400558c
```

The six new boundaries partition the old body exactly: every symbol's `value + size` equals the next
boundary, ending at the existing `0x558c` boundary. No gap or overlap was introduced.

`toolchain/sdk_exports.py` (the earlier hash-gated supplement for these six exports) is now a
**no-op**: called with the real sdk beside the prepared runtime, it finds every
`nndetailCryptoBignum*` address already declared in the generated table and returns `None`.

### Why Linux did not fail

The existing Linux qualification build linked the **supplemented** table produced by
`toolchain/sdk_exports.py` (28234 entries, including `0x4c30`), because its job layout has
`<job>/inputs/exefs/sdk` next to the prepared runtime. The Windows cross build used
`source = local/prepared`, whose parent has no `inputs/exefs/sdk`; `sdk_exports.supplement()`
silently returned `None`, so the DLL linked the raw generated table (28228). The Linux table
therefore contains the entry and cannot reproduce the FATAL; whether the Linux guest calls
`bigint_mod_mult` in this boot is not instrumented, and it would succeed if it did. The general fix
removes that layout dependency: the entry now comes from the generator itself, so every build and
module gets the same table.

## Fix

`runtime/prepare.py` (the bounded upstream adapter, not `upstream/`):

```python
def symbol_function_entry(info, size):
    kind = info & 0xF
    return kind == 2 or (kind == 0 and size > 0)
```

`adapt_recompiler()` inserts this helper into the prepared `tools/gen.py` and replaces the
`STT_FUNC`-only condition with `shndx and symbol_function_entry(info, size)`. The caller still
filters every candidate through `Module.in_text()`, so data symbols and the zero-sized
`__nnDetailNintendoSdkNsoFile` marker are rejected. No address, name or game hash is special-cased;
any defined text symbol with a size becomes an entry on every module.

The rule is not gated on a game hash, so it applies to every module. A symbol outside the
executable range is still ignored by `Module.in_text()`; zero-sized markers and `STT_OBJECT`
symbols are rejected. The remaining assumption is that a sized symbol whose value lies in `.text`
describes code, which holds for every defined symbol in these three modules.

Operationally this changes the prepared `tools/gen.py` and the generated sdk table, so an existing
build must run a full Build (regenerate and relink the guest); `--runtime-only` is not enough. The
launcher's normal hash checks force the re-preparation. `local/config.json`, profiles, saves and
`local/user*` are not touched.

### Test

`tests/test_pipeline.py::PipelineTests::test_sized_notype_text_symbols_become_dispatch_entries`
builds an authored NSO with three functions:

- `0x20` `STT_FUNC`, size 8;
- `0x40` `STT_NOTYPE`, size 8;
- `0x60` `STT_NOTYPE`, size 0 (marker).

It runs the **prepared** `tools/gen.py` and asserts the table contains `0x20` and `0x40`, does not
contain `0x60`, and that `f_7004000040` is emitted. Negative control: the pinned upstream generator
on the same NSO reports `1 boundaries, 1 functions` and no `0x40` entry; the prepared one reports
`2 boundaries, 2 functions`. The full pipeline suite passes (10 tests).

## Build

Prepared and generated from this tree at the commits in the table below:

```sh
JOB=$HOME/mk8-win-boot/job
python runtime/prepare.py --upstream upstream/smm2recomp --output "$JOB/runtime" \
  --profile games/mk8d/profiles/4.0.1.json
python toolchain/recompiler/generate.py --source "$JOB/runtime" \
  --exefs local/qualification/imports/mk8-4.0.1/exefs \
  --output "$JOB/generated" --profile games/mk8d/profiles/4.0.1.json
```

`sdk: 28234 functions, 122 chunks`; `main: 57034 functions`; `subsdk0` unchanged. Compared with the
pre-fix generated tree, **only** `sdk/sdk_000.c` and `sdk/sdk_table.c` differ; all 243 `main` and 116
`subsdk0` files are byte-identical, and so are the other 121 sdk chunk files.

Windows guest (Zig 0.17.0, `x86_64-windows-gnu`, `-mcpu=x86_64_v2`, `-O1`), same platform switch as
the documented `local/cross_game.py`; driver kept outside the repo at
`$HOME/mk8-win-boot/cross_win.py`:

```sh
MK8_REPO=$PWD MK8_ZIG=/…/zig-x86_64-linux-0.17.0/zig \
MK8_WIN_TOOLS=/…/local/native-tools \
python "$HOME/mk8-win-boot/cross_win.py" "$JOB/runtime" "$JOB/generated" \
  "$JOB/native-win" "$JOB/object-cache-win" 4
```

Artifacts:

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `mk8dgame.dll` | 535,040 | `a56b2263277c5f78df84f908cd244a5dbd09f487b87b6b81c7b170146a6e0468` |
| `mk8d-code-main.dll` | 75,041,792 | `84a1592b154041a55dd411eb081d749eb74ab4f9e0ce44474f53e38798d08fdc` |
| `mk8d-code-sdk.dll` | 24,498,688 | `0d80585d17c978423ca69aac10082e15b8e8f025bd2ed8ae783cf29b77153cb0` |
| `mk8d-code-subsdk0.dll` | 19,187,712 | `cceea3cc74910b779f92fcd858cc8d5477bc16353ce785f1e4cec203078b02a9` |

The linked DLL's exported `sdk_fn_count` is **28234** and `sdk_fn_addrs[73]` is `0x7004004c30`
(verified by reading the PE export table and `.lrodata`). The old DLL (`local/run-fresh`) is 28228
and has no `0x4c30`.

Linux control library (`x86_64-linux-gnu`, clang 23.1.5, `-O1`):

```sh
python "$HOME/mk8-win-boot/build_linux.py" "$JOB/runtime" "$JOB/generated" \
  "$JOB/native-linux" "$JOB/object-cache-linux" 5
```

`mk8dgame.so`, 159,340,120 bytes, SHA-256
`e0423aa61558baca04e8687b36b1a052761ed6bcd6ac6adc7011e3a845ddc6f4`; exported sdk table
28234 entries with `0x4c30`.

## Runs

### Windows, Wine 11.22 — first frame

```sh
cd $HOME/mk8-win-boot/run-win
WINEPREFIX=$HOME/mk8-win-boot/wineprefix MK8_EXPERIMENTAL_NO_MII=1 WINEDEBUG=-all \
  LARECOMP_WINDOW_SHOT=150:450:150 \
  LARECOMP_SHOT_PATH='Z:/home/mitchell/mk8-win-boot/win-shot-%05d.png' \
  LARECOMP_EXIT_AFTER_SHOT=1 timeout 220 wine ./mk8drecomp.exe --play
```

Exit 0 after the last capture. The FATAL line is absent:

```text
[vk] t=9.4 frame 6 1.1 fps max 2775ms level … (25 draws …)
[vk] t=23.5 frame 44 4.2 fps …
[vk] t=103.3 frame 437 3.2 fps …
[vk] saved Z:/home/mitchell/mk8-win-boot/win-shot-00150.png (1280x720)
[vk] saved Z:/home/mitchell/mk8-win-boot/win-shot-00300.png (1280x720)
[vk] saved Z:/home/mitchell/mk8-win-boot/win-shot-00450.png (1280x720)
```

Screenshots (runtime-written swapchain images, not X captures):

- `$HOME/mk8-win-boot/win-shot-00150.png` — Mario Kart 8 Deluxe logo/title.
- `$HOME/mk8-win-boot/win-shot-00300.png` — attract screen, `Ver. 4.0.1`.
- `$HOME/mk8-win-boot/win-shot-00450.png` — `Press L R to start`.

The frame rate shown is host-bound (0.4–9 fps under heavy external load) and is not a performance
claim.

### Windows, Wine 11.22 — menu

```sh
LARECOMP_INPUT='300:L:6,360:A:6' LARECOMP_WINDOW_SHOT=500 \
LARECOMP_SHOT_PATH='Z:/home/mitchell/mk8-win-boot/menu-win.png' \
LARECOMP_EXIT_AFTER_SHOT=1 timeout 300 wine ./mk8drecomp.exe --play
```

`[input] id 0 buttons 0x40` (L) at sampling 306, then `0x1` (A) at 366. The capture
`$HOME/mk8-win-boot/menu-win.png` shows **Select a Mii** with the runtime's Mii-name overlay
(`Nome do Mii … Player`), i.e. the game is past the title and running its menu flow. A second run
with only `LARECOMP_INPUT='250:L:6'` and the capture at frame 500 produced
`$HOME/mk8-win-boot/menu-win2.png`, the same **Select a Mii** screen without the overlay. This data
root has no Mii resource, so the game asks for a Mii before the main menu. The main-menu screen was
not reached in this session.

### Proton-GE 11-12

```sh
cd $HOME/mk8-win-boot/run-win
STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam" \
STEAM_COMPAT_DATA_PATH=/home/mitchell/Projects/MK8-online-windows-verify/local/proton-prefix \
SteamGameId=480 SteamAppId=480 MK8_EXPERIMENTAL_NO_MII=1 WINEDEBUG=-all \
  LARECOMP_INPUT='350:L:6' LARECOMP_WINDOW_SHOT=650 \
  LARECOMP_SHOT_PATH='Z:/home/mitchell/mk8-win-boot/proton-menu2.png' \
  LARECOMP_EXIT_AFTER_SHOT=1 timeout 300 \
  "$HOME/.local/share/Steam/compatibilitytools.d/Proton-GE Latest/proton" run \
  /home/mitchell/mk8-win-boot/run-win/mk8drecomp.exe --play
```

The runtime's stdout went to `run-win/mk8drecomp.log` (Proton gives it no console); a copy is
`proton-runtime-2026-11-20.log`:

```text
[vk] t=17.1 frame 4 1.5 fps max 1506ms level … (70 draws …)
[vk] t=19.2 frame 10 3.0 fps …
...
[input] id 0 buttons 0x40 (sampling 362, lr 70005fc0f0)
[vk] saved Z:/home/mitchell/mk8-win-boot/proton-menu2.png (1280x720)
```

No FATAL. The capture `proton-menu2.png` is the same **Select a Mii** screen as Wine; the earlier
`proton-menu.png` captured the title/`Press L R to start` screen. The `timeout` code was 124 because
Proton did not exit after the capture; the screenshot and the guest frames are the evidence, not the
process status.

### Linux control

The same host binary (4,653,176 bytes, `host-optimized/mk8drecomp`, unchanged by this work) ran
twice with the same data root and scripted input; only `LARECOMP_GAME_LIB` differed:

```sh
cd $HOME/mk8-win-boot/linux-{old,new}-data
MK8_EXPERIMENTAL_NO_MII=1 LARECOMP_DATA_ROOT=$PWD LARECOMP_SAVE_DIR=$PWD/save \
  LARECOMP_INPUT='250:L:6' LARECOMP_GAME_LIB=<game.so> \
  LARECOMP_WINDOW_SHOT=150:450:150 \
  LARECOMP_SHOT_PATH=$HOME/mk8-win-boot/linux-{old,new}-%05d.png \
  LARECOMP_EXIT_AFTER_SHOT=1 timeout 180 \
  /…/host-optimized/mk8drecomp extracted
```

| Build | First frame line | Last logged frame before the capture | Exit | Screenshots |
| --- | --- | --- | --- | --- |
| old `mk8dgame.so` `4f86d6c5…` | `[vk] t=4.9 frame 3` | `t=25.9 frame 405` | 0 | `linux-old-00150/00300/00450.png` |
| new `mk8dgame.so` `e0423aa6…` | `[vk] t=4.4 frame 41` | `t=22.5 frame 397` | 0 | `linux-new-00150/00300/00450.png` |

No FATAL in either log. Both show the title, `Press L R to start` after the scripted L, and the same
**Select a Mii** screen at frame 450. The new library's exported sdk table is 28234 entries with
`0x7004004c30`; the old library's linked table was also 28234 because the qualification job applied
`toolchain/sdk_exports.py` (the generated table alone was 28228). The Linux guest never calls
`bigint_mod_mult` on this data, so both boot; the new build proves the generator change does not
regress Linux.

## What remains unverified

- **In-game main menu, race, restart/save.** Only the title/attract and the Mii selection screen were
  captured on Windows. A full race, clean exit, save reload and MKTV are not qualified.
- **Audio, controls, clock speed.** The audio device opens; no sound was heard or measured. The
  scripted L/A presses reached the guest, but physical controllers and the clock were not compared.
- **Race between two PCs.** Not attempted.
- **Real Windows / MSVC.** Everything used the GNU target under Wine/Proton on Linux. The release
  workflow uses `windows-latest` and MSVC; the launcher is unaffected by this change.
- **Timing.** The host was heavily loaded by external builds; no FPS or latency claim is made.
- **Other interior entries.** The scan found no other non-`STT_FUNC` sized text symbols in `main`,
  `sdk` or `subsdk0`. A future module could still contain unsized assembly entries; those remain a
  known limitation and must be added with a hash-gated supplement or a proper symbol.
