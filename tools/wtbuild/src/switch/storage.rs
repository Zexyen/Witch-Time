//! [witchtime] Layered Nintendo Switch NCA storage stack:
//! SliceStorage, SubStorage, AesCtrStorage, AesXtsStorage, SparseStorage,
//! AesCtrExStorage (Subsection BKTR), IndirectStorage (Relocation BKTR),
//! and CompressedStorage (LZ4 Compression BKTR).

use super::container::FileSlice;
use super::crypto::{aes128_ctr_apply, aes128_xts_decrypt, make_upper_ctr};
use std::sync::{Arc, Mutex};

pub const BKTR_NODE_SIZE: usize = 0x4000;

pub trait Storage: Send + Sync {
    fn size(&self) -> u64;
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), String>;
}

pub fn read_vec(s: &dyn Storage, offset: u64, len: usize) -> Result<Vec<u8>, String> {
    let mut buf = vec![0u8; len];
    s.read_at(offset, &mut buf)?;
    Ok(buf)
}

pub struct SliceStorage {
    slice: FileSlice,
}

impl SliceStorage {
    pub fn new(slice: FileSlice) -> Self {
        Self { slice }
    }
}

impl Storage for SliceStorage {
    fn size(&self) -> u64 {
        self.slice.len
    }

    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), String> {
        self.slice.read_exact_at(offset, buf)
    }
}

pub struct SubStorage {
    inner: Arc<dyn Storage>,
    offset: u64,
    len: u64,
}

impl SubStorage {
    pub fn new(inner: Arc<dyn Storage>, offset: u64, len: u64) -> Result<Self, String> {
        let end = offset.checked_add(len).ok_or("SubStorage offset overflow")?;
        if end > inner.size() {
            return Err(format!(
                "SubStorage [0x{:x}..0x{:x}] exceeds backing size 0x{:x}",
                offset,
                end,
                inner.size()
            ));
        }
        Ok(Self { inner, offset, len })
    }
}

impl Storage for SubStorage {
    fn size(&self) -> u64 {
        self.len
    }

    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), String> {
        let end = offset.checked_add(buf.len() as u64).ok_or("SubStorage read overflow")?;
        if end > self.len {
            return Err(format!(
                "SubStorage read [0x{:x}..0x{:x}] exceeds size 0x{:x}",
                offset, end, self.len
            ));
        }
        self.inner.read_at(self.offset + offset, buf)
    }
}

/// AES-128-CTR decrypted view over a backing storage.
/// `ctr_base_offset` is the absolute NCA byte offset corresponding to `offset == 0` of this storage.
pub struct AesCtrStorage {
    inner: Arc<dyn Storage>,
    key: [u8; 16],
    upper_ctr: [u8; 8],
    ctr_base_offset: u64,
}

impl AesCtrStorage {
    pub fn new(inner: Arc<dyn Storage>, key: [u8; 16], upper_ctr: [u8; 8], ctr_base_offset: u64) -> Self {
        Self {
            inner,
            key,
            upper_ctr,
            ctr_base_offset,
        }
    }
}

impl Storage for AesCtrStorage {
    fn size(&self) -> u64 {
        self.inner.size()
    }

    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), String> {
        self.inner.read_at(offset, buf)?;
        aes128_ctr_apply(&self.key, self.upper_ctr, self.ctr_base_offset + offset, buf);
        Ok(())
    }
}

/// AES-128-XTS decrypted view (sector-aligned reads handled transparently).
pub struct AesXtsStorage {
    inner: Arc<dyn Storage>,
    key: [u8; 32],
    sector_size: usize,
}

impl AesXtsStorage {
    pub fn new(inner: Arc<dyn Storage>, key: [u8; 32], sector_size: usize) -> Self {
        Self { inner, key, sector_size }
    }
}

impl Storage for AesXtsStorage {
    fn size(&self) -> u64 {
        self.inner.size()
    }

    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), String> {
        if buf.is_empty() {
            return Ok(());
        }
        let ss = self.sector_size as u64;
        let first_sec = offset / ss;
        let last_sec = (offset + buf.len() as u64 - 1) / ss;
        let aligned_start = first_sec * ss;
        let aligned_len = ((last_sec - first_sec + 1) * ss) as usize;
        let mut tmp = vec![0u8; aligned_len];
        self.inner.read_at(aligned_start, &mut tmp)?;
        aes128_xts_decrypt(&self.key, first_sec, self.sector_size, &mut tmp);
        let skip = (offset - aligned_start) as usize;
        buf.copy_from_slice(&tmp[skip..skip + buf.len()]);
        Ok(())
    }
}

