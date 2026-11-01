// `wtbuild textures` decodes ASTC with the runtime's own decoder, so its texels are the ones the game would have made.
fn main() {
    println!("cargo:rerun-if-changed=../../runtime/tex_decode.c");
    println!("cargo:rerun-if-changed=../../runtime/tex_decode.h");
    println!("cargo:rerun-if-changed=src/tex_shim.c");
    cc::Build::new()
        .file("../../runtime/tex_decode.c")
        .file("src/tex_shim.c")
        .include("../../runtime")
        .warnings(false)
        .compile("texdec");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux") {
        println!("cargo:rustc-link-lib=stdc++"); // intel_tex_2's Linux archive refers to the C++ unwinder
    }
}
