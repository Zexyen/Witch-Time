//! `wtbuild setup`: from a fresh clone and YOUR OWN dump to a runnable build, on Linux and Windows. The steps, their output lines,
//! build/setup_state.json and build/logs/ are scripts/setup.py's (the launcher follows them); every step runs inside
//! this program, so the build needs no Python. scripts/setup.py's module comment describes the steps and the dump.

use crate::compile::{self, which};
use crate::target::{self, Target};
use crate::util::{fmt_secs, py_repr, sha256_file};
use crate::{lifter, link, nso, pchtxt, tables, text};
use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Instant;

const STEPS: [&str; 10] = ["deps", "verify", "convert", "mods", "tables", "lift", "compile", "link", "text", "check"];
/// cheap: deps finds the tools every later step uses, mods notices a changed mods folder
const ALWAYS: [&str; 2] = ["deps", "mods"];
const REDO_ON_UPDATE: [&str; 6] = ["tables", "lift", "compile", "link", "text", "check"];

// ---- the one version this port handles (scripts/setup.py VERSION / EXEFS / ROMFS_*)
const VERSION: &str = "the build with main build ID b47cc79b88b7091e";
/// name, size, sha256, NSO build id
const EXEFS: [(&str, u64, &str, Option<&str>); 5] = [
    (
        "main",
        16894145,
        "499983f6adabc071488be7fe75f547c079c8c8dac95c3573e78a4277d7641800",
        Some("b47cc79b88b7091ea514b2b725c50ae01c6f00df"),
    ),
    (
        "sdk",
        5989236,
        "2a89fa1c0e677710e89ed29b578162affef530a0c9b8c93cda15d50a57a20d28",
        Some("357efc198e728cb5d55265aff304f03cfc9cc0e3"),
    ),
    (
        "subsdk0",
        3385425,
        "f38d30e8241ee208cbf3090f44613e328af517ad199eb5985619416bb2cbcb55",
        Some("a8cb97803f7f442a3b6cce1c7cd4ee4a78b7def3"),
    ),
    (
        "rtld",
        7132,
        "8e08c8cf2e4d6b39d69643af10ce765d39c06860878224c68f8cafc45a2636aa",
        Some("567cdd4c5251494c5cd70822e3ccc740c190c2b2"),
    ),
    ("main.npdm", 1604, "0a716fe54a6bad240c5ae69310e6daebd0f94eccbd0348c15f3e786fd60089ca", None),
];
const REQUIRED: [&str; 4] = ["main", "sdk", "subsdk0", "main.npdm"]; // subsdk0: main imports from it (movie playback)
const ROMFS_FILES: [(&str, u64, &str); 3] = [
    ("root_000.pkz", 7323136, "87e7d717d1ced64d155d07e0b5197b2b24c08317b03c89d7ad49671699b2ce88"),
    ("Text_000.pkz", 2395648, "e9eb6c45bf8ea944578497831e46baa80a6aedea3d2464e2f323b821f8b6c3c9"),
    ("chapter_000.pkz", 39040, "dac504642db7b44018613a0c8aef968208ab65f7d54bfd54dab77c48aa05209e"),
];
const ROMFS_MANIFEST: (usize, u64, &str) = (552, 15047510227, "4926189e02ad8f4a385ac36f21fd62a914926fd70778869bec2f4bd19afab741");
const ENCRYPTED: [&str; 5] = [".nsp", ".xci", ".nca", ".nsz", ".xcz"];

/// the system libraries the link step uses when it builds the runtime: (what, a header the runtime includes, linker flags)
const SYSTEM_LIBS: [(&str, &str, &str); 5] = [
    ("SDL3", "SDL3/SDL.h", "-lSDL3"),
    ("Vulkan headers", "vulkan/vulkan.h", ""),
    (
        "glslang",
        "glslang/Include/glslang_c_interface.h",
        "-lglslang -lglslang-default-resource-limits",
    ),
    ("FFmpeg", "libavformat/avformat.h", "-lavformat -lavcodec -lswscale -lswresample -lavutil"),
    ("OpenGL", "", "-lGL"),
];
/// the same on Windows (llvm-mingw has the system's own; SDL3, the Vulkan headers, glslang and FFmpeg come with the developer's)
const WINDOWS_LIBS: [(&str, &str, &str); 4] = [
    ("SDL3", "SDL3/SDL.h", "-lSDL3"),
    ("Vulkan headers", "vulkan/vulkan.h", ""),
    (
        "glslang",
        "glslang/Include/glslang_c_interface.h",
        "-lglslang -lglslang-default-resource-limits",
    ),
    ("FFmpeg", "libavformat/avformat.h", "-lavformat -lavcodec -lswscale -lswresample -lavutil"),
];

const USAGE: &str = "usage: wtbuild setup [--dump <folder>] [--jobs N] [--only <step> | --from <step>] [--status] [--accept-modified] [--move-dump]
                     [--mods <folder>] [--llvm <folder>] [--repo <folder>]
steps: deps verify convert mods tables lift compile link text check (scripts/setup.py describes them)";

struct Fail(String);

fn say(msg: impl AsRef<str>) {
    println!("{}", msg.as_ref());
    let _ = std::io::stdout().flush();
}

struct Args {
    dump: Option<String>,
    jobs: Option<usize>,
    only: Option<String>,
    from: Option<String>,
    status: bool,
    accept_modified: bool,
    move_dump: bool,
    mods: Option<String>,
    llvm: Option<String>,
    repo: Option<String>,
}

