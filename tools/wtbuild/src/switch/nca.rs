//! [witchtime] Nintendo Switch NCA3/NCA2 parser, ExeFS (PFS0) reader, and RomFS reader/extractor.

use super::container::FileSlice;
use super::crypto::{aes128_ctr_apply, aes128_ecb_decrypt, aes128_xts_decrypt, make_upper_ctr};
use super::keys::KeySet;
use super::storage::{
    parse_compression_bktr, parse_indirect_bktr, parse_subsection_bktr, read_vec, AesCtrExStorage, AesCtrStorage,
    AesXtsStorage, CompressedStorage, IndirectStorage, SliceStorage, SparseStorage, Storage, SubStorage,
};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::Write;
use std::path::Path;
use std::sync::Arc;

const ROMFS_ENTRY_EMPTY: u32 = 0xFFFF_FFFF;

fn u16_le(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes(b[off..off + 2].try_into().unwrap())
}

fn u32_le(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(b[off..off + 4].try_into().unwrap())
}

fn u64_le(b: &[u8], off: usize) -> u64 {
    u64::from_le_bytes(b[off..off + 8].try_into().unwrap())
}

#[derive(Debug, Clone)]
pub struct SectionInfo {
    pub index: usize,
    pub start: u64,
    pub size: u64,
    pub fs_type: u8,         // 0 = RomFS, 1 = PartitionFS (ExeFS)
    pub hash_type: u8,       // 2 = HierarchicalSha256, 3 = HierarchicalIntegrity (IVFC)
    pub encryption_type: u8, // 1 = None, 2 = AesXts, 3/5 = AesCtr, 4/6 = AesCtrEx (BKTR)
    pub fs_header: [u8; 0x200],
}

impl SectionInfo {
    pub fn is_bktr_patch(&self) -> bool {
        (self.encryption_type == 4 || self.encryption_type == 6) && &self.fs_header[0x110..0x114] == b"BKTR"
    }

    pub fn generation(&self) -> u32 {
        u32_le(&self.fs_header, 0x140)
    }

    pub fn secure_value(&self) -> u32 {
        u32_le(&self.fs_header, 0x144)
    }
}

#[derive(Clone)]
pub struct Nca {
    pub name: String,
    pub slice: FileSlice,
    pub title_id: u64,
    pub content_type: u8, // 0 = Program, 1 = Meta, 2 = Control, ...
    pub content_index: u32,
    pub sdk_version: u32,
    pub sections: Vec<SectionInfo>,
    /// Decrypted AES-128 CTR/BKTR content key (slot 2 of key area, or decrypted titlekey).
    pub ctr_key: Option<[u8; 16]>,
    /// Decrypted AES-128 XTS content key (slots 0..1 of key area), if standard crypto.
    pub xts_key: Option<[u8; 32]>,
    /// Why `ctr_key` could not be unwrapped (deferred until this NCA is actually needed).
    pub key_error: Option<String>,
}

