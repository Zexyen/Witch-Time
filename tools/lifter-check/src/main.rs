//! The lifter's semantic check (developers): words lifted by the shipped lifter (`wtbuild lift-words`) run in Unicorn
//! and, as the lifted statement, in a generated C program built on the real runtime helpers (-I runtime); all 32 V
//! registers, X0..X30, SP and a 256-byte buffer must come out the same. Replaces lifter/test_ext.py and
//! lifter/test_b3.py, which checked the Python lifter. Never part of a release: Unicorn is GPL-2.0.
//!
//!   lifter-check [--main <main.elf> | --synthetic N] [--trials N] [--seed N] [--keep] [--wtbuild <path>]
//!
//! --main (default targets/exefs/main.elf): every word of the player's .text in one of test_ext's FORMS, and every
//! AdvSIMD word whose statement uses a [witchtime] form of test_b3 (b3_form) or that is a one-lane / replicate
//! ld2-4 / st2-4. --synthetic N: N random words of FORMS that Unicorn's A57 accepts (no dump needed). A selected word
//! that lifts to UNIMPL or to the wrong helper fails. A stlxp runs twice: with no exclusive monitor (it fails) and after
//! an LDXP from its base (it stores). --keep keeps the C, its program and the trials (`./check < trials`).

use std::collections::{BTreeSet, HashMap};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use unicorn_engine::{uc_error, Arch, Arm64CpuModel, Mode, Prot, RegisterARM64, Unicorn};

const BASE: u64 = 0x7100000000;
/// the guest buffer, at the same address in Unicorn and in the C program; a load / store's base register points 64 in
const BUF: u64 = 0x10000000;
const PAGE: usize = 0x1000;
/// the buffer bytes that start random and are compared; the rest of the page must stay zero
const MEM: usize = 256;
/// a 16-byte slot per case, never rewritten (no stale translation): CLREX, so no exclusive monitor is open, NOP or an
/// LDXP that opens one, the word
const CODE: u64 = 0x1000000;
const CODE_SIZE: u64 = 0x100000;
const CLREX: u32 = 0xd503305f;
const NOP: u32 = 0xd503201f;

/// (mask, value, name, what its statement must contain): test_ext.py's forms
const FORMS: [(u32, u32, &str, &str); 8] = [
    (0xBF20FC00, 0x0E20C000, "smull", "v_long3("),        // SMULL{2} <Vd>.<Ta>, <Vn>.<Tb>, <Vm>.<Tb> (U=0, opcode 1100)
    (0xBF20FC00, 0x0E20A000, "smlsl", "v_long3("),        // SMLSL{2} (opcode 1010)
    (0xBF3FFC00, 0x0E214800, "sqxtn", "v_qxtn("),         // SQXTN{2} (U=0)
    (0xBF80FC00, 0x0F00FC00, "fcvtzs", "v_fcvtz_fix("),   // FCVTZS (vector, fixed-point), immh != 0
    (0xBF80FC00, 0x0F009C00, "sqrshrn", "v_sqshrn("),     // SQRSHRN{2}, immh != 0
    (0xBF80FC00, 0x0F002400, "srshr", "v_rshr("),         // SRSHR (vector), immh != 0
    (0xBFFF8000, 0x887F8000, "ldaxp", "c->ex_addr = a;"), // LDAXP <Rt>, <Rt2>, [<Xn>]
    (0xBFE08000, 0x88208000, "stlxp", "(stx"),            // STLXP <Ws>, <Rt>, <Rt2>, [<Xn>]
];

/// test_b3.py's [witchtime] AdvSIMD helpers
const B3_HELPERS: [&str; 10] = [
    "v_i3x",
    "v_addp",
    "v_addsubl",
    "v_across",
    "v_frint",
    "v_fcvta",
    "v_addlp",
    "v_sra",
    "v_shrn",
    "v_ielem",
];

