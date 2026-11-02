"""The frame-rate census report (docs/STATUS.md, "Frame rates above 60").

    python scripts/fps_census.py <census at 60> <census above 60> [build/gen_eh/fps_sites.txt] [--all]

A census file is what a census build (WTBUILD_FPS_CENSUS=1 at lift time) writes with WT_FPS_CENSUS=<file>; take the same
scene at frame_rate = 60 and above (scripts/perf_bench.py). Per site, what one visit adds to its field (the mean of its
size) at 60 against above 60: a site that takes the frame's time adds less per visit when the frames are shorter (half
at 120); a frame-locked one adds as much, so above 60 it runs faster than the game's time. Listed: the frame-locked
sites (ratio 0.8 .. 1.25) that are visited about once per clock update per field (consec: most repeat visits come in
the update after the field's last one; a loop's visits come in the same one), the busiest first:
    site function kind field | visits@hi fields consec% offthread% visits-per-update(hi/60) | mean |add| @60 @hi ratio |
    least, largest add @hi
'own clock': frame-locked, but visited as often per second at both rates (a thread with its own clock): not listed.
Then a line per class: frame-locked, own clock, scaled with the frame time, other. --all lists every site seen in both runs.
"""
import sys


def read(path):
    lines = open(path).read().split('\n')
    out = {}
    for l in lines[1:]:
        a = l.split()
        if len(a) < 11:
            continue
        out[a[0]] = dict(visits=int(a[1]), fields=int(a[2]), consec=int(a[3]), same=int(a[4]), later=int(a[5]),
                         off=int(a[6]), upds=int(a[7]), mean=float(a[8]), lo=float(a[9]), hi=float(a[10]))
    return int(lines[0].split()[1]), out


def main(argv):
    files = [a for a in argv if not a.startswith('--')]
    u60, s60 = read(files[0])
    uhi, shi = read(files[1])
    sites_path = files[2] if len(files) > 2 else 'build/gen_eh/fps_sites.txt'
    every = '--all' in argv
    info = {}
    for l in open(sites_path):
        if not l.startswith('#'):
            a = l.split(None, 5)
            info[a[0]] = (a[1], a[4], a[5].strip())
    rows, classes = [], {'frame-locked': 0, 'own clock': 0, 'scaled': 0, 'other': 0}
    for site, h in shi.items():
        b = s60.get(site)
        if not b or b['mean'] == 0 or h['mean'] == 0:
            continue
        ratio = b['mean'] / h['mean']
        rep = h['consec'] + h['same'] + h['later']
        cq = h['consec'] / max(rep, 1)
        # visits per clock update, above / at 60: about 1 for code that runs every frame, about 60 / rate for code on a
        # thread of its own clock (the guest's 60 Hz vsync, audio), which the frame rate does not speed up
        vpu = (h['visits'] / max(uhi, 1)) / max(b['visits'] / max(u60, 1), 1e-12)
        cls = 'frame-locked' if 0.8 <= ratio <= 1.25 else 'scaled' if ratio >= 1.5 else 'other'
        if cls == 'frame-locked' and vpu < 0.75:
            cls = 'own clock'
        if rep >= 30 and cq >= 0.5:
            classes[cls] += 1
        if every or (cls == 'frame-locked' and rep >= 30 and cq >= 0.5):
            rows.append((h['visits'], site, h, b, ratio, cq, vpu))
    rows.sort(key=lambda r: -r[0])
    print('# clock updates: %d at 60, %d above; sites seen in both: %d' % (u60, uhi, sum(1 for s in shi if s in s60)))
    for visits, site, h, b, ratio, cq, vpu in rows:
        fn, kind, field = info.get(site, ('?', '?', '?'))
        print('%s %s %-9s %-14s | %8d %5d %5.1f %5.1f %4.2f | %10.4g %10.4g %5.2f | %.4g, %.4g' % (
            site, fn, kind, field, visits, h['fields'], 100.0 * cq, 100.0 * h['off'] / max(visits, 1), vpu,
            b['mean'], h['mean'], ratio, h['lo'], h['hi']))
    print('# per-update sites (30+ repeat visits, half or more in consecutive updates): %s' %
          ', '.join('%s %d' % kv for kv in classes.items()))


if __name__ == '__main__':
    main(sys.argv[1:])
