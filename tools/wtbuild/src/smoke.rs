//! Developer and CI check of the link step on the stand-in modules (testdata/fake_main: no game or sdk code): tables,
//! lift, compile and link as on a player's PC, then the program is run until it has mapped the stand-in main.
//!
//!     wtbuild smoke [--repo <dir>] [--prebuilt <dir>] [--lib <file>]... [--target windows] [--wine <program>]
//!
//! --prebuilt: the runtime made by `wtbuild runtime` (as a release ships it), so only game.so is built here; without it
//! the link step builds the runtime too, against the system's libraries. --lib: a library a release keeps in build/
//! (libSDL3.so.0, SDL3.dll). Checks that every symbol resolves (`ldd -r`; only the game functions the runtime replaces,
//! which the stand-in does not have, may stay undefined) and that witch-time-game maps the image with the values game.so
//! holds.
//!
//! Windows (on Windows, or --target windows elsewhere): game.dll and witch-time-game.exe. A DLL cannot leave a symbol
//! undefined, so the game functions the runtime replaces are stand-ins too, and the program starting at all is the check
//! that every import resolves. With --target windows elsewhere the program runs under Wine (--wine <program>, default wine).

use crate::target::Target;
use crate::{compile, lifter, link, nso, setup, tables, util};
use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const USAGE: &str = "usage: smoke [--repo <dir>] [--prebuilt <dir>] [--lib <file>]... [--target windows] [--wine <program>]";

// nvn_sdk.c's sdk addresses the stand-in sdk has no counterpart of (any value links; the smoke run never calls them)
const SDKA: [&str; 15] = [
    "nvnTextureBuilderGetRawStorageClass",
    "nvnTextureBuilderGetSparseTileLayout",
    "nvnTextureBuilderGetStorageAlignment",
    "nvnTextureBuilderGetStorageClass",
    "nvnTextureBuilderGetStorageSize",
    "nvnTextureGetRawStorageClass",
    "nvnTextureGetSparseTileLayout",
    "nvnTextureGetStorageClass",
    "nvnTextureGetStorageSize",
    "chip_descriptor",
    "format_count",
    "format_table",
    "texture_layout",
    "vt_hal_gm200",
    "vt_nvgpu_device",
];

/// the game functions game_shim_eh.c declares (`^void (f_71[0-9a-f]+)\(Ctx \*c\);`): their lifted originals, which a
/// stand-in does not have
fn game_only(shim: &str) -> BTreeSet<String> {
    shim.lines()
        .filter_map(|l| {
            let hex = l.strip_prefix("B3_FN ").unwrap_or(l).strip_prefix("void f_71")?; // B3_FN: a weak reference (#115)
            let n = hex.bytes().take_while(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')).count();
            (n > 0 && hex[n..].starts_with("(Ctx *c);")).then(|| format!("f_71{}", &hex[..n]))
        })
        .collect()
}

/// (--prebuilt, --lib..., --wine); every option takes a value
fn parse(args: &[String]) -> Result<(Option<PathBuf>, Vec<PathBuf>, Option<String>), String> {
    let (mut prebuilt, mut libs, mut wine) = (None, Vec::new(), None);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let v = it.next().ok_or(USAGE)?;
        match a.as_str() {
            "--prebuilt" => prebuilt = Some(PathBuf::from(v)),
            "--lib" => libs.push(PathBuf::from(v)),
            "--wine" => wine = Some(v.clone()),
            "--repo" | "--target" => {} // repo_root, Target::from_args
            _ => return Err(USAGE.into()),
        }
    }
    Ok((prebuilt, libs, wine))
}

/// shutil.copytree: the files below `from`, but the entries named in `skip`
fn copy_tree(from: &Path, to: &Path, skip: &[&str]) -> Result<(), String> {
    fs::create_dir_all(to).map_err(|e| format!("{}: {}", to.display(), e))?;
    for e in fs::read_dir(from).map_err(|e| format!("{}: {}", from.display(), e))? {
        let e = e.map_err(|e| e.to_string())?;
        if skip.iter().any(|s| e.file_name() == *s) {
            continue;
        }
        let (p, q) = (e.path(), to.join(e.file_name()));
        if p.is_dir() {
            copy_tree(&p, &q, skip)?;
        } else {
            fs::copy(&p, &q).map_err(|e| format!("{}: {}", p.display(), e))?;
        }
    }
    Ok(())
}