fn die(msg: impl std::fmt::Display) -> ! {
    eprintln!("lifter-check: {}", msg);
    std::process::exit(1)
}

/// the FORMS entry of w (immh = 0 makes the three shifts an AdvSIMD modified immediate)
fn form(w: u32) -> Option<usize> {
    FORMS
        .iter()
        .position(|&(m, v, ..)| w & m == v)
        .filter(|&i| !(3..6).contains(&i) || w >> 19 & 0xf != 0)
}

/// a statement of test_b3.py's NEW: a [witchtime] helper, orr / bic (vector, immediate) or scalar addp
fn b3_form(s: &str) -> bool {
    B3_HELPERS.iter().any(|h| s.contains(h))
        || s.match_indices("c->v[").any(|(i, _)| {
            let r = &s[i + 5..];
            let t = r.trim_start_matches(|c: char| c.is_ascii_digit());
            t.len() < r.len()
                && t.strip_prefix("].d[0] ")
                    .is_some_and(|t| ["&= ~", "|=", "+ c->v"].iter().any(|p| t.starts_with(p)))
        })
}

fn is_simd(w: u32) -> bool {
    w >> 25 & 7 == 7 || matches!(w >> 24 & 0x3f, 0x0c | 0x0d)
}

/// one-lane / replicate structure load-store with 2..4 registers (the [witchtime] part of the ldN decoder)
fn lane_ldst(w: u32) -> bool {
    w >> 24 & 0xbf == 0x0d && (w >> 21 & 1 != 0 || w >> 13 & 1 != 0)
}

/// an LDXP from the base of the stlxp w into two registers it does not use: it opens the monitor the store needs
fn ldxp_for(w: u32) -> u32 {
    let used = [w & 31, w >> 5 & 31, w >> 10 & 31, w >> 16 & 31];
    let mut free = (0..31).filter(|r| !used.contains(r));
    let (a, b) = (free.next().unwrap(), free.next().unwrap());
    w & 0x40000000 | 0x887f0000 | b << 10 | w & 0x3e0 | a
}

/// Unicorn's id of X<n>: X0..X28 are contiguous, X29, X30 and SP are not (UC_ARM64_REG_X0 + 29 is V0)
fn xreg(n: usize) -> i32 {
    match n {
        29 => RegisterARM64::X29 as i32,
        30 => RegisterARM64::X30 as i32,
        31 => RegisterARM64::SP as i32,
        _ => RegisterARM64::X0 as i32 + n as i32,
    }
}

/// splitmix64
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15);
        let z = (self.0 ^ self.0 >> 30).wrapping_mul(0xbf58476d1ce4e5b9);
        let z = (z ^ z >> 27).wrapping_mul(0x94d049bb133111eb);
        z ^ z >> 31
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
    /// uniform in [-r, r)
    fn uniform(&mut self, r: f64) -> f64 {
        (self.next() >> 11) as f64 / (1u64 << 52) as f64 * r - r
    }
    /// float lanes: ordinary, NaN, saturating, infinite and tiny values, which random bits almost never are
    fn float_vec(&mut self, dbl: bool) -> u128 {
        let (lanes, bits) = if dbl { (2, 64) } else { (4, 32) };
        (0..lanes).fold(0, |v, i| {
            let x = match self.below(5) {
                0 => self.uniform(4.0),
                1 => self.uniform(1e5),
                2 => f64::NAN,
                3 => [1e30, -1e30, f64::INFINITY, f64::NEG_INFINITY][self.below(4) as usize],
                _ => self.uniform(1.0) * (-20f64).exp2(),
            };
            let lane = if dbl { x.to_bits() as u128 } else { (x as f32).to_bits() as u128 };
            v | lane << (i * bits)
        })
    }
    /// 30% of the time a saturation edge of sqxtn and sqrshrn
    fn int_vec(&mut self) -> u128 {
        const EDGES: [u128; 6] = [
            0,
            !0,
            0x7fff7fff7fff7fff7fff7fff7fff7fff,
            0x80008000800080008000800080008000,
            0x7fffffff7fffffff7fffffff7fffffff,
            0x80000000800000008000000080000000,
        ];
        if self.below(10) < 3 {
            EDGES[self.below(6) as usize]
        } else {
            (self.next() as u128) << 64 | self.next() as u128
        }
    }
}

