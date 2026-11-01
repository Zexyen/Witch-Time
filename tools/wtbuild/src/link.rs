//! The `link` step: build/game.so (Windows: game.dll) from the lifted objects and the player's own tables, beside the
//! runtime (libwitchtime.so, witch-time-game, libssbu_glslang.so; Windows: witch-time-game.exe, ssbu_glslang.dll).
//!
//! The runtime holds nothing from the dump: the few values it needs from it (runtime/split) are data in game.so
//! (runtime/wt_tables.c, with gen/sdkdata.c and gen/sdklift_gen.c). So a release ships the runtime prebuilt (prebuilt/,
//! made by `wtbuild runtime` in CI) and the player's PC only compiles the tables and links game.so, with no system
//! headers or libraries (runtime/freestanding). A checkout without prebuilt/, or whose runtime sources changed since,
//! builds the runtime here, against the system's SDL3, FFmpeg, glslang and GL.
//!
//! Windows: the runtime is one program, witch-time-game.exe, as scripts/build_runtime.py makes it, importing game.dll's
//! tables (GAME_EXPORTS) through an import library made from game.def. game.dll imports nothing from it: the calls back
//! into the runtime go through the pointers game_bind hands over (game_shim.c). It is linked without the C runtime's
//! start-up code and imports the few C functions it calls from the UCRT, which every Windows 10 and 11 has, through
//! mingw-w64's libucrtbase.a (shipped in prebuilt/). So the player's PC needs only clang and lld there too, and builds
//! the runtime itself only in a checkout (with llvm-mingw, SDL3, the Vulkan headers and glslang).

use crate::compile::{run, tail};
use crate::target::Target;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Instant;

