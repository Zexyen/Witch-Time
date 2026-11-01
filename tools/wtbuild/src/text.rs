//! The `text` step: the labels and values of the PC settings rows of the game's Options screen, spelled with the game's
//! own glyphs, as a RomFS mod made from YOUR dump (mods/witchtime-pc-settings/romfs/ui_000.pkz). A port of
//! scripts/pc_settings_mod.py with its readers scripts/pkz.py and scripts/pgdat.py; the archive is byte for byte the one
//! it writes. The MCD layout is in pc_settings_mod.py and docs/RE_NOTES.md section 8.

use crate::py;
use crate::text_strings::{CORE, LANGS, MENU};
use crate::util::{u16_at, u32_at, u64_at};
use std::collections::{BTreeSet, HashMap};
use std::fs;
use std::io::{Read, Seek};
use std::path::Path;
use std::process::Command;

const BRIGHTNESS: u32 = 0x5f236d04; // MENU_OPTION_LIST_016: its font is the list's
const ON: u32 = 0x39bcb833; // CORE_SETTING_003 ("ON"): the font of the values
pub const MOD: &str = "witchtime-pc-settings";
const OLD_MOD: &str = "b3recomp-pc-settings"; // its name before the rename (2026-10-27)

type R<T> = Result<T, String>;

fn rd32(d: &[u8], o: usize) -> R<u32> {
    d.get(o..o + 4)
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
        .ok_or_else(|| "MCD: an offset past the end of the file".into())
}

// ---- pkz / DAT

/// (file size, [(name, codec, size, offset, stored size)]) of a pkz archive (pkz.py read_pkz)
pub fn read_pkz(path: &Path) -> R<(u64, Vec<(String, String, u64, u64, u64)>)> {
    let mut f = fs::File::open(path).map_err(|e| format!("{}: {}", path.display(), e))?;
    let mut d = Vec::new();
    f.by_ref().take(0x20).read_to_end(&mut d).map_err(|e| format!("{}: {}", path.display(), e))?;
    if d.len() < 0x20 || &d[..4] != b"pkzl" {
        return Err(format!(
            "{}: not a pkz archive (magic {})",
            path.display(),
            py::repr_bytes(&d[..d.len().min(4)])
        ));
    }
    if u32_at(&d, 4) != 0x20000 {
        return Err(format!("{}: unknown pkz version 0x{:x}", path.display(), u32_at(&d, 4)));
    }
    let (fsize, count, toff, nsize) = (u64_at(&d, 8), u32_at(&d, 0x10) as usize, u32_at(&d, 0x14) as usize, u32_at(&d, 0x18) as usize);
    let mut table = vec![0u8; count * 0x20 + nsize];
    f.seek(std::io::SeekFrom::Start(toff as u64))
        .and_then(|_| f.read_exact(&mut table))
        .map_err(|_| "pkz: entry table past the end")?;
    let names = &table[count * 0x20..];
    let name = |o: usize| -> R<String> {
        let e = names.get(o..).and_then(|s| s.iter().position(|&c| c == 0)).ok_or("pkz: name past the end")?;
        Ok(String::from_utf8_lossy(&names[o..o + e]).into_owned())
    };
    let mut out = Vec::new();
    for i in 0..count {
        let t = &table[i * 0x20..];
        out.push((
            name(u32_at(t, 0) as usize)?,
            name(u32_at(t, 4) as usize)?,
            u64_at(t, 8),
            u64_at(t, 16),
            u64_at(t, 24),
        ));
    }
    Ok((fsize, out))
}

/// (name, extension, offset, size) of every member of a PlatinumGames DAT (pgdat.py entries)
fn dat_entries(b: &[u8]) -> R<Vec<(String, String, usize, usize)>> {
    if b.len() < 0x1c || &b[..4] != b"DAT\0" {
        return Err("DAT: bad magic".into());
    }
    let (count, off_t, ext_t, name_t, size_t) = (
        u32_at(b, 4) as usize,
        u32_at(b, 8) as usize,
        u32_at(b, 12) as usize,
        u32_at(b, 16) as usize,
        u32_at(b, 20) as usize,
    );
    let width = rd32(b, name_t)? as usize;
    let latin1 = |s: &[u8]| s.split(|&c| c == 0).next().unwrap_or(&[]).iter().map(|&c| c as char).collect::<String>();
    let mut out = Vec::new();
    for i in 0..count {
        let ext = b.get(ext_t + 4 * i..ext_t + 4 * i + 4).ok_or("DAT: table past the end")?;
        let s = name_t + 4 + width * i;
        let name = b.get(s..s + width).ok_or("DAT: table past the end")?;
        out.push((latin1(name), latin1(ext), rd32(b, off_t + 4 * i)? as usize, rd32(b, size_t + 4 * i)? as usize));
    }
    Ok(out)
}