#[derive(Clone)]
struct State {
    v: [u128; 32],
    x: [u64; 32],
    m: [u8; MEM],
}

/// random registers and buffer for w; a load / store's base register points into the buffer
fn inputs(rng: &mut Rng, w: u32, stmt: &str) -> State {
    let float = ["v_frint", "v_fcvta", "v_fcvtz_fix"].iter().any(|h| stmt.contains(h));
    let mut s = State {
        v: [0; 32],
        x: [0; 32],
        m: [0; MEM],
    };
    for v in &mut s.v {
        // bit 22: .2d / double in the fixed-point, frint and fcvta encodings
        *v = if float { rng.float_vec(w >> 22 & 1 != 0) } else { rng.int_vec() };
    }
    for x in &mut s.x {
        *x = rng.next();
    }
    for b in &mut s.m {
        *b = rng.next() as u8;
    }
    if w >> 25 & 5 == 4 {
        s.x[(w >> 5 & 31) as usize] = BUF + 64;
        let rm = (w >> 16 & 31) as usize;
        if w >> 24 & 0xbe == 0x0c && w >> 23 & 1 != 0 && rm != 31 {
            // structure load / store, post-indexed by a register
            s.x[rm] = [16, 32, -16i64 as u64][rng.below(3) as usize];
        }
    }
    s
}

struct Emu<'a> {
    uc: Unicorn<'a, ()>,
    slots: u64,
}

impl Emu<'_> {
    fn new() -> Self {
        let mut uc = Unicorn::new(Arch::ARM64, Mode::ARM).unwrap_or_else(|e| die(format!("Unicorn: {:?}", e)));
        // the Switch's core; then FP / SIMD on (CPACR_EL1.FPEN)
        let ok = |r: Result<(), uc_error>| r.unwrap_or_else(|e| die(format!("Unicorn: {:?}", e)));
        ok(uc.ctl_set_cpu_model(Arm64CpuModel::A57 as i32));
        ok(uc.reg_write(RegisterARM64::CPACR_EL1, 3 << 20));
        ok(uc.mem_map(CODE, CODE_SIZE, Prot::ALL));
        ok(uc.mem_map(BUF, PAGE as u64, Prot::ALL));
        Emu { uc, slots: 0 }
    }

    fn place(&mut self, pre: u32, w: u32) -> u64 {
        let at = CODE + self.slots * 16;
        self.slots += 1;
        if at + 16 > CODE + CODE_SIZE {
            die("more words than code slots");
        }
        self.uc
            .mem_write(at, &[CLREX, pre, w].map(u32::to_le_bytes).concat())
            .unwrap_or_else(|e| die(format!("Unicorn: {:?}", e)));
        at
    }

    fn run(&mut self, at: u64, s: &State) -> Result<State, uc_error> {
        let mut page = [0u8; PAGE];
        page[..MEM].copy_from_slice(&s.m);
        for i in 0..32 {
            self.uc.reg_write_long(RegisterARM64::V0 as i32 + i as i32, &s.v[i].to_le_bytes())?;
            self.uc.reg_write(xreg(i), s.x[i])?;
        }
        self.uc.mem_write(BUF, &page)?;
        self.uc.emu_start(at, at + 12, 0, 0)?;
        let mut o = s.clone();
        for i in 0..32 {
            o.v[i] = u128::from_le_bytes(self.uc.reg_read_long(RegisterARM64::V0 as i32 + i as i32)?[..16].try_into().unwrap());
            o.x[i] = self.uc.reg_read(xreg(i))?;
        }
        self.uc.mem_read(BUF, &mut o.m)?;
        Ok(o)
    }
}