impl Nca {
    /// Read and decrypt the `0xC00`-byte NCA header and unwrap its content key.
    pub fn open(
        name: String,
        slice: FileSlice,
        keys: &KeySet,
        tickets: &HashMap<[u8; 16], ([u8; 16], bool)>,
    ) -> Result<Self, String> {
        if slice.len < 0xC00 {
            return Err(format!("{}: too small to be an NCA ({} bytes)", name, slice.len));
        }
        let header_key = keys.header_key()?;
        let mut hdr = slice.read_vec_at(0, 0xC00)?;
        // Decrypt sectors 0 and 1 first to inspect NCA3 vs NCA2 magic
        aes128_xts_decrypt(header_key, 0, 0x200, &mut hdr[0..0x400]);
        let magic = &hdr[0x200..0x204];
        if magic == b"NCA3" {
            aes128_xts_decrypt(header_key, 2, 0x200, &mut hdr[0x400..0xC00]);
        } else if magic == b"NCA2" {
            for s in 0..4 {
                aes128_xts_decrypt(header_key, 0, 0x200, &mut hdr[0x400 + s * 0x200..0x600 + s * 0x200]);
            }
        } else {
            return Err(format!(
                "{}: invalid NCA header after decryption (wrong `header_key` in prod.keys, or corrupt NCA)",
                name
            ));
        }

        let content_type = hdr[0x205];
        let key_gen_old = hdr[0x206];
        let kaek_index = hdr[0x207];
        let title_id = u64_le(&hdr, 0x210);
        let content_index = u32_le(&hdr, 0x218);
        let sdk_version = u32_le(&hdr, 0x21C);
        let key_gen_new = hdr[0x220];
        let key_generation = key_gen_old.max(key_gen_new);
        let master_key_rev = key_generation.saturating_sub(1);
        let rights_id: [u8; 16] = hdr[0x230..0x240].try_into().unwrap();

        let mut sections = Vec::new();
        for i in 0..4 {
            let e = &hdr[0x240 + i * 16..0x240 + (i + 1) * 16];
            let m_start = u32_le(e, 0) as u64;
            let m_end = u32_le(e, 4) as u64;
            if m_start == 0 && m_end == 0 {
                continue;
            }
            if m_end < m_start {
                return Err(format!("{}: invalid section {} range", name, i));
            }
            let start = m_start * 0x200;
            let size = (m_end - m_start) * 0x200;
            let mut fs_header = [0u8; 0x200];
            fs_header.copy_from_slice(&hdr[0x400 + i * 0x200..0x600 + i * 0x200]);
            let want_hash = &hdr[0x280 + i * 32..0x280 + (i + 1) * 32];
            if want_hash.iter().any(|&b| b != 0) {
                let got = Sha256::digest(&fs_header);
                if got.as_slice() != want_hash {
                    return Err(format!("{}: section {} FS header SHA-256 mismatch", name, i));
                }
            }
            sections.push(SectionInfo {
                index: i,
                start,
                size,
                fs_type: fs_header[2],
                hash_type: fs_header[3],
                encryption_type: fs_header[4],
                fs_header,
            });
        }

        // Unwrap content keys (defer errors so unrelated NCAs like Control/Manual don't fail header scanning)
        let mut ctr_key = None;
        let mut xts_key = None;
        let mut key_error = None;
        let has_rights_id = rights_id.iter().any(|&b| b != 0);
        if has_rights_id {
            let enc_tk = if let Some(&(tk, personalized)) = tickets.get(&rights_id) {
                if personalized {
                    // Fallback to title.keys if the ticket in the NSP was personalized
                    if let Some(&from_file) = keys.encrypted_title_keys.get(&rights_id) {
                        Ok(from_file)
                    } else {
                        Err(format!(
                            "{}: uses a console-personalized ticket (rights ID {}). Provide `title.keys` beside `prod.keys`, or dump with a standard/common ticket.",
                            name,
                            crate::util::hex(&rights_id)
                        ))
                    }
                } else {
                    Ok(tk)
                }
            } else if let Some(&from_file) = keys.encrypted_title_keys.get(&rights_id) {
                Ok(from_file)
            } else {
                Err(format!(
                    "{}: requires title key for rights ID {}, but no matching `.tik` ticket was found in the package and no `title.keys` entry matched",
                    name,
                    crate::util::hex(&rights_id)
                ))
            };
            match enc_tk.and_then(|tk| keys.decrypt_title_key(&tk, master_key_rev)) {
                Ok(k) => ctr_key = Some(k),
                Err(e) => key_error = Some(e),
            }
        } else {
            match keys.key_area_key(kaek_index, master_key_rev) {
                Ok(kak) => {
                    let mut ka = [0u8; 0x40];
                    ka.copy_from_slice(&hdr[0x300..0x340]);
                    aes128_ecb_decrypt(kak, &mut ka);
                    xts_key = Some(ka[0x00..0x20].try_into().unwrap());
                    ctr_key = Some(ka[0x20..0x30].try_into().unwrap());
                }
                Err(e) => key_error = Some(e),
            }
        }

        Ok(Self {
            name,
            slice,
            title_id,
            content_type,
            content_index,
            sdk_version,
            sections,
            ctr_key,
            xts_key,
            key_error,
        })
    }

    pub fn is_program(&self) -> bool {
        self.content_type == 0
    }

    pub fn has_bktr_patch(&self) -> bool {
        self.sections.iter().any(|s| s.is_bktr_patch())
    }

    pub fn exefs_section(&self) -> Option<&SectionInfo> {
        self.sections.iter().find(|s| s.fs_type == 1)
    }

    pub fn romfs_section(&self) -> Option<&SectionInfo> {
        self.sections.iter().find(|s| s.fs_type == 0)
    }

    fn require_ctr_key(&self) -> Result<[u8; 16], String> {
        self.ctr_key.ok_or_else(|| {
            self.key_error
                .clone()
                .unwrap_or_else(|| format!("{}: missing decryption key", self.name))
        })
    }