fn parse(args: &[String]) -> Result<Args, String> {
    let mut a = Args {
        dump: None,
        jobs: None,
        only: None,
        from: None,
        status: false,
        accept_modified: false,
        move_dump: false,
        mods: None,
        llvm: None,
        repo: None,
    };
    let mut it = args.iter();
    while let Some(k) = it.next() {
        let (k, inline) = match k.split_once('=') {
            Some((k, v)) if k.starts_with("--") => (k, Some(v.to_string())),
            _ => (k.as_str(), None),
        };
        let mut val = || {
            inline
                .clone()
                .or_else(|| it.next().cloned())
                .ok_or(format!("argument {}: expected one argument", k))
        };
        match k {
            "--dump" => a.dump = Some(val()?),
            "--jobs" => a.jobs = Some(val()?.parse().map_err(|_| "argument --jobs: invalid int value".to_string())?),
            "--only" | "--from" => {
                let v = val()?;
                if !STEPS.contains(&v.as_str()) {
                    return Err(format!("argument {}: invalid choice: '{}' (choose from {})", k, v, STEPS.join(", ")));
                }
                if k == "--only" {
                    a.only = Some(v)
                } else {
                    a.from = Some(v)
                }
            }
            "--status" => a.status = true,
            "--accept-modified" => a.accept_modified = true,
            "--move-dump" => a.move_dump = true,
            "--mods" => a.mods = Some(val()?),
            "--llvm" => a.llvm = Some(val()?),
            "--repo" => a.repo = Some(val()?),
            "--toolchain" => {
                let v = val()?;
                if v != Target::host().name() {
                    return Err(format!("--toolchain {}: wtbuild builds for this system ({})", v, Target::host().name()));
                }
            }
            _ => return Err(format!("unrecognized arguments: {}", k)),
        }
    }
    Ok(a)
}

/// os.path.abspath: absolute, normalised, symbolic links kept
#[cfg(windows)]
fn abspath(p: &str) -> String {
    // GetFullPathNameW, which also takes out . and ..
    std::path::absolute(p)
        .map(|a| a.to_string_lossy().into_owned())
        .unwrap_or_else(|_| p.to_string())
}

/// os.path.abspath: absolute, normalised, symbolic links kept
#[cfg(not(windows))]
fn abspath(p: &str) -> String {
    let base = if p.starts_with('/') {
        PathBuf::new()
    } else {
        std::env::current_dir().unwrap_or_default()
    };
    let full = base.join(p);
    let mut parts: Vec<String> = Vec::new();
    for c in full.components() {
        match c {
            std::path::Component::ParentDir => {
                parts.pop();
            }
            std::path::Component::Normal(s) => parts.push(s.to_string_lossy().into_owned()),
            _ => {}
        }
    }
    format!("/{}", parts.join("/"))
}

struct Ctx {
    a: Args,
    repo: String,
    build: PathBuf,
    targets: PathBuf,
    logs: PathBuf,
    state_file: PathBuf,
    state: Map<String, Value>,
    dump: Option<String>,
    jobs: usize,
    clang: String,
}

impl Ctx {
    fn root(&self) -> &Path {
        Path::new(&self.repo)
    }

    fn save(&self) {
        let _ = fs::create_dir_all(&self.build);
        let mut s = String::new();
        dump_json(&Value::Object(self.state.clone()), 0, &mut s);
        let _ = fs::write(&self.state_file, s);
    }

    fn done(&mut self) -> &mut Map<String, Value> {
        if !matches!(self.state.get("done"), Some(Value::Object(_))) {
            self.state.insert("done".into(), json!({}));
        }
        self.state.get_mut("done").unwrap().as_object_mut().unwrap()
    }

    /// a step's work with its output in build/logs/<step>.log; a failure says where to look, with the log's end
    fn logged(&self, step: &str, f: impl FnOnce(&mut dyn FnMut(String)) -> Result<(), String>) -> Result<String, Fail> {
        let _ = fs::create_dir_all(&self.logs);
        let path = self.logs.join(format!("{}.log", step));
        let mut file = fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&path)
            .map_err(|e| Fail(format!("{}: {}", path.display(), e)))?;
        let _ = write!(file, "\n$ wtbuild {}\n", step);
        let t0 = Instant::now();
        let mut written = String::new();
        let r = f(&mut |l: String| {
            let _ = writeln!(file, "{}", l);
            let _ = file.flush();
            written.push_str(&l);
            written.push('\n');
        });
        if let Err(e) = r {
            let _ = writeln!(file, "{}", e);
            written.push_str(&e);
            return Err(Fail(format!(
                "{} failed after {}; see {}\n{}",
                step,
                fmt_secs(t0.elapsed().as_secs_f64()),
                path.display(),
                compile::tail(&written, 1500)
            )));
        }
        Ok(written)
    }
}

/// what the build's children see: the runtime's and lifter's test switches never reach them
pub fn child_env(c: &mut Command) {
    for (k, _) in std::env::vars_os() {
        let u = k.to_string_lossy().to_uppercase();
        if u.starts_with("SSBU_") || u.starts_with("RECOMP_") || u.starts_with("RECOMPS_") {
            c.env_remove(&k);
        }
    }
}

/// json.dump(v, indent=1), as setup.py writes build/setup_state.json
fn dump_json(v: &Value, level: usize, out: &mut String) {
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Number(n) => {
            if n.is_f64() {
                out.push_str(&py_repr(n.as_f64().unwrap()))
            } else {
                out.push_str(&n.to_string())
            }
        }
        Value::String(s) => out.push_str(&crate::py::json_str(s)),
        Value::Array(a) if a.is_empty() => out.push_str("[]"),
        Value::Object(m) if m.is_empty() => out.push_str("{}"),
        Value::Array(a) => {
            out.push('[');
            for (i, x) in a.iter().enumerate() {
                out.push_str(if i > 0 { ",\n" } else { "\n" });
                out.push_str(&" ".repeat(level + 1));
                dump_json(x, level + 1, out);
            }
            out.push('\n');
            out.push_str(&" ".repeat(level));
            out.push(']');
        }
        Value::Object(m) => {
            out.push('{');
            for (i, (k, x)) in m.iter().enumerate() {
                out.push_str(if i > 0 { ",\n" } else { "\n" });
                out.push_str(&" ".repeat(level + 1));
                out.push_str(&crate::py::json_str(k));
                out.push_str(": ");
                dump_json(x, level + 1, out);
            }
            out.push('\n');
            out.push_str(&" ".repeat(level));
            out.push('}');
        }
    }
}

