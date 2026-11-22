//! [witchtime] AES-128 primitives for Nintendo Switch packages (ECB, Switch-tweak XTS, CTR).
//! Uses the `aes` crate for hardware AES-NI / ARMv8 acceleration. No Nintendo keys are stored anywhere.

use aes::cipher::{BlockCipherDecrypt, BlockCipherEncrypt, KeyInit};
use aes::{Aes128, Block};

#[inline]
pub fn aes128_ecb_decrypt_block(key: &[u8; 16], input: &[u8; 16]) -> [u8; 16] {
    let cipher = Aes128::new(key.into());
    let mut b = Block::from(*input);
    cipher.decrypt_block(&mut b);
    b.into()
}

#[cfg(test)]
#[inline]
pub fn aes128_ecb_encrypt_block(key: &[u8; 16], input: &[u8; 16]) -> [u8; 16] {
    let cipher = Aes128::new(key.into());
    let mut b = Block::from(*input);
    cipher.encrypt_block(&mut b);
    b.into()
}

pub fn aes128_ecb_decrypt(key: &[u8; 16], data: &mut [u8]) {
    let cipher = Aes128::new(key.into());
    for chunk in data.chunks_exact_mut(16) {
        let mut b = Block::default();
        b.copy_from_slice(chunk);
        cipher.decrypt_block(&mut b);
        chunk.copy_from_slice(&b);
    }
}

#[cfg(test)]
pub fn aes128_ecb_encrypt(key: &[u8; 16], data: &mut [u8]) {
    let cipher = Aes128::new(key.into());
    for chunk in data.chunks_exact_mut(16) {
        let mut b = Block::default();
        b.copy_from_slice(chunk);
        cipher.encrypt_block(&mut b);
        chunk.copy_from_slice(&b);
    }
}

/// Nintendo's 2- or 3-step AES-128-ECB key derivation (GenerateAesKek / GenerateAesKey).
pub fn generate_aes_kek(src: &[u8; 16], master_key: &[u8; 16], kek_seed: &[u8; 16], key_seed: Option<&[u8; 16]>) -> [u8; 16] {
    let kek = aes128_ecb_decrypt_block(master_key, kek_seed);
    let src_kek = aes128_ecb_decrypt_block(&kek, src);
    match key_seed {
        Some(ks) => aes128_ecb_decrypt_block(&src_kek, ks),
        None => src_kek,
    }
}

/// Multiply a 128-bit XTS tweak by `x` in GF(2^128) (polynomial x^128 + x^7 + x^2 + x + 1).
#[inline]
fn gf128_mul_x(tweak: &mut [u8; 16]) {
    let mut carry = 0u8;
    for b in tweak.iter_mut() {
        let next = *b >> 7;
        *b = (*b << 1) | carry;
        carry = next;
    }
    if carry != 0 {
        tweak[0] ^= 0x87;
    }
}

/// Nintendo Switch AES-128-XTS sector tweak: 64-bit sector index in big-endian in the upper 8 bytes (`tweak[8..16]`).
#[inline]
fn nintendo_xts_tweak(k2: &Aes128, sector: u64) -> [u8; 16] {
    let mut raw = [0u8; 16];
    raw[8..16].copy_from_slice(&sector.to_be_bytes());
    let mut b = Block::from(raw);
    k2.encrypt_block(&mut b);
    b.into()
}

/// Decrypt `data` in place using Nintendo Switch AES-128-XTS (`key[0..16]` = data key, `key[16..32]` = tweak key).
pub fn aes128_xts_decrypt(key: &[u8; 32], first_sector: u64, sector_size: usize, data: &mut [u8]) {
    debug_assert!(sector_size >= 16 && sector_size % 16 == 0);
    debug_assert!(data.len() % 16 == 0);
    let k1 = Aes128::new((&key[0..16]).try_into().unwrap());
    let k2 = Aes128::new((&key[16..32]).try_into().unwrap());
    for (s_idx, sec) in data.chunks_mut(sector_size).enumerate() {
        let mut tweak = nintendo_xts_tweak(&k2, first_sector + s_idx as u64);
        for chunk in sec.chunks_exact_mut(16) {
            let mut b = [0u8; 16];
            for i in 0..16 {
                b[i] = chunk[i] ^ tweak[i];
            }
            let mut blk = Block::from(b);
            k1.decrypt_block(&mut blk);
            let out: [u8; 16] = blk.into();
            for i in 0..16 {
                chunk[i] = out[i] ^ tweak[i];
            }
            gf128_mul_x(&mut tweak);
        }
    }
}

#[cfg(test)]
pub fn aes128_xts_encrypt(key: &[u8; 32], first_sector: u64, sector_size: usize, data: &mut [u8]) {
    debug_assert!(sector_size >= 16 && sector_size % 16 == 0);
    debug_assert!(data.len() % 16 == 0);
    let k1 = Aes128::new((&key[0..16]).try_into().unwrap());
    let k2 = Aes128::new((&key[16..32]).try_into().unwrap());
    for (s_idx, sec) in data.chunks_mut(sector_size).enumerate() {
        let mut tweak = nintendo_xts_tweak(&k2, first_sector + s_idx as u64);
        for chunk in sec.chunks_exact_mut(16) {
            let mut b = [0u8; 16];
            for i in 0..16 {
                b[i] = chunk[i] ^ tweak[i];
            }
            let mut blk = Block::from(b);
            k1.encrypt_block(&mut blk);
            let out: [u8; 16] = blk.into();
            for i in 0..16 {
                chunk[i] = out[i] ^ tweak[i];
            }
            gf128_mul_x(&mut tweak);
        }
    }
}

