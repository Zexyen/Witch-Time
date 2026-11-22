//! [witchtime] Direct extraction of Bayonetta 3 (Program 0) and The Old Picture Book prologue
//! (Program 1) from the player's own Nintendo Switch NSP & XCI files using their own `prod.keys`.
//!
//! This repository contains NO keys, NO game code, and NO game assets.

pub mod container;
pub mod crypto;
pub mod keys;
pub mod nca;
pub mod storage;

use container::open_package;
use keys::{discover_prod_keys, parse_ticket, KeySet};
use nca::{ExeFs, Nca, RomFs};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

pub const BAYO3_TITLE_MASK: u64 = 0xFFFF_FFFF_FFFF_F000;
pub const BAYO3_BASE_ID: u64 = 0x0100_4A40_10FE_A000;
const MAIN_BUILD_ID: &str = "b47cc79b88b7091ea514b2b725c50ae01c6f00df";
const PROGRAM1_MAIN_SHA256: &str = "d6e43604b86503373c108e40800af680c8f4133d6d42af869f462a39dd7f3cdf";

/// True if `path` has a `.nsp` or `.xci` extension (case-insensitive).
pub fn is_package_file(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| matches!(e.to_ascii_lowercase().as_str(), "nsp" | "xci"))
        .unwrap_or(false)
}

/// True if `path` has a compressed Switch package extension (`.nsz` / `.xcz`).
pub fn is_compressed_package(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| matches!(e.to_ascii_lowercase().as_str(), "nsz" | "xcz"))
        .unwrap_or(false)
}

/// Classify whether `--dump` (and optional `--update`) points to NSP/XCI package(s) or an extracted folder.
pub enum DumpSource {
    ExtractedDir,
    Packages(Vec<PathBuf>),
}

/// Resolve user-supplied dump + optional update paths into either an extracted folder or a list of `.nsp`/`.xci` files.
pub fn classify_dump(dump: &Path, update: Option<&Path>) -> Result<DumpSource, String> {
    let mut files: Vec<PathBuf> = Vec::new();
    let mut compressed: Vec<String> = Vec::new();

    if dump.is_file() {
        if is_compressed_package(dump) {
            return Err(format!(
                "{}: compressed NSZ/XCZ files are not supported directly. Decompress to NSP or XCI first.",
                dump.display()
            ));
        }
        if !is_package_file(dump) {
            return Err(format!(
                "{}: expected an extracted dump folder or a Switch .nsp / .xci file",
                dump.display()
            ));
        }
        files.push(dump.to_path_buf());
    } else if dump.is_dir() {
        // An extracted folder with exefs/ and romfs/ stays an extracted folder unless --update was also passed
        if update.is_none() && dump.join("exefs").is_dir() && dump.join("romfs").is_dir() {
            return Ok(DumpSource::ExtractedDir);
        }
        let Ok(rd) = fs::read_dir(dump) else {
            return Ok(DumpSource::ExtractedDir);
        };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_file() {
                if is_package_file(&p) {
                    files.push(p);
                } else if is_compressed_package(&p) {
                    compressed.push(e.file_name().to_string_lossy().into_owned());
                }
            }
        }
        files.sort();
        if files.is_empty() {
            if !compressed.is_empty() {
                return Err(format!(
                    "{}: compressed NSZ/XCZ files ({}) are not supported directly. Decompress them to NSP or XCI first.",
                    dump.display(),
                    compressed.join(", ")
                ));
            }
            if update.is_none() {
                return Ok(DumpSource::ExtractedDir);
            }
        }
    } else if update.is_none() {
        return Ok(DumpSource::ExtractedDir);
    } else {
        return Err(format!("{}: file or folder not found", dump.display()));
    }

    if let Some(upd) = update {
        if !upd.exists() {
            return Err(format!("--update {}: file or folder not found", upd.display()));
        }
        if upd.is_file() {
            if is_compressed_package(upd) {
                return Err(format!(
                    "{}: compressed NSZ/XCZ files are not supported directly. Decompress to NSP or XCI first.",
                    upd.display()
                ));
            }
            if !is_package_file(upd) {
                return Err(format!("--update {}: expected a .nsp or .xci file", upd.display()));
            }
            if !files.contains(&upd.to_path_buf()) {
                files.push(upd.to_path_buf());
            }
        } else if upd.is_dir() {
            if let Ok(rd) = fs::read_dir(upd) {
                for e in rd.flatten() {
                    let p = e.path();
                    if p.is_file() && is_package_file(&p) && !files.contains(&p) {
                        files.push(p);
                    }
                }
            }
        }
    }

    if files.is_empty() {
        return Err(format!("{}: no .nsp or .xci files found", dump.display()));
    }
    Ok(DumpSource::Packages(files))
}

/// Resolved Bayonetta 3 Program 0 (base + update 1.2.0) and Program 1 (Old Picture Book prologue).
pub struct TitleSet {
    pub keys_path: PathBuf,
    pub prog0_base: Option<Nca>,
    pub prog0_update: Nca,
    pub prog1_base: Option<Nca>,
    pub prog1: Option<Nca>,
}

fn nso_build_id_bytes(data: &[u8]) -> Option<String> {
    if data.len() >= 0x54 && &data[0..4] == b"NSO0" {
        Some(crate::util::hex(&data[0x40..0x54]))
    } else {
        None
    }
}

