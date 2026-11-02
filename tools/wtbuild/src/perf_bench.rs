//! Repeatable performance runs of the built game and their report (docs/STATUS.md, "Performance report"), for developers
//! on Linux.
//!
//! `perf-bench run <scene> <scale> [--tag T] [--save DIR] [--desktop-scale F] [--secs N] [--env K=V]...`
//! `perf-bench report <run dir>...` (a markdown table, a row per run, then the other programs that had the GPU open)
//!
//! Scenes (a copy of the save is played, audio muted, graphics.ini the committed defaults, written to the run folder):
//! - shibuya: Continue -> Tokyo: Shibuya street fight; the fight pattern from 40 s; 330 s; frames 1500 .. end
//! - tokyo: the earlier Tokyo save (--save, required); the fight pattern from 560 s; 700 s; frames 19800 .. end
//! - cutscene: Shibuya, no input: the cutscene that starts after about 75 s on the street, at cutscene_fps = 60; 300 s;
//!   the frames between the game's two "fps mode" changes around it
//!
//! Scales: 1.0 (1600x900), 1.6 (2560x1440), 2.4 (3840x2160); the window is the output size over --desktop-scale, the
//! desktop's scaling (default 1.25). --secs replaces the scene's length.
//!
//! A run takes /tmp/b3_bench.lock (another session sharing the PC starts no game run while it is there) and waits while
//! someone else's lock is there or any "Witch Time" process runs (it never stops one). It records the load average, the
//! GPU's busy % and the other processes that have the GPU open, before and after. Results in
//! build/perf/<scene>_<scale>[_<tag>]/: log.txt, frametimes.txt (SSBU_FRAMETIMES), gputimes.txt (SSBU_VK_GPUTIME +
//! SSBU_GPUTIMES), meta.json.

use crate::die;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::thread::sleep;
use std::time::Duration;

const LOCK: &str = "/tmp/b3_bench.lock";
/// one 5.6 s cycle of the stage-5 fight pattern
const FIGHT: &str = "0.00+0.15:V 0.35+0.15:V 0.70+0.15:V 1.05+0.15:C 1.40+0.15:C 1.90+0.15:S 2.30+0.80:r 3.20+0.15:X 3.50+0.15:V \
                     4.00+0.60:l 4.70+0.15:C 5.00+0.40:VS";
/// the desktop's own processes with the GPU open: left out of the report
const DESKTOP: [&str; 16] = [
    "kwin_wayland",
    "Xwayland",
    "plasmashell",
    "WebKitWebProces",
    "monocode",
    "xdg-desktop-por",
    "kded6",
    "krunner",
    "kwin_wayland_wr",
    "ksmserver",
    "polkit-kde-auth",
    "xembedsniproxy",
    "kaccess",
    "org_kde_powerde",
    "Witch Time",
    "witch-time-game",
];

struct Scene {
    name: &'static str,
    secs: i64,
    fight_from: Option<i64>,
    /// the report's first frame; None: the frames of the cutscene
    first: Option<i64>,
    env: &'static [(&'static str, &'static str)],
}

static SCENES: [Scene; 3] = [
    Scene {
        name: "shibuya",
        secs: 330,
        fight_from: Some(40),
        first: Some(1500),
        env: &[],
    },
    Scene {
        name: "tokyo",
        secs: 700,
        fight_from: Some(560),
        first: Some(19800),
        env: &[],
    },
    Scene {
        name: "cutscene",
        secs: 300,
        fight_from: None,
        first: None,
        env: &[("SSBU_CUTSCENE_FPS", "60")],
    },
];

fn scene(name: &str) -> Result<&'static Scene, String> {
    SCENES
        .iter()
        .find(|s| s.name == name)
        .ok_or_else(|| format!("unknown scene {} (shibuya, tokyo, cutscene)", name))
}

pub fn main(args: &[String]) {
    if !cfg!(target_os = "linux") {
        die("perf-bench is for Linux: it reads /proc and runs the Linux game");
    }
    match args.first().map(String::as_str) {
        Some("run") if args.len() >= 3 => run(&args[1..]),
        Some("report") if args.len() >= 2 => print!("{}", report(&args[1..]).unwrap_or_else(|e| die(e))),
        _ => die("usage: perf-bench run <scene> <scale> [--tag T] [--save DIR] [--desktop-scale F] [--secs N] [--env K=V]... | perf-bench report <run dir>..."),
    }
}

fn num<T: std::str::FromStr>(s: &str) -> Result<T, String> {
    s.trim().parse().map_err(|_| format!("not a number: {}", s))
}

/// sets k in a list kept in the order Python's dict keeps it: a key set again keeps its place
fn put(v: &mut Vec<(String, String)>, k: &str, x: &str) {
    match v.iter_mut().find(|e| e.0 == k) {
        Some(e) => e.1 = x.to_string(),
        None => v.push((k.to_string(), x.to_string())),
    }
}

// ---------------------------------------------------------------------------------------------- run