/// Apply Switch NCA AES-128-CTR to `data` starting at byte `offset` (CTR block index = `offset >> 4`).
/// `upper_ctr` is bytes `0..8` of the 16-byte IV (big-endian `secure_value` then big-endian `generation`/`ctr_val`).
/// Processes 256 blocks (4 KiB) per `encrypt_blocks` call to saturate AES-NI / ARMv8 pipelines.
pub fn aes128_ctr_apply(key: &[u8; 16], upper_ctr: [u8; 8], offset: u64, data: &mut [u8]) {
    if data.is_empty() {
        return;
    }
    let cipher = Aes128::new(key.into());
    let mut pos = 0usize;
    let mut blk_idx = offset >> 4;
    let lead = (offset & 15) as usize;

    if lead != 0 {
        let mut iv = [0u8; 16];
        iv[0..8].copy_from_slice(&upper_ctr);
        iv[8..16].copy_from_slice(&blk_idx.to_be_bytes());
        let mut ks = Block::from(iv);
        cipher.encrypt_block(&mut ks);
        let take = (16 - lead).min(data.len());
        for i in 0..take {
            data[i] ^= ks[lead + i];
        }
        pos += take;
        blk_idx += 1;
    }

    const BATCH: usize = 256;
    let mut ks_blocks = [Block::default(); BATCH];
    while pos < data.len() {
        let rem = data.len() - pos;
        let full_blocks = rem / 16;
        if full_blocks > 0 {
            let n = full_blocks.min(BATCH);
            for i in 0..n {
                let mut iv = [0u8; 16];
                iv[0..8].copy_from_slice(&upper_ctr);
                iv[8..16].copy_from_slice(&(blk_idx + i as u64).to_be_bytes());
                ks_blocks[i] = Block::from(iv);
            }
            cipher.encrypt_blocks(&mut ks_blocks[..n]);
            for i in 0..n {
                let dst = &mut data[pos + i * 16..pos + (i + 1) * 16];
                let ks: &[u8] = &ks_blocks[i];
                for j in 0..16 {
                    dst[j] ^= ks[j];
                }
            }
            pos += n * 16;
            blk_idx += n as u64;
        } else {
            let mut iv = [0u8; 16];
            iv[0..8].copy_from_slice(&upper_ctr);
            iv[8..16].copy_from_slice(&blk_idx.to_be_bytes());
            let mut ks = Block::from(iv);
            cipher.encrypt_block(&mut ks);
            for i in 0..rem {
                data[pos + i] ^= ks[i];
            }
            break;
        }
    }
}

/// Build the 8-byte big-endian upper CTR from Switch FsHeader `(secure_value, generation_or_ctr_val)`.
#[inline]
pub fn make_upper_ctr(secure_value: u32, generation: u32) -> [u8; 8] {
    let mut out = [0u8; 8];
    out[0..4].copy_from_slice(&secure_value.to_be_bytes());
    out[4..8].copy_from_slice(&generation.to_be_bytes());
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn xts_round_trip_multi_sector() {
        let key = [0x42u8; 32];
        let orig: Vec<u8> = (0..0xC00).map(|i| (i * 37 + 13) as u8).collect();
        let mut buf = orig.clone();
        aes128_xts_encrypt(&key, 0, 0x200, &mut buf);
        assert_ne!(buf, orig);
        aes128_xts_decrypt(&key, 0, 0x200, &mut buf);
        assert_eq!(buf, orig);
        // Decrypting a single sector starting at sector 3 must match too
        let mut enc = orig.clone();
        aes128_xts_encrypt(&key, 0, 0x200, &mut enc);
        aes128_xts_decrypt(&key, 3, 0x200, &mut enc[0x600..0x800]);
        assert_eq!(&enc[0x600..0x800], &orig[0x600..0x800]);
    }

    #[test]
    fn ctr_round_trip_unaligned_and_batched() {
        let key = [0x11u8; 16];
        let upper = make_upper_ctr(0x12345678, 0x00000003);
        let orig: Vec<u8> = (0..10000).map(|i| (i * 19 + 7) as u8).collect();
        let mut enc = orig.clone();
        aes128_ctr_apply(&key, upper, 0x100, &mut enc);
        assert_ne!(enc, orig);
        // Decrypt in odd-sized unaligned chunks
        let mut dec = enc.clone();
        let mut pos = 0usize;
        let sizes = [7, 1, 15, 33, 4099, 13, 5000, 10000];
        for s in sizes {
            if pos >= dec.len() {
                break;
            }
            let take = s.min(dec.len() - pos);
            aes128_ctr_apply(&key, upper, 0x100 + pos as u64, &mut dec[pos..pos + take]);
            pos += take;
        }
        assert_eq!(dec, orig);
    }
}