impl TitleSet {
    /// Open one or more `.nsp` / `.xci` files, decrypt NCA headers using `prod.keys`, and resolve
    /// Bayonetta 3 Program 0 (base + update) and Program 1 (The Old Picture Book prologue).
    pub fn open(files: &[PathBuf], explicit_keys: Option<&Path>, repo: Option<&Path>) -> Result<Self, String> {
        if files.is_empty() {
            return Err("no NSP or XCI files provided".into());
        }

        // Collect hint directories (parent dirs of all provided package files)
        let mut hint_dirs: Vec<PathBuf> = Vec::new();
        for f in files {
            if let Some(p) = f.parent() {
                if !p.as_os_str().is_empty() && !hint_dirs.contains(&p.to_path_buf()) {
                    hint_dirs.push(p.to_path_buf());
                }
            }
        }
        let hint_refs: Vec<&Path> = hint_dirs.iter().map(PathBuf::as_path).collect();

        let keys_path = match explicit_keys {
            Some(p) => {
                if !p.is_file() {
                    return Err(format!("--keys {}: file not found", p.display()));
                }
                p.to_path_buf()
            }
            None => discover_prod_keys(&hint_refs, repo).ok_or_else(|| {
                "prod.keys not found: pass --keys <path/to/prod.keys>, or place your console's prod.keys beside your NSP/XCI files or in ~/.switch/prod.keys".to_string()
            })?,
        };

        let mut keys = KeySet::load_prod_keys(&keys_path)?;
        // Also check for title.keys beside the package files
        for d in &hint_dirs {
            let tk = d.join("title.keys");
            if tk.is_file() {
                if let Ok(t) = fs::read_to_string(&tk) {
                    keys.load_title_keys_str(&t);
                }
            }
        }

        // Open every container and collect tickets + NCA slices
        let mut raw_entries = Vec::new();
        for f in files {
            raw_entries.extend(open_package(f)?);
        }

        // Pass 1: parse all .tik tickets (prefer common over personalized if both exist for a rights ID)
        let mut tickets: HashMap<[u8; 16], ([u8; 16], bool)> = HashMap::new();
        for e in &raw_entries {
            if e.name.to_ascii_lowercase().ends_with(".tik") && e.slice.len <= 1024 * 1024 {
                if let Ok(bytes) = e.slice.read_vec_at(0, e.slice.len as usize) {
                    if let Ok(t) = parse_ticket(&bytes) {
                        tickets
                            .entry(t.rights_id)
                            .and_modify(|cur| {
                                if cur.1 && !t.is_personalized {
                                    *cur = (t.encrypted_title_key, false);
                                }
                            })
                            .or_insert((t.encrypted_title_key, t.is_personalized));
                    }
                }
            }
        }

        // Pass 2: decrypt headers of all .nca entries and filter to Program NCAs
        let mut b3_programs: Vec<(Nca, PathBuf)> = Vec::new();
        let mut other_titles: Vec<u64> = Vec::new();
        let mut first_nca_err: Option<String> = None;

        for e in raw_entries {
            let lower = e.name.to_ascii_lowercase();
            if !lower.ends_with(".nca") {
                continue;
            }
            match Nca::open(e.name, e.slice, &keys, &tickets) {
                Ok(nca) => {
                    if !nca.is_program() {
                        continue;
                    }
                    if (nca.title_id & BAYO3_TITLE_MASK) == BAYO3_BASE_ID {
                        b3_programs.push((nca, e.source_path));
                    } else if !other_titles.contains(&nca.title_id) {
                        other_titles.push(nca.title_id);
                    }
                }
                Err(err) => {
                    if first_nca_err.is_none() {
                        first_nca_err = Some(err);
                    }
                }
            }
        }

        if b3_programs.is_empty() {
            if !other_titles.is_empty() {
                let ids: Vec<String> = other_titles.iter().map(|t| format!("{:016X}", t)).collect();
                return Err(format!(
                    "no Bayonetta 3 (01004A4010FEA000) program found in the selected NSP/XCI file(s); found title ID(s): {}",
                    ids.join(", ")
                ));
            }
            if let Some(err) = first_nca_err {
                return Err(err);
            }
            return Err("no Switch Program NCAs found in the selected NSP/XCI file(s)".into());
        }

        // Separate Program 0 and Program 1 (The Old Picture Book prologue: 01004A4010FEA001 or content_index == 1)
        let mut prog0_bases: Vec<(Nca, PathBuf)> = Vec::new();
        let mut prog0_updates: Vec<(Nca, PathBuf)> = Vec::new();
        let mut prog1_bases: Vec<(Nca, PathBuf)> = Vec::new();
        let mut prog1_updates: Vec<(Nca, PathBuf)> = Vec::new();

        for (nca, src) in b3_programs {
            let is_prog1 = nca.content_index == 1 || (nca.title_id & 0xFF) == 1;
            if is_prog1 {
                if nca.has_bktr_patch() {
                    prog1_updates.push((nca, src));
                } else {
                    prog1_bases.push((nca, src));
                }
            } else if nca.has_bktr_patch() || (nca.title_id & 0x800) != 0 {
                prog0_updates.push((nca, src));
            } else {
                prog0_bases.push((nca, src));
            }
        }

        if prog0_updates.is_empty() {
            return Err(
                "found Bayonetta 3 base game (01004A4010FEA000), but Update 1.2.0 (01004A4010FEA800) is missing. \
                 Witch Time requires Update 1.2.0 (which also includes The Old Picture Book prologue). \
                 Select both your base game NSP/XCI and Update 1.2.0 NSP (or put both in the same folder)."
                    .into(),
            );
        }

        // Pick the best Program 0 update (preferring the one whose ExeFS `main` matches build ID 1.2.0)
        let mut best_upd_idx = 0usize;
        let mut matched_120 = false;
        for (i, (nca, _)) in prog0_updates.iter().enumerate() {
            if let Ok(exefs) = nca.open_exefs() {
                if let Ok(Some(main_bytes)) = exefs.read_file("main") {
                    if nso_build_id_bytes(&main_bytes).as_deref() == Some(MAIN_BUILD_ID) {
                        best_upd_idx = i;
                        matched_120 = true;
                        break;
                    }
                }
            }
        }
        if !matched_120 {
            // Fall back to highest sdk_version (so step_verify reports the exact build ID mismatch)
            best_upd_idx = prog0_updates
                .iter()
                .enumerate()
                .max_by_key(|(_, (n, _))| n.sdk_version)
                .map(|(i, _)| i)
                .unwrap_or(0);
        }
        let (prog0_update, upd_source) = prog0_updates.remove(best_upd_idx);

        // If Program 0 update uses BKTR, we need the Base Program 0 NCA
        let prog0_base = prog0_bases.into_iter().next().map(|(n, _)| n);
        if prog0_update.has_bktr_patch() && prog0_base.is_none() {
            return Err(
                "found Bayonetta 3 update (01004A4010FEA800), but the base game (01004A4010FEA000) is missing. \
                 Select both your base game NSP/XCI and Update 1.2.0 NSP (or put both in the same folder)."
                    .into(),
            );
        }

        // Resolve Program 1 (The Old Picture Book prologue)
        let (prog1, prog1_base) = if !prog1_updates.is_empty() {
            let (u, _) = prog1_updates.remove(0);
            let b = prog1_bases.into_iter().next().map(|(n, _)| n);
            (Some(u), b)
        } else if !prog1_bases.is_empty() {
            // Prefer exact Program 1 main SHA-256, else same container as Update 1.2.0
            let idx = prog1_bases
                .iter()
                .position(|(n, _)| {
                    n.open_exefs()
                        .ok()
                        .and_then(|e| e.read_file("main").ok().flatten())
                        .map(|b| crate::util::hex(&Sha256::digest(&b)) == PROGRAM1_MAIN_SHA256)
                        .unwrap_or(false)
                })
                .or_else(|| prog1_bases.iter().position(|(_, s)| *s == upd_source))
                .unwrap_or(0);
            let (p1, _) = prog1_bases.remove(idx);
            (Some(p1), None)
        } else {
            (None, None)
        };

        Ok(Self {
            keys_path,
            prog0_base,
            prog0_update,
            prog1_base,
            prog1,
        })
    }

