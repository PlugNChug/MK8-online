# Publishing a release

**English** · [Português](../pt-BR/RELEASING.md)

This is the workflow for the maintainer and for AI agents preparing a release. Testers get `setup.exe` from GitHub Releases and update from the launcher itself (**Support → Update now**).

The updater queries the public GitHub API without authentication, so the repository and its assets must be public; while they are private, the query fails. Version 0.1.6 was packaged with `MK8_UPDATE_REPO=Vnnc1/MK8-online`: making that same repository public needs no rebuild and no change on the player's PC. Never embed tokens in the launcher.

For each new version, bump the compiled version and publish the matching tag with `MK8-online-<version>-setup.exe`, the Windows portable ZIP, the Linux AppImage and the Linux tar.gz, each with its own `.sha256`. Compute the SHA-256 **after** any change to or signing of the executable. Also attach the FFmpeg sources/scripts required below. Drafts are not offered to players; published pre-releases are. Updates are triggered by the player; nothing installs automatically when the launcher opens.

## Versioning

The project uses `MAJOR.MINOR.PATCH`, with a suffix for pre-releases (for example `0.3.0-alpha.1`). 0.5.0 uses the unsuffixed tag `v0.5.0` and is a public experimental release. The workflow keeps the historical exception for tag `v0.3.0`. Its experimental state and CTGP/Steam limits stay explicit in the title and notes.

The version lives in `Cargo.toml` (`[workspace.package]`) and in `apps/launcher/tauri.conf.json`; `scripts/release.py` keeps both in sync. The launcher only offers an update when the release tag is **greater** than the compiled version.

## Through GitHub Actions (recommended)

```powershell
python scripts/release.py --bump 0.5.0   # updates Cargo.toml, tauri.conf.json and Cargo.lock
git commit -am "Release 0.5.0"
git tag v0.5.0
git push origin master v0.5.0
```

The tag triggers `.github/workflows/release.yml`:

- **Windows:** FFmpeg is built on a Linux runner; the installer and ZIP are assembled on Windows, and the portable edition is opened to verify the extracted runtime's key.
- **Linux:** builds the AppImage and tar.gz, starts the launcher and exercises the importer and updater rollback.
- **Publish:** depends on both platforms and verifies the SHA-256 files. A suffixed tag, or the historical `v0.3.0`, is published as a pre-release.

The Linux code for this version is pinned by SHA in the `platforms/linux` submodule. Run `git submodule update --init platforms/linux` to build the port locally. When preparing a new version, update the workspace on the Linux line, publish that revision in the same repository and record the new submodule commit on the main branch. The packager checks the version against the tag. [Linux 0.5.0 source](https://github.com/Vnnc1/MK8-online/tree/edfc423697977594b49db64b67c433c73086664f)

Review `docs/releases/<version>.md`: the first line is the title and the rest forms the notes, together with the generated SHA-256 and notices. When asking an AI to prepare a release, have it update `docs/{en,pt-BR}/STATUS.md` and `ROADMAP.md` with the new evidence (dated detail goes in `docs/{en,pt-BR}/log/`), check the READMEs, and only then create the tag.

## Locally

Requirements: Rust, Python 3.12, PowerShell 7 and .NET 10 x64 (package extractor). The native tools must also be installed (`scripts/setup-native.py` and `scripts/setup-runtime-deps.py`).

- Packaging compiles the game runtime (`scripts/prebuild-host.py`, ~1 min) and ships it ready in `prebuilt/host` with the key of its inputs; a player's build uses it only if their key is identical.
- Minimal FFmpeg (`scripts/build-ffmpeg.py`) is compiled in WSL (Ubuntu with `make`, `gcc` and `curl`; ~6 min the first time, then reused while the script is unchanged). The release attaches `ffmpeg-9.0.2.tar.xz` and `build-ffmpeg.sh` (LGPL).
- To rebuild the extractor, set `MK8_IMPORT_PWSH` to the PowerShell 7 `pwsh.exe` path when it is not on `PATH`.
- The setup is the packaged `Launcher.exe` itself, renamed `MK8-online-<version>-setup.exe`: that name opens the installation window (`apps/launcher/src/installer.rs`).

```powershell
$env:MK8_UPDATE_REPO = "owner/repository-name"
python scripts/release.py                  # target/dist/MK8-online-<version>-setup.exe + .sha256 + release-notes.md
python scripts/release.py --skip-package   # only re-copies target/portable/Launcher.exe as the setup
```

The scripts resolve the output folder through `cargo metadata`: `target/` above means the configured `target_directory`, including when it is on another drive. Do not move or clean builds to match a literal example path.

To publish already-tested binaries, prepare a draft from the `master` revision with the title and bilingual notes. Attach the setup, portable ZIP, amd64 AppImage and tar.gz, each with its SHA-256, plus the FFmpeg source and build script. Check the draft's hashes before making it public. When the tag already has a published release, the workflow checks the existing assets and keeps the tested binaries.

## What goes into the installer

- Only the packaged `Launcher.exe`: verified runtime and pipeline sources, the extractor and the .NET runtime. `upstream` sources are downloaded at packaging time and checked against the manifests in `upstream/manifests`.
- Never: the game, keys, saves, builds, `local/` or test data.
- The per-user installer copies the launcher and `Uninstall.exe`, writes `mk8-online.json` with the data folder, unpacks the version's sources, creates shortcuts and registers uninstallation under `HKCU\...\Uninstall\MK8-online`. Updates keep the data. On uninstall, keeping data is the default; removing managed data requires confirmation.
- Command line (the updater uses the first): `/SILENT` installs over the previous folders and opens the launcher; `/DIR=`, `/DATADIR=`, `/DESKTOP=0|1` and `/NOLAUNCH` change that. `Launcher.exe --uninstall [/SILENT]` uninstalls. A previous Inno Setup installation (up to 0.1.1) in the same folder is replaced, and its old uninstaller and menu group are removed.

## Pre-publish checklist

- [ ] `cargo fmt --all -- --check`, Clippy and `cargo test --workspace --locked`
- [ ] Browser tests: `python -m unittest discover -s tests -p test_launcher_e2e.py` (with `MK8_TEST_RELAY_EXE` for the relay)
- [ ] Install the setup into a temporary folder (`setup.exe /SILENT /DIR=... /DATADIR=... /DESKTOP=0 /NOLAUNCH`), open the launcher and uninstall with `Launcher.exe --uninstall /SILENT`; open the setup window and walk through all four screens
- [ ] `docs/{en,pt-BR}/STATUS.md` and `ROADMAP.md` updated; no personal paths or game data in versioned files
- [ ] `README.md` and `README.pt-BR.md` in sync, as well as each `docs/en` / `docs/pt-BR` pair; links and images checked
- [ ] `python -m unittest discover -s tests -p test_launcher_languages.py`; check the launcher and installer in both languages, including persistence after restart
- [ ] Review new files, Git history and authorship metadata before opening the repository; `.gitignore` does not remove information already committed. Setting a public identity and `noreply` email for future commits does not change old ones
- [ ] Separately review release assets, public logs and GitHub settings/secrets; the local scan does not cover those services
- [ ] Use `scripts/package-launcher.py` for the public executable: packaging remaps Rust paths and enables `tauri/custom-protocol` so the development config directory is not embedded. Also check the binary's bytes and payload; the development executable in `target/release` contains the checkout path and is not a distribution package
