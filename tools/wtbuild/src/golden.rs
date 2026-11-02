//! Golden files of the regression tests, in testdata/golden: `WTBUILD_BLESS=1 cargo test --release` rewrites them.

use std::path::{Path, PathBuf};

pub fn path(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("testdata/golden").join(name)
}

/// panics unless `actual` equals the golden file `name`; on a difference the new output is left in the temporary folder for a diff
pub fn check(name: &str, actual: &str) {
    let p = path(name);
    if std::env::var_os("WTBUILD_BLESS").is_some() {
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(&p, actual).unwrap();
        return;
    }
    let want = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("{}: {} (WTBUILD_BLESS=1 cargo test --release writes it)", p.display(), e));
    if want != actual {
        let got = std::env::temp_dir().join(format!("wtbuild-golden-{}-{}", std::process::id(), name.replace(['/', '\\'], "_")));
        std::fs::write(&got, actual).unwrap();
        // the first line that differs, for a CI log nobody can diff
        let (w, a): (Vec<&str>, Vec<&str>) = (want.lines().collect(), actual.lines().collect());
        let n = (0..w.len().max(a.len())).find(|&i| w.get(i) != a.get(i)).unwrap_or(0);
        panic!(
            "{} differs from {} from line {}:\n  want {:?}\n  got  {:?}\ncompare the two; WTBUILD_BLESS=1 cargo test --release accepts the new output",
            got.display(),
            p.display(),
            n + 1,
            w.get(n).unwrap_or(&"<end>"),
            a.get(n).unwrap_or(&"<end>")
        );
    }
}