struct Run {
    scene: &'static Scene,
    scale: String,
    tag: Option<String>,
    save: PathBuf,
    size: (i64, i64),
    secs: i64,
    extra: Vec<(String, String)>,
    repo: PathBuf,
}

/// `<scene> <scale> [options]`
fn parse(args: &[String]) -> Result<Run, String> {
    let repo = std::path::absolute(crate::repo_root(args)).map_err(|e| e.to_string())?;
    let (mut tag, mut save, mut dscale, mut secs, mut extra) = (None, repo.join("build").join("run").join("userdata"), 1.25, 0i64, Vec::new());
    let mut i = 2;
    while i < args.len() {
        let a = args[i].as_str();
        if !["--tag", "--save", "--desktop-scale", "--secs", "--env", "--repo"].contains(&a) {
            return Err(format!("unknown argument {}", a));
        }
        let v = args.get(i + 1).ok_or_else(|| format!("{} needs a value", a))?;
        match a {
            "--tag" => tag = Some(v.clone()),
            "--save" => save = PathBuf::from(v),
            "--desktop-scale" => dscale = num(v)?,
            "--secs" => secs = num(v)?,
            "--env" => {
                let (k, x) = v.split_once('=').ok_or_else(|| format!("--env {}: K=V", v))?;
                put(&mut extra, k, x);
            }
            _ => {}
        }
        i += 2;
    }
    let sc = scene(&args[0])?;
    Ok(Run {
        scene: sc,
        scale: args[1].clone(),
        tag,
        save,
        size: window(num(&args[1])?, dscale),
        secs: if secs != 0 { secs } else { sc.secs },
        extra,
        repo,
    })
}

/// SSBU_WINDOW: the output size in the desktop's scaled pixels; Python's round() takes a half to the even side
fn window(scale: f64, dscale: f64) -> (i64, i64) {
    (
        (1600.0 * scale / dscale).round_ties_even() as i64,
        (900.0 * scale / dscale).round_ties_even() as i64,
    )
}

/// SSBU_SCRIPT: the fight pattern from start to end s
fn fight_script(start: i64, end: i64) -> String {
    let mut out = Vec::new();
    let mut t0 = start as f64;
    while t0 < end as f64 {
        for st in FIGHT.split(' ') {
            let (at, key) = st.split_once(':').unwrap();
            let (t, hold) = at.split_at(at.find('+').unwrap_or(at.len()));
            out.push(format!("{:.2}{}:{}", t0 + t.parse::<f64>().unwrap(), hold, key));
        }
        t0 += 5.6;
    }
    out.join(" ")
}

impl Run {
    fn name(&self) -> String {
        let tag = self.tag.as_deref().filter(|t| !t.is_empty());
        format!("{}_{}{}", self.scene.name, self.scale, tag.map(|t| format!("_{}", t)).unwrap_or_default())
    }
    fn out(&self) -> PathBuf {
        self.repo.join("build").join("perf").join(self.name())
    }
    fn userdata(&self) -> PathBuf {
        PathBuf::from(format!("/tmp/b3_perf_userdata_{}", self.name()))
    }
    /// what the game gets on top of this process's environment
    fn env(&self) -> Vec<(String, String)> {
        let out = self.out();
        let p = |f: &str| out.join(f).to_string_lossy().into_owned();
        let mut env = Vec::new();
        for (k, v) in [
            ("SSBU_AUDIO_MUTE", "1".to_string()),
            ("SSBU_PRESS", "Z,25,3".into()),
            ("SSBU_SHOW", "0".into()),
            ("SSBU_FOCUS_NOTIFY", "0".into()),
            ("SSBU_GRAPHICS_CONFIG", p("graphics.ini")),
            ("SSBU_VK_SCALE", self.scale.clone()),
            ("SSBU_WINDOW", format!("{}x{}", self.size.0, self.size.1)),
            ("SSBU_FRAMETIMES", p("frametimes.txt")),
            ("SSBU_VK_GPUTIME", "1".into()),
            ("SSBU_GPUTIMES", p("gputimes.txt")),
            ("SSBU_ROMFS", self.repo.join("targets").join("romfs").to_string_lossy().into_owned()),
            ("SSBU_USERDATA", self.userdata().to_string_lossy().into_owned()),
        ] {
            put(&mut env, k, &v);
        }
        for (k, v) in self.scene.env {
            put(&mut env, k, v);
        }
        if let Some(from) = self.scene.fight_from {
            put(&mut env, "SSBU_SCRIPT", &fight_script(from, self.secs));
        }
        for (k, v) in &self.extra {
            put(&mut env, k, v);
        }
        env
    }
}