// ---------------------------------------------------------------------------------------------- steps
fn step_deps(cx: &mut Ctx) -> Result<(), Fail> {
    let t = Target::host();
    let clang_exe = target::exe("clang");
    if let Some(l) = &cx.a.llvm {
        if !Path::new(l).join("bin").join(&clang_exe).exists() {
            return Err(Fail(format!("--llvm {}: there is no bin/{} in it", l, clang_exe)));
        }
        std::env::set_var("LLVM_ROOT", abspath(l));
    } else if std::env::var_os("LLVM_ROOT").is_none() {
        if let Some(saved) = cx.state.get("tools").and_then(|t| t.get("LLVM_ROOT")).and_then(|v| v.as_str()) {
            if Path::new(saved).join("bin").join(&clang_exe).exists() {
                std::env::set_var("LLVM_ROOT", saved);
            }
        }
    }
    let clang = compile::find_clang(cx.root());
    if !Path::new(&clang).exists() && which(&clang).is_none() {
        return Err(Fail(if t.windows() {
            "clang not found: a release has it in deps\\llvm; a checkout needs llvm-mingw (--llvm <folder>, README)".into()
        } else {
            "clang not found on PATH (install clang and lld from your distribution)".into()
        }));
    }
    let mut tools = Map::new();
    tools.insert("TOOLCHAIN".into(), json!(t.name()));
    if let Some(l) = std::env::var_os("LLVM_ROOT") {
        tools.insert("LLVM_ROOT".into(), json!(l.to_string_lossy()));
    }
    let ver = Command::new(&clang)
        .arg("--version")
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).lines().next().unwrap_or("").to_string())
        .unwrap_or_default();
    say(format!("  compiler ({}): {}   [{}]", t.name(), ver, clang));
    let probe = cx.build.join("probe");
    fs::create_dir_all(&probe).map_err(|e| Fail(e.to_string()))?;
    if link::has_prebuilt(cx.root()) {
        // a release: the runtime is prebuilt, and this PC only compiles and links game.so, without system headers
        let src = probe.join("t.c");
        let mut code = String::from(
            "#include <string.h>\n#include <math.h>\n#include <stdint.h>\ndouble t(double *d, const void *s) { memcpy(d, s, 8); return trunc(*d); }\n",
        );
        if t.windows() {
            code += "int __stdcall DllMainCRTStartup(void *m, unsigned long r, void *p) { return 1; }\n";
            // as game_shim.c's
        }
        fs::write(&src, code).map_err(|e| Fail(e.to_string()))?;
        let mut c = Command::new(&clang);
        c.args(t.clang_flags())
            .args(["-shared", "-O2", "-nostdlib", "-fuse-ld=lld"])
            .args(t.pic())
            .args(link::freestanding(cx.root()))
            .arg(&src);
        if t.windows() {
            c.arg(cx.root().join(link::PREBUILT).join("libucrtbase.a")); // memcpy and trunc from the UCRT, as game.dll
        }
        c.arg("-o").arg(probe.join(if t.windows() { "t.dll" } else { "t.so" }));
        let (code, out, _) = compile::run(&mut c, Some(120));
        if code != Some(0) {
            return Err(Fail(format!(
                "the compiler cannot build a library with the lld linker (the bundled one is deps/llvm)\n{}",
                compile::tail(&out, 800)
            )));
        }
        say("  compiler builds the game's library: ok");
        let missing = link::prebuilt_missing(cx.root());
        if !missing.is_empty() {
            return Err(Fail(if t.windows() {
                let mf = missing.iter().any(|d| d.to_ascii_lowercase().starts_with("mf"));
                format!(
                    "the game needs these DLLs, which this PC does not have: {}{} (README, \"Build (Windows)\")",
                    missing.join(", "),
                    if mf {
                        ". The MF ones are Media Foundation, which Windows N editions get from Microsoft's Media Feature Pack"
                    } else {
                        ""
                    }
                )
            } else {
                format!(
                    "the game needs these libraries, which are not installed: {}. Install them from your distribution (README, \"Build (Linux)\")",
                    missing.join(", ")
                )
            }));
        }
        say("  runtime: prebuilt, its libraries are installed");
    } else {
        let src = probe.join("p.c");
        let exe = probe.join(target::exe("p"));
        fs::write(&src, "#include <stdio.h>\nint main(void){puts(\"ok\");return 0;}\n").map_err(|e| Fail(e.to_string()))?;
        let build_run = |extra: &[&str]| -> Result<(), String> {
            let o = Command::new(&clang)
                .args(t.clang_flags())
                .arg(&src)
                .arg("-o")
                .arg(&exe)
                .args(extra)
                .output()
                .map_err(|e| e.to_string())?;
            if !o.status.success() {
                return Err(compile::tail(&(String::from_utf8_lossy(&o.stdout).into_owned() + &String::from_utf8_lossy(&o.stderr)), 800).to_string());
            }
            let r = Command::new(&exe).output().map_err(|e| e.to_string())?;
            if String::from_utf8_lossy(&r.stdout).trim() != "ok" {
                return Err("the program did not print ok".into());
            }
            Ok(())
        };
        let os = if t.windows() { "Windows" } else { "Linux" };
        build_run(&[]).map_err(|e| Fail(format!("the compiler cannot build and run a {} program\n{}", os, e)))?;
        build_run(&["-fuse-ld=lld"]).map_err(|e| {
            Fail(format!(
                "clang works, but not with the lld linker: install lld (ld.lld) from your distribution\n{}",
                e
            ))
        })?;
        say(format!("  compiler builds and runs a {} program: ok", os));
        if std::env::var_os("WTBUILD_SKIP_LIB_CHECK").is_none() {
            let mut missing = Vec::new();
            for (what, header, libs) in if t.windows() { &WINDOWS_LIBS[..] } else { &SYSTEM_LIBS[..] } {
                let c = probe.join("lib.c");
                let inc = if header.is_empty() {
                    String::new()
                } else {
                    format!("#include <{}>\n", header)
                };
                let _ = fs::write(&c, format!("{}int main(void){{return 0;}}\n", inc));
                let ok = Command::new(&clang)
                    .args(t.clang_flags())
                    .arg(&c)
                    .arg("-o")
                    .arg(probe.join("lib"))
                    .arg("-fuse-ld=lld")
                    .args(libs.split_whitespace())
                    .output()
                    .map(|o| o.status.success())
                    .unwrap_or(false);
                if !ok {
                    missing.push(format!(
                        "{} ({})",
                        what,
                        [*header, *libs].iter().filter(|s| !s.is_empty()).cloned().collect::<Vec<_>>().join(", ")
                    ));
                }
            }
            if !missing.is_empty() {
                return Err(Fail(if t.windows() {
                    format!(
                        "the development files of these libraries are missing: {}. Name their folders in CPATH and LIBRARY_PATH (README, \"Build (Windows)\")",
                        missing.join("; ")
                    )
                } else {
                    format!(
                        "the development files of these libraries are missing: {}. Install them from your distribution (README, \"Build (Linux)\")",
                        missing.join("; ")
                    )
                }));
            }
            say(if t.windows() {
                "  SDL3, Vulkan headers, glslang: ok"
            } else {
                "  SDL3, Vulkan headers, glslang, FFmpeg, OpenGL: ok"
            });
        }
    }
    cx.clang = clang;
    cx.state.insert("tools".into(), Value::Object(tools));
    Ok(())
}

