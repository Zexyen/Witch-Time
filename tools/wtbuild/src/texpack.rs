//! `wtbuild textures` (#54): the game's ASTC textures as BC7, in build/cache/bc7, for GPUs without ASTC: 15.7 GB. The launcher runs this after a build.
//! The game's files stay as they are. A texture is keyed by a hash of its ASTC bytes (runtime/tex_pack.c, which documents the
//! files), so a texture changed by a mod is simply not found and the game decodes it as it always did.
//!
//! The archives are compressed with Kraken, which only the game's code decodes, so one family of archives at a time (`st3_000`
//! and `st3_001` hold .dat and .dtt of the same textures) goes through `witch-time-game --pkz-dump` into a temporary folder;
//! that keeps the extra space at 3.2 GB (`ba`). A family's .wtx appears when it is complete: a stopped build starts again at
//! the family it was in.

use intel_tex_2::{bc7, RgbaSurface};
use rayon::prelude::*;
use std::collections::{BTreeMap, HashSet};
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::process::Command;
use std::sync::Mutex;
use std::time::Instant;

use crate::text::read_pkz;
use crate::util::{u32_at, u64_at};

type R<T> = Result<T, String>;

// raises in step with TEX_PACK_VERSION (runtime/tex_pack.h); also when the encoder's settings change, so a pack is made again
const VERSION: u32 = 1;
const ENTRY: usize = 56;
const FOOTER: usize = 16;

extern "C" {
    fn tx_info(target: i32, fmt: i32, w: i32, h: i32, d: i32, mips: i32, layout: i32, levels: *mut i32, layers: *mut i32) -> i64;
    fn tx_level(target: i32, fmt: i32, w: i32, h: i32, d: i32, mips: i32, layout: i32, level: i32, lw: *mut i32, lh: *mut i32, lin: *mut i64);
    fn tx_decode(target: i32, fmt: i32, w: i32, h: i32, d: i32, mips: i32, layout: i32, src: *const u8, level: i32, layer: i32, tmp: *mut u8, rgba: *mut u8) -> i32;
}

// shortcut: intel_tex_2's Windows archives are built for MSVC; release.yml gives them to the gnullvm build. These are what
// they ask of the MSVC runtime, plus the ASTC encoder's C++ half (a stub: only BC7 is called).
#[cfg(all(windows, target_env = "gnu"))]
mod msvc_shim {
    #[no_mangle]
    #[used]
    pub static _fltused: i32 = 0;
    #[no_mangle]
    pub extern "C" fn pack_block_c() {
        std::process::abort()
    }
    std::arch::global_asm!(".globl __chkstk", "__chkstk:", "jmp ___chkstk_ms");
}

/// runtime/tex_pack.c tex_pack_hash
fn hash(p: &[u8]) -> u64 {
    let mut h = 0x9E3779B97F4A7C15u64 ^ p.len() as u64;
    for c in p.chunks_exact(8) {
        h = (h ^ u64::from_le_bytes(c.try_into().unwrap())).wrapping_mul(0x100000001B3);
        h ^= h >> 29;
    }
    h
}

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
struct Key {
    hash: u64,
    data_size: u64,
    format: u32,
    width: u32,
    height: u32,
    levels: u16,
    layers: u16,
}

#[derive(Clone, Copy, Debug)]
struct Entry {
    key: Key,
    offset: u64,
    stored: u32,
    raw: u32,
}

impl Entry {
    fn bytes(&self, shard: u16) -> [u8; ENTRY] {
        let k = &self.key;
        let mut b = [0u8; ENTRY];
        b[0..8].copy_from_slice(&k.hash.to_le_bytes());
        b[8..16].copy_from_slice(&k.data_size.to_le_bytes());
        b[16..24].copy_from_slice(&self.offset.to_le_bytes());
        for (i, v) in [self.stored, self.raw, k.format, k.width, k.height].iter().enumerate() {
            b[24 + i * 4..28 + i * 4].copy_from_slice(&v.to_le_bytes());
        }
        b[44..46].copy_from_slice(&k.levels.to_le_bytes());
        b[46..48].copy_from_slice(&k.layers.to_le_bytes());
        b[48..50].copy_from_slice(&shard.to_le_bytes());
        b
    }
    fn parse(b: &[u8]) -> Entry {
        Entry {
            key: Key {
                hash: u64_at(b, 0),
                data_size: u64_at(b, 8),
                format: u32_at(b, 32),
                width: u32_at(b, 36),
                height: u32_at(b, 40),
                levels: u16::from_le_bytes([b[44], b[45]]),
                layers: u16::from_le_bytes([b[46], b[47]]),
            },
            offset: u64_at(b, 16),
            stored: u32_at(b, 24),
            raw: u32_at(b, 28),
        }
    }
}