fn run(args: &[String]) {
    let r = parse(args).unwrap_or_else(|e| die(e));
    let (name, out, ud) = (r.name(), r.out(), r.userdata());
    let _ = fs::remove_dir_all(&out);
    fs::create_dir_all(&out).unwrap_or_else(|e| die(format!("{}: {}", out.display(), e)));
    // the committed defaults, not the working copy
    let ini = Command::new("git")
        .args(["show", "HEAD:config/graphics.ini"])
        .current_dir(&r.repo)
        .output()
        .unwrap_or_else(|e| die(format!("git: {}", e)));
    fs::write(out.join("graphics.ini"), ini.stdout).unwrap_or_else(|e| die(e));
    let _ = fs::remove_dir_all(&ud);
    copy_tree(&r.save, &ud).unwrap_or_else(|e| die(format!("{}: {}", r.save.display(), e)));
    // wait for the PC: someone else's lock, or any game running
    while Path::new(LOCK).exists() || !game_pids().is_empty() {
        let why = if Path::new(LOCK).exists() {
            format!("lock: {}", fs::read_to_string(LOCK).unwrap_or_default().trim())
        } else {
            "a Witch Time process runs".into()
        };
        println!("perf-bench: waiting ({})", why);
        sleep(Duration::from_secs(15));
    }
    let note = format!(
        "witchtime wtbuild perf-bench run {} {} (pid {}): benchmark, please start no game run until this file is gone\n",
        r.scene.name,
        r.scale,
        std::process::id()
    );
    fs::write(LOCK, note).unwrap_or_else(|e| die(format!("{}: {}", LOCK, e)));
    sig::catch();
    let extra: serde_json::Map<String, Value> = r.extra.iter().map(|(k, v)| (k.clone(), v.as_str().into())).collect();
    let mut meta = json!({"scene": r.scene.name, "scale": r.scale, "tag": r.tag, "extra": extra, "start": now(),
                          "load_before": load(), "gpu_users_before": gpu_users(), "gpu_busy_before": gpu_busy()});
    // nothing between taking the lock and removing it may die
    let res = (|| -> Result<(), String> {
        let log = fs::File::create(out.join("log.txt")).map_err(|e| format!("log.txt: {}", e))?;
        let mut cmd = Command::new("timeout");
        cmd.arg((r.secs + 90).to_string())
            .arg("../witch-time-game")
            .arg("../../targets/exefs/main.elf")
            .arg(r.secs.to_string());
        cmd.current_dir(r.repo.join("build").join("run"))
            .envs(r.env())
            .stdout(log.try_clone().map_err(|e| e.to_string())?)
            .stderr(log);
        if !sig::stopped() {
            let mut child = cmd.spawn().map_err(|e| format!("timeout: {}", e))?;
            sig::child(child.id());
            child.wait().map_err(|e| e.to_string())?;
        }
        // our process may still be closing
        while !sig::stopped() && !game_pids().is_empty() {
            sleep(Duration::from_secs(1));
        }
        Ok(())
    })();
    meta["end"] = now().into();
    meta["load_after"] = load().into();
    meta["gpu_users_after"] = gpu_users().into();
    meta["gpu_busy_after"] = gpu_busy().into();
    let _ = fs::remove_file(LOCK);
    if let Err(e) = res {
        die(e)
    }
    if sig::stopped() {
        die(format!("stopped: {} has no meta.json", out.display()))
    }
    let mut s = String::new();
    crate::setup::dump_json(&meta, 0, &mut s);
    fs::write(out.join("meta.json"), s).unwrap_or_else(|e| die(e));
    let _ = fs::remove_dir_all(&ud);
    println!("perf-bench: {} done; {}", name, out.display());
}

/// shutil.copytree: symbolic links followed
fn copy_tree(from: &Path, to: &Path) -> std::io::Result<()> {
    fs::create_dir_all(to)?;
    for e in fs::read_dir(from)? {
        let e = e?;
        let (src, dst) = (e.path(), to.join(e.file_name()));
        if fs::metadata(&src)?.is_dir() {
            copy_tree(&src, &dst)?;
        } else {
            fs::copy(&src, &dst)?;
        }
    }
    Ok(())
}

/// the processes named "Witch Time" (runtime/boot.c's prctl): the game, also inside a launcher
fn game_pids() -> Vec<i32> {
    // its first call comes before the lock is taken, so a missing pgrep dies there
    let o = Command::new("pgrep")
        .args(["-x", "Witch Time"])
        .output()
        .unwrap_or_else(|e| die(format!("pgrep: {}", e)));
    String::from_utf8_lossy(&o.stdout).split_whitespace().filter_map(|p| p.parse().ok()).collect()
}

/// "pid comm" of every process with a file of /dev/dri open
fn gpu_users() -> Vec<String> {
    let mut users = Vec::new();
    for e in fs::read_dir("/proc").into_iter().flatten().flatten() {
        let pid = e.file_name().to_string_lossy().into_owned();
        if pid.is_empty() || !pid.bytes().all(|c| c.is_ascii_digit()) {
            continue;
        }
        // as the Python's any(): a descriptor that cannot be read before a match leaves the process out
        let uses = || -> std::io::Result<bool> {
            for fd in fs::read_dir(e.path().join("fd"))? {
                if fs::read_link(fd?.path())?.to_string_lossy().starts_with("/dev/dri/") {
                    return Ok(true);
                }
            }
            Ok(false)
        };
        if matches!(uses(), Ok(true)) {
            if let Ok(comm) = fs::read_to_string(e.path().join("comm")) {
                users.push(format!("{} {}", pid, comm.trim()));
            }
        }
    }
    users
}

