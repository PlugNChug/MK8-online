"""Build the Windows extractor and bundle the runtime used by it."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "toolchain/import-native/Program.cs"
COMPILE = ROOT / "scripts/compile-import-tool.ps1"
DEST = ROOT / "tools/mk8-import"
LIBHAC_SHA256 = "bbbdba4ed64bd117e3f2631d0e33ebf9b6d6ae518d4a800a180768ce1a76a5a5"
LIBHAC_URL = "https://api.nuget.org/v3-flatcontainer/libhac/0.18.0/libhac.0.18.0.nupkg"


def main():
    dll = DEST / "mk8-import.dll"
    receipt = DEST / "build-sha256.txt"
    identity = hashlib.sha256(Path(__file__).read_bytes() + SOURCE.read_bytes() + COMPILE.read_bytes()).hexdigest()
    if dll.is_file() and receipt.is_file() and receipt.read_text().split() == [
        identity, hashlib.sha256(dll.read_bytes()).hexdigest()
    ] and (DEST / "dotnet/dotnet.exe").is_file():
        return
    pwsh = os.environ.get("MK8_IMPORT_PWSH") or shutil.which("pwsh")
    dotnet = shutil.which("dotnet")
    if not pwsh or not dotnet:
        raise ValueError("Para empacotar o extrator, instale PowerShell 7 e .NET 10 x64.")
    engine = Path(dotnet).resolve().parent
    shared = engine / "shared/Microsoft.NETCore.App"
    runtime = max((p for p in shared.glob("10.0.*") if p.name.replace(".", "").isdigit()),
                  key=lambda p: tuple(map(int, p.name.split("."))), default=None)
    if runtime is None or not (engine / "host/fxr" / runtime.name / "hostfxr.dll").is_file():
        raise ValueError("Runtime .NET 10 x64 incompleto no ambiente de empacotamento.")
    with tempfile.TemporaryDirectory(prefix="mk8-import-build-") as temp:
        work = Path(temp)
        package = ROOT / "local/libhac-import/libhac.0.18.0.nupkg"
        if not package.is_file():
            package = work / "libhac.nupkg"
            subprocess.run(["curl.exe", "--fail", "--silent", "--show-error", "--location",
                            "--proto", "=https", "--max-time", "90", "-o", str(package), LIBHAC_URL], check=True)
        if hashlib.sha256(package.read_bytes()).hexdigest() != LIBHAC_SHA256:
            raise ValueError("Hash da biblioteca de extração divergente.")
        lib = work / "LibHac.dll"
        with ZipFile(package) as archive:
            lib.write_bytes(archive.read("lib/net7.0/LibHac.dll"))
        built = work / dll.name
        subprocess.run([pwsh, "-NoProfile", "-NonInteractive", "-File", COMPILE,
                        "-Source", SOURCE, "-Runtime", runtime, "-LibHac", lib, "-Output", built], check=True)
        DEST.mkdir(parents=True, exist_ok=True)
        bundle = DEST / "dotnet"
        bundle.mkdir(exist_ok=True)
        for name in ("dotnet.exe", "LICENSE.txt", "ThirdPartyNotices.txt"):
            shutil.copyfile(engine / name, bundle / name)
        for folder in (runtime, engine / "host/fxr" / runtime.name):
            shutil.copytree(folder, bundle / folder.relative_to(engine), dirs_exist_ok=True)
        shutil.copyfile(lib, DEST / lib.name)
        shutil.copyfile(built, dll)
        (DEST / "mk8-import.runtimeconfig.json").write_text(json.dumps({"runtimeOptions": {
            "tfm": "net10.0", "rollForward": "Disable",
            "framework": {"name": "Microsoft.NETCore.App", "version": runtime.name}
        }}), encoding="utf-8")
        receipt.write_text(identity + "\n" + hashlib.sha256(dll.read_bytes()).hexdigest() + "\n")
    print("Extrator Windows pronto.", flush=True)


if __name__ == "__main__":
    main()
