mod compile;
mod lifter;
mod link;
mod nso;
mod nvn_procs;
mod pchtxt;
mod py;
mod sdklift;
mod setup;
mod tables;
mod target;
mod text;
mod text_strings;
mod texpack;
mod util;

use std::io::{BufRead, Write};
use std::path::{Path, PathBuf};

fn die(msg: impl std::fmt::Display) -> ! {
    eprintln!("wtbuild: {}", msg);
    std::process::exit(1)
}

/// the checkout: --repo <dir>, else the current folder
fn repo_root(args: &[String]) -> PathBuf {
    args.iter()
        .position(|a| a == "--repo")
        .and_then(|i| args.get(i + 1))
        .map(PathBuf::from)
        .unwrap_or_else(|| std::env::current_dir().unwrap_or_else(|e| die(e)))
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("lift-words") => {
            // lines "<word hex> <pc hex> <entry hex> <end hex>" on stdin -> one statement per line
            let stdin = std::io::stdin();
            let mut out = std::io::BufWriter::new(std::io::stdout());
            for line in stdin.lock().lines() {
                let line = line.unwrap();
                let f: Vec<i64> = line.split_whitespace().map(|s| i64::from_str_radix(s, 16).unwrap()).collect();
                let (s, br) = lifter::lift_word(f[0] as u32, f[1], f[2], f[3]);
                writeln!(out, "{}{}", if br { "BR " } else { "" }, s).unwrap();
            }
        }
        Some("nso2elf") if args.len() == 3 => {
            let d = std::fs::read(&args[1]).unwrap_or_else(|e| die(format!("{}: {}", args[1], e)));
            let c = nso::nso2elf(&d).unwrap_or_else(|e| die(e));
            for l in &c.report {
                println!("{}", l);
            }
            std::fs::write(&args[2], &c.elf).unwrap_or_else(|e| die(e));
            println!("wrote {} {}", args[2], c.elf.len());
        }
        Some("lift") if args.len() >= 3 => {
            let img = std::fs::read(&args[1]).unwrap_or_else(|e| die(format!("{}: {}", args[1], e)));
            let mut im = lifter::Image::main(img);
            if !args.iter().any(|a| a == "--no-patches") {
                lifter::eh::options_row_patches(&mut im).unwrap_or_else(|e| die(e));
            }
            lifter::eh::lift_main(&im, Path::new(&args[2]), &mut |l| println!("{}", l)).unwrap_or_else(|e| die(e));
        }
        Some("setup") => std::process::exit(setup::main(&args[1..])),
        Some("compile") => {
            // compile_main.py on build/gen_eh (developers: the step on its own)
            let repo = repo_root(&args).to_string_lossy().into_owned();
            let clang = compile::find_clang(Path::new(&repo));
            let jobs = args
                .iter()
                .position(|a| a == "--jobs")
                .and_then(|i| args.get(i + 1))
                .and_then(|j| j.parse().ok())
                .unwrap_or(8);
            let t = target::Target::from_args(&args).unwrap_or_else(|e| die(e));
            if let Err(e) = compile::run_step(&repo, &clang, t, jobs, &setup::child_env, &mut |l| println!("{}", l)) {
                die(e)
            }
        }
        Some("link") => {
            // the link step on its own (developers, smoke_link.py): build/game.so, and the runtime when not prebuilt
            let repo = repo_root(&args);
            let clang = compile::find_clang(&repo);
            let t = target::Target::from_args(&args).unwrap_or_else(|e| die(e));
            link::run_step(&repo, &clang, t, &setup::child_env, &mut |l| println!("{}", l)).unwrap_or_else(|e| die(e));
        }
        Some("runtime") => {
            // the prebuilt runtime of a release (CI): wtbuild runtime --out <dir> [--repo <dir>] [--target windows]
            let repo = repo_root(&args);
            let out = args
                .iter()
                .position(|a| a == "--out")
                .and_then(|i| args.get(i + 1))
                .unwrap_or_else(|| die("runtime: --out <dir>"));
            let clang = compile::find_clang(&repo);
            let t = target::Target::from_args(&args).unwrap_or_else(|e| die(e));
            link::make_prebuilt(&repo, &clang, t, Path::new(out), &mut |l| println!("{}", l)).unwrap_or_else(|e| die(e));
        }
        Some("tables") if args.len() == 2 => {
            tables::run(Path::new(&args[1]), &mut |l| println!("{}", l)).unwrap_or_else(|e| die(e));
        }
        Some("mods-tree") | Some("mods-apply") => {
            // pchtxt.py tree <mods folder> [<main NSO>] / apply <mods folder> <main NSO> <in.elf> <out.elf>  [--ini=<mods.ini>]
            let ini = args
                .iter()
                .find_map(|a| a.strip_prefix("--ini=").map(PathBuf::from))
                .unwrap_or_else(|| repo_root(&args).join("config").join("mods.ini"));
            let rest: Vec<&String> = args[1..]
                .iter()
                .enumerate()
                .filter(|(i, a)| !a.starts_with("--") && args[*i] != "--repo")
                .map(|(_, a)| a)
                .collect();
            let settings = pchtxt::read_settings(&ini);
            let r = if args[0] == "mods-tree" && !rest.is_empty() {
                pchtxt::tree(Path::new(rest[0]), rest.get(1).map(Path::new), &settings).map(|lines| println!("{}", lines.join("\n")))
            } else if rest.len() == 4 {
                pchtxt::load_mods(Path::new(rest[0]), Some(Path::new(rest[1])), &settings).and_then(|mods| {
                    let mut elf = std::fs::read(rest[2]).unwrap_or_else(|e| die(e));
                    let n = pchtxt::apply(&mut elf, &mods, &mut |l| println!("{}", l))?;
                    std::fs::write(rest[3], &elf).unwrap_or_else(|e| die(e));
                    println!("{} bytes patched -> {}", n, rest[3]);
                    Ok(())
                })
            } else {
                die("usage: see tools/wtbuild/README.md")
            };
            if let Err(pchtxt::ModError(e)) = r {
                eprintln!("error: {}", e);
                std::process::exit(1);
            }
        }
        Some("text") => {
            // pc_settings_mod.py [--romfs <folder>] [--dump <folder>] [--out <mods folder>]
            let repo = repo_root(&args);
            let opt = |k: &str| args.iter().position(|a| a == k).and_then(|i| args.get(i + 1)).map(PathBuf::from);
            let romfs = opt("--romfs").unwrap_or_else(|| repo.join("targets").join("romfs"));
            let mods = opt("--out").unwrap_or_else(|| repo.join("mods"));
            text::run(&repo, &romfs, opt("--dump").as_deref(), &mods, &|_| {}, &mut |l| println!("{}", l)).unwrap_or_else(|e| die(e));
        }
        Some("textures") => {
            // #54: the BC7 pack of the game's ASTC textures. textures [--romfs <folder>] [--dump <--pkz-dump folder>] [--jobs N]
            let repo = repo_root(&args);
            let opt = |k: &str| args.iter().position(|a| a == k).and_then(|i| args.get(i + 1)).map(PathBuf::from);
            let romfs = opt("--romfs").unwrap_or_else(|| repo.join("targets").join("romfs"));
            let jobs = opt("--jobs").and_then(|j| j.to_str()?.parse().ok()).unwrap_or(0);
            texpack::run(&repo, &romfs, opt("--dump").as_deref(), jobs, &setup::child_env, &mut |l| println!("{}", l)).unwrap_or_else(|e| die(e));
        }
        Some("sha256") if args.len() == 2 => {
            // the launcher's updater checks a downloaded release with it
            println!("{}", util::sha256_file(Path::new(&args[1])).unwrap_or_else(|e| die(format!("{}: {}", args[1], e))));
        }
        _ => die("usage: see tools/wtbuild/README.md"),
    }
}
