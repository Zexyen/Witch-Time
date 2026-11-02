//! `wtbuild fps-census`: the frame-rate census report (docs/STATUS.md, "Frame rates above 60"). Per counter site, what
//! one visit adds to its field at 60 and above 60 (census files from a WTBUILD_FPS_CENSUS=1 build run with WT_FPS_CENSUS,
//! runtime/game_shim_eh.c): frame-locked sites add as much per visit when the frames are shorter. Lists the
//! frame-locked per-update sites, busiest first; --all lists every site.

use crate::py::fmt_g;
use std::collections::HashMap;

struct Site {
    visits: i64,
    fields: i64,
    consec: i64,
    same: i64,
    later: i64,
    off: i64,
    mean: f64,
    lo: f64,
    hi: f64,
}

/// (clock updates, sites in file order); a repeated site keeps its first place and takes the last values, as a dict does
fn read(text: &str) -> Result<(i64, Vec<(&str, Site)>), String> {
    let mut lines = text.split('\n');
    let updates = lines
        .next()
        .and_then(|l| l.split_whitespace().nth(1)?.parse().ok())
        .ok_or("census: no 'updates <n>' line")?;
    let mut out: Vec<(&str, Site)> = Vec::new();
    let mut at: HashMap<&str, usize> = HashMap::new();
    for l in lines {
        let a: Vec<&str> = l.split_whitespace().collect();
        if a.len() < 11 {
            continue;
        }
        let n = |i: usize| a[i].parse::<i64>().map_err(|_| format!("census: {} is not a count", a[i]));
        let f = |i: usize| a[i].parse::<f64>().map_err(|_| format!("census: {} is not a number", a[i]));
        let s = Site {
            visits: n(1)?,
            fields: n(2)?,
            consec: n(3)?,
            same: n(4)?,
            later: n(5)?,
            off: n(6)?,
            mean: f(8)?,
            lo: f(9)?,
            hi: f(10)?,
        };
        match at.get(a[0]) {
            Some(&i) => out[i].1 = s,
            None => {
                at.insert(a[0], out.len());
                out.push((a[0], s));
            }
        }
    }
    Ok((updates, out))
}

/// fps_sites.txt (lifter/eh.rs): store site -> (function, kind, field); the field ("[x19 +16]") holds a space
fn sites(text: &str) -> HashMap<&str, (&str, &str, &str)> {
    let mut out = HashMap::new();
    for l in text.lines().filter(|l| !l.starts_with('#')) {
        let (mut a, mut rest) = (Vec::new(), l.trim_start());
        while a.len() < 5 && !rest.is_empty() {
            let e = rest.find(char::is_whitespace).unwrap_or(rest.len());
            a.push(&rest[..e]);
            rest = rest[e..].trim_start();
        }
        if a.len() == 5 && !rest.is_empty() {
            out.insert(a[0], (a[1], a[4], rest.trim_end()));
        }
    }
    out
}