/// .text and .plt: the first PT_LOAD's file size (wtbuild's lifter::layout)
fn text_size(img: &[u8]) -> usize {
    let rd = |o: usize, n: usize| {
        img.get(o..o + n)
            .unwrap_or_else(|| die("not an ELF"))
            .iter()
            .rev()
            .fold(0, |a, &x| a << 8 | x as usize)
    };
    if !img.starts_with(b"\x7fELF") {
        die("not an ELF");
    }
    (0..rd(0x38, 2))
        .map(|i| rd(0x20, 8) + i * 0x38)
        .find(|&p| rd(p, 4) == 1)
        .map(|p| rd(p + 32, 8))
        .unwrap_or_else(|| die("ELF without PT_LOAD"))
}

/// `wtbuild lift-words`, at BASE + 0x1000000 in a function BASE..end as both Python checks lifted
fn lift(wtbuild: &Path, words: &[u32], end: u64) -> Vec<String> {
    let mut child = Command::new(wtbuild)
        .arg("lift-words")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap_or_else(|e| {
            die(format!(
                "{}: {} (cargo build --release --manifest-path tools/wtbuild/Cargo.toml)",
                wtbuild.display(),
                e
            ))
        });
    let input: String = words.iter().map(|w| format!("{:x} {:x} {:x} {:x}\n", w, BASE + 0x1000000, BASE, end)).collect();
    let mut stdin = child.stdin.take().unwrap();
    let feed = std::thread::spawn(move || stdin.write_all(input.as_bytes()));
    let out = child.wait_with_output().unwrap_or_else(|e| die(e));
    feed.join().unwrap().unwrap_or_else(|e| die(format!("lift-words: {}", e)));
    let lines: Vec<String> = String::from_utf8_lossy(&out.stdout)
        .lines()
        .map(|l| l.strip_prefix("BR ").unwrap_or(l).to_string())
        .collect();
    if !out.status.success() || lines.len() != words.len() {
        die(format!("lift-words failed ({}, {} lines for {} words)", out.status, lines.len(), words.len()));
    }
    lines
}

/// n distinct random words of FORMS that Unicorn runs
fn synthetic(n: usize, rng: &mut Rng, emu: &mut Emu) -> Vec<u32> {
    let (mut seen, mut out) = (BTreeSet::new(), vec![]);
    while out.len() < n {
        let (m, v, name, _) = FORMS[rng.below(FORMS.len() as u64) as usize];
        let w = v | rng.next() as u32 & !m;
        let (rt, rn, rt2, rs) = (w & 31, w >> 5 & 31, w >> 10 & 31, w >> 16 & 31);
        // CONSTRAINED UNPREDICTABLE register overlaps: Unicorn picks one behaviour, the lifter need not pick the same
        let unpredictable = match name {
            "ldaxp" => rt == rt2,
            "stlxp" => rs == rt || rs == rt2 || rs == rn && rn != 31,
            _ => false,
        };
        if form(w).is_none() || unpredictable || !seen.insert(w) {
            continue;
        }
        // a word the A57 rejects (a reserved size, FP16) is not a Switch instruction
        let at = emu.place(NOP, w);
        let probe = inputs(rng, w, "");
        if emu.run(at, &probe).is_ok() {
            out.push(w);
        }
    }
    out
}

const PRELUDE: &str = r#"/* generated by tools/lifter-check: statements lifted from YOUR main.elf or from random words -- do not commit */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "runtime.h"
#include "rt_ext.h"

typedef struct { uint64_t v[64], x[32]; uint8_t m[256]; } State;
static int fails;
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {}
"#;

const MAIN: &str = r#"
/* counts a failure; the first of a word (for 20 words; a word's trials come in a row) prints the word and its statement,
 * and returns 1 for the caller to say what differs */
