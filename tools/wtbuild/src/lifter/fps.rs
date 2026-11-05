//! Frame rates above 60 (docs/STATUS.md, "Frame rates above 60"): the game's per-frame accumulations, found by their
//! form in the machine code. A site is a load of a field, an add or subtract on the loaded value (an immediate, a
//! register, a float register) and a store of the result to the same field ([Xn + off]), in straight-line code (a
//! conditional branch between them is allowed) with Xn unchanged. The census (WTBUILD_FPS_CENSUS=1 at lift time, then a
//! run with WT_FPS_CENSUS=<file>, at 60 and at above 60) records what each visit adds: a site that adds as much per
//! visit at 120 as at 60 is frame-locked; one that adds half as much takes the frame's time already. Many sites are
//! not timers (loop counters in memory, reference counts): the frame-locked ones are read one by one.

use super::Image;
use crate::util::{bits, sext};

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Kind {
    /// integer field of 1 / 2 / 4 bytes, + or - an immediate (delta: the signed amount)
    Int { size: u32, delta: i32 },
    /// integer field + or - a register
    IntReg { size: u32 },
    /// float field, +1.0 or -1.0
    F1 { delta: i32 },
    /// float field + or - a float register
    FReg,
}

#[derive(Clone, Copy, Debug)]
pub struct Site {
    pub load: i64,
    pub op: i64,
    pub store: i64,
    /// the field is x[base] + off at the op and at the store
    pub base: u32,
    pub off: i64,
    pub kind: Kind,
    /// the register holding the loaded value at the op; F1: the register holding the +/-1.0
    pub val: u32,
    pub one: u32,
    /// the register stored (the op's result, or a CSEL's that takes it)
    pub src: u32,
}

/// a load or store of a general (fp = false) or SIMD&FP register with a base register and an immediate offset:
/// (is_load, fp, size in bytes, rt, rn, offset); pre/post-index forms are not taken (they write the base)
fn ldst(w: u32) -> Option<(bool, bool, u32, u32, u32, i64)> {
    let (rt, rn) = (w & 31, (w >> 5) & 31);
    let size = 1u32 << (w >> 30);
    let fp = bits(w, 26, 26) == 1;
    let opc = bits(w, 23, 22);
    if bits(w, 29, 27) != 0b111 {
        return None;
    }
    let load = match opc {
        0 => false,
        1 => true,
        _ => return None, // sign-extending loads, 128-bit q forms
    };
    if bits(w, 25, 24) == 0b01 {
        // unsigned offset, scaled
        return Some((load, fp, size, rt, rn, (bits(w, 21, 10) * size) as i64));
    }
    if bits(w, 25, 24) == 0b00 && bits(w, 21, 21) == 0 && bits(w, 11, 10) == 0 {
        // unscaled (LDUR / STUR)
        return Some((load, fp, size, rt, rn, sext(bits(w, 20, 12) as i64, 9)));
    }
    None
}

/// ADD / SUB / ADDS / SUBS (immediate, 32-bit, no shift) Wd, Wn, #imm: (rd, rn, +imm / -imm)
fn addsub_imm(w: u32) -> Option<(u32, u32, i32)> {
    let imm = bits(w, 21, 10) as i32;
    if w & 0x9f800000 != 0x11000000 || bits(w, 22, 22) != 0 || imm == 0 || w & 31 == 31 {
        return None;
    }
    Some((w & 31, (w >> 5) & 31, if bits(w, 30, 30) == 1 { -imm } else { imm }))
}

/// ADD / SUB / ADDS / SUBS (shifted register, 32-bit, no shift) Wd, Wn, Wm: (rd, rn, rm, sub)
fn addsub_reg(w: u32) -> Option<(u32, u32, u32, bool)> {
    if w & 0x9fe0fc00 != 0x0b000000 || w & 31 == 31 {
        return None;
    }
    Some((w & 31, (w >> 5) & 31, (w >> 16) & 31, bits(w, 30, 30) == 1))
}

/// FADD / FSUB Sd, Sn, Sm (single): (rd, rn, rm, sub)
fn faddsub(w: u32) -> Option<(u32, u32, u32, bool)> {
    match w & 0xffe0fc00 {
        0x1e202800 => Some((w & 31, (w >> 5) & 31, (w >> 16) & 31, false)),
        0x1e203800 => Some((w & 31, (w >> 5) & 31, (w >> 16) & 31, true)),
        _ => None,
    }
}

