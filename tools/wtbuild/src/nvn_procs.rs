//! Every NVN entry point of YOUR sdk by name, read from the sdk's own proc table, and what runtime/nvn_sdk.c's GPU
//! model points at (`wtbuild nvn-procs` lists them). Nothing is hard-coded; docs/RE_NOTES.md "NVN" has the details.

use crate::lifter::layout;
use crate::util::{cstr, hx, i64_at, sext, u32_at, u64_at};
use std::collections::{BTreeMap, HashMap};

/// R_AARCH64_RELATIVE relocations: slot -> addend, in the order Python's dict keeps them (first insertion)
pub struct Rel {
    pub order: Vec<u64>,
    pub map: HashMap<u64, i64>,
}

impl Rel {
    pub fn read(d: &[u8]) -> Rel {
        let (_, dt) = layout(d);
        let (rela, relasz) = (*dt.get(&7).unwrap_or(&0) as usize, *dt.get(&8).unwrap_or(&0) as usize);
        let mut r = Rel {
            order: Vec::new(),
            map: HashMap::new(),
        };
        for i in 0..relasz / 24 {
            let o = 0x1000 + rela + i * 24;
            let (off, info, add) = (u64_at(d, o), u64_at(d, o + 8), i64_at(d, o + 16));
            if info == 0x403 && r.map.insert(off, add).is_none() {
                r.order.push(off);
            }
        }
        r
    }
    pub fn get(&self, k: i64) -> Option<i64> {
        if k < 0 {
            return None;
        }
        self.map.get(&(k as u64)).copied()
    }
}

fn word(d: &[u8], off: i64) -> Option<u32> {
    let o = 0x1000 + off;
    if o < 0 || o as usize + 4 > d.len() {
        return None;
    }
    Some(u32_at(d, o as usize))
}

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Add,
    Got,
    Count,
}

/// adrp+add (and adrp+ldr GOT) targets of the code in [lo, hi), and "mov w23, #count - 1"
fn refs(ws: &[u32], lo: i64, hi: i64) -> Vec<(Kind, i64)> {
    let mut page: HashMap<u32, i64> = HashMap::new();
    let mut out = Vec::new();
    let (lo, hi) = (lo.max(0) as usize, (hi.max(0) as usize).min(ws.len()));
    for (k, &w) in ws.iter().enumerate().take(hi).skip(lo) {
        if w & 0x9f000000 == 0x90000000 {
            page.insert(
                w & 31,
                ((k as i64 * 4) & !0xfff) + (sext(((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)) as i64, 21) << 12),
            );
        } else if w & 0xffc00000 == 0x91000000 && page.contains_key(&((w >> 5) & 31)) {
            out.push((Kind::Add, page[&((w >> 5) & 31)] + ((w >> 10) & 0xfff) as i64));
        } else if w & 0xffc00000 == 0xf9400000 && page.contains_key(&((w >> 5) & 31)) {
            out.push((Kind::Got, page[&((w >> 5) & 31)] + ((w >> 10) & 0xfff) as i64 * 8));
        } else if w & 0xffe0001f == 0x52800017 {
            out.push((Kind::Count, ((w >> 5) & 0xffff) as i64 + 1));
        }
    }
    out
}

/// name -> sdk offset of every NVN entry point
pub fn procs(d: &[u8]) -> Result<BTreeMap<String, i64>, String> {
    let (text, _) = layout(d);
    let rel = Rel::read(d);
    let key = find(d, b"\0DeviceGetInteger\0").map(|p| p as i64).unwrap_or(-1) + 1 - 0x1000;
    let ws: Vec<u32> = (0..(text / 4) as usize).map(|i| u32_at(d, 0x1000 + 4 * i)).collect();
    for k in 0..ws.len() as i64 {
        let last = refs(&ws, k - 1, k + 1).last().copied();
        let Some((Kind::Add, names)) = last else { continue };
        if !(names <= key && key < names + 0x10000) {
            continue;
        }
        let around = refs(&ws, k - 24, k + 40);
        let counts: Vec<i64> = around.iter().filter(|r| r.0 == Kind::Count).map(|r| r.1).collect();
        let gots: Vec<i64> = around.iter().filter(|r| r.0 == Kind::Got && rel.get(r.1).is_some()).map(|r| r.1).collect();
        for table in around.iter().filter(|r| r.0 == Kind::Add && r.1 != names).map(|r| r.1) {
            for &count in &counts {
                for &got in &gots {
                    let mut keys: Vec<String> = Vec::new();
                    let mut out: HashMap<String, Option<i64>> = HashMap::new();
                    let mut ok = true;
                    for n in 0..count {
                        let Some(no) = word(d, table + 4 * n) else {
                            ok = false;
                            break;
                        };
                        let q = 0x1000 + names + no as i64;
                        if q < 0 || q as usize >= d.len() || !d[q as usize..].contains(&0) {
                            ok = false;
                            break;
                        }
                        let name = format!("nvn{}", cstr(d, q as usize));
                        let v = rel.get(rel.get(got).unwrap() + 8 * n);
                        if out.insert(name.clone(), v).is_none() {
                            keys.push(name);
                        }
                    }
                    if !ok {
                        continue;
                    }
                    let mut sorted = keys.clone();
                    sorted.sort();
                    if out.contains_key("nvnDeviceGetInteger") && keys == sorted && out.values().all(|v| matches!(v, Some(x) if *x != 0)) {
                        return Ok(out.into_iter().map(|(k, v)| (k, v.unwrap())).collect());
                    }
                }
            }
        }
    }
    Err("sdk.elf: no NVN proc table found (not nnSdk 13.3.2?)".into())
}

