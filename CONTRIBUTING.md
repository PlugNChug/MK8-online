# Contributing

**English** · [Português](docs/pt-BR/CONTRIBUTING.md)

Thanks for helping. This page covers building from source, the repository layout, testing and documentation. AI agents should also read [AGENTS.md](AGENTS.md), which holds the project's hard rules.

## Ground rules

- Bring your own game files. Never commit or attach ROMs, console keys, saves, generated game code or game assets, including in tests and fixtures.
- Keep MK8 adaptations in `runtime/mk8` and `toolchain`. Preparation applies them to a verified build copy. Do not edit `upstream/` snapshots or generated code directly.
- Keep each change small and complete, and run the tests for the area you change.
- Credit external sources in [docs/en/THIRD_PARTY.md](docs/en/THIRD_PARTY.md) and its Portuguese pair, and keep dependency license notices intact.

## Requirements

- Windows x64
- Rust (stable, with `rustfmt` and `clippy`)
- Python 3.12 x64
- PowerShell 7
- .NET 10 x64 (for the importer)

For the Linux port, see [docs/en/LINUX.md](docs/en/LINUX.md) and the `platforms/linux` submodule.

## Getting started

Development happens on the `fix-chore` branch. From the repository root:

```powershell
.\scripts\start-launcher.ps1
```

This downloads the pinned upstream sources, verifies their hashes, builds the launcher and opens it.

## Repository layout

| Location | Responsibility |
| --- | --- |
| `apps/launcher` | Native launcher, web interface, installer, updates and reports |
| `crates/installer`, `crates/manifest`, `crates/build-manager` | Installation, validation, Miis and build supervision |
| `crates/session`, `crates/transport` | Rooms, Steam, relay and authenticated transport |
| `toolchain`, `runtime/mk8` | Recompilation pipeline and MK8 runtime adaptations |
| `games/mk8d` | Version profiles, symbols and game configuration |
| `scripts`, `tests` | Preparation, packaging and regression checks |
| `upstream/manifests` | Pinned source manifests; downloaded snapshots stay outside Git |
| `platforms/linux` | Linux port (submodule) |
| `docs/en`, `docs/pt-BR` | Documentation, one copy per language |
| `docs/releases` | Bilingual release notes, read by `scripts/release.py` |
| `local`, `target`, `prebuilt` | Private data and generated artifacts, ignored by Git |

## Building

| What changed | What to rebuild |
| --- | --- |
| Launcher (Rust, web UI) | `cargo build` / `.\scripts\start-launcher.ps1` |
| Runtime only, guest unchanged | `toolchain/build.py ... --runtime-only` |
| Guest hooks or generated units | Regenerate and recompile the affected units with a full game build |
| ABI or shared structures | Every consumer of that structure |

Example runtime-only rebuild against an isolated, already prepared data folder:

```powershell
.\local\python\Scripts\python.exe toolchain/build.py --config 'E:\MK8-TEST\config.json' --local 'E:\MK8-TEST' --runtime-only
```

Reuse existing build folders. Builds run for a long time; starting one is not the same as finishing it.

**Packaging:**

- Portable launcher: `python scripts/package-launcher.py` (output: `target/portable/Launcher.exe`).
- Local test installer: `python scripts/release.py` (output: `<Cargo target>/dist/MK8-online-<version>-setup.exe`). Install same-version test builds manually; the updater only offers newer versions.
- Publishing: see [docs/en/RELEASING.md](docs/en/RELEASING.md).

## Testing

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
python -m unittest discover -s tests
python -m unittest discover -s tests -p test_launcher_e2e.py        # browser tests (MK8_TEST_RELAY_EXE for the relay)
python -m unittest discover -s tests -p test_launcher_languages.py  # launcher/installer translations
```

- Rust changes need formatting, Clippy and tests.
- Interface changes need the browser tests.
- A native test must exercise the behaviour, not just search the source for text.

**DLC import from the command line**, against an isolated prepared data folder:

```powershell
.\local\python\Scripts\python.exe toolchain/import_dlc.py --package 'D:\Switch\BoosterCoursePass.nsp' --keys 'D:\Switch\prod.keys' --local 'E:\MK8-TEST'
```

## Launcher interface development

`python scripts/preview-launcher.py` compiles a debug build and opens the real interface with separate data under `local/design-preview`.

- `--target-dir D:/path/to/target` reuses a Cargo folder; `--no-build` skips compilation.
- Setting `MK8_WEB_ROOT` serves live interface files in debug builds only. Edit the HTML/CSS/JS and reload.
- `/?storyboard=1#install` shows a labeled installation simulation with sample data. It calls no installation or build API.

Release builds use embedded assets and omit these debug controls. The preview does not reuse installed profiles, resources or saves, and does not qualify gameplay.

## Developer-only paths

These paths are reachable only through the API and are not public features: LAN, relay and P2P transports, and the static Mii head converter. See the [technical log](docs/en/log/README.md).

## Documentation

- Every document exists in English (`docs/en/`) and Portuguese (`docs/pt-BR/`). Update both in the same change. Root files (`README.md`, `AGENTS.md`, this file) are English, with Portuguese pairs (`README.pt-BR.md`, `docs/pt-BR/AGENTS.md`, `docs/pt-BR/CONTRIBUTING.md`).
- `docs/*/STATUS.md` is a short current-state summary. Replace outdated lines; don't append.
- Dated investigation detail goes in [docs/en/log/README.md](docs/en/log/README.md). Add one section per topic, newest first, not one per attempt.
- Per-version changes go in `docs/releases/<version>.md`, not in the README.
- `docs/STATUS.md` and `docs/THIRD_PARTY.md` are redirect stubs kept for code comments that cite them. Do not delete them.

## Reporting bugs

See [Reporting a problem](docs/en/GUIDE.md#reporting-a-problem) in the user guide.