/// the sign of the +/-1.0 that w (the last write of a SIMD&FP register before pc) leaves: FMOV Sd, #1.0 / #-1.0, or
/// FMOV Sd, Wn with Wn last set by MOV Wn, #0x3f800000 / #0xbf800000
fn one(im: &Image, lo: i64, pc: i64, w: u32) -> Option<i32> {
    match w & 0xffffffe0 {
        0x1e2e1000 => return Some(1),
        0x1e3e1000 => return Some(-1),
        _ => {}
    }
    if w & 0xfffffc00 != 0x1e270000 {
        return None;
    }
    let n = (w >> 5) & 31;
    let mut q = pc - 4;
    while q >= lo {
        let v = im.word(q);
        if is_flow(v) {
            return None;
        }
        if writes_gpr(v, n) {
            return match v & 0xffffffe0 {
                0x52a7f000 => Some(1),
                0x52b7f000 => Some(-1),
                _ => None,
            };
        }
        q -= 4;
    }
    None
}

/// branches, calls, returns, exception generation, system: the end of straight-line code. A conditional branch
/// (B.cond, CBZ / CBNZ, TBZ / TBNZ) is not: `if (t > 0) t--` tests before the step (the path that skips it leaves)
fn is_flow(w: u32) -> bool {
    let cond = w & 0xff000010 == 0x54000000 || w & 0x7e000000 == 0x34000000 || w & 0x7e000000 == 0x36000000;
    bits(w, 28, 26) == 0b101 && !cond
}

/// CSEL / CSINC / CSINV / CSNEG (32-bit): (rd, rn, rm)
fn csel(w: u32) -> Option<(u32, u32, u32)> {
    (w & 0xffe00000 == 0x1a800000 || w & 0xffe00000 == 0x5a800000).then_some((w & 31, (w >> 5) & 31, (w >> 16) & 31))
}

/// whether w may write general register r: anything with r in bits 0-4 but a store (conservative: a site is lost, never
/// taken wrongly)
fn writes_gpr(w: u32, r: u32) -> bool {
    if w & 31 != r || cond_target(w, 0).is_some() {
        return false; // a conditional branch's low bits are its condition or register tested
    }
    if let Some((load, fp, ..)) = ldst(w) {
        return load && !fp;
    }
    // a store pair (bit 25 clear: ORR / ADDS shifted register share bits 29-27 and do write)
    !(bits(w, 29, 27) == 0b101 && bits(w, 25, 25) == 0 && bits(w, 22, 22) == 0)
}

/// the last instruction in [lo, at) of the straight-line code before at that writes SIMD&FP register r, as far as the
/// forms below tell: Some((pc, word)) or None
fn fp_def(im: &Image, lo: i64, at: i64, r: u32) -> Option<(i64, u32)> {
    let mut pc = at - 4;
    while pc >= lo {
        let w = im.word(pc);
        if is_flow(w) {
            return None;
        }
        let fp_dp = bits(w, 28, 25) & 0b0111 == 0b0111;
        let fp_ld = matches!(ldst(w), Some((true, true, ..)));
        if (fp_dp || fp_ld) && w & 31 == r {
            return Some((pc, w));
        }
        pc -= 4;
    }
    None
}

/// the target of a conditional branch at pc (B.cond, CBZ / CBNZ, TBZ / TBNZ)
fn cond_target(w: u32, pc: i64) -> Option<i64> {
    if w & 0xff000010 == 0x54000000 || w & 0x7e000000 == 0x34000000 {
        return Some(pc + 4 * sext(bits(w, 23, 5) as i64, 19));
    }
    (w & 0x7e000000 == 0x36000000).then(|| pc + 4 * sext(bits(w, 18, 5) as i64, 14))
}

/// the register known to hold the base n after w: a `mov xd, xn` (ORR xd, xzr, xn) makes it d, a write ends it
fn step_alias(a: Option<u32>, w: u32, n: u32) -> Option<u32> {
    if w & 0xffe0ffe0 == 0xaa0003e0 && (w >> 16) & 31 == n && w & 31 != 31 {
        return Some(w & 31);
    }
    a.filter(|&r| !writes_gpr(w, r))
}

