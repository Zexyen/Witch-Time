"""Repeatable performance runs and their report (docs/STATUS.md, "Performance report").

    python scripts/perf_bench.py run <scene> <scale> [--tag T] [--save DIR] [--env K=V ...]
    python scripts/perf_bench.py report <run dir> ...            (a markdown table per run, then a summary)

Scenes (a COPY of the save is played; audio muted; graphics.ini = the committed defaults, written to the run folder):
    shibuya    Continue -> Tokyo: Shibuya street fight; the fight pattern from 40 s; 330 s; frames 1500 .. end
    tokyo      the earlier Tokyo save (--save, required); the fight pattern from 560 s; 700 s; frames 19800 .. end
    cutscene   Shibuya, no input: the cutscene that starts after about 75 s on the street, at cutscene_fps = 60;
               300 s; the frames between the game's two "fps mode" changes around it
Scales: 1.0 (1600x900), 1.6 (2560x1440), 2.4 (3840x2160); the window is the output size (--desktop-scale, the desktop's
scaling, 1.25 here).

A run takes /tmp/b3_bench.lock (another session sharing the PC starts no game run while it is there) and waits while
the lock of someone else is there or any "Witch Time" process runs (it never stops one). It records the load average
and the other processes that have the GPU open, before and after. Results: build/perf/<scene>_<scale>[_<tag>]/:
log.txt, frametimes.txt (WT_FRAMETIMES), gputimes.txt (WT_VK_GPUTIME + WT_GPUTIMES), meta.json.
"""
import json, os, re, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(ROOT, 'build', 'run')
LOCK = '/tmp/b3_bench.lock'
FIGHT = ('0.00+0.15:V 0.35+0.15:V 0.70+0.15:V 1.05+0.15:C 1.40+0.15:C 1.90+0.15:S 2.30+0.80:r 3.20+0.15:X 3.50+0.15:V '
         '4.00+0.60:l 4.70+0.15:C 5.00+0.40:VS').split()        # one 5.6 s cycle of the stage-5 fight pattern
SCENES = {'shibuya': dict(secs=330, fight_from=40, first=1500),
          'tokyo': dict(secs=700, fight_from=560, first=19800),
          'cutscene': dict(secs=300, fight_from=None, first=None, env={'WT_CUTSCENE_FPS': '60'})}


DESKTOP = {'kwin_wayland', 'Xwayland', 'plasmashell', 'WebKitWebProces', 'monocode', 'xdg-desktop-por', 'kded6', 'krunner',
           'kwin_wayland_wr', 'ksmserver', 'polkit-kde-auth', 'xembedsniproxy', 'kaccess', 'org_kde_powerde', 'Witch Time', 'witch-time-game'}   # the desktop's own


def fight_script(start, end):
    out, t0 = [], float(start)
    while t0 < end:
        for st in FIGHT:
            m = re.match(r'([\d.]+)(\+[\d.]+)?:(\S+)', st)
            out.append('%.2f%s:%s' % (t0 + float(m.group(1)), m.group(2) or '', m.group(3)))
        t0 += 5.6
    return ' '.join(out)


def game_pids():
    r = subprocess.run(['pgrep', '-x', 'Witch Time'], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split()]


def gpu_users():
    users = []
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            fds = os.listdir('/proc/%s/fd' % pid)
            if any(os.readlink('/proc/%s/fd/%s' % (pid, fd)).startswith('/dev/dri/') for fd in fds):
                users.append('%s %s' % (pid, open('/proc/%s/comm' % pid).read().strip()))
        except OSError:
            pass
    return users


def gpu_busy(seconds=2.0):
    """the GPU's own busy percentage (amdgpu sysfs), mean of 10 samples; None when there is none"""
    import glob
    paths = glob.glob('/sys/class/drm/card*/device/gpu_busy_percent')
    if not paths: return None
    v = []
    for _ in range(10):
        v.append(max(int(open(p).read()) for p in paths)); time.sleep(seconds / 10)
    return sum(v) / len(v)