/// the DAT with `member` appended at the end (16-byte aligned) and its offset / size pointed there
fn dat_replace(blob: &[u8], member: &str, data: &[u8]) -> R<Vec<u8>> {
    let (off_t, size_t) = (u32_at(blob, 8) as usize, u32_at(blob, 20) as usize);
    let i = dat_entries(blob)?.iter().position(|e| e.0 == member).ok_or("DAT: no such member")?;
    let mut out = blob.to_vec();
    out.resize(out.len() + (16 - out.len() % 16) % 16, 0);
    let at = out.len() as u32;
    out[off_t + 4 * i..off_t + 4 * i + 4].copy_from_slice(&at.to_le_bytes());
    out[size_t + 4 * i..size_t + 4 * i + 4].copy_from_slice(&(data.len() as u32).to_le_bytes());
    out.extend_from_slice(data);
    out.resize(out.len() + (16 - data.len() % 16) % 16, 0);
    Ok(out)
}

// ---- MCD

struct Line {
    us: Vec<u16>,
    pad: u32,
    extra: i64,
    below: u32,
    horiz: u32,
    off: u64,
}

struct Par {
    vp: u32,
    hp: u32,
    font: u32,
    lines: Vec<Line>,
}

struct Msg {
    seq: u32,
    ev: u32,
    pars: Vec<Par>,
}

struct Mcd {
    msgs: Vec<Msg>,
    syms: Vec<(u16, u16, u32)>,
    glyphs: Vec<Vec<u8>>,
    fonts: Vec<Vec<u8>>,
    evs: Vec<(u32, u32, [u8; 32])>,
}

fn mcd_read(d: &[u8]) -> R<Mcd> {
    let h: Vec<usize> = (0..10).map(|i| rd32(d, 4 * i).map(|v| v as usize)).collect::<R<_>>()?;
    let (mo, mc, so, sc, go, gc, fo, fc, eo, ec) = (h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9]);
    let mut msgs = Vec::new();
    for i in 0..mc {
        let (po, pc, seq, ev) = (
            rd32(d, mo + 16 * i)? as usize,
            rd32(d, mo + 16 * i + 4)? as usize,
            rd32(d, mo + 16 * i + 8)?,
            rd32(d, mo + 16 * i + 12)?,
        );
        let mut pars = Vec::new();
        for j in 0..pc {
            let p = po + 20 * j;
            let (lo, lc) = (rd32(d, p)? as usize, rd32(d, p + 4)? as usize);
            let mut lines = Vec::new();
            for k in 0..lc {
                let l = lo + 24 * k;
                let (off, pad, n, n2) = (rd32(d, l)? as usize, rd32(d, l + 4)?, rd32(d, l + 8)? as usize, rd32(d, l + 12)?);
                let us = d
                    .get(off..off + 2 * n)
                    .ok_or("MCD: text past the end")?
                    .chunks(2)
                    .map(|c| u16::from_le_bytes([c[0], c[1]]))
                    .collect();
                lines.push(Line {
                    us,
                    pad,
                    extra: n2 as i64 - n as i64,
                    below: rd32(d, l + 16)?,
                    horiz: rd32(d, l + 20)?,
                    off: off as u64,
                });
            }
            pars.push(Par {
                vp: rd32(d, p + 8)?,
                hp: rd32(d, p + 12)?,
                font: rd32(d, p + 16)?,
                lines,
            });
        }
        msgs.push(Msg { seq, ev, pars });
    }
    let slice = |o: usize, n: usize| d.get(o..o + n).map(|s| s.to_vec()).ok_or_else(|| "MCD: a table past the end".to_string());
    Ok(Mcd {
        msgs,
        syms: (0..sc)
            .map(|i| Ok((u16_at(&slice(so + 8 * i, 8)?, 0), u16_at(d, so + 8 * i + 2), u32_at(d, so + 8 * i + 4))))
            .collect::<R<_>>()?,
        glyphs: (0..gc).map(|i| slice(go + 40 * i, 40)).collect::<R<_>>()?,
        fonts: (0..fc).map(|i| slice(fo + 20 * i, 20)).collect::<R<_>>()?,
        evs: (0..ec)
            .map(|i| {
                Ok((
                    rd32(d, eo + 40 * i)?,
                    rd32(d, eo + 40 * i + 4)?,
                    slice(eo + 40 * i + 8, 32)?.try_into().unwrap(),
                ))
            })
            .collect::<R<_>>()?,
    })
}

