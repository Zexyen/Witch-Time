//! NSO -> ELF, ported from ssbu-recomp's tools/extract/nso2elf.py (GPL-3.0). LZ4 segments, the SHA-256 of each segment
//! checked against the NSO header; reads no keys. The ELF is byte for byte the one nso2elf.py wrote (the lift test's
//! golden pins its hash).

use crate::util::{hex, i32_at, put_u64, u32_at, u64_at};
use sha2::{Digest, Sha256};
use std::collections::HashMap;

pub struct Converted {
    pub elf: Vec<u8>,
    /// the conversion's lines: segment hashes, build ID, dynamic
    pub report: Vec<String>,
    /// every segment's hash matched
    pub hashes_ok: bool,
}

pub fn nso2elf(d: &[u8]) -> Result<Converted, String> {
    if d.len() < 0x100 || &d[..4] != b"NSO0" {
        return Err("not an NSO file".into());
    }
    let mut report = Vec::new();
    let mut hashes_ok = true;
    let flags = u32_at(d, 12);
    let mut segs: Vec<(usize, Vec<u8>)> = Vec::new();
    for (i, (ho, cso)) in [(0x10usize, 0x60usize), (0x20, 0x64), (0x30, 0x68)].into_iter().enumerate() {
        let (foff, moff, size) = (u32_at(d, ho) as usize, u32_at(d, ho + 4) as usize, u32_at(d, ho + 8) as usize);
        let csz = u32_at(d, cso) as usize;
        let raw = d.get(foff..foff + csz).ok_or("NSO segment past the end of the file")?;
        let raw = if flags & (1 << i) != 0 {
            lz4_flex::block::decompress(raw, size).map_err(|e| format!("segment {}: LZ4: {}", i, e))?
        } else {
            raw.to_vec()
        };
        if raw.len() != size {
            return Err(format!("segment {}: {} bytes, the header says {}", i, raw.len(), size));
        }
        let ok = Sha256::digest(&raw)[..] == d[0xA0 + i * 0x20..0xC0 + i * 0x20];
        hashes_ok &= ok;
        report.push(format!(
            "seg {} vaddr {:x} size {:x} sha256 {}",
            i,
            moff,
            size,
            if ok { "OK" } else { "MISMATCH" }
        ));
        segs.push((moff, raw));
    }
    let bss = u32_at(d, 0x3c) as usize;
    report.push(format!("build id {}", hex(&d[0x40..0x54])));
    let (tv, ref text) = segs[0];
    let (rv, ref ro) = segs[1];
    let (dv, ref data) = segs[2];
    let mut mem = vec![0u8; dv + data.len()];
    for (v, r) in &segs {
        mem[*v..*v + r.len()].copy_from_slice(r);
    }
    // MOD0 -> dynamic
    let m = u32_at(text, 4) as usize;
    if mem.get(m..m + 4) != Some(b"MOD0") {
        return Err("no MOD0 header".into());
    }
    let dyn_off = i32_at(&mem, m + 4) as i64;
    let (bss_s, bss_e) = (i32_at(&mem, m + 8) as i64, i32_at(&mem, m + 12) as i64);
    let dynv = (m as i64 + dyn_off) as usize;
    let mut tags: HashMap<i64, u64> = HashMap::new();
    let mut n = 0;
    loop {
        let t = u64_at(&mem, dynv + n * 16) as i64;
        let v = u64_at(&mem, dynv + n * 16 + 8);
        n += 1;
        if t == 0 {
            break;
        }
        tags.entry(t).or_insert(v);
    }
    let dynsz = n * 16;
    let tag = |t: i64| tags.get(&t).copied().ok_or(format!("DYNAMIC has no tag {}", t));
    let (symtab, strtab, strsz) = (tag(6)? as usize, tag(5)? as usize, tag(10)? as usize);
    let nsym = match tags.get(&4) {
        Some(&h) => u32_at(&mem, h as usize + 4) as usize,
        None => (strtab - symtab) / 24,
    };
    report.push(format!("dynamic {:x} dynsym {:x} ({} syms) dynstr {:x}", dynv, symtab, nsym, strtab));
    const OFF: usize = 0x1000;
    let mut names: Vec<u8> = vec![0];
    let mut secs: Vec<Vec<u8>> = vec![vec![0u8; 64]];
    #[allow(clippy::too_many_arguments)]
    fn sec(
        names: &mut Vec<u8>,
        secs: &mut Vec<Vec<u8>>,
        name: &str,
        typ: u32,
        fl: u64,
        addr: u64,
        off: u64,
        size: u64,
        link: u32,
        info: u32,
        align: u64,
        ent: u64,
    ) {
        let ni = names.len() as u32;
        names.extend_from_slice(name.as_bytes());
        names.push(0);
        let mut s = Vec::with_capacity(64);
        s.extend_from_slice(&ni.to_le_bytes());
        s.extend_from_slice(&typ.to_le_bytes());
        for v in [fl, addr, off, size] {
            s.extend_from_slice(&v.to_le_bytes());
        }
        s.extend_from_slice(&link.to_le_bytes());
        s.extend_from_slice(&info.to_le_bytes());
        s.extend_from_slice(&align.to_le_bytes());
        s.extend_from_slice(&ent.to_le_bytes());
        secs.push(s);
    }
    let o = |v: usize| (OFF + v) as u64;
    sec(&mut names, &mut secs, ".text", 1, 6, tv as u64, o(tv), text.len() as u64, 0, 0, 16, 0);
    if !(rv <= symtab && symtab + nsym * 24 <= strtab) {
        return Err("dynsym / dynstr not in the order nso2elf expects".into());
    }
    sec(&mut names, &mut secs, ".rodata", 1, 2, rv as u64, o(rv), (symtab - rv) as u64, 0, 0, 16, 0);
    sec(
        &mut names,
        &mut secs,
        ".dynsym",
        11,
        2,
        symtab as u64,
        o(symtab),
        (nsym * 24) as u64,
        4,
        1,
        8,
        24,
    );
    sec(&mut names, &mut secs, ".dynstr", 3, 2, strtab as u64, o(strtab), strsz as u64, 0, 0, 1, 0);
    let r1 = strtab + strsz;
    sec(
        &mut names,
        &mut secs,
        ".rodata.1",
        1,
        2,
        r1 as u64,
        o(r1),
        (rv + ro.len()) as u64 - r1 as u64,
        0,
        0,
        1,
        0,
    );
    sec(&mut names, &mut secs, ".data", 1, 3, dv as u64, o(dv), (dynv - dv) as u64, 0, 0, 16, 0);
    sec(&mut names, &mut secs, ".dynamic", 6, 3, dynv as u64, o(dynv), dynsz as u64, 4, 0, 8, 16);
    let d1 = dynv + dynsz;
    sec(
        &mut names,
        &mut secs,
        ".data.1",
        1,
        3,
        d1 as u64,
        o(d1),
        (dv + data.len() - d1) as u64,
        0,
        0,
        1,
        0,
    );
    sec(
        &mut names,
        &mut secs,
        ".bss",
        8,
        3,
        bss_s as u64,
        o(dv + data.len()),
        (bss_e - bss_s) as u64,
        0,
        0,
        16,
        0,
    );
    let shstr_i = secs.len();
    let shstr_off = OFF + mem.len();
    sec(&mut names, &mut secs, ".shstrtab", 3, 0, 0, shstr_off as u64, 0, 0, 0, 1, 0);
    let last = secs.last_mut().unwrap();
    put_u64(last, 32, names.len() as u64);
    let shoff = (shstr_off + names.len() + 7) & !7;
    let mut out: Vec<u8> = Vec::with_capacity(shoff + secs.len() * 64);
    out.extend_from_slice(b"\x7fELF\x02\x01\x01");
    out.extend_from_slice(&[0u8; 9]);
    out.extend_from_slice(&3u16.to_le_bytes());
    out.extend_from_slice(&183u16.to_le_bytes());
    out.extend_from_slice(&1u32.to_le_bytes());
    out.extend_from_slice(&0u64.to_le_bytes());
    out.extend_from_slice(&0x40u64.to_le_bytes());
    out.extend_from_slice(&(shoff as u64).to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    for v in [0x40u16, 0x38, 4, 0x40, secs.len() as u16, shstr_i as u16] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    let mut phdr = |t: u32, fl: u32, v: usize, fsz: usize, msz: usize, al: u64| {
        out.extend_from_slice(&t.to_le_bytes());
        out.extend_from_slice(&fl.to_le_bytes());
        for x in [(OFF + v) as u64, v as u64, v as u64, fsz as u64, msz as u64, al] {
            out.extend_from_slice(&x.to_le_bytes());
        }
    };
    phdr(1, 5, tv, text.len(), text.len(), 0x1000);
    phdr(1, 4, rv, ro.len(), ro.len(), 0x1000);
    let msz = *[data.len() as i64, bss_e - dv as i64, (data.len() + bss) as i64].iter().max().unwrap();
    phdr(1, 6, dv, data.len(), msz as usize, 0x1000);
    phdr(2, 6, dynv, dynsz, dynsz, 8);
    out.resize(OFF, 0);
    out.extend_from_slice(&mem);
    out.extend_from_slice(&names);
    out.resize(shoff, 0);
    for s in &secs {
        out.extend_from_slice(s);
    }
    Ok(Converted { elf: out, report, hashes_ok })
}
