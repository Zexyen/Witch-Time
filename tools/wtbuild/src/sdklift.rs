//! Lifts selected functions of YOUR sdk module to C with the same lifter (part of the `tables` step).
//!
//! The SDK image is mapped by sdklift_rt.c at SDK_BASE; a call from a lifted function goes to another lifted SDK
//! function, to the host handler of the same name (an exported SDK function the game imports: a "cut"), or to a trap.
//! lifter/sdklift_roots.txt: one mangled name per line = root; `cut <name>`, `trap <name>`, `lift <name>`,
//! `addr <hex> [label]`, `nvn <nvnName>`, `vmethod <typeinfo name> <hex offset> <label>`.

use crate::lifter::{extent, CallEnv, Fn, Image, Plt, Unimpl};
use crate::nvn_procs;
use crate::util::{cstr, hx, i64_at, sext, u16_at, u32_at, u64_at};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::path::Path;

pub const SDK_BASE: i64 = 0x7500000000;

struct Sdk {
    im: Image,
    syms: HashMap<String, (u64, u64, u8)>,
    by_addr: HashMap<i64, Vec<String>>,
    code_slots: HashMap<i64, i64>,
    game_imports: HashSet<String>,
}

fn load(d: Vec<u8>, gen: &Path) -> Result<Sdk, String> {
    let phoff = u64_at(&d, 0x20) as usize;
    let (phes, phn) = (u16_at(&d, 0x36) as usize, u16_at(&d, 0x38) as usize);
    let loads: Vec<(u32, u64, u64, u64)> = (0..phn)
        .map(|i| {
            let p = phoff + i * phes;
            (u32_at(&d, p), u64_at(&d, p + 8), u64_at(&d, p + 16), u64_at(&d, p + 32))
        })
        .collect();
    let mut dynt: HashMap<i64, u64> = HashMap::new();
    let mut o = loads.iter().find(|p| p.0 == 2).ok_or("sdk.elf: no DYNAMIC")?.1 as usize;
    loop {
        let (tag, val) = (i64_at(&d, o), u64_at(&d, o + 8));
        o += 16;
        if tag == 0 {
            break;
        }
        dynt.insert(tag, val); // the last value of a tag wins
    }
    let g = |t: i64| dynt.get(&t).copied().ok_or(format!("sdk.elf: DYNAMIC has no tag {}", t));
    let text_size = loads.iter().find(|p| p.0 == 1 && p.2 == 0).ok_or("sdk.elf: no text segment")?.3 as i64;
    const FILE_OFF: usize = 0x1000;
    let nsym = u32_at(&d, FILE_OFF + g(4)? as usize + 4) as i64;
    let (symtab, strtab) = (g(6)? as usize, g(5)? as usize);
    let mut syms = HashMap::new();
    let mut by_addr: HashMap<i64, Vec<String>> = HashMap::new();
    for i in 0..nsym as usize {
        let o = FILE_OFF + symtab + i * 24;
        let (name_off, info, shndx, value, size) = (u32_at(&d, o), d[o + 4], u16_at(&d, o + 6), u64_at(&d, o + 8), u64_at(&d, o + 16));
        let name = cstr(&d, FILE_OFF + strtab + name_off as usize);
        if shndx != 0 && !name.is_empty() {
            syms.insert(name.clone(), (value, size, info & 0xf));
            if info & 0xf == 2 {
                by_addr.entry(SDK_BASE + value as i64).or_default().push(name);
            }
        }
    }
    // data slots that hold the address of a function after relocation (GOT entries, method tables): slot -> target
    let mut code_slots = HashMap::new();
    let (rela, relasz) = (g(7)? as usize, g(8)? as usize);
    for i in 0..relasz / 24 {
        let o = FILE_OFF + rela + i * 24;
        let (off, info, addend) = (u64_at(&d, o) as i64, u64_at(&d, o + 8), i64_at(&d, o + 16));
        let (typ, sym) = (info & 0xffffffff, (info >> 32) as usize);
        if typ == 0x403 {
            if 0x200 <= addend && addend < text_size && addend % 4 == 0 {
                code_slots.insert(SDK_BASE + off, SDK_BASE + addend);
            }
        } else if typ == 0x401 || typ == 0x101 {
            let so = FILE_OFF + symtab + sym * 24;
            let (sinfo, shndx, value) = (d[so + 4], u16_at(&d, so + 6), u64_at(&d, so + 8) as i64);
            if shndx != 0 && sinfo & 0xf == 2 {
                code_slots.insert(SDK_BASE + off, SDK_BASE + value + if typ == 0x101 { addend } else { 0 });
            }
        }
    }
    let game_imports = fs::read_to_string(gen.join("imports_all.txt"))
        .map_err(|e| format!("runtime/gen/imports_all.txt: {}", e))?
        .lines()
        .map(|l| l.trim().to_string())
        .filter(|l| !l.is_empty())
        .collect();
    let im = Image {
        base: SDK_BASE,
        file_off: FILE_OFF,
        text_size,
        rela: g(7)? as i64,
        relasz: g(8)? as i64,
        jmprel: g(23)? as i64,
        pltrelsz: g(2)? as i64,
        symtab: symtab as i64,
        strtab: strtab as i64,
        patches: HashMap::new(),
        fps_counters: Default::default(),
        img: d,
    };
    Ok(Sdk {
        im,
        syms,
        by_addr,
        code_slots,
        game_imports,
    })
}

