//! The `compile` step: build/gen_eh/*.c -> build/obj/*.obj with clang -O2, only the files whose object is out of date.
//! build/obj/_stamps.json keeps, per file, the SHA-256 of the C source, the times of source and object, and a hash of
//! everything else its object depends on (Cfg::hash: the compiler's path, size and time, the flags, the optnone
//! threshold, RULES and the headers the lifted files include, found by following #include "..."). funcs.h, which the
//! lifter rewrites on every run, is compared with the copy kept from the last compile (_funcs.h.prev): an added
//! prototype cannot change an unchanged file's object, a removed or changed one compiles the files that name it again.
//! Objects without a C file are deleted. Functions over OPTNONE statements get optnone; a file over the time limit is
//! tried again with smaller thresholds, then at -O0. The code is compiled against runtime/freestanding instead of the
//! system's C library headers (the same objects, without a libc development package: `wtbuild check-freestanding`).
//! On Windows the objects are MinGW ones (target.rs), for game.dll; on macOS Mach-O ones, for game.dylib.

use crate::target::{self, mtime_ns, Target};
use crate::util::sha256_file;
use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;
use std::time::{Duration, Instant};

const FORCE_INLINE: &str = "-Dinline=inline __attribute__((always_inline))";
pub const OPT: &str = "-O2";
const OPTNONE: usize = 5000; // statements: a bigger function gets optnone (clang 23 is slow on the largest)
const TIME_LIMIT: u64 = 900; // seconds for one file at -O2
const RULES: i64 = 1; // in every stamp: raising it compiles every file again

/// clang: $LLVM_ROOT/bin/clang, <repo>/deps/llvm/bin/clang, else clang on PATH
/// (clang.exe on Windows: the bundled one in a release, llvm-mingw's in a checkout)
pub fn find_clang(repo: &Path) -> String {
    let deps = repo.join("deps").join("llvm");
    let clang = target::exe("clang");
    for d in [std::env::var_os("LLVM_ROOT").map(PathBuf::from), Some(deps)].into_iter().flatten() {
        if d.as_os_str().is_empty() {
            continue;
        }
        if d.join("bin").join(&clang).exists() {
            let d = if d.is_absolute() { d } else { std::env::current_dir().unwrap().join(d) };
            return d.join("bin").join(&clang).to_string_lossy().into_owned();
        }
    }
    which(&clang).unwrap_or(clang)
}

/// shutil.which
pub fn which(name: &str) -> Option<String> {
    let path = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path) {
        let p = dir.join(name);
        if target::is_program(&p) {
            return Some(p.to_string_lossy().into_owned());
        }
    }
    None
}