/// every file below root with its size: symbolic links to folders are not followed (os.walk)
fn walk_files(root: &Path, rel: &str, out: &mut Vec<(String, PathBuf)>) {
    let Ok(rd) = fs::read_dir(root) else { return };
    for e in rd.flatten() {
        let p = e.path();
        let name = e.file_name().to_string_lossy().into_owned();
        let r = if rel.is_empty() { name } else { format!("{}/{}", rel, name) };
        let is_dir = fs::metadata(&p).map(|m| m.is_dir()).unwrap_or(false);
        if is_dir {
            if !e.file_type().map(|t| t.is_symlink()).unwrap_or(false) {
                walk_files(&p, &r, out);
            }
        } else {
            out.push((r, p));
        }
    }
}

/// (count, total bytes, sha256 of "<rel path>\t<size>\n" lines sorted by byte value) and {path: size}
fn romfs_manifest(root: &Path) -> ((usize, u64, String), HashMap<String, u64>) {
    let mut files = Vec::new();
    walk_files(root, "", &mut files);
    let sizes: HashMap<String, u64> = files.iter().map(|(r, p)| (r.clone(), fs::metadata(p).map(|m| m.len()).unwrap_or(0))).collect();
    let mut lines: Vec<Vec<u8>> = sizes.iter().map(|(r, s)| format!("{}\t{}\n", r, s).into_bytes()).collect();
    lines.sort();
    let mut h = Sha256::new();
    for l in &lines {
        h.update(l);
    }
    ((sizes.len(), sizes.values().sum(), crate::util::hex(&h.finalize())), sizes)
}

fn step_verify(cx: &mut Ctx) -> Result<(), Fail> {
    let Some(dump) = cx.dump.clone() else {
        return Err(Fail("--dump <folder> is required".into()));
    };
    // encrypted packages anywhere outside romfs/
    fn scan(d: &Path) -> Result<(), Fail> {
        let Ok(rd) = fs::read_dir(d) else { return Ok(()) };
        let mut dirs = Vec::new();
        let mut bad = None;
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if fs::metadata(e.path()).map(|m| m.is_dir()).unwrap_or(false) {
                if n.to_lowercase() != "romfs" && !e.file_type().map(|t| t.is_symlink()).unwrap_or(false) {
                    dirs.push(e.path());
                }
            } else if bad.is_none() && ENCRYPTED.iter().any(|x| n.to_lowercase().ends_with(x)) {
                bad = Some(n);
            }
        }
        if let Some(b) = bad {
            return Err(Fail(format!(
                "{}: encrypted packages are not accepted ({}). Give the extracted ExeFS and RomFS folders (README, \"What you extract from your own console\").",
                d.display(),
                crate::py::repr_str(&b)
            )));
        }
        dirs.iter().try_for_each(|d| scan(d))
    }
    scan(Path::new(&dump))?;
    let (exefs, mut romfs) = (Path::new(&dump).join("exefs"), Path::new(&dump).join("romfs"));
    if !romfs.is_dir() && cx.targets.join("romfs").is_dir() {
        romfs = cx.targets.join("romfs"); // moved into the build folder by an earlier run (--move-dump)
    }
    for d in [&exefs, &romfs] {
        if !d.is_dir() {
            return Err(Fail(format!("{} is missing: the dump folder needs exefs/ and romfs/", d.display())));
        }
    }
    let mut warnings = Vec::new();
    for (name, _size, digest, bid) in EXEFS {
        let p = exefs.join(name);
        if !p.is_file() {
            if REQUIRED.contains(&name) {
                return Err(Fail(format!(
                    "exefs/{} is missing: extract the whole ExeFS (main, sdk, subsdk0, rtld, main.npdm)",
                    name
                )));
            }
            warnings.push(format!("exefs/{} is missing", name));
            continue;
        }
        let head = fs::read(&p).map(|b| b[..b.len().min(0x54)].to_vec()).unwrap_or_default();
        let magic: &[u8] = if name == "main.npdm" { b"META" } else { b"NSO0" };
        if head.get(..4) != Some(magic) {
            return Err(Fail(format!(
                "exefs/{} is not a plain {} file (magic {}): give decrypted, extracted files",
                name,
                if name == "main.npdm" { "NPDM" } else { "NSO" },
                crate::py::repr_bytes(&head[..head.len().min(4)])
            )));
        }
        if sha256_file(&p).ok().as_deref() == Some(digest) {
            continue;
        }
        let got = if head.len() >= 0x54 && &head[..4] == b"NSO0" {
            Some(crate::util::hex(&head[0x40..0x54]))
        } else {
            None
        };
        if bid.is_some() && got.as_deref() == bid {
            say(format!(
                "  exefs/{}: same build ID, different file (recompressed?); segment hashes are checked in convert",
                name
            ));
        } else if name == "main" || name == "sdk" {
            return Err(Fail(format!(
                "exefs/{} is a different version (another update or region), or a modified file (build ID {}). This port handles only {} (main build ID {}).",
                name,
                got.unwrap_or_else(|| "None".into()),
                VERSION,
                EXEFS[0].3.unwrap()
            )));
        } else {
            warnings.push(format!("exefs/{} differs from the expected version (modified or other version)", name));
        }
    }
    let ((count, total, digest), sizes) = romfs_manifest(&romfs);
    if digest != ROMFS_MANIFEST.2 {
        let missing = ROMFS_FILES.iter().any(|(p, _, _)| !sizes.contains_key(*p));
        let msg = format!("romfs: {} files / {} bytes, expected {} / {}", count, total, ROMFS_MANIFEST.0, ROMFS_MANIFEST.1);
        if count < ROMFS_MANIFEST.0 || missing {
            return Err(Fail(format!("{}: files are missing (incomplete extraction, or not {})", msg, VERSION)));
        }
        warnings.push(format!("{}: the file list differs (other version, or modded)", msg));
    }
    for (rel, size, want) in ROMFS_FILES {
        let p = romfs.join(rel);
        if fs::metadata(&p).map(|m| m.len()).ok() != Some(size) || sha256_file(&p).ok().as_deref() != Some(want) {
            warnings.push(format!("romfs/{} differs (corrupt, other version, or modded)", rel));
        }
    }
    for w in &warnings {
        say(format!("  warning: {}", w));
    }
    if !warnings.is_empty() && !cx.a.accept_modified {
        return Err(Fail(format!(
            "the dump differs from the supported version, {} (above). Fix it, or run again with --accept-modified to continue at your own risk (mods are not blocked, but are unsupported).",
            VERSION
        )));
    }
    cx.state.insert("dump".into(), json!(dump));
    say(format!("  exefs and romfs match the supported version: {} ({} RomFS files)", VERSION, count));
    Ok(())
}

