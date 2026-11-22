//! [witchtime] Console keyfile (`prod.keys`, `title.keys`) and ticket (`.tik`) parser.
//! This project ships NO keys of any kind: every key is read from the player's own `prod.keys`.

use super::crypto::{aes128_ecb_decrypt_block, generate_aes_kek};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

/// Parsed console keyset from the user's `prod.keys` (and optional `title.keys`).
#[derive(Default, Clone)]
pub struct KeySet {
    pub header_key: Option<[u8; 32]>,
    pub key_area_key_application: HashMap<u8, [u8; 16]>,
    pub key_area_key_ocean: HashMap<u8, [u8; 16]>,
    pub key_area_key_system: HashMap<u8, [u8; 16]>,
    pub titlekek: HashMap<u8, [u8; 16]>,
    /// Encrypted title keys from `title.keys` (keyed by 16-byte `rights_id`).
    pub encrypted_title_keys: HashMap<[u8; 16], [u8; 16]>,
}

fn parse_hex(s: &str) -> Option<Vec<u8>> {
    let s = s.trim();
    if s.len() % 2 != 0 {
        return None;
    }
    let mut out = Vec::with_capacity(s.len() / 2);
    for i in (0..s.len()).step_by(2) {
        out.push(u8::from_str_radix(&s[i..i + 2], 16).ok()?);
    }
    Some(out)
}

fn as_16(v: &[u8]) -> Option<[u8; 16]> {
    v.try_into().ok()
}

fn as_32(v: &[u8]) -> Option<[u8; 32]> {
    v.try_into().ok()
}

fn parse_key_file(text: &str) -> HashMap<String, Vec<u8>> {
    let mut map = HashMap::new();
    for line in text.lines() {
        let l = line.trim();
        if l.is_empty() || l.starts_with('#') || l.starts_with(';') {
            continue;
        }
        if let Some((k, v)) = l.split_once('=') {
            // Strip inline comments if any
            let v = v.split(['#', ';']).next().unwrap_or("").trim();
            if let Some(bytes) = parse_hex(v) {
                map.insert(k.trim().to_ascii_lowercase(), bytes);
            }
        }
    }
    map
}

impl KeySet {
    pub fn from_prod_keys_str(text: &str) -> Result<Self, String> {
        let raw = parse_key_file(text);
        let mut ks = KeySet::default();

        if let Some(hk) = raw.get("header_key").and_then(|v| as_32(v)) {
            ks.header_key = Some(hk);
        }

        let mut master_keys: HashMap<u8, [u8; 16]> = HashMap::new();
        for (k, v) in &raw {
            if let Some(hex_rev) = k.strip_prefix("master_key_") {
                if let (Ok(rev), Some(b)) = (u8::from_str_radix(hex_rev, 16), as_16(v)) {
                    master_keys.insert(rev, b);
                }
            } else if let Some(hex_rev) = k.strip_prefix("key_area_key_application_") {
                if let (Ok(rev), Some(b)) = (u8::from_str_radix(hex_rev, 16), as_16(v)) {
                    ks.key_area_key_application.insert(rev, b);
                }
            } else if let Some(hex_rev) = k.strip_prefix("key_area_key_ocean_") {
                if let (Ok(rev), Some(b)) = (u8::from_str_radix(hex_rev, 16), as_16(v)) {
                    ks.key_area_key_ocean.insert(rev, b);
                }
            } else if let Some(hex_rev) = k.strip_prefix("key_area_key_system_") {
                if let (Ok(rev), Some(b)) = (u8::from_str_radix(hex_rev, 16), as_16(v)) {
                    ks.key_area_key_system.insert(rev, b);
                }
            } else if let Some(hex_rev) = k.strip_prefix("titlekek_") {
                if let (Ok(rev), Some(b)) = (u8::from_str_radix(hex_rev, 16), as_16(v)) {
                    ks.titlekek.insert(rev, b);
                }
            }
        }

        // Derive any missing keys from master_key_XX + *_source when present in prod.keys
        let kek_seed = raw.get("aes_kek_generation_source").and_then(|v| as_16(v));
        let key_seed = raw.get("aes_key_generation_source").and_then(|v| as_16(v));

        if ks.header_key.is_none() {
            if let (Some(mk0), Some(hks), Some(hsrc), Some(ksd), Some(kys)) = (
                master_keys.get(&0),
                raw.get("header_kek_source").and_then(|v| as_16(v)),
                raw.get("header_key_source").and_then(|v| as_32(v)),
                kek_seed.as_ref(),
                key_seed.as_ref(),
            ) {
                let header_kek = generate_aes_kek(&hks, mk0, ksd, Some(kys));
                let k0 = aes128_ecb_decrypt_block(&header_kek, hsrc[0..16].try_into().unwrap());
                let k1 = aes128_ecb_decrypt_block(&header_kek, hsrc[16..32].try_into().unwrap());
                let mut hk = [0u8; 32];
                hk[0..16].copy_from_slice(&k0);
                hk[16..32].copy_from_slice(&k1);
                ks.header_key = Some(hk);
            }
        }

        if let (Some(ksd), Some(kys)) = (kek_seed.as_ref(), key_seed.as_ref()) {
            let app_src = raw.get("key_area_key_application_source").and_then(|v| as_16(v));
            let ocn_src = raw.get("key_area_key_ocean_source").and_then(|v| as_16(v));
            let sys_src = raw.get("key_area_key_system_source").and_then(|v| as_16(v));
            for (&rev, mk) in &master_keys {
                if let Some(src) = &app_src {
                    ks.key_area_key_application
                        .entry(rev)
                        .or_insert_with(|| generate_aes_kek(src, mk, ksd, Some(kys)));
                }
                if let Some(src) = &ocn_src {
                    ks.key_area_key_ocean
                        .entry(rev)
                        .or_insert_with(|| generate_aes_kek(src, mk, ksd, Some(kys)));
                }
                if let Some(src) = &sys_src {
                    ks.key_area_key_system
                        .entry(rev)
                        .or_insert_with(|| generate_aes_kek(src, mk, ksd, Some(kys)));
                }
            }
        }

        if let Some(tk_src) = raw.get("titlekek_source").and_then(|v| as_16(v)) {
            for (&rev, mk) in &master_keys {
                ks.titlekek
                    .entry(rev)
                    .or_insert_with(|| aes128_ecb_decrypt_block(mk, &tk_src));
            }
        }

        if ks.header_key.is_none() {
            return Err("prod.keys is missing `header_key` (is it a valid Switch prod.keys file?)".into());
        }
        Ok(ks)
    }