def cmd_run(argv):
    scene, scale = argv[0], argv[1]
    tag, save, extra, dscale, secs_override = None, os.path.join(RUN, 'userdata'), {}, 1.25, None
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == '--tag': tag = argv[i + 1]; i += 2
        elif a == '--save': save = argv[i + 1]; i += 2
        elif a == '--desktop-scale': dscale = float(argv[i + 1]); i += 2
        elif a == '--secs': secs_override = int(argv[i + 1]); i += 2
        elif a == '--env': k, v = argv[i + 1].split('=', 1); extra[k] = v; i += 2
        else: sys.exit('unknown argument ' + a)
    sc = dict(SCENES[scene])
    if secs_override: sc['secs'] = secs_override
    name = '%s_%s%s' % (scene, scale, '_' + tag if tag else '')
    out = os.path.join(ROOT, 'build', 'perf', name)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    ini = os.path.join(out, 'graphics.ini')
    with open(ini, 'w') as f:
        f.write(subprocess.run(['git', 'show', 'HEAD:config/graphics.ini'], cwd=ROOT, capture_output=True, text=True).stdout)
    ud = '/tmp/b3_perf_userdata_' + name
    shutil.rmtree(ud, ignore_errors=True); shutil.copytree(save, ud)
    w, h = round(1600 * float(scale) / dscale), round(900 * float(scale) / dscale)
    env = dict(os.environ, WT_AUDIO_MUTE='1', WT_PRESS='Z,25,3', WT_SHOW='0', WT_FOCUS_NOTIFY='0',
               WT_GRAPHICS_CONFIG=ini, WT_VK_SCALE=scale, WT_WINDOW='%dx%d' % (w, h),
               WT_FRAMETIMES=os.path.join(out, 'frametimes.txt'), WT_VK_GPUTIME='1', WT_GPUTIMES=os.path.join(out, 'gputimes.txt'),
               WT_ROMFS=os.path.join(ROOT, 'targets', 'romfs'), WT_USERDATA=ud)
    env.update(sc.get('env', {}))
    if sc['fight_from'] is not None:
        env['WT_SCRIPT'] = fight_script(sc['fight_from'], sc['secs'])
    env.update(extra)
    while True:                                     # wait for the PC: someone else's lock, or any game running
        if not os.path.exists(LOCK) and not game_pids():
            break
        print('perf_bench: waiting (%s)' % ('lock: ' + open(LOCK).read().strip() if os.path.exists(LOCK) else 'a Witch Time process runs'), flush=True)
        time.sleep(15)
    with open(LOCK, 'w') as f:
        f.write('witchtime perf_bench.py run %s %s (pid %d): benchmark, please start no game run until this file is gone\n' % (scene, scale, os.getpid()))
    meta = dict(scene=scene, scale=scale, tag=tag, extra=extra, start=time.strftime('%Y-%m-%d %H:%M:%S'),
                load_before=open('/proc/loadavg').read().split()[:3], gpu_users_before=gpu_users(), gpu_busy_before=gpu_busy())
    try:
        with open(os.path.join(out, 'log.txt'), 'w') as log:
            subprocess.run(['timeout', str(sc['secs'] + 90), '../witch-time-game', '../../targets/exefs/main.elf', str(sc['secs'])], cwd=RUN, env=env, stdout=log, stderr=subprocess.STDOUT)
        while game_pids():                          # our process may still be closing
            time.sleep(1)
    finally:
        meta.update(end=time.strftime('%Y-%m-%d %H:%M:%S'), load_after=open('/proc/loadavg').read().split()[:3], gpu_users_after=gpu_users(), gpu_busy_after=gpu_busy())
        os.remove(LOCK)
    json.dump(meta, open(os.path.join(out, 'meta.json'), 'w'), indent=1)
    shutil.rmtree(ud, ignore_errors=True)
    print('perf_bench: %s done; %s' % (name, out))


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q / 100.0 * len(v)))] if v else 0.0