/// The entries of a finished shard; None for a file that is not one of this version.
fn read_shard(path: &Path) -> Option<Vec<Entry>> {
    let mut f = fs::File::open(path).ok()?;
    let len = f.metadata().ok()?.len();
    let mut foot = [0u8; FOOTER];
    f.seek(SeekFrom::Start(len.checked_sub(FOOTER as u64)?)).ok()?;
    f.read_exact(&mut foot).ok()?;
    if &foot[12..] != b"WTX1" || u32_at(&foot, 8) != VERSION {
        return None;
    }
    let count = u64_at(&foot, 0) as usize;
    let table = (count as u64).checked_mul(ENTRY as u64)?;
    f.seek(SeekFrom::Start(len.checked_sub(table + FOOTER as u64)?)).ok()?;
    let mut t = vec![0u8; table as usize];
    f.read_exact(&mut t).ok()?;
    Some(t.chunks_exact(ENTRY).map(Entry::parse).collect())
}

fn write_index(dir: &Path, shards: &[(String, Vec<Entry>)]) -> R<()> {
    let mut all: Vec<[u8; ENTRY]> = Vec::new();
    for (i, (_, es)) in shards.iter().enumerate() {
        all.extend(es.iter().map(|e| e.bytes(i as u16)));
    }
    all.sort_by_key(|b| u64_at(b, 0));
    let mut out = Vec::with_capacity(16 + shards.len() * 32 + all.len() * ENTRY);
    out.extend_from_slice(b"WTXI");
    for v in [VERSION, shards.len() as u32, all.len() as u32] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    for (name, _) in shards {
        let mut n = [0u8; 32];
        n[..name.len()].copy_from_slice(name.as_bytes());
        out.extend_from_slice(&n);
    }
    for b in &all {
        out.extend_from_slice(b);
    }
    let tmp = dir.join("index.tmp");
    fs::write(&tmp, &out).map_err(|e| format!("{}: {}", tmp.display(), e))?;
    fs::rename(&tmp, dir.join("index.wtx")).map_err(|e| e.to_string())
}

// ---- finding the textures: DAT archives (nested), WTA / WTB records, WTP data (scripts/pgdat.py has the DAT format)

/// (entry name, offset, size) of a DAT at `base`; damaged tables end the list
fn dat_entries(b: &[u8], base: usize) -> Vec<(String, usize, usize)> {
    let u = |o: usize| b.get(o..o + 4).map(|_| u32_at(b, o) as usize);
    let mut out = Vec::new();
    let (Some(count), Some(off_t), Some(name_t), Some(size_t)) = (u(base + 4), u(base + 8), u(base + 16), u(base + 20)) else { return out };
    let Some(width) = u(base + name_t) else { return out };
    for i in 0..count {
        let (Some(off), Some(size)) = (u(base + off_t + 4 * i), u(base + size_t + 4 * i)) else { break };
        let s = base + name_t + 4 + width * i;
        let Some(n) = b.get(s..s + width) else { break };
        let name = String::from_utf8_lossy(n.split(|&c| c == 0).next().unwrap_or(n)).into_owned();
        if base + off + size <= b.len() {
            out.push((name, base + off, size));
        }
    }
    out
}

/// One leaf of a file: `dir` is the path of DATs above it
#[derive(Clone)]
struct Leaf {
    dir: String,
    name: String,
    off: usize,
    size: usize,
}

fn walk(b: &[u8], base: usize, dir: &str, out: &mut Vec<Leaf>) {
    for (name, off, size) in dat_entries(b, base) {
        if size >= 4 && &b[off..off + 4] == b"DAT\0" {
            walk(b, off, &format!("{}/{}", dir, name), out);
        } else {
            out.push(Leaf { dir: dir.to_string(), name, off, size });
        }
    }
}

fn stem(n: &str) -> &str {
    let n = n.rsplit('/').next().unwrap_or(n);
    n.rsplit_once('.').map_or(n, |x| x.0)
}

