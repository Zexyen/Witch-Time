//! What the build makes differs between Linux, Windows and macOS: the file names, the compiler's target and the runtime's
//! libraries. The player's build is always for the system it runs on; `wtbuild compile / link / runtime --target windows`
//! makes the Windows files elsewhere too (CI builds the Windows runtime on Linux with llvm-mingw). macOS (Apple silicon)
//! is built on a Mac only, with Apple's clang and linker.

use std::fs;
use std::path::Path;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Target {
    Linux,
    Windows,
    MacOS,
}

impl Target {
    pub fn host() -> Target {
        if cfg!(windows) {
            Target::Windows
        } else if cfg!(target_os = "macos") {
            Target::MacOS
        } else {
            Target::Linux
        }
    }

    /// `--target linux|windows|macos` among the arguments, else the host
    pub fn from_args(args: &[String]) -> Result<Target, String> {
        match args.iter().position(|a| a == "--target").map(|i| args.get(i + 1).map(String::as_str)) {
            None => Ok(Target::host()),
            Some(Some("linux")) => Ok(Target::Linux),
            Some(Some("windows")) => Ok(Target::Windows),
            Some(Some("macos")) => Ok(Target::MacOS),
            Some(v) => Err(format!("--target {}: linux, windows or macos", v.unwrap_or(""))),
        }
    }

    pub fn windows(self) -> bool {
        self == Target::Windows
    }

    pub fn macos(self) -> bool {
        self == Target::MacOS
    }

    /// TOOLCHAIN in build/setup_state.json, the log lines and the compile step's configuration hash
    pub fn name(self) -> &'static str {
        match self {
            Target::Linux => "linux",
            Target::Windows => "windows",
            Target::MacOS => "macos",
        }
    }

    /// the game's own library, linked on the player's PC
    pub fn game_lib(self) -> &'static str {
        match self {
            Target::Linux => "game.so",
            Target::Windows => "game.dll",
            Target::MacOS => "game.dylib",
        }
    }

    /// the program that runs the game
    pub fn game_exe(self) -> &'static str {
        match self {
            Target::Linux | Target::MacOS => "witch-time-game",
            Target::Windows => "witch-time-game.exe",
        }
    }

    /// clang flags that pick the target. Windows: the MinGW ABI the runtime is built with, for any clang (the bundled one,
    /// LLVM's own release, would make MSVC objects; llvm-mingw's makes these anyway)
    pub fn clang_flags(self) -> Vec<String> {
        match self {
            Target::Linux | Target::MacOS => vec![],
            Target::Windows => vec!["--target=x86_64-w64-mingw32".into()],
        }
    }

    /// code that goes into a shared library: position-independent on ELF and Mach-O; PE has no such flag (clang refuses it)
    pub fn pic(self) -> Vec<String> {
        match self {
            Target::Linux | Target::MacOS => vec!["-fPIC".into()],
            Target::Windows => vec![],
        }
    }

    /// flags of the processor the build is for: the 16-byte compare-and-swap on x86-64 (clang refuses the flag for
    /// Apple silicon, which has the instruction pair anyway)
    pub fn cpu(self) -> Vec<String> {
        match self {
            Target::Linux | Target::Windows => vec!["-mcx16".into()],
            Target::MacOS => vec![],
        }
    }

    /// the linker: lld, which a release brings with its compiler; on macOS Apple's own, which every clang there uses
    pub fn linker(self) -> Vec<String> {
        match self {
            Target::Linux | Target::Windows => vec!["-fuse-ld=lld".into()],
            Target::MacOS => vec![],
        }
    }

    /// where the runtime's libraries are when the compiler does not look there by itself: Homebrew's folders on macOS
    /// (`-I<prefix>/include -L<prefix>/lib`; HOMEBREW_PREFIX, else /opt/homebrew), nothing elsewhere
    pub fn lib_dirs(self) -> Vec<String> {
        if !self.macos() {
            return vec![];
        }
        let prefix = std::env::var("HOMEBREW_PREFIX").ok().filter(|p| !p.is_empty()).unwrap_or_else(|| "/opt/homebrew".into());
        if !Path::new(&prefix).join("include").is_dir() {
            return vec![];
        }
        vec![format!("-I{}/include", prefix), format!("-L{}/lib", prefix)]
    }
}

/// the compiler's name: clang, clang.exe
pub fn exe(name: &str) -> String {
    format!("{}{}", name, std::env::consts::EXE_SUFFIX)
}

/// a file's modification time in nanoseconds since 1970, as Python's st_mtime_ns
pub fn mtime_ns(m: &fs::Metadata) -> i128 {
    match m.modified().map(|t| t.duration_since(std::time::UNIX_EPOCH)) {
        Ok(Ok(d)) => d.as_nanos() as i128,
        Ok(Err(e)) => -(e.duration().as_nanos() as i128),
        Err(_) => 0,
    }
}

/// a program file this system can run (shutil.which's test)
pub fn is_program(p: &Path) -> bool {
    let Ok(m) = fs::metadata(p) else { return false };
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        m.is_file() && m.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        m.is_file()
    }
}

/// removes a symbolic link or junction to a folder (older setups linked targets/romfs), never what it points to
pub fn remove_dir_link(link: &Path) -> std::io::Result<()> {
    if cfg!(windows) {
        fs::remove_dir(link) // a junction (or a directory symbolic link) is removed as an empty folder
    } else {
        fs::remove_file(link)
    }
}