#[derive(Default)]
struct Config {
    roots: Vec<i64>,
    cut: HashSet<String>,
    trap: HashSet<String>,
    lift: HashSet<String>,
    /// address -> label of `nvn` / `vmethod` roots
    named: BTreeMap<i64, String>,
}

fn read_config(sdk: &Sdk, roots_file: &Path, log: &mut dyn FnMut(String)) -> Result<Config, String> {
    let mut cfg = Config::default();
    let text = fs::read_to_string(roots_file).unwrap_or_default();
    let mut procs: Option<BTreeMap<String, i64>> = None;
    for line in text.lines() {
        let line = line.split('#').next().unwrap().trim();
        if line.is_empty() {
            continue;
        }
        let parts: Vec<&str> = line.split_whitespace().collect();
        let arg = |i: usize| parts.get(i).copied().ok_or(format!("sdklift_roots.txt: {:?} needs more words", line));
        match parts[0] {
            "cut" => {
                cfg.cut.insert(arg(1)?.into());
            }
            "trap" => {
                cfg.trap.insert(arg(1)?.into());
            }
            "lift" => {
                cfg.lift.insert(arg(1)?.into());
            }
            "addr" => cfg.roots.push(SDK_BASE + i64::from_str_radix(arg(1)?, 16).map_err(|e| e.to_string())?),
            "nvn" => {
                if procs.is_none() {
                    procs = Some(nvn_procs::procs(&sdk.im.img)?);
                }
                let off = *procs
                    .as_ref()
                    .unwrap()
                    .get(arg(1)?)
                    .ok_or(format!("{}: not in this sdk's proc table", arg(1)?))?;
                cfg.roots.push(SDK_BASE + off);
                cfg.named.insert(SDK_BASE + off, arg(1)?.into());
            }
            "vmethod" => {
                let slot = nvn_procs::vtable(&sdk.im.img, arg(1)?)? + i64::from_str_radix(arg(2)?, 16).map_err(|e| e.to_string())?;
                let f = *sdk
                    .code_slots
                    .get(&(SDK_BASE + slot))
                    .ok_or(format!("sdklift: {} +{} is not a code slot", arg(1)?, arg(2)?))?;
                cfg.roots.push(f);
                cfg.named.insert(f, arg(3)?.into());
            }
            name => match sdk.syms.get(name) {
                Some(s) => cfg.roots.push(SDK_BASE + s.0 as i64),
                None => log(format!("sdklift: {} is not a symbol of sdk.elf", name)),
            },
        }
    }
    Ok(cfg)
}

fn throw_helper(n: &str) -> bool {
    // ^_ZNKSt3__12[01]__(basic_string|vector_base)_commonILb1EE20__throw_(length_error|out_of_range)Ev$
    let Some(r) = n.strip_prefix("_ZNKSt3__12") else { return false };
    let Some(r) = r.strip_prefix('0').or_else(|| r.strip_prefix('1')) else {
        return false;
    };
    let Some(r) = r.strip_prefix("__") else { return false };
    let Some(r) = r.strip_prefix("basic_string").or_else(|| r.strip_prefix("vector_base")) else {
        return false;
    };
    let Some(r) = r.strip_prefix("_commonILb1EE20__throw_") else { return false };
    matches!(r, "length_errorEv" | "out_of_rangeEv")
}

