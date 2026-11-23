"""Build a release: the packaged launcher, its setup.exe and the SHA-256 the in-launcher updater checks.

    python scripts/release.py --bump 0.4.0    # set the version (Cargo.toml, tauri.conf.json, Cargo.lock)
    python scripts/release.py                         # build target/dist/MK8-online-<version>-setup.exe

Set MK8_UPDATE_REPO=owner/name (the GitHub repository whose Releases the launcher reads) and, if there is one,
MK8_RELAY_URL before building: both are compiled into the launcher. The setup is the packaged launcher itself
under the name MK8-online-<version>-setup.exe, which opens its own install window (apps/launcher/src/installer.rs).
Publishing is a separate step: upload the
setup and its .sha256 to a GitHub Release tagged v<version> (docs/en/RELEASING.md; CI does it on a tag push).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from cargo_target import target_dir

ROOT = Path(__file__).resolve().parents[1]
CARGO = ROOT / "Cargo.toml"
TAURI = ROOT / "apps/launcher/tauri.conf.json"
TARGET = target_dir()
DIST = TARGET / "dist"
# Hand-written notes for a version (first line: "# <release title>"), published above the generated part.
NOTES = ROOT / "docs/releases"
VERSION = re.compile(r"^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$")


def workspace_version():
    text = CARGO.read_text(encoding="utf-8")
    section = text[text.index("[workspace.package]"):]
    return re.search(r'^version\s*=\s*"([^"]+)"', section, re.M).group(1)


def bump(version):
    if not VERSION.match(version):
        raise SystemExit(f"Versão inválida: {version} (use 1.2.3 ou 1.2.3-alpha.1)")
    text = CARGO.read_text(encoding="utf-8")
    start = text.index("[workspace.package]")
    head, section = text[:start], text[start:]
    section, n = re.subn(r'^version\s*=\s*"[^"]+"', f'version = "{version}"', section, count=1, flags=re.M)
    if n != 1:
        raise SystemExit("Cargo.toml sem versão em [workspace.package]")
    CARGO.write_text(head + section, encoding="utf-8")
    config = json.loads(TAURI.read_text(encoding="utf-8"))
    config["version"] = version
    TAURI.write_text(json.dumps(config, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["cargo", "update", "--workspace", "--offline"], cwd=ROOT, check=True)
    print(f"Versão {version}. Faça commit e crie a tag v{version}.")


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def build(args):
    version = workspace_version()
    if json.loads(TAURI.read_text(encoding="utf-8"))["version"] != version:
        raise SystemExit("tauri.conf.json e Cargo.toml têm versões diferentes: use --bump")
    if args.tag and args.tag != f"v{version}":
        raise SystemExit(f"A tag {args.tag} não corresponde à versão v{version}")
    if not os.environ.get("MK8_UPDATE_REPO"):
        print("Aviso: MK8_UPDATE_REPO não definido; esta build não oferecerá atualizações.", flush=True)
    launcher = TARGET / "portable/Launcher.exe"
    if not args.skip_package:
        subprocess.run([sys.executable, ROOT / "scripts/package-launcher.py"], cwd=ROOT, check=True)
    if not launcher.is_file():
        raise SystemExit(f"Launcher empacotado ausente: {launcher}")
    DIST.mkdir(parents=True, exist_ok=True)
    # (the name is what makes the launcher open as the installer)
    setup = DIST / f"MK8-online-{version}-setup.exe"
    shutil.copyfile(launcher, setup)
    portable = DIST / f"MK8-online-{version}-portable.zip"
    shutil.copyfile(launcher.parent / "MK8-online-portable.zip", portable)
    digest = sha256(setup)
    portable_digest = sha256(portable)
    (DIST / f"{setup.name}.sha256").write_text(f"{digest}  {setup.name}\n", encoding="ascii")
    (DIST / f"{portable.name}.sha256").write_text(f"{portable_digest}  {portable.name}\n", encoding="ascii")
    # LGPL: the setup carries FFmpeg DLLs built by scripts/build-ffmpeg.sh, so its source goes with the release.
    ffmpeg = TARGET / "prebuilt-ffmpeg"
    for name in ("ffmpeg-9.0.2.tar.xz", "build-ffmpeg.sh"):
        if not (ffmpeg / name).is_file():
            raise SystemExit(f"Fonte do FFmpeg ausente: {ffmpeg / name} (execute scripts/build-ffmpeg.py)")
        shutil.copyfile(ffmpeg / name, DIST / name)
    notes = DIST / "release-notes.md"
    written = NOTES / f"{version}.md"
    title, body = f"MK8-online {version}", ""
    if written.is_file():
        first, _, rest = written.read_text(encoding="utf-8").partition("\n")
        if not first.startswith("# "):
            raise SystemExit(f"{written}: a primeira linha deve ser '# <título da release>'")
        title, body = first[2:].strip(), rest.strip() + "\n\n---\n\n"
    (DIST / "release-title.txt").write_text(title + "\n", encoding="utf-8")
    notes.write_text(
        body +
        "## Checksums / Somas de verificação\n\n"
        "SHA-256 files accompany the downloads. Arquivos SHA-256 acompanham os downloads.\n\n"
        "Windows FFmpeg 9.0.2 (LGPL 2.1): `ffmpeg-9.0.2.tar.xz` · `build-ffmpeg.sh`.\n\n"
        f"| Windows x64 | SHA-256 |\n| --- | --- |\n"
        f"| `{setup.name}` | `{digest}` |\n"
        f"| `{portable.name}` | `{portable_digest}` |\n",
        encoding="utf-8")
    print(f"{setup}\n{digest}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bump", metavar="VERSION")
    parser.add_argument("--tag", help="tag a conferir contra a versão (CI)")
    parser.add_argument("--skip-package", action="store_true", help="reaproveita <target>/portable/Launcher.exe")
    args = parser.parse_args()
    bump(args.bump) if args.bump else build(args)
