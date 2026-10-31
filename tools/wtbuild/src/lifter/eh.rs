//! The whole-program lift of main with C++ landing-pad resume entries: a port of lifter/recomp_eh.py. Writes
//! <outdir>/c_<bucket>.c, funcs.h, table.c and imports.c, byte for byte as recomp_eh.py does (RECOMPS_REGCACHE off, as
//! setup.py always ran it).
//!
//!   - a function with landing pads becomes u_<entry>(Ctx *c, uint64_t rp) (rp = 0 from the top, else the pad to resume
//!     at) plus f_<entry>(Ctx *c) { u_<entry>(c, 0); }
//!   - a function with a catch clause whose handler can return is entered through nro_catch_call(c, u_<entry>)
//!   - the start of every FDE is a function entry; table.c also holds resume_table_main

use super::{extent, fps, lift_lines, CallEnv, Fn, Image, Plt};
use crate::util::{hx, i32_at, sext, u32_at, u64_at};
use rayon::prelude::*;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::path::Path;

/// Game functions whose direct calls go to a host function instead (runtime/game_shim_eh.c): see recomp_eh.py.
pub const HOST_OVERRIDES: &[(i64, &str)] = &[(0x7100b382c0, "morton_split32"), (0x7100d02de0, "fps_mode_set"), (0x7100c1f600, "frame_clock"), (0x71007e1fe4, "quad_walk"), (0x7100b20a20, "main_pass"), (0x710067037c, "step_kind")];

/// runtime/options_row.c OPTX_BASE: the page after main's image
const OPTX_PAGE: i64 = 0x7105a00000;

/// [witchtime] the PC settings rows of the Options screen: instruction patches (recomp_eh._options_row_patches).
pub fn options_row_patches(im: &mut Image) -> Result<(), String> {
    fn adrp(rd: u32, pc: i64, target: i64) -> u32 {
        let imm = (((target >> 12) - (pc >> 12)) & 0x1fffff) as u32;
        0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd
    }
    fn add(rd: u32, imm: u32) -> u32 {
        0x91000000 | imm << 10 | rd << 5 | rd
    }
    fn movz_w(rd: u32, imm: u32) -> u32 {
        0x52800000 | imm << 5 | rd
    }
    fn ldr_w(rt: u32, rn: u32, off: u32) -> u32 {
        0xb9400000 | (off / 4) << 10 | rn << 5 | rt
    }
    let mut want: Vec<(i64, (u32, u32))> = Vec::new();
    let put = |want: &mut Vec<(i64, (u32, u32))>, a: i64, v: (u32, u32)| {
        if let Some(e) = want.iter_mut().find(|e| e.0 == a) {
            e.1 = v;
        } else {
            want.push((a, v));
        }
    };
    for (a, r) in [
        (0x7100a30db0, 9),
        (0x7100a31808, 9),
        (0x7100a31890, 9),
        (0x7100a319fc, 9),
        (0x7100a31b48, 9),
        (0x7100a31bd8, 9),
        (0x7100a31d10, 9),
        (0x7100a31e1c, 9),
        (0x7100a31f08, 9),
        (0x7100a320dc, 9),
        (0x7100a321e0, 9),
        (0x7100a322fc, 9),
        (0x7100a32438, 28),
    ] {
        put(&mut want, a - 4, (adrp(r, a - 4, 0x7101851000), adrp(r, a - 4, OPTX_PAGE)));
        put(&mut want, a, (add(r, 0x1bc), add(r, 0)));
    }
    for (a, r) in [
        (0x7100a30dc0, 10),
        (0x7100a318b8, 10),
        (0x7100a31a0c, 10),
        (0x7100a31b58, 10),
        (0x7100a31be8, 10),
        (0x7100a31d20, 10),
        (0x7100a31e44, 10),
        (0x7100a31f10, 23),
        (0x7100a3245c, 24),
    ] {
        put(&mut want, a, (adrp(r, a, 0x7101c2b000), adrp(r, a, OPTX_PAGE)));
        put(&mut want, a + 4, (add(r, 0x1a8), add(r, 0x100)));
    }
    put(&mut want, 0x7100a31fe8, (movz_w(21, 7), ldr_w(21, 20, 0x28bc)));
    put(&mut want, 0x7100a32430, (movz_w(20, 7), adrp(28, 0x7100a32430, OPTX_PAGE)));
    let old434 = want.iter().find(|e| e.0 == 0x7100a32434).unwrap().1 .0;
    put(&mut want, 0x7100a32434, (old434, ldr_w(20, 28, 0xffc)));
    let old438 = want.iter().find(|e| e.0 == 0x7100a32438).unwrap().1 .0;
    put(&mut want, 0x7100a32438, (old438, add(28, 0)));
    for (a, (old, new)) in want {
        let have = im.raw_word(a);
        if have != old {
            return Err(format!("options row patch {:x}: the binary has {:08x}, expected {:08x}", a, have, old));
        }
        im.patches.insert(a, new);
    }
    im.fps_counters = fps::counters();
    Ok(())
}