fn put(out: &mut [u8], o: usize, vs: &[u32]) {
    for (k, v) in vs.iter().enumerate() {
        out[o + 4 * k..o + 4 * k + 4].copy_from_slice(&v.to_le_bytes());
    }
}

fn mcd_write(m: &Mcd) -> Vec<u8> {
    let lines: Vec<&Line> = m.msgs.iter().flat_map(|msg| msg.pars.iter().flat_map(|p| p.lines.iter())).collect();
    let mut out = vec![0u8; 40];
    let mut toff = vec![0usize; lines.len()];
    let mut order: Vec<usize> = (0..lines.len()).collect();
    order.sort_by_key(|&i| lines[i].off); // texts in their original order, new ones last
    for i in order {
        toff[i] = out.len();
        for u in &lines[i].us {
            out.extend_from_slice(&u.to_le_bytes());
        }
    }
    out.resize((out.len() + 4) & !3, 0);
    let npar: usize = m.msgs.iter().map(|x| x.pars.len()).sum();
    let mo = out.len();
    out.resize(out.len() + 16 * m.msgs.len() + 4, 0);
    let po = out.len();
    out.resize(out.len() + 20 * npar + 4, 0);
    let lo = out.len();
    out.resize(out.len() + 24 * lines.len() + 4, 0);
    let (mut pi, mut li) = (0, 0);
    for (i, msg) in m.msgs.iter().enumerate() {
        put(&mut out, mo + 16 * i, &[(po + 20 * pi) as u32, msg.pars.len() as u32, msg.seq, msg.ev]);
        for p in &msg.pars {
            put(&mut out, po + 20 * pi, &[(lo + 24 * li) as u32, p.lines.len() as u32, p.vp, p.hp, p.font]);
            pi += 1;
            for l in &p.lines {
                put(
                    &mut out,
                    lo + 24 * li,
                    &[
                        toff[li] as u32,
                        l.pad,
                        l.us.len() as u32,
                        (l.us.len() as i64 + l.extra) as u32,
                        l.below,
                        l.horiz,
                    ],
                );
                li += 1;
            }
        }
    }
    let mut heads = Vec::new();
    heads.extend([out.len() as u32, m.syms.len() as u32]);
    for (f, c, g) in &m.syms {
        out.extend_from_slice(&f.to_le_bytes());
        out.extend_from_slice(&c.to_le_bytes());
        out.extend_from_slice(&g.to_le_bytes());
    }
    out.extend([0; 4]);
    heads.extend([out.len() as u32, m.glyphs.len() as u32]);
    m.glyphs.iter().for_each(|g| out.extend_from_slice(g));
    out.extend([0; 4]);
    heads.extend([out.len() as u32, m.fonts.len() as u32]);
    m.fonts.iter().for_each(|g| out.extend_from_slice(g));
    out.extend([0; 4]);
    heads.extend([out.len() as u32, m.evs.len() as u32]);
    for (h, i, n) in &m.evs {
        out.extend_from_slice(&h.to_le_bytes());
        out.extend_from_slice(&i.to_le_bytes());
        out.extend_from_slice(n);
    }
    let mut h = vec![mo as u32, m.msgs.len() as u32];
    h.extend(heads);
    put(&mut out, 0, &h);
    out
}

/// the first of `keys` with the highest count, as Python's max(d, key=d.get) picks it (insertion order)
fn most(counts: &[(u16, u32)]) -> Option<u16> {
    let mut best: Option<(u16, u32)> = None;
    for &(k, n) in counts {
        if best.map_or(true, |b| n > b.1) {
            best = Some((k, n));
        }
    }
    best.map(|b| b.0)
}