/// What a file holds of interest: its record tables (WTB magic) and its texture data (.wtp)
#[derive(Default)]
struct FileIndex {
    tables: Vec<(Leaf, Vec<Rec>)>,
    data: Vec<Leaf>,
}

/// An XT1 record of an ASTC texture
#[derive(Clone, Copy)]
struct Rec {
    to: usize,
    target: i32,
    fmt: i32,
    w: i32,
    h: i32,
    d: i32,
    mips: i32,
    layout: i32,
}

fn is_astc(fmt: i32) -> bool {
    (0x79..=0x94).contains(&fmt)
}

fn records(b: &[u8]) -> Vec<Rec> {
    let u = |o: usize| b.get(o..o + 4).map(|_| u32_at(b, o) as usize);
    let (Some(cnt), Some(ot), Some(xt)) = (u(8), u(12), u(28)) else { return Vec::new() };
    (0..cnt)
        .filter_map(|i| {
            let x = xt + 56 * i;
            if b.get(x..x + 56)?.get(..4)? != b"XT1\0" {
                return None;
            }
            let v = |o: usize| u32_at(b, x + o) as i32;
            let r = Rec { to: u(ot + 4 * i)?, target: v(24), fmt: v(28), w: v(32), h: v(36), d: v(40), mips: v(20), layout: v(48) };
            is_astc(r.fmt).then_some(r)
        })
        .collect()
}

fn index_file(path: &Path, name: &str) -> FileIndex {
    let mut fi = FileIndex::default();
    let Ok(b) = fs::read(path) else { return fi };
    let leaves = if b.starts_with(b"DAT\0") {
        let mut l = Vec::new();
        walk(&b, 0, name, &mut l);
        l
    } else {
        vec![Leaf { dir: String::new(), name: name.to_string(), off: 0, size: b.len() }]
    };
    for l in leaves {
        if l.name.ends_with(".wtp") {
            fi.data.push(l);
        } else if b[l.off..l.off + l.size].starts_with(b"WTB\0") {
            let r = records(&b[l.off..l.off + l.size]);
            if !r.is_empty() {
                fi.tables.push((l, r));
            }
        }
    }
    fi
}

/// The .wtp of a .wta `l` in the file `name`, as the game's data lies: beside it, in the .dtt of a .dat, the .eft of a .eff, or
/// a loose file of the same stem. A wrong guess costs only an entry nothing asks for: the key is the texture's bytes.
fn find_wtp(files: &BTreeMap<String, FileIndex>, name: &str, l: &Leaf) -> Option<(String, Leaf)> {
    let st = stem(&l.name);
    let of = |f: &str| files.get(f).map(|x| x.data.as_slice()).unwrap_or(&[]);
    let by_stem = |f: &str| of(f).iter().find(|w| stem(&w.name) == st).map(|w| (f.to_string(), w.clone()));
    if let Some(w) = of(name).iter().find(|w| stem(&w.name) == st && w.dir == l.dir) {
        return Some((name.to_string(), w.clone()));
    }
    let base = name.rsplit_once('.').map_or(name, |x| x.0);
    let dtt = format!("{}.dtt", base);
    let eft = format!("{}.eft", base);
    by_stem(name)
        .or_else(|| by_stem(&dtt))
        .or_else(|| (of(&dtt).len() == 1).then(|| (dtt.clone(), of(&dtt)[0].clone())))
        .or_else(|| {
            of(&eft)
                .iter()
                .find(|w| stem(&w.name) == st && stem(&w.dir) == stem(&l.dir))
                .map(|w| (eft.clone(), w.clone()))
                .or_else(|| by_stem(&eft))
        })
        .or_else(|| by_stem(&format!("{}.wtp", base)))
        .or_else(|| (of(name).len() == 1).then(|| (name.to_string(), of(name)[0].clone())))
}

struct Job {
    file: String,
    at: u64,
    room: u64, // bytes of the .wtp from here on
    r: Rec,
}

fn jobs_of(files: &BTreeMap<String, FileIndex>) -> Vec<Job> {
    let mut jobs = Vec::new();
    for (name, fi) in files {
        for (l, recs) in &fi.tables {
            let (file, base, lim) = if l.name.ends_with(".wtb") {
                (name.clone(), l.off, l.size)
            } else if let Some((f, w)) = find_wtp(files, name, l) {
                (f, w.off, w.size)
            } else {
                continue;
            };
            for r in recs {
                if r.to < lim {
                    jobs.push(Job { file: file.clone(), at: (base + r.to) as u64, room: (lim - r.to) as u64, r: *r });
                }
            }
        }
    }
    jobs
}

