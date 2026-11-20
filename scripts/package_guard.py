"""Reject AMD FidelityFX provider artifacts from a Linux release package.

The FSR provider is an optional, private component: its binaries, ML models and
frame-generation SPIR-V blobs must never be redistributed.  The Linux package
contains runtime *sources* (including the MIT FFX headers and the optional
bridge), so a name check is enough for files; ELF shared objects are also
checked for the FFX entry point in case a provider was copied in by accident.
"""
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import zipfile

SYMBOL = "ffxCreateContext"
# Paths that identify a provider binary or its private data.  Keep these
# precise: runtime/mk8/graphics/fsr.cpp and the MIT headers are source files and
# are allowed in the source payload.
FORBIDDEN = (
    (re.compile(r"(^|/)libamd_fidelityfx_[^/]*\.(?:so|dll)(?:\..*)?$", re.I),
     "AMD FidelityFX provider library"),
    (re.compile(r"(^|/)amd_fidelityfx_[^/]*\.(?:so|dll)(?:\..*)?$", re.I),
     "AMD FidelityFX provider library"),
    (re.compile(r"(^|/)fsr4-assets(?:/|$)", re.I),
     "FSR4 model assets"),
    (re.compile(r"(^|/)fg_blobs(?:/|$)", re.I),
     "frame-generation SPIR-V blobs"),
    (re.compile(r"(^|/)fg[-_][^/]*\.spv$", re.I),
     "frame-generation SPIR-V blob"),
)


class ForbiddenArtifact(RuntimeError):
    """A release would contain an AMD FidelityFX artifact."""


def _check_name(name):
    relative = name.replace("\\", "/")
    while relative.startswith("./"):
        relative = relative[2:]
    for pattern, description in FORBIDDEN:
        if pattern.search(relative):
            raise ForbiddenArtifact(
                f"{description} in the release package: {name}\n"
                "Remove the provider/assets and rebuild the package; the FSR provider is not redistributable."
            )


def _exports_symbol(data):
    with tempfile.NamedTemporaryFile(prefix="mk8-package-guard-", suffix=".so") as temporary:
        temporary.write(data)
        temporary.flush()
        try:
            result = subprocess.run(
                ["readelf", "--dyn-syms", "--wide", temporary.name],
                capture_output=True, text=True, check=False,
            )
        except FileNotFoundError:
            result = None
        if result is not None and result.returncode == 0:
            return re.search(rf"\b{re.escape(SYMBOL)}\b", result.stdout) is not None
        try:
            result = subprocess.run(
                ["nm", "-D", "--defined-only", temporary.name],
                capture_output=True, text=True, check=False,
            )
        except FileNotFoundError:
            result = None
        if result is not None and result.returncode == 0:
            return re.search(rf"\b{re.escape(SYMBOL)}\b", result.stdout) is not None
        # Last resort: the string is enough for a package that should not export it at all.
        return SYMBOL.encode() in data


def _audit_file(path):
    if path.name.endswith(".so") or ".so." in path.name:
        try:
            data = path.read_bytes()
        except OSError as error:
            raise ForbiddenArtifact(f"cannot read {path} to check for {SYMBOL}: {error}") from error
        if _exports_symbol(data):
            raise ForbiddenArtifact(
                f"{path} exports {SYMBOL}\n"
                "Remove the AMD FidelityFX provider from the release package."
            )


def audit_tree(root):
    """Check every path under *root*; raise ForbiddenArtifact on the first match."""
    root = Path(root)
    if not root.is_dir():
        raise ForbiddenArtifact(f"package directory not found: {root}")
    for path in sorted(root.rglob("*")):
        _check_name(path.relative_to(root).as_posix())
        if path.is_file():
            _audit_file(path)


def audit_archive(path):
    """Check a .zip/.tar.gz payload or package without extracting it to disk."""
    path = Path(path)
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            for name in archive.namelist():
                _check_name(name)
                if name.endswith(".so") or ".so." in name:
                    with archive.open(name) as member:
                        data = member.read()
                    if _exports_symbol(data):
                        raise ForbiddenArtifact(
                            f"{path}:{name} exports {SYMBOL}\n"
                            "Remove the AMD FidelityFX provider from the release package."
                        )
        return
    if tarfile.is_tarfile(path):
        with tarfile.open(path) as archive:
            for member in archive.getmembers():
                _check_name(member.name)
                if member.isfile() and (member.name.endswith(".so") or ".so." in member.name):
                    extracted = archive.extractfile(member)
                    data = extracted.read() if extracted is not None else b""
                    if _exports_symbol(data):
                        raise ForbiddenArtifact(
                            f"{path}:{member.name} exports {SYMBOL}\n"
                            "Remove the AMD FidelityFX provider from the release package."
                        )
        return
    raise ForbiddenArtifact(f"not a directory, zip or tar archive: {path}")
