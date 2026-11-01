//! Small helpers shared by the steps: little-endian reads, sign extension, Python-compatible number formatting (the
//! generated C must be byte for byte what the Python pipeline wrote, so that a switch from setup.py recompiles nothing).

use sha2::{Digest, Sha256};
use std::fs;
use std::io::Read;
use std::path::Path;

pub fn u16_at(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes(b[o..o + 2].try_into().unwrap())
}
pub fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
pub fn i32_at(b: &[u8], o: usize) -> i32 {
    i32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
pub fn u64_at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}
pub fn i64_at(b: &[u8], o: usize) -> i64 {
    i64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}
pub fn put_u64(b: &mut [u8], o: usize, v: u64) {
    b[o..o + 8].copy_from_slice(&v.to_le_bytes());
}

/// The NUL-terminated string at `o` (latin-1, as the Python scripts decode names).
pub fn cstr(b: &[u8], o: usize) -> String {
    let end = b[o..].iter().position(|&c| c == 0).map(|n| o + n).unwrap_or(b.len());
    b[o..end].iter().map(|&c| c as char).collect()
}

/// bits hi..lo of w
#[inline]
pub fn bits(w: u32, hi: u32, lo: u32) -> u32 {
    (w >> lo) & (((1u64 << (hi - lo + 1)) - 1) as u32)
}

#[inline]
pub fn sext(v: i64, n: u32) -> i64 {
    if v & (1 << (n - 1)) != 0 {
        v - (1 << n)
    } else {
        v
    }
}

/// Python's "%x" of an int (a negative one gets a minus sign).
pub fn hx(v: i64) -> String {
    if v < 0 {
        format!("-{:x}", -(v as i128))
    } else {
        format!("{:x}", v)
    }
}

/// Python's repr() of a float (shortest round-trip digits; exponent form below 1e-4 and from 1e16 on).
pub fn py_repr(v: f64) -> String {
    if v.is_nan() {
        return "nan".into();
    }
    if v.is_infinite() {
        return if v > 0.0 { "inf".into() } else { "-inf".into() };
    }
    if v == 0.0 {
        return if v.is_sign_negative() { "-0.0".into() } else { "0.0".into() };
    }
    let mut e = format!("{:e}", v); // e.g. "-1.8446744073709552e19", "1e-5"
    if e.split_once('e').unwrap().0.chars().filter(|c| c.is_ascii_digit()).count() >= 17 {
        // 17 digits: no shorter form, so the correctly rounded one (ties to even), as Python writes it
        e = format!("{:.16e}", v);
    }
    let (mant, exp) = e.split_once('e').unwrap();
    let exp: i32 = exp.parse().unwrap();
    let neg = mant.starts_with('-');
    let digits: String = mant.chars().filter(|c| c.is_ascii_digit()).collect();
    let sign = if neg { "-" } else { "" };
    if (-4..16).contains(&exp) {
        let n = digits.len() as i32;
        let s = if exp >= 0 {
            if n > exp + 1 {
                format!("{}.{}", &digits[..(exp + 1) as usize], &digits[(exp + 1) as usize..])
            } else {
                format!("{}{}.0", digits, "0".repeat((exp + 1 - n) as usize))
            }
        } else {
            format!("0.{}{}", "0".repeat((-exp - 1) as usize), digits)
        };
        format!("{}{}", sign, s)
    } else {
        let m = if digits.len() > 1 {
            format!("{}.{}", &digits[..1], &digits[1..])
        } else {
            digits.clone()
        };
        format!("{}{}e{}{:02}", sign, m, if exp < 0 { '-' } else { '+' }, exp.abs())
    }
}

/// Compile and texture jobs when no --jobs is given: half the logical CPUs, so a build does not peg the PC.
/// The other half are mostly SMT siblings: 16 -> 8 jobs of 16 threads took 17% longer on 31% less CPU time.
pub fn default_jobs() -> usize {
    (std::thread::available_parallelism().map(|n| n.get()).unwrap_or(8) / 2).clamp(1, 24)
}

pub fn sha256_file(path: &Path) -> std::io::Result<String> {
    let mut f = fs::File::open(path)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(hex(&h.finalize()))
}

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

/// setup.py's time format: "1m05s" or "4.2s"
pub fn fmt_secs(secs: f64) -> String {
    if secs >= 60.0 {
        let s = secs as u64;
        format!("{}m{:02}s", s / 60, s % 60)
    } else {
        format!("{:.1}s", secs)
    }
}

/// Write only when the content differs (keeps file times, so the compile step skips unchanged files).
pub fn write_if_changed(path: &Path, content: &[u8]) -> std::io::Result<bool> {
    if let Ok(old) = fs::read(path) {
        if old == content {
            return Ok(false);
        }
    }
    fs::write(path, content)?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn repr_matches_python() {
        let cases: &[(f64, &str)] = &[
            (0.5, "0.5"),
            (2.0f64.powi(-5), "0.03125"),
            (2.0f64.powi(-14), "6.103515625e-05"),
            (2.0f64.powi(-64), "5.421010862427522e-20"),
            (2.0f64.powi(64), "1.8446744073709552e+19"),
            (2.0f64.powi(50), "1125899906842624.0"),
            (2.0f64.powi(54), "1.8014398509481984e+16"),
            (31.0, "31.0"),
            (-0.125, "-0.125"),
            (0.0001, "0.0001"),
            (1e16, "1e+16"),
            (-1.9375, "-1.9375"),
            (2.0f64.powi(-25), "2.9802322387695312e-08"),
        ];
        for (v, want) in cases {
            assert_eq!(py_repr(*v), *want, "{}", v);
        }
    }
}
