# runtime/gen_static

Generated inputs of the runtime that do not depend on the dump: the binary128 arithmetic vectors (`quad_tests.h`,
`lifter/gen_quad_tests.py`), the deflate and JPEG
self-test vectors (`sdktest_data.h`) and the empty strict-mode audit lists (`fake_whitelist.h`, `fake_openlist.h`).

They are exactly what `runtime/gen/make_gen.py` writes (it needs Pillow for the JPEG vectors), made once and kept here,
so the `tables` step of `wtbuild` copies them into `runtime/gen/` and the player needs no Python.
`tools/wtbuild/compare_tables.py` checks that they still match what make_gen.py writes.