    pub fn open_prog0_exefs(&self) -> Result<ExeFs, String> {
        self.prog0_update.open_exefs()
    }

    pub fn open_prog0_romfs(&self) -> Result<RomFs, String> {
        self.prog0_update.open_romfs(self.prog0_base.as_ref())
    }

    pub fn has_prog1(&self) -> bool {
        self.prog1.is_some()
    }

    pub fn open_prog1_exefs(&self) -> Result<Option<ExeFs>, String> {
        match &self.prog1 {
            Some(p1) => Ok(Some(p1.open_exefs()?)),
            None => Ok(None),
        }
    }

    pub fn open_prog1_romfs(&self) -> Result<Option<RomFs>, String> {
        match &self.prog1 {
            Some(p1) => Ok(Some(p1.open_romfs(self.prog1_base.as_ref())?)),
            None => Ok(None),
        }
    }

    /// Extract Program 0 ExeFS to `dst_exefs`.
    pub fn extract_prog0_exefs(&self, dst_exefs: &Path) -> Result<usize, String> {
        let exefs = self.open_prog0_exefs()?;
        exefs.extract_all(dst_exefs)
    }

    /// Stream Program 0 (merged base + update) RomFS to `dst_romfs`.
    pub fn extract_prog0_romfs(
        &self,
        dst_romfs: &Path,
        on_progress: &mut dyn FnMut(usize, usize, u64, u64),
    ) -> Result<(usize, usize), String> {
        let romfs = self.open_prog0_romfs()?;
        romfs.extract_all(dst_romfs, on_progress)
    }

    /// Extract Program 1 (Old Picture Book prologue) ExeFS + RomFS to `dst_program1/{exefs,romfs}`.
    pub fn extract_prog1(
        &self,
        dst_program1: &Path,
        on_progress: &mut dyn FnMut(usize, usize, u64, u64),
    ) -> Result<(usize, usize, usize), String> {
        let exefs = self
            .open_prog1_exefs()?
            .ok_or_else(|| "Program 1 (The Old Picture Book prologue) is not present in the provided NSP/XCI files".to_string())?;
        let romfs = self
            .open_prog1_romfs()?
            .ok_or_else(|| "Program 1 RomFS is not present in the provided NSP/XCI files".to_string())?;
        let n_exe = exefs.extract_all(&dst_program1.join("exefs"))?;
        let (total_rom, extracted_rom) = romfs.extract_all(&dst_program1.join("romfs"), on_progress)?;
        Ok((n_exe, total_rom, extracted_rom))
    }
}

