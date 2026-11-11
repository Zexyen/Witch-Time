mod compile;
mod fake;
mod fps_census;
mod freestanding_check;
#[cfg(test)]
mod golden;
mod layout;
mod lifter;
mod link;
mod lprof;
mod nso;
mod nvn_procs;
mod pchtxt;
mod perf_bench;
mod py;
mod sdklift;
mod setup;
mod smoke;
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

/// the arguments after the command, without --flags and the folder of --repo
fn positional(args: &[String]) -> Vec<&str> {
    (1..args.len()).filter(|&i| !args[i].starts_with("--") && args[i - 1] != "--repo").map(|i| args[i].as_str()).collect()
}

fn print_all(lines: Result<Vec<String>, String>) {
    let mut out = std::io::BufWriter::new(std::io::stdout());
    for l in lines.unwrap_or_else(|e| die(e)) {
        writeln!(out, "{}", l).unwrap_or_else(|e| die(e));
    }
}

fn main() {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    // [witchtime] --program N: the tables, compile and link commands work on program N's folders (layout.rs)
    if let Some(i) = args.iter().position(|a| a == "--program") {
        let p = args.get(i + 1).and_then(|v| v.parse().ok()).unwrap_or_else(|| die("--program needs a number"));
        layout::set_program(p);
        args.drain(i..i + 2);
    }
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
        Some("check-freestanding") => std::process::exit(freestanding_check::main(&args)),
        Some("compile") => {
            // the compile step on build/gen_eh on its own (developers)
            let repo = repo_root(&args).to_string_lossy().into_owned();
            let clang = compile::find_clang(Path::new(&repo));
            let jobs = args
                .iter()
                .position(|a| a == "--jobs")
                .and_then(|i| args.get(i + 1))
                .and_then(|j| j.parse().ok())
                .unwrap_or_else(util::default_jobs);
            let t = target::Target::from_args(&args).unwrap_or_else(|e| die(e));
            if let Err(e) = compile::run_step(&repo, &clang, t, jobs, &setup::child_env, &mut |l| println!("{}", l)) {
                die(e)
            }
        }
        Some("link") => {
            // the link step on its own (developers): build/game.so, and the runtime when not prebuilt
            let repo = repo_root(&args);
            let clang = compile::find_clang(&repo);
            let t = target::Target::from_args(&args).unwrap_or_else(|e| die(e));
            link::run_step(&repo, &clang, t, &setup::child_env, &mut |l| println!("{}", l)).unwrap_or_else(|e| die(e));
        }
        Some("smoke") => smoke::main(&args[1..]).unwrap_or_else(|e| die(e)),
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
            // mods-tree <mods folder> [<main NSO>] / mods-apply <mods folder> <main NSO> <in.elf> <out.elf>  [--ini=<mods.ini>]
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
            // text [--romfs <folder>] [--dump <folder>] [--out <mods folder>]
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
            let jobs = opt("--jobs").and_then(|j| j.to_str()?.parse().ok()).unwrap_or_else(util::default_jobs);
            // build/logs/textures.log too (#158): the launcher shows only the last lines
            let logs = repo.join("build").join("logs");
            let mut file = std::fs::create_dir_all(&logs).and_then(|_| std::fs::OpenOptions::new().create(true).append(true).open(logs.join("textures.log"))).ok();
            let mut keep = |l: &str| file.as_mut().map(|f| writeln!(f, "{}", l).ok());
            keep("\n$ wtbuild textures");
            let r = texpack::run(&repo, &romfs, opt("--dump").as_deref(), jobs, &setup::child_env, &mut |l| {
                keep(&l);
                println!("{}", l)
            });
            r.unwrap_or_else(|e| {
                keep(&e);
                die(e)
            });
        }
        Some("fake-nso") => fake::main(&args[1..]).unwrap_or_else(|e| die(e)),
        Some("nvn-procs") => {
            // nvn-procs [<sdk.elf>] [name...]: the sdk's NVN entry points, by default all of targets/exefs/sdk.elf's
            let mut names = positional(&args);
            let path = match names.first() {
                Some(p) if p.ends_with(".elf") => PathBuf::from(names.remove(0)),
                _ => repo_root(&args).join("targets").join("exefs").join("sdk.elf"),
            };
            let d = std::fs::read(&path).unwrap_or_else(|e| die(format!("{}: {}", path.display(), e)));
            print_all(nvn_procs::procs(&d).map(|t| nvn_procs::listing(&t, &names)));
        }
        Some("pkz") if args.len() == 3 && args[1] == "list" => print_all(text::pkz_list(Path::new(&args[2]))),
        Some("dat") if args.len() >= 3 => {
            // dat list <file.dat> / dat extract <file.dat> <folder>
            let d = std::fs::read(&args[2]).unwrap_or_else(|e| die(format!("{}: {}", args[2], e)));
            match (args[1].as_str(), args.get(3)) {
                ("list", _) => print_all(text::dat_list(&d)),
                ("extract", Some(out)) => println!("{} files -> {}", text::dat_extract(&d, Path::new(out)).unwrap_or_else(|e| die(e)), out),
                _ => die("usage: see tools/wtbuild/README.md"),
            }
        }
        Some("fps-census") => {
            // fps-census <census at 60> <census above 60> [<fps_sites.txt>] [--all]
            let files = positional(&args);
            if files.len() < 2 {
                die("usage: see tools/wtbuild/README.md")
            }
            let read = |p: &Path| std::fs::read_to_string(p).unwrap_or_else(|e| die(format!("{}: {}", p.display(), e)));
            let sites = files.get(2).map(PathBuf::from).unwrap_or_else(|| repo_root(&args).join("build").join("gen_eh").join("fps_sites.txt"));
            let every = args.iter().any(|a| a == "--all");
            print_all(fps_census::report(&read(Path::new(files[0])), &read(Path::new(files[1])), &read(&sites), every));
        }
        Some("lprof") => {
            // lprof <lprof.txt> [threads (6)] [functions per list (25)]
            let p = positional(&args);
            let count = |i: usize, default: usize| p.get(i).map_or(default, |s| s.parse().unwrap_or_else(|_| die(format!("lprof: {} is not a count", s))));
            let path = p.first().unwrap_or_else(|| die("usage: see tools/wtbuild/README.md"));
            let d = std::fs::read(path).unwrap_or_else(|e| die(format!("{}: {}", path, e)));
            print_all(Ok(lprof::report(&String::from_utf8_lossy(&d), count(1, 6), count(2, 25), &mut lprof::file_syms)));
        }
        Some("perf-bench") => perf_bench::main(&args[1..]),
        Some("sha256") if args.len() == 2 => {
            // the launcher's updater checks a downloaded release with it
            println!("{}", util::sha256_file(Path::new(&args[1])).unwrap_or_else(|e| die(format!("{}: {}", args[1], e))));
        }
        _ => die("usage: see tools/wtbuild/README.md"),
    }
}