fn noreturn(n: &str) -> bool {
    [
        "Abort",
        "UnexpectedDefault",
        "OnAssertionFailure",
        "__assert_fail",
        "__cxa_throw",
        "__cxa_pure_virtual",
        "_Unwind_Resume",
        "__throw_",
        "terminate",
    ]
    .iter()
    .any(|s| n.contains(s))
        || matches!(n, "abort" | "exit" | "_Exit")
}

struct Env<'a> {
    sdk: &'a Sdk,
    cfg: &'a Config,
    plt: Plt,
    host_names: Vec<String>,
    host_index: HashMap<String, usize>,
    root_set: HashSet<i64>,
    noreturn_stmt: bool,
    wanted: Vec<i64>,
    wanted_set: HashSet<i64>,
}

impl Env<'_> {
    fn host(&mut self, name: &str) -> String {
        if !self.host_index.contains_key(name) {
            self.host_index.insert(name.into(), self.host_names.len());
            self.host_names.push(name.into());
        }
        format!("sdk_host_call(c, {});", self.host_index[name])
    }
    fn want(&mut self, a: i64) {
        if self.wanted_set.insert(a) {
            self.wanted.push(a);
        }
    }
    /// Follow PLT stubs: (address, None) or (None, imported name).
    fn resolve(&mut self, mut target: i64) -> (Option<i64>, Option<String>) {
        for _ in 0..4 {
            let Some((_, shndx, value)) = self.plt.entry(&self.sdk.im, target) else {
                return (Some(target), None);
            };
            if shndx == 0 {
                return (None, self.plt.symbol(&self.sdk.im, target));
            }
            target = SDK_BASE + value as i64;
        }
        (Some(target), None)
    }
}

impl CallEnv for Env<'_> {
    fn call(&mut self, target: i64) -> String {
        self.noreturn_stmt = false;
        let (addr, imp) = self.resolve(target);
        if let Some(imp) = imp {
            return self.host(&imp); // malloc & co: the game's own
        }
        let addr = addr.unwrap();
        let sdk = self.sdk;
        let empty = Vec::new();
        let names = sdk.by_addr.get(&addr).unwrap_or(&empty);
        if names.iter().any(|n| n == "setjmp" || n == "_setjmp") {
            return "SDK_SETJMP(c);".into();
        }
        if names.iter().any(|n| n == "longjmp" || n == "_longjmp") {
            self.noreturn_stmt = true;
            return "{ sdk_longjmp(c); return; }".into();
        }
        for n in names {
            if throw_helper(n) && sdk.game_imports.contains(n) && !self.cfg.trap.contains(n) {
                self.noreturn_stmt = true;
                let h = self.host(n);
                return format!("{{ {} return; }}", h);
            }
        }
        for n in names {
            if noreturn(n) || self.cfg.trap.contains(n) {
                self.noreturn_stmt = true;
                return format!("{{ sdk_trap_call(c, \"{}\"); return; }}", n);
            }
        }
        if !self.root_set.contains(&addr) {
            for n in names {
                if (sdk.game_imports.contains(n) || self.cfg.cut.contains(n)) && !self.cfg.lift.contains(n) {
                    return self.host(n);
                }
            }
        }
        self.want(addr);
        format!("f_{}(c);", hx(addr))
    }
}

const TAILS: [&str; 7] = ["return;", "goto", "{ jt", "{ f_", "{ sdk_host_call", "{ sdk_trap_call", "{ sdk_longjmp"];

/// Text addresses materialised by ADRP+ADD / ADR inside [entry, end) that are function starts.
fn scan_code_refs(sdk: &Sdk, entry: i64, end: i64) -> Vec<i64> {
    let im = &sdk.im;
    let mut page: HashMap<u32, (i64, i64)> = HashMap::new();
    let mut found = Vec::new();
    let in_text = |t: i64| SDK_BASE + 0x200 <= t && t < im.text_end() && t % 4 == 0;
    let mut pc = entry;
    while pc < end {
        let w = im.word(pc);
        if w & 0x9f000000 == 0x90000000 {
            let imm = sext(((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)) as i64, 21);
            page.insert(w & 31, (pc, (pc & !0xfff) + (imm << 12)));
        } else if w & 0xffc00000 == 0x91000000 {
            if let Some(&(hp, hv)) = page.get(&((w >> 5) & 31)) {
                if pc - hp <= 32 {
                    let t = hv + ((w >> 10) & 0xfff) as i64;
                    if in_text(t) {
                        found.push(t);
                    }
                }
            }
        } else if w & 0x9f000000 == 0x10000000 {
            let imm = sext(((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)) as i64, 21);
            let t = pc + imm;
            if in_text(t) && !(entry <= t && t < end) {
                found.push(t);
            }
        } else if w & 0xffc00000 == 0xf9400000 {
            if let Some(&(hp, hv)) = page.get(&((w >> 5) & 31)) {
                if pc - hp <= 32 {
                    if let Some(&t) = sdk.code_slots.get(&(hv + ((w >> 10) & 0xfff) as i64 * 8)) {
                        found.push(t);
                    }
                }
            }
        }
        pc += 4;
    }
    found
}

