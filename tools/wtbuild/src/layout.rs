//! [witchtime] where a program's files go (issue 115). Program 0 is Bayonetta 3, as always: targets/, build/, runtime/gen/.
//! Program N of the application (1 = the Old Picture Book's story, the Bayonetta Origins prologue, which the game starts
//! with nn::oe::ExecuteProgram): targets/programN/, build/programN/, build/programN/gen/. The runtime sources, lifter/
//! and prebuilt/ are the checkout's for every program.
//!
//! shortcut: one program per wtbuild process at a time (set_program before the steps); pass it down if two must build
//! at once

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

static PROGRAM: AtomicU32 = AtomicU32::new(0);

pub fn set_program(p: u32) {
    PROGRAM.store(p, Ordering::Relaxed)
}

pub fn program() -> u32 {
    PROGRAM.load(Ordering::Relaxed)
}

/// "" for program 0, else "programN"
pub fn sub() -> String {
    sub_of(program())
}

fn sub_of(p: u32) -> String {
    if p == 0 {
        String::new()
    } else {
        format!("program{}", p)
    }
}

/// (targets, build, gen) of program p
pub fn folders(repo: &Path, p: u32) -> (PathBuf, PathBuf, PathBuf) {
    if p == 0 {
        return (repo.join("targets"), repo.join("build"), repo.join("runtime").join("gen"));
    }
    let b = repo.join("build").join(sub_of(p));
    (repo.join("targets").join(sub_of(p)), b.clone(), b.join("gen"))
}

pub fn targets(repo: &Path) -> PathBuf {
    folders(repo, program()).0
}

pub fn build(repo: &Path) -> PathBuf {
    folders(repo, program()).1
}

/// the generated tables (runtime/gen for program 0: the folder every older build used)
pub fn gen(repo: &Path) -> PathBuf {
    folders(repo, program()).2
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn program_folders() {
        let r = Path::new("/w");
        assert_eq!(folders(r, 0), ("/w/targets".into(), "/w/build".into(), "/w/runtime/gen".into()));
        assert_eq!(
            folders(r, 1),
            ("/w/targets/program1".into(), "/w/build/program1".into(), "/w/build/program1/gen".into())
        );
    }
}