const WIN: i64 = 8 * 4; // at most 8 instructions from the load to the op, and from the op to the store

/// every counter site of the function [entry, end)
pub fn sites(im: &Image, entry: i64, end: i64) -> Vec<Site> {
    let mut out = Vec::new();
    let mut pc = entry;
    while pc < end {
        let w = im.word(pc);
        pc += 4;
        let Some((true, fp, size, rt, rn, off)) = ldst(w) else { continue };
        if rn == 31 || (fp && size != 4) || (!fp && size > 4) {
            continue; // stack slots; 64-bit fields
        }
        let load = pc - 4;
        // the op that reads rt, before rt or the base change
        let mut op = None;
        let mut q = pc;
        while q < end && q <= load + WIN {
            let v = im.word(q);
            if is_flow(v) {
                break;
            }
            if !fp {
                if let Some((rd, n, d)) = addsub_imm(v) {
                    if n == rt {
                        op = Some((q, rd, Kind::Int { size, delta: d }, 0));
                        break;
                    }
                }
                if let Some((rd, n, m, sub)) = addsub_reg(v) {
                    if n == rt || (m == rt && !sub) {
                        op = Some((q, rd, Kind::IntReg { size }, if n == rt { m } else { n }));
                        break;
                    }
                }
                if writes_gpr(v, rt) || writes_gpr(v, rn) {
                    break;
                }
            } else {
                if let Some((rd, n, m, sub)) = faddsub(v) {
                    let other = if n == rt { Some(m) } else if m == rt && !sub { Some(n) } else { None };
                    if let Some(k) = other {
                        match fp_def(im, entry, q, k).and_then(|(p, d)| one(im, entry, p, d)) {
                            Some(s) => op = Some((q, rd, Kind::F1 { delta: if sub { -s } else { s } }, k)),
                            None => op = Some((q, rd, Kind::FReg, k)),
                        }
                        break;
                    }
                }
                if writes_gpr(v, rn) {
                    break;
                }
            }
            q += 4;
        }
        let Some((opc, rd, kind, one)) = op else { continue };
        // a copy of the base (mov xd, xn) between the load and the store: the store may name it (`mov x19, x0` at entry)
        let mut alias = (load + 4..opc).map(|p| im.word(p)).fold(None, |a, v| step_alias(a, v, rn));
        // the store of rd (or of a CSEL that takes it: t = max(t - 1, 0)) to the same field, also the target of a
        // conditional branch (two paths that count the field down into one store)
        let mut rd = rd;
        let mut s = opc + 4;
        while s < end && s <= opc + WIN {
            let v = im.word(s);
            if is_flow(v) {
                break;
            }
            let stores = |w: u32| {
                matches!(ldst(w), Some((false, sfp, ssize, srt, srn, soff))
                    if sfp == fp && ssize == size && srt == rd && (srn == rn || Some(srn) == alias) && soff == off)
            };
            if stores(v) {
                out.push(Site { load, op: opc, store: s, base: rn, off, kind, val: rt, one, src: rd });
                break;
            }
            if let Some(t) = cond_target(v, s).filter(|t| (entry..end).contains(t) && stores(im.word(*t))) {
                out.push(Site { load, op: opc, store: t, base: rn, off, kind, val: rt, one, src: rd });
                break;
            }
            alias = step_alias(alias, v, rn);
            if let Some((d, a, b)) = csel(v).filter(|_| !fp) {
                if (a == rd || b == rd) && d != rn && d != 31 {
                    rd = d;
                    s += 4;
                    continue;
                }
            }
            if writes_gpr(v, rn) || (!fp && writes_gpr(v, rd)) {
                break;
            }
            s += 4;
        }
    }
    out
}

/// WTBUILD_FPS_CENSUS=1 at lift time: every site's store reports its site, field and the value stored first
/// (runtime/game_shim_eh.c fps_census; a test build: the normal lift writes the files back)
pub fn census_on() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("WTBUILD_FPS_CENSUS").map(|v| v == "1").unwrap_or(false))
}