fn need_dump(cx: &Ctx) -> Result<String, Fail> {
    match &cx.dump {
        Some(d) if Path::new(d).is_dir() => Ok(d.clone()),
        _ => Err(Fail("the dump folder is not known: run with --dump <folder>".into())),
    }
}

/// dst = a real folder with the files of src, so the build folder holds the game on its own (portable): moved when
/// `mv`, else hard-linked (same drive: no extra space, the dump stays whole) or copied. A file already in dst with
/// the same size is kept, so an interrupted run resumes; a symbolic link or junction at dst (older setups) is replaced.
fn place_folder(dst: &Path, src: &Path, mv: bool) -> Result<String, Fail> {
    let f = |e: std::io::Error| Fail(e.to_string());
    if fs::symlink_metadata(dst).map(|m| m.file_type().is_symlink()).unwrap_or(false) {
        target::remove_dir_link(dst).map_err(f)?; // the link only, never what it points to
    }
    if !src.is_dir() {
        return if dst.is_dir() { Ok("in place (moved here before)".into()) } else { Err(Fail(format!("{} is missing", src.display()))) };
    }
    let mut files = Vec::new();
    walk_files(src, "", &mut files);
    let (mut placed, mut linked) = (0, 0);
    for (rel, from) in &files {
        let to = dst.join(rel);
        let size = fs::metadata(from).map_err(f)?.len();
        if fs::metadata(&to).map(|m| m.len()).ok() != Some(size) {
            fs::create_dir_all(to.parent().unwrap()).map_err(f)?;
            let _ = fs::remove_file(&to);
            let done = if mv { fs::rename(from, &to) } else { fs::hard_link(from, &to).map(|_| linked += 1) };
            if done.is_err() {
                fs::copy(from, &to).map_err(|e| Fail(format!("{} -> {}: {}", from.display(), to.display(), e)))?; // another drive
            }
            placed += 1;
        }
        if mv {
            fs::remove_file(from).ok(); // gone already when renamed
        }
    }
    if mv {
        remove_empty_dirs(src);
    }
    Ok(format!(
        "{} files, {} {} from {}{}",
        files.len(),
        placed,
        if mv { "moved" } else { "copied" },
        src.display(),
        if linked > 0 { format!(" ({} as hard links: no extra space)", linked) } else { String::new() }
    ))
}

fn remove_empty_dirs(d: &Path) {
    if let Ok(rd) = fs::read_dir(d) {
        rd.flatten().filter(|e| e.file_type().map(|t| t.is_dir()).unwrap_or(false)).for_each(|e| remove_empty_dirs(&e.path()));
    }
    let _ = fs::remove_dir(d); // fails, as it should, when something is left
}

/// shutil.copy2: the bytes and the modification time
fn copy2(from: &Path, to: &Path) -> std::io::Result<()> {
    fs::copy(from, to)?;
    let t = fs::metadata(from)?.modified()?;
    fs::File::options().write(true).open(to)?.set_modified(t)
}

fn convert_nso(src: &Path, dst: &Path) -> Result<Vec<u8>, String> {
    let d = fs::read(src).map_err(|e| format!("{}: {}", src.display(), e))?;
    let c = nso::nso2elf(&d)?;
    if !c.hashes_ok {
        return Err(c.report.join("\n"));
    }
    fs::write(dst, &c.elf).map_err(|e| format!("{}: {}", dst.display(), e))?;
    Ok(c.elf)
}

