# runtime/gen_static

Generated inputs of the runtime that do not depend on the dump: the empty strict-mode audit lists (`fake_whitelist.h`,
`fake_openlist.h`).

They are committed and never regenerated: the `tables` step of `wtbuild` (`tools/wtbuild/src/tables.rs`) copies them
into `runtime/gen/`, and `cargo test` in `tools/wtbuild` checks that the copies are byte for byte these files. They
were written once by the Python tables step, `runtime/gen/make_gen.py` (removed 2026-11-02).