/// `wtbuild nvn-procs`: "addr <offset> <name>" per name (every entry point when none is given), for lifter/sdklift_roots.txt
pub fn listing(table: &BTreeMap<String, i64>, names: &[&str]) -> Vec<String> {
    let all: Vec<&str> = table.keys().map(String::as_str).collect();
    let names = if names.is_empty() { &all[..] } else { names };
    names
        .iter()
        .map(|n| match table.get(*n) {
            Some(&a) if a != 0 => format!("addr {} {}", hx(a), n),
            _ => format!("# {}: not in this sdk", n),
        })
        .collect()
}

pub fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

/// non-overlapping occurrences, as re.finditer finds them
fn find_all(hay: &[u8], needle: &[u8]) -> Vec<usize> {
    let mut out = Vec::new();
    let mut i = 0;
    while let Some(p) = find(&hay[i..], needle) {
        out.push(i + p);
        i += p + needle.len();
    }
    out
}

/// Offset of the method table (vtable + 0x10) of the class whose typeinfo name is typeinfo_name (nvn_procs.vtable).
pub fn vtable(d: &[u8], typeinfo_name: &str) -> Result<i64, String> {
    let rel = Rel::read(d);
    let mut key = typeinfo_name.as_bytes().to_vec();
    key.push(0);
    let names: std::collections::HashSet<i64> = find_all(d, &key).into_iter().map(|m| m as i64 - 0x1000).collect();
    let ti: Vec<i64> = rel.order.iter().filter(|k| names.contains(&rel.map[k])).map(|&k| k as i64 - 8).collect();
    let text = layout(d).0;
    let vt: Vec<i64> = if ti.is_empty() {
        vec![]
    } else {
        rel.order
            .iter()
            .filter(|&&k| {
                let k = k as i64;
                rel.map[&(k as u64)] == ti[0] && rel.get(k - 8).is_none() && {
                    let n = rel.get(k + 8).unwrap_or(0);
                    0 < n && n < text
                }
            })
            .map(|&k| k as i64)
            .collect()
    };
    if ti.len() != 1 || vt.len() != 1 {
        return Err(format!("sdk.elf: no single vtable for {}", typeinfo_name));
    }
    Ok(vt[0] + 8)
}

/// What nvn_sdk.c's device model points at, by key.
pub fn gpu_model(d: &[u8]) -> Result<BTreeMap<String, i64>, String> {
    let rel = Rel::read(d);
    let p = procs(d)?;
    let func = p["nvnTextureBuilderGetStorageAlignment"];
    let ws: Vec<u32> = (0..64).map(|k| word(d, func + 4 * k).unwrap_or(0)).collect();
    let mut fmt = None;
    for k in 0..63 {
        let (w, w2) = (ws[k], ws[k + 1]);
        if w & 0x9f000000 == 0x90000000 && w2 & 0xffc00000 == 0xf9400000 && (w2 >> 5) & 31 == w & 31 {
            let page = ((func + 4 * k as i64) & !0xfff) + (sext(((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)) as i64, 21) << 12);
            fmt = Some(
                rel.get(page + ((w2 >> 10) & 0xfff) as i64 * 8)
                    .ok_or("NVN format table slot is not relocated")?,
            );
            break;
        }
    }
    let name = find(d, b"\0NVIDIA Tegra/Nintendo Switch\0").map(|p| p as i64).unwrap_or(-1) + 1 - 0x1000;
    let desc: Vec<i64> = rel
        .order
        .iter()
        .filter(|k| rel.map[k] == name && word(d, **k as i64 + 0x10) == Some(0x12b))
        .map(|&k| k as i64)
        .collect();
    let (Some(fmt), 1) = (fmt, desc.len()) else {
        return Err("sdk.elf: NVN format table or Tegra X1 descriptor not found".into());
    };
    let size = p["nvnTextureBuilderGetStorageSize"];
    let bl = (0..32)
        .find_map(|k| {
            let w = word(d, size + 4 * k)?;
            (w >> 26 == 0x25).then(|| size + 4 * k + 4 * sext((w & 0x3ffffff) as i64, 26))
        })
        .ok_or("nvnTextureBuilderGetStorageSize: no call")?;
    let count = (0..0x400)
        .find(|n| (0..0x30).step_by(8).any(|k| rel.get(fmt + 0x30 * n + k).is_some()))
        .ok_or("NVN format table: no end")?;
    let mut m = BTreeMap::new();
    m.insert("format_table".into(), fmt);
    m.insert("format_count".into(), count);
    m.insert("chip_descriptor".into(), desc[0]);
    m.insert("texture_layout".into(), bl);
    m.insert("vt_nvgpu_device".into(), vtable(d, "N8nvrm_gpu5nvgpu15NvRmNvgpuDeviceE")?);
    m.insert("vt_hal_gm200".into(), vtable(d, "N8nvrm_gpu33NvRmGpu_HwHalFunctions_GM200_BaseE")?);
    Ok(m)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn listing_lines() {
        let t: BTreeMap<String, i64> = [("nvnQueueFinish", 0x4b30d0), ("nvnDeviceGetInteger", 0x4a6db0)].into_iter().map(|(k, v)| (k.into(), v)).collect();
        assert_eq!(listing(&t, &[]), ["addr 4a6db0 nvnDeviceGetInteger", "addr 4b30d0 nvnQueueFinish"]);
        assert_eq!(listing(&t, &["nvnQueueFinish", "nvnNope"]), ["addr 4b30d0 nvnQueueFinish", "# nvnNope: not in this sdk"]);
    }
}