fn step_convert(cx: &mut Ctx) -> Result<(), Fail> {
    let dump = need_dump(cx)?;
    let d = Path::new(&dump).join("exefs");
    let out = cx.targets.join("exefs");
    fs::create_dir_all(&out).map_err(|e| Fail(e.to_string()))?;
    for e in fs::read_dir(&d).map_err(|e| Fail(e.to_string()))?.flatten() {
        if e.path().is_file() {
            copy2(&e.path(), &out.join(e.file_name())).map_err(|x| Fail(format!("{}: {}", e.path().display(), x)))?;
        }
    }
    for name in ["main", "sdk", "subsdk0"] {
        convert_nso(&out.join(name), &out.join(format!("{}.elf", name))).map_err(|e| {
            Fail(format!(
                "{}: NSO segment hash mismatch or conversion error: corrupt or modified file\n{}",
                name, e
            ))
        })?;
        say(format!("  {} -> targets/exefs/{}.elf, segment hashes ok", name, name));
    }
    // the game, the launcher and the run scripts read targets/romfs and targets/sysdata/fonts
    if !cx.a.move_dump {
        say("  copying the game data into targets/ (--move-dump moves it instead)");
    }
    say(format!(
        "  targets/romfs: {}",
        place_folder(&cx.targets.join("romfs"), &Path::new(&dump).join("romfs"), cx.a.move_dump)?
    ));
    let fonts = Path::new(&dump).join("sysdata").join("fonts");
    if fonts.is_dir() || cx.targets.join("sysdata").join("fonts").is_dir() {
        say(format!(
            "  targets/sysdata/fonts: {}",
            place_folder(&cx.targets.join("sysdata").join("fonts"), &fonts, cx.a.move_dump)?
        ));
    } else {
        say("  no sysdata/fonts in the dump: a font of this PC will stand in for the console's system fonts (BUILDING.md)");
    }
    Ok(())
}

fn now_utc() -> String {
    let s = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    let (days, rem) = (s.div_euclid(86400), s.rem_euclid(86400));
    // civil from days (Howard Hinnant)
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + (m <= 2) as i64;
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC", y, m, d, rem / 3600, rem / 60 % 60, rem % 60)
}

fn step_mods(cx: &mut Ctx) -> Result<(), Fail> {
    let folder = abspath(
        &cx.a
            .mods
            .clone()
            .or_else(|| cx.state.get("mods_dir").and_then(|v| v.as_str()).map(String::from))
            .unwrap_or_else(|| format!("{}/mods", cx.repo)),
    );
    let folder_p = PathBuf::from(&folder);
    let (nso_p, elf) = (cx.targets.join("exefs").join("main"), cx.targets.join("exefs").join("main.elf"));
    let applied = cx.state.get("mods").and_then(|v| v.as_str()).map(String::from);
    let files = pchtxt::find_mods(&folder_p);
    if files.is_empty() && applied.is_none() {
        say(format!("  no mods (no .pchtxt / .ips files in {}): main.elf as converted", folder));
        return Ok(());
    }
    if !nso_p.is_file() || !elf.is_file() {
        return Err(Fail("targets/exefs/main / main.elf are missing: run convert first".into()));
    }
    let settings = pchtxt::read_settings(&cx.root().join("config").join("mods.ini")); // the launcher's Mods window switches
    let rel = |f: &PathBuf| f.strip_prefix(&folder_p).unwrap_or(f).to_string_lossy().into_owned();
    let mut off: Vec<String> = files
        .iter()
        .filter(|f| !pchtxt::file_on(&rel(f), &settings))
        .map(|f| pchtxt::rel_key(&rel(f)))
        .collect();
    if !off.is_empty() {
        off.sort();
        off.dedup();
        say(format!("  switched off in config/mods.ini: {}", off.join(", ")));
    }
    let mods = pchtxt::load_mods(&folder_p, Some(&nso_p), &settings).map_err(|pchtxt::ModError(e)| Fail(e))?;
    let want = pchtxt::fingerprint(&mods);
    let elf_hash = || sha256_file(&elf).ok();
    if want == applied && (want.is_none() || elf_hash().as_deref() == cx.state.get("mods_elf").and_then(|v| v.as_str())) {
        say(format!(
            "  {} mod file(s), {}",
            files.len(),
            if want.is_some() {
                "unchanged since the last build"
            } else {
                "no enabled patches"
            }
        ));
        return Ok(());
    }
    // main.elf again from the NSO, exactly as convert makes it, then the patches on top
    let tmp = elf.with_extension("elf.tmp");
    let mut img = convert_nso(&nso_p, &tmp).map_err(|e| Fail(format!("main: NSO conversion error\n{}", e)))?;
    let mut lines = Vec::new();
    let n = match pchtxt::apply(&mut img, &mods, &mut |l| lines.push(l)) {
        Ok(n) => n,
        Err(pchtxt::ModError(e)) => {
            let _ = fs::remove_file(&tmp);
            return Err(Fail(e));
        }
    };
    fs::write(&tmp, &img).map_err(|e| Fail(e.to_string()))?;
    fs::rename(&tmp, &elf).map_err(|e| Fail(e.to_string()))?;
    let _ = fs::create_dir_all(&cx.logs);
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(cx.logs.join("mods.log")) {
        let _ = write!(
            f,
            "\n{}  {}  {}\n{}\n",
            now_utc(),
            folder,
            want.clone().unwrap_or_else(|| "None".into()),
            lines.join("\n")
        );
    }
    for l in &lines {
        say(l);
    }
    say(format!(
        "  main.elf: {}",
        if want.is_some() {
            format!("{} bytes patched by {} enabled patch(es) from {} file(s)", n, mods.len(), files.len())
        } else {
            "unpatched again (no enabled patches left)".into()
        }
    ));
    if want != applied {
        let after = &STEPS[STEPS.iter().position(|s| *s == "mods").unwrap() + 1..];
        let done = cx.done();
        let redo: Vec<&str> = after.iter().filter(|s| done.contains_key(**s)).cloned().collect();
        for s in &redo {
            done.remove(*s);
        }
        if !redo.is_empty() {
            say(format!("  the mods changed: {} run again", redo.join(", ")));
        }
    }
    cx.state.insert("mods".into(), want.clone().map_or(Value::Null, Value::String));
    cx.state.insert(
        "mods_elf".into(),
        if want.is_some() {
            elf_hash().map_or(Value::Null, Value::String)
        } else {
            Value::Null
        },
    );
    if cx.a.mods.is_some() {
        cx.state.insert("mods_dir".into(), json!(folder));
    }
    Ok(())
}