    /// Open a section as a decrypted `Storage` (handling Sparse, Plain, AesCtr, AesXts, and BKTR Patch).
    pub fn open_section_storage(
        &self,
        sec: &SectionInfo,
        base_section: Option<Arc<dyn Storage>>,
    ) -> Result<Arc<dyn Storage>, String> {
        let fh = &sec.fs_header;

        // 1. Sparse storage (Switch 12.0+ compacted NCAs, e.g. Bayonetta 3 eShop base NCA)
        let sparse_gen = u16_le(fh, 0x170);
        if sparse_gen != 0 && !sec.is_bktr_patch() {
            let table_off = u64_le(fh, 0x148);
            let table_size = u64_le(fh, 0x150);
            let table_entries_cnt = u32_le(fh, 0x160);
            let physical_offset = u64_le(fh, 0x168);

            let crypto = match sec.encryption_type {
                1 => None,
                3 | 5 => Some((self.require_ctr_key()?, make_upper_ctr(sec.secure_value(), sec.generation()))),
                other => {
                    return Err(format!(
                        "{}: unsupported encryption type {} on sparse section {}",
                        self.name, other, sec.index
                    ));
                }
            };

            if table_entries_cnt == 0 || table_size == 0 {
                return Ok(Arc::new(SparseStorage::new(
                    self.slice.clone(),
                    physical_offset,
                    sec.start,
                    sec.size,
                    Vec::new(),
                    crypto,
                )));
            }
            if &fh[0x158..0x15C] != b"BKTR" {
                return Err(format!("{}: sparse section {} missing BKTR magic", self.name, sec.index));
            }
            let abs_table_off = physical_offset + table_off;
            let mut table_buf = self.slice.read_vec_at(abs_table_off, table_size as usize)?;
            if let Some((key, _)) = crypto {
                let table_upper = make_upper_ctr(sec.secure_value(), (sparse_gen as u32) << 16);
                aes128_ctr_apply(&key, table_upper, abs_table_off, &mut table_buf);
            }
            let (end_off, entries) = parse_indirect_bktr(&table_buf)?;
            return Ok(Arc::new(SparseStorage::new(
                self.slice.clone(),
                physical_offset,
                sec.start,
                end_off.max(sec.size),
                entries,
                crypto,
            )));
        }

        // 2. BKTR Patch section (Update NCA AesCtrEx / AesCtrExSkipLayerHash)
        if sec.is_bktr_patch() {
            let base = base_section.ok_or_else(|| {
                format!(
                    "{}: section {} is an update BKTR patch and requires the base game NCA",
                    self.name, sec.index
                )
            })?;
            let key = self.require_ctr_key()?;
            let reloc_off = u64_le(fh, 0x100);
            let reloc_size = u64_le(fh, 0x108);
            let subsec_off = u64_le(fh, 0x120);
            let subsec_size = u64_le(fh, 0x128);
            if &fh[0x130..0x134] != b"BKTR" {
                return Err(format!(
                    "{}: patch section {} missing subsection BKTR magic",
                    self.name, sec.index
                ));
            }

            let raw_sec = self.slice.subslice(sec.start, sec.size)?;
            let raw_storage: Arc<dyn Storage> = Arc::new(SliceStorage::new(raw_sec));
            let table_upper = make_upper_ctr(sec.secure_value(), sec.generation());

            // Both BKTR tables are encrypted with regular AES-CTR using FsHeader's (SecureValue, Generation)
            let mut reloc_buf = read_vec(raw_storage.as_ref(), reloc_off, reloc_size as usize)?;
            aes128_ctr_apply(&key, table_upper, sec.start + reloc_off, &mut reloc_buf);
            let (reloc_end, reloc_entries) = parse_indirect_bktr(&reloc_buf)?;

            let mut subsec_buf = read_vec(raw_storage.as_ref(), subsec_off, subsec_size as usize)?;
            aes128_ctr_apply(&key, table_upper, sec.start + subsec_off, &mut subsec_buf);
            let (subsec_end, subsec_entries) = parse_subsection_bktr(&subsec_buf)?;

            let patch_storage: Arc<dyn Storage> = Arc::new(AesCtrExStorage::new(
                raw_storage,
                sec.start,
                key,
                sec.secure_value(),
                subsec_end,
                subsec_entries,
            ));
            return Ok(Arc::new(IndirectStorage::new(base, patch_storage, reloc_end, reloc_entries)));
        }

        // 3. Regular (non-sparse, non-patch) section
        let raw_sec = self.slice.subslice(sec.start, sec.size)?;
        let raw_storage: Arc<dyn Storage> = Arc::new(SliceStorage::new(raw_sec));
        match sec.encryption_type {
            1 => Ok(raw_storage),
            2 => {
                let xk = self
                    .xts_key
                    .ok_or_else(|| format!("{}: section {} uses AES-XTS without XTS key", self.name, sec.index))?;
                Ok(Arc::new(AesXtsStorage::new(raw_storage, xk, 0x4000)))
            }
            3 | 5 => {
                let key = self.require_ctr_key()?;
                let upper = make_upper_ctr(sec.secure_value(), sec.generation());
                Ok(Arc::new(AesCtrStorage::new(raw_storage, key, upper, sec.start)))
            }
            other => Err(format!(
                "{}: unsupported section {} encryption type {}",
                self.name, sec.index, other
            )),
        }
    }