/// build_runtime.py HOST_SOURCES, without the player's tables; on Linux also the POSIX platform layer (POSIX_SOURCES)
const HOST_SOURCES: &[&str] = &[
    "hostrt.c",
    "boot.c",
    "nnsdk.c",
    "libcxx.c",
    "os_threads.c",
    "nvn_stub.c",
    "nvn_gl.c",
    "nvn_vk.c",
    "mx_translate.c",
    "tex_decode.c",
    "tex_pack.c",
    "nnfs.c",
    "clib.c",
    "nntime.c",
    "nnhid.c",
    "nnpad_sdl.c",
    "display.c",
    "nnpl.c",
    "nnfontll.c",
    "nnmovie.c",
    "nnmisc.c",
    "nnaudio.c",
    "nnmovie_mf.c",
    "nniostream.c",
    "nncxxrt.c",
    "nncxxlib.c",
    "nnlibc2.c",
    "sdklift_rt.c",
    "nnsdk2.c",
    "nnapplet.c",
    "nnalbum.c",
    "nnnet.c",
    "nnfsdir.c",
    "nnunwind.c",
    "nvn_sdk.c",
    "nvn_c4.c",
    "nvn_c5.c",
    "gfx_options.c",
    "hitch.c",
    "r0_removed.c",
    "r2_imports.c",
    "b3_imports.c",
    "watch.c",
    "pkz_dump.c",
    "gamedll.c",
    "pcmenu.c",
    "options_row.c",
    "prompts.c",
];
const POSIX_SOURCES: &[&str] = &["plat_posix.c", "plat_audio_sdl.c"];
/// the player's tables, compiled on their PC into game.so: the dump's sdk data, the lifted sdk code, and the values
/// the runtime reads (runtime/split)
const TABLE_SOURCES: &[&str] = &["gen/sdkdata.c", "gen/sdklift_gen.c", "wt_tables.c"];
/// SDL3, OpenGL and FFmpeg (movies): the system's, or in CI the ones a release ships beside the runtime
const LIBS: &[&str] = &[
    "-lSDL3",
    "-lGL",
    "-ldl",
    "-lpthread",
    "-lm",
    "-lavformat",
    "-lavcodec",
    "-lswscale",
    "-lswresample",
    "-lavutil",
];
/// Windows (build_runtime.py's mingw LIBS): SDL3, FFmpeg (movies), and the system's libraries (Media Foundation for the
/// album's clips, waveOut)
const WIN_LIBS: &[&str] = &[
    "-lSDL3",
    "-lavformat",
    "-lavcodec",
    "-lswscale",
    "-lswresample",
    "-lavutil",
    "-luser32",
    "-lgdi32",
    "-lwinmm",
    "-lmfplat",
    "-lmfreadwrite",
    "-lmfuuid",
    "-lole32",
    "-lshlwapi",
    "-lopengl32",
    "-lws2_32",
    "-ladvapi32",
    "-luuid",
];
/// the shader compiler shim's glslang: the system's shared libraries; WT_GLSLANG_LIBS replaces it (CI: a static glslang)
const GLSLANG_LIBS: &str = "-lglslang -lglslang-default-resource-limits";
/// what game.dll exports to witch-time-game.exe (runtime/split, gen/sdkdata.c, gen/sdklift_gen.c, game_shim_eh.c): the
/// functions, then the data. A name the runtime uses and this list lacks fails the runtime's link (CI), not the player's.
const GAME_EXPORTS: (&[&str], &[&str]) = (
    &["game_bind", "game_eh_bind"],
    &[
        "game_cutscene_fps",
        "game_rebind",
        "game_rebind_on",
        "game_rebind_live",
        "sdk_data",
        "sdk_data_count",
        "sdk_fns",
        "sdk_fn_count",
        "sdk_host_names",
        "sdk_host_name_count",
        "wt_layout_FILE_IMAGE",
        "wt_layout_IMAGE_SIZE",
        "wt_layout_GUEST_RO_END",
        "wt_layout_RELA",
        "wt_layout_RELASZ",
        "wt_layout_JMPREL",
        "wt_layout_PLTRELSZ",
        "wt_layout_MAIN_SYMTAB",
        "wt_layout_MAIN_STRTAB",
        "wt_layout_INIT_ARRAY_OFF",
        "wt_layout_INIT_ARRAY_SIZE",
        "wt_layout_DT_INIT_OFF",
        "wt_layout_SDK_IMAGE_SPAN",
        "wt_layout_NNINIT_STARTUP_OFF",
        "wt_layout_NN_MAIN_OFF",
        "wt_layout_GAME_ALIGNED_ALLOC_OFF",
        "wt_layout_GAME_FREE_OFF",
        "wt_layout_GAME_OPDELETE_OFF",
        "wt_sdka_chip_descriptor",
        "wt_sdka_format_count",
        "wt_sdka_format_table",
        "wt_sdka_nvnTextureBuilderGetRawStorageClass",
        "wt_sdka_nvnTextureBuilderGetSparseTileLayout",
        "wt_sdka_nvnTextureBuilderGetStorageAlignment",
        "wt_sdka_nvnTextureBuilderGetStorageClass",
        "wt_sdka_nvnTextureBuilderGetStorageSize",
        "wt_sdka_nvnTextureGetRawStorageClass",
        "wt_sdka_nvnTextureGetSparseTileLayout",
        "wt_sdka_nvnTextureGetStorageClass",
        "wt_sdka_nvnTextureGetStorageSize",
        "wt_sdka_texture_layout",
        "wt_sdka_vt_hal_gm200",
        "wt_sdka_vt_nvgpu_device",
        "wt_nvn_devinfo_count",
        "wt_nvn_devinfo_sdk",
    ],
);
/// the UCRT's import library (mingw-w64's), which game.dll links against on the player's PC
const UCRT_IMPLIB: &str = "libucrtbase.a";
/// what `wtbuild runtime` makes (with the libraries it was linked against copied beside it beforehand); the .o and .a
/// files are inputs of the player's link, not copied into build/
fn runtime_files(t: Target) -> &'static [&'static str] {
    match t {
        Target::Linux => &["libwitchtime.so", "witch-time-game", "libssbu_glslang.so", "game_shim_eh.o"],
        Target::Windows => &["witch-time-game.exe", "ssbu_glslang.dll", "game_shim_eh.o", UCRT_IMPLIB],
    }
}
pub const PREBUILT: &str = "prebuilt";

fn s(p: &Path) -> String {
    p.to_string_lossy().into_owned()
}

fn strs(v: &[&str]) -> Vec<String> {
    v.iter().map(|x| x.to_string()).collect()
}

/// clang flags for code compiled on the player's PC: no system headers, runtime/freestanding instead
pub fn freestanding(repo: &Path) -> Vec<String> {
    vec!["-nostdlibinc".into(), "-isystem".into(), s(&repo.join("runtime").join("freestanding"))]
}

