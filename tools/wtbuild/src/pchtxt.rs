//! Emulator-style code mods: IPSwitch text patches (.pchtxt) and IPS / IPS32 patches (.ips), the formats Yuzu / Ryujinx
//! mods use, applied to targets/exefs/main.elf before the lift (the `mods` step; `mods-tree` lines for the launcher's
//! Mods window). The repository ships only this mechanism, never anyone's patches. Files are read from the mods folder
//! and every folder under it (an emulator mod pack's `<mod>/exefs/` layout works as it is), in path order; a later file
//! wins where two patch the same bytes.
//!
//! On / off switches, like the emulators' per-mod check boxes: config/mods.ini (the launcher's Mods window writes it),
//! section [mods], lines `<key> = off` or `<key> = on`. A key is a mod (the first folder below the mods folder, or a
//! file lying directly in it), a file (its path below the mods folder, with /), or one block of a .pchtxt
//! (`<file> :: <block title>`, or `<file> :: #<n>` for an untitled or repeated title; `on` also applies a block written
//! under @disabled). Keys are compared without case. Anything not listed keeps its default: mods and files on, blocks as
//! their @enabled / @disabled line says. A mod or file switched off is not read at all, so one for another version of
//! the game does not stop the build. Renaming a file (e.g. to .pchtxt.off) also leaves it out.
//!
//! Offsets, as the emulators apply them: the patched buffer is the 0x100-byte NSO header followed by the program image
//! (text, rodata, data at their memory offsets). So a .pchtxt offset X means image offset X + offset_shift - 0x100
//! (mods written with `@flag offset_shift 0x100` give image offsets directly), and an .ips offset means offset - 0x100.
//! Image offset = the address minus the module base; nso2elf writes the image at file offset 0x1000 + image offset
//! (found here through the ELF's PT_LOAD headers). Every file must name the build ID of your main (`@nsobid-<id>` /
//! `<id>.ips`); a file for another build or module is refused.

use crate::py;
use crate::util::{hex, u16_at, u32_at, u64_at};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

const NSO_HEADER: i64 = 0x100;

pub struct ModError(pub String);

type R<T> = Result<T, ModError>;

fn err<T>(m: impl Into<String>) -> R<T> {
    Err(ModError(m.into()))
}

/// one record: (image offset, bytes, line number or record number)
pub type Rec = (i64, Vec<u8>, usize);
/// (title, written under @enabled, records)
pub type Block = (String, bool, Vec<Rec>);
/// an enabled block: (file, title, [(image offset, bytes, "file:line")])
pub type Mod = (String, String, Vec<(i64, Vec<u8>, String)>);

pub fn rel_key(rel: &str) -> String {
    rel.replace('\\', "/")
}

/// re.sub(r'\s*::\s*', ' :: ', rel_key(k.strip())).lower()
pub fn norm_key(k: &str) -> String {
    let s: Vec<char> = rel_key(k.trim()).chars().collect();
    let mut out = String::new();
    let mut i = 0;
    while i < s.len() {
        let mut j = i;
        while j < s.len() && s[j].is_whitespace() {
            j += 1;
        }
        if j + 1 < s.len() && s[j] == ':' && s[j + 1] == ':' {
            let mut k = j + 2;
            while k < s.len() && s[k].is_whitespace() {
                k += 1;
            }
            out.push_str(" :: ");
            i = k;
        } else {
            out.push(s[i]);
            i += 1;
        }
    }
    out.to_lowercase()
}

/// mods.ini: normalised key -> on / off; a missing file = every switch at its default
pub fn read_settings(path: &Path) -> HashMap<String, bool> {
    let mut out = HashMap::new();
    let Ok(b) = fs::read(path) else { return out };
    let text = String::from_utf8_lossy(&b);
    let text = text.strip_prefix('\u{feff}').unwrap_or(&text);
    let mut sect = String::new();
    for raw in text.split("\r\n").flat_map(|l| l.split(['\r', '\n'])) {
        let line = raw.trim();
        if line.is_empty() || line.starts_with([';', '#']) {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') && line.len() >= 2 {
            sect = line[1..line.len() - 1].trim().to_lowercase();
            continue;
        }
        if !(sect.is_empty() || sect == "mods") || !line.contains('=') {
            continue;
        }
        let (k, v) = line.rsplit_once('=').unwrap();
        // re.split(r'\s[;#]', v)[0]: the value ends before whitespace followed by ; or #
        let cs: Vec<(usize, char)> = v.char_indices().collect();
        let mut cut = v.len();
        for w in cs.windows(2) {
            if w[0].1.is_whitespace() && (w[1].1 == ';' || w[1].1 == '#') {
                cut = w[0].0;
                break;
            }
        }
        let v = v[..cut].trim().to_lowercase();
        if ["off", "0", "no", "false", "disabled"].contains(&v.as_str()) {
            out.insert(norm_key(k), false);
        } else if ["on", "1", "yes", "true", "enabled"].contains(&v.as_str()) {
            out.insert(norm_key(k), true);
        }
    }
    out
}

fn mod_of(rel: &str) -> String {
    rel_key(rel).split('/').next().unwrap_or("").to_string()
}

pub fn file_on(rel: &str, settings: &HashMap<String, bool>) -> bool {
    *settings.get(&norm_key(&mod_of(rel))).unwrap_or(&true) && *settings.get(&norm_key(rel)).unwrap_or(&true)
}

