# The lifter (GPL-3.0, from ssbu-recomp)

The lifter lives in `tools/wtbuild/src/lifter` (`mod.rs`: one instruction to C; `eh.rs`: the whole-program lift with
landing-pad resume entries; `fps.rs`: the frame-counter sites, new). It is a Rust port of ssbu-recomp's `recomp.py` and
`recomp_eh.py`, copied from https://github.com/Denkishi/ssbu-recomp `tools/lifter/` at commit `3a96916` (2026-10-16)
through B2recomp, with the changes below; it writes byte for byte the C those wrote. The Python copies were removed from
this folder on 2026-11-02. License: GPL-3.0 (`LICENSE`).

This folder now holds the lifter's inputs and its licence: `sdklift_install.txt` (imports whose host handler is
replaced by SDK code lifted from the player's sdk), `sdklift_roots.txt` (the sdk functions to lift) and `LICENSE`.
`wtbuild setup` runs the lift (step `lift`); `wtbuild lift <main.elf> <outdir>` runs it on its own.

The change list below is the record of what was changed from upstream, with the names the files had then.

Changes from upstream:
- `recomp.py`: `ELF` points at `../targets/exefs/main.elf`; `TEXT_SIZE`, `RELA/RELASZ`, `JMPREL/PLTRELSZ`,
  `SYMTAB/STRTAB` set from our main.elf's DYNAMIC segment; added `import os`.
- `recomp_eh.py`: unchanged.

Further changes (2026-10-15):
- `recomp.py`: exclusive pairs (`ldxp/ldaxp/stxp/stlxp`), `{s,u}{mull,mlal,mlsl}{2}`, `{s,u}qxtn{2}`,
  `{s,u}rshr`, `sq{r}shrn{2}`, `fcvtz{s,u}` (fixed-point). Marked `[Recomps#1]`.
- `rt_ext.h` (new): the C helpers those emit. Runtime must add `uint64_t ex_val2;` to `Ctx` and build with `-mcx16`.
  (Now only `runtime/rt_ext.h`; the copy in this folder was removed on 2026-11-02.)
- `recomp_eh.py`: every `.eh_frame` FDE start is a function entry; generated files include `rt_ext.h`.
- `test_ext.py` (new; since replaced by `tools/lifter-check`): decoder + semantics check of the added forms against Unicorn.

## Witch Time changes (2026-10-23)
- `recomp.py`: layout (`TEXT_SIZE`, `RELA`, `JMPREL`, `SYMTAB`, `STRTAB`) read from the ELF's first PT_LOAD and DYNAMIC
  (`_layout`), so the same lifter runs on Bayonetta 3's main.
- `recomp.py` + `rt_ext.h`, marked `[witchtime]`: the AdvSIMD forms B3's main uses and B2's did not: `{s,u}{max,min}`,
  `{s,u}{r,q}shl` by register, `{s,u}q{add,sub}`, `mla`/`mls`, `addp` (vector and scalar), `{s,u}{addl,addw,subl,subw}{2}`,
  `addv`/`{s,u}{max,min}v`/`{s,u}addlv`, `frint{n,m,p,z,a}`, `fcvta{s,u}`, `{s,u}addlp`/`{s,u}adalp`, `{s,u}sra`,
  `{r}shrn{2}`, `orr`/`bic` (vector, immediate), integer `mul`/`mla`/`mls`/`sq{r}dmulh` by element, one-lane
  `ld2..ld4`/`st2..st4` and `ld2r..ld4r`. With them every instruction of B3's main translates (what is left is
  zero padding, the MOD0 header and `udf` traps).
- `test_b3.py` (new; since replaced by `tools/lifter-check`): every word of your main.elf that uses these forms, run in
  Unicorn and in C with the real helpers (all 32 vector registers, the base register and memory compared). On the
  developer's dump: 200 words, 4,000 cases, 0 failures.

## Rust port (2026-10-27; the Python removed 2026-11-02)
- `recomp.py` -> `tools/wtbuild/src/lifter/mod.rs`, `recomp_eh.py` -> `tools/wtbuild/src/lifter/eh.rs`, the same C byte
  for byte (checked against the Python, then pinned by golden tests: `tools/wtbuild/README.md`, "Regression tests").
  Without `regcache.py` (B2recomp's optional register cache, off in every build).
- Witch Time changes the lift carries besides those above (marked `[witchtime]` in `eh.rs`; some made in `recomp_eh.py` first):
  host overrides (`eh.rs` `HOST_OVERRIDES`: direct calls of chosen game functions go to `runtime/game_shim_eh.c`), the
  Options-row patches (`options_row_patches`), the UI size sites of the aspect ratios (`UI_SIZE_SITES`) and, new in
  Rust, the frame-counter sites (`fps.rs`, `tools/wtbuild/fps_counters.txt`).
- `test_ext.py` and `test_b3.py` -> `tools/lifter-check`, which runs words of both kinds of forms through the shipped
  lifter (`wtbuild lift-words`), Unicorn and C. It finds `sq{r}dmulh` by element wrong when both 32-bit lanes are
  INT32_MIN (`v_ielem` in `runtime/rt_ext.h`; open).