    /// Wrap data-level storage with `CompressedStorage` if `CompressionInfo` (FsHeader + 0x178) is active.
    fn maybe_wrap_compression(
        &self,
        sec: &SectionInfo,
        data_storage: Arc<dyn Storage>,
    ) -> Result<Arc<dyn Storage>, String> {
        let fh = &sec.fs_header;
        let comp_off = u64_le(fh, 0x178);
        let comp_size = u64_le(fh, 0x180);
        if comp_size == 0 || &fh[0x188..0x18C] != b"BKTR" {
            return Ok(data_storage);
        }
        let table = read_vec(data_storage.as_ref(), comp_off, comp_size as usize)?;
        let (end_off, entries) = parse_compression_bktr(&table)?;
        Ok(Arc::new(CompressedStorage::new(data_storage, end_off, entries)))
    }

    /// Open ExeFS (PFS0) from this NCA.
    pub fn open_exefs(&self) -> Result<ExeFs, String> {
        let sec = self
            .exefs_section()
            .ok_or_else(|| format!("{}: NCA has no ExeFS section", self.name))?;
        if sec.hash_type != 2 {
            return Err(format!(
                "{}: ExeFS section has unexpected hash_type {} (expected 2)",
                self.name, sec.hash_type
            ));
        }
        let pfs0_off = u64_le(&sec.fs_header, 0x08 + 0x38);
        let pfs0_size = u64_le(&sec.fs_header, 0x08 + 0x40);
        if pfs0_size == 0 {
            return Err(format!("{}: ExeFS section is empty", self.name));
        }
        let sec_storage = self.open_section_storage(sec, None)?;
        let raw_pfs0: Arc<dyn Storage> = Arc::new(SubStorage::new(sec_storage, pfs0_off, pfs0_size)?);
        let pfs0_storage = self.maybe_wrap_compression(sec, raw_pfs0)?;
        ExeFs::open(pfs0_storage)
    }

    /// Open RomFS from this NCA (supply `base_nca` when this is an update NCA with a BKTR RomFS).
    pub fn open_romfs(&self, base_nca: Option<&Nca>) -> Result<RomFs, String> {
        let sec = self
            .romfs_section()
            .ok_or_else(|| format!("{}: NCA has no RomFS section", self.name))?;
        if sec.hash_type != 3 {
            return Err(format!(
                "{}: RomFS section has unexpected hash_type {} (expected 3)",
                self.name, sec.hash_type
            ));
        }
        let base_sec_storage = if sec.is_bktr_patch() {
            let base = base_nca.ok_or_else(|| {
                format!("{}: update RomFS requires base game NCA", self.name)
            })?;
            let bsec = base
                .romfs_section()
                .ok_or_else(|| format!("{}: base NCA has no RomFS section", base.name))?;
            Some(base.open_section_storage(bsec, None)?)
        } else {
            None
        };

        let fh = &sec.fs_header;
        if &fh[0x08..0x0C] != b"IVFC" {
            return Err(format!("{}: RomFS section missing IVFC magic", self.name));
        }
        let num_levels = u32_le(fh, 0x08 + 0x0C) as usize;
        if !(2..=7).contains(&num_levels) {
            return Err(format!("{}: unexpected IVFC level count {}", self.name, num_levels));
        }
        let data_level = num_levels - 2;
        let lvl_off = 0x18 + data_level * 24;
        let romfs_off = u64_le(fh, lvl_off);
        let romfs_size = u64_le(fh, lvl_off + 8);

        let sec_storage = self.open_section_storage(sec, base_sec_storage)?;
        let raw_romfs: Arc<dyn Storage> = Arc::new(SubStorage::new(sec_storage, romfs_off, romfs_size)?);
        let romfs_storage = self.maybe_wrap_compression(sec, raw_romfs)?;
        RomFs::open(romfs_storage)
    }
}