pub fn main(args: &[String]) -> Result<(), String> {
    let (prebuilt, libs, wine) = parse(args)?;
    let t = Target::from_args(args)?;
    let wine = wine.or_else(|| (t.windows() && !cfg!(windows)).then(|| "wine".into()));
    let root = crate::repo_root(args);
    let shim = root.join("runtime").join("game_shim_eh.c");
    let only = game_only(&fs::read_to_string(&shim).map_err(|e| format!("{}: {}", shim.display(), e))?);
    if only.is_empty() {
        return Err(format!("{}: no game function prototypes found", shim.display()));
    }
    let tmp = std::env::temp_dir().join(format!("wtsmoke_{}", std::process::id()));
    let _ = fs::remove_dir_all(&tmp);
    let r = run(&root, &tmp, t, prebuilt.as_deref(), &libs, wine.as_deref(), &only);
    let _ = fs::remove_dir_all(&tmp);
    r
}

fn run(root: &Path, tmp: &Path, t: Target, prebuilt: Option<&Path>, libs: &[PathBuf], wine: Option<&str>, only: &BTreeSet<String>) -> Result<(), String> {
    let log = &mut |l: String| println!("{}", l);
    let io = |p: &Path, e: std::io::Error| format!("{}: {}", p.display(), e);
    let fake = root.join("tools").join("wtbuild").join("testdata").join("fake_main");
    let repo = tmp.join("repo");
    let (build, exefs) = (repo.join("build"), repo.join("targets").join("exefs"));
    // an exact copy, or link.rs's sources hash differs from the prebuilt runtime's
    copy_tree(&root.join("runtime"), &repo.join("runtime"), &["gen"])?;
    for d in [repo.join("runtime").join("gen"), repo.join("lifter"), exefs.clone(), build.clone()] {
        fs::create_dir_all(&d).map_err(|e| io(&d, e))?;
    }
    for (from, to) in [
        (root.join("lifter").join("sdklift_install.txt"), repo.join("lifter").join("sdklift_install.txt")),
        (fake.join("roots.txt"), repo.join("lifter").join("sdklift_roots.txt")),
        (fake.join("sdk.nso"), exefs.join("sdk")),
    ] {
        fs::copy(&from, &to).map_err(|e| io(&from, e))?;
    }
    let main_nso = fake.join("main.nso");
    let main = nso::nso2elf(&fs::read(&main_nso).map_err(|e| io(&main_nso, e))?)?.elf;
    let main_elf = exefs.join("main.elf");
    fs::write(&main_elf, &main).map_err(|e| io(&main_elf, e))?;
    tables::run(&repo, log).map_err(|e| format!("tables: {}", e))?;
    let addrs = repo.join("runtime").join("gen").join("nvn_sdk_addrs.h");
    let mut h = fs::read_to_string(&addrs).map_err(|e| io(&addrs, e))?;
    for n in SDKA {
        if !h.contains(&format!("#define SDKA_{} ", n)) {
            h += &format!("#define SDKA_{} 0x10000ull  /* wtbuild smoke */\n", n);
        }
    }
    fs::write(&addrs, h).map_err(|e| io(&addrs, e))?;
    // the Options-row patches fit only the real main
    lifter::eh::lift_main(&lifter::Image::main(main), &build.join("gen_eh"), log).map_err(|e| format!("lift: {}", e))?;
    let clang = compile::find_clang(root);
    compile::run_step(&repo.to_string_lossy(), &clang, t, util::default_jobs(), &setup::child_env, log)?;
    if t.windows() || t.macos() {
        // macOS: dyld looks up every symbol of game.dylib when it loads it, the functions of the real main too
        let standin = build.join("standin.c");
        let c: String = only.iter().map(|n| format!("void {}(Ctx *c) {{ (void)c; }}\n", n)).collect();
        fs::write(&standin, format!("typedef struct Ctx Ctx;\n{}", c)).map_err(|e| io(&standin, e))?;
        let mut cmd = Command::new(&clang);
        cmd.args(t.clang_flags())
            .arg("-c")
            .arg(&standin)
            .arg("-o")
            .arg(build.join("obj").join("zz_standin.obj"));
        let (code, out, _) = compile::run(&mut cmd, None);
        if code != Some(0) {
            return Err(format!("the stand-ins of the game functions did not compile:\n{}", compile::tail(&out, 3000)));
        }
    }
    if let Some(p) = prebuilt {
        copy_tree(p, &repo.join(link::PREBUILT), &[])?;
    }
    for lib in libs {
        let to = build.join(lib.file_name().ok_or(format!("--lib {}: not a file", lib.display()))?);
        fs::copy(lib, &to).map_err(|e| io(lib, e))?;
    }
    let mut used_prebuilt = false;
    link::run_step(&repo, &clang, t, &setup::child_env, &mut |l| {
        used_prebuilt |= l.contains("runtime: prebuilt");
        println!("{}", l)
    })?;
    if prebuilt.is_some() && !used_prebuilt {
        return Err("the prebuilt runtime was not used".into());
    }
    if t == Target::Linux {
        let o = Command::new("ldd")
            .arg("-r")
            .arg(build.join(t.game_exe()))
            .output()
            .map_err(|e| format!("ldd: {}", e))?;
        let (out, err) = (String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr));
        let undef: BTreeSet<&str> = out
            .lines()
            .chain(err.lines())
            .filter(|l| l.starts_with("undefined symbol:"))
            .filter_map(|l| l.split_whitespace().nth(2))
            .filter(|n| !only.contains(*n))
            .collect();
        let missing: Vec<&str> = out
            .lines()
            .filter(|l| l.contains("=> not found"))
            .filter_map(|l| l.split_whitespace().next())
            .collect();
        if !undef.is_empty() || !missing.is_empty() {
            let list = |v: Vec<&str>| if v.is_empty() { "-".to_string() } else { v.join(", ") };
            return Err(format!(
                "unresolved: {}; libraries not found: {}",
                list(undef.into_iter().collect()),
                list(missing)
            ));
        }
        println!("ldd -r: every symbol resolves (but the {} game functions a stand-in lacks)", only.len());
    }
    let run_dir = build.join("run");
    fs::create_dir_all(&run_dir).map_err(|e| io(&run_dir, e))?;
    let game = build.join(t.game_exe());
    let mut cmd = match wine {
        Some(w) => {
            let mut c = Command::new(w);
            if std::env::var_os("WINEDEBUG").is_none() {
                c.env("WINEDEBUG", "-all");
            }
            c.arg(&game);
            c
        }
        None => Command::new(&game),
    };
    // not setup::child_env: it would remove WT_ROMFS
    cmd.arg(&main_elf).current_dir(&run_dir);
    cmd.env("SDL_VIDEO_DRIVER", "offscreen")
        .env("SDL_AUDIO_DRIVER", "dummy")
        .env("WT_ROMFS", tmp.join("romfs"));
    let (_, text, _) = compile::run(&mut cmd, Some(60));
    let mapped: Vec<&str> = text
        .lines()
        .filter(|l| ["host address table:", "image mapped at", "image protection:"].iter().any(|p| l.starts_with(p)))
        .collect();
    if mapped.len() < 3 {
        return Err(format!("{} did not map the stand-in main:\n{}", t.game_exe(), compile::tail(&text, 3000)));
    }
    println!("{}\nsmoke: ok", mapped.join("\n"));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn game_functions() {
        // a definition, an indented line, no digits, upper case and another prefix do not count
        let shim = [
            "B3_FN void f_7100b382c0(Ctx *c);",
            "void f_71007e1fe4(Ctx *c);  /* x */",
            "void f_7100b382c0(Ctx *c) {",
        ]
        .into_iter()
        .chain([
            " void f_7100aaaaaa(Ctx *c);",
            "void f_71(Ctx *c);",
            "void f_7100ABCDEF(Ctx *c);",
            "void f_7200aaaaaa(Ctx *c);",
        ])
        .collect::<Vec<_>>()
        .join("\n");
        assert_eq!(game_only(&shim).into_iter().collect::<Vec<_>>(), ["f_71007e1fe4", "f_7100b382c0"]);
        let real = fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../runtime/game_shim_eh.c")).unwrap();
        assert!(!game_only(&real).is_empty());
    }

    #[test]
    fn args() {
        let a: Vec<String> = "--lib a.so --repo r --prebuilt p --lib b.dll --target windows --wine wine64"
            .split(' ')
            .map(String::from)
            .collect();
        let (p, libs, wine) = parse(&a).unwrap();
        assert_eq!(
            (p, libs, wine),
            (
                Some(PathBuf::from("p")),
                vec![PathBuf::from("a.so"), PathBuf::from("b.dll")],
                Some("wine64".into())
            )
        );
        assert!(parse(&["--lib".into()]).is_err());
        assert!(parse(&["--jobs".into(), "4".into()]).is_err());
    }
}