pub struct EhInfo {
    pub pads: Vec<i64>,
    pub catchers: HashSet<i64>,
    pub starts: Vec<i64>,
    pub fde_starts: Vec<i64>,
}

struct Rd<'a> {
    im: &'a Image,
}

impl Rd<'_> {
    fn byte(&self, o: i64) -> u8 {
        self.im.img[self.im.file_off + o as usize]
    }
    fn u32(&self, o: i64) -> u32 {
        u32_at(&self.im.img, self.im.file_off + o as usize)
    }
    fn s32(&self, o: i64) -> i64 {
        i32_at(&self.im.img, self.im.file_off + o as usize) as i64
    }
    fn uleb(&self, mut o: i64) -> (i64, i64) {
        let (mut r, mut s) = (0i64, 0u32);
        loop {
            let b = self.byte(o) as i64;
            o += 1;
            if s < 64 {
                r |= (b & 0x7f) << s;
            }
            s += 7;
            if b & 0x80 == 0 {
                return (r, o);
            }
        }
    }
    fn sleb(&self, mut o: i64) -> (i64, i64) {
        let (mut r, mut s) = (0i64, 0u32);
        loop {
            let b = self.byte(o) as i64;
            o += 1;
            if s < 64 {
                r |= (b & 0x7f) << s;
            }
            s += 7;
            if b & 0x80 == 0 {
                if b & 0x40 != 0 && s < 64 {
                    r -= 1 << s;
                }
                return (r, o);
            }
        }
    }
    /// value of a DW_EH_PE field as an image offset (indirect pointers are not followed)
    fn enc(&self, o: i64, e: u8) -> Result<(i64, i64), String> {
        let f = e & 0x0f;
        let (mut v, n) = match f {
            0 | 4 => (u64_at(&self.im.img, self.im.file_off + o as usize) as i64, 8),
            0xc => (u64_at(&self.im.img, self.im.file_off + o as usize) as i64, 8),
            3 => (self.u32(o) as i64, 4),
            0xb => (self.s32(o), 4),
            1 => {
                let (v, o2) = self.uleb(o);
                (v, o2 - o)
            }
            _ => return Err(format!("unknown pointer encoding {:#x}", e)),
        };
        if v != 0 && e & 0x70 == 0x10 {
            v += o;
        }
        Ok((v, o + n))
    }
}

