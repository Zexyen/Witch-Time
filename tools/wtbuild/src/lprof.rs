//! `wtbuild lprof`: the report of an SSBU_LPROF sample file (runtime/lprof.c, Linux).
//! Per thread, the functions most often innermost ("self") and anywhere in the sampled chain ("incl", once per sample).
//! PCs are named from the mapped files' own symbol tables, the ones `nm` lists (lifted functions are f_<guest address>).

use crate::util::{hx, u16_at, u32_at, u64_at};
use std::collections::HashMap;

const IDLE: [&str; 11] = [
    "wait",
    "syscall",
    "sleep",
    "poll",
    "sched_yield",
    "pause",
    "sigtimedwait",
    "recv",
    "accept",
    "select",
    "ioctl",
];

/// (address, name) of the symbols `nm -n --defined-only` lists as t, T, W or i (the Python kept tTwWiI; w and I are
/// never defined), or when there are none, of `nm -D` (with the version nm appends: name@@VERSION, name@VERSION hidden)
pub fn elf_syms(d: &[u8]) -> Vec<(u64, String)> {
    // shortcut: ELF64 little endian only (what Linux x86-64 and AArch64 map); others get no names, like a stripped file
    if d.len() < 64 || &d[..4] != b"\x7fELF" || d[4] != 2 || d[5] != 1 {
        return Vec::new();
    }
    let get = |o: u64, n: u64| d.get(o as usize..o.checked_add(n)? as usize);
    let (shoff, shnum) = (u64_at(d, 0x28), u16_at(d, 0x3c) as u64);
    let Some(sh) = get(shoff, 64 * shnum) else { return Vec::new() };
    // (type, flags, contents, link)
    let secs: Vec<(u32, u64, &[u8], usize)> = sh
        .chunks_exact(64)
        .map(|h| {
            let body = if u32_at(h, 4) == 8 { None } else { get(u64_at(h, 24), u64_at(h, 32)) };
            (u32_at(h, 4), u64_at(h, 8), body.unwrap_or(&[]), u32_at(h, 40) as usize)
        })
        .collect();
    let text = |s: &[u8], o: usize| {
        let s = s.get(o..).unwrap_or(&[]);
        String::from_utf8_lossy(&s[..s.iter().position(|&c| c == 0).unwrap_or(s.len())]).into_owned()
    };
    let find = |ty: u32| secs.iter().find(|s| s.0 == ty);
    let strings = |link: usize| secs.get(link).map_or(&[][..], |t| t.2);
    // version index -> name, from .gnu.version_d; index 1 with the base flag is the file itself and gets none.
    // shortcut: .gnu.version_r is not read: only undefined or copied data symbols carry another file's versions
    let mut vers = HashMap::new();
    if let Some(vd) = find(0x6ffffffd) {
        let mut o = 0;
        while let Some(h) = vd.2.get(o..o + 20) {
            let aux = o + u32_at(h, 12) as usize;
            if u16_at(h, 2) & 1 == 0 && vd.2.len() >= aux + 4 {
                vers.insert(u16_at(h, 4) & 0x7fff, text(strings(vd.3), u32_at(vd.2, aux) as usize));
            }
            match u32_at(h, 16) {
                0 => break,
                n => o += n as usize,
            }
        }
    }
    let versym = find(0x6fffffff).map_or(&[][..], |s| s.2);
    let table = |ty: u32| {
        let mut out = Vec::new();
        let Some(st) = find(ty) else { return out };
        for (i, s) in st.2.chunks_exact(24).enumerate().skip(1) {
            let (bind, kind, shndx) = (s[4] >> 4, s[4] & 15, u16_at(s, 6) as usize);
            // nm's i (STT_GNU_IFUNC), W (weak, not an object) and t / T (local or global in a SHF_EXECINSTR section); not
            // undefined, common, section or file symbols
            let code = shndx < 0xff00 && secs.get(shndx).is_some_and(|x| x.1 & 4 != 0);
            let listed = shndx != 0 && shndx != 0xfff2 && kind != 3 && kind != 4 && (kind == 10 || (bind == 2 && kind != 1 && kind != 5) || (bind < 2 && code));
            if !listed {
                continue;
            }
            let mut name = text(strings(st.3), u32_at(s, 0) as usize);
            if ty == 11 && versym.len() >= 2 * i + 2 {
                let v = u16_at(versym, 2 * i);
                if let Some(ver) = vers.get(&(v & 0x7fff)).filter(|ver| **ver != name) {
                    name = format!("{}{}{}", name, if v & 0x8000 != 0 { "@" } else { "@@" }, ver);
                }
            }
            // as the Python read nm's lines: the name's first word, and no nameless symbol
            if let Some(n) = name.split_whitespace().next() {
                out.push((u64_at(s, 8), n.to_string()));
            }
        }
        out
    };
    let syms = table(2);
    if syms.is_empty() {
        table(11)
    } else {
        syms
    }
}