/// tools/wtbuild/fps_counters.txt: the store address of every site of main whose add counts the game time due
/// (fps_ci / fps_cf / fps_fs). Taken with main's instruction patches (eh::options_row_patches): a stand-in (`lift
/// --no-patches`) has none
pub fn counters() -> std::collections::HashSet<i64> {
    include_str!("../../fps_counters.txt")
        .lines()
        .filter_map(|l| l.split_whitespace().next())
        .filter(|w| !w.starts_with('#'))
        .map(|w| i64::from_str_radix(w, 16).expect("fps_counters.txt: a hex store address"))
        .collect()
}

/// the op statement of a listed site with its +/-1 made the frames due: Err when the lifted text is not one of the forms
pub fn patch_op(s: &Site, stmt: &str) -> Result<String, String> {
    let key = format!("0x{}ULL", crate::util::hx(s.store));
    let field = format!("c->x[{}] + (uint64_t){}LL", s.base, s.off);
    let (from, to) = match s.kind {
        Kind::Int { delta, .. } => {
            let a = delta.abs();
            let k = format!("(uint32_t)fps_ci(c, {}, {}, (uint32_t)c->x[{}], {}, {})", key, field, s.val, a, delta.signum());
            if stmt.contains(&format!("+ {}ULL)", a)) {
                (format!("+ {}ULL)", a), format!("+ (uint64_t){})", k))
            } else if stmt.contains(&format!("- {}ULL)", a)) {
                (format!("- {}ULL)", a), format!("- (uint64_t){})", k))
            } else if stmt.contains(&format!("~(uint32_t){}ULL, 1)", a)) {
                (format!("~(uint32_t){}ULL, 1)", a), format!("~{}, 1)", k))
            } else {
                (format!("(uint32_t){}ULL, 0)", a), format!("{}, 0)", k))
            }
        }
        Kind::F1 { delta } => {
            let one = format!("c->v[{}].f[0]", s.one);
            let to = format!("({} * (float)fps_cf(c, {}, {}, c->v[{}].f[0], {}))", one, key, field, s.val, delta);
            return float_operand(s, stmt, &one, &to);
        }
        // x +/- a, a any float: a x this update's step (frames of game time; fractional, so motion stays smooth)
        Kind::FReg if s.one != s.val => {
            let a = format!("c->v[{}].f[0]", s.one);
            let to = format!("({} * fps_fs(c, {}, {}))", a, key, field);
            return float_operand(s, stmt, &a, &to);
        }
        Kind::IntReg { .. } | Kind::FReg => return Err(format!("fps_counters.txt: {}: an add of a register or of x to itself is not patched", crate::util::hx(s.store))),
    };
    if stmt.matches(&from).count() != 1 {
        return Err(format!("fps_counters.txt: {}: the op at {} is not a form the patch knows: {}", crate::util::hx(s.store), crate::util::hx(s.op), stmt));
    }
    Ok(stmt.replacen(&from, &to, 1))
}

