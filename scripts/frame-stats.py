"""Frame-time statistics from a race log written with LARECOMP_VK_PROF=1 and MK8_FRAME_TIMES=1.

    python scripts/frame-stats.py local/race-grand-prix-xxxx [--from 3600 --to 15000] [...more sessions]

Frames within 3 of a window capture (frame-NNNNN.png) are left out: a capture stalls its frame on purpose.
Reports the mean FPS, percentiles and how many frames missed the 60 Hz budget, plus the slowest frames split
into backend execution, shader prefetch, presentation and the rest (as the runtime logs them).
"""
import argparse
import math
from pathlib import Path
import re

NUMBER = r'[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?'
FRAME = re.compile(r'^\[frame\] (\d+)' + (r' (' + NUMBER + r')') * 5 + r'\s*$', re.M)
SHOT = re.compile(r'saved .*frame-(\d+)\.png')


def percentile(values, p):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(round(p / 100 * (len(ordered) - 1))))]


def stats(session, first, last, slowest):
    log = Path(session) / 'game.log' if Path(session).is_dir() else Path(session)
    text = log.read_text(encoding='utf-8', errors='replace')
    shots = {int(n) for n in SHOT.findall(text)}
    near = {f + d for f in shots for d in range(-3, 4)}
    rows = [(int(m[0]), *map(float, m[1:])) for m in FRAME.findall(text)]
    rows = [r for r in rows if r[1] > 0 and all(math.isfinite(v) for v in r[1:])]
    rejected = len(re.findall(r'^\[frame\]', text, re.M)) - len(rows)
    rows = [r for r in rows if first <= r[0] <= last and r[0] not in near]
    if not rows:
        return f'{log}: no [frame] lines in {first}-{last} (run with LARECOMP_VK_PROF=1 MK8_FRAME_TIMES=1)'
    total = [r[1] for r in rows]
    out = [f'{log.parent.name}: {len(rows)} frames {rows[0][0]}-{rows[-1][0]}',
           f'  rejected invalid rows in log: {rejected}; capture-adjacent frames excluded',
           f'  mean {1000 * len(total) / sum(total):.2f} FPS | p50 {percentile(total, 50):.2f} | p95 {percentile(total, 95):.2f}'
           f' | p99 {percentile(total, 99):.2f} | max {max(total):.2f} ms',
           '  over budget: ' + ', '.join(f'>{b} ms {sum(t > b for t in total)}' for b in (17.5, 20, 25, 33.4, 50))]
    if slowest:
        out.append('  slowest: frame total exec prefetch present other')
        for r in sorted(rows, key=lambda r: -r[1])[:slowest]:
            out.append('    ' + ' '.join(f'{v:.1f}' if isinstance(v, float) else str(v) for v in r))
    return '\n'.join(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sessions', nargs='+')
    parser.add_argument('--from', dest='first', type=int, default=0)
    parser.add_argument('--to', dest='last', type=int, default=10**9)
    parser.add_argument('--slowest', type=int, default=8)
    args = parser.parse_args()
    for session in args.sessions:
        print(stats(session, args.first, args.last, args.slowest))