static int fail(uint32_t w, uint32_t k) {
    static uint32_t shown = ~0u;
    static int words;
    fails++;
    if (w == shown || words == 20) return 0;
    shown = w, words++;
    printf("FAIL %08x  %s\n    ", w, stmt[k]);
    return 1;
}

static struct { uint32_t k, w; State in, out; } t;
void guest_unimpl(uint64_t pc, uint32_t w, const char *why) { if (fail(t.w, t.k)) printf("guest_unimpl: %s\n", why); }

int main(void) {
    static Ctx ctx; Ctx *c = &ctx;
    uint8_t *buf = mmap((void *)0x10000000ULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (buf != (uint8_t *)0x10000000ULL) { puts("cannot map the test buffer"); return 2; }
    /* stx128: succeeds when memory still holds what LDXP saw, fails otherwise */
    { static uint64_t mem[2] __attribute__((aligned(16))) = { 1, 2 }; uint64_t a = (uint64_t)(uintptr_t)mem;
      c->ex_addr = a; c->ex_val = 1; c->ex_val2 = 2;
      if (stx128(c, a, 5, 6) != 0 || mem[0] != 5 || mem[1] != 6) { fails++; printf("FAIL stx128 success case\n"); }
      c->ex_addr = a; c->ex_val = 1; c->ex_val2 = 2;
      if (stx128(c, a, 7, 8) != 1 || mem[0] != 5 || mem[1] != 6) { fails++; printf("FAIL stx128 stale case\n"); }
      if (stx128(c, a, 7, 8) != 1) { fails++; printf("FAIL stx128 without monitor\n"); } }
    long cases = 3;
    while (fread(&t, sizeof t, 1, stdin) == 1) {
        cases++;
        memcpy(c->v, t.in.v, sizeof t.in.v);
        memcpy(c->x, t.in.x, sizeof t.in.x);
        memset(buf, 0, 0x1000);
        memcpy(buf, t.in.m, sizeof t.in.m);
        c->ex_addr = ~0ull;   /* no open monitor, as after Unicorn's CLREX */
        run(c, t.k);
        int i = 0;
        while (i < 32 && c->v[i].d[0] == t.out.v[2 * i] && c->v[i].d[1] == t.out.v[2 * i + 1]) i++;
        if (i < 32 && fail(t.w, t.k))
            printf("v%d: in %016llx%016llx got %016llx%016llx want %016llx%016llx\n", i, (unsigned long long)t.in.v[2 * i + 1],
                   (unsigned long long)t.in.v[2 * i], (unsigned long long)c->v[i].d[1], (unsigned long long)c->v[i].d[0],
                   (unsigned long long)t.out.v[2 * i + 1], (unsigned long long)t.out.v[2 * i]);
        for (i = 0; i < 32; i++)
            if (c->x[i] != t.out.x[i] && fail(t.w, t.k))
                printf("x%d: in %016llx got %016llx want %016llx\n", i, (unsigned long long)t.in.x[i], (unsigned long long)c->x[i],
                       (unsigned long long)t.out.x[i]);
        for (i = 0; i < 256 && buf[i] == t.out.m[i]; i++) ;
        if (i < 256 && fail(t.w, t.k)) printf("buffer + %d: in %02x got %02x want %02x\n", i, t.in.m[i], buf[i], t.out.m[i]);
        for (i = 256; i < 0x1000 && !buf[i]; i++) ;
        if (i < 0x1000 && fail(t.w, t.k)) printf("wrote past the compared bytes, at buffer + %d\n", i);
    }
    printf("%ld cases, %d failures\n", cases, fails);
    return fails != 0;
}
"#;

/// the C program: each distinct statement once, as a case of run()
fn c_program(stmts: &[(&str, Vec<u32>)]) -> String {
    let mut c = String::from(PRELUDE);
    c += "\nstatic const char *const stmt[] = {\n";
    for (s, _) in stmts {
        // Debug quoting is C quoting for printable ASCII, which the lifter writes
        c += &format!("    {:?},\n", s);
    }
    c += "};\n\nstatic void run(Ctx *c, uint32_t k) {\n    switch (k) {\n";
    for (k, (s, ws)) in stmts.iter().enumerate() {
        let ws: Vec<String> = ws.iter().map(|w| format!("{:08x}", w)).collect();
        c += &format!("    case {}: /* {} */ {{ {} }} break;\n", k, ws.join(" "), s);
    }
    c + "    }\n}\n" + MAIN
}

/// a trial as the C reads it: k, w, then the state in and out (v as 64 halves, low first; x; the buffer)
fn record(k: u32, w: u32, i: &State, o: &State) -> Vec<u8> {
    let mut r = [k.to_le_bytes(), w.to_le_bytes()].concat();
    for s in [i, o] {
        s.v.iter().for_each(|v| r.extend(v.to_le_bytes()));
        s.x.iter().for_each(|x| r.extend(x.to_le_bytes()));
        r.extend(s.m);
    }
    r
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--main" | "--synthetic" | "--trials" | "--seed" | "--wtbuild" if i + 1 < args.len() => i += 2,
            "--keep" => i += 1,
            _ => die("usage: lifter-check [--main <main.elf> | --synthetic N] [--trials N] [--seed N] [--keep] [--wtbuild <path>]"),
        }
    }
    let opt = |k: &str| args.iter().position(|a| a == k).map(|i| args[i + 1].as_str());
    let num = |k: &str, d: u64| opt(k).map_or(d, |s| s.parse().unwrap_or_else(|_| die(format!("{} {}: not a number", k, s))));
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join("..");
    let wtbuild = opt("--wtbuild").map(PathBuf::from).unwrap_or_else(|| {
        repo.join("tools/wtbuild/target/release")
            .join(format!("wtbuild{}", std::env::consts::EXE_SUFFIX))
    });
    let trials = num("--trials", 300);
    let mut rng = Rng(num("--seed", 1));
    let keep = args.iter().any(|a| a == "--keep");
    if opt("--main").is_some() && opt("--synthetic").is_some() {
        die("--main or --synthetic, not both");
    }

    let mut emu = Emu::new();
    let mut fails = vec![];
    // the selected words, their statements and kinds
    let (mut words, mut names) = (vec![], vec![]);
    let end;
    if let Some(n) = opt("--synthetic") {
        let n = n.parse().unwrap_or_else(|_| die(format!("--synthetic {}: not a number", n)));
        let ws = synthetic(n, &mut rng, &mut emu);
        end = BASE + 0x2000000;
        for (w, s) in ws.iter().zip(lift(&wtbuild, &ws, end)) {
            let f = FORMS[form(*w).unwrap()];
            if s.contains(f.3) {
                names.push(f.2);
                words.push((*w, s));
            } else {
                fails.push(format!("{:08x} {} lifts to: {}", w, f.2, s));
            }
        }
    } else {
        let elf = opt("--main").map_or_else(|| repo.join("targets/exefs/main.elf"), PathBuf::from);
        let img = std::fs::read(&elf).unwrap_or_else(|e| die(format!("{}: {}", elf.display(), e)));
        let size = text_size(&img);
        let text = img
            .get(0x1000..0x1000 + size)
            .unwrap_or_else(|| die(format!("{}: .text past the end", elf.display())));
        let ws: BTreeSet<u32> = text.chunks_exact(4).map(|b| u32::from_le_bytes(b.try_into().unwrap())).collect();
        let ws: Vec<u32> = ws.into_iter().filter(|&w| form(w).is_some() || is_simd(w)).collect();
        end = BASE + size as u64;
        for (w, s) in ws.iter().copied().zip(lift(&wtbuild, &ws, end)) {
            let name = match form(w) {
                Some(f) if !s.contains(FORMS[f].3) => {
                    fails.push(format!("{:08x} {} lifts to: {}", w, FORMS[f].2, s));
                    continue;
                }
                Some(f) => FORMS[f].2,
                None if lane_ldst(w) && s.starts_with("UNIMPL") => {
                    fails.push(format!("{:08x} ldN / stN lane lifts to: {}", w, s));
                    continue;
                }
                None if !s.starts_with("UNIMPL") && (b3_form(&s) || lane_ldst(w)) => "[witchtime] AdvSIMD",
                None => continue,
            };
            names.push(name);
            words.push((w, s));
        }
    }
    // (word, its code slot, statement): a stlxp also after an LDXP, so that it stores (CLREX alone makes it fail)
    let stlxp: Vec<u32> = words.iter().filter(|(w, _)| form(*w) == Some(7)).map(|(w, _)| ldxp_for(*w)).collect();
    let mut pre = stlxp.iter().zip(lift(&wtbuild, &stlxp, end));
    let mut cases: Vec<(u32, u64, String)> = vec![];
    for (w, s) in words {
        cases.push((w, emu.place(NOP, w), s.clone()));
        if form(w) == Some(7) {
            let (p, ps) = pre.next().unwrap();
            if ps.contains(FORMS[6].3) {
                cases.push((w, emu.place(*p, w), format!("{} {}", ps, s)));
            } else {
                fails.push(format!("{:08x} ldxp lifts to: {}", p, ps));
            }
        }
    }
    let mut count: Vec<(&str, usize)> = vec![];
    for &n in &names {
        match count.iter_mut().find(|c| c.0 == n) {
            Some(c) => c.1 += 1,
            None => count.push((n, 1)),
        }
    }
    let count: Vec<String> = count.iter().map(|(n, c)| format!("{} {}", n, c)).collect();
    println!("{} words: {}", names.len(), count.join(", "));
    for f in &fails {
        println!("FAIL {}", f);
    }

    // each distinct statement once
    let mut stmts: Vec<(&str, Vec<u32>)> = vec![];
    let mut index: HashMap<&str, usize> = HashMap::new();
    let ks: Vec<u32> = cases
        .iter()
        .map(|(w, _, s)| {
            let k = *index.entry(s.as_str()).or_insert_with(|| {
                stmts.push((s, vec![]));
                stmts.len() - 1
            });
            stmts[k].1.push(*w);
            k as u32
        })
        .collect();
    let dir = std::env::temp_dir().join(format!("lifter-check-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap_or_else(|e| die(format!("{}: {}", dir.display(), e)));
    let (src, exe) = (dir.join("check.c"), dir.join("check"));
    std::fs::write(&src, c_program(&stmts)).unwrap_or_else(|e| die(e));
    let st = Command::new("clang")
        .args(["-O1", "-w", "-mcx16"])
        .args(std::env::var("LIFTER_CHECK_CFLAGS").unwrap_or_default().split_whitespace()) // e.g. -march=x86-64-v3 -O2
        .arg("-I")
        .arg(repo.join("runtime"))
        .arg(&src)
        .arg("-o")
        .arg(&exe)
        .arg("-lm")
        .status()
        .unwrap_or_else(|e| die(format!("clang: {}", e)));
    if !st.success() {
        die(format!("clang failed on {}", src.display()));
    }

    let mut child = Command::new(&exe)
        .stdin(Stdio::piped())
        .spawn()
        .unwrap_or_else(|e| die(format!("{}: {}", exe.display(), e)));
    let mut pipe = BufWriter::new(child.stdin.take().unwrap());
    let mut saved = keep.then(|| BufWriter::new(std::fs::File::create(dir.join("trials")).unwrap_or_else(|e| die(e))));
    let mut n = 0;
    'feed: for ((w, at, s), k) in cases.iter().zip(ks) {
        for _ in 0..trials {
            let i = inputs(&mut rng, *w, s);
            match emu.run(*at, &i) {
                Ok(o) => {
                    let r = record(k, *w, &i, &o);
                    if let Some(f) = &mut saved {
                        f.write_all(&r).unwrap_or_else(|e| die(e));
                    }
                    if pipe.write_all(&r).is_err() {
                        break 'feed; // the program stopped: its status says why
                    }
                    n += 1;
                }
                Err(e) => {
                    let f = format!("{:08x} Unicorn {:?}: {}", w, e, s);
                    println!("FAIL {}", f);
                    fails.push(f);
                    break;
                }
            }
        }
    }
    drop(pipe);
    drop(saved);
    let st = child.wait().unwrap_or_else(|e| die(e));
    if !matches!(st.code(), Some(0 | 1)) {
        println!("FAIL {}: {}", exe.display(), st);
    }
    println!("{} statements, {} trials in Unicorn; {} failures outside the C", stmts.len(), n, fails.len());
    if keep {
        println!("kept {} (./check < trials)", dir.display());
    } else {
        let _ = std::fs::remove_dir_all(&dir);
    }
    std::process::exit(if st.success() && fails.is_empty() { 0 } else { 1 })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn selection() {
        // smull v0.2d, v1.2s, v2.2s; immh = 0 is movi, not fcvtzs; srshr v0.16b, v1.16b, #4; ldaxp x18, x0, [x8];
        // stlxp w0, x18, x15, [x16]
        assert_eq!(form(0x0ea2c020), Some(0));
        assert_eq!(form(0x0f00fc00), None);
        assert_eq!(form(0x4f0c2420), Some(5));
        assert_eq!(form(0xc87f8112), Some(6));
        assert_eq!(form(0xc820be12), Some(7));
        assert!(b3_form("v_addp(&c->v[2], &c->v[2], &c->v[3], 2, 0);"));
        assert!(b3_form("{ c->v[0].d[0] &= ~ 0xf0000000f0000000ULL; c->v[0].d[1] = 0; }"));
        assert!(b3_form("{ c->v[0].d[0] |= 0x0ULL; c->v[0].d[1] = 0; }"));
        assert!(b3_form("{ uint64_t t = c->v[1].d[0] + c->v[1].d[1]; c->v[0].d[0] = t; c->v[0].d[1] = 0; }"));
        assert!(!b3_form("{ c->v[0].d[0] = 0x4000000000000000ULL; c->v[0].d[1] = 0; }"));
        assert!(!b3_form("c->v[].d[0] |= 1;"));
        // st4 {v0.s-v3.s}[0], [x9]; ld1 {v0.s}[0], [x9] has one register
        assert!(lane_ldst(0x0d20a120) && is_simd(0x0d20a120));
        assert!(!lane_ldst(0x0d408120));
        // stlxp w0, x18, x15, [x16] stores after ldxp x1, x2, [x16]
        assert_eq!(ldxp_for(0xc820be12), 0xc87f0a01);
    }

    #[test]
    fn registers() {
        // add x29, sp, #0; add x30, x29, #1; orr v0.16b, v31.16b, v31.16b: X29, X30, SP and the V registers reach Unicorn
        let mut emu = Emu::new();
        let i = inputs(&mut Rng(7), 0, "");
        let mut run = |w| {
            let at = emu.place(NOP, w);
            emu.run(at, &i).unwrap()
        };
        assert_eq!(run(0x910003fd).x[29], i.x[31]);
        assert_eq!(run(0x910007be).x[30], i.x[29].wrapping_add(1));
        let o = run(0x4ebf1fe0);
        assert_eq!((o.v[0], &o.v[1..], o.x), (i.v[31], &i.v[1..], i.x));
        assert_eq!(xreg(28), RegisterARM64::X28 as i32);
    }

    #[test]
    fn layout() {
        // the trial record is the C's struct { uint32_t k, w; State in, out; }
        let s = inputs(&mut Rng(1), 0, "");
        assert_eq!(record(0, 0, &s, &s).len(), 8 + 2 * (64 * 8 + 32 * 8 + MEM));
    }
}