/// the operand `from` of the float add's expression (`fnan32(..);`, not the store of the result after it) made `to`
fn float_operand(s: &Site, stmt: &str, from: &str, to: &str) -> Result<String, String> {
    let bad = || format!("fps_counters.txt: {}: the op at {} is not a form the patch knows: {}", crate::util::hx(s.store), crate::util::hx(s.op), stmt);
    let a = stmt.find("fnan32(").ok_or_else(bad)?;
    let b = a + stmt[a..].find(");").ok_or_else(bad)?;
    let e = &stmt[a..b];
    if e.matches(from).count() != 1 {
        return Err(bad());
    }
    Ok(format!("{}{}{}", &stmt[..a], e.replacen(from, to, 1), &stmt[b..]))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn forms() {
        // ldr w8, [x19, #0x10]; ldur s0, [x0, #-4]; str w8, [x19, #0x10]; ldp: not a single load
        assert_eq!(ldst(0xb9401268), Some((true, false, 4, 8, 19, 0x10)));
        assert_eq!(ldst(0xbc5fc000), Some((true, true, 4, 0, 0, -4)));
        assert_eq!(ldst(0xb9001268), Some((false, false, 4, 8, 19, 0x10)));
        assert_eq!(ldst(0x29400000), None);
        // add w8, w8, #1; subs w9, w8, #1; add w8, w8, #2 is not one
        assert_eq!(addsub_imm(0x11000508), Some((8, 8, 1)));
        assert_eq!(addsub_imm(0x71000509), Some((9, 8, -1)));
        assert_eq!(addsub_imm(0x11000908), Some((8, 8, 2)));
        // add w8, w8, w9; sub w8, w9, w8
        assert_eq!(addsub_reg(0x0b090108), Some((8, 8, 9, false)));
        assert_eq!(addsub_reg(0x4b080128), Some((8, 9, 8, true)));
        // fadd s0, s0, s1; fsub s2, s3, s4; fmov s1, #1.0; fmov s1, #-1.0
        assert_eq!(faddsub(0x1e212800), Some((0, 0, 1, false)));
        assert_eq!(faddsub(0x1e243862), Some((2, 3, 4, true)));
        // b.lt, cbz w8, tbnz w8, #3 are not the end of straight-line code; b, bl, ret are
        assert!(!is_flow(0x5400004b) && !is_flow(0x34000048) && !is_flow(0x37180048));
        assert!(is_flow(0x14000002) && is_flow(0x94000002) && is_flow(0xd65f03c0));
        // csel w8, w9, wzr, gt
        assert_eq!(csel(0x1a9fc128), Some((8, 9, 31)));
        // cmp w8, #1 writes no register
        assert_eq!(addsub_imm(0x7100051f), None);
        // mov x19, x2 and adds w8, w8, w9 write their register; stp x29, x30, [sp, #16] does not
        assert!(writes_gpr(0xaa0203f3, 19) && writes_gpr(0x2b090108, 8) && !writes_gpr(0xa9017bfd, 29));
        // b.lt (condition 11) and cbz w8 write no register
        assert!(!writes_gpr(0x5400006b, 11) && !writes_gpr(0x34000048, 8));
    }

    /// the store through a copy of the base and the store reached by a conditional branch (the stick tracker's forms,
    /// 0x710055a7ec: its first octant through x0 / x19, the double-push window counted down on two paths)
    #[test]
    fn site_paths() {
        let words: [u32; 23] = [
            // ldr w8, [x0, #0x178]; mov x20, x1; mov x19, x0; subs w9, w8, #1; b.lt +12; mov w8, w9; str w9, [x19, #0x178]; ret
            0xb9417808, 0xaa0103f4, 0xaa0003f3, 0x71000509, 0x5400006b, 0x2a0903e8, 0xb9017a69, 0xd65f03c0,
            // 0x20: ldr w8, [x19, #0x1ec]; subs w8, w8, #1; b.lt +8; str w8, [x19, #0x1ec]; ret
            0xb941ee68, 0x71000508, 0x5400004b, 0xb901ee68, 0xd65f03c0,
            // 0x34: ldr w8, [x19, #0x1ec]; subs w8, w8, #1; b.ge 0x2c; b 0x30
            0xb941ee68, 0x71000508, 0x54ffff8a, 0x17fffffc,
            // 0x44: ldr w8, [x0, #16]; mov x19, x0; mov x19, x2; sub w8, w8, #1; str w8, [x19, #16]; ret: another object
            0xb9401008, 0xaa0003f3, 0xaa0203f3, 0x51000508, 0xb9001268, 0xd65f03c0,
        ];
        let mut img = vec![0u8; 0x1000];
        img.extend(words.iter().flat_map(|w| w.to_le_bytes()));
        let b = 0x7100000000i64;
        let im = Image {
            img,
            base: b,
            file_off: 0x1000,
            text_size: 4 * words.len() as i64,
            rela: 0,
            relasz: 0,
            jmprel: 0,
            pltrelsz: 0,
            symtab: 0,
            strtab: 0,
            patches: Default::default(),
            fps_counters: Default::default(),
            b3: false,
        };
        let got: Vec<(i64, i64, i64, u32, i64)> =
            sites(&im, b, b + 4 * words.len() as i64).iter().map(|s| (s.load - b, s.op - b, s.store - b, s.base, s.off)).collect();
        assert_eq!(got, vec![(0x00, 0x0c, 0x18, 0, 0x178), (0x20, 0x24, 0x2c, 19, 0x1ec), (0x34, 0x38, 0x2c, 19, 0x1ec)]);
        assert_eq!(cond_target(0x54ffff8a, b + 0x3c), Some(b + 0x2c));
        assert_eq!(cond_target(0x17fffffc, b + 0x40), None);
    }
}