fn step_tables(cx: &mut Ctx) -> Result<(), Fail> {
    let root = cx.root().to_path_buf();
    cx.logged("tables", |log| tables::run(&root, log)).map(|_| ())
}

fn step_lift(cx: &mut Ctx) -> Result<(), Fail> {
    let (elf, out) = (cx.targets.join("exefs").join("main.elf"), cx.build.join("gen_eh"));
    cx.logged("lift", |log| {
        let img = fs::read(&elf).map_err(|e| format!("{}: {}", elf.display(), e))?;
        let mut im = lifter::Image::main(img);
        lifter::eh::options_row_patches(&mut im)?;
        lifter::eh::lift_main(&im, &out, log).map(|_| ())
    })
    .map(|_| ())
}

fn step_compile(cx: &mut Ctx) -> Result<(), Fail> {
    let (repo, clang, jobs) = (cx.repo.clone(), cx.clang.clone(), cx.jobs);
    let out = cx.logged("compile", |log| compile::run_step(&repo, &clang, Target::host(), jobs, &child_env, log))?;
    say(format!("  {}", out.lines().find(|l| l.starts_with("compiled ")).unwrap_or("compiled")));
    Ok(())
}

fn link_outputs(t: Target) -> &'static [&'static str] {
    if t.windows() {
        &["game.dll", "witch-time-game.exe"]
    } else {
        &["game.so", "libwitchtime.so", "witch-time-game"]
    }
}

fn step_link(cx: &mut Ctx) -> Result<(), Fail> {
    let (root, clang) = (cx.root().to_path_buf(), cx.clang.clone());
    let t = Target::host();
    cx.logged("link", |log| link::run_step(&root, &clang, t, &child_env, log))?;
    let files = link_outputs(t);
    for f in files {
        if !cx.build.join(f).is_file() {
            return Err(Fail(format!(
                "build/{} is missing after the link step (see {})",
                f,
                cx.logs.join("link.log").display()
            )));
        }
    }
    let mb = |f: &str| fs::metadata(cx.build.join(f)).map(|m| m.len()).unwrap_or(0) as f64 / (1u64 << 20) as f64;
    say(format!(
        "  build/{} {:.0} MB, {} {:.1} MB ({})",
        files[0],
        mb(files[0]),
        files[1],
        mb(files[1]),
        if link::has_prebuilt(&root) { "prebuilt" } else { "built here" }
    ));
    Ok(())
}

fn step_text(cx: &mut Ctx) -> Result<(), Fail> {
    let root = cx.root().to_path_buf();
    let romfs = cx.targets.join("romfs");
    let mods = root.join("mods");
    let out = cx.logged("text", |log| text::run(&root, &romfs, None, &mods, &child_env, log))?;
    say(format!("  {}", out.trim().lines().last().unwrap_or("")));
    Ok(())
}

fn step_check(_cx: &mut Ctx) -> Result<(), Fail> {
    say("  skipped (the lifter self-tests need Python and Unicorn: python3 lifter/test_ext.py, a developer's check)");
    Ok(())
}

// ---------------------------------------------------------------------------------------------- updates
/// A newer Witch Time unpacked over a finished build: the files the later steps read have changed, so tables, lift,
/// compile, link, text and check run again (compile then compiles only the C files that changed). This program itself
/// stands for the lifter and the build scripts.
const SOURCE_GLOBS: [(&str, &[&str]); 9] = [
    ("lifter", &[".txt", ".h"]),
    ("runtime", &[".c", ".h", ".inc"]),
    ("runtime/gen_static", &[""]),
    ("runtime/linux", &[".c", ".h"]),
    ("runtime/linux/GL", &[".h"]),
    ("runtime/posix", &[".c", ".h"]),
    ("runtime/gen", &["make_gen.py"]),
    ("runtime/split", &[".h"]),
    ("runtime/freestanding", &[".h"]),
];

fn source_fingerprint(repo: &Path) -> String {
    let mut h = Sha256::new();
    for (dir, exts) in SOURCE_GLOBS {
        let mut files: Vec<String> = fs::read_dir(repo.join(dir))
            .map(|rd| {
                rd.flatten()
                    .filter(|e| e.path().is_file())
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .filter(|n| !n.starts_with('.') && exts.iter().any(|x| n.ends_with(x)))
                    .collect()
            })
            .unwrap_or_default();
        files.sort();
        for n in files {
            h.update(format!("{}/{}", dir, n).as_bytes());
            h.update([0]);
            h.update(Sha256::digest(fs::read(repo.join(dir).join(&n)).unwrap_or_default()));
        }
    }
    if let Some(exe) = std::env::current_exe().ok().and_then(|p| fs::read(p).ok()) {
        h.update(b"wtbuild\0");
        h.update(Sha256::digest(exe));
    }
    crate::util::hex(&h.finalize())[..20].to_string()
}

fn package_version(repo: &Path) -> Option<String> {
    fs::read_to_string(repo.join("VERSION"))
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
}

