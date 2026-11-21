# Agent guide

**English** · [Português](docs/pt-BR/AGENTS.md)

These rules are mandatory for AI agents working in this repository. [CONTRIBUTING.md](CONTRIBUTING.md) has the commands and layout. The Portuguese copy carries the same rules; keep both in sync.

## Read first

1. [README.md](README.md) — what the project is.
2. [docs/en/STATUS.md](docs/en/STATUS.md) — current state and qualification.
3. [docs/en/ROADMAP.md](docs/en/ROADMAP.md) — open work.
4. [docs/en/log/README.md](docs/en/log/README.md) — dated technical history, split by topic. Open only the topic file you need.

These are the operational documentation. Update them when the state changes, following the rules below.

## Documentation

**Two languages.** Every document exists in English and Portuguese: `docs/en/` and `docs/pt-BR/`. Root files are English, with Portuguese pairs: `README.pt-BR.md`, `docs/pt-BR/AGENTS.md`, `docs/pt-BR/CONTRIBUTING.md`. Update both versions in the same change and keep the same headings in each. Never leave one language behind.

**Where content goes.**

| Content | File |
| --- | --- |
| What the project is, download, quick start | `README.md` (short entry point only; no changelogs or fix notes) |
| How to use a feature | `docs/*/GUIDE.md` |
| Building, testing, layout | `CONTRIBUTING.md` |
| Changes in a version | `docs/releases/<version>.md` (bilingual; read by `scripts/release.py`) |
| Current state and qualification | `docs/*/STATUS.md` |
| Open work | `docs/*/ROADMAP.md` |
| Dated investigations, measurements, evidence | `docs/*/log/<topic>.md` |
| External sources and attributions | `docs/*/THIRD_PARTY.md` |

**STATUS and log.**

- Keep `STATUS.md` short and current: replace outdated lines instead of appending.
- In the log, add to the existing topic file, newest section first, one section per subject rather than one per attempt.
- Create a new topic file only if none fits, and add it to both log indexes.
- Don't create standalone report files.

**Structure and links.**

- Don't move or rename documents without updating every link to them.
- Keep the `docs/STATUS.md` and `docs/THIRD_PARTY.md` redirect pages; code comments cite those paths.
- Relative links must resolve.
- Don't add new top-level documents.

## Scope and structure

- Make the smallest complete change. Keep the launcher, pipeline, runtime and private data separate. Don't create packages for hypothetical features.
- Project adaptations live in `runtime/mk8` and `toolchain`. Never edit `upstream/` or generated code directly: preparation verifies hashes and applies changes to the build copy.
- Active profiles are MK8 4.0.1 AArch64. Generic support for subsdk1–4 does not mean those commercial modules have been tested.
- Old serialized identifiers and environment/ABI names are contracts, not text to rename cosmetically.

## Guest and ABI changes

- ABI or structure changes require recompiling all their consumers.
- Guest hooks must validate version/hash and reject unexpected bounds.
- Never return a fake success to get past a failure.

## Private data and the filesystem

- Preserve `local/config.json`, `local/mii-profile.json`, `local/profiles`, `local/user*`, inputs and saves.
- Never copy ROMs, keys or generated code into public sources or tests.
- Reuse existing build folders. Before any cleanup, check the absolute path, manifests and running processes. Don't follow junctions.
- Keep at least 4 GiB of free disk space.
- If a deletion is blocked, don't work around it with another mechanism.

## Build

| Change | Action |
| --- | --- |
| Launcher | Compile the Rust workspace |
| Runtime, no guest change | `toolchain/build.py ... --runtime-only` |
| Guest | Regenerate and recompile the affected units |

Starting a build is not finishing it. Wait for the build to complete and check the result.

## Tests

- Run the tests for the area you changed.
- For Rust changes, also run `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets --locked -- -D warnings` and `cargo test --workspace --locked`.
- For interface changes, run the browser tests (`python -m unittest discover -s tests -p test_launcher_e2e.py`).
- A native test must exercise the behaviour, not just search the source for text.

## Qualification

- Qualification requires evidence of the menu, a full race, restart/save, image, audio, controller and clock speed.
- `gameplay_verified` stays `false` until all of that is shown.
- Multiplayer requires separate validation on two machines.

## Mii and graphics

- Editor profiles and GLBs are not models integrated into the MK8 engine.
- Don't re-enable Mii graphics consumers with empty pointers or empty files.

## Attribution and licenses

- External references and attributions go in [docs/en/THIRD_PARTY.md](docs/en/THIRD_PARTY.md) and [docs/pt-BR/THIRD_PARTY.md](docs/pt-BR/THIRD_PARTY.md), with license texts in `docs/licenses`.
- Preserve the license notices of dependencies.
- Don't carry results from other projects over to this build.