/// MUL Vd.T, Vn.T, Vm.Ts[index], 16- and 32-bit elements
fn mul_by_element(w: u32) -> Option<String> {
    if w & 0xbf00f400 != 0x0f008000 {
        return None;
    }
    let (q, size, l, m, mut rm, h, rn, rd) = (
        (w >> 30) & 1,
        (w >> 22) & 3,
        (w >> 21) & 1,
        (w >> 20) & 1,
        (w >> 16) & 15,
        (w >> 11) & 1,
        (w >> 5) & 31,
        w & 31,
    );
    let (idx, fld, n, typ);
    if size == 2 {
        idx = (h << 1) | l;
        rm |= m << 4;
        fld = "w";
        n = if q != 0 { 4 } else { 2 };
        typ = "uint32_t";
    } else if size == 1 {
        idx = (h << 2) | (l << 1) | m;
        fld = "h";
        n = if q != 0 { 8 } else { 4 };
        typ = "uint16_t";
    } else {
        return None;
    }
    Some(format!(
        "{{ V128 a_ = c->v[{}], r_; {} e_ = c->v[{}].{}[{}]; r_.d[0] = 0; r_.d[1] = 0; for (int i_ = 0; i_ < {}; i_++) r_.{}[i_] = ({})(a_.{}[i_] * e_); c->v[{}] = r_; }}",
        rn, typ, rm, fld, idx, n, fld, typ, fld, rd
    ))
}

/// ADDV Bd/Hd/Sd, Vn.T
fn addv(w: u32) -> Option<String> {
    if w & 0xbf3ffc00 != 0x0e31b800 {
        return None;
    }
    let (q, size, rn, rd) = ((w >> 30) & 1, (w >> 22) & 3, (w >> 5) & 31, w & 31);
    let (fld, n, typ) = match (size, q) {
        (0, _) => ("b", if q != 0 { 16 } else { 8 }, "uint8_t"),
        (1, _) => ("h", if q != 0 { 8 } else { 4 }, "uint16_t"),
        (2, 1) => ("w", 4, "uint32_t"),
        _ => return None,
    };
    Some(format!(
        "{{ V128 a_ = c->v[{}]; {} s_ = 0; for (int i_ = 0; i_ < {}; i_++) s_ = ({})(s_ + a_.{}[i_]); c->v[{}].d[0] = s_; c->v[{}].d[1] = 0; }}",
        rn, typ, n, typ, fld, rd, rd
    ))
}

/// USHL / SSHL Vd.T, Vn.T, Vm.T
fn ushl_vec(w: u32) -> Option<String> {
    if w & 0x9f20fc00 != 0x0e204400 {
        return None;
    }
    let (q, u, size, rm, rn, rd) = ((w >> 30) & 1, (w >> 29) & 1, (w >> 22) & 3, (w >> 16) & 31, (w >> 5) & 31, w & 31);
    let (fld, nbits, ut, st) = [
        ("b", 8, "uint8_t", "int8_t"),
        ("h", 16, "uint16_t", "int16_t"),
        ("w", 32, "uint32_t", "int32_t"),
        ("d", 64, "uint64_t", "int64_t"),
    ][size as usize];
    if size == 3 && q == 0 {
        return None;
    }
    let n = (if q != 0 { 128 } else { 64 }) / nbits;
    let (body, decl) = if u != 0 {
        (
            format!(
                "r_.{}[i_] = s_ >= {} || s_ <= -{} ? 0 : s_ >= 0 ? ({})(x_ << s_) : ({})(x_ >> -s_);",
                fld, nbits, nbits, ut, ut
            ),
            format!("{} x_ = a_.{}[i_];", ut, fld),
        )
    } else {
        (
            format!(
                "r_.{}[i_] = ({})(s_ >= {} ? 0 : s_ >= 0 ? ({})(({}){}) : s_ <= -{} ? (x_ < 0 ? -1 : 0) : ({})(x_ >> -s_));",
                fld, ut, nbits, st, ut, "x_ << s_", nbits, st
            ),
            format!("{} x_ = ({})a_.{}[i_];", st, st, fld),
        )
    };
    Some(format!(
        "{{ V128 a_ = c->v[{}], b_ = c->v[{}], r_; r_.d[0] = 0; r_.d[1] = 0; for (int i_ = 0; i_ < {}; i_++) {{ int s_ = (int8_t)b_.{}[i_]; {} {} }} c->v[{}] = r_; }}",
        rn, rm, n, fld, decl, body, rd
    ))
}