    pub fn load_prod_keys(path: &Path) -> Result<Self, String> {
        let text = fs::read_to_string(path).map_err(|e| format!("{}: {}", path.display(), e))?;
        let mut ks = Self::from_prod_keys_str(&text).map_err(|e| format!("{}: {}", path.display(), e))?;
        // Also load title.keys beside prod.keys if present
        if let Some(dir) = path.parent() {
            let tk = dir.join("title.keys");
            if tk.is_file() {
                if let Ok(t) = fs::read_to_string(&tk) {
                    ks.load_title_keys_str(&t);
                }
            }
        }
        Ok(ks)
    }

    pub fn load_title_keys_str(&mut self, text: &str) {
        for (k, v) in parse_key_file(text) {
            if let (Some(rid_vec), Some(key)) = (parse_hex(&k), as_16(&v)) {
                if let Some(rid) = as_16(&rid_vec) {
                    self.encrypted_title_keys.insert(rid, key);
                }
            }
        }
    }

    pub fn header_key(&self) -> Result<&[u8; 32], String> {
        self.header_key
            .as_ref()
            .ok_or_else(|| "prod.keys is missing `header_key`".to_string())
    }

    pub fn key_area_key(&self, kaek_index: u8, master_key_rev: u8) -> Result<&[u8; 16], String> {
        let (name, map) = match kaek_index {
            0 => ("key_area_key_application", &self.key_area_key_application),
            1 => ("key_area_key_ocean", &self.key_area_key_ocean),
            2 => ("key_area_key_system", &self.key_area_key_system),
            _ => return Err(format!("unsupported NCA key area index {}", kaek_index)),
        };
        map.get(&master_key_rev).ok_or_else(|| {
            format!(
                "prod.keys is missing `{}_{:02x}` (your prod.keys may be from an older firmware than this game/update; re-dump prod.keys from your console)",
                name, master_key_rev
            )
        })
    }

    pub fn decrypt_title_key(&self, encrypted: &[u8; 16], master_key_rev: u8) -> Result<[u8; 16], String> {
        let tkek = self.titlekek.get(&master_key_rev).ok_or_else(|| {
            format!(
                "prod.keys is missing `titlekek_{:02x}` (your prod.keys may be from an older firmware than this game/update; re-dump prod.keys from your console)",
                master_key_rev
            )
        })?;
        Ok(aes128_ecb_decrypt_block(tkek, encrypted))
    }
}

/// Parsed Switch `.tik` ticket entry.
#[derive(Debug, Clone)]
pub struct TicketInfo {
    pub rights_id: [u8; 16],
    pub encrypted_title_key: [u8; 16],
    pub is_personalized: bool,
}

