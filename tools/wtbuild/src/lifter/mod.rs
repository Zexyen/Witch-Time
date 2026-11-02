//! The instruction lifter: AArch64 -> C, one statement per instruction. Ported from ssbu-recomp's tools/lifter/recomp.py
//! (GPL-3.0, through B2recomp), writing byte for byte the C it wrote; the tests at the end pin that output.
//!
//! Every guest function becomes `void f_<addr>(Ctx *c)` working on a register context; guest memory is mapped at its
//! real address, so guest pointers are host pointers.

pub mod eh;
pub mod fps;

use crate::util::{bits, cstr, hx, i64_at, py_repr, sext, u16_at, u32_at, u64_at};
use std::collections::HashMap;

/// An nso2elf ELF image and the layout values the lifter needs.
pub struct Image {
    pub img: Vec<u8>,
    pub base: i64,
    pub file_off: usize,
    pub text_size: i64,
    pub rela: i64,
    pub relasz: i64,
    pub jmprel: i64,
    pub pltrelsz: i64,
    pub symtab: i64,
    pub strtab: i64,
    /// instruction patches: address -> the word lifted instead of the binary's
    pub patches: HashMap<i64, u32>,
    /// the frame counters that move by the game time due (fps::counters; main's, as the patches: empty for a stand-in)
    pub fps_counters: std::collections::HashSet<i64>,
}

/// .text size (the first PT_LOAD) and the DYNAMIC tags (first value of each tag).
pub fn layout(img: &[u8]) -> (i64, HashMap<i64, u64>) {
    let phoff = u64_at(img, 0x20) as usize;
    let phnum = u16_at(img, 0x38) as usize;
    let (mut text, mut dyn_off) = (None, None);
    for i in 0..phnum {
        let p = phoff + i * 0x38;
        let t = u32_at(img, p);
        let off = u64_at(img, p + 8);
        let fsz = u64_at(img, p + 32);
        if t == 1 && text.is_none() {
            text = Some(fsz as i64);
        } else if t == 2 {
            dyn_off = Some(off as usize);
        }
    }
    let mut tags = HashMap::new();
    let mut d = dyn_off.expect("ELF without DYNAMIC");
    loop {
        let tag = i64_at(img, d);
        let val = u64_at(img, d + 8);
        if tag == 0 {
            return (text.unwrap_or(0), tags);
        }
        tags.entry(tag).or_insert(val);
        d += 16;
    }
}

impl Image {
    /// main.elf: BASE 0x7100000000, layout from the ELF.
    pub fn main(img: Vec<u8>) -> Image {
        let (text_size, dt) = layout(&img);
        let g = |t: i64| *dt.get(&t).unwrap_or(&0) as i64;
        Image {
            base: 0x7100000000,
            file_off: 0x1000,
            text_size,
            rela: g(7),
            relasz: g(8),
            jmprel: g(23),
            pltrelsz: g(2),
            symtab: g(6),
            strtab: g(5),
            patches: HashMap::new(),
            fps_counters: Default::default(),
            img,
        }
    }

    #[inline]
    pub fn word(&self, addr: i64) -> u32 {
        if !self.patches.is_empty() {
            if let Some(w) = self.patches.get(&addr) {
                return *w;
            }
        }
        u32_at(&self.img, self.file_off + (addr - self.base) as usize)
    }

    /// the word in the file, without patches
    pub fn raw_word(&self, addr: i64) -> u32 {
        u32_at(&self.img, self.file_off + (addr - self.base) as usize)
    }

    pub fn text_end(&self) -> i64 {
        self.base + self.text_size
    }

    /// dynamic symbol i: (name offset, info, shndx, value)
    pub fn sym(&self, i: i64) -> (u32, u8, u16, u64) {
        let o = self.file_off + (self.symtab + i * 24) as usize;
        (u32_at(&self.img, o), self.img[o + 4], u16_at(&self.img, o + 6), u64_at(&self.img, o + 8))
    }

    pub fn sym_name(&self, name_off: u32) -> String {
        cstr(&self.img, self.file_off + self.strtab as usize + name_off as usize)
    }

    /// Function entries: every BL target, every code address stored in data (relative relocations), code addresses
    /// materialised by ADRP+ADD / ADR (not jump-table bases), exported FUNC symbols. (Env.compute_starts)
    pub fn compute_starts(&self) -> Vec<i64> {
        let mut starts = std::collections::HashSet::new();
        let fo = self.file_off;
        let n = (self.text_size / 4) as usize;
        let text = |i: usize| u32_at(&self.img, fo + 4 * i);
        for i in 0..n {
            let w = text(i);
            if w & 0xfc000000 == 0x94000000 {
                starts.insert(self.base + i as i64 * 4 + sext((w & 0x3ffffff) as i64, 26) * 4);
            }
        }
        for i in 0..self.relasz / 24 {
            let o = fo + (self.rela + i * 24) as usize;
            let info = u64_at(&self.img, o + 8);
            let addend = i64_at(&self.img, o + 16);
            if info == 0x403 && 0 <= addend && addend < self.text_size && addend % 4 == 0 {
                starts.insert(self.base + addend);
            }
        }
        // [witchtime] a jump-table base reaches a `br` within 24 instructions: not a function entry
        let table_base = |i: usize, r: u32| -> bool {
            for k in i + 1..std::cmp::min(i + 25, n) {
                let w = text(k);
                if w == 0xd61f0000 | (r << 5) {
                    return true;
                }
                if w & 0xff000000 == 0x8b000000 && w & 31 == r && (w >> 5) & 31 == r {
                    continue;
                }
                if (w >> 25) & 7 == 7 || ((w >> 25) & 5 == 4 && (w >> 26) & 1 != 0) {
                    continue;
                }
                if w & 31 == r || w & 0x7c000000 == 0x14000000 || w & 0xff000010 == 0x54000000 || w & 0xfffffc1f == 0xd65f0000 {
                    return false;
                }
            }
            false
        };
        let mut page: HashMap<u32, (usize, i64)> = HashMap::new();
        for i in 0..n {
            let w = text(i);
            if w & 0x9f000000 == 0x90000000 {
                let imm = sext(((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)) as i64, 21);
                page.insert(w & 31, (i, ((i as i64 * 4) & !0xfff) + (imm << 12)));
            } else if w & 0xffc00000 == 0x91000000 {
                if let Some(&(hi, hp)) = page.get(&((w >> 5) & 31)) {
                    if i - hi <= 8 {
                        let target = hp + ((w >> 10) & 0xfff) as i64;
                        if 0 <= target && target < self.text_size && target % 4 == 0 && !table_base(i, w & 31) {
                            starts.insert(self.base + target);
                        }
                    }
                }
            } else if w & 0x9f000000 == 0x10000000 {
                let imm = sext(((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)) as i64, 21);
                let target = i as i64 * 4 + imm;
                if 0 <= target && target < self.text_size && target % 4 == 0 && !table_base(i, w & 31) {
                    starts.insert(self.base + target);
                }
            }
        }
        for i in 0..(self.strtab - self.symtab) / 24 {
            let (_, info, shndx, value) = self.sym(i);
            if shndx != 0 && info & 0xf == 2 && value % 4 == 0 {
                starts.insert(self.base + value as i64);
            }
        }
        let mut v: Vec<i64> = starts.into_iter().filter(|&a| self.base <= a && a < self.text_end()).collect();
        v.sort_unstable();
        v
    }
}

/// The next function start after entry (Env.extent).
pub fn extent(starts: &[i64], entry: i64, text_end: i64) -> i64 {
    let i = starts.partition_point(|&s| s <= entry);
    if i < starts.len() {
        starts[i]
    } else {
        text_end
    }
}

/// PLT stubs: which imported symbol a stub calls (Env.plt_symbol / plt_entry).
pub struct Plt {
    cache: HashMap<i64, Option<String>>,
    slot: HashMap<i64, i64>,
    syms: Option<(HashMap<i64, String>, HashMap<i64, (i64, u16, u64)>)>,
}

impl Plt {
    pub fn new() -> Plt {
        Plt {
            cache: HashMap::new(),
            slot: HashMap::new(),
            syms: None,
        }
    }

    fn load(im: &Image) -> (HashMap<i64, String>, HashMap<i64, (i64, u16, u64)>) {
        let (mut syms, mut info_map) = (HashMap::new(), HashMap::new());
        for i in 0..im.pltrelsz / 24 {
            let o = im.file_off + (im.jmprel + i * 24) as usize;
            let off = u64_at(&im.img, o) as i64;
            let info = u64_at(&im.img, o + 8);
            let si = (info >> 32) as i64;
            let (name_off, _, shndx, value) = im.sym(si);
            let slot = if off < im.base { im.base + off } else { off };
            syms.insert(slot, im.sym_name(name_off));
            info_map.insert(slot, (si, shndx, value));
        }
        (syms, info_map)
    }

    pub fn symbol(&mut self, im: &Image, target: i64) -> Option<String> {
        if let Some(n) = self.cache.get(&target) {
            return n.clone();
        }
        let mut name = None;
        if target + 16 <= im.text_end() {
            let (w0, w1, _w2, w3) = (im.word(target), im.word(target + 4), im.word(target + 8), im.word(target + 12));
            if w0 & 0x9f00001f == 0x90000010 && w1 & 0xffc003ff == 0xf9400211 && w3 == 0xd61f0220 {
                if self.syms.is_none() {
                    self.syms = Some(Plt::load(im));
                }
                let page = (target & !0xfff) + (sext(((bits(w0, 23, 5) << 2) | bits(w0, 30, 29)) as i64, 21) << 12);
                let slot = page + bits(w1, 21, 10) as i64 * 8;
                name = self.syms.as_ref().unwrap().0.get(&slot).cloned();
                if name.is_some() {
                    self.slot.insert(target, slot);
                }
            }
        }
        self.cache.insert(target, name.clone());
        name
    }

