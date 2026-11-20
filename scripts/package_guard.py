"""Reject AMD FidelityFX provider artifacts from a release package.

The FSR provider is an optional, private component: its binaries, ML models and
frame-generation SPIR-V blobs must never be redistributed.  The release package
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
    (re.compile(r"(^|/)libmk8_mlfi_[^/]*\.so(?:\..*)?$", re.I),
     "AMD FidelityFX provider library"),
    (re.compile(r"(^|/)(?:models/[^/]+\.(?:bin|dat|weights)|[^/]+\.(?:onnx|weights))$", re.I),
     "generated model"),
    (re.compile(r"(^|/)(?:[^/]+\.spv|(?:fg|upscaler)_blobs\.(?:h|hpp|inc)|mlfi_shaders\.h)$", re.I),
     "generated shader"),
    (re.compile(r"(^|/)mk8d(?:game|[-_]code[-_][^/]+)\.(?:dll|so)(?:\.id)?$", re.I),
     "recompiled game module"),
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
                "Remove the artifact and rebuild the package."
            )


def _binary(name):
    name = name.lower()
    return name.endswith((".dll", ".so")) or ".so." in name


def _exports_symbol(data):
    if data.startswith(b"MZ"):
        # The PE export name table contains the exact NUL-terminated entry point.
        return b"\0" + SYMBOL.encode() + b"\0" in data
    if not data.startswith(b"\x7fELF"):
        return False
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
    if _binary(path.name):
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
                if _binary(name):
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
                if member.isfile() and _binary(member.name):
                    extracted = archive.extractfile(member)
                    data = extracted.read() if extracted is not None else b""
                    if _exports_symbol(data):
                        raise ForbiddenArtifact(
                            f"{path}:{member.name} exports {SYMBOL}\n"
                            "Remove the AMD FidelityFX provider from the release package."
                        )
        return
    raise ForbiddenArtifact(f"not a directory, zip or tar archive: {path}")
