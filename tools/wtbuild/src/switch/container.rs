//! [witchtime] Streaming Switch container readers: NSP (PFS0) and XCI (HFS0).

use std::fs::File;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::Arc;

#[cfg(unix)]
use std::os::unix::fs::FileExt;
#[cfg(not(unix))]
use std::io::{Read, Seek, SeekFrom};
#[cfg(not(unix))]
use std::sync::Mutex;

/// Thread-safe, zero-copy slice of an open file.
#[derive(Clone)]
pub struct FileSlice {
    #[cfg(unix)]
    file: Arc<File>,
    #[cfg(not(unix))]
    file: Arc<Mutex<File>>,
    pub label: Arc<String>,
    pub offset: u64,
    pub len: u64,
}

impl FileSlice {
    pub fn open(path: &Path) -> Result<Self, String> {
        let f = File::open(path).map_err(|e| format!("{}: {}", path.display(), e))?;
        let len = f.metadata().map_err(|e| format!("{}: {}", path.display(), e))?.len();
        Ok(Self {
            #[cfg(unix)]
            file: Arc::new(f),
            #[cfg(not(unix))]
            file: Arc::new(Mutex::new(f)),
            label: Arc::new(path.display().to_string()),
            offset: 0,
            len,
        })
    }

    pub fn subslice(&self, rel_offset: u64, len: u64) -> Result<Self, String> {
        let end = rel_offset
            .checked_add(len)
            .ok_or_else(|| format!("{}: slice offset overflow", self.label))?;
        if end > self.len {
            return Err(format!(
                "{}: slice [0x{:x}..0x{:x}] exceeds size 0x{:x}",
                self.label, rel_offset, end, self.len
            ));
        }
        Ok(Self {
            file: self.file.clone(),
            label: self.label.clone(),
            offset: self.offset + rel_offset,
            len,
        })
    }

    pub fn read_exact_at(&self, rel_offset: u64, buf: &mut [u8]) -> Result<(), String> {
        let end = rel_offset
            .checked_add(buf.len() as u64)
            .ok_or_else(|| format!("{}: read offset overflow", self.label))?;
        if end > self.len {
            return Err(format!(
                "{}: unexpected EOF reading 0x{:x} bytes at 0x{:x} (size 0x{:x})",
                self.label,
                buf.len(),
                rel_offset,
                self.len
            ));
        }
        let abs = self.offset + rel_offset;
        #[cfg(unix)]
        {
            let mut done = 0usize;
            while done < buf.len() {
                match self.file.read_at(&mut buf[done..], abs + done as u64) {
                    Ok(0) => {
                        return Err(format!(
                            "{}: unexpected EOF at 0x{:x}",
                            self.label,
                            rel_offset + done as u64
                        ));
                    }
                    Ok(n) => done += n,
                    Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                    Err(e) => return Err(format!("{}: read error: {}", self.label, e)),
                }
            }
            Ok(())
        }
        #[cfg(not(unix))]
        {
            let mut guard = self.file.lock().unwrap();
            guard
                .seek(SeekFrom::Start(abs))
                .map_err(|e| format!("{}: seek error: {}", self.label, e))?;
            guard
                .read_exact(buf)
                .map_err(|e| format!("{}: read error: {}", self.label, e))
        }
    }

    pub fn read_vec_at(&self, rel_offset: u64, len: usize) -> Result<Vec<u8>, String> {
        let mut buf = vec![0u8; len];
        self.read_exact_at(rel_offset, &mut buf)?;
        Ok(buf)
    }
}

pub struct ContainerEntry {
    pub name: String,
    pub slice: FileSlice,
    pub source_path: PathBuf,
}

fn cstr_at(table: &[u8], off: usize) -> Result<String, String> {
    if off >= table.len() {
        return Err("string table offset out of range".into());
    }
    let tail = &table[off..];
    let end = tail.iter().position(|&b| b == 0).unwrap_or(tail.len());
    Ok(String::from_utf8_lossy(&tail[..end]).into_owned())
}

fn u32_le(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(b[off..off + 4].try_into().unwrap())
}

fn u64_le(b: &[u8], off: usize) -> u64 {
    u64::from_le_bytes(b[off..off + 8].try_into().unwrap())
}