/// sha256 of what the runtime is built from: runtime/ except its generated gen/, and lifter/sdklift_install.txt
pub fn sources_hash(repo: &Path) -> String {
    fn walk(dir: &Path, rel: &str, out: &mut Vec<(String, PathBuf)>) {
        let Ok(rd) = fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            let r = if rel.is_empty() { name.clone() } else { format!("{}/{}", rel, name) };
            let p = e.path();
            if p.is_dir() {
                if r != "gen" {
                    walk(&p, &r, out);
                }
            } else if p.is_file() {
                out.push((r, p));
            }
        }
    }
    let mut files = Vec::new();
    walk(&repo.join("runtime"), "", &mut files);
    files.iter_mut().for_each(|f| f.0 = format!("runtime/{}", f.0));
    files.push(("lifter/sdklift_install.txt".into(), repo.join("lifter").join("sdklift_install.txt")));
    files.sort();
    let mut h = Sha256::new();
    for (r, p) in files {
        h.update(r.as_bytes());
        h.update([0]);
        h.update(Sha256::digest(fs::read(p).unwrap_or_default()));
    }
    crate::util::hex(&h.finalize())
}

struct Clang<'a> {
    path: &'a str,
    t: Target,
    env: &'a (dyn Fn(&mut Command) + Sync),
}

impl Clang<'_> {
    fn run(&self, args: &[String]) -> (Option<i32>, String, f64) {
        let mut c = Command::new(self.path);
        (self.env)(&mut c);
        c.args(self.t.clang_flags());
        c.args(args);
        run(&mut c, None)
    }

    /// `-c src -o obj` for each (src, obj) on default_jobs threads: the failures (name, output), in order
    fn compile_all(&self, jobs: &[(PathBuf, PathBuf)], flags: &[String]) -> Vec<(String, String)> {
        let failed = Mutex::new(Vec::new());
        let next = AtomicUsize::new(0);
        std::thread::scope(|sc| {
            for _ in 0..crate::util::default_jobs() {
                sc.spawn(|| loop {
                    let i = next.fetch_add(1, Ordering::SeqCst);
                    let Some((src, obj)) = jobs.get(i) else { break };
                    let mut a = vec!["-c".to_string()];
                    a.extend(flags.iter().cloned());
                    a.extend([s(src), "-o".into(), s(obj)]);
                    let (code, out, _) = self.run(&a);
                    if code != Some(0) {
                        failed.lock().unwrap().push((i, src.file_name().unwrap().to_string_lossy().into_owned(), out));
                    }
                });
            }
        });
        let mut failed = failed.into_inner().unwrap();
        failed.sort_by_key(|f| f.0);
        failed.into_iter().map(|(_, n, o)| (n, o)).collect()
    }

    /// the compiler's own helper routines (128-bit division, conversions; on Windows the stack probe) for game.so, which
    /// links no C library: clang's compiler-rt builtins, else the libgcc.a it finds; None when it has neither
    fn builtins(&self) -> Option<String> {
        for args in [&["--rtlib=compiler-rt", "-print-libgcc-file-name"][..], &["-print-libgcc-file-name"][..]] {
            let (code, out, _) = self.run(&strs(args));
            let p = out.trim();
            if code == Some(0) && Path::new(p).is_absolute() && Path::new(p).is_file() {
                return Some(p.to_string());
            }
        }
        None
    }
}

