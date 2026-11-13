"""Frame-time statistics from a race log written with LARECOMP_VK_PROF=1 and MK8_FRAME_TIMES=1.

    python scripts/frame-stats.py local/race-grand-prix-xxxx [--from 3600 --to 15000] [...more sessions]

Frames within 3 of a window or incident capture are left out: readback stalls its frame on purpose.
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
INCIDENT_CAPTURE = re.compile(r'\[gfx-incident\] capture host frame (\d+)')
PREVIEW_CAPTURE = re.compile(r'\[gfx-preview\] (start|end) host frame (\d+)')
HEAP = re.compile(r'^\[mk8:vram\] heap (\d+): (\d+) MB in (\d+) blocks, (\d+) MB used by (\d+) allocations; '
                  r'VMA estimate (\d+) of (\d+) MB; vertex cache (\d+) MB; (\d+) live textures$')
COMMANDS = re.compile(r'^\[mk8:vram\] command lists: (\d+) active, (\d+) retired, clock (\d+)$')


def memory_samples(text):
    heaps, commands = [], []
    frame = None
    for line in text.splitlines():
        match = FRAME.match(line)
        if match:
            frame = int(match[1])
        is_heap = line.startswith('[mk8:vram] heap ')
        is_commands = line.startswith('[mk8:vram] command lists:')
        if not (is_heap or is_commands):
            continue
        match = (HEAP if is_heap else COMMANDS).fullmatch(line)
        if not match:
            raise ValueError('Formato de telemetria VRAM desconhecido')
        if frame is None:
            raise ValueError('Telemetria VRAM sem quadro de referência; habilite MK8_FRAME_TIMES')
        names = ('heap', 'reserved_mib', 'blocks', 'allocated_mib', 'allocations',
                 'estimated_mib', 'budget_mib', 'vertex_mib', 'textures') if is_heap else ('active', 'retired', 'clock')
        record = dict(zip(names, map(int, match.groups())), frame=frame)
        (heaps if is_heap else commands).append(record)
    return heaps, commands


def memory_stats(text, first, last):
    heaps, commands = memory_samples(text)
    heaps = [s for s in heaps if first <= s['frame'] <= last]
    commands = [s for s in commands if first <= s['frame'] <= last]
    out = []
    for heap in sorted({s['heap'] for s in heaps}):
        samples = [s for s in heaps if s['heap'] == heap]
        a, b = samples[0], samples[-1]
        out.append(f"  heap {heap}: {len(samples)} samples, frames {a['frame']}-{b['frame']} (rounded MiB)")
        for name, label in (('reserved_mib', 'reserved'), ('allocated_mib', 'allocated'), ('textures', 'live textures')):
            out.append(f"    {label}: {a[name]} -> {b[name]}, peak {max(s[name] for s in samples)}")
    if commands:
        for name in ('active', 'retired'):
            out.append(f"  command lists {name}: {commands[0][name]} -> {commands[-1][name]}, peak {max(s[name] for s in commands)}")
    return out


def percentile(values, p):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(round(p / 100 * (len(ordered) - 1))))]


def stats(session, first, last, slowest):
    log = Path(session) / 'game.log' if Path(session).is_dir() else Path(session)
    text = log.read_text(encoding='utf-8', errors='replace')
    shots = {int(n) for n in SHOT.findall(text)} | {int(n) for n in INCIDENT_CAPTURE.findall(text)}
    near = {f + d for f in shots for d in range(-3, 4)}
    rows = [(int(m[0]), *map(float, m[1:])) for m in FRAME.findall(text)]
    rows = [r for r in rows if r[1] > 0 and all(math.isfinite(v) for v in r[1:])]
    rejected = len(re.findall(r'^\[frame\]', text, re.M)) - len(rows)
    preview_ranges=[]
    preview_start=None
    for state, value in PREVIEW_CAPTURE.findall(text):
        frame=int(value)
        if state=='start':
            preview_start=frame
        elif preview_start is not None:
            if frame<preview_start:
                raise ValueError('Intervalo de captura preview inválido')
            preview_ranges.append((preview_start-3,frame+3))
            preview_start=None
    if preview_start is not None and rows:
        preview_ranges.append((preview_start-3,rows[-1][0]+3))
    rows = [r for r in rows if first <= r[0] <= last and r[0] not in near
            and not any(a<=r[0]<=b for a,b in preview_ranges)]
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
    out.extend(memory_stats(text, first, last))
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
