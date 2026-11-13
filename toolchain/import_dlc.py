"""Importação local do Booster Course Pass verificado para o perfil MK8 4.0.1."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

from build import atomic_json, file_record, sha
from build_lock import exclusive_build
from import_mk8 import Entry, entries
from repair import is_link
from storage import GIB, require_space

ROOT = Path(__file__).resolve().parents[1]
TITLE = 0x0100152000023001
APPLICATION = 0x0100152000022000
CONTENTS = {
    "a3e030a30e112d39de059a24f7c80b63.nca": (114688, "a3e030a30e112d39de059a24f7c80b63b40d4975f9bb9382bc51a880d77f4e31"),
    "1fa7f2036e3aabdede91fa1cd78705ca.cnmt.nca": (3584, "1fa7f2036e3aabdede91fa1cd78705ca7e23cb2541654ec923cfda902051af07"),
}
FILES = {
    "contents.dat": (4096, "93f0853a9cdcd28280735faad6a4a04ba9c2ce84b25ca46b632f46e2a6e99b46"),
    "version.txt": (5, "56acd2f85500d246e0851f77035f8b4414a57c4f31735c8f37c9cec0f3739d47"),
}
MANIFEST = struct.Struct("<8sQQIIQQ32s32s")


def regular_path(path):
    for parent in (path, *path.parents):
        if is_link(parent):
            raise ValueError(f"DLC: caminho com link recusado: {parent}")


def validate_files(folder):
    regular_path(folder)
    if {p.name for p in folder.iterdir()} != set(FILES):
        raise ValueError("DLC: conjunto de arquivos inesperado.")
    for name, (size, expected) in FILES.items():
        path = folder / name
        regular_path(path)
        if not path.is_file() or path.stat().st_size != size or sha(path) != expected:
            raise ValueError(f"DLC: arquivo não corresponde ao perfil: {name}")


def manifest_bytes():
    return MANIFEST.pack(b"MK8DLC1\0", APPLICATION, TITLE, 65536, 1,
                         FILES["contents.dat"][0], FILES["version.txt"][0],
                         bytes.fromhex(FILES["contents.dat"][1]), bytes.fromhex(FILES["version.txt"][1]))


def validate_installation(folder):
    regular_path(folder / "installed.bin")
    if {p.name for p in folder.iterdir()} != {"installed.bin", f"{TITLE:016x}"} or \
            {p.name for p in (folder / f"{TITLE:016x}").iterdir()} != {"romfs"}:
        raise ValueError("DLC: estrutura de instalação inesperada.")
    if (folder / "installed.bin").read_bytes() != manifest_bytes():
        raise ValueError("DLC: manifesto inválido.")
    validate_files(folder / f"{TITLE:016x}/romfs")


def attach(candidate, local):
    """Reaproveita a instalação independente da identidade da build."""
    stable = local / "dlc"
    regular_path(stable)
    legacy = Path(candidate["data_root"]) / "extracted/dlc"
    if not stable.exists() and not legacy.exists() and (local / "candidate.json").is_file():
        previous = json.loads((local / "candidate.json").read_text(encoding="utf-8"))
        legacy = Path(previous["data_root"]) / "extracted/dlc"
    regular_path(legacy)
    if not legacy.resolve().is_relative_to(local.resolve()):
        raise ValueError("DLC: instalação anterior fora da pasta selecionada.")
    if not stable.exists() and legacy.exists():
        validate_installation(legacy)
        require_space(local, 4 * GIB + 2 * 1024**2)
        stage = local / f"dlc-migration-{time.time_ns()}"
        shutil.copytree(legacy, stage)
        validate_installation(stage)
        stage.rename(stable)
    if not stable.exists():
        return
    validate_installation(stable)
    romfs = stable / f"{TITLE:016x}/romfs"
    for label, path in (("dlc-manifest", stable / "installed.bin"), ("dlc-contents", romfs / "contents.dat"),
                        ("dlc-version", romfs / "version.txt")):
        candidate["files"][label] = file_record(path)


def install(package, keys, local, *, reinstall=False):
    local = local.absolute()
    regular_path(local)
    local = local.resolve()
    require_space(local, 4 * GIB + 2 * 1024**2)
    candidate_path = local / "candidate.json"
    regular_path(candidate_path)
    candidate = json.loads(candidate_path.read_text(encoding="utf-8"))
    if candidate.get("schema") != 2 or candidate.get("profile") != "mk8d-smm2-experimental" or candidate.get("gameplay_verified"):
        raise ValueError("DLC: candidato experimental incompatível.")
    for record in (candidate["executable"], *(record for name, record in candidate["files"].items() if not name.startswith("dlc-"))):
        if sha(Path(record["path"])) != record["sha256"]:
            raise ValueError("DLC: arquivos da build alterados; verifique o candidato antes de importar.")
    profile = json.loads((ROOT / "games/mk8d/profiles/4.0.1.json").read_text(encoding="utf-8"))
    for name, expected in profile["input_sha256"].items():
        record = candidate["files"]["npdm" if name == "main.npdm" else name]
        if record["sha256"] != expected or sha(Path(record["path"])) != expected:
            raise ValueError("DLC: prepare primeiro o MK8 4.0.1 AArch64 verificado.")
    data = Path(candidate["data_root"])
    if not data.is_absolute():
        raise ValueError("DLC: dados do jogo fora da pasta selecionada.")
    regular_path(data)
    data = data.resolve()
    if not data.is_relative_to(local):
        raise ValueError("DLC: dados do jogo fora da pasta selecionada.")
    if not Path(candidate["executable"]["path"]).resolve().is_relative_to(local):
        raise ValueError("DLC: executável fora da pasta de dados selecionada.")
    destination = local / "dlc"
    regular_path(destination)
    marker = destination / "installed.bin"
    regular_path(marker)
    expected_marker = manifest_bytes()
    valid = False
    if destination.exists():
        try:
            validate_installation(destination)
            valid = True
        except (ValueError, FileNotFoundError):
            pass
    if not valid or reinstall:
        stage = local / f"dlc-stage-{time.time_ns()}"
        stage.mkdir()
        with package.open("rb") as stream:
            index = entries(stream, package.stat().st_size)
            wanted = {name.encode() for name in CONTENTS}
            if {name for name in index if name.endswith((b".nca", b".ncz"))} != wanted:
                raise ValueError("DLC: pacote não corresponde ao Booster Course Pass verificado.")
            for name, (size, expected) in CONTENTS.items():
                member = Entry(stream, *index[name.encode()])
                if member.size != size:
                    raise ValueError("DLC: tamanho do NCA divergente.")
                payload = member.read()
                if hashlib.sha256(payload).hexdigest() != expected:
                    raise ValueError("DLC: tamanho ou hash do NCA divergente.")
                (stage / name).write_bytes(payload)
            tickets = [name for name in index if name.endswith(b".tik")]
            if len(tickets) != 1 or not tickets[0].startswith(b"0100152000023001") or index[tickets[0]][1] > 16384:
                raise ValueError("DLC: ticket ausente ou incompatível.")
            (stage / "content.tik").write_bytes(Entry(stream, *index[tickets[0]]).read())
        require_space(ROOT / "tools", 4 * GIB + 128 * 1024**2)
        subprocess.run([sys.executable, ROOT / "scripts/setup-import-tool.py"], check=True)
        tool = ROOT / "tools/mk8-import"
        result = subprocess.run([tool / "dotnet/dotnet.exe", tool / "mk8-import.dll", "--dlc", stage, keys, stage / "output"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode:
            raise ValueError("DLC: falha na validação de metadados, chaves ou integridade do arquivo.")
        validate_files(stage / "output/romfs")
        publish = stage / "publish"
        target = publish / f"{TITLE:016x}"
        target.mkdir(parents=True)
        (stage / "output/romfs").rename(target / "romfs")
        (publish / "installed.bin").write_bytes(expected_marker)
        backup = local / f"dlc-backup-{time.time_ns()}"
        if destination.exists():
            destination.rename(backup)
        try:
            publish.rename(destination)
        except OSError:
            if backup.exists():
                backup.rename(destination)
            raise
    attach(candidate, local)
    generation = json.loads((local / "generation.json").read_text(encoding="utf-8"))["identity"]
    candidate["identity"] = hashlib.sha256(json.dumps({"generation": generation, "files": candidate["files"],
                                                     "exe": candidate["executable"]}, sort_keys=True).encode()).hexdigest()
    atomic_json(candidate_path, candidate)
    print("DLC importado e verificado. Requer runtime com suporte a conteúdo adicional.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--keys", required=True, type=Path)
    parser.add_argument("--local", required=True, type=Path)
    parser.add_argument("--reinstall", action="store_true", help="Valida o NSP novamente e preserva a instalação anterior em backup.")
    args = parser.parse_args()
    with exclusive_build(args.local):
        install(args.package, args.keys, args.local, reinstall=args.reinstall)
