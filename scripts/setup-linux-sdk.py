"""Build pinned SDL3 and install modern Vulkan headers in the private Linux tools directory."""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

from downloads import fetch

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get("MK8_NATIVE_TOOLS") or ROOT / "local/native-tools").resolve()
SDK = TOOLS / "linux-sdk"
SDL_VERSION = "3.4.18"
SDL_SHA256 = "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"
VULKAN_COMMIT = "7feeb71f59b8c1531ef5f7bb4b36e1aac46a0acb"  # v1.4.363
VULKAN_SHA256 = "6f7263dc91708f2df370ef4f0ef13b27609f6cddc1165e4d7f717e6746d18fea"
FILES = ("lib/libSDL3.so.0", "lib/pkgconfig/sdl3.pc", "include/SDL3/SDL.h", "include/vulkan/vulkan.h")


def ready(folder):
    receipt = folder / "sources.sha256"
    return (receipt.is_file() and receipt.read_text(encoding="ascii").split() == [SDL_SHA256, VULKAN_SHA256]
            and all((folder / name).is_file() for name in FILES))


def extract(archive, destination):
    with tarfile.open(archive, "r:gz") as source:
        source.extractall(destination, filter="data")


def setup():
    TOOLS.mkdir(parents=True, exist_ok=True)
    with (TOOLS / ".linux-sdk.lock").open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if ready(SDK):
            return SDK
        if shutil.disk_usage(TOOLS).free < 4 * 1024**3 + 256 * 1024**2:
            raise ValueError("O SDK Linux requer espaço além da reserva de 4 GiB.")
        return install_sdk()


def install_sdk():
    cmake = shutil.which("cmake")
    if not cmake:
        raise ValueError("CMake ausente. Instale o pacote cmake no ambiente Linux usado para compilar o SDL.")
    sdl = fetch(f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz",
                TOOLS / f"SDL3-{SDL_VERSION}.tar.gz", sha256=SDL_SHA256)
    headers = fetch(f"https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/{VULKAN_COMMIT}",
                    TOOLS / f"Vulkan-Headers-{VULKAN_COMMIT}.tar.gz", sha256=VULKAN_SHA256)
    with tempfile.TemporaryDirectory(prefix="mk8-linux-sdk-", dir=TOOLS) as temp:
        work = Path(temp)
        extract(sdl, work)
        extract(headers, work)
        source = work / f"SDL3-{SDL_VERSION}"
        build = work / "build"
        stage = work / "stage"
        subprocess.run([cmake, "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
                        f"-DCMAKE_INSTALL_PREFIX={SDK}", "-DCMAKE_INSTALL_LIBDIR=lib",
                        "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_INSTALL_TESTS=OFF", "-DSDL_TEST_LIBRARY=OFF"],
                       check=True)
        subprocess.run([cmake, "--build", build, "--parallel", str(min(8, os.cpu_count() or 1))], check=True)
        subprocess.run([cmake, "--install", build], env=dict(os.environ, DESTDIR=str(stage)), check=True)
        prepared = stage / SDK.relative_to(SDK.anchor)
        vulkan = work / f"Vulkan-Headers-{VULKAN_COMMIT}"
        for name in ("vulkan", "vk_video"):
            shutil.copytree(vulkan / "include" / name, prepared / "include" / name)
        licenses = prepared / "share/licenses"
        shutil.copytree(vulkan / "LICENSES", licenses / "Vulkan-Headers", dirs_exist_ok=True)
        shutil.copyfile(vulkan / "LICENSE.md", licenses / "Vulkan-Headers/LICENSE.md")
        (prepared / "sources.sha256").write_text(f"{SDL_SHA256}\n{VULKAN_SHA256}\n", encoding="ascii")
        if not ready(prepared):
            raise ValueError("SDK Linux incompleto após compilação.")
        if SDK.exists():
            backup = TOOLS / f"linux-sdk-previous-{work.name.rsplit('-', 1)[-1]}"
            SDK.rename(backup)
            print(f"SDK anterior preservado em {backup}", flush=True)
        prepared.rename(SDK)
    return SDK


if __name__ == "__main__":
    print(setup())