// ---------------------------------------------------------------------------------------------- ExeFS (PFS0)

pub struct ExeFsEntry {
    pub name: String,
    pub offset: u64,
    pub size: u64,
}

pub struct ExeFs {
    storage: Arc<dyn Storage>,
    data_start: u64,
    pub entries: Vec<ExeFsEntry>,
}

impl ExeFs {
    pub fn open(storage: Arc<dyn Storage>) -> Result<Self, String> {
        if storage.size() < 0x10 {
            return Err("ExeFS PFS0 too small".into());
        }
        let hdr = read_vec(storage.as_ref(), 0, 0x10)?;
        if &hdr[0..4] != b"PFS0" {
            return Err(format!(
                "ExeFS does not start with PFS0 magic (got {:02x?}) — wrong decryption key or corrupt NCA",
                &hdr[0..4]
            ));
        }
        let file_count = u32_le(&hdr, 4) as usize;
        let str_size = u32_le(&hdr, 8) as usize;
        let table_size = file_count * 24 + str_size;
        let data_start = 0x10 + table_size as u64;
        let table = read_vec(storage.as_ref(), 0x10, table_size)?;
        let entries_raw = &table[..file_count * 24];
        let strings = &table[file_count * 24..];

        let mut entries = Vec::with_capacity(file_count);
        for i in 0..file_count {
            let e = &entries_raw[i * 24..(i + 1) * 24];
            let off = u64_le(e, 0);
            let size = u64_le(e, 8);
            let so = u32_le(e, 16) as usize;
            let tail = strings.get(so..).ok_or("ExeFS string offset out of range")?;
            let end = tail.iter().position(|&b| b == 0).unwrap_or(tail.len());
            let name = String::from_utf8_lossy(&tail[..end]).into_owned();
            validate_member_name(&name)?;
            entries.push(ExeFsEntry { name, offset: off, size });
        }
        Ok(Self {
            storage,
            data_start,
            entries,
        })
    }

    pub fn read_file(&self, name: &str) -> Result<Option<Vec<u8>>, String> {
        let Some(e) = self.entries.iter().find(|e| e.name == name) else {
            return Ok(None);
        };
        Ok(Some(read_vec(
            self.storage.as_ref(),
            self.data_start + e.offset,
            e.size as usize,
        )?))
    }

    pub fn extract_all(&self, dst: &Path) -> Result<usize, String> {
        fs::create_dir_all(dst).map_err(|e| format!("{}: {}", dst.display(), e))?;
        for e in &self.entries {
            let out_path = dst.join(&e.name);
            let tmp_path = dst.join(format!("{}.tmp", e.name));
            let data = read_vec(self.storage.as_ref(), self.data_start + e.offset, e.size as usize)?;
            fs::write(&tmp_path, &data).map_err(|err| format!("{}: {}", tmp_path.display(), err))?;
            fs::rename(&tmp_path, &out_path).map_err(|err| format!("{}: {}", out_path.display(), err))?;
        }
        Ok(self.entries.len())
    }
}

// ---------------------------------------------------------------------------------------------- RomFS

#[derive(Debug, Clone)]
pub struct RomFsFile {
    pub rel_path: String,
    pub offset: u64,
    pub size: u64,
}

pub struct RomFs {
    storage: Arc<dyn Storage>,
    file_data_offset: u64,
    pub files: Vec<RomFsFile>,
}

fn validate_member_name(name: &str) -> Result<(), String> {
    if name.is_empty() || name == "." || name == ".." || name.contains('/') || name.contains('\\') || name.contains('\0') {
        return Err(format!("unsafe archive member name {:?}", name));
    }
    Ok(())
}