/// the GPU's own busy percentage (amdgpu sysfs), the mean of 10 samples over 2 s; None when there is none
fn gpu_busy() -> Option<f64> {
    let paths: Vec<PathBuf> = fs::read_dir("/sys/class/drm")
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().starts_with("card"))
        .map(|e| e.path().join("device").join("gpu_busy_percent"))
        .filter(|p| p.exists())
        .collect();
    if paths.is_empty() {
        return None;
    }
    let mut sum = 0i64;
    for _ in 0..10 {
        sum += paths
            .iter()
            .map(|p| fs::read_to_string(p).ok().and_then(|s| s.trim().parse().ok()).unwrap_or(0))
            .max()
            .unwrap();
        sleep(Duration::from_millis(200));
    }
    Some(sum as f64 / 10.0)
}

fn load() -> Vec<String> {
    fs::read_to_string("/proc/loadavg")
        .unwrap_or_default()
        .split_whitespace()
        .take(3)
        .map(String::from)
        .collect()
}

/// local time as time.strftime('%Y-%m-%d %H:%M:%S'): std has no time zones
fn now() -> String {
    Command::new("date")
        .arg("+%Y-%m-%d %H:%M:%S")
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .unwrap_or_default()
}

/// Ctrl-C or a TERM during a run stops the game through `timeout` and still removes the lock, as the Python's finally did
#[cfg(target_os = "linux")]
mod sig {
    use std::sync::atomic::{AtomicBool, AtomicI32, Ordering::SeqCst};
    static CHILD: AtomicI32 = AtomicI32::new(0);
    static STOP: AtomicBool = AtomicBool::new(false);
    extern "C" {
        fn signal(sig: i32, handler: extern "C" fn(i32)) -> usize;
        fn kill(pid: i32, sig: i32) -> i32;
    }
    extern "C" fn on_signal(_: i32) {
        STOP.store(true, SeqCst);
        let pid = CHILD.load(SeqCst);
        if pid > 0 {
            unsafe { kill(pid, 15) }; // timeout passes the TERM on to the game
        }
    }
    pub fn catch() {
        unsafe {
            signal(2, on_signal);
            signal(15, on_signal);
        }
    }
    pub fn child(pid: u32) {
        CHILD.store(pid as i32, SeqCst);
        if STOP.load(SeqCst) {
            on_signal(15);
        }
    }
    pub fn stopped() -> bool {
        STOP.load(SeqCst)
    }
}

#[cfg(not(target_os = "linux"))]
mod sig {
    pub fn catch() {}
    pub fn child(_: u32) {}
    pub fn stopped() -> bool {
        false
    }
}

// ---------------------------------------------------------------------------------------------- report

fn report(dirs: &[String]) -> Result<String, String> {
    let mut out = String::from(
        "| run | frames | avg fps | 1%-low | p50 / p90 / p99 ms | over 50 / 100 ms | at 60 | GPU ms p50 / p90 / p99 | CPU ms/frame: main, render, MainJob1, MainDraw | load, GPU busy before | bound |\n\
         |---|---|---|---|---|---|---|---|---|---|---|\n",
    );
    let mut users = String::new();
    for d in dirs {
        let (row, u) = analyse(Path::new(d)).map_err(|e| format!("{}: {}", d, e))?;
        out += &row;
        users += &u;
    }
    Ok(out + &users)
}

/// a text file as Python's open() reads it: universal newlines, errors='replace'
fn read_text(p: &Path) -> Result<String, String> {
    let b = fs::read(p).map_err(|e| format!("{}: {}", p.display(), e))?;
    Ok(String::from_utf8_lossy(&b).replace("\r\n", "\n").replace('\r', "\n"))
}