/// the report of a census at 60 and one above, with the lifter's list of sites
pub fn report(at60: &str, above: &str, site_list: &str, every: bool) -> Result<Vec<String>, String> {
    let (u60, s60) = read(at60)?;
    let (uhi, shi) = read(above)?;
    let s60: HashMap<&str, &Site> = s60.iter().map(|(k, v)| (*k, v)).collect();
    let info = sites(site_list);
    let mut classes = [("frame-locked", 0), ("own clock", 0), ("scaled", 0), ("other", 0)];
    let mut rows = Vec::new();
    for (site, h) in &shi {
        let Some(b) = s60.get(site) else { continue };
        if b.mean == 0.0 || h.mean == 0.0 {
            continue;
        }
        let ratio = b.mean / h.mean;
        let rep = h.consec + h.same + h.later;
        let cq = h.consec as f64 / rep.max(1) as f64;
        // visits per clock update, above / at 60: about 1 for code that runs every frame, about 60 / rate for code on a
        // thread of its own clock (the guest's 60 Hz vsync, audio), which the frame rate does not speed up
        let vpu = (h.visits as f64 / uhi.max(1) as f64) / (b.visits as f64 / u60.max(1) as f64).max(1e-12);
        let mut cls = if (0.8..=1.25).contains(&ratio) {
            "frame-locked"
        } else if ratio >= 1.5 {
            "scaled"
        } else {
            "other"
        };
        if cls == "frame-locked" && vpu < 0.75 {
            cls = "own clock";
        }
        let per_update = rep >= 30 && cq >= 0.5;
        if per_update {
            classes.iter_mut().find(|c| c.0 == cls).unwrap().1 += 1;
        }
        if every || (cls == "frame-locked" && per_update) {
            rows.push((site, h, b, ratio, cq, vpu));
        }
    }
    rows.sort_by_key(|r| -r.1.visits);
    let both = shi.iter().filter(|(s, _)| s60.contains_key(s)).count();
    let mut out = vec![format!("# clock updates: {} at 60, {} above; sites seen in both: {}", u60, uhi, both)];
    for (site, h, b, ratio, cq, vpu) in rows {
        let (func, kind, field) = info.get(site).copied().unwrap_or(("?", "?", "?"));
        out.push(format!(
            "{} {} {:<9} {:<14} | {:>8} {:>5} {:>5.1} {:>5.1} {:>4.2} | {:>10} {:>10} {:>5.2} | {}, {}",
            site,
            func,
            kind,
            field,
            h.visits,
            h.fields,
            100.0 * cq,
            100.0 * h.off as f64 / h.visits.max(1) as f64,
            vpu,
            fmt_g(b.mean, 4),
            fmt_g(h.mean, 4),
            ratio,
            fmt_g(h.lo, 4),
            fmt_g(h.hi, 4)
        ));
    }
    let counts: Vec<String> = classes.iter().map(|(k, n)| format!("{} {}", k, n)).collect();
    out.push(format!(
        "# per-update sites (30+ repeat visits, half or more in consecutive updates): {}",
        counts.join(", ")
    ));
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    // one site per class (a1 frame-locked, b2 own clock, c3 scaled, d4 other), e5 too rarely repeated, f6 / h8 in one
    // file only, g7 with a zero mean; a1 comes twice above 60 (its first place, its last values)
    const AT60: &str = "updates 100
a1 1000 10 900 50 50 0 100 0.5 0.1 1
b2 1000 10 900 50 50 0 100 0.5 0.1 1
c3 1000 10 900 50 50 0 100 1 0.1 2
d4 1000 10 900 50 50 0 100 0.65 0.1 1
e5 10 1 5 0 0 0 10 1 1 1
f6 10 1 5 0 0 0 10 1 1 1
g7 10 1 5 0 0 0 10 1 1 1
";
    const ABOVE: &str = "updates 200
a1 1 1 1 1 1 1 1 1 1 1
b2 1000 10 900 50 50 0 100 0.5 0.1 1
c3 2000 10 1800 100 100 1500 200 0.5 -1 2
d4 2000 10 1800 100 100 0 200 0.5 0.1 1
e5 20 1 10 0 0 0 20 1 1 1
g7 10 1 5 0 0 0 10 0 0 0
h8 10 1 5 0 0 0 10 1 1 1
short line
a1 2000 12 1800 100 100 500 200 0.5 1e-05 123456
";
    const SITES: &str = "# store function load op kind field
a1 f_a 0 0 f32+reg [x19 +16]
c3 f_c 0 0 int32+1 [x8 +0]
";

    #[test]
    fn classes_and_rows() {
        // fps_census.py's output on the same three files
        let head = "# clock updates: 100 at 60, 200 above; sites seen in both: 6";
        let a1 = "a1 f_a f32+reg   [x19 +16]      |     2000    12  90.0  25.0 1.00 |        0.5        0.5  1.00 | 1e-05, 1.235e+05";
        let tail = "# per-update sites (30+ repeat visits, half or more in consecutive updates): frame-locked 1, own clock 1, scaled 1, other 1";
        assert_eq!(report(AT60, ABOVE, SITES, false).unwrap(), [head, a1, tail]);
        let all = [
            head,
            a1,
            "c3 f_c int32+1   [x8 +0]        |     2000    10  90.0  75.0 1.00 |          1        0.5  2.00 | -1, 2",
            "d4 ? ?         ?              |     2000    10  90.0   0.0 1.00 |       0.65        0.5  1.30 | 0.1, 1",
            "b2 ? ?         ?              |     1000    10  90.0   0.0 0.50 |        0.5        0.5  1.00 | 0.1, 1",
            "e5 ? ?         ?              |       20     1 100.0   0.0 1.00 |          1          1  1.00 | 1, 1",
            tail,
        ];
        assert_eq!(report(AT60, ABOVE, SITES, true).unwrap(), all);
    }
}