// ---------------------------------------------------------------------------------------------- main
pub fn main(args: &[String]) -> i32 {
    let a = match parse(args) {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{}\nwtbuild setup: error: {}", USAGE, e);
            return 2;
        }
    };
    let repo = abspath(a.repo.as_deref().unwrap_or("."));
    let build = Path::new(&repo).join("build");
    let state_file = build.join("setup_state.json");
    let state: Map<String, Value> = fs::read(&state_file)
        .ok()
        .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
        .and_then(|v| if let Value::Object(m) = v { Some(m) } else { None })
        .unwrap_or_default();
    let dump = a
        .dump
        .as_deref()
        .map(abspath)
        .or_else(|| state.get("dump").and_then(|v| v.as_str()).map(String::from));
    let jobs = a.jobs.filter(|j| *j > 0).unwrap_or_else(crate::util::default_jobs);
    let mut cx = Ctx {
        repo: repo.clone(),
        targets: Path::new(&repo).join("targets"),
        logs: build.join("logs"),
        build,
        state_file,
        state,
        dump,
        jobs,
        clang: String::new(),
        a,
    };
    let _ = rayon::ThreadPoolBuilder::new().num_threads(jobs).build_global();
    cx.done();
    if cx.a.status {
        let done = cx.done().clone();
        for s in STEPS {
            say(format!(
                "{:<8} {}",
                s,
                match done.get(s).and_then(|v| v.as_f64()) {
                    Some(t) => format!("done ({})", fmt_secs(t)),
                    None => "-".into(),
                }
            ));
        }
        return 0;
    }
    if cx.a.dump.is_some() {
        if let (Some(d), Some(old)) = (&cx.dump, cx.state.get("dump").and_then(|v| v.as_str())) {
            if d != old {
                cx.done().clear(); // another dump: everything is made again
            }
        }
    }
    let todo: Vec<&str> = match &cx.a.only {
        Some(o) => STEPS.iter().filter(|s| ALWAYS.contains(s) || *s == o).cloned().collect(),
        None => STEPS.to_vec(),
    };
    let fp = source_fingerprint(Path::new(&repo));
    if cx.a.only.is_none() && cx.state.get("source_fp").and_then(|v| v.as_str()) != Some(&fp) && REDO_ON_UPDATE.iter().any(|s| cx.done().contains_key(*s)) {
        let done = cx.done();
        let redo: Vec<&str> = REDO_ON_UPDATE.iter().filter(|s| done.contains_key(**s)).cloned().collect();
        for s in &redo {
            done.remove(*s);
        }
        say(format!(
            "Witch Time itself changed since the last build (an update): {} run again; compile redoes only the files that changed",
            redo.join(", ")
        ));
    }
    if let Some(f) = cx.a.from.clone() {
        let i = STEPS.iter().position(|s| *s == f).unwrap();
        let done = cx.done();
        for s in &STEPS[i..] {
            done.remove(*s);
        }
    }
    // a link marked done whose files are gone (deleted, or made by an older Witch Time under other names) links again
    if cx.done().contains_key("link") && link_outputs(Target::host()).iter().any(|f| !cx.build.join(f).is_file()) {
        let i = STEPS.iter().position(|s| *s == "link").unwrap();
        let done = cx.done();
        for s in &STEPS[i..] {
            done.remove(*s);
        }
    }
    let t_all = Instant::now();
    let n = STEPS.len();
    for (i, s) in STEPS.iter().enumerate() {
        let already = cx.done().contains_key(*s);
        if !todo.contains(s) || (already && !ALWAYS.contains(s) && cx.a.only.is_none()) {
            continue;
        }
        say(format!("[{}/{}] {} ...", i + 1, n, s));
        let t0 = Instant::now();
        let step = match *s {
            "deps" => step_deps,
            "verify" => step_verify,
            "convert" => step_convert,
            "mods" => step_mods,
            "tables" => step_tables,
            "lift" => step_lift,
            "compile" => step_compile,
            "link" => step_link,
            "text" => step_text,
            _ => step_check,
        };
        // a bug (or a file of a shape nobody expected) ends the step with a message, not without its FAILED line
        let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| step(&mut cx))).unwrap_or_else(|p| {
            let what = p
                .downcast_ref::<String>()
                .cloned()
                .or_else(|| p.downcast_ref::<&str>().map(|s| s.to_string()))
                .unwrap_or_default();
            Err(Fail(format!("internal error in wtbuild ({}); please report it with build/logs", what)))
        });
        if let Err(Fail(e)) = r {
            say(format!("[{}/{}] {}: FAILED\n{}", i + 1, n, s, e));
            cx.save();
            return 1;
        }
        let secs = (t0.elapsed().as_secs_f64() * 10.0).round() / 10.0;
        cx.done().insert(s.to_string(), json!(secs));
        cx.save();
        say(format!("[{}/{}] {}: ok  {}", i + 1, n, s, fmt_secs(secs)));
    }
    say(format!("setup: {} this run", fmt_secs(t_all.elapsed().as_secs_f64())));
    if STEPS.iter().all(|s| cx.done().contains_key(*s)) {
        cx.state.insert("source_fp".into(), json!(fp)); // the build matches these files (and this package version)
        if let Some(v) = package_version(Path::new(&repo)) {
            cx.state.insert("version".into(), json!(v));
        }
        cx.save();
        say(format!(
            "Start the game: build/{} targets/exefs/main.elf (docs/STATUS.md)",
            Target::host().game_exe()
        ));
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn place_folder_copies_resumes_and_moves() {
        let t = std::env::temp_dir().join(format!("wtbuild_place_{}", std::process::id()));
        let (src, dst) = (t.join("dump romfs"), t.join("targets").join("romfs"));
        fs::create_dir_all(src.join("movie")).unwrap();
        fs::write(src.join("a.pkz"), b"aa").unwrap();
        fs::write(src.join("movie").join("b.webm"), b"b").unwrap();
        #[cfg(unix)]
        {
            fs::create_dir_all(dst.parent().unwrap()).unwrap();
            std::os::unix::fs::symlink(&src, &dst).unwrap(); // an older setup's link: replaced, its target kept
        }
        assert!(place_folder(&dst, &src, false).ok().unwrap().starts_with("2 files, 2 copied"));
        assert!(!fs::symlink_metadata(&dst).unwrap().file_type().is_symlink());
        assert!(place_folder(&dst, &src, false).ok().unwrap().starts_with("2 files, 0 copied"));
        fs::remove_file(dst.join("a.pkz")).unwrap();
        assert!(place_folder(&dst, &src, true).ok().unwrap().starts_with("2 files, 1 moved"));
        assert!(!src.exists());
        assert_eq!(fs::read(dst.join("movie").join("b.webm")).unwrap(), b"b");
        assert_eq!(fs::read(dst.join("a.pkz")).unwrap(), b"aa");
        assert!(place_folder(&dst, &src, true).is_ok()); // moved before: in place
        fs::remove_dir_all(&t).unwrap();
    }
}