/// a run's table row and its line of other GPU users (empty when there are none)
fn analyse(d: &Path) -> Result<(String, String), String> {
    let meta: Value = serde_json::from_str(&read_text(&d.join("meta.json"))?).map_err(|e| format!("meta.json: {}", e))?;
    let sc = scene(meta["scene"].as_str().unwrap_or_default())?;
    let log = read_text(&d.join("log.txt"))?;
    let mut ft = Vec::new(); // (frame, ms, epoch s)
    for l in read_text(&d.join("frametimes.txt"))?.lines() {
        let a: Vec<&str> = l.split_whitespace().collect();
        if a.len() >= 4 {
            ft.push((num::<i64>(a[0])?, num::<f64>(a[1])?, num::<f64>(a[3])?));
        }
    }
    let (lo, hi) = match sc.first {
        Some(first) => (first, ft.last().ok_or("frametimes.txt has no frames")?.0),
        None => {
            // the cutscene: between the last change to mode 1 and the change back more than 30 s after it
            let m = marks(&log)?;
            let t = (1..m.len())
                .rev()
                .find(|&k| m[k - 1].0 == 1 && m[k].0 == 0 && m[k].1 - m[k - 1].1 > 30.0)
                .map(|k| (m[k - 1].1, m[k].1));
            let sel: Vec<i64> = ft.iter().filter(|f| t.is_some_and(|(t0, t1)| t0 <= f.2 && f.2 <= t1)).map(|f| f.0).collect();
            match (sel.first(), sel.last()) {
                (Some(&a), Some(&b)) => (a, b),
                _ => (0, -1),
            }
        }
    };
    let win: Vec<_> = ft.iter().filter(|f| lo <= f.0 && f.0 <= hi).collect();
    let ms: Vec<f64> = win.iter().map(|f| f.1).collect();
    let mut gpu = HashMap::new();
    if let Ok(t) = read_text(&d.join("gputimes.txt")) {
        for l in t.lines() {
            let a: Vec<&str> = l.split_whitespace().collect();
            if a.len() == 2 {
                gpu.insert(num::<i64>(a[0])?, num::<f64>(a[1])?);
            }
        }
    }
    let g: Vec<f64> = win.iter().filter_map(|f| gpu.get(&f.0).copied()).collect();
    // the "cpu ms/frame by thread" lines of the windows inside the range, same-name threads added up
    let (mut cpu, mut ncpu, mut frame) = (Vec::new(), 0, None);
    for line in crate::py::splitlines(&log) {
        if let Some(f) = frames_end(line) {
            frame = Some(f);
            continue;
        }
        if line.starts_with("nvn_perf: cpu ms/frame by thread") && frame.is_some_and(|f| lo + 300 <= f && f <= hi) {
            ncpu += 1;
            let mut seen = Vec::new();
            for (name, v) in thread_ms(line) {
                add(&mut seen, name, num(v)?);
            }
            for (k, v) in seen {
                add(&mut cpu, k, v);
            }
        }
    }
    for e in &mut cpu {
        e.1 /= ncpu as f64;
    }
    let c = |k: &str| cpu.iter().find(|e| e.0 == k).map_or(0.0, |e| e.1);
    let n = ms.len();
    let sec = py_sum(&ms) / 1000.0;
    let fps = if sec != 0.0 { n as f64 / sec } else { 0.0 };
    let share = |hit: usize, all: usize| if all > 0 { 100.0 * hit as f64 / all as f64 } else { 0.0 };
    let at60 = share(ms.iter().filter(|&&x| x < 18.5).count(), n);
    // the frames that missed 60: was their GPU time near the budget?
    let lg: Vec<f64> = win.iter().filter(|f| f.1 >= 18.5).filter_map(|f| gpu.get(&f.0).copied()).collect();
    let late_gpu = share(lg.iter().filter(|&&x| x > 14.0).count(), lg.len());
    // max(): the first of equal values
    let (top, top_ms) = cpu.iter().copied().reduce(|b, e| if e.1 > b.1 { e } else { b }).unwrap_or(("?", 0.0));
    let bound = if at60 >= 99.0 {
        "-".to_string()
    } else if late_gpu >= 50.0 {
        format!("GPU ({:.0} % of late frames)", late_gpu)
    } else {
        format!("CPU side (GPU in {:.0} % of late frames; top thread {} {:.1} ms)", late_gpu, top, top_ms)
    };
    // meta.get(k, [None])[0] as %s prints it
    let first = |k: &str| match meta.get(k).and_then(|v| v.get(0)) {
        Some(Value::String(s)) => s.clone(),
        None | Some(Value::Null) => "None".into(),
        Some(v) => v.to_string(),
    };
    let busy = meta
        .get("gpu_busy_before")
        .and_then(Value::as_f64)
        .map_or("-".into(), |b| format!("{:.0} %", b));
    let name = d.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let row = format!(
        "| {} | {} | {:.1} | {:.1} | {:.1} / {:.1} / {:.1} | {} / {} | {:.1} % | {:.1} / {:.1} / {:.1} | {:.1}, {:.1}, {:.1}, {:.1} | {}-{}, {} | {} |\n",
        name,
        n,
        fps,
        if n > 0 { 1000.0 / pct(&ms, 99.0) } else { 0.0 },
        pct(&ms, 50.0),
        pct(&ms, 90.0),
        pct(&ms, 99.0),
        ms.iter().filter(|&&x| x > 50.0).count(),
        ms.iter().filter(|&&x| x > 100.0).count(),
        at60,
        pct(&g, 50.0),
        pct(&g, 90.0),
        pct(&g, 99.0),
        c("main"),
        c("render(host)"),
        c("MainJob1"),
        c("MainDraw"),
        first("load_before"),
        first("load_after"),
        busy,
        bound
    );
    let others: Vec<&str> = meta
        .get("gpu_users_before")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .filter(|u| !DESKTOP.contains(&u.split_once(' ').map_or(*u, |x| x.1)))
        .collect();
    let users = if others.is_empty() {
        String::new()
    } else {
        format!("{}: other GPU users at the start: {}\n", name, others.join(", "))
    };
    Ok((row, users))
}

/// adds v to k's value in a list kept in first-seen order
fn add<'a>(m: &mut Vec<(&'a str, f64)>, k: &'a str, v: f64) {
    match m.iter_mut().find(|e| e.0 == k) {
        Some(e) => e.1 += v,
        None => m.push((k, v)),
    }
}