fn bump(v: &mut Vec<(u16, u32)>, k: u16, by: u32) {
    match v.iter_mut().find(|e| e.0 == k) {
        Some(e) => e.1 += by,
        None => v.push((k, by)),
    }
}

/// d (a messmenu.mcd or messcore.mcd) with the messages `msgs` added in the font of the message `font_of`;
/// -> (the file, [what could not be spelled])
fn add_msgs(d: &[u8], msgs: &[(&str, &[(&str, &[&str])])], lang: &str, font_of: u32) -> R<(Vec<u8>, Vec<String>)> {
    let mut m = mcd_read(d)?;
    if mcd_write(&m) != d {
        return Err("MCD: this layout is not the one known (the file would not be rebuilt as it is)".into());
    }
    let mut bad = Vec::new();
    let fi = m
        .evs
        .iter()
        .find(|e| e.0 == font_of)
        .ok_or("MCD: the message whose font the rows take is not there")?
        .1 as usize;
    let font = m.msgs.get(fi).and_then(|x| x.pars.first()).ok_or("MCD: that message has no text")?.font;
    let mut kern: HashMap<(u16, u16), u16> = HashMap::new();
    let mut kern1: HashMap<u16, Vec<(u16, u32)>> = HashMap::new();
    let mut space: Vec<(u16, u32)> = Vec::new();
    for msg in &m.msgs {
        for p in &msg.pars {
            if p.font != font {
                continue;
            }
            for l in &p.lines {
                let us = &l.us;
                let pairs: Vec<(u16, u16)> = (0..us.len().saturating_sub(1)).step_by(2).map(|i| (us[i], us[i + 1])).collect();
                for (k, &(a, ka)) in pairs.iter().enumerate() {
                    let b = pairs.get(k + 1).map_or(0x8000, |x| x.0);
                    if a == 0x8001 {
                        bump(&mut space, ka, 1);
                    } else if a < 0x8000 {
                        kern.insert((a, b), ka);
                        bump(kern1.entry(a).or_default(), ka, 1);
                    }
                }
            }
        }
    }
    let mut order: Vec<u32> = vec![font, 7];
    order.extend(m.syms.iter().map(|s| s.0 as u32).filter(|&f| f != font && f != 7).collect::<BTreeSet<_>>());
    let mut first: HashMap<(u32, u32), usize> = HashMap::new();
    for (i, &(f, c, _)) in m.syms.iter().enumerate() {
        first.entry((f as u32, c as u32)).or_insert(i);
    }
    let sym = |ch: char, fonts: &[u32]| fonts.iter().find_map(|&f| first.get(&(f, ch as u32)).copied());
    let have: Vec<u32> = m.evs.iter().map(|e| e.0).collect();
    for (name, texts) in msgs {
        let h = crc32fast::hash(name.to_lowercase().as_bytes()) & 0x7fffffff;
        if have.contains(&h) {
            continue;
        }
        let cands = texts.iter().find(|t| t.0 == lang).ok_or("text: no such language")?.1;
        let spell = |t: &str, k: &[u32]| t.chars().all(|ch| ch == ' ' || sym(ch, k).is_some());
        let text = [&order[..2], &order[..]]
            .iter()
            .find_map(|k| cands.iter().find(|t| spell(t, k)))
            .unwrap_or(&cands[0]);
        let missing: String = text
            .chars()
            .filter(|&ch| ch != ' ' && sym(ch, &order).is_none())
            .collect::<BTreeSet<_>>()
            .into_iter()
            .collect();
        let other: String = text
            .chars()
            .filter(|&ch| ch != ' ' && sym(ch, &order).is_some() && sym(ch, &order[..2]).is_none())
            .collect::<BTreeSet<_>>()
            .into_iter()
            .collect();
        if !missing.is_empty() {
            bad.push(format!("{} {} without {}", name, py::repr_str(text), py::repr_str(&missing)));
        }
        if !other.is_empty() {
            bad.push(format!("{} {}: {} in another font", name, py::repr_str(text), py::repr_str(&other)));
        }
        let mut seq: Vec<u32> = text
            .chars()
            .filter_map(|ch| if ch == ' ' { Some(0x8001) } else { sym(ch, &order).map(|i| i as u32) })
            .collect();
        seq.push(0x8000);
        let mut us: Vec<u16> = Vec::new();
        for (k, &a) in seq.iter().enumerate() {
            let b = seq.get(k + 1).copied();
            if a == 0x8000 {
                us.push(a as u16);
            } else if a == 0x8001 {
                us.extend([a as u16, most(&space).unwrap_or(7)]);
            } else {
                let a = a as u16;
                let kk = b.and_then(|b| kern.get(&(a, b as u16)).copied());
                us.extend([a, kk.unwrap_or_else(|| kern1.get(&a).and_then(|v| most(v)).unwrap_or(0))]);
            }
        }
        let seqno = m.msgs.len() as u32 + 1;
        m.msgs.push(Msg {
            seq: seqno,
            ev: h,
            pars: vec![Par {
                vp: 0,
                hp: 0,
                font,
                lines: vec![Line {
                    us,
                    pad: 0,
                    extra: 0,
                    below: 0,
                    horiz: 0,
                    off: 1 << 40,
                }],
            }],
        });
        let mut nm = [0u8; 32];
        let nb = name.as_bytes();
        nm[..nb.len().min(32)].copy_from_slice(&nb[..nb.len().min(32)]);
        m.evs.push((h, m.msgs.len() as u32 - 1, nm));
    }
    m.evs.sort_by_key(|e| e.0);
    Ok((mcd_write(&m), bad))
}