/// (text, instructions, untranslated (pc, word, why), has svc)
fn lift_function(env: &mut Env, starts: &[i64], entry: i64) -> (String, usize, Vec<(i64, u32, String)>, bool) {
    let im = &env.sdk.im;
    let end = extent(starts, entry, im.text_end());
    let mut f = Fn::new(entry, end);
    let (mut lines, mut bad) = (Vec::new(), Vec::new());
    let mut pc = entry;
    let mut last_noreturn = false;
    while pc < end {
        let w = im.word(pc);
        env.noreturn_stmt = false;
        let stmt = match f.lift(env, w, pc) {
            Ok(s) => s,
            Err(Unimpl(e)) => match mul_by_element(w).or_else(|| addv(w)).or_else(|| ushl_vec(w)) {
                Some(s) => s,
                None => {
                    bad.push((pc, w, e.clone()));
                    format!("guest_unimpl(0x{}ULL, 0x{:08x}U, \"{}\");", hx(pc), w, e)
                }
            },
        };
        last_noreturn = env.noreturn_stmt;
        lines.push((pc, stmt));
        pc += 4;
    }
    let e = hx(entry);
    let mut out = format!("static void f_{}(Ctx *c) {{\n  c->fn = 0x{}ULL;", e, e);
    if f.has_br {
        out += "\n  uint64_t jt = 0;";
    }
    for (pc, s) in &lines {
        out += &format!("\nL_{}: {}", hx(*pc), s);
    }
    let lastw = im.word(end - 4);
    let mut falls = !TAILS.iter().any(|t| lines.last().unwrap().1.starts_with(t)) && !last_noreturn && lastw != 0 && end < im.text_end();
    // an unconditional branch / ret as the last real instruction never falls through
    if lastw & 0xfc000000 == 0x14000000 || lastw == 0xd65f03c0 || lastw & 0xfffffc1f == 0xd61f0000 {
        falls = false;
    }
    if falls {
        env.want(end);
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
    let has_svc = lines.iter().any(|(_, s)| s.contains("guest_svc"));
    (out, lines.len(), bad, has_svc)
}

/// runtime/gen/sdklift_gen.c, nvn_sdk_addrs.h, sdklift_report.txt
pub fn gen(sdk_elf: &Path, roots_file: &Path, gen_dir: &Path, log: &mut dyn FnMut(String)) -> Result<(), String> {
    let d = fs::read(sdk_elf).map_err(|e| format!("{}: {}", sdk_elf.display(), e))?;
    let sdk = load(d, gen_dir)?;
    let cfg = read_config(&sdk, roots_file, log)?;
    let mut starts = sdk.im.compute_starts();
    let starts_set0: HashSet<i64> = starts.iter().cloned().collect();
    let mut env = Env {
        sdk: &sdk,
        cfg: &cfg,
        plt: Plt::new(),
        host_names: Vec::new(),
        host_index: HashMap::new(),
        root_set: cfg.roots.iter().cloned().collect(),
        noreturn_stmt: false,
        wanted: Vec::new(),
        wanted_set: HashSet::new(),
    };
    let mut starts_set = starts_set0;
    for &r in &cfg.roots {
        env.want(r);
        if !starts_set.contains(&r) {
            let i = starts.partition_point(|&s| s < r);
            starts.insert(i, r);
            starts_set.insert(r);
        }
    }
    let (mut bodies, mut done, mut total, mut allbad, mut svc) = (Vec::new(), Vec::new(), 0usize, Vec::new(), Vec::new());
    let mut seen: HashSet<i64> = HashSet::new();
    loop {
        let todo: Vec<i64> = env.wanted.iter().filter(|a| !seen.contains(a)).cloned().collect();
        if todo.is_empty() {
            break;
        }
        for a in todo {
            let (body, n, bad, has_svc) = lift_function(&mut env, &starts, a);
            for t in scan_code_refs(&sdk, a, extent(&starts, a, sdk.im.text_end())) {
                if starts_set.contains(&t) {
                    env.want(t);
                }
            }
            bodies.push(body);
            done.push(a);
            seen.insert(a);
            total += n;
            allbad.extend(bad);
            if has_svc {
                svc.push(a);
            }
        }
    }
    let label = |a: i64| -> String {
        match sdk.by_addr.get(&a) {
            Some(v) => v[0].clone(),
            None => cfg.named.get(&a).cloned().unwrap_or_else(|| format!("sub_{:x}", a - SDK_BASE)),
        }
    };
    let mut f =
        String::from("/* generated by sdklift.py from sdk.elf -- do not edit */\n#include \"runtime.h\"\n#include \"rt_ext.h\"\n#include \"sdklift.h\"\n\n");
    f += "static void sdk_call_indirect(Ctx *c, uint64_t a);\n#define call_indirect sdk_call_indirect\n";
    for a in &done {
        f += &format!("static void f_{}(Ctx *c);\n", hx(*a));
    }
    f += &format!("\n{}\n\n#undef call_indirect\n", bodies.join("\n\n"));
    f += "const char *const sdk_host_names[] = {\n";
    for n in &env.host_names {
        f += &format!("  \"{}\",\n", n);
    }
    f += &format!("  0}};\nconst int sdk_host_name_count = {};\n", env.host_names.len());
    f += "const SdkFn sdk_fns[] = {\n";
    let mut sorted = done.clone();
    sorted.sort_unstable();
    for a in &sorted {
        f += &format!("  {{0x{}ULL, f_{}, \"{}\"}},\n", hx(*a), hx(*a), label(*a));
    }
    f += &format!("}};\nconst int sdk_fn_count = {};\n", done.len());
    f += "static void sdk_call_indirect(Ctx *c, uint64_t a) { sdk_dispatch(c, a); }\n";
    fs::write(gen_dir.join("sdklift_gen.c"), f).map_err(|e| e.to_string())?;
    // nvn_sdk_addrs.h: sdk offsets of the named roots and of what nvn_sdk.c's model points at
    let mut h = String::from("/* generated by lifter/sdklift.py from YOUR sdk (targets/exefs/sdk.elf) -- do not edit, never publish. */\n");
    let mut named: Vec<(&i64, &String)> = cfg.named.iter().collect();
    named.sort_by(|a, b| a.1.cmp(b.1));
    for (a, n) in &named {
        h += &format!("#define SDKA_{} 0x{:x}ull\n", n, **a - SDK_BASE);
    }
    if cfg.named.values().any(|n| n.starts_with("nvn")) {
        for (k, v) in nvn_procs::gpu_model(&sdk.im.img)? {
            h += &format!("#define SDKA_{} 0x{:x}ull\n", k, v);
        }
    }
    fs::write(gen_dir.join("nvn_sdk_addrs.h"), h).map_err(|e| e.to_string())?;
    log(format!(
        "functions lifted: {}   instructions: {}   not translated: {}   with svc: {}   host names: {}",
        done.len(),
        total,
        allbad.len(),
        svc.len(),
        env.host_names.len()
    ));
    for a in &svc {
        log(format!("  svc in {:x} {}", a - SDK_BASE, label(*a)));
    }
    for (pc, w, why) in allbad.iter().take(20) {
        log(format!("  untranslated {:x} {:08x} {}", pc - SDK_BASE, w, why));
    }
    let mut r = format!("lifted {} functions, {} instructions\nhost calls:\n", done.len(), total);
    for n in &env.host_names {
        r += &format!("  {}\n", n);
    }
    r += "functions:\n";
    for a in &sorted {
        r += &format!("  {:x} {}\n", a - SDK_BASE, label(*a));
    }
    fs::write(gen_dir.join("sdklift_report.txt"), r).map_err(|e| e.to_string())?;
    Ok(())
}