// ---------------------------------------------------------------------------------------------- BKTR tables

fn u32_le(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(b[off..off + 4].try_into().unwrap())
}

fn u64_le(b: &[u8], off: usize) -> u64 {
    u64::from_le_bytes(b[off..off + 8].try_into().unwrap())
}

#[derive(Debug, Clone, Copy)]
pub struct IndirectEntry {
    pub virt_offset: u64,
    pub phys_offset: u64,
    pub storage_index: i32,
}

#[derive(Debug, Clone, Copy)]
pub struct SubsectionEntry {
    pub offset: u64,
    pub ctr_val: u32,
}

#[derive(Debug, Clone, Copy)]
pub struct CompressionEntry {
    pub virt_offset: u64,
    pub phys_offset: u64,
    pub comp_type: u8,
    pub phys_size: u32,
}

/// Flatten a Switch 16 KiB-node BKTR bucket tree into `(end_offset, entries)`.
fn parse_bktr<T>(
    table: &[u8],
    entry_size: usize,
    parse_entry: impl Fn(&[u8]) -> T,
    entry_offset: impl Fn(&T) -> u64,
) -> Result<(u64, Vec<T>), String> {
    if table.len() < BKTR_NODE_SIZE {
        return Err(format!("BKTR table too small ({} bytes)", table.len()));
    }
    let num_buckets = u32_le(table, 4) as usize;
    let end_offset = u64_le(table, 8);
    let need = (1 + num_buckets)
        .checked_mul(BKTR_NODE_SIZE)
        .ok_or("BKTR bucket count overflow")?;
    if table.len() < need {
        return Err(format!(
            "BKTR table truncated: have {} bytes, need {} for {} bucket(s)",
            table.len(),
            need,
            num_buckets
        ));
    }
    let mut entries = Vec::new();
    for b in 0..num_buckets {
        let node = &table[(1 + b) * BKTR_NODE_SIZE..(2 + b) * BKTR_NODE_SIZE];
        let count = u32_le(node, 4) as usize;
        let max_in_node = (BKTR_NODE_SIZE - 0x10) / entry_size;
        if count > max_in_node {
            return Err(format!(
                "BKTR bucket {} has {} entries (max {})",
                b, count, max_in_node
            ));
        }
        for i in 0..count {
            let off = 0x10 + i * entry_size;
            let e = parse_entry(&node[off..off + entry_size]);
            if entry_offset(&e) < end_offset {
                entries.push(e);
            }
        }
    }
    Ok((end_offset, entries))
}

pub fn parse_indirect_bktr(table: &[u8]) -> Result<(u64, Vec<IndirectEntry>), String> {
    parse_bktr(
        table,
        0x14,
        |b| IndirectEntry {
            virt_offset: u64_le(b, 0),
            phys_offset: u64_le(b, 8),
            storage_index: i32::from_le_bytes(b[16..20].try_into().unwrap()),
        },
        |e| e.virt_offset,
    )
}

pub fn parse_subsection_bktr(table: &[u8]) -> Result<(u64, Vec<SubsectionEntry>), String> {
    parse_bktr(
        table,
        0x10,
        |b| SubsectionEntry {
            offset: u64_le(b, 0),
            ctr_val: u32_le(b, 12),
        },
        |e| e.offset,
    )
}

pub fn parse_compression_bktr(table: &[u8]) -> Result<(u64, Vec<CompressionEntry>), String> {
    parse_bktr(
        table,
        0x18,
        |b| CompressionEntry {
            virt_offset: u64_le(b, 0),
            phys_offset: u64_le(b, 8),
            comp_type: b[16],
            phys_size: u32_le(b, 20),
        },
        |e| e.virt_offset,
    )
}

// ---------------------------------------------------------------------------------------------- SparseStorage

/// Sparse NCA section storage (e.g. Bayonetta 3 eShop base NCA):
/// Maps virtual section offsets through the Sparse BKTR table.
/// - `storage_index == 0`: physical bytes at `nca[physical_offset + phys_offset]`, decrypted with AES-CTR
///   using the **virtual** NCA offset `(section_virtual_start + virt_offset)` when encrypted.
/// - `storage_index != 0`: zero-filled unallocated gap.
pub struct SparseStorage {
    nca: FileSlice,
    physical_offset: u64,
    section_virtual_start: u64,
    end_offset: u64,
    entries: Vec<IndirectEntry>,
    crypto: Option<([u8; 16], [u8; 8])>,
}

