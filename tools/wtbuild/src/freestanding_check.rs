//! `wtbuild check-freestanding` (a developer's and CI's check): the code compiled on the player's PC comes out the same
//! with runtime/freestanding (no libc headers, as `compile` and `link` build it) as with the system's C library headers,
//! byte for byte.
//!
//!     check-freestanding [--words N] [--seed S] [--clang <c>] [--gen <dir>] [--tables <dir>] [--target windows] [--jobs N]
//!
//! Without --gen the lifted code is made up: N random instruction words (FP / SIMD and load / store heavy), one function
//! each, so no game files are needed. --gen compiles your build/gen_eh instead; --tables <runtime/gen> also compiles its
//! sdkdata.c and sdklift_gen.c and runtime/wt_tables.c. A probe checks jmp_buf's size and alignment. --target windows
//! compares against mingw-w64's headers (the UCRT ones the Windows runtime is built with) instead: --clang names a clang
//! that has them, such as llvm-mingw's.

use crate::compile::{self, run, tail};
use crate::target::Target;
use crate::{lifter, link, util};
use rayon::prelude::*;
use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const BASE: i64 = 0x7100000000;
const PER_FILE: usize = 1500;

/// splitmix64: nothing is compared against a stored result, the seed only makes a run repeatable
struct Rng(u64);

impl Rng {
    fn below(&mut self, n: u64) -> u64 {
        self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
        (z ^ (z >> 31)) % n
    }
}

/// the statements of `n` random words, lifted one by one; the unimplemented ones and branches out are left out
fn words(n: usize, seed: u64) -> Vec<String> {
    let mut r = Rng(seed);
    let mut out = Vec::new();
    for _ in 0..n {
        let k = r.below(100);
        let mut w = r.below(1 << 32) as u32;
        // bits 25-28: FP and SIMD, or loads and stores
        if k < 45 {
            w |= 7 << 25;
        } else if k < 70 {
            w = (w & !(0xf << 25)) | ([4, 6, 12, 14][r.below(4) as usize] << 25);
        }
        let pc = BASE + r.below(1 << 22) as i64 * 4;
        let entry = pc - [0, 64][r.below(2) as usize];
        let end = pc + [4, 256][r.below(2) as usize];
        let (s, _) = lifter::lift_word(w, pc, entry, end);
        if !s.starts_with("UNIMPL") && !s.contains("goto ") {
            out.push(s);
        }
    }
    out
}