/// Parse a Switch `.tik` binary ticket.
pub fn parse_ticket(data: &[u8]) -> Result<TicketInfo, String> {
    if data.len() < 4 {
        return Err("ticket file is too short".into());
    }
    let sig_type = u32::from_le_bytes(data[0..4].try_into().unwrap());
    let data_off: usize = match sig_type {
        0x010000 | 0x010003 => 0x240, // RSA-4096
        0x010001 | 0x010004 => 0x140, // RSA-2048
        0x010002 | 0x010005 => 0x80,  // ECDSA
        _ => return Err(format!("unsupported ticket signature type 0x{:08x}", sig_type)),
    };
    if data.len() < data_off + 0x170 {
        return Err("ticket file is truncated".into());
    }
    let body = &data[data_off..];
    let title_key_type = body[0x141];
    let encrypted_title_key: [u8; 16] = body[0x40..0x50].try_into().unwrap();
    let rights_id: [u8; 16] = body[0x160..0x170].try_into().unwrap();
    Ok(TicketInfo {
        rights_id,
        encrypted_title_key,
        is_personalized: title_key_type != 0,
    })
}

/// Search common locations for the user's `prod.keys` file when `--keys` is not given explicitly.
pub fn discover_prod_keys(hint_dirs: &[&Path], repo: Option<&Path>) -> Option<PathBuf> {
    let mut candidates: Vec<PathBuf> = Vec::new();
    for d in hint_dirs {
        candidates.push(d.join("prod.keys"));
    }
    if let Some(r) = repo {
        candidates.push(r.join("prod.keys"));
        candidates.push(r.join("keys").join("prod.keys"));
    }

    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from);
    if let Some(h) = &home {
        candidates.push(h.join(".switch").join("prod.keys"));
    }

    #[cfg(target_os = "windows")]
    if let Some(appdata) = std::env::var_os("APPDATA").map(PathBuf::from) {
        for sub in [
            "Ryujinx/system/prod.keys",
            "yuzu/keys/prod.keys",
            "suyu/keys/prod.keys",
            "eden/keys/prod.keys",
            "citron/keys/prod.keys",
        ] {
            candidates.push(appdata.join(sub));
        }
    }

    #[cfg(target_os = "macos")]
    if let Some(h) = &home {
        let app_sup = h.join("Library").join("Application Support");
        for sub in [
            "Ryujinx/system/prod.keys",
            "yuzu/keys/prod.keys",
            "suyu/keys/prod.keys",
            "eden/keys/prod.keys",
            "citron/keys/prod.keys",
        ] {
            candidates.push(app_sup.join(sub));
        }
    }

    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Some(cfg) = std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .or_else(|| home.as_ref().map(|h| h.join(".config")))
        {
            candidates.push(cfg.join("Ryujinx").join("system").join("prod.keys"));
        }
        if let Some(data) = std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .or_else(|| home.as_ref().map(|h| h.join(".local").join("share")))
        {
            for emu in ["yuzu", "suyu", "eden", "citron"] {
                candidates.push(data.join(emu).join("keys").join("prod.keys"));
            }
        }
    }

    candidates.into_iter().find(|p| p.is_file())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::hex;

    #[test]
    fn derives_keys_from_master_keys_and_sources() {
        let mk0 = [0x10u8; 16];
        let mk14 = [0x24u8; 16];
        let kek_seed = [0x33u8; 16];
        let key_seed = [0x44u8; 16];
        let hks = [0x55u8; 16];
        let hsrc = [0x66u8; 32];
        let app_src = [0x77u8; 16];
        let tk_src = [0x88u8; 16];
        let txt = format!(
            "# synthetic test keys (not real Nintendo keys)\n\
             master_key_00 = {}\n\
             master_key_0e = {}\n\
             aes_kek_generation_source = {}\n\
             aes_key_generation_source = {}\n\
             header_kek_source = {}\n\
             header_key_source = {}\n\
             key_area_key_application_source = {}\n\
             titlekek_source = {}\n",
            hex(&mk0),
            hex(&mk14),
            hex(&kek_seed),
            hex(&key_seed),
            hex(&hks),
            hex(&hsrc),
            hex(&app_src),
            hex(&tk_src)
        );
        let ks = KeySet::from_prod_keys_str(&txt).unwrap();
        assert!(ks.header_key.is_some());
        assert!(ks.key_area_key(0, 0x0e).is_ok());
        assert!(ks.titlekek.contains_key(&0x0e));
        assert!(ks.key_area_key(0, 0x15).is_err());
    }

    #[test]
    fn parses_common_and_personalized_tickets() {
        let mut tik = vec![0u8; 0x2c0];
        tik[0..4].copy_from_slice(&0x010004u32.to_le_bytes()); // RSA-2048 SHA256 -> body at 0x140
        tik[0x180..0x190].copy_from_slice(&[0xAA; 16]);
        tik[0x281] = 0; // common
        tik[0x2a0..0x2b0].copy_from_slice(&[0xBB; 16]);
        let info = parse_ticket(&tik).unwrap();
        assert!(!info.is_personalized);
        assert_eq!(info.encrypted_title_key, [0xAA; 16]);
        assert_eq!(info.rights_id, [0xBB; 16]);

        tik[0x281] = 1; // personalized
        assert!(parse_ticket(&tik).unwrap().is_personalized);
    }
}
