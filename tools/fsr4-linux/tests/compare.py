#!/usr/bin/env python3
"""Compare a native FSR 4.1.2 output against the AMD reference.

Usage:
  compare.py <reference.rgba8> <native.rgba8> <width> <height>

Reports byte-exact match, max channel difference, MAE, PSNR and the fraction of
differing pixels. Exit code 0 when the images are identical, 2 when they are
close but not identical, 1 when they differ grossly.
"""

import argparse
import math
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("native")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    args = ap.parse_args()

    ref = open(args.reference, "rb").read()
    nat = open(args.native, "rb").read()
    expected = args.width * args.height * 4
    if len(ref) != expected or len(nat) != expected:
        print(f"size mismatch: ref={len(ref)} native={len(nat)} expected={expected}")
        return 1

    if ref == nat:
        print("IDENTICAL (byte-for-byte)")
        return 0

    max_diff = 0
    total_diff = 0
    differing = 0
    sq = 0.0
    for a, b in zip(ref, nat):
        d = abs(a - b)
        if d:
            differing += 1
            total_diff += d
            sq += float(d) * d
            if d > max_diff:
                max_diff = d
    n = len(ref)
    mae = total_diff / n
    mse = sq / n
    psnr = 99.0 if mse == 0 else 10.0 * math.log10((255.0 * 255.0) / mse)
    print(f"differing bytes: {differing}/{n} ({100.0 * differing / n:.4f}%)")
    print(f"max channel diff: {max_diff}")
    print(f"MAE: {mae:.6f}")
    print(f"PSNR: {psnr:.3f} dB")
    if max_diff <= 1 and mae < 0.01:
        print("RESULT: close (<=1 LSB)")
        return 2
    if psnr >= 45.0:
        print("RESULT: visually close (PSNR >= 45 dB)")
        return 2
    print("RESULT: mismatch")
    return 1


if __name__ == "__main__":
    sys.exit(main())