impl RomFs {
    pub fn open(storage: Arc<dyn Storage>) -> Result<Self, String> {
        if storage.size() < 0x50 {
            return Err("RomFS storage too small".into());
        }
        let hdr = read_vec(storage.as_ref(), 0, 0x50)?;
        let header_size = u64_le(&hdr, 0);
        if header_size != 0x50 {
            return Err(format!(
                "invalid RomFS header size 0x{:x} (expected 0x50) — wrong decryption key, mismatched base/update, or corrupt NCA",
                header_size
            ));
        }
        let dir_meta_off = u64_le(&hdr, 0x18);
        let dir_meta_size = u64_le(&hdr, 0x20);
        let file_meta_off = u64_le(&hdr, 0x38);
        let file_meta_size = u64_le(&hdr, 0x40);
        let file_data_offset = u64_le(&hdr, 0x48);

        let dir_meta = read_vec(storage.as_ref(), dir_meta_off, dir_meta_size as usize)?;
        let file_meta = read_vec(storage.as_ref(), file_meta_off, file_meta_size as usize)?;

        let mut files = Vec::new();
        if !dir_meta.is_empty() {
            walk_romfs_dir(&dir_meta, &file_meta, 0, "", &mut files, 0)?;
        }
        Ok(Self {
            storage,
            file_data_offset,
            files,
        })
    }

    /// Compute the exact same RomFS manifest tuple as `setup::romfs_manifest`:
    /// `((file_count, total_bytes, sha256_of_sorted_lines), path -> size)`
    pub fn manifest(&self) -> ((usize, u64, String), HashMap<String, u64>) {
        let sizes: HashMap<String, u64> = self.files.iter().map(|f| (f.rel_path.clone(), f.size)).collect();
        let mut lines: Vec<Vec<u8>> = sizes.iter().map(|(r, s)| format!("{}\t{}\n", r, s).into_bytes()).collect();
        lines.sort();
        let mut h = Sha256::new();
        for l in &lines {
            h.update(l);
        }
        ((sizes.len(), sizes.values().sum(), crate::util::hex(&h.finalize())), sizes)
    }

    pub fn find(&self, rel_path: &str) -> Option<&RomFsFile> {
        self.files.iter().find(|f| f.rel_path == rel_path)
    }

    /// Compute SHA-256 of a single RomFS file by streaming in 1 MiB chunks.
    pub fn sha256_file(&self, file: &RomFsFile) -> Result<String, String> {
        let mut h = Sha256::new();
        let mut buf = vec![0u8; 1024 * 1024];
        let mut rem = file.size;
        let mut off = self.file_data_offset + file.offset;
        while rem > 0 {
            let take = (rem as usize).min(buf.len());
            self.storage.read_at(off, &mut buf[..take])?;
            h.update(&buf[..take]);
            off += take as u64;
            rem -= take as u64;
        }
        Ok(crate::util::hex(&h.finalize()))
    }

    /// Stream every file in RomFS to `dst` (atomic `.tmp` + rename; skips existing files with exact matching size).
    /// Calls `on_progress(done_files, total_files, done_bytes, total_bytes)` after each file.
    pub fn extract_all(
        &self,
        dst: &Path,
        on_progress: &mut dyn FnMut(usize, usize, u64, u64),
    ) -> Result<(usize, usize), String> {
        fs::create_dir_all(dst).map_err(|e| format!("{}: {}", dst.display(), e))?;
        let total_files = self.files.len();
        let total_bytes: u64 = self.files.iter().map(|f| f.size).sum();
        let mut done_bytes = 0u64;
        let mut extracted = 0usize;
        let mut buf = vec![0u8; 1024 * 1024];

        for (i, f) in self.files.iter().enumerate() {
            let out_path = dst.join(&f.rel_path);
            if fs::metadata(&out_path).map(|m| m.is_file() && m.len() == f.size).unwrap_or(false) {
                done_bytes += f.size;
                on_progress(i + 1, total_files, done_bytes, total_bytes);
                continue;
            }
            if let Some(parent) = out_path.parent() {
                fs::create_dir_all(parent).map_err(|e| format!("{}: {}", parent.display(), e))?;
            }
            let tmp_path = out_path.with_file_name(format!(
                "{}.tmp",
                out_path.file_name().unwrap().to_string_lossy()
            ));
            {
                let mut out = File::create(&tmp_path).map_err(|e| format!("{}: {}", tmp_path.display(), e))?;
                let mut rem = f.size;
                let mut off = self.file_data_offset + f.offset;
                while rem > 0 {
                    let take = (rem as usize).min(buf.len());
                    self.storage.read_at(off, &mut buf[..take])?;
                    out.write_all(&buf[..take])
                        .map_err(|e| format!("{}: {}", tmp_path.display(), e))?;
                    off += take as u64;
                    rem -= take as u64;
                }
            }
            fs::rename(&tmp_path, &out_path).map_err(|e| format!("{}: {}", out_path.display(), e))?;
            extracted += 1;
            done_bytes += f.size;
            on_progress(i + 1, total_files, done_bytes, total_bytes);
        }
        Ok((total_files, extracted))
    }
}