impl SparseStorage {
    pub fn new(
        nca: FileSlice,
        physical_offset: u64,
        section_virtual_start: u64,
        end_offset: u64,
        entries: Vec<IndirectEntry>,
        crypto: Option<([u8; 16], [u8; 8])>,
    ) -> Self {
        Self {
            nca,
            physical_offset,
            section_virtual_start,
            end_offset,
            entries,
            crypto,
        }
    }
}

impl Storage for SparseStorage {
    fn size(&self) -> u64 {
        self.end_offset
    }

    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), String> {
        if buf.is_empty() {
            return Ok(());
        }
        let end = offset.checked_add(buf.len() as u64).ok_or("SparseStorage read overflow")?;
        if end > self.end_offset {
            return Err(format!(
                "SparseStorage read [0x{:x}..0x{:x}] exceeds virtual size 0x{:x}",
                offset, end, self.end_offset
            ));
        }
        if self.entries.is_empty() {
            buf.fill(0);
            return Ok(());
        }
        let mut cur = offset;
        let mut pos = 0usize;
        while pos < buf.len() {
            let idx = self.entries.partition_point(|e| e.virt_offset <= cur);
            if idx == 0 {
                // Before first entry: zero fill up to first entry
                let next_v = self.entries[0].virt_offset;
                let take = ((next_v - cur) as usize).min(buf.len() - pos);
                buf[pos..pos + take].fill(0);
                cur += take as u64;
                pos += take;
                continue;
            }
            let e = self.entries[idx - 1];
            let next_v = self.entries.get(idx).map(|n| n.virt_offset).unwrap_or(self.end_offset);
            let avail = next_v.saturating_sub(cur);
            if avail == 0 {
                return Err(format!("SparseStorage: zero-length entry at 0x{:x}", cur));
            }
            let take = (avail as usize).min(buf.len() - pos);
            let dst = &mut buf[pos..pos + take];
            if e.storage_index == 0 {
                let phys = self.physical_offset + e.phys_offset + (cur - e.virt_offset);
                self.nca.read_exact_at(phys, dst)?;
                if let Some((key, upper)) = &self.crypto {
                    aes128_ctr_apply(key, *upper, self.section_virtual_start + cur, dst);
                }
            } else {
                dst.fill(0);
            }
            cur += take as u64;
            pos += take;
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------------------------- AesCtrExStorage

/// Update NCA patch section decrypted via Subsection BKTR (`AesCtrEx` / `AesCtrExSkipLayerHash`).
pub struct AesCtrExStorage {
    inner: Arc<dyn Storage>,
    section_nca_offset: u64,
    key: [u8; 16],
    secure_value: u32,
    end_offset: u64,
    entries: Vec<SubsectionEntry>,
}

impl AesCtrExStorage {
    pub fn new(
        inner: Arc<dyn Storage>,
        section_nca_offset: u64,
        key: [u8; 16],
        secure_value: u32,
        end_offset: u64,
        entries: Vec<SubsectionEntry>,
    ) -> Self {
        Self {
            inner,
            section_nca_offset,
            key,
            secure_value,
            end_offset,
            entries,
        }
    }
}

impl Storage for AesCtrExStorage {
    fn size(&self) -> u64 {
        self.end_offset
    }

    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), String> {
        if buf.is_empty() {
            return Ok(());
        }
        let end = offset.checked_add(buf.len() as u64).ok_or("AesCtrEx read overflow")?;
        if end > self.end_offset {
            return Err(format!(
                "AesCtrEx read [0x{:x}..0x{:x}] exceeds size 0x{:x}",
                offset, end, self.end_offset
            ));
        }
        let mut cur = offset;
        let mut pos = 0usize;
        while pos < buf.len() {
            let idx = self.entries.partition_point(|e| e.offset <= cur);
            if idx == 0 {
                return Err(format!("AesCtrEx: no subsection covers offset 0x{:x}", cur));
            }
            let e = self.entries[idx - 1];
            let next = self.entries.get(idx).map(|n| n.offset).unwrap_or(self.end_offset);
            let avail = next.saturating_sub(cur);
            if avail == 0 {
                return Err(format!("AesCtrEx: zero-length subsection at 0x{:x}", cur));
            }
            let take = (avail as usize).min(buf.len() - pos);
            let dst = &mut buf[pos..pos + take];
            self.inner.read_at(cur, dst)?;
            let upper = make_upper_ctr(self.secure_value, e.ctr_val);
            aes128_ctr_apply(&self.key, upper, self.section_nca_offset + cur, dst);
            cur += take as u64;
            pos += take;
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------------------------- IndirectStorage

/// BKTR Relocation storage merging a Base NCA section (`storage_index == 0`) with a Patch NCA section (`storage_index == 1`).
pub struct IndirectStorage {
    base: Arc<dyn Storage>,
    patch: Arc<dyn Storage>,
    end_offset: u64,
    entries: Vec<IndirectEntry>,
}

impl IndirectStorage {
    pub fn new(
        base: Arc<dyn Storage>,
        patch: Arc<dyn Storage>,
        end_offset: u64,
        entries: Vec<IndirectEntry>,
    ) -> Self {
        Self {
            base,
            patch,
            end_offset,
            entries,
        }
    }
}

impl Storage for IndirectStorage {
    fn size(&self) -> u64 {
        self.end_offset
    }

    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), String> {
        if buf.is_empty() {
            return Ok(());
        }
        let end = offset.checked_add(buf.len() as u64).ok_or("IndirectStorage read overflow")?;
        if end > self.end_offset {
            return Err(format!(
                "IndirectStorage read [0x{:x}..0x{:x}] exceeds size 0x{:x}",
                offset, end, self.end_offset
            ));
        }
        let mut cur = offset;
        let mut pos = 0usize;
        while pos < buf.len() {
            let idx = self.entries.partition_point(|e| e.virt_offset <= cur);
            if idx == 0 {
                return Err(format!("IndirectStorage: no relocation entry covers offset 0x{:x}", cur));
            }
            let e = self.entries[idx - 1];
            let next = self.entries.get(idx).map(|n| n.virt_offset).unwrap_or(self.end_offset);
            let avail = next.saturating_sub(cur);
            if avail == 0 {
                return Err(format!("IndirectStorage: zero-length relocation entry at 0x{:x}", cur));
            }
            let take = (avail as usize).min(buf.len() - pos);
            let phys = e.phys_offset + (cur - e.virt_offset);
            let dst = &mut buf[pos..pos + take];
            if e.storage_index == 0 {
                self.base.read_at(phys, dst)?;
            } else {
                self.patch.read_at(phys, dst)?;
            }
            cur += take as u64;
            pos += take;
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------------------------- CompressedStorage

/// LZ4 CompressedStorage (Switch 12.0.0+ `CompressionInfo` BKTR).
pub struct CompressedStorage {
    inner: Arc<dyn Storage>,
    end_offset: u64,
    entries: Vec<CompressionEntry>,
    /// Single-block cache `(entry_index, decompressed_bytes)` so sequential reads decode each 64 KiB block once.
    cache: Mutex<Option<(usize, Vec<u8>)>>,
}

impl CompressedStorage {
    pub fn new(inner: Arc<dyn Storage>, end_offset: u64, entries: Vec<CompressionEntry>) -> Self {
        Self {
            inner,
            end_offset,
            entries,
            cache: Mutex::new(None),
        }
    }
}

impl Storage for CompressedStorage {
    fn size(&self) -> u64 {
        self.end_offset
    }

    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), String> {
        if buf.is_empty() {
            return Ok(());
        }
        let end = offset.checked_add(buf.len() as u64).ok_or("CompressedStorage read overflow")?;
        if end > self.end_offset {
            return Err(format!(
                "CompressedStorage read [0x{:x}..0x{:x}] exceeds size 0x{:x}",
                offset, end, self.end_offset
            ));
        }
        let mut cur = offset;
        let mut pos = 0usize;
        while pos < buf.len() {
            let idx = self.entries.partition_point(|e| e.virt_offset <= cur);
            if idx == 0 {
                return Err(format!("CompressedStorage: no entry covers offset 0x{:x}", cur));
            }
            let ei = idx - 1;
            let e = self.entries[ei];
            let next = self.entries.get(idx).map(|n| n.virt_offset).unwrap_or(self.end_offset);
            let block_virt_len = next.saturating_sub(e.virt_offset) as usize;
            let in_block = (cur - e.virt_offset) as usize;
            let take = (block_virt_len - in_block).min(buf.len() - pos);
            let dst = &mut buf[pos..pos + take];

            match e.comp_type {
                0 => {
                    // Uncompressed
                    self.inner.read_at(e.phys_offset + in_block as u64, dst)?;
                }
                1 => {
                    // Zeros
                    dst.fill(0);
                }
                3 => {
                    // LZ4 block
                    let mut guard = self.cache.lock().unwrap();
                    let need_decode = !matches!(&*guard, Some((cached_i, _)) if *cached_i == ei);
                    if need_decode {
                        let raw = read_vec(self.inner.as_ref(), e.phys_offset, e.phys_size as usize)?;
                        let dec = lz4_flex::block::decompress(&raw, block_virt_len)
                            .map_err(|err| format!("CompressedStorage LZ4 error at 0x{:x}: {}", e.virt_offset, err))?;
                        *guard = Some((ei, dec));
                    }
                    let (_, ref dec) = *guard.as_ref().unwrap();
                    dst.copy_from_slice(&dec[in_block..in_block + take]);
                }
                other => {
                    return Err(format!("CompressedStorage: unsupported compression type {}", other));
                }
            }
            cur += take as u64;
            pos += take;
        }
        Ok(())
    }
}

#[cfg(test)]
pub fn build_bktr_table(end_offset: u64, entry_size: usize, entries: &[Vec<u8>]) -> Vec<u8> {
    let per_bucket = (BKTR_NODE_SIZE - 0x10) / entry_size;
    let chunks: Vec<&[Vec<u8>]> = if entries.is_empty() {
        Vec::new()
    } else {
        entries.chunks(per_bucket).collect()
    };
    let num_buckets = chunks.len();
    let mut out = vec![0u8; (1 + num_buckets) * BKTR_NODE_SIZE];
    out[4..8].copy_from_slice(&(num_buckets as u32).to_le_bytes());
    out[8..16].copy_from_slice(&end_offset.to_le_bytes());
    for (b, chunk) in chunks.iter().enumerate() {
        let first_off = u64::from_le_bytes(chunk[0][0..8].try_into().unwrap());
        out[0x10 + b * 8..0x18 + b * 8].copy_from_slice(&first_off.to_le_bytes());
        let node_start = (1 + b) * BKTR_NODE_SIZE;
        let bucket_end = chunks
            .get(b + 1)
            .map(|nc| u64::from_le_bytes(nc[0][0..8].try_into().unwrap()))
            .unwrap_or(end_offset);
        out[node_start..node_start + 4].copy_from_slice(&(b as u32).to_le_bytes());
        out[node_start + 4..node_start + 8].copy_from_slice(&(chunk.len() as u32).to_le_bytes());
        out[node_start + 8..node_start + 16].copy_from_slice(&bucket_end.to_le_bytes());
        for (i, raw) in chunk.iter().enumerate() {
            let p = node_start + 0x10 + i * entry_size;
            out[p..p + entry_size].copy_from_slice(raw);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    struct VecStorage(Vec<u8>);
    impl Storage for VecStorage {
        fn size(&self) -> u64 {
            self.0.len() as u64
        }
        fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), String> {
            let o = offset as usize;
            buf.copy_from_slice(&self.0[o..o + buf.len()]);
            Ok(())
        }
    }

    #[test]
    fn compressed_storage_round_trip() {
        let b0 = vec![0xABu8; 4096];
        let b1 = vec![0u8; 4096];
        let b2: Vec<u8> = (0..4096).map(|i| (i * 7 + 3) as u8).collect();
        let c0 = lz4_flex::block::compress(&b0);
        let mut phys = Vec::new();
        phys.extend_from_slice(&c0);
        let p2 = phys.len() as u64;
        phys.extend_from_slice(&b2);

        let entries = vec![
            CompressionEntry { virt_offset: 0, phys_offset: 0, comp_type: 3, phys_size: c0.len() as u32 },
            CompressionEntry { virt_offset: 4096, phys_offset: 0, comp_type: 1, phys_size: 0 },
            CompressionEntry { virt_offset: 8192, phys_offset: p2, comp_type: 0, phys_size: 4096 },
        ];
        let cs = CompressedStorage::new(Arc::new(VecStorage(phys)), 12288, entries);
        let all = read_vec(&cs, 0, 12288).unwrap();
        assert_eq!(&all[0..4096], &b0[..]);
        assert_eq!(&all[4096..8192], &b1[..]);
        assert_eq!(&all[8192..12288], &b2[..]);
        // Cross-boundary read spanning all 3 blocks
        let cross = read_vec(&cs, 3000, 6000).unwrap();
        assert_eq!(&cross[..], &all[3000..9000]);
    }
}