/// <mods>/witchtime-pc-settings/romfs/ui_000.pkz from <romfs>/ui_000.pkz; the text files from `dump` (a --pkz-dump
/// folder), else from build/witch-time-game --pkz-dump. Logs what pc_settings_mod.py printed.
pub fn run(repo: &Path, romfs: &Path, dump: Option<&Path>, mods: &Path, env: &dyn Fn(&mut Command), log: &mut dyn FnMut(String)) -> R<()> {
    let arc = romfs.join("ui_000.pkz");
    let out = mods.join(MOD).join("romfs").join("ui_000.pkz");
    let (fsize, entries) = read_pkz(&arc)?;
    let mut names = Vec::new();
    for l in LANGS {
        for k in ["menu", "core"] {
            names.push(format!("ui/ui_{}{}.dat", k, if l.is_empty() { String::new() } else { format!("_{}", l) }));
        }
    }
    if !names.iter().all(|n| entries.iter().any(|e| &e.0 == n)) {
        return Err("ui_000.pkz: not the archive known".into());
    }
    // the decompressed text files, by the game's own decoder (witch-time-game --pkz-dump)
    let tmp = std::env::temp_dir().join(format!("wtpcmod_{}", std::process::id()));
    let result = (|| -> R<(HashMap<String, Vec<u8>>, Vec<String>)> {
        let src = match dump {
            Some(d) => d.to_path_buf(),
            None => dump_texts(repo, romfs, &tmp, env)?,
        };
        let (mut new, mut bad) = (HashMap::new(), Vec::new());
        for n in &names {
            let lang = if n[6..].contains('_') { &n[11..n.len() - 4] } else { "" };
            let kind = &n[6..10];
            let blob = fs::read(src.join(n)).map_err(|e| format!("{}: {}", n, e))?;
            let member = format!("mess{}.mcd", kind);
            let (_, _, o, s) = dat_entries(&blob)?.into_iter().find(|e| e.0 == member).ok_or("DAT: no message file")?;
            let mcd = blob.get(o..o + s).ok_or("DAT: member past the end")?;
            let (d, b) = if kind == "menu" {
                add_msgs(mcd, MENU, lang, BRIGHTNESS)?
            } else {
                add_msgs(mcd, CORE, lang, ON)?
            };
            new.insert(n.clone(), dat_replace(&blob, &member, &d)?);
            bad.extend(b.into_iter().map(|x| format!("{}: {}", n, x)));
        }
        Ok((new, bad))
    })();
    let _ = fs::remove_dir_all(&tmp);
    let (new, bad) = result?;
    let a = fs::read(&arc).map_err(|e| e.to_string())?;
    let (count, toff, nsize, align) = (
        u32_at(&a, 0x10) as usize,
        u32_at(&a, 0x14) as usize,
        u32_at(&a, 0x18) as usize,
        u32_at(&a, 0x1c) as u64,
    );
    let mut table = a[toff..toff + count * 0x20 + nsize].to_vec();
    let names_t = &table[count * 0x20..];
    let none = names_t.windows(5).position(|w| w == b"None\0").ok_or("pkz: no codec None")?;
    if !(none == 0 || names_t[none - 1] == 0) {
        return Err("pkz: the codec names are not the ones known".into());
    }
    // the same mod under its old name would serve ui_000.pkz first (mods sort by name), and options_row.c looks for MOD
    let _ = fs::remove_dir_all(mods.join(OLD_MOD));
    fs::create_dir_all(out.parent().unwrap()).map_err(|e| e.to_string())?;
    let mut f = a.clone();
    let up = |x: u64| x.div_ceil(align) * align;
    let mut pos = up(fsize);
    for (i, e) in entries.iter().enumerate() {
        if let Some(d) = new.get(&e.0) {
            let no = u32_at(&table, i * 0x20);
            let t = &mut table[i * 0x20..i * 0x20 + 0x20];
            t[0..4].copy_from_slice(&no.to_le_bytes());
            t[4..8].copy_from_slice(&(none as u32).to_le_bytes());
            t[8..16].copy_from_slice(&(d.len() as u64).to_le_bytes());
            t[16..24].copy_from_slice(&pos.to_le_bytes());
            t[24..32].copy_from_slice(&(d.len() as u64).to_le_bytes());
            let p = pos as usize;
            if f.len() < p + d.len() {
                f.resize(p + d.len(), 0);
            }
            f[p..p + d.len()].copy_from_slice(d);
            pos = up(pos + d.len() as u64);
        }
    }
    f.resize(pos as usize, 0);
    let mut head = Vec::with_capacity(0x20 + table.len());
    head.extend_from_slice(b"pkzl");
    head.extend_from_slice(&0x20000u32.to_le_bytes());
    head.extend_from_slice(&pos.to_le_bytes());
    for v in [count as u32, toff as u32, nsize as u32, align as u32] {
        head.extend_from_slice(&v.to_le_bytes());
    }
    head.extend_from_slice(&table);
    f[..head.len()].copy_from_slice(&head);
    let tmp_out = out.with_extension("pkz.tmp");
    fs::write(&tmp_out, &f).map_err(|e| e.to_string())?;
    fs::rename(&tmp_out, &out).map_err(|e| e.to_string())?;
    let files: BTreeSet<&str> = bad.iter().map(|x| x.split(':').next().unwrap()).collect();
    for n in files {
        let xs: Vec<&str> = bad
            .iter()
            .filter(|x| x.starts_with(&format!("{}:", n)))
            .map(|x| x.split_once(": ").unwrap().1)
            .collect();
        log(format!(
            "  {}: {} texts not as written (glyphs the file has not, or of another font): {}",
            n,
            xs.len(),
            if xs.len() < 4 { xs.join("; ") } else { format!("{}; ...", xs[0]) }
        ));
    }
    log(format!(
        "{}: {} bytes, {} text files with the PC settings rows' text ({} labels, {} values)",
        out.display(),
        pos,
        new.len(),
        MENU.len(),
        CORE.len()
    ));
    Ok(())
}

/// build/witch-time-game --pkz-dump of the text files into `tmp`
fn dump_texts(repo: &Path, romfs: &Path, tmp: &Path, env: &dyn Fn(&mut Command)) -> R<std::path::PathBuf> {
    let _ = fs::remove_dir_all(tmp);
    fs::create_dir_all(tmp).map_err(|e| e.to_string())?;
    let exe = repo.join("build").join("witch-time-game");
    let mut c = Command::new(&exe);
    env(&mut c);
    c.arg(repo.join("targets").join("exefs").join("main.elf"))
        .arg("--pkz-dump")
        .arg(tmp)
        .args(["ui/ui_menu", "ui/ui_core"]);
    c.current_dir(repo.join("build")).env("SSBU_ROMFS", romfs);
    let r = c.output().map_err(|e| format!("{}: {}", exe.display(), e))?;
    if !r.status.success() {
        let tail = |b: &[u8]| {
            let s = String::from_utf8_lossy(b).into_owned();
            s.chars().rev().take(2000).collect::<Vec<_>>().into_iter().rev().collect::<String>()
        };
        return Err(format!("witch-time-game --pkz-dump failed:\n{}{}", tail(&r.stdout), tail(&r.stderr)));
    }
    Ok(tmp.to_path_buf())
}