fn block_keys(rel: &str, blocks: &[Block]) -> Vec<String> {
    blocks
        .iter()
        .enumerate()
        .map(|(i, (t, _, _))| {
            if !t.is_empty() && blocks.iter().filter(|b| &b.0 == t).count() == 1 {
                format!("{} :: {}", rel_key(rel), t)
            } else {
                format!("{} :: #{}", rel_key(rel), i + 1)
            }
        })
        .collect()
}

pub fn nso_build_id(path: &Path) -> R<String> {
    let b = fs::read(path).map_err(|e| ModError(format!("{}: {}", path.display(), e)))?;
    if b.len() < 4 || &b[..4] != b"NSO0" {
        return err(format!("{} is not an NSO file", path.display()));
    }
    Ok(hex(&b[0x40.min(b.len())..0x60.min(b.len())]))
}

fn same_build(a: &str, b: &str) -> bool {
    a.to_lowercase().trim_end_matches('0') == b.to_lowercase().trim_end_matches('0')
}

/// every .pchtxt / .ips under folder, ordered by their path below it (without case)
pub fn find_mods(folder: &Path) -> Vec<PathBuf> {
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        let Ok(rd) = fs::read_dir(d) else { return };
        let mut dirs = Vec::new();
        for e in rd.flatten() {
            let p = e.path();
            let is_dir = fs::metadata(&p).map(|m| m.is_dir()).unwrap_or(false);
            if is_dir {
                if !e.file_type().map(|t| t.is_symlink()).unwrap_or(false) {
                    dirs.push(p);
                }
            } else {
                let n = e.file_name().to_string_lossy().to_lowercase();
                if n.ends_with(".pchtxt") || n.ends_with(".ips") {
                    out.push(p);
                }
            }
        }
        dirs.sort_by_key(|p| p.file_name().unwrap().to_string_lossy().to_lowercase());
        for d in dirs {
            walk(&d, out);
        }
    }
    let mut found = Vec::new();
    if folder.is_dir() {
        walk(folder, &mut found);
    }
    found.sort_by_key(|p| rel(folder, p).to_lowercase());
    found
}

fn rel(folder: &Path, p: &Path) -> String {
    p.strip_prefix(folder).unwrap_or(p).to_string_lossy().into_owned()
}