    /// (symbol index, section index, value) when target is a PLT stub
    pub fn entry(&mut self, im: &Image, target: i64) -> Option<(i64, u16, u64)> {
        self.symbol(im, target)?;
        let slot = self.slot[&target];
        Some(self.syms.as_ref().unwrap().1[&slot])
    }
}

/// How a lifted call or an out-of-function branch is written (Env.call): the main lifter and sdklift differ.
pub trait CallEnv {
    fn call(&mut self, target: i64) -> String;
}

pub struct Unimpl(pub String);

fn un<T>(s: impl Into<String>) -> Result<T, Unimpl> {
    Err(Unimpl(s.into()))
}

type R = Result<String, Unimpl>;

// ---------------------------------------------------------------- operands
fn x(n: u32) -> String {
    if n == 31 {
        "0ULL".into()
    } else {
        format!("c->x[{}]", n)
    }
}
fn xs(n: u32) -> String {
    format!("c->x[{}]", n)
}
fn wr(n: u32) -> String {
    if n == 31 {
        "0U".into()
    } else {
        format!("(uint32_t)c->x[{}]", n)
    }
}
fn ws(n: u32) -> String {
    format!("(uint32_t)c->x[{}]", n)
}
fn r(n: u32, sf: bool) -> String {
    if sf {
        x(n)
    } else {
        wr(n)
    }
}
fn r_sp(n: u32, sf: bool) -> String {
    if sf {
        xs(n)
    } else {
        ws(n)
    }
}
fn set(n: u32, sf: bool, expr: &str) -> String {
    set_sp(n, sf, expr, false)
}
fn set_sp(n: u32, sf: bool, expr: &str, sp: bool) -> String {
    if n == 31 && !sp {
        return format!("(void)({});", expr);
    }
    format!("c->x[{}] = ({})({});", n, t(sf), expr)
}
fn t(sf: bool) -> &'static str {
    if sf {
        "uint64_t"
    } else {
        "uint32_t"
    }
}
fn sz(sf: bool) -> u32 {
    if sf {
        64
    } else {
        32
    }
}

fn bit_length(v: u64) -> u32 {
    64 - v.leading_zeros()
}

fn decode_bitmasks(n: u32, imms: u32, immr: u32, datasize: u32) -> Result<(u64, u64), Unimpl> {
    let v = ((n << 6) | (!imms & 0x3f)) as u64;
    let length = bit_length(v) as i32 - 1;
    if length < 1 {
        return un("bitmask");
    }
    let levels: u128 = (1u128 << length) - 1;
    let (s, r) = (imms as u128 & levels, immr as u128 & levels);
    let esize = 1u128 << length;
    let d = (s.wrapping_sub(r)) & levels;
    let mut welem: u128 = (1u128 << (s + 1)) - 1;
    let telem: u128 = (1u128 << (d + 1)) - 1;
    let emask = (1u128 << esize) - 1;
    welem = ((welem >> r) | (welem << (esize - r))) & emask;
    let (mut wmask, mut tmask) = (0u128, 0u128);
    let mut i = 0u128;
    while i < datasize as u128 {
        wmask |= welem << i;
        tmask |= telem << i;
        i += esize;
    }
    let dm = (1u128 << datasize) - 1;
    Ok(((wmask & dm) as u64, (tmask & dm) as u64))
}

fn shifted(expr: &str, kind: u32, amount: u32, sf: bool) -> String {
    if amount == 0 {
        return expr.into();
    }
    match kind {
        0 => format!("({} << {})", expr, amount),
        1 => format!("({} >> {})", expr, amount),
        2 => format!("({})(({}){} >> {})", t(sf), if sf { "int64_t" } else { "int32_t" }, expr, amount),
        _ => format!("ror{}({}, {})", sz(sf), expr, amount),
    }
}

fn extended(n: u32, option: u32, shift: u32) -> String {
    let cast = [
        "(uint64_t)(uint8_t)",
        "(uint64_t)(uint16_t)",
        "(uint64_t)(uint32_t)",
        "(uint64_t)",
        "(uint64_t)(int64_t)(int8_t)",
        "(uint64_t)(int64_t)(int16_t)",
        "(uint64_t)(int64_t)(int32_t)",
        "(uint64_t)",
    ][option as usize];
    let e = format!("({}{})", cast, x(n));
    if shift != 0 {
        format!("({} << {})", e, shift)
    } else {
        e
    }
}

fn b(v: u32) -> bool {
    v != 0
}

/// The FMOV / FCONST immediate as a double: sign * (16 + m) / 16 * 2^(e - 3).
fn vfp_imm(imm8: u32) -> f64 {
    let sign = if imm8 & 0x80 != 0 { -1.0 } else { 1.0 };
    let exp = (((imm8 >> 4) & 7) ^ 4) as i32;
    sign * (16 + (imm8 & 0xf)) as f64 / 16.0 * 2f64.powi(exp - 3)
}

// ---------------------------------------------------------------- lifter
/// Lifting context for one guest function (address range [entry, end)).
pub struct Fn {
    pub entry: i64,
    pub end: i64,
    pub has_br: bool,
}

impl Fn {
    pub fn new(entry: i64, end: i64) -> Fn {
        Fn { entry, end, has_br: false }
    }

    fn jump(&self, env: &mut dyn CallEnv, target: i64) -> String {
        if self.entry <= target && target < self.end {
            return format!("goto L_{};", hx(target));
        }
        format!("{{ {} return; }}", env.call(target))
    }

    pub fn lift(&mut self, env: &mut dyn CallEnv, w: u32, pc: i64) -> R {
        let op0 = bits(w, 28, 25);
        match op0 {
            8 | 9 => self.dp_imm(w, pc),
            10 | 11 => self.branch(env, w, pc),
            4 | 6 | 12 | 14 => self.loadstore(w, pc),
            5 | 13 => self.dp_reg(w),
            7 | 15 => self.fpsimd(w),
            _ => un(format!("class{}", op0)),
        }
    }

    // ---- data processing, immediate
    fn dp_imm(&mut self, w: u32, pc: i64) -> R {
        let (sf, rd, rn) = (b(bits(w, 31, 31)), bits(w, 4, 0), bits(w, 9, 5));
        let g = bits(w, 25, 23);
        match g {
            0 | 1 => {
                let imm = sext(((bits(w, 23, 5) << 2) | bits(w, 30, 29)) as i64, 21);
                let val = if sf { (pc & !0xfff) + (imm << 12) } else { pc + imm };
                Ok(set(rd, true, &format!("0x{:x}ULL", val as u64)))
            }
            2 => {
                let imm: u64 = (bits(w, 21, 10) as u64) << if b(bits(w, 22, 22)) { 12 } else { 0 };
                let (sub, s) = (bits(w, 30, 30), bits(w, 29, 29));
                let a = r_sp(rn, sf);
                if s != 0 {
                    let bb = if sub != 0 { format!("~({}){}ULL", t(sf), imm) } else { format!("{}ULL", imm) };
                    return Ok(set(rd, sf, &format!("adds{}(c, {}, {}, {})", sz(sf), a, bb, sub)));
                }
                Ok(set_sp(rd, sf, &format!("{} {} {}ULL", a, if sub != 0 { "-" } else { "+" }, imm), true))
            }
            4 => {
                let opc = bits(w, 30, 29);
                let (wmask, _) = decode_bitmasks(bits(w, 22, 22), bits(w, 15, 10), bits(w, 21, 16), sz(sf))?;
                let a = r(rn, sf);
                let e = format!("{} {} 0x{:x}ULL", a, &"&|^&"[opc as usize..opc as usize + 1], wmask);
                if opc == 3 {
                    return Ok(set(rd, sf, &format!("logic{}(c, {})", sz(sf), e)));
                }
                Ok(set_sp(rd, sf, &e, true))
            }
            5 => {
                let (opc, hw, imm) = (bits(w, 30, 29), bits(w, 22, 21) * 16, bits(w, 20, 5) as u64);
                match opc {
                    0 => Ok(set(rd, sf, &format!("~(0x{:x}ULL)", imm << hw))),
                    2 => Ok(set(rd, sf, &format!("0x{:x}ULL", imm << hw))),
                    3 => Ok(set(rd, sf, &format!("({} & ~0x{:x}ULL) | 0x{:x}ULL", r(rd, sf), 0xffffu64 << hw, imm << hw))),
                    _ => un("movwide"),
                }
            }
            6 => {
                let (opc, n, immr, imms) = (bits(w, 30, 29), bits(w, 22, 22), bits(w, 21, 16), bits(w, 15, 10));
                let size = sz(sf);
                let (wmask, tmask) = decode_bitmasks(n, imms, immr, size)?;
                if opc == 3 {
                    return un("bitfield3");
                }
                let dst = if opc == 1 { r(rd, sf) } else { "0".into() };
                let top = if opc == 1 {
                    dst.clone()
                } else if opc == 0 {
                    format!("((s >> {}) & 1 ? ~({})0 : 0)", imms, t(sf))
                } else {
                    "0".into()
                };
                Ok(format!(
                    "{{ {} s = {}; {} bot = ({} & ~0x{:x}ULL) | (ror{}(s, {}) & 0x{:x}ULL); {} top = {}; {} }}",
                    t(sf),
                    r(rn, sf),
                    t(sf),
                    dst,
                    wmask,
                    size,
                    immr,
                    wmask,
                    t(sf),
                    top,
                    set(rd, sf, &format!("(top & ~0x{:x}ULL) | (bot & 0x{:x}ULL)", tmask, tmask))
                ))
            }
            7 => {
                let (rm, lsb) = (bits(w, 20, 16), bits(w, 15, 10));
                let size = sz(sf);
                if lsb == 0 {
                    return Ok(set(rd, sf, &r(rm, sf)));
                }
                Ok(set(
                    rd,
                    sf,
                    &format!("({} >> {}) | ({} << {})", r(rm, sf), lsb, r(rn, sf), size as i64 - lsb as i64),
                ))
            }
            _ => un(format!("dpimm{}", g)),
        }
    }