/// Builds the runtime into `out` (objects in `objdir`): libwitchtime.so (the whole host runtime, needing game.so),
/// witch-time-game (a program that runs it), libssbu_glslang.so and game_shim_eh.o (linked into game.so later).
/// Windows: witch-time-game.exe (the whole host runtime, importing game.dll's tables), ssbu_glslang.dll, game_shim_eh.o
/// and the UCRT import library game.dll links against. `install_inc` holds sdklift_install.h.
pub fn build_runtime(
    repo: &Path,
    clang: &str,
    t: Target,
    env: &(dyn Fn(&mut Command) + Sync),
    out: &Path,
    objdir: &Path,
    install_inc: &Path,
    log: &mut dyn FnMut(String),
) -> Result<(), String> {
    let cl = Clang { path: clang, t, env };
    let rt = repo.join("runtime");
    let stub = objdir.join("stub");
    fs::create_dir_all(&stub).map_err(|e| e.to_string())?;
    fs::create_dir_all(out).map_err(|e| e.to_string())?;

    // 1. the host runtime, with runtime/split in place of the player's tables
    let mut flags = Vec::new();
    if !t.windows() {
        flags.extend(["-I".to_string(), s(&rt.join("posix"))]);
    }
    flags.extend(strs(&["-O1", "-g", "-Wno-everything", "-fno-strict-aliasing", "-mcx16"]));
    if t.windows() {
        flags.push("-gcodeview".into()); // debug information in a PDB beside the program (crash addresses -> source)
    }
    for d in [rt.join("split"), rt.clone(), rt.join("gen_static"), install_inc.to_path_buf()] {
        flags.extend(["-I".into(), s(&d)]);
    }
    flags.extend(strs(&[
        "-Dfn_table=(*game_fn_table_p)",
        "-Dfn_table_count=(*game_fn_table_count_p)",
        "-Ddyn_syms=(*game_dyn_syms_p)",
        "-Ddyn_sym_count=(*game_dyn_sym_count_p)",
    ]));
    if t.windows() {
        // build_runtime.py's mingw flags: mingw-w64 defaults to Windows 7; every thread-local must stay one (finding 100)
        flags.extend(strs(&["-D_WIN32_WINNT=0x0A00", "-Werror=unknown-attributes"]));
    } else {
        flags.extend(strs(&["-D_GNU_SOURCE", "-fPIC", "-D__declspec(x)=", "-D__stdcall=", "-D__cdecl="]));
    }
    flags.extend(strs(&[
        "-Werror=implicit-function-declaration",
        "-Werror=incompatible-function-pointer-types",
        "-Werror=int-conversion",
    ]));
    let t0 = Instant::now();
    let sources: Vec<&str> = HOST_SOURCES.iter().chain(if t.windows() { &[][..] } else { POSIX_SOURCES }).cloned().collect();
    let jobs: Vec<(PathBuf, PathBuf)> = sources
        .iter()
        .map(|n| (rt.join(n), objdir.join(format!("{}.obj", Path::new(n).file_stem().unwrap().to_string_lossy()))))
        .collect();
    let failed = cl.compile_all(&jobs, &flags);
    log(format!(
        "host: {} files compiled in {:.0}s, {} failed",
        sources.len(),
        t0.elapsed().as_secs_f64(),
        failed.len()
    ));
    for (name, o) in &failed {
        log(format!("==== {}\n{}", name, tail(o, 2500)));
    }
    if !failed.is_empty() {
        return Err("the host runtime did not compile".into());
    }

    // 2. game.so's own part of the runtime: the calls back into the host, the landing-pad resume, host versions of
    // game functions (on Windows also game.dll's entry point: it is linked without the C runtime)
    let mut a = strs(&["-c", "-O1", "-w", "-mcx16"]);
    a.extend(if t.windows() {
        strs(&["-DWT_GAME_NOSTDLIB"])
    } else {
        strs(&["-fPIC", "-D__declspec(x)="])
    });
    a.extend(["-I".into(), s(&rt), s(&rt.join("game_shim_eh.c")), "-o".into(), s(&out.join("game_shim_eh.o"))]);
    let (code, o, _) = cl.run(&a);
    if code != Some(0) {
        log(tail(&o, 3000).to_string());
        return Err("game_shim_eh.c did not compile".into());
    }

    if t.windows() {
        return build_runtime_windows(repo, &cl, &jobs, out, objdir, log);
    }

    // 3. libwitchtime.so, needing game.so (beside it, rpath $ORIGIN), linked against an empty one of that name: the
    // values from the dump are only looked up when the game starts
    let (code, o, _) = cl.run(&[
        "-shared".into(),
        "-nostdlib".into(),
        "-fuse-ld=lld".into(),
        "-x".into(),
        "c".into(),
        "/dev/null".into(),
        "-Xlinker".into(),
        "-soname=game.so".into(),
        "-o".into(),
        s(&stub.join("game.so")),
    ]);
    if code != Some(0) {
        log(tail(&o, 3000).to_string());
        return Err("the stand-in game.so did not link".into());
    }
    let host = out.join("libwitchtime.so");
    let mut a = strs(&["-shared", "-fuse-ld=lld", "-g", "-Xlinker", "--no-dependent-libraries"]);
    a.extend(jobs.iter().map(|j| s(&j.1)));
    a.extend(["-Xlinker".into(), "--no-as-needed".into(), s(&stub.join("game.so"))]);
    a.extend(strs(&["-Xlinker", "-rpath=$ORIGIN", "-Xlinker", "-soname=libwitchtime.so"]));
    a.extend(strs(LIBS));
    a.extend(["-o".into(), s(&host)]);
    let (code, o, _) = cl.run(&a);
    log(format!("libwitchtime.so: link exit {}", code.unwrap_or(-1)));
    if code != Some(0) {
        log(tail(&o, 6000).to_string());
        return Err("libwitchtime.so did not link".into());
    }

    // 4. witch-time-game: the same runtime as a program of its own (the launcher starts it when it cannot load the
    // library); what libwitchtime.so needs from game.so is resolved when it runs
    let exe = out.join("witch-time-game");
    let mut a = strs(&["-fuse-ld=lld", "-g"]);
    a.extend([s(&rt.join("wt_main.c")), s(&host)]);
    a.extend(strs(&["-Xlinker", "-rpath=$ORIGIN", "-Xlinker", "--allow-shlib-undefined", "-o"]));
    a.push(s(&exe));
    let (code, o, _) = cl.run(&a);
    log(format!("witch-time-game: link exit {}", code.unwrap_or(-1)));
    if code != Some(0) {
        log(tail(&o, 3000).to_string());
        return Err("witch-time-game did not link".into());
    }

    // 5. the shader compiler shim; LoadLibraryA("ssbu_glslang.dll") opens it beside the program
    let so = out.join("libssbu_glslang.so");
    let glslang = std::env::var("WT_GLSLANG_LIBS").unwrap_or_else(|_| GLSLANG_LIBS.into());
    let mut a = strs(&["-shared", "-fPIC", "-O2", "-w", "-D_GNU_SOURCE", "-D__declspec(x)=", "-I"]);
    a.extend([s(&rt.join("posix")), "-I".into(), s(&rt), s(&rt.join("vk_glslang_shim.c"))]);
    a.extend(glslang.split_whitespace().map(String::from));
    a.extend(["-o".into(), s(&so)]);
    let (code, o, _) = cl.run(&a);
    log(format!("libssbu_glslang.so: exit {}", code.unwrap_or(-1)));
    if code != Some(0) {
        log(tail(&o, 3000).to_string());
        return Err("libssbu_glslang.so did not build".into());
    }
    Ok(())
}

