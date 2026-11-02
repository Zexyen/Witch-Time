"""Report of an WT_LPROF sample file (runtime/lprof.c, Linux): per thread, the functions most often innermost ("self")
and anywhere in the sampled chain ("incl", once per sample).

    python scripts/lprof_report.py <lprof.txt> [threads to show (default 6)] [functions per list (default 25)]
"""
import bisect
import collections
import subprocess
import sys

path = sys.argv[1]
nthr = int(sys.argv[2]) if len(sys.argv) > 2 else 6
nfn = int(sys.argv[3]) if len(sys.argv) > 3 else 25
samples, maps, frames = [], [], []
for line in open(path, errors='replace'):
    if line.startswith('S '):
        p = line.split()
        frame = int(p.pop()[1:]) if p[-1].startswith('@') else -1       # the presented frame of the sample (newer files)
        k = len(p)
        while k > 3 and all(ch in '0123456789abcdef' for ch in p[k - 1]):
            k -= 1
        samples.append((int(p[1]), '_'.join(p[2:k]), [int(x, 16) for x in p[k:]]))      # older files: names with spaces
        frames.append(frame)
    elif line.startswith('M '):
        p = line[2:].split()
        if len(p) >= 6 and p[5].startswith('/'):
            lo, hi = (int(x, 16) for x in p[0].split('-'))
            maps.append((lo, hi, int(p[2], 16), p[5]))
bias = {}
for lo, hi, off, f in maps:
    if off == 0 and f not in bias:
        bias[f] = lo
syms = {}


def table(f):
    if f not in syms:
        rows = []
        for opt in (['-n', '--defined-only'], ['-n', '-D', '--defined-only']):
            try:
                out = subprocess.run(['nm'] + opt + [f], capture_output=True, text=True).stdout
            except OSError:
                out = ''
            for l in out.splitlines():
                p = l.split()
                if len(p) >= 3 and p[1] in 'tTwWiI':
                    rows.append((int(p[0], 16), p[2]))
            if rows:
                break
        rows.sort()
        syms[f] = ([a for a, _ in rows], [n for _, n in rows])
    return syms[f]


cache = {}


def name(pc):
    if pc in cache:
        return cache[pc]
    r = '?%x' % pc
    for lo, hi, off, f in maps:
        if lo <= pc < hi:
            short = f.rsplit('/', 1)[-1]
            addrs, names = table(f)
            v = pc - bias.get(f, lo)
            i = bisect.bisect_right(addrs, v) - 1
            r = '%s:%s' % (short, names[i]) if i >= 0 else '%s+%x' % (short, v)
            break
    cache[pc] = r
    return r


IDLE = ('wait', 'syscall', 'sleep', 'poll', 'sched_yield', 'pause', 'sigtimedwait', 'recv', 'accept', 'select', 'ioctl')


def idle(pcs):          # the innermost frame waits in libc / the kernel (futex, poll, sleep, a driver ioctl)
    n = name(pcs[0]) if pcs else ''
    return n.startswith('libc.so') and any(w in n for w in IDLE)


per = collections.defaultdict(list)
for tid, nm, pcs in samples:
    per[(tid, nm)].append(pcs)
total = len(samples)
print('%d samples, %d threads; busy = innermost frame not waiting in libc' % (total, len(per)))
busy = {k: [p for p in v if not idle(p)] for k, v in per.items()}
for (tid, nm), ss in sorted(busy.items(), key=lambda kv: -len(kv[1]))[:nthr]:
    print('  thread %d %-28s busy %5.1f%% of %d samples' % (tid, nm, 100.0 * len(ss) / max(1, len(per[(tid, nm)])), len(per[(tid, nm)])))
for (tid, nm), ss in sorted(busy.items(), key=lambda kv: -len(kv[1]))[:nthr]:
    if not ss:
        continue
    self_c, incl_c = collections.Counter(), collections.Counter()
    for pcs in ss:
        names = [name(p) for p in pcs]
        if names:
            self_c[names[0]] += 1
        for n in set(names):
            incl_c[n] += 1
    print('\n== thread %d %s: %d busy samples (percentages of those)' % (tid, nm, len(ss)))
    print('  self:')
    for n, c in self_c.most_common(nfn):
        print('    %5.1f%%  %s' % (100.0 * c / len(ss), n))
    print('  incl:')
    for n, c in incl_c.most_common(nfn):
        print('    %5.1f%%  %s' % (100.0 * c / len(ss), n))