fn walk_romfs_dir(
    dir_meta: &[u8],
    file_meta: &[u8],
    dir_off: u32,
    prefix: &str,
    out: &mut Vec<RomFsFile>,
    depth: usize,
) -> Result<(), String> {
    if depth > 64 {
        return Err("RomFS directory nesting too deep".into());
    }
    let d = dir_off as usize;
    if d + 0x18 > dir_meta.len() {
        return Err(format!("RomFS directory entry offset 0x{:x} out of range", d));
    }
    let first_child_dir = u32_le(dir_meta, d + 0x08);
    let first_child_file = u32_le(dir_meta, d + 0x0C);

    // Visit files in this directory
    let mut f_off = first_child_file;
    while f_off != ROMFS_ENTRY_EMPTY {
        let fo = f_off as usize;
        if fo + 0x20 > file_meta.len() {
            return Err(format!("RomFS file entry offset 0x{:x} out of range", fo));
        }
        let next_sibling = u32_le(file_meta, fo + 0x04);
        let data_off = u64_le(file_meta, fo + 0x08);
        let data_size = u64_le(file_meta, fo + 0x10);
        let name_len = u32_le(file_meta, fo + 0x1C) as usize;
        if fo + 0x20 + name_len > file_meta.len() {
            return Err("RomFS file name extends past metadata table".into());
        }
        let name = String::from_utf8_lossy(&file_meta[fo + 0x20..fo + 0x20 + name_len]).into_owned();
        validate_member_name(&name)?;
        let rel = if prefix.is_empty() {
            name
        } else {
            format!("{}/{}", prefix, name)
        };
        out.push(RomFsFile {
            rel_path: rel,
            offset: data_off,
            size: data_size,
        });
        f_off = next_sibling;
    }

    // Visit child directories
    let mut c_off = first_child_dir;
    while c_off != ROMFS_ENTRY_EMPTY {
        let co = c_off as usize;
        if co + 0x18 > dir_meta.len() {
            return Err(format!("RomFS child directory offset 0x{:x} out of range", co));
        }
        let next_sibling = u32_le(dir_meta, co + 0x04);
        let name_len = u32_le(dir_meta, co + 0x14) as usize;
        if co + 0x18 + name_len > dir_meta.len() {
            return Err("RomFS directory name extends past metadata table".into());
        }
        let name = String::from_utf8_lossy(&dir_meta[co + 0x18..co + 0x18 + name_len]).into_owned();
        validate_member_name(&name)?;
        let sub = if prefix.is_empty() {
            name
        } else {
            format!("{}/{}", prefix, name)
        };
        walk_romfs_dir(dir_meta, file_meta, c_off, &sub, out, depth + 1)?;
        c_off = next_sibling;
    }
    Ok(())
}