/// game.def: game.dll's exports (GAME_EXPORTS)
fn game_def() -> String {
    let mut d = String::from("LIBRARY game.dll\nEXPORTS\n");
    for f in GAME_EXPORTS.0 {
        d += &format!("    {}\n", f);
    }
    for v in GAME_EXPORTS.1 {
        d += &format!("    {} DATA\n", v);
    }
    d
}

/// steps 3 to 5 of build_runtime for Windows
fn build_runtime_windows(repo: &Path, cl: &Clang, jobs: &[(PathBuf, PathBuf)], out: &Path, objdir: &Path, log: &mut dyn FnMut(String)) -> Result<(), String> {
    let rt = repo.join("runtime");
    // 3. game.dll's import library, from game.def (llvm-dlltool beside clang): the program imports its tables
    let def = objdir.join("game.def");
    fs::write(&def, game_def()).map_err(|e| e.to_string())?;
    let implib = objdir.join("libgame.dll.a");
    let dlltool = Path::new(cl.path).with_file_name(crate::target::exe("llvm-dlltool"));
    let mut c = Command::new(&dlltool);
    c.args(["-m", "i386:x86-64", "-D", "game.dll", "-d"]).arg(&def).arg("-l").arg(&implib);
    let (code, o, _) = run(&mut c, None);
    if code != Some(0) {
        log(tail(&o, 3000).to_string());
        return Err(format!("{} could not make game.dll's import library", dlltool.display()));
    }

    // 4. witch-time-game.exe: the whole runtime (build_runtime.py's program), with its debug information in a PDB and
    // build_runtime.py's fixed base (crash addresses as the logs print them), and its manifest (runtime/win32: UTF-8 as
    // its code page, for paths with any letters in them)
    let res = objdir.join("witch-time-game.res");
    let rc = Path::new(cl.path).with_file_name(crate::target::exe("llvm-rc"));
    let mut c = Command::new(&rc);
    c.arg("-FO").arg(&res).arg(rt.join("win32").join("witch-time-game.rc"));
    let (code, o, _) = run(&mut c, None);
    if code != Some(0) {
        log(tail(&o, 3000).to_string());
        return Err(format!("{} could not compile the program's resources", rc.display()));
    }
    let exe = out.join("witch-time-game.exe");
    let mut a = strs(&["-fuse-ld=lld", "-g"]);
    a.extend(jobs.iter().map(|j| s(&j.1)));
    a.push(s(&implib));
    a.push(s(&res));
    a.extend(strs(WIN_LIBS));
    a.extend(["-Xlinker".into(), format!("--pdb={}", s(&exe.with_extension("pdb")))]);
    a.extend(strs(&["-Wl,--disable-dynamicbase", "-o"]));
    a.push(s(&exe));
    let (code, o, _) = cl.run(&a);
    log(format!("witch-time-game.exe: link exit {}", code.unwrap_or(-1)));
    if code != Some(0) {
        log(tail(&o, 6000).to_string());
        return Err("witch-time-game.exe did not link".into());
    }

    // 5. the shader compiler shim, which the program loads beside it; WT_GLSLANG_LIBS: a static glslang (CI)
    let dll = out.join("ssbu_glslang.dll");
    let glslang = std::env::var("WT_GLSLANG_LIBS").unwrap_or_else(|_| GLSLANG_LIBS.into());
    let mut a = strs(&["-shared", "-O2", "-w", "-I"]);
    a.extend([s(&rt), s(&rt.join("vk_glslang_shim.c"))]);
    a.extend(glslang.split_whitespace().map(String::from));
    a.extend(["-o".into(), s(&dll)]);
    let (code, o, _) = cl.run(&a);
    log(format!("ssbu_glslang.dll: exit {}", code.unwrap_or(-1)));
    if code != Some(0) {
        log(tail(&o, 3000).to_string());
        return Err("ssbu_glslang.dll did not build".into());
    }

    // 6. the UCRT's import library, as this compiler's mingw-w64 has it, for game.dll
    let (code, o, _) = cl.run(&[format!("-print-file-name={}", UCRT_IMPLIB)]);
    let ucrt = PathBuf::from(o.trim());
    if code != Some(0) || !ucrt.is_absolute() || !ucrt.is_file() {
        return Err(format!(
            "{} not found by {} (a mingw-w64 toolchain such as llvm-mingw builds the runtime)",
            UCRT_IMPLIB, cl.path
        ));
    }
    fs::copy(&ucrt, out.join(UCRT_IMPLIB)).map_err(|e| format!("{}: {}", ucrt.display(), e))?;
    Ok(())
}

