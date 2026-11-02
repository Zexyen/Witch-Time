fn main() {
    // the static Unicorn's 16-byte compare-and-swap (QEMU's cputlb.c) needs libatomic, which unicorn-engine-sys does not
    // link; a link-arg goes after the libraries, as a linker with --as-needed requires
    println!("cargo:rustc-link-arg=-latomic");
}