    // ---- branches and system
    fn branch(&mut self, env: &mut dyn CallEnv, w: u32, pc: i64) -> R {
        if bits(w, 30, 26) == 5 {
            let target = pc + sext(bits(w, 25, 0) as i64, 26) * 4;
            if b(bits(w, 31, 31)) {
                return Ok(format!("c->x[30] = 0x{}ULL; {}", hx(pc + 4), env.call(target)));
            }
            return Ok(self.jump(env, target));
        }
        if bits(w, 31, 24) == 0x54 {
            let target = pc + sext(bits(w, 23, 5) as i64, 19) * 4;
            return Ok(format!("if (cond(c, {})) {}", bits(w, 3, 0), self.jump(env, target)));
        }
        if bits(w, 30, 25) == 0x1a {
            let target = pc + sext(bits(w, 23, 5) as i64, 19) * 4;
            return Ok(format!(
                "if ({} {} 0) {}",
                r(bits(w, 4, 0), b(bits(w, 31, 31))),
                if b(bits(w, 24, 24)) { "!=" } else { "==" },
                self.jump(env, target)
            ));
        }
        if bits(w, 30, 25) == 0x1b {
            let bit = (bits(w, 31, 31) << 5) | bits(w, 23, 19);
            let target = pc + sext(bits(w, 18, 5) as i64, 14) * 4;
            return Ok(format!(
                "if ((({} >> {}) & 1) {} 0) {}",
                x(bits(w, 4, 0)),
                bit,
                if b(bits(w, 24, 24)) { "!=" } else { "==" },
                self.jump(env, target)
            ));
        }
        if bits(w, 31, 25) == 0x6b {
            let (opc, rn) = (bits(w, 24, 21), bits(w, 9, 5));
            return match opc {
                2 => Ok("return;".into()),
                1 => Ok(format!("{{ uint64_t t = {}; c->x[30] = 0x{}ULL; call_indirect(c, t); }}", x(rn), hx(pc + 4))),
                0 => {
                    self.has_br = true;
                    Ok(format!("{{ jt = {}; goto L_dispatch; }}", x(rn)))
                }
                _ => un(format!("br{}", opc)),
            };
        }
        if w & 0xfffff01f == 0xd503201f {
            return Ok(";".into());
        }
        if w & 0xffe0001f == 0xd4000001 {
            return Ok(format!("guest_svc(c, 0x{:x});", bits(w, 20, 5)));
        }
        if w & 0xffe0001f == 0xd4200000 {
            return Ok(format!("guest_trap(\"brk\", 0x{}ULL);", hx(pc)));
        }
        let m = w & 0xffffffe0;
        if m == 0xd53bd040 || m == 0xd53bd060 {
            return Ok(set(bits(w, 4, 0), true, "c->tpidr"));
        }
        if m == 0xd51bd040 {
            return Ok(format!("c->tpidr = {};", x(bits(w, 4, 0))));
        }
        if m == 0xd53b00e0 {
            return Ok(set(bits(w, 4, 0), true, "0x10ULL"));
        }
        if m == 0xd53b4400 || m == 0xd53b4420 {
            return Ok(set(bits(w, 4, 0), true, "0ULL"));
        }
        if m == 0xd51b4400 || m == 0xd51b4420 {
            return Ok(";".into());
        }
        let m2 = w & 0xfffff0ff;
        if m2 == 0xd503305f || m2 == 0xd50330bf || m2 == 0xd503309f || m2 == 0xd50330df {
            return Ok(";".into());
        }
        un("system")
    }

    // ---- loads and stores
    /// Single-register addressing: (pre, address, post).
    fn addr_mode(&self, w: u32, scale: u32) -> Result<(String, String, String), Unimpl> {
        let rn = bits(w, 9, 5);
        let base = xs(rn);
        if b(bits(w, 24, 24)) {
            return Ok((String::new(), format!("{} + {}ULL", base, (bits(w, 21, 10) as u64) << scale), String::new()));
        }
        if bits(w, 21, 21) == 0 {
            let (imm, mode) = (sext(bits(w, 20, 12) as i64, 9), bits(w, 11, 10));
            return match mode {
                0 => Ok((String::new(), format!("{} + (uint64_t){}LL", base, imm), String::new())),
                1 => Ok((String::new(), base.clone(), format!("{} += (uint64_t){}LL;", base, imm))),
                3 => Ok((format!("{} += (uint64_t){}LL;", base, imm), base, String::new())),
                _ => un("ldst-unpriv"),
            };
        }
        if bits(w, 11, 10) == 2 {
            let (option, s, rm) = (bits(w, 15, 13), bits(w, 12, 12), bits(w, 20, 16));
            if !matches!(option, 2 | 3 | 6 | 7) {
                return un("ldst-ext");
            }
            return Ok((
                String::new(),
                format!("{} + {}", base, extended(rm, option, if s != 0 { scale } else { 0 })),
                String::new(),
            ));
        }
        un("ldst-atomic")
    }