/// game.so: the lifted objects, game_shim_eh.o and the player's tables, linked without the C library (what they call
/// in it, and in the runtime, is found when the game starts). game.dll: the same, exporting its tables (game.def) and
/// importing the C functions from the UCRT (`implibs`)
fn link_game(
    repo: &Path,
    clang: &str,
    t: Target,
    env: &(dyn Fn(&mut Command) + Sync),
    shim: &Path,
    implibs: &[PathBuf],
    log: &mut dyn FnMut(String),
) -> Result<(), String> {
    let cl = Clang { path: clang, t, env };
    let rt = repo.join("runtime");
    let build = repo.join("build");
    let (obj, objt) = (build.join("obj"), build.join("obj_tables"));
    fs::create_dir_all(&objt).map_err(|e| e.to_string())?;

    let mut flags = strs(&["-O1", "-g", "-Wno-everything", "-fno-strict-aliasing", "-mcx16"]);
    if !t.windows() {
        flags.extend(strs(&["-fPIC", "-D__declspec(x)="]));
    }
    flags.extend(freestanding(repo));
    flags.extend(["-I".into(), s(&rt), "-I".into(), s(&rt.join("gen"))]);
    let jobs: Vec<(PathBuf, PathBuf)> = TABLE_SOURCES
        .iter()
        .map(|n| (rt.join(n), objt.join(format!("{}.obj", Path::new(n).file_stem().unwrap().to_string_lossy()))))
        .collect();
    let t0 = Instant::now();
    let failed = cl.compile_all(&jobs, &flags);
    log(format!(
        "tables: {} files compiled in {:.0}s, {} failed",
        jobs.len(),
        t0.elapsed().as_secs_f64(),
        failed.len()
    ));
    for (name, o) in &failed {
        log(format!("==== {}\n{}", name, tail(o, 2500)));
    }
    if !failed.is_empty() {
        return Err("the player's tables did not compile".into());
    }

    let mut objs: Vec<String> = fs::read_dir(&obj)
        .map_err(|e| e.to_string())?
        .flatten()
        .map(|e| s(&e.path()))
        .filter(|p| p.ends_with(".obj"))
        .collect();
    objs.sort();
    let rsp = build.join("game_dll.rsp");
    fs::write(&rsp, objs.iter().map(|o| format!("\"{}\"\n", o.replace('\\', "/"))).collect::<String>()).map_err(|e| e.to_string())?;
    let name = t.game_lib();
    let dll = build.join(name);
    let t0 = Instant::now();
    let mut a = strs(&["-shared", "-nostdlib", "-fuse-ld=lld"]);
    if !t.windows() {
        a.extend(strs(&["-Xlinker", "-soname=game.so"]));
    }
    a.push(s(shim));
    a.extend(jobs.iter().map(|j| s(&j.1)));
    a.push(format!("@{}", s(&rsp)));
    if t.windows() {
        let def = build.join("game.def");
        fs::write(&def, game_def()).map_err(|e| e.to_string())?;
        a.push(s(&def));
        a.extend(implibs.iter().map(|p| s(p)));
    }
    let builtins = cl.builtins();
    a.extend(builtins.iter().cloned());
    a.extend(["-o".into(), s(&dll)]);
    let (code, out, _) = cl.run(&a);
    log(format!(
        "{}: exit {} in {:.0}s, {} objects, builtins {}",
        name,
        code.unwrap_or(-1),
        t0.elapsed().as_secs_f64(),
        objs.len(),
        builtins.as_deref().unwrap_or("(none found)")
    ));
    if code != Some(0) {
        log(tail(&out, 4000).to_string());
        return Err(format!("{} did not link", name));
    }
    Ok(())
}