/// the f_<hex> functions `s` calls (the regex \b(f_[0-9a-f]+)\( )
fn calls(s: &str) -> impl Iterator<Item = &str> {
    let b = s.as_bytes();
    (0..b.len()).filter_map(move |i| {
        if !b[i..].starts_with(b"f_") || i > 0 && (b[i - 1].is_ascii_alphanumeric() || b[i - 1] == b'_') {
            return None;
        }
        let n = b[i + 2..].iter().take_while(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f')).count();
        (n > 0 && b.get(i + 2 + n) == Some(&b'(')).then(|| &s[i..i + 2 + n])
    })
}

/// `body` as w_NNNN.c files of PER_FILE functions of one statement each, with prototypes for what they call
fn write_corpus(body: &[String], dir: &Path) -> Result<Vec<PathBuf>, String> {
    fs::create_dir_all(dir).map_err(|e| format!("{}: {}", dir.display(), e))?;
    let mut files = Vec::new();
    for (n, part) in body.chunks(PER_FILE).enumerate() {
        let mut src = String::from("#include \"runtime.h\"\n#include \"rt_ext.h\"\n");
        for f in part.iter().flat_map(|s| calls(s)).collect::<BTreeSet<_>>() {
            src += &format!("void {}(Ctx *c);\n", f);
        }
        for (j, s) in part.iter().enumerate() {
            src += &format!("void w_{}(Ctx *c) {{ {} }}\n", n * PER_FILE + j, s);
        }
        let p = dir.join(format!("w_{:04}.c", n));
        fs::write(&p, src).map_err(|e| format!("{}: {}", p.display(), e))?;
        files.push(p);
    }
    Ok(files)
}

fn s(p: &Path) -> String {
    p.to_string_lossy().into_owned()
}

/// every job compiled with the system's headers and with runtime/freestanding: (name, why it failed)
fn check(repo: &Path, clang: &str, t: Target, args: &[String], tmp: &Path, log: &mut dyn FnMut(String)) -> Result<Vec<(String, String)>, String> {
    let opt = |k: &str| args.iter().position(|a| a == k).and_then(|i| args.get(i + 1));
    let num = |k: &str, d: u64| opt(k).map_or(d, |v| v.parse().unwrap_or_else(|_| crate::die(format!("{} {}: a number", k, v))));
    let (words_n, seed, threads) = (
        num("--words", 60000) as usize,
        num("--seed", 1),
        num("--jobs", util::default_jobs() as u64).max(1) as usize,
    );
    let rt = repo.join("runtime");
    fs::create_dir_all(tmp).map_err(|e| format!("{}: {}", tmp.display(), e))?;
    let (gen, files) = match opt("--gen") {
        Some(g) => {
            let mut v: Vec<PathBuf> = fs::read_dir(g).map_err(|e| format!("{}: {}", g, e))?.flatten().map(|e| e.path()).collect();
            v.retain(|p| p.extension().is_some_and(|x| x == "c"));
            v.sort();
            (PathBuf::from(g), v)
        }
        None => {
            let gen = tmp.join("gen");
            let body = words(words_n, seed);
            let files = write_corpus(&body, &gen)?;
            log(format!("{} instructions lifted into {} files", body.len(), files.len()));
            (gen, files)
        }
    };
    let mut lifted = vec![compile::OPT.to_string()];
    lifted.extend(compile::lifted_flags(t));
    lifted.extend(["-I".into(), s(&rt), "-I".into(), s(&gen)]);
    let name = |p: &Path| p.file_name().unwrap().to_string_lossy().into_owned();
    let mut jobs: Vec<(String, PathBuf, Vec<String>)> = files.into_iter().map(|p| (name(&p), p, lifted.clone())).collect();
    if let Some(tables) = opt("--tables") {
        // the debug info names the header files, which differ by design
        let mut f: Vec<String> = link::table_flags(t).into_iter().filter(|f| f != "-g").collect();
        f.extend(["-I".into(), s(&rt), "-I".into(), tables.clone()]);
        for n in link::TABLE_SOURCES {
            let p = n.strip_prefix("gen/").map_or_else(|| rt.join(n), |g| Path::new(tables).join(g));
            jobs.push((name(&p), p, f.clone()));
        }
    }
    let probe = tmp.join("probe.c");
    let mut src = String::from("#include <setjmp.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <math.h>\n");
    src += "const unsigned long jmp_size = sizeof(jmp_buf), jmp_align = _Alignof(jmp_buf);\n";
    if t.windows() {
        // setjmp as mingw-w64 expands it (the UCRT's _setjmp and a frame)
        src += "int jump(jmp_buf b) { return setjmp(b); }\nfloat mag(float x) { return fabsf(x); }\n";
    } else {
        src += "const unsigned long tag_mask = __builtin_offsetof(struct __jmp_buf_tag, __mask_was_saved);\n";
    }
    fs::write(&probe, src).map_err(|e| format!("{}: {}", probe.display(), e))?;
    jobs.push(("probe.c".into(), probe, vec!["-O2".into()]));

    let mut pre = t.clang_flags();
    if t.windows() {
        pre.push("-mno-incremental-linker-compatible".into()); // no time stamp in the objects
    }
    let free = link::freestanding(repo);
    let build = |(name, src, flags): &(String, PathBuf, Vec<String>)| -> Option<(String, String)> {
        let mut objs = Vec::new();
        for (side, extra) in [("sys", &[][..]), ("free", &free[..])] {
            let o = tmp.join(side).join(format!("{}.o", name));
            let _ = fs::create_dir_all(o.parent().unwrap());
            let mut cmd = Command::new(clang);
            cmd.args(&pre).arg("-c").args(flags).args(extra).arg(src).arg("-o").arg(&o);
            let (code, out, _) = run(&mut cmd, None);
            if code != Some(0) {
                return Some((name.clone(), format!("{} headers: {}", side, tail(&out, 1500))));
            }
            objs.push(fs::read(&o).unwrap_or_default());
        }
        (objs[0] != objs[1]).then(|| (name.clone(), "objects differ".into()))
    };
    let pool = rayon::ThreadPoolBuilder::new().num_threads(threads).build().map_err(|e| e.to_string())?;
    let bad: Vec<(String, String)> = pool.install(|| jobs.par_iter().map(build).collect::<Vec<_>>()).into_iter().flatten().collect();
    for (name, why) in bad.iter().take(10) {
        log(format!("  {}: {}", name, why));
    }
    log(format!("freestanding: {} files compiled both ways, {} differ", jobs.len(), bad.len()));
    Ok(bad)
}

const USAGE: &str = "usage: check-freestanding [--words N] [--seed S] [--clang <c>] [--gen <dir>] [--tables <dir>] [--target windows] [--jobs N] [--repo <dir>]";

/// every option takes a value: a misspelled one would silently skip a check
fn parse(args: &[String]) -> Result<(), &'static str> {
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--words" | "--seed" | "--clang" | "--gen" | "--tables" | "--target" | "--jobs" | "--repo" if it.next().is_some() => {}
            _ => return Err(USAGE),
        }
    }
    Ok(())
}

pub fn main(args: &[String]) -> i32 {
    parse(&args[1..]).unwrap_or_else(|e| crate::die(e));
    let repo = crate::repo_root(args);
    let clang = args
        .iter()
        .position(|a| a == "--clang")
        .and_then(|i| args.get(i + 1))
        .cloned()
        .unwrap_or_else(|| compile::find_clang(&repo));
    let t = Target::from_args(args).unwrap_or_else(|e| crate::die(e));
    let tmp = std::env::temp_dir().join(format!("wtfree_{}", std::process::id()));
    let r = check(&repo, &clang, t, args, &tmp, &mut |l| println!("{}", l));
    let _ = fs::remove_dir_all(&tmp);
    match r {
        Ok(bad) if bad.is_empty() => 0,
        Ok(_) => 1,
        Err(e) => crate::die(e),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn calls_as_the_regex() {
        let s = "f_71a(c); xf_1(c); _f_2(c); f_3g(c); f_(c); f_4 (c); (f_5b)(c); &f_6(c); f_71A(c); f_8(f_9(c));";
        assert_eq!(calls(s).collect::<Vec<_>>(), ["f_71a", "f_6", "f_8", "f_9"]);
    }

    #[test]
    fn options() {
        let a = |s: &str| s.split_whitespace().map(String::from).collect::<Vec<_>>();
        assert!(parse(&a("--words 10 --tables runtime/gen --target windows")).is_ok());
        assert!(parse(&a("--table runtime/gen")).is_err());
        assert!(parse(&a("--words")).is_err());
    }
}
