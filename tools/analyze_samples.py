"""Summarize in-process engine-thread samples from benchmark_app.py --sample.

Reports self and inclusive time by function for all frames and for frames
whose CPU time between presentations exceeded a threshold.
"""
import argparse
import bisect
import collections
import json
from pathlib import Path
import subprocess


def symbols(path, cache={}):
    if path not in cache:
        names = []
        try:
            out = subprocess.run(['nm', '-n', '--defined-only', path], capture_output=True, text=True).stdout
        except OSError:
            out = ''
        for line in out.splitlines():
            parts = line.split(maxsplit=2)
            if len(parts) == 3 and parts[1] in 'tT':
                names.append((int(parts[0], 16), parts[2]))
        cache[path] = ([a for a, _ in names], [n for _, n in names])
    return cache[path]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('report', type=Path, help='Benchmark report base path, e.g. work/reports/name')
    ap.add_argument('--slow-ms', type=float, default=8.0, help='Frame CPU threshold for the slow-frame profile')
    ap.add_argument('--top', type=int, default=35)
    ap.add_argument('--module', action='store_true', help='Group by image instead of function')
    ap.add_argument('--callers', help='Show call paths for samples whose leaf function contains this text')
    ap.add_argument('--path-depth', type=int, default=4)
    args = ap.parse_args()
    base = args.report.with_suffix('')
    data = json.loads(base.with_suffix('.samples.json').read_text())
    rows = json.loads(base.with_suffix('.present.json').read_text())['frames']
    work = {i + 1: (b[0] - a[1]) * 1000 for i, (a, b) in enumerate(zip(rows, rows[1:]))}
    images = sorted((addr, name) for name, addr in data['images'])
    starts = [a for a, _ in images]
    resolved = {}

    def name(pc):
        if pc not in resolved:
            i = bisect.bisect_right(starts, pc) - 1
            if i < 0:
                resolved[pc] = '?'
            else:
                base_addr, path = images[i]
                module = Path(path).name
                if args.module:
                    resolved[pc] = module
                else:
                    addrs, names = symbols(path)
                    j = bisect.bisect_right(addrs, pc - base_addr) - 1
                    resolved[pc] = f'{module}:{names[j] if j >= 0 else hex(pc - base_addr)}'
        return resolved[pc]

    def profile(selected, title):
        total = len(selected)
        own, inclusive = collections.Counter(), collections.Counter()
        for sample in selected:
            # pc[0] is the interrupted instruction, pc[1] the link register
            # (often a duplicate of the first return address); skip it.
            stack = [sample[1]] + sample[3:]
            names = [name(pc) for pc in stack]
            own[names[0]] += 1
            for n in set(names):
                inclusive[n] += 1
        print(f'\n== {title}: {total} samples')
        if not total:
            return
        print('-- self')
        for n, c in own.most_common(args.top):
            print(f'{100 * c / total:6.2f}%  {n}')
        print('-- inclusive')
        for n, c in inclusive.most_common(args.top):
            print(f'{100 * c / total:6.2f}%  {n}')

    samples = data['samples']
    if args.callers:
        paths = collections.Counter()
        chosen = [s for s in samples if args.callers in name(s[1])]
        for sample in chosen:
            stack = [name(pc) for pc in [sample[1]] + sample[3:]]
            paths[' <- '.join(stack[1:1 + args.path_depth])] += 1
        print(f'{len(chosen)} samples ({100 * len(chosen) / len(samples):.2f}%) in {args.callers}')
        for path, c in paths.most_common(args.top):
            print(f'{100 * c / len(chosen):6.2f}%  {path}')
        return
    slow = {f for f, w in work.items() if w > args.slow_ms}
    print(f'frames {len(rows)}, slow (> {args.slow_ms} ms) {len(slow)}')
    profile(samples, 'all frames')
    profile([s for s in samples if s[0] in slow], f'slow frames (> {args.slow_ms} ms)')


if __name__ == '__main__':
    main()
