# runtime/gen_static

Generated inputs of the runtime that do not depend on the dump: the binary128 arithmetic vectors (`quad_tests.h`),
the exception-table vectors (`eh_tests.h`: empty, main has no LSDA), the deflate and JPEG
self-test vectors (`sdktest_data.h`) and the empty strict-mode audit lists (`fake_whitelist.h`, `fake_openlist.h`).

They are committed and never regenerated: the `tables` step of `wtbuild` (`tools/wtbuild/src/tables.rs`) copies them
into `runtime/gen/`, and `cargo test` in `tools/wtbuild` checks that the copies are byte for byte these files. They
were written once by the Python tables step, `runtime/gen/make_gen.py` (removed 2026-11-02; Pillow made the JPEG
vectors), the way ssbu-recomp's generators at commit `3a96916` make them: `quad_tests.h` is the output of its
`tools/lifter/gen_quad_tests.py` (seed 20261008), `eh_tests.h` and `sdktest_data.h` follow its `gen_eh_tests.py` and
`gen_sdktest.py` (the latter without `nn::mii`). The first line of each names the script that wrote it.