// ---- BC7

/// BC7 wants whole blocks: edge texels repeated, as tex_encode_bc3 does
fn pad4(w: usize, h: usize, rgba: &[u8]) -> (usize, usize, Vec<u8>) {
    let (pw, ph) = ((w + 3) & !3, (h + 3) & !3);
    let mut p = vec![0u8; pw * ph * 4];
    for y in 0..ph {
        for x in 0..pw {
            let s = (y.min(h - 1) * w + x.min(w - 1)) * 4;
            let d = (y * pw + x) * 4;
            p[d..d + 4].copy_from_slice(&rgba[s..s + 4]);
        }
    }
    (pw, ph, p)
}

/// very fast: 12 dB above the runtime's BC3 on the game's textures; the next setting up gains 1.5 dB for 4 times the wait
/// (docs/STATUS.md). A texture without alpha spends the block's bits on colour.
fn bc7_level(w: usize, h: usize, rgba: &[u8], out: &mut Vec<u8>) {
    let (pw, ph, p) = pad4(w, h, rgba);
    let surface = RgbaSurface { data: &p, width: pw as u32, height: ph as u32, stride: (pw * 4) as u32 };
    let s = if p.chunks_exact(4).all(|t| t[3] == 255) { bc7::opaque_very_fast_settings() } else { bc7::alpha_very_fast_settings() };
    out.extend_from_slice(&bc7::compress_blocks(&s, &surface));
}

fn encode(r: &Rec, levels: i32, layers: i32, src: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    for layer in 0..layers {
        for level in 0..levels {
            let (mut w, mut h, mut lin) = (0, 0, 0i64);
            // SAFETY: the sizes come from the layout the data was checked against in convert()
            unsafe { tx_level(r.target, r.fmt, r.w, r.h, r.d, r.mips, r.layout, level, &mut w, &mut h, &mut lin) };
            let (w, h) = (w as usize, h as usize);
            let (mut tmp, mut rgba) = (vec![0u8; lin as usize], vec![0u8; w * h * 4]);
            unsafe { tx_decode(r.target, r.fmt, r.w, r.h, r.d, r.mips, r.layout, src.as_ptr(), level, layer, tmp.as_mut_ptr(), rgba.as_mut_ptr()) };
            bc7_level(w, h, &rgba, &mut out);
        }
    }
    out
}

/// Every new texture of `jobs` into `shard`; `seen` holds the textures of earlier shards. The entries written.
fn convert(root: &Path, jobs: &[Job], shard: &Path, seen: &Mutex<HashSet<Key>>) -> R<Vec<Entry>> {
    let part = shard.with_extension("part");
    let out = Mutex::new((fs::File::create(&part).map_err(|e| format!("{}: {}", part.display(), e))?, 0u64, Vec::new()));
    jobs.par_iter().try_for_each(|j| -> R<()> {
        let r = &j.r;
        let (mut levels, mut layers) = (0, 0);
        // SAFETY: plain values; the C side checks them against the format table
        let size = unsafe { tx_info(r.target, r.fmt, r.w, r.h, r.d, r.mips, r.layout, &mut levels, &mut layers) };
        if size <= 0 || size as u64 > j.room {
            return Ok(());
        }
        let mut src = vec![0u8; size as usize];
        let path = root.join(&j.file);
        let mut f = fs::File::open(&path).map_err(|e| format!("{}: {}", path.display(), e))?;
        f.seek(SeekFrom::Start(j.at)).and_then(|_| f.read_exact(&mut src)).map_err(|e| format!("{}: {}", path.display(), e))?;
        let key = Key {
            hash: hash(&src),
            data_size: size as u64,
            format: r.fmt as u32,
            width: r.w as u32,
            height: r.h as u32,
            levels: levels as u16,
            layers: layers as u16,
        };
        if !seen.lock().unwrap().insert(key) {
            return Ok(());
        }
        let raw = encode(r, levels, layers, &src);
        let packed = lz4_flex::block::compress(&raw);
        let body: &[u8] = if packed.len() < raw.len() { &packed } else { &raw };
        let mut o = out.lock().unwrap();
        o.0.write_all(body).map_err(|e| format!("{}: {}", part.display(), e))?;
        let offset = o.1;
        o.1 += body.len() as u64;
        o.2.push(Entry { key, offset, stored: body.len() as u32, raw: raw.len() as u32 });
        Ok(())
    })?;
    let (mut f, _, entries) = out.into_inner().unwrap();
    let mut tail = Vec::with_capacity(entries.len() * ENTRY + FOOTER);
    for e in &entries {
        tail.extend_from_slice(&e.bytes(0));
    }
    tail.extend_from_slice(&(entries.len() as u64).to_le_bytes());
    tail.extend_from_slice(&VERSION.to_le_bytes());
    tail.extend_from_slice(b"WTX1");
    f.write_all(&tail).and_then(|_| f.sync_all()).map_err(|e| format!("{}: {}", part.display(), e))?;
    drop(f);
    fs::rename(&part, shard).map_err(|e| e.to_string())?;
    Ok(entries)
}