/// CLI subcommand: `wtbuild extract <file_or_dir>... [--update <file>] [--keys <prod.keys>] --out <dir>`
pub fn main_extract(args: &[String], repo: &Path) -> Result<(), String> {
    let mut inputs: Vec<PathBuf> = Vec::new();
    let mut keys: Option<PathBuf> = None;
    let mut out: Option<PathBuf> = None;

    let mut it = args.iter();
    while let Some(a) = it.next() {
        let (k, inline) = match a.split_once('=') {
            Some((k, v)) if k.starts_with("--") => (k, Some(v.to_string())),
            _ => (a.as_str(), None),
        };
        let mut val = || {
            inline
                .clone()
                .or_else(|| it.next().cloned())
                .ok_or_else(|| format!("extract: {} requires a value", k))
        };
        match k {
            "--keys" => keys = Some(PathBuf::from(val()?)),
            "--out" => out = Some(PathBuf::from(val()?)),
            "--update" | "--dump" => inputs.push(PathBuf::from(val()?)),
            "--repo" => {
                let _ = val()?;
            }
            s if s.starts_with("--") => return Err(format!("extract: unknown option {}", s)),
            _ => inputs.push(PathBuf::from(a)),
        }
    }

    if inputs.is_empty() {
        return Err("usage: wtbuild extract <base.nsp|.xci|folder> [<update.nsp>] [--keys <prod.keys>] --out <folder>".into());
    }
    let out_dir = out.ok_or("extract: --out <folder> is required")?;

    // Expand directories into package files
    let mut pkg_files: Vec<PathBuf> = Vec::new();
    for p in &inputs {
        match classify_dump(p, None)? {
            DumpSource::Packages(v) => {
                for f in v {
                    if !pkg_files.contains(&f) {
                        pkg_files.push(f);
                    }
                }
            }
            DumpSource::ExtractedDir => {
                return Err(format!("{}: no .nsp or .xci files found", p.display()));
            }
        }
    }

    let ts = TitleSet::open(&pkg_files, keys.as_deref(), Some(repo))?;
    println!("keys: {}", ts.keys_path.display());
    println!("extracting Bayonetta 3 (Program 0) -> {}", out_dir.display());
    let n_exe = ts.extract_prog0_exefs(&out_dir.join("exefs"))?;
    println!("  exefs: {} files", n_exe);

    let mut last_pct = usize::MAX;
    let (total_r, new_r) = ts.extract_prog0_romfs(&out_dir.join("romfs"), &mut |done, total, bytes, total_bytes| {
        let pct = if total_bytes > 0 { (bytes * 100 / total_bytes) as usize } else { 100 };
        if done == total || pct / 5 != last_pct / 5 {
            last_pct = pct;
            println!(
                "  romfs: [{}/{}] {:.2} / {:.2} GB ({}%)",
                done,
                total,
                bytes as f64 / 1e9,
                total_bytes as f64 / 1e9,
                pct
            );
        }
    })?;
    println!("  romfs: {} files ({} written)", total_r, new_r);

    if ts.has_prog1() {
        println!("extracting The Old Picture Book prologue (Program 1) -> {}/program1", out_dir.display());
        let (p1_exe, p1_total, p1_new) = ts.extract_prog1(&out_dir.join("program1"), &mut |_, _, _, _| {})?;
        println!("  program1/exefs: {} files, program1/romfs: {} files ({} written)", p1_exe, p1_total, p1_new);
    } else {
        println!("  warning: Program 1 (The Old Picture Book prologue) was not found in the provided package(s)");
    }
    println!("done -> {}", out_dir.display());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::switch::container::{build_pfs0, build_xci};
    use crate::switch::crypto::{aes128_ctr_apply, aes128_ecb_encrypt, aes128_ecb_encrypt_block, aes128_xts_encrypt, make_upper_ctr};
    use crate::switch::nca::build_romfs;
    use crate::switch::storage::build_bktr_table;

    /// Helper: build a synthetic NCA3 encrypted with our synthetic test keys (NO real Nintendo keys!).
    struct SyntheticKeys {
        header_key: [u8; 32],
        kak_app: [u8; 16],
        titlekek: [u8; 16],
        prod_keys_text: String,
    }

    impl SyntheticKeys {
        fn make() -> Self {
            let header_key = [0x31u8; 32];
            let kak_app = [0x42u8; 16];
            let titlekek = [0x53u8; 16];
            let prod_keys_text = format!(
                "header_key = {}\nkey_area_key_application_0e = {}\ntitlekek_0e = {}\n",
                crate::util::hex(&header_key),
                crate::util::hex(&kak_app),
                crate::util::hex(&titlekek),
            );
            Self {
                header_key,
                kak_app,
                titlekek,
                prod_keys_text,
            }
        }
    }

    fn align_up(v: usize, a: usize) -> usize {
        (v + a - 1) & !(a - 1)
    }

    fn make_fake_nso(build_id_hex: &str, payload: &[u8]) -> Vec<u8> {
        let mut out = vec![0u8; 0x100];
        out[0..4].copy_from_slice(b"NSO0");
        for i in 0..20 {
            out[0x40 + i] = u8::from_str_radix(&build_id_hex[i * 2..i * 2 + 2], 16).unwrap();
        }
        out.extend_from_slice(payload);
        out
    }

    fn make_ticket(rights_id: &[u8; 16], enc_title_key: &[u8; 16]) -> Vec<u8> {
        let mut tik = vec![0u8; 0x2C0];
        tik[0..4].copy_from_slice(&0x010004u32.to_le_bytes()); // RSA-2048 SHA256
        tik[0x180..0x190].copy_from_slice(enc_title_key);
        tik[0x281] = 0; // common
        tik[0x2A0..0x2B0].copy_from_slice(rights_id);
        tik
    }

    fn finalize_nca(
        sk: &SyntheticKeys,
        title_id: u64,
        content_index: u32,
        rights_id: Option<[u8; 16]>,
        ctr_key: [u8; 16],
        sections: &[(usize, u64, u64, [u8; 0x200])],
        body: &[u8],
    ) -> Vec<u8> {
        let mut hdr = vec![0u8; 0xC00];
        hdr[0x200..0x204].copy_from_slice(b"NCA3");
        hdr[0x205] = 0; // Program
        hdr[0x207] = 0; // Application KAEK
        hdr[0x210..0x218].copy_from_slice(&title_id.to_le_bytes());
        hdr[0x218..0x21C].copy_from_slice(&content_index.to_le_bytes());
        hdr[0x220] = 0x0F; // key_gen 0x0F -> master_key_rev 0x0E
        if let Some(rid) = rights_id {
            hdr[0x230..0x240].copy_from_slice(&rid);
        } else {
            let mut ka = [0u8; 0x40];
            ka[0x20..0x30].copy_from_slice(&ctr_key);
            aes128_ecb_encrypt(&sk.kak_app, &mut ka);
            hdr[0x300..0x340].copy_from_slice(&ka);
        }
        for &(idx, start, size, ref fsh) in sections {
            let ms = (start / 0x200) as u32;
            let me = ((start + size) / 0x200) as u32;
            hdr[0x240 + idx * 16..0x244 + idx * 16].copy_from_slice(&ms.to_le_bytes());
            hdr[0x244 + idx * 16..0x248 + idx * 16].copy_from_slice(&me.to_le_bytes());
            let hash = Sha256::digest(fsh);
            hdr[0x280 + idx * 32..0x280 + (idx + 1) * 32].copy_from_slice(&hash);
            hdr[0x400 + idx * 0x200..0x600 + idx * 0x200].copy_from_slice(fsh);
        }
        let total_size = (0xC00 + body.len()) as u64;
        hdr[0x208..0x210].copy_from_slice(&total_size.to_le_bytes());
        aes128_xts_encrypt(&sk.header_key, 0, 0x200, &mut hdr);
        let mut nca = hdr;
        nca.extend_from_slice(body);
        nca
    }

    fn make_ivfc_header(romfs_off: u64, romfs_size: u64) -> [u8; 0xF8] {
        let mut h = [0u8; 0xF8];
        h[0..4].copy_from_slice(b"IVFC");
        h[4..8].copy_from_slice(&0x20000u32.to_le_bytes());
        h[0x0C..0x10].copy_from_slice(&7u32.to_le_bytes()); // 6 levels -> data level is index 5
        let lvl5 = 0x10 + 5 * 24;
        h[lvl5..lvl5 + 8].copy_from_slice(&romfs_off.to_le_bytes());
        h[lvl5 + 8..lvl5 + 16].copy_from_slice(&romfs_size.to_le_bytes());
        h
    }

    fn make_pfs0_hash_header(pfs0_off: u64, pfs0_size: u64) -> [u8; 0xF8] {
        let mut h = [0u8; 0xF8];
        h[0x24..0x28].copy_from_slice(&2u32.to_le_bytes()); // 2 layers
        h[0x38..0x40].copy_from_slice(&pfs0_off.to_le_bytes());
        h[0x40..0x48].copy_from_slice(&pfs0_size.to_le_bytes());
        h
    }

    #[test]
    fn end_to_end_sparse_base_bktr_update_and_program1() {
        let sk = SyntheticKeys::make();
        let dir = std::env::temp_dir().join(format!("wtbuild_nx_e2e_{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();

        let keys_path = dir.join("prod.keys");
        fs::write(&keys_path, &sk.prod_keys_text).unwrap();

        // ---- 1. Build a SPARSE Base Program 0 NCA (01004A4010FEA000), just like Bayonetta 3 eShop!
        // Virtual section layout:
        //   [0x0000 .. 0x1000): unallocated zero gap (storage_index = 1)
        //   [0x1000 .. 0x3000): stored physical chunk (8 KiB of base RomFS payload, encrypted at virtual offset!)
        let base_payload: Vec<u8> = (0..0x2000).map(|i| ((i * 17 + 5) & 0xFF) as u8).collect();
        let base_ctr_key = [0x61u8; 16];
        let base_sec_start: u64 = 0x4000; // virtual media_start_offset * 0x200
        let base_sec_virt_size: u64 = 0x3000;
        let base_phys_off: u64 = 0xC00; // physical start in NCA file
        let base_sec_val: u32 = 0x10000001;
        let base_sec_gen: u32 = 1;
        let sparse_gen: u16 = 2;

        // Encrypt physical chunk using VIRTUAL offset (base_sec_start + 0x1000)!
        let mut base_chunk_enc = base_payload.clone();
        aes128_ctr_apply(
            &base_ctr_key,
            make_upper_ctr(base_sec_val, base_sec_gen),
            base_sec_start + 0x1000,
            &mut base_chunk_enc,
        );

        // Sparse BKTR table sits at physical offset `base_phys_off + 0x2000`
        let mut s_entries = Vec::new();
        let mut e0 = vec![0u8; 0x14];
        e0[0..8].copy_from_slice(&0u64.to_le_bytes());
        e0[8..16].copy_from_slice(&0u64.to_le_bytes());
        e0[16..20].copy_from_slice(&1i32.to_le_bytes()); // zero gap
        s_entries.push(e0);
        let mut e1 = vec![0u8; 0x14];
        e1[0..8].copy_from_slice(&0x1000u64.to_le_bytes());
        e1[8..16].copy_from_slice(&0u64.to_le_bytes()); // phys 0 relative to PhysicalOffset
        e1[16..20].copy_from_slice(&0i32.to_le_bytes()); // physical data
        s_entries.push(e1);
        let mut sparse_table = build_bktr_table(base_sec_virt_size, 0x14, &s_entries);
        let sparse_table_rel: u64 = 0x2000;
        aes128_ctr_apply(
            &base_ctr_key,
            make_upper_ctr(base_sec_val, (sparse_gen as u32) << 16),
            base_phys_off + sparse_table_rel,
            &mut sparse_table,
        );

        let mut base_fsh = [0u8; 0x200];
        base_fsh[2] = 0; // RomFS
        base_fsh[3] = 3; // IVFC
        base_fsh[4] = 3; // AesCtr
        base_fsh[0x08..0x100].copy_from_slice(&make_ivfc_header(0x1000, 0x2000));
        base_fsh[0x140..0x144].copy_from_slice(&base_sec_gen.to_le_bytes());
        base_fsh[0x144..0x148].copy_from_slice(&base_sec_val.to_le_bytes());
        // SparseInfo at +0x148
        base_fsh[0x148..0x150].copy_from_slice(&sparse_table_rel.to_le_bytes());
        base_fsh[0x150..0x158].copy_from_slice(&(sparse_table.len() as u64).to_le_bytes());
        base_fsh[0x158..0x15C].copy_from_slice(b"BKTR");
        base_fsh[0x160..0x164].copy_from_slice(&2u32.to_le_bytes());
        base_fsh[0x168..0x170].copy_from_slice(&base_phys_off.to_le_bytes());
        base_fsh[0x170..0x172].copy_from_slice(&sparse_gen.to_le_bytes());

        let mut base_body = Vec::new();
        base_body.extend_from_slice(&base_chunk_enc);
        base_body.extend_from_slice(&sparse_table);
        let base_nca = finalize_nca(
            &sk,
            0x0100_4A40_10FE_A000,
            0,
            None,
            base_ctr_key,
            &[(1, base_sec_start, base_sec_virt_size, base_fsh)],
            &base_body,
        );

        // ---- 2. Build a BKTR-patched Update Program 0 NCA (01004A4010FEA800) with TitleKey crypto (.tik)!
        // Combined RomFS image:
        //   - "root_000.pkz": comes from base_payload[0x200..0x1200] (4096 bytes) via IndirectStorage is_patch=0!
        //   - "movie/intro.webm": comes from patch (200 bytes) via IndirectStorage is_patch=1!
        let merged_romfs = build_romfs(&[
            ("root_000.pkz", &base_payload[0x200..0x1200]),
            ("movie/intro.webm", b"synthetic-webm-movie-bytes-from-update"),
        ]);
        // Construct relocation: we'll store RomFS header + metadata in patch, and point root_000.pkz's exact range
        // to base_payload! Look up root_000.pkz's offset in merged_romfs:
        let root_pos = merged_romfs
            .windows(32)
            .position(|w| w == &base_payload[0x200..0x220])
            .unwrap();
        let root_len = 0x1000usize;
        let romfs_ivfc_off = 0x200u64; // virtual start of RomFS inside the patched section
        let total_virt = romfs_ivfc_off + merged_romfs.len() as u64;

        // Patch raw section layout:
        //   [0 .. patch_a_len): merged_romfs[0 .. root_pos] (with ctr_val = 7)
        //   [patch_a_len .. patch_a_len + patch_b_len): merged_romfs[root_pos + root_len ..] (with ctr_val = 9)
        let part_a = &merged_romfs[..root_pos];
        let part_b = &merged_romfs[root_pos + root_len..];
        let mut patch_data = Vec::new();
        patch_data.extend_from_slice(part_a);
        let part_b_off = patch_data.len() as u64;
        patch_data.extend_from_slice(part_b);
        let patch_data_aligned = align_up(patch_data.len(), 0x200);
        patch_data.resize(patch_data_aligned, 0);

        // ExeFS (section 0) of Update Program 0
        let main_nso = make_fake_nso(MAIN_BUILD_ID, b"bayo3-main-payload");
        let exefs_pfs0 = build_pfs0(&[("main", &main_nso), ("main.npdm", b"META-test")]);
        let exefs_sec_start = 0xC00u64;
        let exefs_sec_size = align_up(exefs_pfs0.len(), 0x200) as u64;
        let mut exefs_enc = exefs_pfs0.clone();
        exefs_enc.resize(exefs_sec_size as usize, 0);

        let upd_rights_id = [0x99u8; 16];
        let upd_plain_key = [0x71u8; 16];
        let upd_enc_tk = aes128_ecb_encrypt_block(&sk.titlekek, &upd_plain_key);
        let upd_tik = make_ticket(&upd_rights_id, &upd_enc_tk);

        let exefs_sec_val = 0x20000001u32;
        aes128_ctr_apply(&upd_plain_key, make_upper_ctr(exefs_sec_val, 0), exefs_sec_start, &mut exefs_enc);

        let mut upd_fsh0 = [0u8; 0x200];
        upd_fsh0[2] = 1; // PartitionFS (ExeFS)
        upd_fsh0[3] = 2; // HierarchicalSha256
        upd_fsh0[4] = 3; // AesCtr
        upd_fsh0[0x08..0x100].copy_from_slice(&make_pfs0_hash_header(0, exefs_pfs0.len() as u64));
        upd_fsh0[0x144..0x148].copy_from_slice(&exefs_sec_val.to_le_bytes());

        // RomFS BKTR (section 1) of Update Program 0
        let rom_sec_start = exefs_sec_start + exefs_sec_size;
        let rom_sec_val = 0x20000002u32;
        let rom_sec_gen = 3u32;

        // Encrypt part_a with ctr_val=7 and part_b with ctr_val=9!
        aes128_ctr_apply(&upd_plain_key, make_upper_ctr(rom_sec_val, 7), rom_sec_start, &mut patch_data[..part_a.len()]);
        aes128_ctr_apply(
            &upd_plain_key,
            make_upper_ctr(rom_sec_val, 9),
            rom_sec_start + part_b_off,
            &mut patch_data[part_b_off as usize..],
        );

        // Relocation BKTR:
        //   [0 .. romfs_ivfc_off + root_pos): patch @ 0 (is_patch = 1)
        //   [romfs_ivfc_off + root_pos .. romfs_ivfc_off + root_pos + root_len): base @ 0x1000 + 0x200 (is_patch = 0!)
        //   [romfs_ivfc_off + root_pos + root_len .. total_virt): patch @ part_b_off (is_patch = 1)
        let mut r_entries = Vec::new();
        for (v, p, is_p) in [
            (0u64, 0u64, 1i32),
            (romfs_ivfc_off, 0u64, 1i32),
            (romfs_ivfc_off + root_pos as u64, 0x1000u64 + 0x200, 0i32),
            (romfs_ivfc_off + (root_pos + root_len) as u64, part_b_off, 1i32),
        ] {
            let mut b = vec![0u8; 0x14];
            b[0..8].copy_from_slice(&v.to_le_bytes());
            b[8..16].copy_from_slice(&p.to_le_bytes());
            b[16..20].copy_from_slice(&is_p.to_le_bytes());
            r_entries.push(b);
        }
        let reloc_off = patch_data.len() as u64;
        let mut reloc_tbl = build_bktr_table(total_virt, 0x14, &r_entries);
        aes128_ctr_apply(
            &upd_plain_key,
            make_upper_ctr(rom_sec_val, rom_sec_gen),
            rom_sec_start + reloc_off,
            &mut reloc_tbl,
        );

        // Subsection BKTR:
        //   [0 .. part_b_off): ctr_val = 7
        //   [part_b_off .. patch_data.len()): ctr_val = 9
        let mut ss_entries = Vec::new();
        for (o, cv) in [(0u64, 7u32), (part_b_off, 9u32)] {
            let mut b = vec![0u8; 0x10];
            b[0..8].copy_from_slice(&o.to_le_bytes());
            b[12..16].copy_from_slice(&cv.to_le_bytes());
            ss_entries.push(b);
        }
        let subsec_off = reloc_off + reloc_tbl.len() as u64;
        let mut subsec_tbl = build_bktr_table(patch_data.len() as u64, 0x10, &ss_entries);
        aes128_ctr_apply(
            &upd_plain_key,
            make_upper_ctr(rom_sec_val, rom_sec_gen),
            rom_sec_start + subsec_off,
            &mut subsec_tbl,
        );

        let mut upd_rom_sec = patch_data;
        upd_rom_sec.extend_from_slice(&reloc_tbl);
        upd_rom_sec.extend_from_slice(&subsec_tbl);
        let upd_rom_sec_size = upd_rom_sec.len() as u64;

        let mut upd_fsh1 = [0u8; 0x200];
        upd_fsh1[2] = 0; // RomFS
        upd_fsh1[3] = 3; // IVFC
        upd_fsh1[4] = 4; // AesCtrEx (BKTR)
        upd_fsh1[0x08..0x100].copy_from_slice(&make_ivfc_header(romfs_ivfc_off, merged_romfs.len() as u64));
        upd_fsh1[0x100..0x108].copy_from_slice(&reloc_off.to_le_bytes());
        upd_fsh1[0x108..0x110].copy_from_slice(&(reloc_tbl.len() as u64).to_le_bytes());
        upd_fsh1[0x110..0x114].copy_from_slice(b"BKTR");
        upd_fsh1[0x120..0x128].copy_from_slice(&subsec_off.to_le_bytes());
        upd_fsh1[0x128..0x130].copy_from_slice(&(subsec_tbl.len() as u64).to_le_bytes());
        upd_fsh1[0x130..0x134].copy_from_slice(b"BKTR");
        upd_fsh1[0x140..0x144].copy_from_slice(&rom_sec_gen.to_le_bytes());
        upd_fsh1[0x144..0x148].copy_from_slice(&rom_sec_val.to_le_bytes());

        let mut upd_body = exefs_enc;
        upd_body.extend_from_slice(&upd_rom_sec);
        let upd_nca = finalize_nca(
            &sk,
            0x0100_4A40_10FE_A800,
            0,
            Some(upd_rights_id),
            upd_plain_key,
            &[
                (0, exefs_sec_start, exefs_sec_size, upd_fsh0),
                (1, rom_sec_start, upd_rom_sec_size, upd_fsh1),
            ],
            &upd_body,
        );

        // ---- 3. Build Program 1 NCA (The Old Picture Book prologue: 01004A4010FEA001, content_index=1)
        let p1_key = [0x81u8; 16];
        let p1_exefs = build_pfs0(&[("main", b"NSO0-prologue-main"), ("sdk", b"NSO0-prologue-sdk"), ("main.npdm", b"META-p1")]);
        let p1_romfs = build_romfs(&[("root_000.pkz", b"prologue-root-pkz"), ("data/story.dat", b"cereza-story")]);
        let p1_s0_start = 0xC00u64;
        let p1_s0_size = align_up(p1_exefs.len(), 0x200) as u64;
        let mut p1_s0 = p1_exefs.clone();
        p1_s0.resize(p1_s0_size as usize, 0);
        aes128_ctr_apply(&p1_key, make_upper_ctr(0x30000001, 0), p1_s0_start, &mut p1_s0);

        let p1_s1_start = p1_s0_start + p1_s0_size;
        let p1_s1_size = align_up(p1_romfs.len(), 0x200) as u64;
        let mut p1_s1 = p1_romfs.clone();
        p1_s1.resize(p1_s1_size as usize, 0);
        aes128_ctr_apply(&p1_key, make_upper_ctr(0x30000002, 0), p1_s1_start, &mut p1_s1);

        let mut p1_fsh0 = [0u8; 0x200];
        p1_fsh0[2] = 1;
        p1_fsh0[3] = 2;
        p1_fsh0[4] = 5; // AesCtrSkipLayerHash (14.0+)
        p1_fsh0[0x08..0x100].copy_from_slice(&make_pfs0_hash_header(0, p1_exefs.len() as u64));
        p1_fsh0[0x144..0x148].copy_from_slice(&0x30000001u32.to_le_bytes());

        let mut p1_fsh1 = [0u8; 0x200];
        p1_fsh1[2] = 0;
        p1_fsh1[3] = 3;
        p1_fsh1[4] = 5; // AesCtrSkipLayerHash
        p1_fsh1[0x08..0x100].copy_from_slice(&make_ivfc_header(0, p1_romfs.len() as u64));
        p1_fsh1[0x144..0x148].copy_from_slice(&0x30000002u32.to_le_bytes());

        let mut p1_body = p1_s0;
        p1_body.extend_from_slice(&p1_s1);
        let p1_nca = finalize_nca(
            &sk,
            0x0100_4A40_10FE_A001,
            1,
            None,
            p1_key,
            &[
                (0, p1_s0_start, p1_s0_size, p1_fsh0),
                (1, p1_s1_start, p1_s1_size, p1_fsh1),
            ],
            &p1_body,
        );

        // ---- 4. Test Scenario A: Base XCI (with card key area) + Update NSP (with Program 0 + Program 1 + .tik)
        let base_xci_path = dir.join("base.xci");
        let upd_nsp_path = dir.join("update_120.nsp");
        fs::write(&base_xci_path, build_xci(&[("base.nca", &base_nca)], true)).unwrap();
        fs::write(
            &upd_nsp_path,
            build_pfs0(&[("upd0.nca", &upd_nca), ("prog1.nca", &p1_nca), ("upd.tik", &upd_tik)]),
        )
        .unwrap();

        let ts = TitleSet::open(&[base_xci_path.clone(), upd_nsp_path.clone()], Some(&keys_path), None).unwrap();
        assert!(ts.has_prog1());

        let out_a = dir.join("out_a");
        ts.extract_prog0_exefs(&out_a.join("exefs")).unwrap();
        let (total_r, wrote_r) = ts.extract_prog0_romfs(&out_a.join("romfs"), &mut |_, _, _, _| {}).unwrap();
        assert_eq!((total_r, wrote_r), (2, 2));
        // Second run must resume/skip both files (0 written)
        let (total_r2, wrote_r2) = ts.extract_prog0_romfs(&out_a.join("romfs"), &mut |_, _, _, _| {}).unwrap();
        assert_eq!((total_r2, wrote_r2), (2, 0));

        let (p1_exe_n, p1_rom_n, p1_rom_w) = ts.extract_prog1(&out_a.join("program1"), &mut |_, _, _, _| {}).unwrap();
        assert_eq!((p1_exe_n, p1_rom_n, p1_rom_w), (3, 2, 2));

        // Verify exact byte-for-byte contents of Program 0 and Program 1!
        assert_eq!(fs::read(out_a.join("exefs/main")).unwrap(), main_nso);
        assert_eq!(fs::read(out_a.join("exefs/main.npdm")).unwrap(), b"META-test");
        assert_eq!(fs::read(out_a.join("romfs/root_000.pkz")).unwrap(), &base_payload[0x200..0x1200]);
        assert_eq!(
            fs::read(out_a.join("romfs/movie/intro.webm")).unwrap(),
            b"synthetic-webm-movie-bytes-from-update"
        );
        assert_eq!(fs::read(out_a.join("program1/exefs/main")).unwrap(), b"NSO0-prologue-main");
        assert_eq!(fs::read(out_a.join("program1/romfs/root_000.pkz")).unwrap(), b"prologue-root-pkz");
        assert_eq!(fs::read(out_a.join("program1/romfs/data/story.dat")).unwrap(), b"cereza-story");

        // ---- 5. Test Scenario B: Single Merged Super-XCI containing Base + Update + Program 1
        let super_xci = dir.join("merged.xci");
        fs::write(
            &super_xci,
            build_xci(&[("base.nca", &base_nca), ("upd0.nca", &upd_nca), ("prog1.nca", &p1_nca), ("upd.tik", &upd_tik)], false),
        )
        .unwrap();
        let ts_super = TitleSet::open(&[super_xci], Some(&keys_path), None).unwrap();
        assert!(ts_super.has_prog1());
        let romfs = ts_super.open_prog0_romfs().unwrap();
        let ((cnt, _, _), _) = romfs.manifest();
        assert_eq!(cnt, 2);

        // ---- 6. Test Scenario C: Error paths (base-only, update-only)
        let base_only = TitleSet::open(&[base_xci_path], Some(&keys_path), None);
        assert!(base_only.err().unwrap().contains("Update 1.2.0"));
        let upd_only = TitleSet::open(&[upd_nsp_path], Some(&keys_path), None);
        assert!(upd_only.err().unwrap().contains("base game"));

        fs::remove_dir_all(&dir).unwrap();
    }
}
