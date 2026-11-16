"""Compare guest function call counts of two LARECOMP_FNPROF runs (read only).

    python scripts/fnprof-diff.py base.txt changed.txt [--generated local/builds/<id>/generated/main] [--top 60]

Lists the functions that run only in the second profile, then those called clearly more often there, each with the
generated unit that holds it: the logic an input (a cursor move, a page change) sets off. Profiles come from runs over
the same frame window (LARECOMP_FNPROF=from:to, LARECOMP_FNPROF_OUT=<file>).
"""
import argparse
from pathlib import Path
import re


def load(path):
    counts = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if line.strip():
            address, count = line.split()
            counts[int(address, 16)] = int(count)
    return counts


def units(generated):
    """function address -> generated unit name (main_123.c)."""
    found = {}
    if not generated:
        return found
    pattern = re.compile(r'^void f_([0-9a-f]+)\(Cpu \*c\) \{', re.M)
    for unit in sorted(Path(generated).glob('*.c')):
        for match in pattern.finditer(unit.read_text(encoding='utf-8', errors='replace')):
            found[int(match.group(1), 16)] = unit.name
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('base')
    parser.add_argument('changed')
    parser.add_argument('--generated', type=Path)
    parser.add_argument('--top', type=int, default=60)
    parser.add_argument('--ratio', type=float, default=1.5, help='Minimum growth to list a function called in both')
    args = parser.parse_args()
    base, changed = load(args.base), load(args.changed)
    where = units(args.generated)
    only = sorted(((a, n) for a, n in changed.items() if a not in base), key=lambda r: -r[1])
    grown = sorted(((a, base[a], n) for a, n in changed.items() if a in base and n >= base[a] * args.ratio and n - base[a] >= 4),
                   key=lambda r: -(r[2] - r[1]))
    print(f'{len(base)} functions in {args.base}, {len(changed)} in {args.changed}')
    print(f'\nonly in {args.changed}: {len(only)}')
    for a, n in only[:args.top]:
        print(f'  f_{a:x} {n:8d} {where.get(a, "")}')
    print(f'\ncalled at least {args.ratio}x more: {len(grown)}')
    for a, before, after in grown[:args.top]:
        print(f'  f_{a:x} {before:8d} -> {after:8d} {where.get(a, "")}')


if __name__ == '__main__':
    main()