/// the nearest-rank percentile, not interpolated; 0 for no values
fn pct(v: &[f64], q: f64) -> f64 {
    let mut v = v.to_vec();
    v.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    if v.is_empty() {
        0.0
    } else {
        v[(v.len() - 1).min((q / 100.0 * v.len() as f64) as usize)]
    }
}

/// sum() of floats as CPython 3.12 and later adds them (Neumaier's compensated sum)
fn py_sum(v: &[f64]) -> f64 {
    let (mut s, mut c) = (0.0f64, 0.0f64);
    for &x in v {
        let t = s + x;
        c += if s.abs() >= x.abs() { (s - t) + x } else { (x - t) + s };
        s = t;
    }
    if c != 0.0 && c.is_finite() {
        s + c
    } else {
        s
    }
}

/// the length of s's leading run of bytes that f accepts
fn span(s: &[u8], f: impl Fn(u8) -> bool) -> usize {
    s.iter().take_while(|&&c| f(c)).count()
}

fn num_char(c: u8) -> bool {
    c.is_ascii_digit() || c == b'.'
}

/// b of a line `nvn_perf: frames (\d+)\.\.(\d+) frame time`
fn frames_end(line: &str) -> Option<i64> {
    let s = line.strip_prefix("nvn_perf: frames ")?;
    let a = span(s.as_bytes(), |c| c.is_ascii_digit());
    let s = s[a..].strip_prefix("..").filter(|_| a > 0)?;
    let b = span(s.as_bytes(), |c| c.is_ascii_digit());
    (b > 0 && s[b..].starts_with(" frame time")).then(|| s[..b].parse().ok())?
}

/// (mode, epoch s) of every `fps mode: the game asks for mode (\d) .*? at ([\d.]+) s` (runtime/game_shim_eh.c)
fn marks(log: &str) -> Result<Vec<(u8, f64)>, String> {
    const KEY: &str = "fps mode: the game asks for mode ";
    let mut out = Vec::new();
    for line in log.split('\n') {
        let mut s = line;
        while let Some(i) = s.find(KEY) {
            let after = &s[i + KEY.len()..];
            let hit = match after.as_bytes() {
                [m @ b'0'..=b'9', b' ', ..] => at_secs(&after[2..]).map(|(t, rest)| (m - b'0', t, rest)),
                _ => None,
            };
            match hit {
                Some((m, t, rest)) => {
                    out.push((m, num(t)?));
                    s = rest;
                }
                None => s = &s[i + 1..],
            }
        }
    }
    Ok(out)
}

/// the first ` at ([\d.]+) s` (the lazy `.*?` before it): the number and what follows
fn at_secs(s: &str) -> Option<(&str, &str)> {
    let mut j = 0;
    while let Some(k) = s[j..].find(" at ") {
        let p = j + k + 4;
        let n = span(s[p..].as_bytes(), num_char);
        if n > 0 && s[p + n..].starts_with(" s") {
            return Some((&s[p..p + n], &s[p + n + 2..]));
        }
        j += k + 1;
    }
    None
}

