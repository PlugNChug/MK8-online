#!/usr/bin/env python3
"""Local probe for te_dlss5_native.dll (NGX Feature 18 / DLSS 5 Neural Rendering).

This is an experiment, not part of the game or the launcher. It never downloads
anything: point --dll at a runtime set you obtained yourself. The NR runtime is a
modified NVIDIA binary (see docs/DLSS5-NR.md), so it is not redistributable and
must stay outside the repository.

Decodes a PNG (8-bit RGB/RGBA, non-interlaced), calls te_nr_create /
te_nr_process_rgba / te_nr_reset / te_nr_destroy through ctypes, writes the
result as PNG + raw RGBA, and prints machine-readable statistics. No third-party
Python modules are required on the target machine.

Observed on 2026-11-20, RTX 5080 / driver 621.01:
  * te_nr_create only succeeds when settings include runtimeDir (the directory
    holding nvngx_dlssnr.dll). With {} it returns NULL and last_error is the
    terse string "LoadNvngxDlssnr".
  * te_nr_reset / te_nr_destroy can spin at 100% CPU for many minutes after a
    successful create. --skip-destroy exits with os._exit instead; the OS then
    reclaims the process. Do not treat the hang as a processing failure.

Usage (Windows, runtime files beside the script):
  python dlss5_te_harness.py --input frame.png --out out/nr \
      --width 1280 --height 720 --frames 3 \
      --runtime-dir C:/path/to/runtime --log-file out/nr.log --skip-destroy
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import struct
import sys
import time
import zlib
from pathlib import Path


# ---------------------------------------------------------------- PNG codec
def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"not a PNG: {path}")
    pos = 8
    width = height = bit_depth = color_type = interlace = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _comp, _filt, interlace = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if bit_depth != 8 or color_type not in (2, 6) or interlace != 0:
        raise SystemExit(f"unsupported PNG (bit_depth={bit_depth} color_type={color_type} interlace={interlace})")
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    out = bytearray(height * stride)
    prev = bytearray(stride)
    off = 0
    for y in range(height):
        f = raw[off]
        off += 1
        line = bytearray(raw[off:off + stride])
        off += stride
        if f == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                upleft = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(left, prev[i], upleft)) & 0xFF
        elif f != 0:
            raise SystemExit(f"bad PNG filter {f} at row {y}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if channels == 4:
        rgba = bytes(out)
    else:
        rgba = bytearray(width * height * 4)
        rgba[0::4] = out[0::3]
        rgba[1::4] = out[1::3]
        rgba[2::4] = out[2::3]
        rgba[3::4] = b"\xff" * (width * height)
        rgba = bytes(rgba)
    return width, height, rgba


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    stride = width * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    body = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))
    path.write_bytes(body)


# ---------------------------------------------------------------- stats
def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stats(name: str, data: bytes, reference: bytes | None = None) -> dict:
    uniq = len(set(data))
    nz = sum(1 for b in data if b)
    digest = hashlib.sha256(data).hexdigest()
    result = {"name": name, "bytes": len(data), "sha256": digest,
              "nonzero_bytes": nz, "unique_byte_values": uniq}
    channels = [data[i::4] for i in range(4)]
    result["mean_rgba"] = [round(sum(c) / len(c), 3) for c in channels]
    if reference is not None and len(reference) == len(data):
        diffs = [abs(a - b) for a, b in zip(data, reference)]
        changed = sum(1 for d in diffs if d)
        result["mean_abs_diff"] = round(sum(diffs) / len(diffs), 4)
        result["max_abs_diff"] = max(diffs) if diffs else 0
        result["changed_bytes"] = changed
        result["changed_fraction"] = round(changed / len(diffs), 6)
        result["identical"] = changed == 0
    return result


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", default=str(Path(__file__).with_name("te_dlss5_native.dll")))
    ap.add_argument("--input", required=True)
    ap.add_argument("--out", required=True, help="output prefix (writes .png and .rgba)")
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--settings", default="{}", help="JSON for te_nr_create; {runtimeDir} is substituted")
    ap.add_argument("--runtime-dir", default=None, help="sets settings.runtimeDir to this directory")
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--guided", action="store_true", help="also probe te_nr_process_rgba_guided with zero depth")
    ap.add_argument("--log-file", default=None, help="tee stdout to this file, flushed per line")
    ap.add_argument("--skip-destroy", action="store_true", help="exit without te_nr_reset/te_nr_destroy (known to hang)")
    args = ap.parse_args()

    if args.log_file:
        class _Tee:
            def __init__(self, stream, path):
                self.stream = stream
                self.file = open(path, "w", encoding="utf-8", buffering=1)

            def write(self, data):
                self.stream.write(data)
                self.file.write(data)
                self.file.flush()
                return len(data)

            def flush(self):
                self.stream.flush()
                self.file.flush()

        sys.stdout = _Tee(sys.stdout, args.log_file)
        sys.stderr = sys.stdout

    dll_path = Path(args.dll).resolve()
    workdir = dll_path.parent
    os.chdir(workdir)
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(workdir))
    os.environ["PATH"] = str(workdir) + os.pathsep + os.environ.get("PATH", "")

    w, h = args.width, args.height
    iw, ih, rgba = read_png(Path(args.input))
    if (iw, ih) != (w, h):
        raise SystemExit(f"input is {iw}x{ih}, expected {w}x{h}")
    print(f"input: {args.input} {iw}x{ih} sha256={hashlib.sha256(rgba).hexdigest()}", flush=True)

    print(f"dll: {dll_path} bytes={dll_path.stat().st_size} sha256={sha256_file(dll_path)}", flush=True)
    lib = ctypes.WinDLL(str(dll_path), winmode=0)

    lib.te_nr_create.restype = ctypes.c_void_p
    lib.te_nr_create.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p]
    lib.te_nr_process_rgba.restype = ctypes.c_int
    lib.te_nr_process_rgba.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
    lib.te_nr_reset.argtypes = [ctypes.c_void_p]
    lib.te_nr_reset.restype = None
    lib.te_nr_destroy.argtypes = [ctypes.c_void_p]
    lib.te_nr_destroy.restype = None
    lib.te_nr_last_error.restype = ctypes.c_char_p
    lib.te_nr_last_error.argtypes = []

    def last_error() -> str:
        try:
            return (lib.te_nr_last_error() or b"").decode("utf-8", "replace")
        except Exception as exc:  # pragma: no cover
            return f"<last_error failed: {exc}>"

    settings_obj = json.loads(args.settings)
    if args.runtime_dir:
        settings_obj["runtimeDir"] = args.runtime_dir
    if settings_obj.get("runtimeDir") == "{runtimeDir}":
        settings_obj["runtimeDir"] = str(workdir)
    settings = json.dumps(settings_obj)
    print(f"create {w}x{h} settings={settings}", flush=True)
    t0 = time.perf_counter()
    handle = lib.te_nr_create(w, h, settings.encode("utf-8"))
    create_ms = (time.perf_counter() - t0) * 1000.0
    print(f"te_nr_create -> {handle} ({create_ms:.1f} ms) last_error={last_error()!r}", flush=True)
    if not handle:
        return 2

    frame_bytes = w * h * 4
    size = len(rgba)
    src = ctypes.create_string_buffer(rgba, size)

    results = []
    ok = True
    for i in range(args.frames):
        dst = ctypes.create_string_buffer(frame_bytes)
        t0 = time.perf_counter()
        rc = lib.te_nr_process_rgba(handle, src, dst, frame_bytes)
        ms = (time.perf_counter() - t0) * 1000.0
        print(f"frame {i}: rc={rc} {ms:.1f} ms last_error={last_error()!r}", flush=True)
        if rc != 0:
            ok = False
            break
        out = dst.raw[:frame_bytes]
        info = stats(f"frame{i}", out, rgba)
        info["rc"] = rc
        info["ms"] = round(ms, 1)
        results.append(info)
        png_path = Path(f"{args.out}.frame{i}.png")
        write_png(png_path, w, h, out)
        Path(f"{args.out}.frame{i}.rgba").write_bytes(out)
        print("  " + json.dumps(info, sort_keys=True), flush=True)

    if args.guided:
        # Honest API probe: zero float32 depth, NULL motion. Not a quality test.
        depth = ctypes.create_string_buffer(frame_bytes)  # 4 bytes per pixel, enough for float32 depth
        dst = ctypes.create_string_buffer(frame_bytes)
        fn = getattr(lib, "te_nr_process_rgba_guided", None)
        if fn is None:
            print("guided: export missing", flush=True)
        else:
            fn.restype = ctypes.c_int
            fn.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                           ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
            t0 = time.perf_counter()
            rc = fn(handle, src, None, depth, dst, frame_bytes)
            ms = (time.perf_counter() - t0) * 1000.0
            print(f"guided(zero depth, null motion): rc={rc} {ms:.1f} ms last_error={last_error()!r}", flush=True)
            if rc == 0:
                out = dst.raw[:frame_bytes]
                print("  " + json.dumps(stats("guided", out, rgba), sort_keys=True), flush=True)
                write_png(Path(f"{args.out}.guided.png"), w, h, out)

    if args.skip_destroy:
        print("skipping te_nr_reset/te_nr_destroy (--skip-destroy); exiting via os._exit", flush=True)
        sys.stdout.flush()
        os._exit(0 if ok and results else 3)
    lib.te_nr_reset(handle)
    lib.te_nr_destroy(handle)
    print(f"te_nr_reset+destroy done; overall={'ok' if ok and results else 'failed'}", flush=True)
    return 0 if ok and results else 3


if __name__ == "__main__":
    sys.exit(main())