    fn loadstore(&mut self, w: u32, pc: i64) -> R {
        let v = b(bits(w, 26, 26));
        let g = bits(w, 29, 27);
        let (rt, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        if g == 5 {
            // pairs
            let (opc, mode, load) = (bits(w, 31, 30), bits(w, 24, 23), b(bits(w, 22, 22)));
            let rt2 = bits(w, 14, 10);
            let size: i64 = if v {
                4 << opc
            } else {
                if opc == 3 || (opc == 1 && !load) {
                    return un("pair");
                }
                if opc == 2 {
                    8
                } else {
                    4
                }
            };
            let imm = sext(bits(w, 21, 15) as i64, 7) * size;
            let base = xs(rn);
            let pre = if mode == 3 {
                format!("{} += (uint64_t){}LL;", base, imm)
            } else {
                String::new()
            };
            let post = if mode == 1 {
                format!("{} += (uint64_t){}LL;", base, imm)
            } else {
                String::new()
            };
            let off = if mode == 2 || mode == 0 { imm } else { 0 };
            let mut body = format!("uint64_t a = {} + (uint64_t){}LL; ", base, off);
            if v {
                if load {
                    body += &format!(
                        "V128 t1 = rdv(a, {}), t2 = rdv(a + {}, {}); c->v[{}] = t1; c->v[{}] = t2;",
                        size, size, size, rt, rt2
                    );
                } else {
                    body += &format!("wrv(a, &c->v[{}], {}); wrv(a + {}, &c->v[{}], {});", rt, size, size, rt2, size);
                }
            } else if load {
                let rdn = format!("rd{}", size * 8);
                if opc == 1 {
                    body += "uint64_t t1 = (uint64_t)(int64_t)(int32_t)rd32(a), t2 = (uint64_t)(int64_t)(int32_t)rd32(a + 4); ";
                } else {
                    body += &format!("uint64_t t1 = {}(a), t2 = {}(a + {}); ", rdn, rdn, size);
                }
                body += &format!("{} {}", set(rt, true, "t1"), set(rt2, true, "t2"));
            } else {
                body += &format!("wr{}(a, {}); wr{}(a + {}, {});", size * 8, r(rt, size == 8), size * 8, size, r(rt2, size == 8));
            }
            return Ok(format!("{{ {} {} {} }}", pre, body, post));
        }
        if g == 7 {
            // single register
            let (size, opc) = (bits(w, 31, 30), bits(w, 23, 22));
            if v {
                let nbytes = if opc & 2 != 0 { 16 } else { 1 << size };
                if opc & 2 != 0 && size != 0 {
                    return un("ldst-v");
                }
                let scale = if opc & 2 != 0 { 4 } else { size };
                let (pre, a, post) = self.addr_mode(w, scale)?;
                let body = if opc & 1 != 0 {
                    format!("c->v[{}] = rdv(a, {});", rt, nbytes)
                } else {
                    format!("wrv(a, &c->v[{}], {});", rt, nbytes)
                };
                return Ok(format!("{{ {} uint64_t a = {}; {} {} }}", pre, a, body, post));
            }
            if size == 3 && opc == 2 {
                return Ok(";".into());
            }
            let (pre, a, post) = self.addr_mode(w, size)?;
            let nb = 8 << size;
            let body = match opc {
                0 => format!("wr{}(a, {});", nb, r(rt, size == 3)),
                1 => set(rt, true, &format!("rd{}(a)", nb)),
                _ => set(rt, opc == 2, &format!("(int64_t)(int{}_t)rd{}(a)", nb, nb)),
            };
            return Ok(format!("{{ {} uint64_t a = {}; {} {} }}", pre, a, body, post));
        }
        if g == 3 && bits(w, 24, 24) == 0 {
            // literal
            let opc = bits(w, 31, 30);
            let a = format!("0x{}ULL", hx(pc + sext(bits(w, 23, 5) as i64, 19) * 4));
            if v {
                return Ok(format!("c->v[{}] = rdv({}, {});", rt, a, 4 << opc));
            }
            if opc == 3 {
                return Ok(";".into());
            }
            let e = match opc {
                0 => format!("rd32({})", a),
                1 => format!("rd64({})", a),
                _ => format!("(int64_t)(int32_t)rd32({})", a),
            };
            return Ok(set(rt, true, &e));
        }
        if bits(w, 29, 24) == 8 && !v {
            // exclusive / ordered
            let (size, o2, load, o1) = (bits(w, 31, 30), bits(w, 23, 23), b(bits(w, 22, 22)), bits(w, 21, 21));
            let nb = 8 << size;
            let rs = bits(w, 20, 16);
            if o1 != 0 {
                // [Recomps#1] ldxp / ldaxp / stxp / stlxp (see rt_ext.h)
                if o2 != 0 || size < 2 {
                    return un("excl-pair");
                }
                let rt2 = bits(w, 14, 10);
                if size == 3 {
                    if load {
                        return Ok(format!(
                            "{{ uint64_t a = {}; c->ex_addr = a; c->ex_val = rd64(a); c->ex_val2 = rd64(a + 8); \
                             uint64_t t1 = c->ex_val, t2 = c->ex_val2; {} {} }}",
                            xs(rn),
                            set(rt, true, "t1"),
                            set(rt2, true, "t2")
                        ));
                    }
                    return Ok(set(rs, false, &format!("stx128(c, {}, {}, {})", xs(rn), r(rt, true), r(rt2, true))));
                }
                if load {
                    return Ok(format!(
                        "{{ uint64_t a = {}; c->ex_addr = a; c->ex_val = rd64(a); uint64_t t = c->ex_val; {} {} }}",
                        xs(rn),
                        set(rt, true, "(uint32_t)t"),
                        set(rt2, true, "t >> 32")
                    ));
                }
                return Ok(set(
                    rs,
                    false,
                    &format!(
                        "stx64(c, {}, (uint64_t)(uint32_t){} | ((uint64_t)(uint32_t){} << 32))",
                        xs(rn),
                        r(rt, false),
                        r(rt2, false)
                    ),
                ));
            }
            if o2 != 0 {
                if load {
                    return Ok(set(rt, true, &format!("rd{}({})", nb, xs(rn))));
                }
                return Ok(format!("wr{}({}, {});", nb, xs(rn), r(rt, size == 3)));
            }
            if load {
                return Ok(format!(
                    "{{ uint64_t a = {}; c->ex_addr = a; c->ex_val = rd{}(a); {} }}",
                    xs(rn),
                    nb,
                    set(rt, true, "c->ex_val")
                ));
            }
            return Ok(set(rs, false, &format!("stx{}(c, {}, {})", nb, xs(rn), r(rt, size == 3))));
        }
        if bits(w, 31, 31) == 0 && matches!(bits(w, 29, 24), 0x0c | 0x0d) {
            // AdvSIMD structure load/store
            let (q, post, load, rm) = (bits(w, 30, 30), b(bits(w, 23, 23)), b(bits(w, 22, 22)), bits(w, 20, 16));
            let size = bits(w, 11, 10);
            let body: String;
            let total: u32;
            if bits(w, 24, 24) == 0 {
                let opcode = bits(w, 15, 12);
                let nb = if q != 0 { 16 } else { 8 };
                let nreg = match opcode {
                    7 => Some(1),
                    10 => Some(2),
                    6 => Some(3),
                    2 => Some(4),
                    _ => None,
                };
                if let Some(nreg) = nreg {
                    if load {
                        let a: Vec<String> = (0..nreg).map(|i| format!("V128 t{} = rdv(a + {}, {});", i, i * nb, nb)).collect();
                        let c: Vec<String> = (0..nreg).map(|i| format!("c->v[{}] = t{};", (rt + i) & 31, i)).collect();
                        body = format!("{} {}", a.join(" "), c.join(" "));
                    } else {
                        let a: Vec<String> = (0..nreg).map(|i| format!("wrv(a + {}, &c->v[{}], {});", i * nb, (rt + i) & 31, nb)).collect();
                        body = a.join(" ");
                    }
                    total = nb * nreg;
                } else {
                    let nstruct = match opcode {
                        8 => 2,
                        4 => 3,
                        0 => 4,
                        _ => return un("ldN"),
                    };
                    if size == 3 && q == 0 {
                        return un("ldN");
                    }
                    body = format!("{}(c, a, {}, {}, {}, {});", if load { "v_ldn" } else { "v_stn" }, rt, nstruct, size, q);
                    total = nb * nstruct;
                }
            } else {
                let (mut opcode, s_bit) = (bits(w, 15, 13), bits(w, 12, 12));
                // [witchtime] LD2..LD4 / ST2..ST4 of one lane and LD2R..LD4R: selem = (opcode<0>:R) + 1 registers
                let selem = (((opcode & 1) << 1) | bits(w, 21, 21)) + 1;
                if matches!(opcode, 6 | 7) && load {
                    let parts: Vec<String> = (0..selem)
                        .map(|k| {
                            format!(
                                "v_dup(&c->v[{}], rd{}(a + {}), {}, {});",
                                (rt + k) & 31,
                                8 << size,
                                k << size,
                                size,
                                (if q != 0 { 16 } else { 8 }) >> size
                            )
                        })
                        .collect();
                    body = parts.join(" ");
                    total = selem << size;
                } else {
                    opcode &= 6;
                    let (es, idx) = if opcode == 0 {
                        (0, (q << 3) | (s_bit << 2) | size)
                    } else if opcode == 2 {
                        (1, (q << 2) | (s_bit << 1) | (size >> 1))
                    } else if opcode == 4 && size == 0 {
                        (2, (q << 1) | s_bit)
                    } else if opcode == 4 && size == 1 {
                        (3, q)
                    } else {
                        return un("ldN");
                    };
                    let parts: Vec<String> = if load {
                        (0..selem)
                            .map(|k| format!("vset(&c->v[{}], {}, {}, rd{}(a + {}));", (rt + k) & 31, es, idx, 8 << es, k << es))
                            .collect()
                    } else {
                        (0..selem)
                            .map(|k| format!("wr{}(a + {}, vget(&c->v[{}], {}, {}));", 8 << es, k << es, (rt + k) & 31, es, idx))
                            .collect()
                    };
                    body = parts.join(" ");
                    total = selem << es;
                }
            }
            let wb = if post {
                format!("{} += {};", xs(rn), if rm == 31 { format!("{}ULL", total) } else { x(rm) })
            } else {
                String::new()
            };
            return Ok(format!("{{ uint64_t a = {}; {} {} }}", xs(rn), body, wb));
        }
        un(format!("ldst{}", g))
    }

    // ---- data processing, register
    fn dp_reg(&mut self, w: u32) -> R {
        let (sf, rd, rn, rm) = (b(bits(w, 31, 31)), bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16));
        let size = sz(sf);
        if bits(w, 28, 28) == 0 {
            if bits(w, 24, 24) == 0 {
                // logical, shifted register
                let (opc, n) = (bits(w, 30, 29), bits(w, 21, 21));
                let mut bb = shifted(&r(rm, sf), bits(w, 23, 22), bits(w, 15, 10), sf);
                if n != 0 {
                    bb = format!("~{}", bb);
                }
                let e = format!("{} {} {}", r(rn, sf), &"&|^&"[opc as usize..opc as usize + 1], bb);
                if opc == 3 {
                    return Ok(set(rd, sf, &format!("logic{}(c, {})", size, e)));
                }
                return Ok(set(rd, sf, &e));
            }
            let (sub, s) = (bits(w, 30, 30), bits(w, 29, 29));
            let (bb, a, sp);
            if bits(w, 21, 21) == 0 {
                bb = shifted(&r(rm, sf), bits(w, 23, 22), bits(w, 15, 10), sf);
                a = r(rn, sf);
                sp = false;
            } else {
                bb = format!("({}){}", t(sf), extended(rm, bits(w, 15, 13), bits(w, 12, 10)));
                a = r_sp(rn, sf);
                sp = true;
            }
            if s != 0 {
                let arg = if sub != 0 { format!("~({})", bb) } else { bb };
                return Ok(set(rd, sf, &format!("adds{}(c, {}, {}, {})", size, a, arg, sub)));
            }
            return Ok(set_sp(rd, sf, &format!("{} {} {}", a, if sub != 0 { "-" } else { "+" }, bb), sp));
        }
        let g = bits(w, 24, 21);
        if b(bits(w, 24, 24)) {
            // three-source
            let (op31, o0, ra) = (bits(w, 23, 21), bits(w, 15, 15), bits(w, 14, 10));
            let sign = if o0 != 0 { "-" } else { "+" };
            return match op31 {
                0 => Ok(set(rd, sf, &format!("{} {} {} * {}", r(ra, sf), sign, r(rn, sf), r(rm, sf)))),
                1 => Ok(set(
                    rd,
                    true,
                    &format!("{} {} (uint64_t)((int64_t)(int32_t){} * (int64_t)(int32_t){})", x(ra), sign, wr(rn), wr(rm)),
                )),
                5 => Ok(set(rd, true, &format!("{} {} (uint64_t){} * (uint64_t){}", x(ra), sign, wr(rn), wr(rm)))),
                2 => Ok(set(rd, true, &format!("smulh({}, {})", x(rn), x(rm)))),
                6 => Ok(set(rd, true, &format!("umulh({}, {})", x(rn), x(rm)))),
                _ => un("mul3"),
            };
        }
        if g == 0 && bits(w, 15, 10) == 0 {
            // adc / sbc
            let (sub, s) = (bits(w, 30, 30), bits(w, 29, 29));
            let bb = if sub != 0 { format!("~{}", r(rm, sf)) } else { r(rm, sf) };
            if s != 0 {
                return Ok(set(rd, sf, &format!("adds{}(c, {}, {}, c->C)", size, r(rn, sf), bb)));
            }
            return Ok(set(rd, sf, &format!("{} + {} + c->C", r(rn, sf), bb)));
        }
        if g == 2 {
            // ccmn / ccmp
            let (sub, cc, nzcv) = (bits(w, 30, 30), bits(w, 15, 12), bits(w, 3, 0));
            let bb = if b(bits(w, 11, 11)) { format!("{}ULL", rm) } else { r(rm, sf) };
            let bb = if sub != 0 {
                format!("~({}){}", t(sf), bb)
            } else {
                format!("({}){}", t(sf), bb)
            };
            return Ok(format!(
                "if (cond(c, {})) (void)adds{}(c, {}, {}, {}); else set_nzcv(c, {});",
                cc,
                size,
                r(rn, sf),
                bb,
                sub,
                nzcv
            ));
        }
        if g == 4 {
            // csel family
            let (op, o2, cc) = (bits(w, 30, 30), bits(w, 10, 10), bits(w, 15, 12));
            let bb = r(rm, sf);
            let alt = match op * 2 + o2 {
                0 => bb,
                1 => format!("{} + 1", bb),
                2 => format!("~{}", bb),
                _ => format!("({})0 - {}", t(sf), bb),
            };
            return Ok(set(rd, sf, &format!("cond(c, {}) ? {} : {}", cc, r(rn, sf), alt)));
        }
        if g == 6 {
            let opcode = bits(w, 15, 10);
            if bits(w, 30, 30) == 0 {
                // two-source
                let (a, bb) = (r(rn, sf), r(rm, sf));
                return match opcode {
                    2 => Ok(set(rd, sf, &format!("{} ? {} / {} : 0", bb, a, bb))),
                    3 => Ok(set(rd, sf, &format!("sdiv{}({}, {})", size, a, bb))),
                    8..=11 => {
                        let amt = format!("({} & {})", bb, size - 1);
                        Ok(match opcode {
                            8 => set(rd, sf, &format!("{} << {}", a, amt)),
                            9 => set(rd, sf, &format!("{} >> {}", a, amt)),
                            10 => set(rd, sf, &format!("({}){} >> {}", if sf { "int64_t" } else { "int32_t" }, a, amt)),
                            _ => set(rd, sf, &format!("ror{}({}, {})", size, a, amt)),
                        })
                    }
                    _ => un("dp2"),
                };
            }
            let a = r(rn, sf);
            return match opcode {
                0 => Ok(set(rd, sf, &format!("rbit{}({})", size, a))),
                1 => Ok(set(rd, sf, &format!("rev16_{}({})", size, a))),
                2 => Ok(set(rd, sf, &if sf { format!("rev32_64({})", a) } else { format!("bswap32({})", a) })),
                3 if sf => Ok(set(rd, sf, &format!("bswap64({})", a))),
                4 => Ok(set(rd, sf, &format!("clz{}({})", size, a))),
                5 => Ok(set(rd, sf, &format!("cls{}({})", size, a))),
                _ => un("dp1"),
            };
        }
        un(format!("dpreg{}", g))
    }

