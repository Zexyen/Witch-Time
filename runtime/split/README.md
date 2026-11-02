# runtime/split

`image_layout.h`, `nvn_devinfo.h` and `nvn_sdk_addrs.h` for the runtime built apart from the player's dump. The
generated headers of the same names in `runtime/gen/` hold numbers read from the player's `main.elf` and `sdk.elf`
(addresses, table sizes, the sdk's fixed NVN answers); these declare the same names as `extern const` variables
instead. `runtime/wt_tables.c` defines the variables from the generated headers and is compiled into `game.so` on the
player's PC, with `gen/sdkdata.c` and `gen/sdklift_gen.c`.

So nothing in `libwitchtime.so` / `witch-time-game` depends on the dump: a release builds them once in CI and ships
them (`prebuilt/`, `wtbuild runtime`), and the player's PC compiles only the lifted code and those three files. Every
use of these names in the runtime is an ordinary expression (none is in `#if`, a `case` label or a static
initializer), which is what makes the swap possible; keep it that way when adding one. `wtbuild link` builds the
runtime with `-I runtime/split` ahead of everything else, also when it builds it locally.