#[cfg(test)]
pub fn build_romfs(files: &[(&str, &[u8])]) -> Vec<u8> {
    // Simple RomFS builder for synthetic tests supporting nested directories (e.g. "movie/intro.webm")
    use std::collections::BTreeMap;

    #[derive(Default)]
    struct DirNode {
        subdirs: BTreeMap<String, DirNode>,
        files: Vec<(String, Vec<u8>)>,
    }

    let mut root = DirNode::default();
    for &(path, data) in files {
        let parts: Vec<&str> = path.split('/').collect();
        let mut cur = &mut root;
        for &d in &parts[..parts.len() - 1] {
            cur = cur.subdirs.entry(d.to_string()).or_default();
        }
        cur.files.push((parts.last().unwrap().to_string(), data.to_vec()));
    }

    let mut file_data = Vec::new();
    let mut dir_meta = Vec::new();
    let mut file_meta = Vec::new();

    fn emit_dir(
        node: &DirNode,
        name: &str,
        parent_off: u32,
        dir_meta: &mut Vec<u8>,
        file_meta: &mut Vec<u8>,
        file_data: &mut Vec<u8>,
    ) -> u32 {
        let my_off = dir_meta.len() as u32;
        dir_meta.extend_from_slice(&parent_off.to_le_bytes());
        dir_meta.extend_from_slice(&ROMFS_ENTRY_EMPTY.to_le_bytes()); // next_sibling (patched by parent)
        dir_meta.extend_from_slice(&ROMFS_ENTRY_EMPTY.to_le_bytes()); // first_child_dir
        dir_meta.extend_from_slice(&ROMFS_ENTRY_EMPTY.to_le_bytes()); // first_child_file
        dir_meta.extend_from_slice(&ROMFS_ENTRY_EMPTY.to_le_bytes()); // next_hash
        dir_meta.extend_from_slice(&(name.len() as u32).to_le_bytes());
        dir_meta.extend_from_slice(name.as_bytes());
        while dir_meta.len() % 4 != 0 {
            dir_meta.push(0);
        }

        // Emit files
        let mut prev_file: Option<u32> = None;
        for (fname, fdata) in &node.files {
            while file_data.len() % 16 != 0 {
                file_data.push(0);
            }
            let data_off = file_data.len() as u64;
            file_data.extend_from_slice(fdata);

            let fo = file_meta.len() as u32;
            if let Some(pf) = prev_file {
                file_meta[pf as usize + 4..pf as usize + 8].copy_from_slice(&fo.to_le_bytes());
            } else {
                dir_meta[my_off as usize + 0x0C..my_off as usize + 0x10].copy_from_slice(&fo.to_le_bytes());
            }
            file_meta.extend_from_slice(&my_off.to_le_bytes());
            file_meta.extend_from_slice(&ROMFS_ENTRY_EMPTY.to_le_bytes());
            file_meta.extend_from_slice(&data_off.to_le_bytes());
            file_meta.extend_from_slice(&(fdata.len() as u64).to_le_bytes());
            file_meta.extend_from_slice(&ROMFS_ENTRY_EMPTY.to_le_bytes());
            file_meta.extend_from_slice(&(fname.len() as u32).to_le_bytes());
            file_meta.extend_from_slice(fname.as_bytes());
            while file_meta.len() % 4 != 0 {
                file_meta.push(0);
            }
            prev_file = Some(fo);
        }

        // Emit subdirs
        let mut prev_dir: Option<u32> = None;
        for (dname, child) in &node.subdirs {
            let co = emit_dir(child, dname, my_off, dir_meta, file_meta, file_data);
            if let Some(pd) = prev_dir {
                dir_meta[pd as usize + 4..pd as usize + 8].copy_from_slice(&co.to_le_bytes());
            } else {
                dir_meta[my_off as usize + 0x08..my_off as usize + 0x0C].copy_from_slice(&co.to_le_bytes());
            }
            prev_dir = Some(co);
        }
        my_off
    }

    emit_dir(&root, "", 0, &mut dir_meta, &mut file_meta, &mut file_data);

    let data_offset = 0x200u64;
    let mut out = vec![0u8; data_offset as usize];
    out.extend_from_slice(&file_data);
    while out.len() % 4 != 0 {
        out.push(0);
    }
    let dir_hash_off = out.len() as u64;
    out.extend_from_slice(&[0xFF; 12]); // dummy 3-bucket hash table
    let dir_meta_off = out.len() as u64;
    out.extend_from_slice(&dir_meta);
    let file_hash_off = out.len() as u64;
    out.extend_from_slice(&[0xFF; 12]);
    let file_meta_off = out.len() as u64;
    out.extend_from_slice(&file_meta);

    out[0x00..0x08].copy_from_slice(&0x50u64.to_le_bytes());
    out[0x08..0x10].copy_from_slice(&dir_hash_off.to_le_bytes());
    out[0x10..0x18].copy_from_slice(&12u64.to_le_bytes());
    out[0x18..0x20].copy_from_slice(&dir_meta_off.to_le_bytes());
    out[0x20..0x28].copy_from_slice(&(dir_meta.len() as u64).to_le_bytes());
    out[0x28..0x30].copy_from_slice(&file_hash_off.to_le_bytes());
    out[0x30..0x38].copy_from_slice(&12u64.to_le_bytes());
    out[0x38..0x40].copy_from_slice(&file_meta_off.to_le_bytes());
    out[0x40..0x48].copy_from_slice(&(file_meta.len() as u64).to_le_bytes());
    out[0x48..0x50].copy_from_slice(&data_offset.to_le_bytes());
    out
}