/// The archives by family, those that can hold textures: name of the family -> (archive file names, entry names)
fn families(romfs: &Path) -> R<BTreeMap<String, (Vec<String>, Vec<String>)>> {
    let mut fam: BTreeMap<String, (Vec<String>, Vec<String>)> = BTreeMap::new();
    let mut names: Vec<String> = fs::read_dir(romfs)
        .map_err(|e| format!("{}: {}", romfs.display(), e))?
        .filter_map(|e| e.ok()?.file_name().into_string().ok())
        .filter(|n| n.ends_with(".pkz"))
        .collect();
    names.sort();
    for a in names {
        let (_, entries) = read_pkz(&romfs.join(&a))?;
        let wanted: Vec<String> = entries
            .into_iter()
            .map(|e| e.0)
            .filter(|n| [".dat", ".dtt", ".eff", ".eft", ".wta", ".wtb", ".wtp", ".evn"].iter().any(|x| n.ends_with(x)))
            .collect();
        if !wanted.is_empty() {
            let family = a.rsplit_once('_').map_or(a.as_str(), |x| x.0).to_string();
            let f = fam.entry(family).or_default();
            f.0.push(a);
            f.1.extend(wanted);
        }
    }
    Ok(fam)
}

fn dump_family(repo: &Path, romfs: &Path, archives: &[String], tmp: &Path, env: &dyn Fn(&mut Command)) -> R<()> {
    let _ = fs::remove_dir_all(tmp);
    fs::create_dir_all(tmp).map_err(|e| format!("{}: {}", tmp.display(), e))?;
    let exe = repo.join("build").join(crate::target::Target::host().game_exe());
    let mut c = Command::new(&exe);
    env(&mut c);
    c.arg(repo.join("targets").join("exefs").join("main.elf")).arg("--pkz-dump").arg(tmp).args(archives);
    c.current_dir(repo.join("build")).env("SSBU_ROMFS", romfs);
    let r = c.output().map_err(|e| format!("{}: {}", exe.display(), e))?;
    if !r.status.success() {
        let s = String::from_utf8_lossy(&r.stdout).into_owned() + &String::from_utf8_lossy(&r.stderr);
        return Err(format!("witch-time-game --pkz-dump failed: {}", s.chars().rev().take(1500).collect::<Vec<_>>().into_iter().rev().collect::<String>()));
    }
    Ok(())
}