def analyse(d):
    meta = json.load(open(os.path.join(d, 'meta.json')))
    sc = SCENES[meta['scene']]
    log = open(os.path.join(d, 'log.txt'), errors='replace').read()
    ft = [l.split() for l in open(os.path.join(d, 'frametimes.txt'))]
    ft = [(int(a[0]), float(a[1]), int(a[2]), float(a[3])) for a in ft if len(a) >= 4]
    if sc['first'] is not None:
        lo, hi = sc['first'], ft[-1][0]
    else:                                           # the cutscene: between the last change to mode 1 and the change back after it
        marks = [(int(m.group(1)), float(m.group(2))) for m in re.finditer(r'fps mode: the game asks for mode (\d) .*? at ([\d.]+) s', log)]
        t0 = t1 = None
        for k in range(len(marks) - 1, 0, -1):
            if marks[k - 1][0] == 1 and marks[k][0] == 0 and marks[k][1] - marks[k - 1][1] > 30:
                t0, t1 = marks[k - 1][1], marks[k][1]; break
        sel = [f for f in ft if t0 is not None and t0 <= f[3] <= t1]
        lo, hi = (sel[0][0], sel[-1][0]) if sel else (0, -1)
    win = [f for f in ft if lo <= f[0] <= hi]
    ms = [f[1] for f in win]
    gpu = {}
    gp = os.path.join(d, 'gputimes.txt')
    if os.path.exists(gp):
        for l in open(gp):
            a = l.split()
            if len(a) == 2: gpu[int(a[0])] = float(a[1])
    g = [gpu[f[0]] for f in win if f[0] in gpu]
    cpu, ncpu = {}, 0                               # "cpu ms/frame by thread" lines of the windows inside the range
    frame = None
    for line in log.splitlines():
        m = re.match(r'nvn_perf: frames (\d+)\.\.(\d+) frame time', line)
        if m: frame = int(m.group(2)); continue
        if line.startswith('nvn_perf: cpu ms/frame by thread') and frame is not None and lo + 300 <= frame <= hi:
            ncpu += 1
            seen = {}
            for name, v in re.findall(r' (\S+?)\[\d+\] ([\d.]+)', line):
                seen[name] = seen.get(name, 0) + float(v)
            for k, v in seen.items(): cpu[k] = cpu.get(k, 0) + v
    cpu = {k: v / ncpu for k, v in cpu.items()} if ncpu else {}
    sec = sum(ms) / 1000.0
    r = dict(name=os.path.basename(d), scene=meta['scene'], scale=meta['scale'], frames=len(ms), lo=lo, hi=hi,
             fps=len(ms) / sec if sec else 0, low1=1000.0 / pct(ms, 99) if ms else 0, p50=pct(ms, 50), p90=pct(ms, 90), p99=pct(ms, 99),
             at60=100.0 * sum(1 for x in ms if x < 18.5) / len(ms) if ms else 0, over50=sum(1 for x in ms if x > 50), over100=sum(1 for x in ms if x > 100),
             gpu50=pct(g, 50), gpu90=pct(g, 90), gpu99=pct(g, 99), cpu=cpu, load=meta.get('load_before', [None])[0], load_after=meta.get('load_after', [None])[0],
             gpu_users=[u for u in meta.get('gpu_users_before', []) if u.split(' ', 1)[-1] not in DESKTOP])
    late = [f[0] for f in win if f[1] >= 18.5]      # the frames that missed 60: was their GPU time near the budget?
    lg = [gpu[f] for f in late if f in gpu]
    r['late_gpu'] = 100.0 * sum(1 for x in lg if x > 14.0) / len(lg) if lg else 0.0
    r['gpu_idle'] = meta.get('gpu_busy_before')
    top = max(cpu, key=cpu.get) if cpu else '?'
    r['bound'] = '-' if r['at60'] >= 99 else 'GPU (%.0f %% of late frames)' % r['late_gpu'] if r['late_gpu'] >= 50 else 'CPU side (GPU in %.0f %% of late frames; top thread %s %.1f ms)' % (r['late_gpu'], top, cpu.get(top, 0))
    return r


def cmd_report(dirs):
    rows = [analyse(d) for d in dirs]
    print('| run | frames | avg fps | 1%-low | p50 / p90 / p99 ms | over 50 / 100 ms | at 60 | GPU ms p50 / p90 / p99 | CPU ms/frame: main, render, MainJob1, MainDraw | load, GPU busy before | bound |')
    print('|---|---|---|---|---|---|---|---|---|---|---|')
    for r in rows:
        c = r['cpu']
        print('| %s | %d | %.1f | %.1f | %.1f / %.1f / %.1f | %d / %d | %.1f %% | %.1f / %.1f / %.1f | %.1f, %.1f, %.1f, %.1f | %s | %s |' % (
            r['name'], r['frames'], r['fps'], r['low1'], r['p50'], r['p90'], r['p99'], r['over50'], r['over100'], r['at60'], r['gpu50'], r['gpu90'], r['gpu99'],
            c.get('main', 0), c.get('render(host)', 0), c.get('MainJob1', 0), c.get('MainDraw', 0), '%s-%s, %s' % (r['load'], r['load_after'], '-' if r['gpu_idle'] is None else '%.0f %%' % r['gpu_idle']), r['bound']))
    for r in rows:
        if r['gpu_users']: print('%s: other GPU users at the start: %s' % (r['name'], ', '.join(r['gpu_users'])))


if __name__ == '__main__':
    if len(sys.argv) < 3 or sys.argv[1] not in ('run', 'report'):
        sys.exit(__doc__)
    (cmd_run if sys.argv[1] == 'run' else cmd_report)(sys.argv[2:])