/// Parse a PFS0 container (NSP or NCA ExeFS) from a `FileSlice`.
pub fn parse_pfs0(slice: &FileSlice, source_path: &Path) -> Result<Vec<ContainerEntry>, String> {
    if slice.len < 0x10 {
        return Err(format!("{}: too small to be a PFS0/NSP", slice.label));
    }
    let hdr = slice.read_vec_at(0, 0x10)?;
    if &hdr[0..4] != b"PFS0" {
        return Err(format!("{}: expected PFS0 magic", slice.label));
    }
    let file_count = u32_le(&hdr, 4) as usize;
    let str_size = u32_le(&hdr, 8) as usize;
    let table_size = file_count
        .checked_mul(24)
        .and_then(|v| v.checked_add(str_size))
        .ok_or_else(|| format!("{}: PFS0 header overflow", slice.label))?;
    let data_start = 0x10u64
        .checked_add(table_size as u64)
        .ok_or_else(|| format!("{}: PFS0 data offset overflow", slice.label))?;
    if data_start > slice.len {
        return Err(format!("{}: truncated PFS0 header", slice.label));
    }
    let table = slice.read_vec_at(0x10, table_size)?;
    let entries_raw = &table[..file_count * 24];
    let strings = &table[file_count * 24..];

    let mut out = Vec::with_capacity(file_count);
    for i in 0..file_count {
        let e = &entries_raw[i * 24..(i + 1) * 24];
        let off = u64_le(e, 0);
        let size = u64_le(e, 8);
        let str_off = u32_le(e, 16) as usize;
        let name = cstr_at(strings, str_off)?;
        let sub = slice.subslice(data_start + off, size)?;
        out.push(ContainerEntry {
            name,
            slice: sub,
            source_path: source_path.to_path_buf(),
        });
    }
    Ok(out)
}

/// Parse an HFS0 partition (used inside XCI).
pub fn parse_hfs0(slice: &FileSlice, source_path: &Path) -> Result<Vec<ContainerEntry>, String> {
    if slice.len < 0x10 {
        return Err(format!("{}: too small to be an HFS0 partition", slice.label));
    }
    let hdr = slice.read_vec_at(0, 0x10)?;
    if &hdr[0..4] != b"HFS0" {
        return Err(format!("{}: expected HFS0 magic", slice.label));
    }
    let file_count = u32_le(&hdr, 4) as usize;
    let str_size = u32_le(&hdr, 8) as usize;
    let table_size = file_count
        .checked_mul(0x40)
        .and_then(|v| v.checked_add(str_size))
        .ok_or_else(|| format!("{}: HFS0 header overflow", slice.label))?;
    let data_start = 0x10u64
        .checked_add(table_size as u64)
        .ok_or_else(|| format!("{}: HFS0 data offset overflow", slice.label))?;
    if data_start > slice.len {
        return Err(format!("{}: truncated HFS0 header", slice.label));
    }
    let table = slice.read_vec_at(0x10, table_size)?;
    let entries_raw = &table[..file_count * 0x40];
    let strings = &table[file_count * 0x40..];

    let mut out = Vec::with_capacity(file_count);
    for i in 0..file_count {
        let e = &entries_raw[i * 0x40..(i + 1) * 0x40];
        let off = u64_le(e, 0);
        let size = u64_le(e, 8);
        let str_off = u32_le(e, 16) as usize;
        let name = cstr_at(strings, str_off)?;
        let sub = slice.subslice(data_start + off, size)?;
        out.push(ContainerEntry {
            name,
            slice: sub,
            source_path: source_path.to_path_buf(),
        });
    }
    Ok(out)
}

/// Parse an XCI gamecard image (supports both standard `HEAD` at 0x100 and card-key-area `HEAD` at 0x1100).
pub fn parse_xci(slice: &FileSlice, source_path: &Path) -> Result<Vec<ContainerEntry>, String> {
    if slice.len < 0x200 {
        return Err(format!("{}: too small to be an XCI", slice.label));
    }
    let head_base: u64 = if slice.read_vec_at(0x100, 4)? == b"HEAD" {
        0
    } else if slice.len >= 0x1200 && slice.read_vec_at(0x1100, 4)? == b"HEAD" {
        0x1000
    } else {
        return Err(format!("{}: expected XCI HEAD magic at 0x100 or 0x1100", slice.label));
    };

    let xci_hdr = slice.read_vec_at(head_base + 0x100, 0x100)?;
    let hfs0_offset = head_base + u64_le(&xci_hdr, 0x30);
    if hfs0_offset >= slice.len {
        return Err(format!("{}: XCI root HFS0 offset 0x{:x} out of range", slice.label, hfs0_offset));
    }
    let root_slice = slice.subslice(hfs0_offset, slice.len - hfs0_offset)?;
    let partitions = parse_hfs0(&root_slice, source_path)?;

    let mut entries = Vec::new();
    let mut found_secure = false;
    for part in partitions {
        let lower = part.name.to_ascii_lowercase();
        if lower == "secure" || lower == "normal" {
            if lower == "secure" {
                found_secure = true;
            }
            if part.slice.len >= 0x10 && part.slice.read_vec_at(0, 4).ok().as_deref() == Some(b"HFS0") {
                entries.extend(parse_hfs0(&part.slice, source_path)?);
            }
        }
    }
    if !found_secure {
        return Err(format!("{}: XCI has no `secure` partition", slice.label));
    }
    Ok(entries)
}