/// Lines "[i/n] textures <family> ..." and "[n/n] textures: ok": the launcher follows them as it follows the build's steps.
pub fn run(repo: &Path, romfs: &Path, dump: Option<&Path>, jobs: usize, env: &dyn Fn(&mut Command), log: &mut dyn FnMut(String)) -> R<()> {
    let dir = repo.join("build").join("cache").join("bc7");
    fs::create_dir_all(&dir).map_err(|e| format!("{}: {}", dir.display(), e))?;
    let _ = fs::remove_file(dir.join("complete")); // the launcher's sign that nothing is left to do
    let fam = families(romfs)?;
    let (mut shards, mut seen) = (Vec::new(), HashSet::new());
    for f in fam.keys() {
        if let Some(es) = read_shard(&dir.join(format!("{}.wtx", f))) {
            seen.extend(es.iter().map(|e| e.key));
            shards.push((format!("{}.wtx", f), es));
        }
    }
    let seen = Mutex::new(seen);
    let pool = rayon::ThreadPoolBuilder::new().num_threads(jobs).build().map_err(|e| e.to_string())?;
    let tmp = dir.join("tmp");
    let t0 = Instant::now();
    let n = fam.len();
    for (i, (f, (archives, entries))) in fam.iter().enumerate() {
        let name = format!("{}.wtx", f);
        if shards.iter().any(|s| s.0 == name) {
            continue;
        }
        log(format!("[{}/{}] textures {} ...", i + 1, n, f));
        if dump.is_none() {
            dump_family(repo, romfs, archives, &tmp, env)?;
        }
        let root = dump.unwrap_or(&tmp);
        let files: BTreeMap<String, FileIndex> = pool.install(|| entries.par_iter().map(|e| (e.clone(), index_file(&root.join(e), e))).collect());
        let list = jobs_of(&files);
        let done = pool.install(|| convert(root, &list, &dir.join(&name), &seen));
        if done.is_err() {
            let _ = fs::remove_file(dir.join(&name).with_extension("part"));
        }
        let es = done?;
        log(format!("  {}: {} textures, {} MB", f, es.len(), es.iter().map(|e| e.stored as u64).sum::<u64>() >> 20));
        shards.push((name, es));
        write_index(&dir, &shards)?;
    }
    let _ = fs::remove_dir_all(&tmp);
    write_index(&dir, &shards)?;
    fs::write(dir.join("complete"), b"").map_err(|e| e.to_string())?;
    let (count, bytes) = (shards.iter().map(|s| s.1.len()).sum::<usize>(), shards.iter().flat_map(|s| s.1.iter()).map(|e| e.stored as u64).sum::<u64>());
    log(format!("  {} textures, {:.1} GB in {} ({} s)", count, bytes as f64 / 1e9, dir.display(), t0.elapsed().as_secs()));
    log(format!("[{}/{}] textures: ok", n, n));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn le(v: &[u32]) -> Vec<u8> {
        v.iter().flat_map(|x| x.to_le_bytes()).collect()
    }
    /// A DAT of `(name, data)`; the fields walk() reads
    fn dat(files: &[(&str, Vec<u8>)]) -> Vec<u8> {
        let n = files.len() as u32;
        let width = 16u32;
        let (off_t, name_t) = (0x20u32, 0x20 + 4 * n);
        let size_t = name_t + 4 + width * n;
        let mut at = size_t + 4 * n;
        let mut offs = Vec::new();
        for f in files {
            offs.push(at);
            at += f.1.len() as u32;
        }
        let mut b = b"DAT\0".to_vec();
        b.extend(le(&[n, off_t, 0, name_t, size_t, 0]));
        b.resize(0x20, 0);
        b.extend(le(&offs));
        b.extend(le(&[width]));
        for f in files {
            let mut nm = f.0.as_bytes().to_vec();
            nm.resize(width as usize, 0);
            b.extend(nm);
        }
        b.extend(le(&files.iter().map(|f| f.1.len() as u32).collect::<Vec<_>>()));
        for f in files {
            b.extend_from_slice(&f.1);
        }
        b
    }
    /// A WTA of one ASTC 4x4 texture at offset 0 of its .wtp, 8x8, one level
    fn wta() -> Vec<u8> {
        let (ot, st, xt) = (0x20u32, 0x24u32, 0x28u32);
        let mut b = b"WTB\0".to_vec();
        b.extend(le(&[1, 1, ot, st, 0, 0, xt]));
        b.extend(le(&[0, 128]));
        b.extend_from_slice(b"XT1\0");
        b.extend(le(&[0]));
        b.extend_from_slice(&128u64.to_le_bytes());
        b.extend(le(&[0, 1, 1, 0x79, 8, 8, 1, 0, 0, 0]));
        b
    }
    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("wt_texpack_{}_{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn hash_is_the_runtimes() {
        // tests/tex_pack_test.c holds the same two values
        assert_eq!(hash(b"12345678abcdefgh"), 0xbe51ff8d19fcccd3);
        assert_eq!(hash(b""), 0x9E3779B97F4A7C15);
    }

    #[test]
    fn lz4_block_is_the_runtimes() {
        // the same block as tests/tex_pack_test.c: lz4_flex's output, a repeat that overlaps itself
        let raw: Vec<u8> = b"abcabcabcabcabcabcabcabcabcabcabcabcxyz".to_vec();
        assert_eq!(lz4_flex::block::compress(&raw), [63, 97, 98, 99, 3, 0, 11, 96, 97, 98, 99, 120, 121, 122][..].to_vec());
    }

    #[test]
    fn entry_bytes_round_trip() {
        let key = Key { hash: 7, data_size: 512, format: 0x87, width: 1024, height: 512, levels: 11, layers: 1 };
        let e = Entry { key, offset: 1 << 33, stored: 100, raw: 200 };
        let b = e.bytes(3);
        let p = Entry::parse(&b);
        assert_eq!((p.key, p.offset, p.stored, p.raw), (key, e.offset, 100, 200));
        assert_eq!(u16::from_le_bytes([b[48], b[49]]), 3);
    }

    #[test]
    fn family_of_a_split_archive() {
        let d = tmp("fam");
        for (a, entries) in [("st3_000.pkz", vec!["st3/a.dat"]), ("st3_001.pkz", vec!["st3/a.dtt"]), ("sound_000.pkz", vec!["sound/x.wem"])] {
            let names: Vec<u8> = entries.iter().flat_map(|e| e.bytes().chain([0])).chain(*b"None\0").collect();
            let none = (names.len() - 5) as u32;
            let mut b = b"pkzl".to_vec();
            b.extend(le(&[0x20000]));
            b.extend_from_slice(&0u64.to_le_bytes());
            b.extend(le(&[entries.len() as u32, 0x20, names.len() as u32, 0x1000]));
            let mut o = 0u32;
            for e in &entries {
                b.extend(le(&[o, none]));
                b.extend_from_slice(&[0u8; 24]);
                o += e.len() as u32 + 1;
            }
            b.extend(names);
            fs::write(d.join(a), b).unwrap();
        }
        let f = families(&d).unwrap();
        assert_eq!(f.keys().collect::<Vec<_>>(), ["st3"]);
        assert_eq!(f["st3"].0, ["st3_000.pkz", "st3_001.pkz"]);
        assert_eq!(f["st3"].1, ["st3/a.dat", "st3/a.dtt"]);
    }

    #[test]
    fn wta_finds_its_wtp_in_the_dtt_and_a_second_family_adds_nothing() {
        let d = tmp("pair");
        // ASTC void-extent blocks: every texel 0xffff, 0x8000, 0x0000, 0xffff; block linear, but all blocks alike
        let block: [u8; 16] = [0xfc, 0xfd, 255, 255, 255, 255, 255, 255, 255, 255, 0, 128, 0, 0, 255, 255];
        let wtp: Vec<u8> = block.iter().cycle().take(512).cloned().collect();
        fs::write(d.join("a.dat"), dat(&[("a.wta", wta())])).unwrap();
        fs::write(d.join("a.dtt"), dat(&[("a.wtp", wtp)])).unwrap();
        let names = ["a.dat".to_string(), "a.dtt".to_string()];
        let files: BTreeMap<String, FileIndex> = names.iter().map(|e| (e.clone(), index_file(&d.join(e), e))).collect();
        let list = jobs_of(&files);
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].file, "a.dtt");
        let seen = Mutex::new(HashSet::new());
        let es = convert(&d, &list, &d.join("x.wtx"), &seen).unwrap();
        assert_eq!(es.len(), 1);
        assert_eq!(es[0].raw, 16 * 4);                // 8x8: four blocks
        assert_eq!((es[0].key.width, es[0].key.levels, es[0].key.format), (8, 1, 0x79));
        let back = read_shard(&d.join("x.wtx")).unwrap();
        assert_eq!(back.len(), 1);
        assert_eq!(back[0].key, es[0].key);
        assert!(convert(&d, &list, &d.join("y.wtx"), &seen).unwrap().is_empty());     // dedupe across shards
        write_index(&d, &[("x.wtx".into(), back)]).unwrap();
        let idx = fs::read(d.join("index.wtx")).unwrap();
        assert_eq!(&idx[..4], b"WTXI");
        assert_eq!(idx.len(), 16 + 32 + ENTRY);
    }

    #[test]
    fn a_stale_or_cut_shard_is_not_taken() {
        let d = tmp("cut");
        fs::write(d.join("a.wtx"), b"short").unwrap();
        assert!(read_shard(&d.join("a.wtx")).is_none());
        let mut b = vec![0u8; 32];
        b[28..].copy_from_slice(b"WTX1");
        b[24..28].copy_from_slice(&(VERSION + 1).to_le_bytes());
        fs::write(d.join("b.wtx"), b).unwrap();
        assert!(read_shard(&d.join("b.wtx")).is_none());
    }
}