/// The exception tables of main (parse_main_eh): landing pads, functions entered through nro_catch_call, FDE starts.
pub fn parse_main_eh(im: &Image, log: &mut dyn FnMut(String)) -> Result<EhInfo, String> {
    let rd = Rd { im };
    let mut plt = Plt::new();
    let bl_target = |a: i64| -> Option<i64> {
        let w = im.word(a);
        if w & 0xfc000000 != 0x94000000 {
            return None;
        }
        Some(a + sext((w & 0x3ffffff) as i64, 26) * 4)
    };
    let mut term: HashMap<i64, bool> = HashMap::new();
    let mut is_terminate_helper = |plt: &mut Plt, t: i64| -> bool {
        if let Some(&v) = term.get(&t) {
            return v;
        }
        let mut calls: Vec<Option<String>> = Vec::new();
        for i in 0..6 {
            if let Some(x) = bl_target(t + i * 4) {
                calls.push(plt.symbol(im, x));
            }
        }
        let v = calls.len() >= 2 && calls[0].as_deref() == Some("__cxa_begin_catch") && calls[1].as_deref() == Some("_ZSt9terminatev");
        term.insert(t, v);
        v
    };
    let mod0 = rd.u32(4) as i64;
    let hdr = mod0 + rd.s32(mod0 + 0x10);
    let count = rd.u32(hdr + 8) as i64;
    let mut cies: HashMap<i64, (u8, u8)> = HashMap::new();
    let (mut pads, mut catchers, mut starts, mut fde_starts) = (HashSet::new(), HashSet::new(), HashSet::new(), HashSet::new());
    let mut n_term = 0;
    for i in 0..count {
        let fde = hdr + rd.s32(hdr + 16 + i * 8);
        let cie = fde + 4 - rd.u32(fde + 4) as i64;
        if !cies.contains_key(&cie) {
            let mut p = cie + 9;
            let mut aug = Vec::new();
            while rd.byte(p) != 0 {
                aug.push(rd.byte(p));
                p += 1;
            }
            p += 1;
            p = rd.uleb(p).1;
            p = rd.sleb(p).1;
            p += 1;
            let (mut le, mut fe) = (0xffu8, 0u8);
            if aug.first() == Some(&b'z') {
                p = rd.uleb(p).1;
                for &ch in &aug[1..] {
                    if ch == 76 {
                        le = rd.byte(p);
                        p += 1;
                    } else if ch == 82 {
                        fe = rd.byte(p);
                        p += 1;
                    } else if ch == 80 {
                        let e = rd.byte(p);
                        p += 1;
                        p = rd.enc(p, e & 0x7f)?.1;
                    }
                }
            }
            cies.insert(cie, (le, fe));
        }
        let (le, fe) = cies[&cie];
        let mut p = fde + 8;
        let start;
        (start, p) = rd.enc(p, fe)?;
        p = rd.enc(p, fe & 0x0f)?.1;
        if start.rem_euclid(4) == 0 && 0 <= start && start < im.text_size {
            fde_starts.insert(im.base + start);
        }
        p = rd.uleb(p).1;
        let lsda = if le != 0xff { rd.enc(p, le)?.0 } else { 0 };
        if lsda == 0 || start.rem_euclid(4) != 0 || !(0 <= start && start < im.text_size) {
            continue;
        }
        starts.insert(im.base + start);
        let mut p = lsda;
        if rd.byte(p) != 0xff {
            return Err(format!("LSDA at {:#x}: unexpected landing-pad base encoding", lsda));
        }
        p += 1;
        let tte = rd.byte(p);
        p += 1;
        if tte != 0xff {
            p = rd.uleb(p).1;
        }
        let cse = rd.byte(p);
        p += 1;
        let csl;
        (csl, p) = rd.uleb(p);
        let cs_end = p + csl;
        while p < cs_end {
            p = rd.enc(p, cse & 0x0f)?.1;
            p = rd.enc(p, cse & 0x0f)?.1;
            let lp;
            (lp, p) = rd.enc(p, cse & 0x0f)?;
            let act;
            (act, p) = rd.uleb(p);
            if lp == 0 {
                continue;
            }
            let pad = im.base + start + lp;
            pads.insert(pad);
            if act != 0 {
                let mut a = cs_end + act - 1;
                let mut clause = false;
                loop {
                    let (filt, a2) = rd.sleb(a);
                    let (nxt, _) = rd.sleb(a2);
                    if filt != 0 {
                        clause = true;
                    }
                    if nxt == 0 {
                        break;
                    }
                    a = a2 + nxt;
                }
                if clause {
                    // the pad's first instruction (after an optional register move) calls __clang_call_terminate
                    let mut terminate = false;
                    for k in 0..2 {
                        if let Some(t) = bl_target(pad + k * 4) {
                            terminate = is_terminate_helper(&mut plt, t);
                            break;
                        }
                    }
                    if terminate {
                        n_term += 1;
                    } else {
                        catchers.insert(im.base + start);
                    }
                }
            }
        }
    }
    log(format!(
        "exception tables of main: {} functions with an LSDA, {} landing pads, {} functions entered through nro_catch_call \
         ({} call-site records lead straight to __clang_call_terminate and need none)",
        starts.len(),
        pads.len(),
        catchers.len(),
        n_term
    ));
    log(format!("FDE function starts: {}", fde_starts.len()));
    let sorted = |s: HashSet<i64>| {
        let mut v: Vec<i64> = s.into_iter().collect();
        v.sort_unstable();
        v
    };
    Ok(EhInfo {
        pads: sorted(pads),
        catchers,
        starts: sorted(starts),
        fde_starts: sorted(fde_starts),
    })
}