/// prebuilt/manifest.json of a release, when its runtime was built from these sources: (folder, its files)
fn prebuilt(repo: &Path, t: Target, log: &mut dyn FnMut(String)) -> Option<(PathBuf, Vec<String>)> {
    let dir = repo.join(PREBUILT);
    let m: Value = serde_json::from_slice(&fs::read(dir.join("manifest.json")).ok()?).ok()?;
    let files: Vec<String> = m.get("files")?.as_array()?.iter().filter_map(|v| v.as_str().map(String::from)).collect();
    if m.get("sources").and_then(|v| v.as_str()) != Some(sources_hash(repo).as_str()) {
        log(format!("{}/ was built from other runtime sources: the runtime is built here", PREBUILT));
        return None;
    }
    let missing = (runtime_files(t).iter().map(|f| f.to_string()).filter(|f| !files.contains(f)))
        .chain(files.iter().filter(|f| !dir.join(f).is_file()).cloned())
        .next();
    if let Some(f) = missing {
        log(format!("{}/ is incomplete (no {}): the runtime is built here", PREBUILT, f));
        return None;
    }
    Some((dir, files))
}

/// `wtbuild runtime --out <dir>` (CI): the runtime of a release, and its manifest.json. The libraries it is linked
/// against and ships with (FFmpeg) are copied into <dir> beforehand and listed too.
pub fn make_prebuilt(repo: &Path, clang: &str, t: Target, out: &Path, log: &mut dyn FnMut(String)) -> Result<(), String> {
    let objdir = repo.join("build").join(if t == Target::host() {
        "obj_runtime".to_string()
    } else {
        format!("obj_runtime_{}", t.name())
    });
    let inc = objdir.join("inc");
    fs::create_dir_all(&inc).map_err(|e| e.to_string())?;
    fs::write(inc.join("sdklift_install.h"), crate::tables::install_header(repo)?).map_err(|e| e.to_string())?;
    build_runtime(repo, clang, t, &|_| {}, out, &objdir, &inc, log)?;
    let mut files: Vec<String> = fs::read_dir(out)
        .map_err(|e| e.to_string())?
        .flatten()
        .filter(|e| e.path().is_file())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n != "manifest.json")
        .collect();
    files.sort();
    let ver = Command::new(clang)
        .arg("--version")
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).lines().next().unwrap_or("").to_string())
        .unwrap_or_default();
    let m = json!({"sources": sources_hash(repo), "clang": ver, "target": t.name(), "files": files});
    fs::write(out.join("manifest.json"), serde_json::to_string_pretty(&m).unwrap() + "\n").map_err(|e| e.to_string())?;
    log(format!(
        "{}: {} files, runtime sources {}",
        out.display(),
        files.len(),
        &sources_hash(repo)[..16]
    ));
    Ok(())
}

/// copy through a new file and a rename: a running game keeps the file it has open
fn install(from: &Path, to: &Path) -> std::io::Result<()> {
    let tmp = to.with_extension("new");
    fs::copy(from, &tmp)?;
    fs::rename(&tmp, to)
}

/// an input of the player's link (game_shim_eh.o, the UCRT's import library), not a file of the runtime
fn link_input(f: &str) -> bool {
    f.ends_with(".o") || f.ends_with(".a")
}