    // ---- scalar floating point (advanced SIMD in simd())
    fn fpsimd(&mut self, w: u32) -> R {
        let (rd, rn, rm) = (bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16));
        if bits(w, 30, 24) == 0x1e && bits(w, 21, 21) == 0 {
            // fixed-point <-> fp: the value is scaled by 2^fbits
            let (sf, ty, rmode, opcode) = (b(bits(w, 31, 31)), bits(w, 23, 22), bits(w, 20, 19), bits(w, 18, 16));
            let fbits = 64 - bits(w, 15, 10) as i32;
            if ty > 1 || fbits < 1 || fbits > (if sf { 64 } else { 32 }) {
                return un("fp16");
            }
            let f = if ty != 0 { "df[0]" } else { "f[0]" };
            let ft = if ty != 0 { "double" } else { "float" };
            if rmode == 0 && matches!(opcode, 2 | 3) {
                let mut src = r(rn, sf);
                if opcode == 2 {
                    src = format!("({}){}", if sf { "int64_t" } else { "int32_t" }, src);
                }
                return Ok(format!(
                    "{{ {} t = ({}){} * ({}){}; vzero(&c->v[{}]); c->v[{}].{} = t; }}",
                    ft,
                    ft,
                    src,
                    ft,
                    py_repr(2f64.powi(-fbits)),
                    rd,
                    rd,
                    f
                ));
            }
            if rmode == 3 && matches!(opcode, 0 | 1) {
                return Ok(set(
                    rd,
                    sf,
                    &format!(
                        "f2{}{}(trunc((double)c->v[{}].{} * {}))",
                        if opcode != 0 { "u" } else { "s" },
                        sz(sf),
                        rn,
                        f,
                        py_repr(2f64.powi(fbits))
                    ),
                ));
            }
            return un("fcvt");
        }
        if (bits(w, 31, 24) == 0x1e && bits(w, 21, 21) == 1) || (bits(w, 30, 24) == 0x1e && bits(w, 21, 21) == 1 && bits(w, 15, 10) == 0) {
            let ty = bits(w, 23, 22);
            if bits(w, 15, 10) == 0 {
                // int <-> fp
                let (sf, rmode, opcode) = (b(bits(w, 31, 31)), bits(w, 20, 19), bits(w, 18, 16));
                if ty == 2 && rmode == 1 && matches!(opcode, 6 | 7) {
                    if opcode == 6 {
                        return Ok(set(rd, true, &format!("c->v[{}].d[1]", rn)));
                    }
                    return Ok(format!("c->v[{}].d[1] = {};", rd, x(rn)));
                }
                if ty > 1 {
                    return un("fp16");
                }
                let f = if ty != 0 { "df[0]" } else { "f[0]" };
                let ft = if ty != 0 { "double" } else { "float" };
                if rmode == 0 && matches!(opcode, 2 | 3) {
                    let mut src = r(rn, sf);
                    if opcode == 2 {
                        src = format!("({}){}", if sf { "int64_t" } else { "int32_t" }, src);
                    }
                    return Ok(format!("{{ {} t = ({}){}; vzero(&c->v[{}]); c->v[{}].{} = t; }}", ft, ft, src, rd, rd, f));
                }
                if rmode == 0 && opcode == 6 {
                    return Ok(set(rd, sf, &format!("c->v[{}].{}", rn, if sf { "d[0]" } else { "w[0]" })));
                }
                if rmode == 0 && opcode == 7 {
                    return Ok(format!(
                        "{{ uint64_t t = {}; vzero(&c->v[{}]); c->v[{}].{} = t; }}",
                        r(rn, sf),
                        rd,
                        rd,
                        if sf { "d[0]" } else { "w[0]" }
                    ));
                }
                let rnd = match (rmode, opcode) {
                    (3, 0) | (3, 1) => "trunc",
                    (0, 0) | (0, 1) => "rint",
                    (1, 0) | (1, 1) => "ceil",
                    (2, 0) | (2, 1) => "floor",
                    (0, 4) | (0, 5) => "round",
                    _ => return un("fcvt"),
                };
                let unsigned = opcode & 1;
                return Ok(set(
                    rd,
                    sf,
                    &format!("f2{}{}({}((double)c->v[{}].{}))", if unsigned != 0 { "u" } else { "s" }, sz(sf), rnd, rn, f),
                ));
            }
            if bits(w, 31, 24) == 0x1e && bits(w, 14, 10) == 0x10 && matches!(ty, 0 | 1 | 3) {
                // FCVT to or from half precision
                let op = bits(w, 20, 15);
                if ty != 3 && op == 7 {
                    return Ok(format!(
                        "{{ uint16_t t = {}(c->v[{}].{}); vzero(&c->v[{}]); c->v[{}].h[0] = t; }}",
                        if ty != 0 { "f64_to_half" } else { "f32_to_half" },
                        rn,
                        if ty != 0 { "df[0]" } else { "f[0]" },
                        rd,
                        rd
                    ));
                }
                if ty == 3 && matches!(op, 4 | 5) {
                    let (tf, tt, fname) = if op == 5 {
                        ("df[0]", "double", "half_to_f64")
                    } else {
                        ("f[0]", "float", "half_to_f32")
                    };
                    return Ok(format!(
                        "{{ {} t = {}(c->v[{}].h[0]); vzero(&c->v[{}]); c->v[{}].{} = t; }}",
                        tt, fname, rn, rd, rd, tf
                    ));
                }
            }
            if bits(w, 31, 24) != 0x1e || ty > 1 {
                return un("fp16");
            }
            let f = if ty != 0 { "df[0]" } else { "f[0]" };
            let ft = if ty != 0 { "double" } else { "float" };
            let sfx = if ty != 0 { "" } else { "f" };
            let nb = if ty != 0 { 64 } else { 32 };
            let setf = |expr: &str| format!("{{ {} t = fnan{}({}); vzero(&c->v[{}]); c->v[{}].{} = t; }}", ft, nb, expr, rd, rd, f);
            let (a, bb) = (format!("c->v[{}].{}", rn, f), format!("c->v[{}].{}", rm, f));
            if bits(w, 11, 10) == 2 {
                let op = bits(w, 15, 12);
                if op < 4 {
                    return Ok(setf(&format!("{} {} {}", a, &"*/+-"[op as usize..op as usize + 1], bb)));
                }
                if matches!(op, 4..=7) {
                    return Ok(format!(
                        "{{ {} t = fp_maxmin{}({}, {}, {}, {}); vzero(&c->v[{}]); c->v[{}].{} = t; }}",
                        ft,
                        nb,
                        a,
                        bb,
                        op & 1,
                        (op >> 1) & 1,
                        rd,
                        rd,
                        f
                    ));
                }
                if op == 8 {
                    return Ok(setf(&format!("-({} * {})", a, bb)));
                }
                return un("fp2");
            }
            if bits(w, 14, 10) == 0x10 {
                let op = bits(w, 20, 15);
                match op {
                    0 => return Ok(setf(&a)),
                    1 => return Ok(setf(&format!("fabs{}({})", sfx, a))),
                    2 => return Ok(setf(&format!("-{}", a))),
                    3 => return Ok(setf(&format!("sqrt{}({})", sfx, a))),
                    4 | 5 => {
                        let (tf, tt) = if op == 5 { ("df[0]", "double") } else { ("f[0]", "float") };
                        return Ok(format!("{{ {} t = ({}){}; vzero(&c->v[{}]); c->v[{}].{} = t; }}", tt, tt, a, rd, rd, tf));
                    }
                    _ => {}
                }
                let rnd = match op {
                    8 => "rint",
                    9 => "ceil",
                    10 => "floor",
                    11 => "trunc",
                    12 => "round",
                    14 => "rint",
                    15 => "rint",
                    _ => return un("fp1"),
                };
                return Ok(setf(&format!("{}{}({})", rnd, sfx, a)));
            }
            if bits(w, 13, 10) == 8 {
                let b2 = if b(bits(w, 3, 3)) { "0".to_string() } else { bb };
                return Ok(format!("fcmp(c, (double){}, (double){});", a, b2));
            }
            if bits(w, 12, 10) == 4 {
                let imm8 = bits(w, 20, 13);
                return Ok(setf(&format!("({}){}", ft, py_repr(vfp_imm(imm8)))));
            }
            if bits(w, 11, 10) == 1 {
                return Ok(format!(
                    "if (cond(c, {})) fcmp(c, (double){}, (double){}); else set_nzcv(c, {});",
                    bits(w, 15, 12),
                    a,
                    bb,
                    bits(w, 3, 0)
                ));
            }
            if bits(w, 11, 10) == 3 {
                return Ok(setf(&format!("cond(c, {}) ? {} : {}", bits(w, 15, 12), a, bb)));
            }
            return un("fp");
        }
        if bits(w, 31, 24) == 0x1f {
            // fmadd family
            let ty = bits(w, 23, 22);
            if ty > 1 {
                return un("fp16");
            }
            let f = if ty != 0 { "df[0]" } else { "f[0]" };
            let ft = if ty != 0 { "double" } else { "float" };
            let (a, bb, ra) = (
                format!("c->v[{}].{}", rn, f),
                format!("c->v[{}].{}", rm, f),
                format!("c->v[{}].{}", bits(w, 14, 10), f),
            );
            let (o1, o0) = (bits(w, 21, 21), bits(w, 15, 15));
            let fma = if ty != 0 { "fma" } else { "fmaf" };
            let expr = match o1 * 2 + o0 {
                0 => format!("{}({}, {}, {})", fma, a, bb, ra),
                1 => format!("{}(-{}, {}, {})", fma, a, bb, ra),
                2 => format!("{}(-{}, {}, -{})", fma, a, bb, ra),
                _ => format!("{}({}, {}, -{})", fma, a, bb, ra),
            };
            return Ok(format!(
                "{{ {} t = fnan{}({}); vzero(&c->v[{}]); c->v[{}].{} = t; }}",
                ft,
                if ty != 0 { 64 } else { 32 },
                expr,
                rd,
                rd,
                f
            ));
        }
        self.simd(w)
    }

    fn simd(&mut self, w: u32) -> R {
        let (rd, rn, rm) = (bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16));
        if w & 0x9ff80c00 == 0x0f000400 {
            // movi / mvni / orr-imm (modified immediate)
            let (q, op, cmode) = (bits(w, 30, 30), bits(w, 29, 29), bits(w, 15, 12));
            let imm8 = (bits(w, 18, 16) << 5) | bits(w, 9, 5);
            let val: u64;
            if cmode == 15 {
                let v = vfp_imm(imm8);
                val = if op == 1 { v.to_bits() } else { (v as f32).to_bits() as u64 * 0x100000001 };
            } else if cmode == 14 && op == 1 {
                val = (0..8).filter(|i| imm8 & (1 << i) != 0).map(|i| 0xffu64 << (8 * i)).sum();
            } else if cmode == 14 {
                val = imm8 as u64 * 0x0101010101010101;
            } else if matches!(cmode, 0 | 2 | 4 | 6 | 12 | 13) {
                let mut v32: u64 = if cmode < 12 {
                    (imm8 as u64) << (8 * (cmode >> 1))
                } else if cmode == 12 {
                    ((imm8 as u64) << 8) | 0xff
                } else {
                    ((imm8 as u64) << 16) | 0xffff
                };
                if op != 0 {
                    v32 ^= 0xffffffff;
                }
                val = v32 * 0x100000001;
            } else if matches!(cmode, 8 | 10) {
                let mut v16: u64 = (imm8 as u64) << (8 * ((cmode >> 1) & 1));
                if op != 0 {
                    v16 ^= 0xffff;
                }
                val = v16 * 0x0001000100010001;
            } else if matches!(cmode, 1 | 3 | 5 | 7 | 9 | 11) {
                // [witchtime] ORR (op 0) / BIC (op 1), vector immediate
                let v = if cmode < 8 {
                    (((imm8 as u64) << (8 * (cmode >> 1))) & 0xffffffff) * 0x100000001
                } else {
                    ((imm8 as u64) << (8 * ((cmode >> 1) & 1))) * 0x0001000100010001
                };
                let opn = if op != 0 { "&= ~" } else { "|=" };
                let hi = if q != 0 {
                    format!("c->v[{}].d[1] {} 0x{:x}ULL;", rd, opn, v)
                } else {
                    format!("c->v[{}].d[1] = 0;", rd)
                };
                return Ok(format!("{{ c->v[{}].d[0] {} 0x{:x}ULL; {} }}", rd, opn, v, hi));
            } else {
                return un("simd-imm");
            }
            let hi = if q != 0 { format!("0x{:x}ULL", val) } else { "0".into() };
            return Ok(format!("{{ c->v[{}].d[0] = 0x{:x}ULL; c->v[{}].d[1] = {}; }}", rd, val, rd, hi));
        }
        if w & 0xbfe0fc00 == 0x0ea01c00 && rn == rm {
            // mov vD.16b, vN.16b (orr)
            let q = bits(w, 30, 30);
            return Ok(format!(
                "{{ V128 t = c->v[{}]; {} c->v[{}] = t; }}",
                rn,
                if q != 0 { "" } else { "t.d[1] = 0;" },
                rd
            ));
        }
        let bitwise = match w & 0xbfe0fc00 {
            0x0e201c00 => Some("a & b"),
            0x2e201c00 => Some("a ^ b"),
            0x0e601c00 => Some("a & ~b"),
            0x0ea01c00 => Some("a | b"),
            0x0ee01c00 => Some("a | ~b"),
            _ => None,
        };
        if let Some(opn) = bitwise {
            // and / eor / bic / orr / orn
            let q = bits(w, 30, 30);
            let body: String = (0..if q != 0 { 2 } else { 1 })
                .map(|i| format!("{{ uint64_t a = c->v[{}].d[{}], b = c->v[{}].d[{}]; t.d[{}] = {}; }} ", rn, i, rm, i, i, opn))
                .collect();
            return Ok(format!("{{ V128 t; t.d[1] = 0; {} c->v[{}] = t; }}", body, rd));
        }
        if b(bits(w, 31, 31)) || bits(w, 27, 25) != 7 {
            return un("simd");
        }
        let (scalar, q, u) = (b(bits(w, 28, 28)), bits(w, 30, 30), bits(w, 29, 29));
        if scalar && q == 0 {
            return un("simd");
        }
        let (size, szb, a) = (bits(w, 23, 22), bits(w, 22, 22), bits(w, 23, 23));
        let v = |n: u32| format!("&c->v[{}]", n);
        let fcount = || -> u32 {
            if scalar {
                1
            } else if q != 0 {
                if szb != 0 {
                    2
                } else {
                    4
                }
            } else if szb != 0 {
                1
            } else {
                2
            }
        };
        let icount = |es: u32| -> u32 {
            if scalar {
                1
            } else {
                (if q != 0 { 16 } else { 8 }) >> es
            }
        };

        if bits(w, 27, 24) == 0xe && b(bits(w, 21, 21)) {
            if b(bits(w, 10, 10)) {
                // three registers, same type
                let opc = bits(w, 15, 11);
                if opc >= 0x18 {
                    let op = match (u, a, opc) {
                        (0, 0, 0x1a) => "F_ADD",
                        (0, 1, 0x1a) => "F_SUB",
                        (1, 0, 0x1b) => "F_MUL",
                        (1, 0, 0x1f) => "F_DIV",
                        (0, 0, 0x19) => "F_MLA",
                        (0, 1, 0x19) => "F_MLS",
                        (0, 0, 0x1e) => "F_MAX",
                        (0, 1, 0x1e) => "F_MIN",
                        (0, 0, 0x18) => "F_MAXNM",
                        (0, 1, 0x18) => "F_MINNM",
                        (1, 0, 0x1a) => "F_ADDP",
                        (1, 1, 0x1a) => "F_ABD",
                        (0, 0, 0x1c) => "F_CMEQ",
                        (1, 0, 0x1c) => "F_CMGE",
                        (1, 1, 0x1c) => "F_CMGT",
                        (0, 0, 0x1f) => "F_RECPS",
                        (0, 1, 0x1f) => "F_RSQRTS",
                        _ => return un("simd-f3"),
                    };
                    if op == "F_ADDP" && scalar {
                        return un("simd-f3");
                    }
                    return Ok(format!("v_f3v({}, {}, {}, {}, {}, {});", v(rd), v(rn), v(rm), op, szb, fcount()));
                }
                if opc == 3 && u != 0 && size != 0 {
                    return Ok(format!("v_bitsel({}, {}, {}, {}, {});", v(rd), v(rn), v(rm), size, q));
                }
                let op = match (u, opc) {
                    (0, 0x10) => Some("I_ADD"),
                    (1, 0x10) => Some("I_SUB"),
                    (0, 0x13) => Some("I_MUL"),
                    (1, 0x11) => Some("I_CMEQ"),
                    (0, 0x11) => Some("I_CMTST"),
                    (0, 0x06) => Some("I_CMGT"),
                    (1, 0x06) => Some("I_CMHI"),
                    (0, 0x07) => Some("I_CMGE"),
                    (1, 0x07) => Some("I_CMHS"),
                    _ => None,
                };
                let Some(op) = op else {
                    // [witchtime] rt_ext.h v_i3x: min/max, shifts by register, saturating add/sub, mla/mls
                    let op = match (u, opc) {
                        (0, 0x0c) => Some("X_SMAX"),
                        (1, 0x0c) => Some("X_UMAX"),
                        (0, 0x0d) => Some("X_SMIN"),
                        (1, 0x0d) => Some("X_UMIN"),
                        (0, 0x08) => Some("X_SSHL"),
                        (1, 0x08) => Some("X_USHL"),
                        (0, 0x0a) => Some("X_SRSHL"),
                        (1, 0x0a) => Some("X_URSHL"),
                        (0, 0x09) => Some("X_SQSHL"),
                        (1, 0x09) => Some("X_UQSHL"),
                        (0, 0x01) => Some("X_SQADD"),
                        (1, 0x01) => Some("X_UQADD"),
                        (0, 0x05) => Some("X_SQSUB"),
                        (1, 0x05) => Some("X_UQSUB"),
                        (0, 0x12) => Some("X_MLA"),
                        (1, 0x12) => Some("X_MLS"),
                        _ => None,
                    };
                    if let Some(op) = op {
                        return Ok(format!("v_i3x({}, {}, {}, {}, {}, {});", v(rd), v(rn), v(rm), op, size, icount(size)));
                    }
                    if (u, opc) == (0, 0x17) && !scalar {
                        return Ok(format!("v_addp({}, {}, {}, {}, {});", v(rd), v(rn), v(rm), size, q));
                    }
                    return un("simd-i3");
                };
                return Ok(format!("v_i3v({}, {}, {}, {}, {}, {});", v(rd), v(rn), v(rm), op, size, icount(size)));
            }
            if bits(w, 20, 17) == 0 && bits(w, 11, 10) == 2 {
                // two registers, miscellaneous
                let opc = bits(w, 16, 12);
                let op = match (u, a, opc) {
                    (0, 1, 0x0f) => Some("F1_ABS"),
                    (1, 1, 0x0f) => Some("F1_NEG"),
                    (1, 1, 0x1f) => Some("F1_SQRT"),
                    (1, 1, 0x1d) => Some("F1_RSQRTE"),
                    (0, 1, 0x1d) => Some("F1_RECPE"),
                    (0, 0, 0x1d) => Some("F1_SCVTF"),
                    (1, 0, 0x1d) => Some("F1_UCVTF"),
                    (0, 1, 0x1b) => Some("F1_CVTZS"),
                    (1, 1, 0x1b) => Some("F1_CVTZU"),
                    (0, 1, 0x0d) => Some("F1_CMEQ0"),
                    (0, 1, 0x0c) => Some("F1_CMGT0"),
                    (1, 1, 0x0c) => Some("F1_CMGE0"),
                    (0, 1, 0x0e) => Some("F1_CMLT0"),
                    (1, 1, 0x0d) => Some("F1_CMLE0"),
                    _ => None,
                };
                if let Some(op) = op {
                    return Ok(format!("v_f1v({}, {}, {}, {}, {});", v(rd), v(rn), op, szb, fcount()));
                }
                if (u, opc) == (0, 0x12) && !scalar && size < 3 {
                    return Ok(format!("v_xtn({}, {}, {});", v(rd), v(rn), size));
                }
                if opc == 0x14 && !scalar && size < 3 {
                    return Ok(format!("v_qxtn({}, {}, {}, {}, {});", v(rd), v(rn), size, q, u));
                }
                // [witchtime] rt_ext.h: FRINTN/M/P/Z/A (vector), FCVTAS/FCVTAU, {S,U}ADDLP, {S,U}ADALP
                let mode = match (u, a, opc) {
                    (0, 0, 0x18) => Some(0),
                    (0, 0, 0x19) => Some(1),
                    (0, 1, 0x18) => Some(2),
                    (0, 1, 0x19) => Some(3),
                    (1, 0, 0x18) => Some(4),
                    _ => None,
                };
                if let Some(mode) = mode {
                    if !scalar {
                        return Ok(format!("v_frint({}, {}, {}, {}, {});", v(rd), v(rn), mode, szb, fcount()));
                    }
                }
                if (a, opc) == (0, 0x1c) {
                    return Ok(format!("v_fcvta({}, {}, {}, {}, {});", v(rd), v(rn), szb, u, fcount()));
                }
                if matches!(opc, 0x02 | 0x06) && !scalar && size < 3 {
                    return Ok(format!("v_addlp({}, {}, {}, {}, {}, {});", v(rd), v(rn), size, u, q, (opc == 0x06) as u32));
                }
                let op = match (u, opc) {
                    (1, 0x05) => "I1_NOT",
                    (0, 0x05) => "I1_CNT",
                    (1, 0x0b) => "I1_NEG",
                    (0, 0x0b) => "I1_ABS",
                    (0, 0x09) => "I1_CMEQ0",
                    (0, 0x00) => "I1_REV64",
                    _ => return un("simd-misc"),
                };
                if (op == "I1_NOT" || op == "I1_CNT") && size != 0 {
                    return un("simd-misc");
                }
                return Ok(format!("v_i1v({}, {}, {}, {}, {});", v(rd), v(rn), op, size, icount(size)));
            }
            if scalar && u != 0 && bits(w, 20, 17) == 8 && bits(w, 16, 12) == 0x0d && bits(w, 11, 10) == 2 {
                let (f, ft) = if szb != 0 { ("df", "double") } else { ("f", "float") };
                return Ok(format!(
                    "{{ {} t = c->v[{}].{}[0] + c->v[{}].{}[1]; vzero({}); c->v[{}].{}[0] = t; }}",
                    ft,
                    rn,
                    f,
                    rn,
                    f,
                    v(rd),
                    rd,
                    f
                ));
            }
            if bits(w, 11, 10) == 0 && !scalar && size < 3 {
                // [Recomps#1] three different: {s,u}{mull,mlal,mlsl}{2}
                let op = match bits(w, 15, 12) {
                    0xc => Some(0),
                    0x8 => Some(1),
                    0xa => Some(2),
                    _ => None,
                };
                if let Some(op) = op {
                    return Ok(format!("v_long3({}, {}, {}, {}, {}, {}, {});", v(rd), v(rn), v(rm), op, size, u, q));
                }
                if bits(w, 15, 12) < 4 {
                    return Ok(format!(
                        "v_addsubl({}, {}, {}, {}, {}, {}, {});",
                        v(rd),
                        v(rn),
                        v(rm),
                        bits(w, 15, 12),
                        size,
                        u,
                        q
                    ));
                }
            }
            if bits(w, 20, 17) == 8 && bits(w, 11, 10) == 2 {
                // [witchtime] across lanes, and scalar addp
                let opc = bits(w, 16, 12);
                if scalar && (u, opc, size) == (0, 0x1b, 3) {
                    return Ok(format!(
                        "{{ uint64_t t = c->v[{}].d[0] + c->v[{}].d[1]; c->v[{}].d[0] = t; c->v[{}].d[1] = 0; }}",
                        rn, rn, rd, rd
                    ));
                }
                let op = match (u, opc) {
                    (0, 0x1b) => Some(0),
                    (0, 0x0a) | (1, 0x0a) => Some(1),
                    (0, 0x1a) | (1, 0x1a) => Some(2),
                    (0, 0x03) | (1, 0x03) => Some(3),
                    _ => None,
                };
                if let Some(op) = op {
                    if !scalar && size < 3 {
                        return Ok(format!("v_across({}, {}, {}, {}, {}, {});", v(rd), v(rn), op, size, u, icount(size)));
                    }
                }
            }
            return un("simd-e");
        }
        if bits(w, 27, 24) == 0xe {
            if bits(w, 15, 15) == 0 && bits(w, 10, 10) == 1 {
                // copy: dup / ins / umov / smov
                let (imm5, imm4) = (bits(w, 20, 16), bits(w, 14, 11));
                if imm5 & 0xf == 0 {
                    return un("simd-copy");
                }
                let es = imm5.trailing_zeros();
                let idx = imm5 >> (es + 1);
                if scalar {
                    if u != 0 || imm4 != 0 {
                        return un("simd-copy");
                    }
                    return Ok(format!(
                        "{{ uint64_t t = vget({}, {}, {}); vzero({}); vset({}, {}, 0, t); }}",
                        v(rn),
                        es,
                        idx,
                        v(rd),
                        v(rd),
                        es
                    ));
                }
                if u != 0 {
                    return Ok(format!("vset({}, {}, {}, vget({}, {}, {}));", v(rd), es, idx, v(rn), es, imm4 >> es));
                }
                return match imm4 {
                    0 => Ok(format!("v_dup({}, vget({}, {}, {}), {}, {});", v(rd), v(rn), es, idx, es, icount(es))),
                    1 => Ok(format!("v_dup({}, {}, {}, {});", v(rd), x(rn), es, icount(es))),
                    3 => Ok(format!("vset({}, {}, {}, {});", v(rd), es, idx, x(rn))),
                    7 => Ok(set(rd, true, &format!("vget({}, {}, {})", v(rn), es, idx))),
                    5 => Ok(set(rd, q != 0, &format!("vsext(vget({}, {}, {}), {})", v(rn), es, idx, es))),
                    _ => un("simd-copy"),
                };
            }
            if bits(w, 15, 15) == 0 && bits(w, 10, 10) == 0 && !scalar {
                if u != 0 {
                    return Ok(format!("v_ext({}, {}, {}, {}, {});", v(rd), v(rn), v(rm), bits(w, 14, 11), q));
                }
                if bits(w, 11, 10) == 0 {
                    return Ok(format!(
                        "v_tbl(c, {}, {}, {}, {}, {}, {});",
                        rd,
                        rn,
                        bits(w, 14, 13) + 1,
                        rm,
                        q,
                        bits(w, 12, 12)
                    ));
                }
                return Ok(format!(
                    "v_perm({}, {}, {}, {}, {}, {});",
                    v(rd),
                    v(rn),
                    v(rm),
                    bits(w, 14, 12),
                    size,
                    icount(size)
                ));
            }
            return un("simd-e");
        }
        if bits(w, 10, 10) == 0 {
            // by element
            let iop = match (u, bits(w, 15, 12)) {
                (0, 8) => Some(0),
                (1, 0) => Some(1),
                (1, 4) => Some(2),
                (0, 0xc) => Some(3),
                (0, 0xd) => Some(4),
                _ => None,
            };
            if let Some(iop) = iop {
                if matches!(size, 1 | 2) {
                    // [witchtime] rt_ext.h v_ielem
                    let (h, l, m) = (bits(w, 11, 11), bits(w, 21, 21), bits(w, 20, 20));
                    let (idx, rm_) = if size == 1 {
                        ((h << 2) | (l << 1) | m, bits(w, 19, 16))
                    } else {
                        ((h << 1) | l, bits(w, 20, 16))
                    };
                    return Ok(format!(
                        "v_ielem({}, {}, vget({}, {}, {}), {}, {}, {});",
                        v(rd),
                        v(rn),
                        v(rm_),
                        size,
                        idx,
                        iop,
                        size,
                        icount(size)
                    ));
                }
            }
            let op = match bits(w, 15, 12) {
                9 => "F_MUL",
                1 => "F_MLA",
                5 => "F_MLS",
                _ => return un("simd-elem"),
            };
            if u != 0 || a == 0 {
                return un("simd-elem");
            }
            let idx = if szb != 0 {
                bits(w, 11, 11)
            } else {
                (bits(w, 11, 11) << 1) | bits(w, 21, 21)
            };
            let n = if scalar {
                1
            } else if q != 0 {
                if szb != 0 {
                    2
                } else {
                    4
                }
            } else {
                2
            };
            return Ok(format!("v_f3e({}, {}, {}, {}, {}, {}, {});", v(rd), v(rn), v(bits(w, 20, 16)), idx, op, szb, n));
        }
        let (immh, immhb, opc) = (bits(w, 22, 19), bits(w, 22, 16) as i64, bits(w, 15, 11));
        if immh == 0 {
            return un("simd-imm");
        }
        let es = 31 - immh.leading_zeros();
        let left = immhb - (8i64 << es);
        let right = (16i64 << es) - immhb;
        if opc == 0x14 && !scalar && es < 3 {
            return Ok(format!("v_shll({}, {}, {}, {}, {}, {});", v(rd), v(rn), left, es, u, q));
        }
        if opc == 0x0a && u == 0 {
            return Ok(format!("v_shift({}, {}, {}, {}, 0, {});", v(rd), v(rn), left, es, icount(es)));
        }
        if opc == 0 {
            return Ok(format!(
                "v_shift({}, {}, {}, {}, {}, {});",
                v(rd),
                v(rn),
                right,
                es,
                if u != 0 { 2 } else { 1 },
                icount(es)
            ));
        }
        // [Recomps#1] helpers in rt_ext.h
        if opc == 0x04 && !scalar {
            return Ok(format!("v_rshr({}, {}, {}, {}, {}, {});", v(rd), v(rn), right, es, u, icount(es)));
        }
        if matches!(opc, 0x12 | 0x13) && u == 0 && !scalar && es < 3 {
            return Ok(format!("v_sqshrn({}, {}, {}, {}, {}, {});", v(rd), v(rn), right, es, q, opc & 1));
        }
        if opc == 0x02 {
            return Ok(format!("v_sra({}, {}, {}, {}, {}, {});", v(rd), v(rn), right, es, u, icount(es)));
        }
        if matches!(opc, 0x10 | 0x11) && u == 0 && !scalar && es < 3 {
            return Ok(format!("v_shrn({}, {}, {}, {}, {}, {});", v(rd), v(rn), right, es, q, opc & 1));
        }
        if opc == 0x1f && !scalar && es >= 2 {
            return Ok(format!(
                "v_fcvtz_fix({}, {}, {}, {}, {}, {});",
                v(rd),
                v(rn),
                right,
                (es == 3) as u32,
                u,
                icount(es)
            ));
        }
        un("simd-shift")
    }
}