fn hex_bytes(s: &str, little: bool) -> Result<Vec<u8>, String> {
    if s.len() % 2 != 0 || s.is_empty() || !s.bytes().all(|c| c.is_ascii_hexdigit()) {
        return Err(format!("not an even number of hex digits: {}", py::repr_str(s)));
    }
    let mut b: Vec<u8> = (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect();
    if little {
        b.reverse();
    }
    Ok(b)
}

fn latin1_error(c: u32, pos: usize) -> String {
    let e = if c < 0x10000 { format!("\\u{:04x}", c) } else { format!("\\U{:08x}", c) };
    format!("'latin-1' codec can't encode character '{}' in position {}: ordinal not in range(256)", e, pos)
}

/// s.encode('latin-1').decode('unicode_escape').encode('latin-1')
fn unescape(s: &str) -> Result<Vec<u8>, String> {
    let mut b = Vec::new();
    for (i, c) in s.chars().enumerate() {
        if c as u32 > 0xff {
            return Err(latin1_error(c as u32, i));
        }
        b.push(c as u32 as u8);
    }
    let mut out: Vec<u32> = Vec::new();
    let mut i = 0;
    let bad = |what: &str, a: usize, z: usize| Err(format!("'unicodeescape' codec can't decode bytes in position {}-{}: {}", a, z, what));
    while i < b.len() {
        if b[i] != b'\\' {
            out.push(b[i] as u32);
            i += 1;
            continue;
        }
        if i + 1 >= b.len() {
            return Err(format!("'unicodeescape' codec can't decode byte 0x5c in position {}: \\ at end of string", i));
        }
        let c = b[i + 1];
        let simple = match c {
            b'\\' => Some(0x5c),
            b'\'' => Some(0x27),
            b'"' => Some(0x22),
            b'a' => Some(7),
            b'b' => Some(8),
            b'f' => Some(12),
            b'n' => Some(10),
            b'r' => Some(13),
            b't' => Some(9),
            b'v' => Some(11),
            _ => None,
        };
        if let Some(v) = simple {
            out.push(v);
            i += 2;
        } else if c == b'\n' {
            i += 2;
        } else if (b'0'..=b'7').contains(&c) {
            let mut v = 0u32;
            let mut j = i + 1;
            while j < b.len() && j < i + 4 && (b'0'..=b'7').contains(&b[j]) {
                v = v * 8 + (b[j] - b'0') as u32;
                j += 1;
            }
            out.push(v);
            i = j;
        } else if c == b'x' || c == b'u' || c == b'U' {
            let n = match c {
                b'x' => 2,
                b'u' => 4,
                _ => 8,
            };
            let digits = b.get(i + 2..i + 2 + n).filter(|d| d.iter().all(|x| x.is_ascii_hexdigit()));
            let Some(d) = digits else {
                let end = (i + 2..b.len().min(i + 2 + n))
                    .take_while(|&k| b[k].is_ascii_hexdigit())
                    .last()
                    .map(|k| k + 1)
                    .unwrap_or(i + 2);
                let what = match c {
                    b'x' => "truncated \\xXX escape",
                    b'u' => "truncated \\uXXXX escape",
                    _ => "truncated \\UXXXXXXXX escape",
                };
                return bad(what, i, end - 1);
            };
            let v = u32::from_str_radix(std::str::from_utf8(d).unwrap(), 16).unwrap();
            if v > 0x10ffff {
                return bad("illegal Unicode character", i, i + 1 + n);
            }
            out.push(v);
            i += 2 + n;
        } else if c == b'N' {
            return bad("malformed \\N character escape", i, i + 1);
        } else {
            out.push(0x5c);
            i += 1;
        }
    }
    let mut r = Vec::with_capacity(out.len());
    for (k, v) in out.iter().enumerate() {
        if *v > 0xff {
            return Err(latin1_error(*v, k));
        }
        r.push(*v as u8);
    }
    Ok(r)
}

/// the start of a record line: `<hex offset> <hex bytes | "string">` followed by whitespace, // or the end
fn record(line: &str) -> Option<(&str, &str)> {
    let off_end = line.find(|c: char| !c.is_ascii_hexdigit()).unwrap_or(line.len());
    if off_end == 0 {
        return None;
    }
    let rest = &line[off_end..];
    let val = rest.trim_start();
    if val.len() == rest.len() {
        return None; // \s+
    }
    let end = if let Some(body) = val.strip_prefix('"') {
        let mut it = body.char_indices();
        let mut close = None;
        while let Some((i, c)) = it.next() {
            if c == '\\' {
                if it.next().is_none() {
                    break;
                }
            } else if c == '"' {
                close = Some(i + 2);
                break;
            }
        }
        close?
    } else {
        let n = val.find(|c: char| !c.is_ascii_hexdigit()).unwrap_or(val.len());
        if n == 0 {
            return None;
        }
        n
    };
    let after = &val[end..];
    if after.is_empty() || after.starts_with(char::is_whitespace) || after.starts_with("//") {
        Some((&line[..off_end], &val[..end]))
    } else {
        None
    }
}

/// -> (build id, blocks), IPSwitch syntax as the emulators read it
pub fn parse_pchtxt(text: &str, name: &str) -> R<(Option<String>, Vec<Block>)> {
    let (mut bid, mut shift, mut little) = (None, 0i128, false);
    let mut blocks: Vec<Block> = Vec::new();
    let mut title = String::new();
    let mut cur: Option<usize> = None;
    for (n, raw) in py::splitlines(text).into_iter().enumerate() {
        let n = n + 1;
        let line = raw.trim();
        if line.is_empty() {
            continue;
        }
        if let Some(t) = line.strip_prefix("//") {
            let t = t.trim();
            if !t.is_empty() {
                title = t.to_string();
            }
            continue;
        }
        if line.starts_with('#') {
            continue;
        }
        if line.starts_with('@') {
            let parts: Vec<&str> = line.split_whitespace().collect();
            let word = parts[0].to_lowercase();
            if word == "@stop" {
                break;
            }
            if word.starts_with("@nsobid-") {
                bid = Some(parts[0].chars().skip(8).collect::<String>());
            } else if word == "@flag" {
                if parts.len() >= 3 && parts[1].to_lowercase() == "offset_shift" {
                    shift = py::int0(parts[2]).ok_or_else(|| {
                        ModError(format!(
                            "{} line {}: invalid literal for int() with base 0: {}",
                            name,
                            n,
                            py::repr_str(parts[2])
                        ))
                    })?;
                } else if parts.len() >= 2 && ["little-endian", "big-endian"].contains(&parts[1].to_lowercase().as_str()) {
                    little = parts[1].to_lowercase() == "little-endian";
                }
            } else if word == "@little-endian" || word == "@big-endian" {
                little = word == "@little-endian";
            } else if word == "@enabled" || word == "@disabled" {
                blocks.push((title.clone(), word == "@enabled", Vec::new()));
                cur = Some(blocks.len() - 1);
            }
            continue;
        }
        let Some(c) = cur else { continue };
        let Some((off, val)) = record(line) else {
            return err(format!(
                "{} line {}: expected \"<hex offset> <hex bytes>\" or <hex offset> \"string\": {}",
                name,
                n,
                py::repr_str(raw)
            ));
        };
        let data = if val.starts_with('"') {
            unescape(&val[1..val.len() - 1])
        } else {
            hex_bytes(val, little)
        }
        .map_err(|e| ModError(format!("{} line {}: {}", name, n, e)))?;
        let off = i128::from_str_radix(off, 16).unwrap_or(i128::MAX / 2) + shift - NSO_HEADER as i128;
        blocks[c].2.push((off as i64, data, n));
    }
    Ok((bid, blocks))
}

/// IPS ('PATCH' ... 'EOF', 3-byte offsets) or IPS32 ('IPS32' ... 'EEOF', 4-byte offsets), RLE records included
pub fn parse_ips(data: &[u8], name: &str) -> R<Vec<Rec>> {
    let (width, eof): (usize, &[u8]) = if data.starts_with(b"PATCH") {
        (3, b"EOF")
    } else if data.starts_with(b"IPS32") {
        (4, b"EEOF")
    } else {
        return err(format!("{}: not an IPS / IPS32 file", name));
    };
    let mut recs = Vec::new();
    let mut p = 5usize;
    let short = || ModError(format!("{}: unpack_from requires a buffer of at least 2 bytes", name));
    loop {
        if data.get(p..(p + width).min(data.len())) == Some(eof) && p + width + 4 >= data.len() {
            break;
        }
        if p + width + 2 > data.len() {
            return err(format!("{}: ends without {}", name, String::from_utf8_lossy(eof)));
        }
        let off = data[p..p + width].iter().fold(0i64, |a, &b| a << 8 | b as i64);
        let size = u16::from_be_bytes([data[p + width], data[p + width + 1]]) as usize;
        p += width + 2;
        let chunk = if size != 0 {
            let c = data[p.min(data.len())..(p + size).min(data.len())].to_vec();
            p += size;
            c
        } else {
            let count = data.get(p..p + 2).ok_or_else(short)?;
            let count = u16::from_be_bytes([count[0], count[1]]) as usize;
            let c = data.get(p + 2..p + 3).map(|v| vec![v[0]; count]).unwrap_or_default();
            p += 3;
            c
        };
        recs.push((off - NSO_HEADER, chunk, recs.len() + 1));
    }
    Ok(recs)
}

/// -> (build id, blocks) of one .pchtxt / .ips file
fn read_mod(folder: &Path, path: &Path) -> R<(String, Vec<Block>)> {
    let r = rel(folder, path);
    let data = fs::read(path).map_err(|e| ModError(format!("{}: {}", r, e)))?;
    if path.to_string_lossy().to_lowercase().ends_with(".ips") {
        let base = path.file_name().unwrap().to_string_lossy();
        let bid = match base.rfind('.') {
            Some(i) if i > 0 && !base[..i].chars().all(|c| c == '.') => base[..i].to_string(),
            _ => base.to_string(),
        };
        if !(2..=64).contains(&bid.len()) || !bid.bytes().all(|c| c.is_ascii_hexdigit()) {
            return err(format!("{}: an .ips file is named after the build ID it patches (<build id>.ips)", r));
        }
        return Ok((bid, vec![(String::new(), true, parse_ips(&data, &r)?)]));
    }
    let text = String::from_utf8_lossy(&data);
    let text = text.strip_prefix('\u{feff}').unwrap_or(&text);
    let (bid, blocks) = parse_pchtxt(text, &r)?;
    match bid {
        Some(b) if !b.is_empty() => Ok((b, blocks)),
        _ => err(format!("{}: no @nsobid-<build id> line, so it is not known which program it patches", r)),
    }
}

fn check_build(rel: &str, bid: &str, want: &Option<String>) -> R<()> {
    if let Some(w) = want {
        if !same_build(bid, w) {
            return err(format!(
                "{} is for build ID {}, but your main is {}: it was written for another version of the game (or another module, \
                 e.g. sdk / subsdk). Remove it from the mods folder or switch it off.",
                rel,
                bid.to_lowercase().trim_end_matches('0'),
                w.trim_end_matches('0')
            ));
        }
    }
    Ok(())
}

/// the enabled patches; refuses a file for another build
pub fn load_mods(folder: &Path, main_nso: Option<&Path>, settings: &HashMap<String, bool>) -> R<Vec<Mod>> {
    let want = main_nso.map(nso_build_id).transpose()?;
    let mut out = Vec::new();
    for path in find_mods(folder) {
        let r = rel(folder, &path);
        if !file_on(&r, settings) {
            continue;
        }
        let (bid, blocks) = read_mod(folder, &path)?;
        check_build(&r, &bid, &want)?;
        for (key, (title, enabled, recs)) in block_keys(&r, &blocks).into_iter().zip(blocks) {
            if *settings.get(&norm_key(&key)).unwrap_or(&enabled) && !recs.is_empty() {
                out.push((r.clone(), title, recs.into_iter().map(|(o, b, w)| (o, b, format!("{}:{}", r, w))).collect()));
            }
        }
    }
    Ok(out)
}

fn has_romfs(m: &Path) -> bool {
    if !m.is_dir() {
        return false;
    }
    if m.join("romfs").is_dir() {
        return true;
    }
    fs::read_dir(m)
        .map(|rd| {
            rd.flatten()
                .any(|e| !e.file_name().to_string_lossy().starts_with('.') && e.path().join("romfs").is_dir())
        })
        .unwrap_or(false)
}

/// every mod, file and block with its switch: the launcher's Mods window (`wtbuild mods-tree`)
pub fn tree(folder: &Path, main_nso: Option<&Path>, settings: &HashMap<String, bool>) -> R<Vec<String>> {
    let want = main_nso.map(nso_build_id).transpose()?;
    let on = |k: &str, d: bool| (*settings.get(&norm_key(k)).unwrap_or(&d) as u8).to_string();
    let mut lines: Vec<Vec<String>> = Vec::new();
    let mut seen: Vec<String> = Vec::new();
    for path in find_mods(folder) {
        let r = rel(folder, &path);
        let (m, rk) = (mod_of(&r), rel_key(&r));
        let ips = path.to_string_lossy().to_lowercase().ends_with(".ips");
        let (blocks, note) = match read_mod(folder, &path) {
            Ok((bid, blocks)) => {
                let note = if ips {
                    format!("IPS, {} record(s)", blocks[0].2.len())
                } else {
                    String::new()
                };
                match &want {
                    Some(w) if !same_build(&bid, w) => (
                        Vec::new(),
                        format!(
                            "for build ID {}, not your main: another version of the game or another module",
                            bid.to_lowercase().trim_end_matches('0')
                        ),
                    ),
                    _ => (blocks, note),
                }
            }
            Err(ModError(e)) => (Vec::new(), e.replace(&format!("{}: ", r), "")),
        };
        if !seen.contains(&m) {
            seen.push(m.clone());
            lines.push(vec![
                "mod".into(),
                m.clone(),
                on(&m, true),
                m.clone(),
                if m == rk { note.clone() } else { String::new() },
                if has_romfs(&folder.join(&m)) { "romfs".into() } else { String::new() },
            ]);
        }
        if m != rk {
            lines.push(vec![
                "file".into(),
                rk.clone(),
                on(&rk, true),
                rk.chars().skip(m.chars().count() + 1).collect(),
                note,
            ]);
        }
        if !ips {
            for (key, (title, enabled, recs)) in block_keys(&r, &blocks).into_iter().zip(blocks) {
                let label = if title.is_empty() {
                    format!("block {}", key.rsplit('#').next().unwrap())
                } else {
                    title
                };
                lines.push(vec![
                    "block".into(),
                    key.clone(),
                    on(&key, enabled),
                    (enabled as u8).to_string(),
                    recs.len().to_string(),
                    label,
                ]);
            }
        }
    }
    if folder.is_dir() {
        let mut ds: Vec<String> = fs::read_dir(folder)
            .map(|rd| rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect())
            .unwrap_or_default();
        ds.sort_by_key(|d| d.to_lowercase());
        for d in ds {
            if !seen.contains(&d) && has_romfs(&folder.join(&d)) {
                lines.push(vec!["mod".into(), d.clone(), on(&d, true), d, String::new(), "romfs-only".into()]);
            }
        }
    }
    match load_mods(folder, main_nso, settings) {
        Ok(mods) => lines.push(vec!["state".into(), fingerprint(&mods).unwrap_or_else(|| "none".into()), String::new()]),
        Err(ModError(e)) => lines.push(vec!["state".into(), "error".into(), e]),
    }
    Ok(lines
        .into_iter()
        .map(|l| {
            l.iter()
                .map(|x| x.split_whitespace().collect::<Vec<_>>().join(" "))
                .collect::<Vec<_>>()
                .join("\t")
        })
        .collect())
}

/// None when nothing would change; else a hash of exactly what would be written
pub fn fingerprint(mods: &[Mod]) -> Option<String> {
    let mut h = Sha256::new();
    let mut any = false;
    for (_, _, recs) in mods {
        for (off, data, _) in recs {
            h.update(off.to_le_bytes());
            h.update((data.len() as u32).to_le_bytes());
            h.update(data);
            any |= !data.is_empty();
        }
    }
    any.then(|| hex(&h.finalize()))
}

fn short(b: &[u8]) -> String {
    if b.len() <= 16 {
        hex(b)
    } else {
        format!("{}.. ({} bytes)", hex(&b[..16]), b.len())
    }
}

/// Patch elf in place; every byte must fall inside a PT_LOAD segment's file image. -> bytes written
pub fn apply(elf: &mut [u8], mods: &[Mod], log: &mut dyn FnMut(String)) -> R<usize> {
    if elf.len() < 0x40 || &elf[..4] != b"\x7fELF" {
        return err("not an ELF file");
    }
    let phoff = u64_at(elf, 0x20) as usize;
    let (phentsize, phnum) = (u16_at(elf, 0x36) as usize, u16_at(elf, 0x38) as usize);
    let segs: Vec<(i64, i64, i64)> = (0..phnum)
        .map(|i| phoff + i * phentsize)
        .filter(|&p| u32_at(elf, p) == 1)
        .map(|p| (u64_at(elf, p + 16) as i64, u64_at(elf, p + 8) as i64, u64_at(elf, p + 32) as i64))
        .collect();
    let mut written: HashMap<i64, String> = HashMap::new();
    for (r, title, recs) in mods {
        log(format!("  {}{}", r, if title.is_empty() { String::new() } else { format!(": {}", title) }));
        for (off, data, wh) in recs {
            if data.is_empty() {
                continue;
            }
            let n = data.len() as i64;
            let seg = segs.iter().find(|s| s.0 <= *off && off + n <= s.0 + s.2);
            let (true, Some(seg)) = (*off >= 0, seg) else {
                return err(format!(
                    "{}: image offset 0x{} (+{} bytes) is outside the program's code and data in the file (offset_shift wrong, or the patch is for another program)",
                    wh,
                    crate::util::hx(*off),
                    data.len()
                ));
            };
            let fo = (seg.1 + off - seg.0) as usize;
            let old = elf[fo..fo + data.len()].to_vec();
            let mut over: Vec<String> = (*off..off + n).filter_map(|o| written.get(&o)).filter(|w| *w != r).cloned().collect();
            over.sort();
            over.dedup();
            elf[fo..fo + data.len()].copy_from_slice(data);
            for o in *off..off + n {
                written.insert(o, r.clone());
            }
            log(format!(
                "    0x{:08x}  {} -> {}{}{}",
                off,
                short(&old),
                short(data),
                if old == *data { "  (unchanged)" } else { "" },
                if over.is_empty() {
                    String::new()
                } else {
                    format!("  (overrides {})", over.join(", "))
                }
            ));
        }
    }
    Ok(written.len())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// splitmix64
    struct Rng(u64);
    impl Rng {
        fn u(&mut self) -> u64 {
            self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15);
            let z = (self.0 ^ (self.0 >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
            let z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
            z ^ (z >> 31)
        }
        fn f(&mut self) -> f64 {
            (self.u() >> 11) as f64 / (1u64 << 53) as f64
        }
        fn p(&mut self, x: f64) -> bool {
            self.f() < x
        }
        fn range(&mut self, lo: usize, hi: usize) -> usize {
            lo + (self.u() % (hi - lo) as u64) as usize
        }
        fn pick<T: Copy>(&mut self, xs: &[T]) -> T {
            xs[self.range(0, xs.len())]
        }
    }

    const WORDS: [&str; 12] = [
        "Speed",
        "Infinite magic",
        "No HUD",
        "Camera",
        "FPS",
        "",
        "",
        "Fix",
        "Fix",
        "Unlock = all",
        "Größe",
        "視点",
    ];
    const TEXT: usize = 0x3000; // the stand-in's text size
    const CASES: usize = 60;

    fn pchtxt(r: &mut Rng, bid: &str) -> String {
        let mut out = Vec::new();
        if r.p(0.9) {
            let id = if r.p(0.95) {
                bid.to_string()
            } else {
                format!("{:016x}{:016x}{:08x}", r.u(), r.u(), r.u() as u32)
            };
            out.push(format!("@nsobid-{}", id));
        }
        let shift = if r.p(0.3) {
            out.push("@flag offset_shift 0x100".to_string());
            0x100
        } else {
            0
        };
        for _ in 0..r.range(1, 5) {
            if r.p(0.8) {
                out.push(format!("// {}", r.pick(&WORDS)));
            }
            if r.p(0.2) {
                out.push(r.pick(&["@little-endian", "@big-endian", "@flag little-endian", "# a comment", ""]).to_string());
            }
            out.push(r.pick(&["@enabled", "@enabled", "@disabled"]).to_string());
            for _ in 0..r.range(0, 5) {
                let mut off = r.range(0, TEXT - 64) + 0x100 - shift;
                let k = r.f();
                let val = if k < 0.5 {
                    let w = 2 * r.range(1, 9);
                    format!("{:01$x}", r.u(), w) // a minimum width, as Python's %0*x: 15 digits are an odd count
                } else if k < 0.75 {
                    format!(
                        "\"{}\"",
                        r.pick(&["abc", "a\\nb", "\\x41\\x42", "q\\\"x", "\\101\\7", "\\u00e9", "tab\\there", "é"])
                    )
                } else if k < 0.77 {
                    // mistakes and odd escapes
                    format!("\"{}\"", r.pick(&["\\u4e2d", "\\x4", "\\q", "漢"]))
                } else if k < 0.78 {
                    "abc".to_string() // an odd number of digits
                } else if k < 0.79 {
                    off = r.pick(&[TEXT * 64, 0]); // outside the program / before it
                    "aabb".to_string()
                } else {
                    format!("{:08x}", r.u() as u32)
                };
                let end = if r.p(0.98) { r.pick(&["", "", " // note", "\t"]) } else { "zz" };
                out.push(format!("{:x} {}{}", off, val, end));
            }
        }
        if r.p(0.1) {
            out.push("@stop".to_string());
            out.push("garbage after stop".to_string());
        }
        out.join("\n") + r.pick(&["\n", "", "\r\n"])
    }

    fn ips(r: &mut Rng, big: bool) -> Vec<u8> {
        let mut out = if big { b"IPS32".to_vec() } else { b"PATCH".to_vec() };
        for _ in 0..r.range(0, 6) {
            let off = (r.range(0, TEXT - 64) + 0x100) as u32;
            out.extend_from_slice(&off.to_be_bytes()[if big { 0 } else { 1 }..]);
            if r.p(0.3) {
                out.extend([0, 0]);
                out.extend_from_slice(&(r.range(1, 40) as u16).to_be_bytes());
                out.push(r.u() as u8);
            } else {
                let data: Vec<u8> = (0..r.range(1, 12)).map(|_| r.u() as u8).collect();
                out.extend_from_slice(&(data.len() as u16).to_be_bytes());
                out.extend(data);
            }
        }
        out.extend_from_slice(if big { b"EEOF" } else { b"EOF" });
        if r.p(0.2) {
            out.truncate(out.len() - 2); // truncated
        }
        out
    }

    /// a mods folder in `d`; -> the text of its mods.ini
    fn make_case(r: &mut Rng, d: &Path, bid: &str) -> String {
        let mut names = Vec::new();
        for _ in 0..r.range(1, 5) {
            let m = r.pick(&["Speed Mod", "cam", "Zeta", "alpha", "Ünicode mod", "x"]);
            for _ in 0..r.range(1, 4) {
                let sub = r.pick(&["", "exefs/", "exefs/", "Extra/"]);
                let (name, data) = if r.p(0.3) {
                    // the upper-case name is 48 digits long: beside '<bid>.ips' it would be the same file on NTFS
                    let name = match r.range(0, 3) {
                        0 => format!("{}.ips", &bid[..r.pick(&[40, 64, 32])]),
                        1 => "notabuildid.ips".to_string(),
                        _ => format!("{}.IPS", bid[..48].to_uppercase()),
                    };
                    let big = r.p(0.5);
                    (name, ips(r, big))
                } else {
                    let name = format!("{}.pchtxt", r.pick(&["main", "patch", "Patch2", "readme"]));
                    let mut data = pchtxt(r, bid).into_bytes();
                    if r.p(0.1) {
                        data.splice(0..0, *b"\xef\xbb\xbf");
                    }
                    (name, data)
                };
                let rel = if r.p(0.85) { format!("{}/{}{}", m, sub, name) } else { name };
                let p = d.join(&rel);
                fs::create_dir_all(p.parent().unwrap()).unwrap();
                fs::write(&p, data).unwrap();
                names.push(rel);
            }
            if r.p(0.2) {
                fs::create_dir_all(d.join(m).join("romfs")).unwrap();
            }
        }
        if r.p(0.3) {
            fs::create_dir_all(d.join("Texture pack").join(r.pick(&["romfs", "inner/romfs"]))).unwrap();
        }
        let mut ini = vec!["[mods]".to_string()];
        for n in &names {
            if r.p(0.3) {
                let k = match r.range(0, 6) {
                    0 => n.clone(),
                    1 => n.split('/').next().unwrap().to_string(),
                    2 => n.to_uppercase(),
                    3 => format!("{} :: #1", n),
                    4 => format!("{}::{}", n, r.pick(&WORDS)),
                    _ => format!("{}  ::  #2", n),
                };
                ini.push(format!("{} = {}", k, r.pick(&["off", "on", "OFF ; why", "0", "yes", "maybe"])));
            }
        }
        ini.push(r.pick(&["", "[other]\nx = off", "; comment"]).to_string());
        ini.join("\n") + "\n"
    }

    /// text, rodata and data at file offset 0x1000 + image offset, as nso2elf writes them
    fn stand_in_elf(r: &mut Rng) -> Vec<u8> {
        let mut e = vec![0u8; 0x1000];
        e[..7].copy_from_slice(b"\x7fELF\x02\x01\x01");
        e[0x20..0x28].copy_from_slice(&0x40u64.to_le_bytes());
        e[0x36..0x3a].copy_from_slice(&[0x38, 0, 3, 0]);
        for (i, (va, n)) in [(0, TEXT), (TEXT, 0x800), (TEXT + 0x800, 0x400)].into_iter().enumerate() {
            let p = 0x40 + 0x38 * i;
            e[p] = 1;
            for (at, v) in [(8, 0x1000 + va), (16, va), (24, va), (32, n), (40, n)] {
                e[p + at..p + at + 8].copy_from_slice(&(v as u64).to_le_bytes());
            }
        }
        e.extend((0..TEXT + 0xc00).map(|_| r.u() as u8));
        e
    }

    /// random mod folders: `mods-tree` with and without the main, then `mods-apply` (exit code, stdout, stderr, the ELF);
    /// the golden was pchtxt.py's output on the same folders
    #[test]
    fn random_mod_folders() {
        // WTBUILD_CASES=<dir> keeps the cases there
        let keep = std::env::var_os("WTBUILD_CASES").map(|d| PathBuf::from(d).join("pchtxt"));
        let t = keep
            .clone()
            .unwrap_or_else(|| std::env::temp_dir().join(format!("wt_pchtxt_{}", std::process::id())));
        let _ = fs::remove_dir_all(&t);
        fs::create_dir_all(&t).unwrap();
        let mut r = Rng(1);
        let mut nso = vec![0u8; 0x100];
        nso[..4].copy_from_slice(b"NSO0");
        nso[0x40..0x54].iter_mut().for_each(|b| *b = r.u() as u8); // a GNU build ID: 20 bytes, then zeros
        let bid = hex(&nso[0x40..0x60]);
        let elf = stand_in_elf(&mut r);
        let main = t.join("main.nso");
        fs::write(&main, &nso).unwrap();
        fs::write(t.join("main.elf"), &elf).unwrap();
        let mut g = String::new();
        let (mut applied, mut failed) = (0, 0);
        for i in 0..CASES {
            let d = t.join(format!("case{}", i));
            let mods = d.join("mods");
            fs::create_dir_all(&mods).unwrap();
            let ini = make_case(&mut r, &mods, &bid);
            fs::write(d.join("mods.ini"), ini).unwrap();
            let settings = read_settings(&d.join("mods.ini"));
            g += &format!("== case {}\n", i);
            for (what, with) in [("tree", Some(main.as_path())), ("tree without main", None)] {
                g += &format!("-- {}\n", what);
                match tree(&mods, with, &settings) {
                    Ok(lines) => g += &(lines.join("\n") + "\n"),
                    Err(ModError(e)) => g += &format!("error: {}\n", e),
                }
            }
            let (mut out, mut patched) = (String::new(), elf.clone());
            let res = load_mods(&mods, Some(&main), &settings).and_then(|m| apply(&mut patched, &m, &mut |l| out += &(l + "\n")));
            match res {
                Ok(n) => {
                    applied += 1;
                    g += &format!(
                        "-- apply exit 0\n{}{} bytes patched -> out.elf\n-- elf {}\n",
                        out,
                        n,
                        hex(&Sha256::digest(&patched))
                    );
                }
                Err(ModError(e)) => {
                    failed += 1;
                    g += &format!("-- apply exit 1\n{}error: {}\n-- elf none\n", out, e);
                }
            }
        }
        if keep.is_none() {
            let _ = fs::remove_dir_all(&t);
        }
        assert!(applied > 5 && failed > 5, "{} applied, {} failed: the cases miss a side", applied, failed);
        // rel() keeps '\\' on Windows
        crate::golden::check("pchtxt_mods.txt", &g.replace('\\', "/"));
    }

    #[test]
    fn unescape_is_pythons_unicode_escape() {
        let ok: [(&str, &[u8]); 7] = [
            ("\\x41\\x42", b"AB"),
            ("\\101\\7", b"A\x07"),
            ("\\u00e9", b"\xe9"),
            ("q\\\"x", b"q\"x"),
            ("\\q", b"\\q"),
            ("tab\\there", b"tab\there"),
            ("é", b"\xe9"),
        ];
        for (s, want) in ok {
            assert_eq!(unescape(s).as_deref(), Ok(want), "{}", s);
        }
        let bad = [
            ("\\x4", "'unicodeescape' codec can't decode bytes in position 0-2: truncated \\xXX escape"),
            (
                "\\u4e2d",
                "'latin-1' codec can't encode character '\\u4e2d' in position 0: ordinal not in range(256)",
            ),
            (
                "漢",
                "'latin-1' codec can't encode character '\\u6f22' in position 0: ordinal not in range(256)",
            ),
            ("a\\", "'unicodeescape' codec can't decode byte 0x5c in position 1: \\ at end of string"),
            (
                "\\U0011ffff",
                "'unicodeescape' codec can't decode bytes in position 0-9: illegal Unicode character",
            ),
        ];
        for (s, want) in bad {
            assert_eq!(unescape(s), Err(want.to_string()), "{}", s);
        }
    }

    #[test]
    fn ips_eof_truncation_size_and_rle() {
        let recs = |d: &[u8]| parse_ips(d, "p").map_err(|e| e.0);
        let ips = [&b"PATCH"[..], &[0, 1, 0x10, 0, 2, 0xaa, 0xbb], &[0, 1, 0x20, 0, 0, 0, 5, 0x7f], b"EOF"].concat();
        let want: Vec<Rec> = vec![(0x10, vec![0xaa, 0xbb], 1), (0x20, vec![0x7f; 5], 2)];
        assert_eq!(recs(&ips), Ok(want.clone()));
        assert_eq!(recs(&[&ips[..], &[0, 0x10, 0]].concat()), Ok(want), "EOF and a truncation size");
        assert_eq!(recs(&ips[..ips.len() - 2]), Err("p: ends without EOF".to_string()));
        // a record at offset 0x454f46 ('EOF') is not the end when more than a truncation size follows
        assert_eq!(
            recs(&[&b"PATCHEOF"[..], &[0, 2, 1, 2], b"EOF"].concat()),
            Ok(vec![(0x454f46 - 0x100, vec![1, 2], 1)])
        );
        assert_eq!(
            recs(&[&b"IPS32"[..], &[0, 0, 1, 0x10, 0, 1, 9], b"EEOF"].concat()),
            Ok(vec![(0x10, vec![9], 1)])
        );
        assert_eq!(recs(b"IPS"), Err("p: not an IPS / IPS32 file".to_string()));
    }

    #[test]
    fn settings_keys_and_build_ids() {
        let ini = std::env::temp_dir().join(format!("wt_pchtxt_ini_{}", std::process::id()));
        let text = "\u{feff}[mods]\ncam = off ; why\nb = on;x\nc=off\t#x\nx = maybe\n; y = off\n[other]\nd = off\n[ Mods ]\n\
                    e/f.pchtxt  ::  #2 = Yes\ng = a = on\r\nh = 0\n";
        fs::write(&ini, text).unwrap();
        let mut s: Vec<(String, bool)> = read_settings(&ini).into_iter().collect();
        let _ = fs::remove_file(&ini);
        s.sort();
        let want = [("c", false), ("cam", false), ("e/f.pchtxt :: #2", true), ("g = a", true), ("h", false)];
        assert_eq!(s, want.map(|(k, v)| (k.to_string(), v)));
        assert_eq!(norm_key("  Cam\\Exefs\\Main.pchtxt::Speed  "), "cam/exefs/main.pchtxt :: speed");
        assert_eq!(norm_key("x  ::  #2"), "x :: #2");
        assert_eq!(norm_key("a :: b::c"), "a :: b :: c");
        let blocks: Vec<Block> = ["Speed", "", "Fix", "Fix"].iter().map(|t| (t.to_string(), true, Vec::new())).collect();
        assert_eq!(
            block_keys("m\\f.pchtxt", &blocks),
            ["m/f.pchtxt :: Speed", "m/f.pchtxt :: #2", "m/f.pchtxt :: #3", "m/f.pchtxt :: #4"]
        );
        assert!(same_build("ABC000", "abc") && same_build("abc", "abc0") && same_build("0", "00"));
        assert!(!same_build("abc0001", "abc"));
    }
}