pub fn run_step(repo: &Path, clang: &str, t: Target, env: &(dyn Fn(&mut Command) + Sync), log: &mut dyn FnMut(String)) -> Result<(), String> {
    let build = repo.join("build");
    let (shim, implibs) = match prebuilt(repo, t, log) {
        Some((dir, files)) => {
            for f in files.iter().filter(|f| !link_input(f)) {
                install(&dir.join(f), &build.join(f)).map_err(|e| format!("{}: {}", f, e))?;
            }
            log(format!("runtime: prebuilt, {} files from {}/", files.len(), PREBUILT));
            (dir.join("game_shim_eh.o"), vec![dir.join(UCRT_IMPLIB)])
        }
        None => {
            let gen = repo.join("runtime").join("gen");
            build_runtime(repo, clang, t, env, &build, &build.join("obj_host"), &gen, log)?;
            (build.join("game_shim_eh.o"), vec![build.join(UCRT_IMPLIB)])
        }
    };
    link_game(repo, clang, t, env, &shim, &implibs, log)?;
    let mb = |f: &str| fs::metadata(build.join(f)).map(|m| m.len()).unwrap_or(0) as f64 / (1 << 20) as f64;
    let host = if t.windows() { "witch-time-game.exe" } else { "libwitchtime.so" };
    log(format!(
        "{}: {:.1} MB, {} {:.0} MB (TOOLCHAIN={})",
        s(&build.join(host)),
        mb(host),
        t.game_lib(),
        mb(t.game_lib()),
        t.name()
    ));
    Ok(())
}

/// the prebuilt runtime is there and current (the deps step then checks the libraries it needs, not development files)
pub fn has_prebuilt(repo: &Path) -> bool {
    prebuilt(repo, Target::host(), &mut |_| {}).is_some()
}

/// libraries the prebuilt runtime needs that this system does not have (game.so is made later). Linux: the sonames ldd
/// does not find. Windows: the DLLs witch-time-game.exe imports that are neither beside it (build/, prebuilt/) nor in
/// System32 (the Media Foundation DLLs are missing from Windows N editions without the Media Feature Pack).
pub fn prebuilt_missing(repo: &Path) -> Vec<String> {
    if cfg!(windows) {
        let exe = repo.join(PREBUILT).join("witch-time-game.exe");
        let sys = PathBuf::from(std::env::var_os("SystemRoot").unwrap_or_else(|| "C:\\Windows".into())).join("System32");
        return pe_imports(&fs::read(exe).unwrap_or_default())
            .into_iter()
            .filter(|d| {
                let l = d.to_ascii_lowercase();
                !(l == "game.dll" || l.starts_with("api-ms-win-") || l.starts_with("ext-ms-"))
            })
            .filter(|d| ![repo.join("build"), repo.join(PREBUILT), sys.clone()].iter().any(|dir| dir.join(d).is_file()))
            .collect();
    }
    let mut c = Command::new("ldd");
    c.arg(repo.join(PREBUILT).join("libwitchtime.so")).env("LD_LIBRARY_PATH", repo.join("build"));
    let Ok(o) = c.output() else { return vec![] };
    String::from_utf8_lossy(&o.stdout)
        .lines()
        .filter(|l| l.contains("=> not found"))
        .filter_map(|l| l.split_whitespace().next().map(String::from))
        .filter(|n| n != "game.so")
        .collect()
}

/// the DLL names a PE32+ program imports (its import directory); empty when it is not one
pub fn pe_imports(b: &[u8]) -> Vec<String> {
    use crate::util::{u16_at, u32_at};
    let mut out = Vec::new();
    let Some(pe) = b.get(0x3c..0x40).map(|_| u32_at(b, 0x3c) as usize) else {
        return out;
    };
    if b.get(pe..pe + 4) != Some(b"PE\0\0") || b.len() < pe + 24 + 0x80 || u16_at(b, pe + 24) != 0x20b {
        return out;
    }
    let (nsec, opt_size) = (u16_at(b, pe + 6) as usize, u16_at(b, pe + 20) as usize);
    let import_rva = u32_at(b, pe + 24 + 0x78) as usize; // data directory 1
    let sections = pe + 24 + opt_size;
    let file_off = |rva: usize| -> Option<usize> {
        (0..nsec).find_map(|i| {
            let h = sections + i * 40;
            let (vsize, va, raw) = (u32_at(b, h + 8) as usize, u32_at(b, h + 12) as usize, u32_at(b, h + 20) as usize);
            (rva >= va && rva < va + vsize.max(1)).then_some(raw + rva - va)
        })
    };
    let Some(mut d) = file_off(import_rva) else { return out };
    while d + 20 <= b.len() && u32_at(b, d + 12) != 0 {
        if let Some(n) = file_off(u32_at(b, d + 12) as usize).filter(|&n| n < b.len()) {
            out.push(crate::util::cstr(b, n));
        }
        d += 20;
    }
    out
}