/// Open an NSP or XCI container by sniffing its magic bytes.
pub fn open_package(path: &Path) -> Result<Vec<ContainerEntry>, String> {
    let slice = FileSlice::open(path)?;
    if slice.len >= 4 && slice.read_vec_at(0, 4)? == b"PFS0" {
        return parse_pfs0(&slice, path);
    }
    if (slice.len >= 0x104 && slice.read_vec_at(0x100, 4)? == b"HEAD")
        || (slice.len >= 0x1104 && slice.read_vec_at(0x1100, 4)? == b"HEAD")
    {
        return parse_xci(&slice, path);
    }
    Err(format!(
        "{}: not a valid NSP (PFS0) or XCI (HEAD) file",
        path.display()
    ))
}

#[cfg(test)]
pub fn build_pfs0(files: &[(&str, &[u8])]) -> Vec<u8> {
    let mut str_table = Vec::new();
    let mut str_offs = Vec::with_capacity(files.len());
    for (name, _) in files {
        str_offs.push(str_table.len() as u32);
        str_table.extend_from_slice(name.as_bytes());
        str_table.push(0);
    }
    while str_table.len() % 16 != 0 {
        str_table.push(0);
    }
    let mut out = Vec::new();
    out.extend_from_slice(b"PFS0");
    out.extend_from_slice(&(files.len() as u32).to_le_bytes());
    out.extend_from_slice(&(str_table.len() as u32).to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    let mut data_off = 0u64;
    for (i, (_, data)) in files.iter().enumerate() {
        out.extend_from_slice(&data_off.to_le_bytes());
        out.extend_from_slice(&(data.len() as u64).to_le_bytes());
        out.extend_from_slice(&str_offs[i].to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes());
        data_off += data.len() as u64;
    }
    out.extend_from_slice(&str_table);
    for (_, data) in files {
        out.extend_from_slice(data);
    }
    out
}

#[cfg(test)]
pub fn build_hfs0(files: &[(&str, &[u8])]) -> Vec<u8> {
    let mut str_table = Vec::new();
    let mut str_offs = Vec::with_capacity(files.len());
    for (name, _) in files {
        str_offs.push(str_table.len() as u32);
        str_table.extend_from_slice(name.as_bytes());
        str_table.push(0);
    }
    while str_table.len() % 16 != 0 {
        str_table.push(0);
    }
    let mut out = Vec::new();
    out.extend_from_slice(b"HFS0");
    out.extend_from_slice(&(files.len() as u32).to_le_bytes());
    out.extend_from_slice(&(str_table.len() as u32).to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    let mut data_off = 0u64;
    for (i, (_, data)) in files.iter().enumerate() {
        out.extend_from_slice(&data_off.to_le_bytes());
        out.extend_from_slice(&(data.len() as u64).to_le_bytes());
        out.extend_from_slice(&str_offs[i].to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes()); // hashed_size
        out.extend_from_slice(&0u64.to_le_bytes()); // reserved
        out.extend_from_slice(&[0u8; 32]); // sha256
        data_off += data.len() as u64;
    }
    out.extend_from_slice(&str_table);
    for (_, data) in files {
        out.extend_from_slice(data);
    }
    out
}

#[cfg(test)]
pub fn build_xci(secure_files: &[(&str, &[u8])], with_key_area: bool) -> Vec<u8> {
    let secure_hfs0 = build_hfs0(secure_files);
    let root_hfs0 = build_hfs0(&[("secure", &secure_hfs0)]);
    let prefix = if with_key_area { 0x1000 } else { 0 };
    let hfs0_rel = 0xF000usize;
    let mut out = vec![0u8; prefix + hfs0_rel];
    out[prefix + 0x100..prefix + 0x104].copy_from_slice(b"HEAD");
    out[prefix + 0x130..prefix + 0x138].copy_from_slice(&(hfs0_rel as u64).to_le_bytes());
    out[prefix + 0x138..prefix + 0x140].copy_from_slice(&(root_hfs0.len() as u64).to_le_bytes());
    out.extend_from_slice(&root_hfs0);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pfs0_and_xci_round_trip() {
        let dir = std::env::temp_dir().join(format!("wtbuild_cont_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();

        let nsp_path = dir.join("test.nsp");
        std::fs::write(&nsp_path, build_pfs0(&[("a.nca", b"hello"), ("b.tik", b"world!")])).unwrap();
        let e = open_package(&nsp_path).unwrap();
        assert_eq!(e.len(), 2);
        assert_eq!(e[0].name, "a.nca");
        assert_eq!(e[0].slice.read_vec_at(0, 5).unwrap(), b"hello");
        assert_eq!(e[1].name, "b.tik");
        assert_eq!(e[1].slice.read_vec_at(0, 6).unwrap(), b"world!");

        for key_area in [false, true] {
            let xci_path = dir.join(if key_area { "ka.xci" } else { "std.xci" });
            std::fs::write(&xci_path, build_xci(&[("game.nca", b"12345678")], key_area)).unwrap();
            let e = open_package(&xci_path).unwrap();
            assert_eq!(e.len(), 1);
            assert_eq!(e[0].name, "game.nca");
            assert_eq!(e[0].slice.read_vec_at(0, 8).unwrap(), b"12345678");
        }

        std::fs::remove_dir_all(&dir).unwrap();
    }
}