/// Env of the full lift: every call is a lifted function (or a host override); targets are remembered.
struct FullEnv {
    wanted: Vec<i64>,
    wanted_set: HashSet<i64>,
    overrides: HashMap<i64, &'static str>,
}

impl FullEnv {
    fn new() -> FullEnv {
        FullEnv {
            wanted: Vec::new(),
            wanted_set: HashSet::new(),
            overrides: HOST_OVERRIDES.iter().cloned().collect(),
        }
    }
    fn want(&mut self, a: i64) {
        if self.wanted_set.insert(a) {
            self.wanted.push(a);
        }
    }
}

impl CallEnv for FullEnv {
    fn call(&mut self, target: i64) -> String {
        if let Some(n) = self.overrides.get(&target) {
            return format!("h_{}(c);", n);
        }
        self.want(target);
        format!("f_{}(c);", hx(target))
    }
}

const TAILS: [&str; 6] = ["return;", "goto", "{ jt", "{ f_", "{ imp_", "{ h_"];

struct Ctx<'a> {
    im: &'a Image,
    starts: &'a [i64],
    eh: &'a EhInfo,
}

/// One function: (text, instruction count, words not translated, [(pad, entry)])
fn lift_function(cx: &Ctx, plt: &mut Plt, env: &mut FullEnv, entry: i64) -> (String, usize, usize, Vec<(i64, i64)>) {
    let im = cx.im;
    if let Some((sym, shndx, value)) = plt.entry(im, entry) {
        // recomp.lift_function on a PLT stub (env.full)
        if shndx != 0 {
            env.want(im.base + value as i64);
            return (format!("void f_{}(Ctx *c) {{ f_{}(c); }}", hx(entry), hx(im.base + value as i64)), 0, 0, vec![]);
        }
        return (format!("void f_{}(Ctx *c) {{ host_import(c, {}); }}", hx(entry), sym), 0, 0, vec![]);
    }
    let end = extent(cx.starts, entry, im.text_end());
    let mut f = Fn::new(entry, end);
    let (lines, bad) = lift_lines(im, env, &mut f, |_, _| None);
    let lo = cx.eh.pads.partition_point(|&p| p < entry);
    let hi = cx.eh.pads.partition_point(|&p| p < end);
    let pads = &cx.eh.pads[lo..hi];
    let catcher = cx.eh.catchers.contains(&entry);
    let two = !pads.is_empty() || catcher;
    let e = hx(entry);
    let mut out = String::with_capacity(lines.len() * 64);
    out += &format!(
        "void {}_{}(Ctx *c{}) {{\n  c->fn = 0x{}ULL;",
        if two { "u" } else { "f" },
        e,
        if two { ", uint64_t rp" } else { "" },
        e
    );
    if f.has_br {
        out += "\n  uint64_t jt = 0;";
    }
    if !pads.is_empty() {
        out += "\n  if (rp) switch (rp) {";
        for lp in pads {
            out += &format!("\n  case 0x{}ULL: goto L_{};", hx(*lp), hx(*lp));
        }
        out += "\n  default: guest_trap(\"resume at an address that is not a landing pad of this function\", rp); }";
    }
    let last = &lines.last().unwrap().1;
    let falls = end < im.text_end() && !TAILS.iter().any(|t| last.starts_with(t));
    let census = if fps::census_on() { fps::sites(im, entry, end) } else { Vec::new() };
    let mut lines = lines;
    let listed = &im.fps_counters;
    if lines.iter().any(|(pc, _)| listed.contains(pc)) {
        for s in fps::sites(im, entry, end).into_iter().filter(|s| listed.contains(&s.store)) {
            let i = ((s.op - entry) / 4) as usize;
            lines[i].1 = fps::patch_op(&s, &lines[i].1).unwrap_or_else(|e| panic!("{}", e));
        }
    }
    for (pc, stmt) in &lines {
        match census.iter().find(|s| s.store == *pc) {
            Some(s) => {
                let (v, code) = match s.kind {
                    fps::Kind::Int { size, .. } | fps::Kind::IntReg { size } => (format!("c->x[{}]", s.src), size),
                    _ => (format!("(uint64_t)c->v[{}].w[0]", s.src), 0),
                };
                out += &format!("\nL_{}: fps_census(c, 0x{}ULL, c->x[{}] + (uint64_t){}LL, {}, {}); {}", hx(*pc), hx(*pc), s.base, s.off, v, code, stmt)
            }
            None => out += &format!("\nL_{}: {}", hx(*pc), stmt),
        }
    }
    if falls {
        out += &format!("\n  f_{}(c); return;", hx(end));
    } else {
        out += "\n  return;";
    }
    if f.has_br {
        out += "\nL_dispatch: switch (jt) {";
        for (pc, _) in &lines {
            out += &format!("\n  case 0x{}ULL: goto L_{};", hx(*pc), hx(*pc));
        }
        out += "\n  default: call_indirect(c, jt); return; }";
    }
    out += "\n}";
    if two {
        let body = if catcher {
            format!("nro_catch_call(c, u_{});", e)
        } else {
            format!("u_{}(c, 0);", e)
        };
        out += &format!("\nvoid f_{}(Ctx *c) {{ {} }}", e, body);
    }
    if falls {
        env.want(end);
    }
    let n = lines.len();
    (out, n, bad.len(), pads.iter().map(|&lp| (lp, entry)).collect())
}

