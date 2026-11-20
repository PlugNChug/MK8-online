"""Fetch pinned vendor SDK artefacts that this project must never bundle.

NVIDIA DLSS/NGX and Intel XeSS are optional host-side components. Their binaries
are not part of the repository, the AppImage, the tarball or the Windows setup:
the user downloads the official file after reading its licence. This tool is the
download half of that contract.

It is deliberately small and fail-closed:

* every artefact is pinned by official URL, byte size and SHA-256;
* a licence acknowledgement is required before any network access;
* the licence text is fetched and verified first and stored beside the files;
* a size/hash mismatch removes the partial download and leaves an existing good
  copy untouched (``--force`` is required to replace a file that differs);
* nothing is written outside ``$XDG_DATA_HOME/mk8online/<vendor>/`` and the
  manifest may not escape that directory.

Usage::

    python3 tools/fetch_vendor_sdk.py --list
    python3 tools/fetch_vendor_sdk.py --accept-license nvidia
    python3 tools/fetch_vendor_sdk.py --accept-license intel --only XeSS_SDK_3.0.2.zip
    python3 tools/fetch_vendor_sdk.py --check

Exit codes: 0 success, 2 licence not accepted, 3 verification failure,
4 network/HTTP failure, 5 unsafe manifest path, 6 not enough free disk.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import sys
from typing import Callable, Iterable
from urllib.parse import urlparse
import urllib.request

MIN_FREE_BYTES = 4 << 30  # project rule: keep at least 4 GiB free
CHUNK = 1 << 20

# Pinned 2026-11-20. Sizes and hashes were produced by downloading these exact
# URLs on that date; the GitHub release API reports the same sha256 digests for
# the release assets. Changing an entry requires a new download and a new hash.
VENDORS: dict[str, dict] = {
    "nvidia": {
        "title": "NVIDIA DLSS / NGX for Linux x86_64 (DLSS SDK v310.9.16)",
        "homepage": "https://github.com/NVIDIA/DLSS/tree/v310.9.16",
        "redistribution": (
            "NVIDIA RTX SDKs License allows object-code distribution only as part "
            "of an application with material additional functionality; this project "
            "still does not bundle it."
        ),
        "license": {
            "name": "LICENSE.txt",
            "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/LICENSE.txt",
            "size": 26620,
            "sha256": "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0",
            "spdx": "LicenseRef-NVIDIA-RTX-SDKs",
            "summary": (
                "NVIDIA RTX SDKs License. Distribution is allowed only as part of "
                "an application that adds material functionality, never as a "
                "stand-alone product; reverse engineering is prohibited."
            ),
        },
        "artifacts": [
            {
                "name": "libnvidia-ngx-dlss.so.310.9.16",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16",
                "size": 59481944,
                "sha256": "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6",
                "note": "DLSS Super Resolution / DLAA, ELF x86-64, exports NVSDK_NGX_VULKAN_*",
            },
            {
                "name": "libnvidia-ngx-dlssd.so.310.9.16",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.310.9.16",
                "size": 48238232,
                "sha256": "a5d59de18841f15779039dda567a15dd723ba737915f489271fdb5f109fbfb96",
                "note": "DLSS Ray Reconstruction, ELF x86-64",
            },
            {
                "name": "libnvidia-ngx-dlssg.so.310.9.16",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.310.9.16",
                "size": 7391232,
                "sha256": "574839cae01c835de4d5845c36ca9cf75361f8160bd52397142c36a24e29e6f7",
                "note": "DLSS Frame Generation, ELF x86-64",
            },
            {
                "name": "ngx/include/nvsdk_ngx.h",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include/nvsdk_ngx.h",
                "size": 40289,
                "sha256": "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848",
            },
            {
                "name": "ngx/include/nvsdk_ngx_vk.h",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include/nvsdk_ngx_vk.h",
                "size": 38382,
                "sha256": "2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d",
            },
            {
                "name": "ngx/include/nvsdk_ngx_helpers_vk.h",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include/nvsdk_ngx_helpers_vk.h",
                "size": 19720,
                "sha256": "c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623",
            },
            {
                "name": "ngx/include/nvsdk_ngx_defs.h",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include/nvsdk_ngx_defs.h",
                "size": 44313,
                "sha256": "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e",
            },
            {
                "name": "ngx/include/nvsdk_ngx_defs_dlssg.h",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include/nvsdk_ngx_defs_dlssg.h",
                "size": 23208,
                "sha256": "c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1",
            },
            {
                "name": "ngx/include/nvsdk_ngx_helpers_dlssg_vk.h",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include/nvsdk_ngx_helpers_dlssg_vk.h",
                "size": 17077,
                "sha256": "b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316",
            },
            {
                "name": "ngx/include/nvsdk_ngx_params.h",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include/nvsdk_ngx_params.h",
                "size": 7744,
                "sha256": "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc",
            },
            {
                "name": "ngx/include/nvsdk_ngx_loader.h",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include/nvsdk_ngx_loader.h",
                "size": 13252,
                "sha256": "abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75",
            },
            {
                "name": "ngx/include/nvsdk_ngx_defs_vk.h",
                "url": "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include/nvsdk_ngx_defs_vk.h",
                "size": 3196,
                "sha256": "0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031",
            },
        ],
    },
    "intel": {
        "title": "Intel XeSS SDK 3.0.2 (Windows x64)",
        "homepage": "https://github.com/intel/xess/releases/tag/v3.0.2",
        "redistribution": (
            "Intel Simplified Software License (October 2022) permits unmodified "
            "binary redistribution with the notice; this project still does not "
            "bundle it."
        ),
        "license": {
            "name": "LICENSE.txt",
            "url": "https://raw.githubusercontent.com/intel/xess/v3.0.2/LICENSE.txt",
            "size": 4194,
            "sha256": "784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df",
            "spdx": "LicenseRef-Intel-Simplified-Software",
            "summary": (
                "Intel Simplified Software License (October 2022). Binary use and "
                "redistribution are allowed without modification and with the "
                "notice; reverse engineering, decompilation and disassembly are "
                "prohibited."
            ),
        },
        "artifacts": [
            {
                "name": "XeSS_SDK_3.0.2.zip",
                "url": "https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip",
                "size": 76839749,
                "sha256": "88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990",
                "note": "Windows-only SDK: libxess.dll, libxess_fg.dll, libxell.dll, headers, docs",
            },
        ],
    },
    "nvidia-streamline": {
        "title": "NVIDIA Streamline SDK 2.14.16 (Windows x64)",
        "homepage": "https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16",
        "redistribution": (
            "Streamline framework source is MIT; the DLSS/NGX DLLs it ships are "
            "under the NVIDIA RTX SDKs License. This project does not bundle either."
        ),
        "license": {
            "name": "LICENSE.txt",
            "url": "https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/v2.14.16/license.txt",
            "size": 1591,
            "sha256": "7b6f23e7d6f3ad6292f9308d2b42cdc3d82ae4e9b2abb55f230279d83bedd43d",
            "spdx": "MIT",
            "summary": (
                "MIT for the Streamline framework. The shipped nvngx_dlss*.dll "
                "files are covered by bin/x64/nvngx_dlss.license.txt (NVIDIA RTX "
                "SDKs License), which must be read before use."
            ),
        },
        "artifacts": [
            {
                "name": "streamline-sdk-v2.14.16.zip",
                "url": "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip",
                "size": 276016170,
                "sha256": "32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b",
                "note": "Windows x64 package; no Linux .so and no sl.dlss_nr.dll",
            },
        ],
    },
}


class VendorSdkError(Exception):
    """Base class for the failures this tool reports without a traceback."""


class LicenseNotAccepted(VendorSdkError):
    pass


class VerificationError(VendorSdkError):
    pass


class UnsafePath(VendorSdkError):
    pass


class NotEnoughSpace(VendorSdkError):
    pass


def data_root(environ: dict[str, str] | None = None) -> Path:
    """``$XDG_DATA_HOME/mk8online`` (absolute), falling back to ``~/.local/share``."""
    env = os.environ if environ is None else environ
    base = env.get("XDG_DATA_HOME")
    if base:
        path = Path(base)
        if not path.is_absolute():
            raise VendorSdkError("XDG_DATA_HOME is not absolute: " + base)
    else:
        home = env.get("HOME")
        if not home:
            raise VendorSdkError("XDG_DATA_HOME and HOME are both unset")
        path = Path(home) / ".local" / "share"
    return path / "mk8online"


def vendor_root(vendor: str, root: Path | None = None) -> Path:
    if not vendor or vendor in {".", ".."} or "/" in vendor or "\\" in vendor:
        raise UnsafePath("invalid vendor name: " + repr(vendor))
    return (data_root() if root is None else root) / vendor


def _safe_destination(base: Path, name: str) -> Path:
    parts = Path(name).parts
    if not parts or Path(name).is_absolute() or any(p in {"", ".", ".."} for p in parts):
        raise UnsafePath("manifest path escapes the vendor directory: " + repr(name))
    target = base.joinpath(*parts)
    base_resolved = base.resolve()
    if base_resolved not in target.resolve().parents and target.resolve() != base_resolved:
        raise UnsafePath("manifest path escapes the vendor directory: " + repr(name))
    return target


def _check_url(url: str) -> None:
    parsed = urlparse(url)
    if parsed.scheme == "https" and parsed.netloc:
        return
    # Plain HTTP is accepted only for the loopback fixtures used by the tests.
    if parsed.scheme == "http" and parsed.hostname in {"127.0.0.1", "::1", "localhost"}:
        return
    raise VendorSdkError("refusing non-HTTPS URL: " + url)


def verify_file(path: Path, size: int, sha256: str) -> None:
    if not path.is_file():
        raise VerificationError("missing: " + str(path))
    actual_size = path.stat().st_size
    if actual_size != size:
        raise VerificationError(
            f"size mismatch for {path}: expected {size}, got {actual_size}")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(CHUNK):
            digest.update(chunk)
    actual_sha = digest.hexdigest()
    if actual_sha != sha256:
        raise VerificationError(
            f"SHA-256 mismatch for {path}: expected {sha256}, got {actual_sha}")


def _matches(entry: dict, path: Path) -> bool:
    try:
        verify_file(path, entry["size"], entry["sha256"])
    except VerificationError:
        return False
    return True


def _fetch(
    url: str,
    target: Path,
    size: int,
    sha256: str,
    opener: Callable,
    quiet: bool,
) -> Path:
    """Download to ``target.part``, verify, then replace ``target`` atomically."""
    _check_url(url)
    part = target.with_name(target.name + ".part")
    part.unlink(missing_ok=True)
    digest = hashlib.sha256()
    total = 0
    try:
        with opener(url, timeout=120) as response, part.open("wb") as out:
            while chunk := response.read(CHUNK):
                out.write(chunk)
                digest.update(chunk)
                total += len(chunk)
            out.flush()
            os.fsync(out.fileno())
        if total != size:
            raise VerificationError(
                f"size mismatch for {url}: expected {size}, got {total}")
        actual = digest.hexdigest()
        if actual != sha256:
            raise VerificationError(
                f"SHA-256 mismatch for {url}: expected {sha256}, got {actual}")
        os.replace(part, target)
        return target
    except BaseException:
        part.unlink(missing_ok=True)
        raise
    finally:
        if not quiet:
            status = "ok" if not part.exists() else "failed"
            print(f"  {status:6} {total:>12,} B  {target.name}", file=sys.stderr)


def _entry(config: dict, key: str) -> dict:
    entry = config[key]
    for field in ("name", "url", "size", "sha256"):
        if field not in entry:
            raise VendorSdkError(f"manifest entry {key!r} lacks {field!r}")
    return entry


def _need_space(destination: Path, needed: int, disk_usage: Callable) -> None:
    usage = disk_usage(destination)
    if usage.free < needed + MIN_FREE_BYTES:
        raise NotEnoughSpace(
            f"not enough space under {destination}: need {needed} B plus the "
            f"4 GiB reserve, have {usage.free} B")


def fetch_vendor(
    vendor: str,
    accepted: Iterable[str],
    config: dict,
    root: Path | None = None,
    opener: Callable = urllib.request.urlopen,
    quiet: bool = False,
    force: bool = False,
    only: Iterable[str] | None = None,
    disk_usage: Callable = shutil.disk_usage,
) -> list[Path]:
    """Fetch one vendor's artefacts after the licence gate.

    ``accepted`` is the set of vendor names the user acknowledged. Passing a
    vendor that is not in it raises before any network access.
    """
    if vendor not in set(accepted):
        raise LicenseNotAccepted(
            f"licence for {vendor!r} was not accepted; run with --accept-license {vendor}")
    destination = vendor_root(vendor, root)
    destination.mkdir(parents=True, exist_ok=True)
    selected = set(only or ())
    unknown = selected - {entry["name"] for entry in config["artifacts"]}
    if unknown:
        raise VendorSdkError("unknown artifact(s): " + ", ".join(sorted(unknown)))

    def wanted(entry: dict) -> bool:
        return not selected or entry["name"] in selected

    # The licence text is always stored beside the artefacts, even with --only,
    # so the terms travel with the files they cover.
    license_entry = _entry(config, "license")
    license_path = _safe_destination(destination, license_entry["name"])
    if not _matches(license_entry, license_path):
        if license_path.exists() and not force:
            raise VerificationError(
                f"{license_path} differs from the pinned licence text; "
                "rerun with --force to replace it")
        _need_space(destination, license_entry["size"], disk_usage)
        _fetch(license_entry["url"], license_path, license_entry["size"],
               license_entry["sha256"], opener, quiet)
    verify_file(license_path, license_entry["size"], license_entry["sha256"])
    if not quiet:
        print(f"licence: {license_path}", file=sys.stderr)

    written: list[Path] = []
    for entry in config["artifacts"]:
        if not wanted(entry):
            continue
        target = _safe_destination(destination, entry["name"])
        target.parent.mkdir(parents=True, exist_ok=True)
        if not force and _matches(entry, target):
            if not quiet:
                print(f"  cached {target.name}", file=sys.stderr)
            written.append(target)
            continue
        if target.exists() and not force:
            raise VerificationError(
                f"{target} differs from the pin; rerun with --force to replace it")
        _need_space(destination, entry["size"], disk_usage)
        _fetch(entry["url"], target, entry["size"], entry["sha256"], opener, quiet)
        verify_file(target, entry["size"], entry["sha256"])
        written.append(target)
    return written


def check_vendor(
    vendor: str,
    config: dict,
    root: Path | None = None,
) -> list[tuple[str, str]]:
    """Return ``(name, status)`` for the licence and every artefact, offline."""
    destination = vendor_root(vendor, root)
    results: list[tuple[str, str]] = []
    for entry in [config["license"], *config["artifacts"]]:
        path = _safe_destination(destination, entry["name"])
        try:
            verify_file(path, entry["size"], entry["sha256"])
            results.append((str(path), "ok"))
        except VerificationError as error:
            results.append((str(path), str(error)))
    return results


def _print_list(vendors: dict[str, dict]) -> None:
    for vendor, config in vendors.items():
        print(f"{vendor}: {config['title']}")
        print(f"  licence: {config['license']['spdx']}  {config['license']['url']}")
        for entry in config["artifacts"]:
            print(f"  {entry['size']:>12,} B  {entry['sha256']}  {entry['name']}")
        print()


def _select_vendors(requested: list[str] | None) -> list[str]:
    if not requested:
        return list(VENDORS)
    unknown = [name for name in requested if name not in VENDORS]
    if unknown:
        raise VendorSdkError(
            "unknown vendor(s): " + ", ".join(unknown) + "; known: " + ", ".join(VENDORS))
    return requested


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Download pinned vendor SDK artefacts outside the repository.")
    parser.add_argument("--list", action="store_true",
                        help="print the pinned manifest and exit")
    parser.add_argument("--check", action="store_true",
                        help="verify the local copies without downloading")
    parser.add_argument("--vendor", action="append", choices=sorted(VENDORS),
                        help="vendor to act on (default: all)")
    parser.add_argument("--accept-license", action="append", default=[],
                        metavar="VENDOR", help="acknowledge that vendor's licence")
    parser.add_argument("--only", action="append", default=[],
                        metavar="NAME", help="only this artefact name")
    parser.add_argument("--force", action="store_true",
                        help="replace a local file that differs from the pin")
    parser.add_argument("--quiet", action="store_true", help="no progress output")
    args = parser.parse_args(argv)

    if args.list:
        _print_list(VENDORS)
        return 0

    try:
        vendors = _select_vendors(args.vendor)
        if args.only and not args.vendor:
            vendors = [vendor for vendor in vendors
                       if any(entry["name"] in args.only for entry in VENDORS[vendor]["artifacts"])]
            if not vendors:
                raise VendorSdkError("no vendor provides: " + ", ".join(args.only))
        if args.check:
            failed = False
            for vendor in vendors:
                for name, status in check_vendor(vendor, VENDORS[vendor]):
                    print(f"{'ok' if status == 'ok' else 'FAIL':4}  {name}"
                          + ("" if status == "ok" else f"\n      {status}"))
                    failed |= status != "ok"
            return 3 if failed else 0

        unknown = [name for name in args.accept_license if name not in VENDORS]
        if unknown:
            raise VendorSdkError("unknown licence name(s): " + ", ".join(unknown))
        missing = [vendor for vendor in vendors if vendor not in args.accept_license]
        if missing:
            raise LicenseNotAccepted(
                "licence not accepted for: " + ", ".join(missing)
                + "; pass --accept-license <vendor> for each one")
        for vendor in vendors:
            print(f"{vendor}: {VENDORS[vendor]['title']}", file=sys.stderr)
            print(f"  licence {VENDORS[vendor]['license']['spdx']}: "
                  f"{VENDORS[vendor]['license']['url']}", file=sys.stderr)
            print(f"  {VENDORS[vendor]['license']['summary']}", file=sys.stderr)
            fetch_vendor(vendor, args.accept_license, VENDORS[vendor],
                         opener=urllib.request.urlopen, quiet=args.quiet,
                         force=args.force, only=args.only)
        return 0
    except LicenseNotAccepted as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except VerificationError as error:
        print(f"error: {error}", file=sys.stderr)
        return 3
    except UnsafePath as error:
        print(f"error: {error}", file=sys.stderr)
        return 5
    except NotEnoughSpace as error:
        print(f"error: {error}", file=sys.stderr)
        return 6
    except (VendorSdkError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 4


if __name__ == "__main__":
    raise SystemExit(main())
