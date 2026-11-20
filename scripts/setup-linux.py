"""Prepare the Linux tool environment and check native build dependencies."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

from downloads import fetch

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get("MK8_NATIVE_TOOLS") or ROOT / "local/native-tools")
VMA_PACKAGE = "mingw-w64-ucrt-x86_64-vulkan-memory-allocator-3.4.0-1-any.pkg.tar.zst"
VMA_SHA256 = "44c81162337915afecda3185f66fd98268a54454580b2717080c9dfef57fe781"
PACKAGES = ("sdl3", "vulkan", "shaderc", "libavformat", "libavcodec", "libavutil", "libswscale")


def pkg_config_env():
    env = dict(os.environ)
    fallback = str(TOOLS / "linux-sdk/lib/pkgconfig")
    env["PKG_CONFIG_PATH"] = os.pathsep.join(filter(None, (fallback, env.get("PKG_CONFIG_PATH"))))
    return env


def sdk_ready(vma=False):
    env = pkg_config_env()
    if subprocess.run(["pkg-config", "--atleast-version=3.4.18", "sdl3"], env=env).returncode:
        return False
    # Match the pkg-config include precedence and compiler used by toolchain/host.py.
    import shlex
    cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", *PACKAGES], env=env, text=True))
    include = TOOLS / "deps/linux/include"
    source = """
#include <vulkan/vulkan.h>
void check() {
    VkPhysicalDeviceShaderFloat8FeaturesEXT fp8{};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR matrix{};
    VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT maintenance{};
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT atomics{};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic_state{};
    VkSwapchainLatencyCreateInfoNV latency{};
}
"""
    if vma:
        if not (include / "vk_mem_alloc.h").is_file():
            return False
        source += '#include "vk_mem_alloc.h"\n'
    return subprocess.run(["clang++", "--target=x86_64-linux-gnu", "-march=x86-64-v2",
                           *cflags, "-I", include, "-std=c++17", "-fsyntax-only", "-x", "c++", "-"],
                          input=source, text=True).returncode == 0


def check_dependencies():
    required = ("clang", "clang++", "ld.lld", "llvm-ar", "pkg-config", "curl", "tar")
    missing = [name for name in required if not shutil.which(name)]
    if missing:
        raise ValueError("Ferramentas Linux ausentes: " + ", ".join(missing))
    subprocess.run(["pkg-config", "--print-errors", "--exists", *PACKAGES[1:]], check=True,
                   env=pkg_config_env())


def setup_vma():
    archive = TOOLS / VMA_PACKAGE
    TOOLS.mkdir(parents=True, exist_ok=True)
    fetch("https://repo.msys2.org/mingw/ucrt64/" + VMA_PACKAGE, archive, sha256=VMA_SHA256)
    include = TOOLS / "deps/linux/include"
    include.mkdir(parents=True, exist_ok=True)
    header = subprocess.check_output(["tar", "-xOf", archive, "ucrt64/include/vma/vk_mem_alloc.h"])
    pending = include / "vk_mem_alloc.h.pending"
    pending.write_bytes(header)
    pending.replace(include / "vk_mem_alloc.h")


def main():
    if "--check" in sys.argv:
        check_dependencies()
        if not sdk_ready(vma=True):
            raise ValueError("SDL, cabeçalhos Vulkan ou VMA ausentes ou incompatíveis.")
        return
    if shutil.disk_usage(ROOT).free < 4 * 1024**3 + 256 * 1024**2:
        raise ValueError("Ferramentas Linux requerem espaço além da reserva de 4 GiB.")
    check_dependencies()
    if not sdk_ready():
        subprocess.run([sys.executable, ROOT / "scripts/setup-linux-sdk.py"], check=True)
    setup_vma()
    if not sdk_ready(vma=True):
        raise ValueError("SDL, cabeçalhos Vulkan ou VMA ausentes ou incompatíveis.")
    if sys.version_info[:2] != (3, 12):
        raise ValueError("Instale Python 3.12 x64 com suporte a venv.")
    env = ROOT / "local/python"
    python = env / "bin/python"
    if not python.is_file():
        subprocess.run([sys.executable, "-I", "-m", "venv", "--copies", env], check=True)
    for requirements in ("upstream/smm2recomp/recomp/tools/requirements-gen.txt", "toolchain/requirements-host.txt"):
        subprocess.run([python, "-I", "-m", "pip", "--isolated", "install", "--only-binary=:all:",
                        "--require-hashes", "-r", ROOT / requirements], check=True)
    tool = ROOT / "tools/mk8-import"
    if not all((tool / name).is_file() for name in ("dotnet/dotnet", "mk8-import.dll", "build-sha256.txt")):
        subprocess.run([python, ROOT / "scripts/setup-import-tool.py"], check=True)


if __name__ == "__main__":
    main()