/// recomp_eh.main: lift every function of main into outdir.
pub fn lift_main(im: &Image, outdir: &Path, log: &mut dyn FnMut(String)) -> Result<(), String> {
    let t0 = std::time::Instant::now();
    let el = || t0.elapsed().as_secs_f64();
    let info = parse_main_eh(im, log)?;
    let mut set: HashSet<i64> = im.compute_starts().into_iter().collect();
    set.extend(info.starts.iter().cloned());
    set.extend(info.fde_starts.iter().cloned());
    let mut starts: Vec<i64> = set.into_iter().collect();
    starts.sort_unstable();
    log(format!("function entries: {}  ({:.0}s)", starts.len(), el()));
    fs::create_dir_all(outdir).map_err(|e| format!("{}: {}", outdir.display(), e))?;
    let cx = Ctx {
        im,
        starts: &starts,
        eh: &info,
    };
    let mut known: HashSet<i64> = starts.iter().cloned().collect();
    let mut pending: Vec<i64> = starts.clone();
    let mut all_funcs: Vec<i64> = Vec::new();
    let (mut total, mut bad) = (0usize, 0usize);
    let in_text = |a: i64| im.base <= a && a < im.text_end();
    while !pending.is_empty() {
        pending.sort_unstable();
        let results: Vec<(usize, usize, Vec<i64>)> = pending
            .par_chunks(400)
            .map(|part| {
                let (mut plt, mut env) = (Plt::new(), FullEnv::new());
                let (mut n, mut b) = (0, 0);
                for &a in part {
                    let (_, k, kb, _) = lift_function(&cx, &mut plt, &mut env, a);
                    n += k;
                    b += kb;
                }
                (n, b, env.wanted)
            })
            .collect();
        let mut new: HashSet<i64> = HashSet::new();
        for (n, b, wanted) in results {
            total += n;
            bad += b;
            new.extend(wanted.into_iter().filter(|w| !known.contains(w) && in_text(*w)));
        }
        all_funcs.extend(pending.iter().cloned());
        log(format!(
            "discovery: {} functions, {} extra branch targets  ({:.0}s)",
            pending.len(),
            new.len(),
            el()
        ));
        known.extend(new.iter().cloned());
        pending = new.into_iter().collect();
    }
    all_funcs.sort_unstable();
    if !im.fps_counters.is_empty() {
        // every address of fps_counters.txt must be a site the patch finds (a typo, or a site whose form changed, fails here)
        let listed = &im.fps_counters;
        let found: HashSet<i64> = all_funcs
            .par_iter()
            .flat_map_iter(|&a| fps::sites(im, a, extent(&starts, a, im.text_end())).into_iter().map(|s| s.store).filter(|s| listed.contains(s)))
            .collect();
        let mut missing: Vec<String> = listed.iter().filter(|a| !found.contains(a)).map(|a| hx(*a)).collect();
        if !missing.is_empty() {
            missing.sort();
            return Err(format!("fps_counters.txt: not a counter site of main: {}", missing.join(" ")));
        }
        log(format!("fps counters: {} sites move by the game time due above 60 fps (tools/wtbuild/fps_counters.txt)", listed.len()));
    }
    const BUCKET: i64 = 0xC000; // one output file per 48 KB of guest code: file contents stay stable across runs
    let mut buckets: BTreeMap<i64, Vec<i64>> = BTreeMap::new();
    for &a in &all_funcs {
        buckets.entry((a - im.base) / BUCKET).or_default().push(a);
    }
    let jobs: Vec<(String, Vec<i64>)> = buckets.into_iter().map(|(b, e)| (format!("c_{:04x}.c", b), e)).collect();
    let written: Vec<Result<(String, bool, Vec<(i64, i64)>), String>> = jobs
        .par_iter()
        .map(|(name, entries)| {
            let (mut plt, mut env) = (Plt::new(), FullEnv::new());
            let mut texts = Vec::with_capacity(entries.len());
            let mut resume = Vec::new();
            for &a in entries {
                let (t, _, _, r) = lift_function(&cx, &mut plt, &mut env, a);
                texts.push(t);
                resume.extend(r);
            }
            let content = format!(
                "/* generated by recomp_eh.py -- do not edit */\n#include \"runtime.h\"\n#include \"rt_ext.h\"\n#include \"funcs.h\"\n\n{}\n",
                texts.join("\n\n")
            );
            let changed = crate::util::write_if_changed(&outdir.join(name), content.as_bytes()).map_err(|e| format!("{}: {}", name, e))?;
            Ok((name.clone(), changed, resume))
        })
        .collect();
    let (mut files, mut changed, mut resume) = (Vec::new(), 0, Vec::new());
    for r in written {
        let (name, ch, res) = r?;
        files.push(name);
        changed += ch as usize;
        resume.extend(res);
    }
    // a pad lifted twice (it is also a branch target lifted on its own): the outer function
    resume.sort_unstable();
    let mut first: BTreeMap<i64, i64> = BTreeMap::new();
    for (lp, e) in resume {
        first.entry(lp).or_insert(e);
    }
    let all_set: HashSet<i64> = all_funcs.iter().cloned().collect();
    let catchers: HashSet<i64> = info.catchers.iter().filter(|c| all_set.contains(c)).cloned().collect();
    let mut f = String::from("/* generated by recomp_eh.py -- do not edit */\n");
    f += "void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t));\n";
    for a in &all_funcs {
        f += &format!("void f_{}(Ctx *c);\n", hx(*a));
    }
    let mut ov: Vec<&str> = HOST_OVERRIDES.iter().map(|o| o.1).collect();
    ov.sort_unstable();
    ov.dedup();
    for n in ov {
        f += &format!("void h_{}(Ctx *c);\n", n);
    }
    let mut us: Vec<i64> = first
        .values()
        .cloned()
        .chain(catchers.iter().cloned())
        .collect::<HashSet<_>>()
        .into_iter()
        .collect();
    us.sort_unstable();
    for a in us {
        f += &format!("void u_{}(Ctx *c, uint64_t rp);\n", hx(a));
    }
    if !im.fps_counters.is_empty() {
        // runtime/game_shim_eh.c; a stand-in's funcs.h stays what recomp_eh.py writes
        f += "uint32_t fps_ci(Ctx *c, uint64_t key, uint64_t field, uint32_t wb, int k, int sign);\n";
        f += "int fps_cf(Ctx *c, uint64_t key, uint64_t field, float w, int sign);\n";
        f += "float fps_fs(Ctx *c, uint64_t key, uint64_t field);\n";
    }
    if fps::census_on() {
        // the census build: every counter site reports its store (runtime/game_shim_eh.c), and the list of the sites
        f += "void fps_census(Ctx *c, uint64_t site, uint64_t field, uint64_t v, int code);\n";
        let mut t = String::from("# store function load op kind field (lifter/fps.rs; WT_FPS_CENSUS=1)\n");
        for &a in &all_funcs {
            for s in fps::sites(im, a, extent(&starts, a, im.text_end())) {
                let k = match s.kind {
                    fps::Kind::Int { size, delta } => format!("int{}{:+}", size * 8, delta),
                    fps::Kind::IntReg { size } => format!("int{}+reg", size * 8),
                    fps::Kind::F1 { delta } => format!("f32{:+}", delta),
                    fps::Kind::FReg => "f32+reg".to_string(),
                };
                t += &format!("{} {} {} {} {} [x{} {:+}]\n", hx(s.store), hx(a), hx(s.load), hx(s.op), k, s.base, s.off);
            }
        }
        fs::write(outdir.join("fps_sites.txt"), t).map_err(|e| e.to_string())?;
        log(format!("fps census: every counter site reports its store (fps_sites.txt)"));
    }
    fs::write(outdir.join("funcs.h"), f).map_err(|e| e.to_string())?;
    let mut t = String::from("/* generated by recomp_eh.py -- do not edit */\n#include \"runtime.h\"\n#include \"funcs.h\"\n\n");
    t += "const FnEntry fn_table[] = {\n";
    for a in &all_funcs {
        t += &format!("  {{ 0x{}ULL, f_{} }},\n", hx(*a), hx(*a));
    }
    t += &format!("}};\nconst int fn_table_count = {};\n", all_funcs.len());
    t += "\n/* landing pad -> the function to call as fn(c, landing pad); sorted by address */\n";
    t += "typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } NroResume;\n";
    t += "const NroResume resume_table_main[] = {\n";
    for (lp, e) in &first {
        t += &format!("  {{ 0x{}ULL, u_{} }},\n", hx(*lp), hx(*e));
    }
    t += &format!("  {{ 0, 0 }}\n}};\nconst int resume_count_main = {};\n", first.len());
    fs::write(outdir.join("table.c"), t).map_err(|e| e.to_string())?;
    fs::write(outdir.join("imports.c"), imports_c(im)).map_err(|e| e.to_string())?;
    let mut keep: HashSet<String> = files.iter().cloned().collect();
    keep.insert("table.c".into());
    keep.insert("imports.c".into());
    if let Ok(rd) = fs::read_dir(outdir) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().to_string();
            if n.ends_with(".c") && !keep.contains(&n) {
                let _ = fs::remove_file(e.path());
            }
        }
    }
    let missing = info.pads.iter().filter(|lp| !first.contains_key(lp)).count();
    log(format!(
        "functions: {}   files: {} ({} rewritten)   instructions: {}   not translated: {}   ({:.0}s)",
        all_funcs.len(),
        files.len(),
        changed,
        total,
        bad,
        el()
    ));
    log(format!(
        "resume entries: {} landing pads ({} pads of the tables are in no lifted function), {} functions through nro_catch_call",
        first.len(),
        missing,
        catchers.len()
    ));
    Ok(())
}

/// every dynamic symbol by index: name, and whether this module defines it
fn imports_c(im: &Image) -> String {
    let nsym = (im.strtab - im.symtab) / 24;
    let mut f = String::from("/* generated by recomp.py -- do not edit */\n#include \"runtime.h\"\n\nconst DynSym dyn_syms[] = {\n");
    for i in 0..nsym {
        let (name_off, _, shndx, value) = im.sym(i);
        f += &format!("  {{ \"{}\", 0x{:x}ULL, {} }},\n", im.sym_name(name_off), value, if shndx != 0 { 1 } else { 0 });
    }
    f += &format!("}};\nconst int dyn_sym_count = {};\n", nsym);
    f
}