/// (exit code or None on timeout, stdout + stderr interleaved, seconds)
pub fn run(cmd: &mut Command, limit: Option<u64>) -> (Option<i32>, String, f64) {
    let t = Instant::now();
    let (mut rd, wr) = match std::io::pipe() {
        Ok(p) => p,
        Err(e) => return (Some(-1), e.to_string(), 0.0),
    };
    let wr2 = match wr.try_clone() {
        Ok(w) => w,
        Err(e) => return (Some(-1), e.to_string(), 0.0),
    };
    cmd.stdin(Stdio::null()).stdout(wr).stderr(wr2);
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => return (Some(-1), format!("{}: {}", cmd.get_program().to_string_lossy(), e), 0.0),
    };
    // the Command still holds the pipe's write ends: replace them, or the reader never sees the end
    cmd.stdout(Stdio::null()).stderr(Stdio::null());
    let reader = std::thread::spawn(move || {
        let mut b = Vec::new();
        let _ = rd.read_to_end(&mut b);
        b
    });
    let code = loop {
        match child.try_wait() {
            Ok(Some(st)) => break Some(st.code().unwrap_or(-1)),
            Ok(None) => {}
            Err(_) => break Some(-1),
        }
        if limit.is_some_and(|l| t.elapsed().as_secs() >= l) {
            let _ = child.kill();
            let _ = child.wait();
            break None;
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    let out = reader.join().unwrap_or_default();
    (code, String::from_utf8_lossy(&out).into_owned(), t.elapsed().as_secs_f64())
}

/// the last `n` characters of s (Python's s[-n:])
pub fn tail(s: &str, n: usize) -> &str {
    let c = s.chars().count();
    if c <= n {
        return s;
    }
    let skip = s.char_indices().nth(c - n).map(|x| x.0).unwrap_or(0);
    &s[skip..]
}

fn includes(text: &str) -> Vec<String> {
    // ^\s*#\s*include\s+"([^"]+)" per line
    let mut out = Vec::new();
    for line in text.split('\n') {
        let Some(r) = line.trim_start().strip_prefix('#') else { continue };
        let Some(r) = r.trim_start().strip_prefix("include") else { continue };
        let t = r.trim_start();
        if t.len() == r.len() {
            continue;
        }
        if let Some(q) = t.strip_prefix('"') {
            if let Some(e) = q.find('"') {
                if e > 0 {
                    out.push(q[..e].to_string());
                }
            }
        }
    }
    out
}

fn latin1(b: &[u8]) -> String {
    b.iter().map(|&c| c as char).collect()
}

/// Python json.dumps of a list of these
enum J<'a> {
    I(i128),
    S(&'a str),
    L(Vec<J<'a>>),
}

fn dumps(j: &J) -> String {
    match j {
        J::I(v) => v.to_string(),
        J::S(s) => crate::py::json_str(s),
        J::L(v) => format!("[{}]", v.iter().map(dumps).collect::<Vec<_>>().join(", ")),
    }
}

struct Cfg {
    target: Target,
    clang: String,
    gen: String,
    obj: PathBuf,
    flags: Vec<String>,
    inc: String,
}

impl Cfg {
    fn src(&self, name: &str) -> PathBuf {
        Path::new(&self.gen).join(name)
    }
    fn obj_of(&self, name: &str) -> PathBuf {
        self.obj.join(format!("{}.obj", &name[..name.len() - 2]))
    }

    /// everything an object depends on besides its C file and funcs.h
    fn hash(&self, names: &[String]) -> Result<(String, Vec<String>), String> {
        let st = fs::metadata(&self.clang).map_err(|e| format!("{}: {}", self.clang, e))?;
        let mtime_ns = mtime_ns(&st);
        let flags: Vec<J> = self.flags.iter().map(|f| J::S(f)).collect();
        let head = J::L(vec![
            J::I(RULES as i128),
            J::S(&self.clang),
            J::I(st.len() as i128),
            J::I(mtime_ns),
            J::S(OPT),
            J::L(flags),
            J::I(OPTNONE as i128),
            J::S(self.target.name()),
        ]);
        let mut h = Sha256::new();
        h.update(dumps(&head).as_bytes());
        let mut todo: BTreeSet<String> = BTreeSet::new();
        for n in names {
            let mut b = vec![0u8; 400];
            let k = fs::File::open(self.src(n)).and_then(|mut f| f.read(&mut b)).unwrap_or(0);
            todo.extend(includes(&latin1(&b[..k])));
        }
        let mut seen: BTreeMap<String, Vec<u8>> = BTreeMap::new();
        while let Some(inc) = todo.pop_first() {
            if seen.contains_key(&inc) || inc == "funcs.h" {
                continue;
            }
            let path = [&self.inc, &self.gen].iter().map(|d| Path::new(d).join(&inc)).find(|p| p.is_file());
            let Some(path) = path else {
                return Err(format!(
                    "header {} (included by the lifted files) not found in {} or {}",
                    inc, self.inc, self.gen
                ));
            };
            let data = fs::read(&path).map_err(|e| e.to_string())?;
            seen.insert(inc, Sha256::digest(&data).to_vec());
            todo.extend(includes(&latin1(&data)));
        }
        // the C library headers the code is compiled against (included as <...>, so not followed above)
        let free = Path::new(&self.inc).join("freestanding");
        let mut libc: Vec<String> = fs::read_dir(&free)
            .map(|rd| {
                rd.flatten()
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .filter(|n| n.ends_with(".h"))
                    .collect()
            })
            .unwrap_or_default();
        libc.sort();
        for n in &libc {
            h.update(format!("<{}>", n).as_bytes());
            h.update([0]);
            h.update(Sha256::digest(fs::read(free.join(n)).unwrap_or_default()));
        }
        for (inc, d) in &seen {
            h.update(inc.as_bytes());
            h.update([0]);
            h.update(d);
        }
        Ok((crate::util::hex(&h.finalize()), seen.into_keys().collect()))
    }
}

/// names whose funcs.h line is gone or changed since the last compile; None when no earlier copy exists
fn removed_prototypes(c: &Cfg) -> Option<HashSet<String>> {
    let prev = fs::read(c.obj.join("_funcs.h.prev")).ok()?;
    let new = fs::read(Path::new(&c.gen).join("funcs.h")).unwrap_or_default();
    let (old, new) = (latin1(&prev), latin1(&new));
    let newset: HashSet<&str> = crate::py::splitlines(&new).into_iter().collect();
    let mut out = HashSet::new();
    for l in crate::py::splitlines(&old) {
        if newset.contains(l) {
            continue;
        }
        if let Some(r) = l.strip_prefix("void ") {
            let n: String = r.chars().take_while(|&ch| is_word(ch)).collect();
            if !n.is_empty() && r[n.len()..].starts_with('(') {
                out.insert(n);
            }
        }
    }
    Some(out)
}

fn is_word(c: char) -> bool {
    c == '_' || c.is_alphanumeric()
}

fn num(v: Option<&Value>) -> Option<i128> {
    v.and_then(|x| x.as_i64().map(|i| i as i128).or_else(|| x.as_u64().map(|u| u as i128)))
}

fn up_to_date(c: &Cfg, name: &str, stamps: &mut Map<String, Value>, cfg: &str, gone: &Option<HashSet<String>>) -> bool {
    let Some(Value::Object(s)) = stamps.get_mut(name) else { return false };
    let obj = c.obj_of(name);
    let om = match fs::metadata(&obj) {
        Ok(m) if m.is_file() => m,
        _ => return false,
    };
    if s.get("cfg").and_then(|v| v.as_str()) != Some(cfg) || Some(mtime_ns(&om)) != num(s.get("obj")) {
        return false;
    }
    let Ok(st) = fs::metadata(c.src(name)) else { return false };
    if (Some(st.len() as i128), Some(mtime_ns(&st))) != (num(s.get("size")), num(s.get("mtime"))) {
        if sha256_file(&c.src(name)).ok().as_deref() != s.get("src").and_then(|v| v.as_str()) {
            return false;
        }
        s.insert("size".into(), json!(st.len()));
        s.insert("mtime".into(), json!(mtime_ns(&st) as u64));
    }
    if let Some(g) = gone {
        if !g.is_empty() {
            let text = latin1(&fs::read(c.src(name)).unwrap_or_default());
            let mut w = String::new();
            for ch in text.chars().chain(std::iter::once(' ')) {
                if is_word(ch) {
                    w.push(ch);
                } else if !w.is_empty() {
                    if g.contains(&w) {
                        return false;
                    }
                    w.clear();
                }
            }
        }
    }
    true
}

fn stamp(c: &Cfg, name: &str, cfg: &str) -> Value {
    let st = fs::metadata(c.src(name)).ok();
    let om = fs::metadata(c.obj_of(name)).ok();
    json!({
        "src": sha256_file(&c.src(name)).unwrap_or_default(),
        "size": st.as_ref().map(|m| m.len()).unwrap_or(0),
        "mtime": st.as_ref().map(|m| mtime_ns(m) as u64).unwrap_or(0),
        "obj": om.as_ref().map(|m| mtime_ns(m) as u64).unwrap_or(0),
        "cfg": cfg,
    })
}

fn save_stamps(c: &Cfg, stamps: &Map<String, Value>) {
    let tmp = c.obj.join("_stamps.json.tmp");
    if fs::write(&tmp, serde_json::to_string(stamps).unwrap()).is_ok() {
        let _ = fs::rename(&tmp, c.obj.join("_stamps.json"));
    }
}

/// functions over `limit` statements: (statements, name, parameters)
fn big_functions(src: &Path, limit: usize) -> Vec<(usize, String, String)> {
    let text = latin1(&fs::read(src).unwrap_or_default());
    let mut out = Vec::new();
    let mut cur: Option<(String, String)> = None;
    let mut n = 0;
    for line in text.split_inclusive('\n') {
        let l = line.strip_suffix('\n').unwrap_or(line);
        match &cur {
            None => {
                // ^(?:static )?void (\w+)\((Ctx \*c(?:, uint64_t rp)?)\) \{$
                let r = l.strip_prefix("static ").unwrap_or(l);
                let Some(r) = r.strip_prefix("void ") else { continue };
                let name: String = r.chars().take_while(|&ch| is_word(ch)).collect();
                if name.is_empty() {
                    continue;
                }
                let r = &r[name.len()..];
                for params in ["Ctx *c", "Ctx *c, uint64_t rp"] {
                    if r == format!("({}) {{", params) {
                        cur = Some((name.clone(), params.to_string()));
                        n = 0;
                    }
                }
            }
            Some((name, params)) => {
                if l.starts_with('}') {
                    if n > limit {
                        out.push((n, name.clone(), params.clone()));
                    }
                    cur = None;
                } else {
                    n += 1;
                }
            }
        }
    }
    out
}

fn compile_one(c: &Cfg, name: &str, env: &dyn Fn(&mut Command)) -> (Option<i32>, String, f64, String) {
    let (src, obj) = (c.src(name), c.obj_of(name));
    let mut total = 0.0;
    for limit in [OPTNONE, OPTNONE / 4, OPTNONE / 16] {
        let big = big_functions(&src, limit);
        let hdr = obj.with_extension("optnone.h");
        let mut extra: Vec<String> = Vec::new();
        if !big.is_empty() {
            let mut s = String::from("#include \"runtime.h\"\n");
            for (_, f, p) in &big {
                s += &format!("void {}({}) __attribute__((optnone, noinline));\n", f, p);
            }
            let _ = fs::write(&hdr, s);
            extra = vec!["-include".into(), hdr.to_string_lossy().into_owned()];
        } else if limit != OPTNONE {
            continue;
        }
        let mut cmd = Command::new(&c.clang);
        env(&mut cmd);
        cmd.args(["-c", OPT]).args(&c.flags).args(&extra).arg(&src).arg("-o").arg(&obj);
        let (code, out, secs) = run(&mut cmd, Some(TIME_LIMIT));
        total += secs;
        if code.is_some() {
            let note = if big.is_empty() {
                String::new()
            } else {
                format!("{} fn(s) > {} stmts optnone", big.len(), limit)
            };
            return (code, out, total, note);
        }
    }
    let mut cmd = Command::new(&c.clang);
    env(&mut cmd);
    cmd.args(["-c", "-O0"]).args(&c.flags).arg(&src).arg("-o").arg(&obj);
    let (code, out, secs) = run(&mut cmd, None);
    (code, out, total + secs, "WHOLE FILE -O0 (time limit hit 3x)".into())
}

/// the lifted code's flags after `-c OPT`, without the target's and the header folders (check-freestanding uses them
/// too). They are hashed into every player's object stamps: a change compiles every build again
pub fn lifted_flags(t: Target) -> Vec<String> {
    let mut flags: Vec<String> = ["-ffp-contract=off", "-w", "-fno-strict-aliasing"].map(String::from).into();
    flags.extend(t.cpu());
    flags.extend(t.pic());
    flags.push(FORCE_INLINE.into());
    flags
}

/// the compile step with `jobs` clang processes: Err(the failure) when a file did not compile
pub fn run_step(repo: &str, clang: &str, t: Target, jobs: usize, env: &(dyn Fn(&mut Command) + Sync), log: &mut dyn FnMut(String)) -> Result<(), String> {
    let root = format!("{}/scripts/..", repo); // the spelling every player's stamps hold: another compiles everything again
    fs::create_dir_all(Path::new(repo).join("scripts")).map_err(|e| format!("scripts: {}", e))?; // so the spelling resolves
    let gen = match crate::layout::sub().as_str() {
        "" => format!("{}/build/gen_eh", root),
        p => format!("{}/build/{}/gen_eh", root, p), // [witchtime] program N (crate::layout)
    };
    let inc = format!("{}/runtime", root);
    let free = format!("{}/freestanding", inc); // no system headers: runtime/freestanding/README.md
    let mut flags = t.clang_flags();
    flags.extend(lifted_flags(t));
    let level = t.cpu_level(); // in the stamps' hash with the rest: a build moved to another processor compiles again
    if let Some(l) = level.first() {
        log(format!("compile: for this PC's processor ({})", l));
    }
    flags.extend(level);
    flags.extend(["-nostdlibinc", "-isystem", &free, "-I", &inc, "-I", &gen].map(String::from));
    let c = Cfg {
        target: t,
        clang: clang.to_string(),
        gen: gen.clone(),
        obj: crate::layout::build(Path::new(repo)).join("obj"),
        flags,
        inc,
    };
    fs::create_dir_all(&c.obj).map_err(|e| e.to_string())?;
    let t0 = Instant::now();
    let mut names: Vec<String> = fs::read_dir(&c.gen)
        .map_err(|e| format!("{}: {}", c.gen, e))?
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.ends_with(".c"))
        .collect();
    names.sort();
    let keep: HashSet<&str> = names.iter().map(|n| &n[..n.len() - 2]).collect();
    for e in fs::read_dir(&c.obj).map_err(|e| e.to_string())?.flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        let base = n.split('.').next().unwrap_or("");
        if (n.ends_with(".obj") || n.ends_with(".optnone.h")) && !keep.contains(base) {
            let _ = fs::remove_file(e.path());
            log(format!("removed {} (its C file is gone)", n));
        }
    }
    let (cfg, headers) = c.hash(&names)?;
    let mut stamps: Map<String, Value> = fs::read(c.obj.join("_stamps.json"))
        .ok()
        .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
        .and_then(|v| if let Value::Object(m) = v { Some(m) } else { None })
        .map(|m| {
            m.into_iter()
                .filter(|(k, _)| keep.contains(&k[..k.len().saturating_sub(2)]) && k.ends_with(".c"))
                .collect()
        })
        .unwrap_or_default();
    let gone = removed_prototypes(&c);
    if gone.is_none() && !stamps.is_empty() {
        log(format!(
            "no copy of the last funcs.h ({}): every file is compiled",
            c.obj.join("_funcs.h.prev").display()
        ));
        stamps.clear();
    }
    let mut stale: Vec<String> = Vec::new();
    for n in &names {
        if !up_to_date(&c, n, &mut stamps, &cfg, &gone) {
            stale.push(n.clone());
        }
    }
    for n in &stale {
        stamps.remove(n);
    }
    // biggest first: better packing (stable, as Python's sort)
    stale.sort_by_key(|n| std::cmp::Reverse(fs::metadata(c.src(n)).map(|m| m.len()).unwrap_or(0)));
    log(format!(
        "{} of {} C files to compile ({} up to date; headers: {}; {} prototype(s) removed or changed in funcs.h) after {:.1}s",
        stale.len(),
        names.len(),
        names.len() - stale.len(),
        headers.join(" "),
        gone.as_ref().map_or(0, |g| g.len()),
        t0.elapsed().as_secs_f64()
    ));
    let (mut failed, mut notes, mut done) = (Vec::new(), Vec::new(), 0usize);
    let next = AtomicUsize::new(0);
    let (tx, rx) = mpsc::channel();
    std::thread::scope(|s| {
        for _ in 0..jobs.max(1).min(stale.len().max(1)) {
            let tx = tx.clone();
            let (c, stale, next) = (&c, &stale, &next);
            s.spawn(move || loop {
                let i = next.fetch_add(1, Ordering::SeqCst);
                let Some(name) = stale.get(i) else { break };
                if tx.send((name.clone(), compile_one(c, name, env))).is_err() {
                    break;
                }
            });
        }
        drop(tx);
        for (name, (code, out, secs, note)) in rx {
            done += 1;
            let bad = code != Some(0);
            if bad {
                failed.push((name.clone(), out));
            } else {
                stamps.insert(name.clone(), stamp(&c, &name, &cfg));
            }
            if !note.is_empty() || secs > 120.0 {
                notes.push(format!("{:<12} {:>6.0}s  {}", name, secs, note));
            }
            if done % 25 == 0 || bad || done == stale.len() {
                log(format!(
                    "[{}/{}] {:.0}s{}",
                    done,
                    stale.len(),
                    t0.elapsed().as_secs_f64(),
                    if bad { format!("  FAILED {}", name) } else { String::new() }
                ));
                save_stamps(&c, &stamps);
            }
        }
    });
    save_stamps(&c, &stamps);
    if failed.is_empty() {
        let _ = fs::copy(Path::new(&c.gen).join("funcs.h"), c.obj.join("_funcs.h.prev"));
    }
    let secs = t0.elapsed().as_secs_f64();
    let mut rep = format!(
        "compiled {} files in {:.0}s ({} up to date), {} failed\n",
        stale.len(),
        secs,
        names.len() - stale.len(),
        failed.len()
    );
    for n in &notes {
        rep += n;
        rep.push('\n');
    }
    for (n, out) in &failed {
        rep += &format!("\n==== {}\n{}\n", n, tail(out, 4000));
    }
    let _ = fs::write(c.obj.join("_report.txt"), rep);
    log(format!(
        "compiled {} files in {:.0}s ({} up to date; TOOLCHAIN={}); {} failed; {} slow/fallback (see {})",
        stale.len(),
        secs,
        names.len() - stale.len(),
        t.name(),
        failed.len(),
        notes.len(),
        c.obj.join("_report.txt").display()
    ));
    for (n, out) in failed.iter().take(5) {
        log(format!("==== {}\n{}", n, tail(out, 1500)));
    }
    if failed.is_empty() {
        Ok(())
    } else {
        Err(format!("{} C file(s) did not compile", failed.len()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lifted_flags_unchanged() {
        // these strings are in every player's _stamps.json hash
        let linux = ["-ffp-contract=off", "-w", "-fno-strict-aliasing", "-mcx16", "-fPIC", "-Dinline=inline __attribute__((always_inline))"];
        assert_eq!(lifted_flags(Target::Linux), linux);
        assert_eq!(lifted_flags(Target::Windows), [&linux[..4], &linux[5..]].concat());
        assert_eq!(lifted_flags(Target::MacOS), [&linux[..3], &linux[4..]].concat());
    }
}
