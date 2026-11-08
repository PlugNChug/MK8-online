"""Build a release: the packaged launcher, its setup.exe and the SHA-256 the in-launcher updater checks.

    python scripts/release.py --bump 0.2.0-alpha.1    # set the version (Cargo.toml, tauri.conf.json, Cargo.lock)
    python scripts/release.py                         # build target/dist/MK8-online-<version>-setup.exe

Set MK8_UPDATE_REPO=owner/name (the GitHub repository whose Releases the launcher reads) and, if there is one,
MK8_RELAY_URL before building: both are compiled into the launcher. The setup is the packaged launcher itself
under the name MK8-online-<version>-setup.exe, which opens its own install window (apps/launcher/src/installer.rs).
Publishing is a separate step: upload the
setup and its .sha256 to a GitHub Release tagged v<version> (docs/RELEASING.md; CI does it on a tag push).
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

ROOT = Path(__file__).resolve().parents[1]
CARGO = ROOT / "Cargo.toml"
TAURI = ROOT / "apps/launcher/tauri.conf.json"
DIST = ROOT / "target/dist"
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
    launcher = ROOT / "target/portable/Launcher.exe"
    if not args.skip_package:
        subprocess.run([sys.executable, ROOT / "scripts/package-launcher.py"], cwd=ROOT, check=True)
    if not launcher.is_file():
        raise SystemExit(f"Launcher empacotado ausente: {launcher}")
    DIST.mkdir(parents=True, exist_ok=True)
    # (the name is what makes the launcher open as the installer)
    setup = DIST / f"MK8-online-{version}-setup.exe"
    shutil.copyfile(launcher, setup)
    digest = sha256(setup)
    (DIST / f"{setup.name}.sha256").write_text(f"{digest}  {setup.name}\n", encoding="ascii")
    notes = DIST / "release-notes.md"
    notes.write_text(
        f"## MK8-online {version} (experimental)\n\n"
        "Instale com o setup abaixo; quem já tem o Launcher pode usar **Suporte → Atualizar agora**.\n"
        "Cada máquina prepara o jogo com os próprios arquivos do console: nada do jogo é distribuído.\n\n"
        "Pendências conhecidas: veja [docs/STATUS.md](../blob/main/docs/STATUS.md). "
        "Ao relatar um problema, anexe o .zip de **Suporte → Gerar relatório**.\n\n"
        f"SHA-256 de `{setup.name}`: `{digest}`\n",
        encoding="utf-8")
    print(f"{setup}\n{digest}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bump", metavar="VERSION")
    parser.add_argument("--tag", help="tag a conferir contra a versão (CI)")
    parser.add_argument("--skip-package", action="store_true", help="reaproveita target/portable/Launcher.exe")
    args = parser.parse_args()
    bump(args.bump) if args.bump else build(args)