/// The statements of [entry, end): (pc, statement) and the words that did not translate.
pub fn lift_lines(
    im: &Image,
    env: &mut dyn CallEnv,
    f: &mut Fn,
    mut on_unimpl: impl FnMut(u32, &str) -> Option<String>,
) -> (Vec<(i64, String)>, Vec<(i64, u32, String)>) {
    let (mut lines, mut bad) = (Vec::new(), Vec::new());
    let mut pc = f.entry;
    while pc < f.end {
        let w = im.word(pc);
        let stmt = match f.lift(env, w, pc) {
            Ok(s) => s,
            Err(Unimpl(e)) => match on_unimpl(w, &e) {
                Some(s) => s,
                None => {
                    bad.push((pc, w, e.clone()));
                    format!("guest_unimpl(0x{}ULL, 0x{:08x}U, \"{}\");", hx(pc), w, e)
                }
            },
        };
        lines.push((pc, stmt));
        pc += 4;
    }
    (lines, bad)
}

/// For lift-words and the tests: the statement one word lifts to, with calls written as `f_<target>(c);`.
pub fn lift_word(w: u32, pc: i64, entry: i64, end: i64) -> (String, bool) {
    struct Plain;
    impl CallEnv for Plain {
        fn call(&mut self, target: i64) -> String {
            format!("f_{}(c);", hx(target))
        }
    }
    let mut f = Fn::new(entry, end);
    match f.lift(&mut Plain, w, pc) {
        Ok(s) => (s, f.has_br),
        Err(Unimpl(e)) => (format!("UNIMPL {}", e), f.has_br),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use sha2::{Digest, Sha256};
    use std::io::Write;

    /// splitmix64
    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 {
            self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15);
            let z = (self.0 ^ (self.0 >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
            let z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
            z ^ (z >> 31)
        }
        fn pick<T: Copy>(&mut self, xs: &[T]) -> T {
            xs[(self.next() % xs.len() as u64) as usize]
        }
    }

    /// random words: 35% FP / SIMD, 20% loads and stores, 10% data processing (register), 35% any; the entry and end
    /// around pc pick between goto and call. Gives each word's lift-words input line and the line it prints
    fn lift_words(n: usize, each: &mut dyn FnMut(String, String)) {
        let mut r = Rng(1);
        for _ in 0..n {
            let k = r.next() % 100;
            let mut w = r.next() as u32;
            if k < 35 {
                w |= 7 << 25;
            } else if k < 55 {
                w = (w & !(0xf << 25)) | (r.pick(&[4, 6, 12, 14]) << 25);
            } else if k < 65 {
                w = (w & !(0xf << 25)) | (r.pick(&[5, 13]) << 25);
            }
            let pc = 0x7100000000 + (r.next() % (1 << 22)) as i64 * 4;
            let (entry, end) = (pc - r.pick(&[0, 4, 64, 4096]), pc + r.pick(&[4, 8, 256, 1 << 16]));
            let (s, br) = lift_word(w, pc, entry, end);
            each(format!("{:x} {:x} {:x} {:x}\n", w, pc, entry, end), format!("{}{}\n", if br { "BR " } else { "" }, s));
        }
    }

    #[test]
    fn lift_words_digest() {
        const N: usize = 4_000_000;
        let mut h = Sha256::new();
        lift_words(N, &mut |_, l| h.update(l.as_bytes()));
        let digest = format!("{}\n", crate::util::hex(&h.finalize()));
        if std::fs::read_to_string(crate::golden::path("lift_words.sha256")).ok().as_deref() != Some(digest.as_str()) {
            // another build's output for the same words: wtbuild lift-words < wtbuild-lift_words.in
            let tmp = |x: &str| std::env::temp_dir().join(format!("wtbuild-lift_words-{}.{}", std::process::id(), x));
            let (i, o) = (tmp("in"), tmp("txt"));
            let (mut fi, mut fo) = (std::io::BufWriter::new(std::fs::File::create(&i).unwrap()), std::io::BufWriter::new(std::fs::File::create(&o).unwrap()));
            lift_words(N, &mut |j, l| {
                fi.write_all(j.as_bytes()).unwrap();
                fo.write_all(l.as_bytes()).unwrap();
            });
            eprintln!("lift-words input {}, output {}", i.display(), o.display());
        }
        crate::golden::check("lift_words.sha256", &digest);
    }

    /// ldaxp / stlxp ([Recomps#1]) for every register and both sizes: register 31 is xzr / wzr, never SP
    #[test]
    fn exclusive_pairs() {
        let x = |n: u32| if n == 31 { "0ULL".to_string() } else { format!("c->x[{}]", n) };
        let w = |n: u32| if n == 31 { "0U".to_string() } else { format!("(uint32_t)c->x[{}]", n) };
        let dst = |n: u32, t: &str| if n == 31 { format!("(void)({});", t) } else { format!("c->x[{}] = (uint64_t)({});", n, t) };
        for sf in [false, true] {
            for (rt, rt2, rn) in (0..32 * 32 * 32).map(|i| (i & 31, (i >> 5) & 31, i >> 10)) {
                let ins = (sf as u32) << 30 | rt2 << 10 | rn << 5 | rt;
                let (t1, t2) = if sf { ("t1", "t2") } else { ("(uint32_t)t", "t >> 32") };
                let (s, _) = lift_word(0x887F8000 | ins, 0x7101000000, 0x7100000000, 0x7102000000);
                for want in [dst(rt, t1), dst(rt2, t2), format!("uint64_t a = c->x[{}];", rn)] {
                    assert!(s.contains(&want), "ldaxp {:08x}: {} lacks {}", 0x887F8000 | ins, s, want);
                }
                for rs in 0..32 {
                    let e = if sf {
                        format!("stx128(c, c->x[{}], {}, {})", rn, x(rt), x(rt2))
                    } else {
                        format!("stx64(c, c->x[{}], (uint64_t)(uint32_t){} | ((uint64_t)(uint32_t){} << 32))", rn, w(rt), w(rt2))
                    };
                    let want = if rs == 31 { format!("(void)({});", e) } else { format!("c->x[{}] = (uint32_t)({});", rs, e) };
                    let (s, _) = lift_word(0x88208000 | rs << 16 | ins, 0x7101000000, 0x7100000000, 0x7102000000);
                    assert!(s.contains(&want), "stlxp {:08x}: {} lacks {}", 0x88208000 | rs << 16 | ins, s, want);
                }
            }
        }
    }
}