/// the symbols of a mapped file; nothing for one that is gone or not a regular file (a device)
pub fn file_syms(path: &str) -> Vec<(u64, String)> {
    match std::fs::metadata(path) {
        Ok(m) if m.is_file() => std::fs::read(path).map(|d| elf_syms(&d)).unwrap_or_default(),
        _ => Vec::new(),
    }
}

/// counts in first-seen order, as collections.Counter keeps them; most_common sorts stably
#[derive(Default)]
struct Counter {
    at: HashMap<usize, usize>,
    counts: Vec<(usize, usize)>,
}

impl Counter {
    fn add(&mut self, k: usize) {
        let n = self.counts.len();
        let i = *self.at.entry(k).or_insert(n);
        if i == n {
            self.counts.push((k, 0));
        }
        self.counts[i].1 += 1;
    }
    fn most_common(mut self, n: usize) -> Vec<(usize, usize)> {
        self.counts.sort_by_key(|c| std::cmp::Reverse(c.1));
        self.counts.truncate(n);
        self.counts
    }
}

/// the report: `nthr` threads, `nfn` functions per list; `syms` reads a mapped file's (address, name) table
pub fn report(text: &str, nthr: usize, nfn: usize, syms: &mut dyn FnMut(&str) -> Vec<(u64, String)>) -> Vec<String> {
    let mut samples: Vec<(i64, String, Vec<u64>)> = Vec::new();
    let mut maps: Vec<(u64, u64, u64, &str)> = Vec::new();
    for line in text.lines() {
        if line.starts_with("S ") {
            let mut p: Vec<&str> = line.split_whitespace().collect();
            if p.last().is_some_and(|t| t.starts_with('@')) {
                p.pop(); // the presented frame (newer files)
            }
            // older files have names with spaces; a name's last word that looks like hex counts as a PC, as it did
            let mut k = p.len();
            while k > 3 && p[k - 1].bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)) {
                k -= 1;
            }
            let Some(tid) = p.get(1).and_then(|t| t.parse().ok()) else { continue };
            let pcs = p[k..].iter().filter_map(|x| u64::from_str_radix(x, 16).ok()).collect();
            samples.push((tid, p[2..k].join("_"), pcs));
        } else if let Some(m) = line.strip_prefix("M ") {
            let p: Vec<&str> = m.split_whitespace().collect();
            if p.len() >= 6 && p[5].starts_with('/') {
                let r = p[0]
                    .split_once('-')
                    .and_then(|(lo, hi)| Some((u64::from_str_radix(lo, 16).ok()?, u64::from_str_radix(hi, 16).ok()?)));
                if let (Some((lo, hi)), Ok(off)) = (r, u64::from_str_radix(p[2], 16)) {
                    maps.push((lo, hi, off, p[5]));
                }
            }
        }
    }
    let mut bias: HashMap<&str, u64> = HashMap::new();
    for &(lo, _, off, f) in &maps {
        if off == 0 {
            bias.entry(f).or_insert(lo);
        }
    }
    // names are numbered: pc -> name id, and the ids' texts
    let mut tables: HashMap<&str, (Vec<u64>, Vec<String>)> = HashMap::new();
    let mut id_of_pc: HashMap<u64, usize> = HashMap::new();
    let (mut names, mut id_of_name): (Vec<String>, HashMap<String, usize>) = (Vec::new(), HashMap::new());
    let mut name = |pc: u64| -> usize {
        if let Some(&i) = id_of_pc.get(&pc) {
            return i;
        }
        let mut r = format!("?{:x}", pc);
        if let Some(&(lo, _, _, f)) = maps.iter().find(|m| m.0 <= pc && pc < m.1) {
            let short = f.rsplit('/').next().unwrap();
            let (addrs, ns) = tables.entry(f).or_insert_with(|| {
                let mut rows = syms(f);
                rows.sort();
                rows.into_iter().unzip()
            });
            let v = pc as i64 - *bias.get(f).unwrap_or(&lo) as i64;
            let i = addrs.partition_point(|&a| a as i128 <= v as i128);
            r = if i > 0 {
                format!("{}:{}", short, ns[i - 1])
            } else {
                format!("{}+{}", short, hx(v))
            };
        }
        let n = names.len();
        let i = *id_of_name.entry(r.clone()).or_insert(n);
        if i == n {
            names.push(r);
        }
        id_of_pc.insert(pc, i);
        i
    };
    let chains: Vec<Vec<usize>> = samples.iter().map(|s| s.2.iter().map(|&pc| name(pc)).collect()).collect();
    // threads by (id, name) in first-seen order: (key, samples, busy samples)
    let mut threads: Vec<((i64, &str), usize, Vec<Vec<usize>>)> = Vec::new();
    let mut at: HashMap<(i64, &str), usize> = HashMap::new();
    for ((tid, nm, _), ids) in samples.iter().zip(chains) {
        let n = threads.len();
        let t = *at.entry((*tid, nm.as_str())).or_insert(n);
        if t == n {
            threads.push(((*tid, nm.as_str()), 0, Vec::new()));
        }
        threads[t].1 += 1;
        // busy: the innermost frame does not wait in libc / the kernel (futex, poll, sleep, a driver ioctl)
        let idle = ids
            .first()
            .is_some_and(|&i| names[i].starts_with("libc.so") && IDLE.iter().any(|w| names[i].contains(w)));
        if !idle {
            threads[t].2.push(ids);
        }
    }
    let mut out = vec![format!(
        "{} samples, {} threads; busy = innermost frame not waiting in libc",
        samples.len(),
        threads.len()
    )];
    threads.sort_by_key(|t| std::cmp::Reverse(t.2.len()));
    threads.truncate(nthr);
    for ((tid, nm), n, busy) in &threads {
        out.push(format!(
            "  thread {} {:<28} busy {:5.1}% of {} samples",
            tid,
            nm,
            100.0 * busy.len() as f64 / (*n).max(1) as f64,
            n
        ));
    }
    for ((tid, nm), _, busy) in threads {
        if busy.is_empty() {
            continue;
        }
        let (mut own, mut incl) = (Counter::default(), Counter::default());
        for ids in &busy {
            if let Some(&i) = ids.first() {
                own.add(i);
            }
            // each name once per sample; the Python's set() gave ties among them no fixed order, this takes the innermost first
            for (j, &i) in ids.iter().enumerate() {
                if !ids[..j].contains(&i) {
                    incl.add(i);
                }
            }
        }
        out.push(String::new());
        out.push(format!("== thread {} {}: {} busy samples (percentages of those)", tid, nm, busy.len()));
        for (title, c) in [("  self:", own), ("  incl:", incl)] {
            out.push(title.into());
            for (i, k) in c.most_common(nfn) {
                out.push(format!("    {:5.1}%  {}", 100.0 * k as f64 / busy.len() as f64, names[i]));
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    /// NUL-terminated names and their offsets
    fn strtab(names: &[&str]) -> (Vec<u8>, Vec<u32>) {
        let mut t = vec![0u8];
        let offs = names
            .iter()
            .map(|n| {
                let o = t.len() as u32;
                t.extend(n.as_bytes());
                t.push(0);
                o
            })
            .collect();
        (t, offs)
    }
    fn sym(name: u32, info: u8, shndx: u16, value: u64) -> Vec<u8> {
        [&name.to_le_bytes()[..], &[info, 0], &shndx.to_le_bytes(), &value.to_le_bytes(), &[0; 8]].concat()
    }
    /// an x86-64 shared object of (name, type, flags, contents, link, info) after section 0, with .shstrtab last
    fn elf(secs: &[(&str, u32, u64, Vec<u8>, u32, u32)]) -> Vec<u8> {
        let (shstr, names) = strtab(&secs.iter().map(|s| s.0).chain([".shstrtab"]).collect::<Vec<_>>());
        let mut all = secs.to_vec();
        all.push((".shstrtab", 3, 0, shstr, 0, 0));
        let (mut d, mut sh) = (vec![0u8; 64], vec![0u8; 64]);
        for (i, (_, ty, flags, body, link, info)) in all.iter().enumerate() {
            d.resize(d.len().next_multiple_of(8), 0);
            let ent: u64 = match ty {
                2 | 11 => 24,
                0x6fffffff => 2,
                _ => 0,
            };
            let (off, size) = ((d.len() as u64).to_le_bytes(), (body.len() as u64).to_le_bytes());
            let fields: [&[u8]; 10] = [
                &names[i].to_le_bytes(),
                &ty.to_le_bytes(),
                &flags.to_le_bytes(),
                &[0; 8],
                &off,
                &size,
                &link.to_le_bytes(),
                &info.to_le_bytes(),
                &[8, 0, 0, 0, 0, 0, 0, 0],
                &ent.to_le_bytes(),
            ];
            sh.extend(fields.concat());
            d.extend(body);
        }
        d.resize(d.len().next_multiple_of(8), 0);
        let shoff = d.len() as u64;
        d.extend(sh);
        d[..7].copy_from_slice(b"\x7fELF\x02\x01\x01");
        d[16..24].copy_from_slice(&[3, 0, 62, 0, 1, 0, 0, 0]);
        d[0x28..0x30].copy_from_slice(&shoff.to_le_bytes());
        for (o, v) in [(0x34, 64u16), (0x3a, 64), (0x3c, all.len() as u16 + 1), (0x3e, all.len() as u16)] {
            d[o..o + 2].copy_from_slice(&v.to_le_bytes());
        }
        d
    }

    #[test]
    fn symbols_as_nm_lists_them() {
        // .symtab: a file and a section symbol, t, T twice at one address, W in data, weak and global objects, an
        // undefined function, an indirect function in data, a nameless function
        let (st, n) = strtab(&["a.c", "loc", "glob", "tie", "wk", "wobj", "dat", "und", "ifn", ""]);
        let syms = [
            vec![0; 24],
            sym(n[0], 0x04, 0xfff1, 0),
            sym(0, 0x03, 1, 0),
            sym(n[1], 0x02, 1, 0x10),
            sym(n[2], 0x12, 1, 0x20),
            sym(n[3], 0x12, 1, 0x20),
            sym(n[4], 0x20, 2, 0x100),
            sym(n[5], 0x21, 2, 0x108),
            sym(n[6], 0x11, 2, 0x110),
            sym(n[7], 0x12, 0, 0),
            sym(n[8], 0x1a, 2, 0x118),
            sym(n[9], 0x12, 1, 0x30),
        ]
        .concat();
        let a = elf(&[
            (".text", 1, 6, vec![0; 0x40], 0, 0),
            (".data", 1, 3, vec![0; 0x20], 0, 0),
            (".symtab", 2, 0, syms, 4, 4),
            (".strtab", 3, 0, st, 0, 0),
        ]);
        let want = [(0x10, "loc"), (0x20, "glob"), (0x20, "tie"), (0x100, "wk"), (0x118, "ifn")];
        assert_eq!(elf_syms(&a), want.map(|(v, s)| (v, s.to_string())));
        // no .symtab: the dynamic symbols, with their versions (V_1 the default, V_2 hidden for g); the definition of V_1
        // itself is absolute
        let (ds, n) = strtab(&["libx.so", "V_1", "V_2", "f", "g", "h", "k"]);
        let dsyms = [
            vec![0; 24],
            sym(n[1], 0x11, 0xfff1, 0),
            sym(n[3], 0x12, 1, 0x10),
            sym(n[4], 0x12, 1, 0x20),
            sym(n[5], 0x12, 1, 0x30),
            sym(n[6], 0x22, 1, 0x40),
        ];
        let versym: Vec<u8> = [0u16, 2, 2, 0x8003, 1, 3].iter().flat_map(|v| v.to_le_bytes()).collect();
        let mut verdef = Vec::new();
        for (i, (flags, name)) in [(1u16, n[0]), (0, n[1]), (0, n[2])].into_iter().enumerate() {
            let next = if i == 2 { 0u32 } else { 28 };
            let h = [1u16, flags, i as u16 + 1, 1].iter().flat_map(|v| v.to_le_bytes()).collect::<Vec<_>>();
            verdef.extend([h, [0u32, 20, next, name, 0].iter().flat_map(|v| v.to_le_bytes()).collect()].concat());
        }
        let b = elf(&[
            (".text", 1, 6, vec![0; 0x50], 0, 0),
            (".dynsym", 11, 2, dsyms.concat(), 3, 1),
            (".dynstr", 3, 2, ds, 0, 0),
            (".gnu.version", 0x6fffffff, 2, versym, 2, 0),
            (".gnu.version_d", 0x6ffffffd, 2, verdef, 3, 3),
        ]);
        let want = [(0x10, "f@@V_1"), (0x20, "g@V_2"), (0x30, "h"), (0x40, "k@@V_2")];
        assert_eq!(elf_syms(&b), want.map(|(v, s)| (v, s.to_string())));
        assert!(elf_syms(b"\x7fELF\x01\x01").is_empty());
    }

    #[test]
    fn report_of_a_sample() {
        // a name with a space, an older line without the frame, a wait in libc, a PC outside every mapping and one before
        // the first symbol, two names at one address, a name whose last word reads as hex
        let text = "# lprof 1.0 s, 7 samples, 1 rounds
S 10 Main Thread 560000000050 5600000000a0 @1
S 10 Main Thread 7f0000001010 5600000000b4
S 10 Main Thread 7f0000002000 5600000000a0 @3
S 11 - 999 5600000000a0 5600000000a0 @3
S 12 cafe 5600000000a0 @4
S 13 beef dead 5600000000a0 @4
S 14 idle 7f0000002000 @5
M 560000000000-560000001000 r-xp 00000000 00:00 0 /x/game.so
M 7f0000000000-7f0000001000 r--p 00000000 00:00 0 /usr/lib/libc.so.6
M 7f0000001000-7f0000003000 r-xp 00001000 00:00 0 /usr/lib/libc.so.6
M 7f0000004000-7f0000005000 rw-p 00000000 00:00 0 [heap]
";
        let mut syms = |f: &str| -> Vec<(u64, String)> {
            let t: &[(u64, &str)] = match f {
                "/x/game.so" => &[(0xa0, "f_a"), (0xb0, "f_b2"), (0xb0, "f_b")],
                "/usr/lib/libc.so.6" => &[(0x1000, "memcpy@@GLIBC_2.2.5"), (0x2000, "__futex_abstimed_wait64@@GLIBC_PRIVATE")],
                _ => &[],
            };
            t.iter().map(|&(a, n)| (a, n.to_string())).collect()
        };
        // lprof_report.py's output with an nm that prints these tables (its set() order of the incl ties pinned)
        let want = "7 samples, 5 threads; busy = innermost frame not waiting in libc
  thread 10 Main_Thread                  busy  66.7% of 3 samples
  thread 11 -                            busy 100.0% of 1 samples
  thread 12 cafe                         busy 100.0% of 1 samples
  thread 13 beef                         busy 100.0% of 1 samples
  thread 14 idle                         busy   0.0% of 1 samples

== thread 10 Main_Thread: 2 busy samples (percentages of those)
  self:
     50.0%  game.so+50
     50.0%  libc.so.6:memcpy@@GLIBC_2.2.5
  incl:
     50.0%  game.so+50
     50.0%  game.so:f_a
     50.0%  libc.so.6:memcpy@@GLIBC_2.2.5
     50.0%  game.so:f_b2

== thread 11 -: 1 busy samples (percentages of those)
  self:
    100.0%  ?999
  incl:
    100.0%  ?999
    100.0%  game.so:f_a

== thread 12 cafe: 1 busy samples (percentages of those)
  self:
    100.0%  game.so:f_a
  incl:
    100.0%  game.so:f_a

== thread 13 beef: 1 busy samples (percentages of those)
  self:
    100.0%  ?dead
  incl:
    100.0%  ?dead
    100.0%  game.so:f_a";
        assert_eq!(report(text, 6, 25, &mut syms).join("\n"), want);
    }
}