/// every (name, ms) of ` (\S+?)\[\d+\] ([\d.]+)` in a "cpu ms/frame by thread" line (runtime/nvn_gl.c)
fn thread_ms(line: &str) -> Vec<(&str, &str)> {
    let b = line.as_bytes();
    let space = |c: u8| matches!(c, b' ' | b'\t'..=b'\r' | 0x1c..=0x1f);
    // `[tid] ms` at the start of s: where the ms start and end
    let tid_ms = |s: &[u8]| {
        let d = span(s.strip_prefix(b"[")?, |c| c.is_ascii_digit());
        let v = d + 3;
        let n = span(s.get(v..)?, num_char);
        (d > 0 && s[1 + d..].starts_with(b"] ") && n > 0).then_some((v, v + n))
    };
    let (mut out, mut i) = (Vec::new(), 0);
    while i < b.len() {
        let mut hit = None;
        if b[i] == b' ' {
            // the shortest name
            let mut j = i + 1;
            while hit.is_none() && j < b.len() && !space(b[j]) {
                j += 1;
                hit = tid_ms(&b[j..]).map(|(v, e)| (j, j + v, j + e));
            }
        }
        match hit {
            Some((j, v, e)) => {
                out.push((&line[i + 1..j], &line[v..e]));
                i = e;
            }
            None => i += 1,
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(a: &str) -> Vec<String> {
        a.split(' ').map(String::from).collect()
    }

    fn sha(s: &str) -> String {
        use sha2::{Digest, Sha256};
        crate::util::hex(&Sha256::digest(s.as_bytes()))
    }

    // the expected values are scripts/perf_bench.py's own, from its fight_script, round() and cmd_run's environment
    #[test]
    fn fight_script_window_and_env_match_the_python() {
        assert_eq!(
            fight_script(0, 6),
            "0.00+0.15:V 0.35+0.15:V 0.70+0.15:V 1.05+0.15:C 1.40+0.15:C 1.90+0.15:S 2.30+0.80:r 3.20+0.15:X 3.50+0.15:V 4.00+0.60:l \
             4.70+0.15:C 5.00+0.40:VS 5.60+0.15:V 5.95+0.15:V 6.30+0.15:V 6.65+0.15:C 7.00+0.15:C 7.50+0.15:S 7.90+0.80:r 8.80+0.15:X \
             9.10+0.15:V 9.60+0.60:l 10.30+0.15:C 10.60+0.40:VS"
        );
        assert_eq!(sha(&fight_script(40, 330)), "31a1639e6aa669a9d0d1776a99e4937132aeb6823ac7d367a45781436c169cc4");
        assert_eq!(sha(&fight_script(560, 700)), "4f9513990623144afd184bc2ab37d6d0a4dc522e566bd0f3ff25e23efd0e1bd9");
        assert_eq!(sha(&fight_script(560, 600)), "fd36b38e37105545eed0c9aab3c716601c648e21385499dc0ef3ab6cbf23214d");
        for (s, d, w, h) in [
            (1.0, 1.25, 1280, 720),
            (1.6, 1.25, 2048, 1152),
            (2.4, 1.25, 3072, 1728),
            (1.0, 8.0, 200, 112),
            (2.5, 4.0, 1000, 562),
            (1.0, 1.0, 1600, 900),
        ] {
            assert_eq!(window(s, d), (w, h));
        }

        let env = |a: &str, name: &str, win: &str, tail: &[(&str, &str)]| {
            let r = parse(&args(a)).unwrap();
            assert_eq!(r.name(), name);
            let out = r.repo.join("build").join("perf").join(name);
            let p = |f: &str| out.join(f).to_string_lossy().into_owned();
            let mut want: Vec<(String, String)> = [
                ("SSBU_AUDIO_MUTE", "1".to_string()),
                ("SSBU_PRESS", "Z,25,3".into()),
                ("SSBU_SHOW", "0".into()),
                ("SSBU_FOCUS_NOTIFY", "0".into()),
                ("SSBU_GRAPHICS_CONFIG", p("graphics.ini")),
                ("SSBU_VK_SCALE", a.split(' ').nth(1).unwrap().into()),
                ("SSBU_WINDOW", win.into()),
                ("SSBU_FRAMETIMES", p("frametimes.txt")),
                ("SSBU_VK_GPUTIME", "1".into()),
                ("SSBU_GPUTIMES", p("gputimes.txt")),
                ("SSBU_ROMFS", r.repo.join("targets").join("romfs").to_string_lossy().into_owned()),
                ("SSBU_USERDATA", format!("/tmp/b3_perf_userdata_{}", name)),
            ]
            .into_iter()
            .map(|(k, v)| (k.to_string(), v))
            .collect();
            for (k, v) in tail {
                put(&mut want, k, v);
            }
            let got: Vec<(String, String)> = r
                .env()
                .into_iter()
                .map(|(k, v)| if k == "SSBU_SCRIPT" { (k, sha(&v)) } else { (k, v) })
                .collect();
            assert_eq!(got, want);
            r.secs
        };
        let shibuya = "shibuya 1.6 --tag t1 --env SSBU_SHOW=1 --env NEW=a=b --env NEW=c --env SSBU_A=1 --repo /r";
        let script = ("SSBU_SCRIPT", "31a1639e6aa669a9d0d1776a99e4937132aeb6823ac7d367a45781436c169cc4");
        assert_eq!(
            env(
                shibuya,
                "shibuya_1.6_t1",
                "2048x1152",
                &[("SSBU_SHOW", "1"), script, ("NEW", "c"), ("SSBU_A", "1")]
            ),
            330
        );
        assert_eq!(
            env(
                "cutscene 2.4 --desktop-scale 1.0 --repo /r",
                "cutscene_2.4",
                "3840x2160",
                &[("SSBU_CUTSCENE_FPS", "60")]
            ),
            300
        );
        let script = ("SSBU_SCRIPT", "fd36b38e37105545eed0c9aab3c716601c648e21385499dc0ef3ab6cbf23214d");
        assert_eq!(
            env("tokyo 1.0 --secs 600 --desktop-scale 8 --tag  --repo /r", "tokyo_1.0", "200x112", &[script]),
            600
        );
        assert!(parse(&args("tokyo 1.0 --bogus")).err().unwrap().contains("unknown argument --bogus"));
    }

    /// four made-up runs: the frame windows, the percentiles' ties, the CPU attribution and every verdict
    #[test]
    fn report_matches_the_python() {
        let t = std::env::temp_dir().join(format!("wtbuild_perf_bench_{}", std::process::id()));
        let _ = fs::remove_dir_all(&t);
        let write = |run: &str, meta: &str, frames: &[(i64, f64, f64)], gpu: Option<String>, log: &str| {
            let d = t.join(run);
            fs::create_dir_all(&d).unwrap();
            fs::write(d.join("meta.json"), meta).unwrap();
            let ft: String = frames.iter().map(|(f, ms, e)| format!("{} {:.3} 4 {:.3}\n", f, ms, e)).collect();
            fs::write(d.join("frametimes.txt"), ft).unwrap();
            if let Some(g) = gpu {
                fs::write(d.join("gputimes.txt"), g).unwrap();
            }
            fs::write(d.join("log.txt"), log).unwrap();
            d.to_string_lossy().into_owned()
        };
        let frames = |r: std::ops::Range<i64>, ms: &dyn Fn(i64) -> f64, t0: f64| r.map(|f| (f, ms(f), t0 + f as f64 * 0.0125)).collect::<Vec<_>>();
        let cpu = |a: i64, b: i64, threads: &str| {
            format!(
                "nvn_perf: frames {}..{} frame time ms: avg 16.67\nnvn_perf: cpu ms/frame by thread (all 9.6):{}\n",
                a, b, threads
            )
        };
        let mode = |m: &str, rest: &str| format!("fps mode: the game asks for mode {}{}\n", m, rest);

        let shibuya_ms = |f: i64| match f {
            2000 => 120.0,
            f if f % 97 == 0 => 55.0,
            f if f % 20 == 0 => 18.5,
            f if f % 20 == 7 => 33.4,
            _ => 16.25,
        };
        let gpu: String = (1400..2400)
            .filter(|f| f % 3 != 0)
            .map(|f| {
                format!(
                    "{} {}\n",
                    f,
                    if f % 40 == 0 {
                        14.5
                    } else if f % 20 == 0 {
                        14.0
                    } else {
                        3.25
                    }
                )
            })
            .chain(["1501 9.0\n".to_string(), "1502 1 2\n".to_string()])
            .collect();
        let log = cpu(1200, 1500, " main[1] 9.0")
            + &cpu(
                1500,
                1800,
                " main[11] 3.0 render(host)[12] 2.0 render(host)[13] 1.0 MainJob1[14] 1.2 a[1]x[2] 0.5 Witch Time[9] 0.25 MainDraw[15] 0.5",
            )
            + "nvn_perf: frames 1800..x frame time\n"
            + &cpu(
                1800,
                2100,
                " main[11] 3.5 render(host)[12] 3.5 MainJob1[14] 1.25 MainDraw[15] 0.75 bad[] 1.0 [7] 2.0",
            )
            + &cpu(2100, 2500, " main[11] 50.0");
        let a = write(
            "a_shibuya",
            r#"{"scene": "shibuya", "scale": "1.0", "tag": "a", "extra": {}, "load_before": ["4.30", "4.39", "4.10"],
                "gpu_users_before": ["1 kwin_wayland", "2 steam", "3 RDD Process", "4 Witch Time", "5nospace"], "gpu_busy_before": 15.5,
                "load_after": ["5.37", "1", "2"]}"#,
            &frames(1400..2400, &shibuya_ms, 1000.0),
            Some(gpu),
            &log,
        );

        let log = mode("2", " (15 fps) at 900.000 s (wall clock)")
            + &mode("1", " (30 fps) at 1000.000 s: runs at 60")
            + &mode("0", " (60 fps) at 1020.000 s")
            + &mode("1", " (30 fps) at noon at 1.2.3x s at 1050.500 s")
            + &mode("0", " x at 1100.25 s; fps mode: the game asks for mode 1 y at 1101.0 s")
            + &cpu(900, 1200, " main[1] 4.0 MainJob1[2] 1.0 MainJob1[3] 2.0")
            + &mode("12", " (x) at 1120.0 s")
            + &mode("0", " (60 fps) at 1140.000 s")
            + &mode("1", " (30 fps) at 1150 never");
        let b = write(
            "b_cutscene",
            r#"{"scene": "cutscene", "scale": "2.4", "tag": null, "extra": {"SSBU_X": "1"}, "gpu_busy_before": null}"#,
            &frames(0..4100, &|_| 16.6, 1090.0),
            None,
            &log,
        );

        let gpu: String = (19790..20100).map(|f| format!("{} {}\n", f, if f % 2 == 1 { 15.0 } else { 2.0 })).collect();
        let c = write(
            "c_tokyo",
            r#"{"scene": "tokyo", "scale": "1.6", "load_before": [4.25, 1, 2], "gpu_busy_before": 16.5, "gpu_users_before": []}"#,
            &frames(19790..20100, &|f| if f % 2 == 1 { 25.0 } else { 16.0 }, 0.0),
            Some(gpu),
            &cpu(19800, 20100, " render(host)[5] 7.0"),
        );

        let d = write(
            "d_cutscene",
            r#"{"scene": "cutscene", "scale": "1.0", "load_before": ["1.00"], "gpu_busy_before": 3}"#,
            &frames(0..50, &|_| 20.0, 1000.0),
            Some("1 30.0\n".into()),
            &(mode("1", " at 1000.0 s") + &mode("0", " at 1010.0 s") + &mode("0", " at 1100.0 s")),
        );

        crate::golden::check("perf_bench_report.md", &report(&[a, b, c, d]).unwrap());
        fs::remove_dir_all(&t).unwrap();
    }
}
