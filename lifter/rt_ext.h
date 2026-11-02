/* [Recomps#1] Runtime helpers for the instructions our lifter copy adds on top of ssbu-recomp's recomp.py.
 * Include after runtime.h and neon.h. Element-by-element, correctness first (same style as neon.h).
 *
 * Requirements on the runtime:
 *   - Ctx needs one more field after ex_val:   uint64_t ex_val2;   (high half seen by the last LDXP of two X regs)
 *   - stx128 uses a 16-byte compare-and-swap: compile with clang -mcx16 (x86-64 CMPXCHG16B).
 * Saturating instructions do not set FPSR.QC (nothing in the lifted code reads it so far). */
#ifndef RECOMP_RT_EXT_H
#define RECOMP_RT_EXT_H

/* STXP / STLXP of two X registers: store only if the 16 bytes still hold what the matching LDXP read. 0 = success. */
static inline uint32_t stx128(Ctx *c, uint64_t a, uint64_t lo, uint64_t hi) {
    if (c->ex_addr != a) { stx_diag(c, a, 0, 0, lo, -128); return 1; }
    c->ex_addr = ~0ull;
    unsigned __int128 expected = ((unsigned __int128)c->ex_val2 << 64) | c->ex_val;
    unsigned __int128 desired = ((unsigned __int128)hi << 64) | lo;
    if (__atomic_compare_exchange_n((unsigned __int128 *)(uintptr_t)a, &expected, desired, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { c->ex_fail = 0; return 0; }
    stx_diag(c, a, c->ex_val, (uint64_t)expected, lo, 128);
    return 1;
}

static inline uint64_t sat_s(int64_t v, int es) {          /* clamp to a signed element of size 8 << es bits */
    int64_t hi = (int64_t)((1ull << ((8 << es) - 1)) - 1), lo = -hi - 1;
    return (uint64_t)(v > hi ? hi : v < lo ? lo : v) & (es == 3 ? ~0ull : (1ull << (8 << es)) - 1);
}
static inline uint64_t sat_u(uint64_t v, int es) {
    uint64_t hi = es == 3 ? ~0ull : (1ull << (8 << es)) - 1;
    return v > hi ? hi : v;
}

/* SMULL/UMULL (op 0), SMLAL/UMLAL (1), SMLSL/UMLSL (2), "2" forms (q = 1) read the upper halves.
 * es = source element size (0..2); the destination elements are twice as wide and fill all 128 bits. */
static inline void v_long3(V128 *d, const V128 *n, const V128 *m, int op, int es, int u, int q) {
    V128 r = *d;
    int cnt = 8 >> es;
    for (int i = 0; i < cnt; i++) {
        uint64_t a = vget(n, es, i + q * cnt), b = vget(m, es, i + q * cnt);
        uint64_t p = u ? a * b : (uint64_t)(vsext(a, es) * vsext(b, es));
        uint64_t acc = op ? vget(d, es + 1, i) : 0;
        vset(&r, es + 1, i, op == 0 ? p : op == 1 ? acc + p : acc - p);
    }
    *d = r;
}

/* SQXTN/UQXTN{2}: narrow each 2x-wide element of n with saturation; q = 1 writes the upper half and keeps the lower. */
static inline void v_qxtn(V128 *d, const V128 *n, int es, int q, int u) {
    V128 r = *d;
    int cnt = 8 >> es;
    if (!q) r.d[1] = 0;
    for (int i = 0; i < cnt; i++) {
        uint64_t x = vget(n, es + 1, i);
        vset(&r, es, i + q * cnt, u ? sat_u(x, es) : sat_s(vsext(x, es + 1), es));
    }
    *d = r;
}

/* SRSHR/URSHR: rounding shift right by sh (1..element bits); count elements (the rest of the register is zeroed). */
static inline void v_rshr(V128 *d, const V128 *n, int sh, int es, int u, int count) {
    V128 r; vzero(&r);
    for (int i = 0; i < count; i++) {
        uint64_t x = vget(n, es, i), y;
        if (u) y = (uint64_t)(((unsigned __int128)x + ((unsigned __int128)1 << (sh - 1))) >> sh);
        else y = (uint64_t)(int64_t)(((__int128)vsext(x, es) + ((__int128)1 << (sh - 1))) >> sh);
        vset(&r, es, i, y);
    }
    *d = r;
}

/* SQSHRN (round 0) / SQRSHRN (round 1){2}: signed 2x-wide source, shift right by sh, saturate to es; q as v_qxtn. */
static inline void v_sqshrn(V128 *d, const V128 *n, int sh, int es, int q, int round) {
    V128 r = *d;
    int cnt = 8 >> es;
    if (!q) r.d[1] = 0;
    for (int i = 0; i < cnt; i++) {
        __int128 v = vsext(vget(n, es + 1, i), es + 1);       /* 128-bit: rounding a 64-bit source cannot overflow */
        if (round) v += (__int128)1 << (sh - 1);
        v >>= sh;                                            /* now fits in es + 1 bits, so in int64 */
        vset(&r, es, i + q * cnt, sat_s((int64_t)v, es));
    }
    *d = r;
}

/* FCVTZS/FCVTZU (vector, fixed-point): x * 2^fbits, round toward zero, saturate; NaN gives 0. */
static inline void v_fcvtz_fix(V128 *d, const V128 *n, int fbits, int dbl, int u, int count) {
    V128 r; vzero(&r);
    double scale = ldexp(1.0, fbits);
    for (int i = 0; i < count; i++) {
        double x = (dbl ? n->df[i] : (double)n->f[i]) * scale;
        uint64_t y;
        if (x != x) y = 0;
        else if (dbl) {
            if (u) y = x <= 0.0 ? 0 : x >= 18446744073709551616.0 ? ~0ull : (uint64_t)x;
            else y = (uint64_t)(x >= 9223372036854775808.0 ? INT64_MAX : x < -9223372036854775808.0 ? INT64_MIN : (int64_t)x);
        } else {
            if (u) y = x <= 0.0 ? 0 : x >= 4294967296.0 ? 0xffffffffull : (uint64_t)x;
            else y = (uint32_t)(x >= 2147483648.0 ? INT32_MAX : x <= -2147483649.0 ? INT32_MIN : (int32_t)x);
        }
        vset(&r, dbl ? 3 : 2, i, y);
    }
    *d = r;
}

/* ---- [witchtime] forms Bayonetta 3's main uses that B2's did not (lifter/recomp.py marks them [witchtime]) ---- */

static inline uint64_t sat128(__int128 r, int es) { return sat_s(r > INT64_MAX ? INT64_MAX : r < INT64_MIN ? INT64_MIN : (int64_t)r, es); }
/* SSHL/USHL/SRSHL/URSHL/SQSHL/UQSHL shift by the signed low byte of each element of m: left if >= 0, else right. */
static inline uint64_t shl_reg(uint64_t x, int8_t sh, int es, int u, int round, int sat) {
    int bits = 8 << es;
    __int128 v = u ? (__int128)x : (__int128)vsext(x, es);
    if (sh >= 0) {
        if (sat) {
            __int128 r = sh >= 64 ? (v == 0 ? 0 : v > 0 ? ((__int128)1 << 100) : -((__int128)1 << 100)) : v << sh;
            if (u) return r > (__int128)(es == 3 ? ~0ull : (1ull << bits) - 1) ? (es == 3 ? ~0ull : (1ull << bits) - 1) : (uint64_t)r;
            return sat128(r, es);
        }
        return sh >= bits ? 0 : (uint64_t)(v << sh);
    }
    int r = -sh;
    if (r > bits) r = bits + 1;                              /* beyond the element: only the sign (or rounding) survives */
    if (round) v += (__int128)1 << (r - 1);
    return (uint64_t)(v >> r);
}
enum { X_SMAX, X_UMAX, X_SMIN, X_UMIN, X_SSHL, X_USHL, X_SRSHL, X_URSHL, X_SQSHL, X_UQSHL, X_SQADD, X_UQADD, X_SQSUB, X_UQSUB,
       X_MLA, X_MLS };
static inline void v_i3x(V128 *d, const V128 *n, const V128 *m, int op, int es, int count) {
    V128 r; vzero(&r);
    uint64_t mask = es == 3 ? ~0ull : (1ull << (8 << es)) - 1;
    for (int i = 0; i < count; i++) {
        uint64_t x = vget(n, es, i), y = vget(m, es, i), z = 0;
        int64_t sx = vsext(x, es), sy = vsext(y, es);
        switch (op) {
        case X_SMAX: z = sx > sy ? x : y; break;
        case X_UMAX: z = x > y ? x : y; break;
        case X_SMIN: z = sx < sy ? x : y; break;
        case X_UMIN: z = x < y ? x : y; break;
        case X_SSHL: case X_USHL: case X_SRSHL: case X_URSHL: case X_SQSHL: case X_UQSHL:
            z = shl_reg(x, (int8_t)y, es, op & 1, op == X_SRSHL || op == X_URSHL, op >= X_SQSHL); break;
        case X_SQADD: z = sat128((__int128)sx + sy, es); break;
        case X_SQSUB: z = sat128((__int128)sx - sy, es); break;
        case X_UQADD: z = x + y < x || x + y > mask ? mask : x + y; break;
        case X_UQSUB: z = x > y ? x - y : 0; break;
        case X_MLA: z = vget(d, es, i) + x * y; break;
        case X_MLS: z = vget(d, es, i) - x * y; break;
        }
        vset(&r, es, i, z);
    }
    *d = r;
}

/* ADDP (vector): pairwise sums of n, then of m. */
static inline void v_addp(V128 *d, const V128 *n, const V128 *m, int es, int q) {
    V128 r; vzero(&r);
    int half = (q ? 16 : 8) >> es >> 1;
    for (int i = 0; i < half; i++) {
        vset(&r, es, i, vget(n, es, 2 * i) + vget(n, es, 2 * i + 1));
        vset(&r, es, i + half, vget(m, es, 2 * i) + vget(m, es, 2 * i + 1));
    }
    *d = r;
}

/* {S,U}ADDL (op 0), ADDW (1), SUBL (2), SUBW (3){2}: wide forms take n as already widened; q = 1 reads upper halves. */
static inline void v_addsubl(V128 *d, const V128 *n, const V128 *m, int op, int es, int u, int q) {
    V128 r;
    int cnt = 8 >> es;
    for (int i = 0; i < cnt; i++) {
        uint64_t b = vget(m, es, i + q * cnt), a;
        b = u ? b : (uint64_t)vsext(b, es);
        if (op & 1) a = vget(n, es + 1, i);
        else { a = vget(n, es, i + q * cnt); a = u ? a : (uint64_t)vsext(a, es); }
        vset(&r, es + 1, i, op & 2 ? a - b : a + b);
    }
    *d = r;
}

/* ADDV (op 0), {S,U}MAXV (1), {S,U}MINV (2), {S,U}ADDLV (3): one result in element 0, the rest zeroed. */
static inline void v_across(V128 *d, const V128 *n, int op, int es, int u, int count) {
    uint64_t acc = vget(n, es, 0);
    if (op == 3 && !u) acc = (uint64_t)vsext(acc, es);
    for (int i = 1; i < count; i++) {
        uint64_t x = vget(n, es, i);
        int64_t sx = vsext(x, es), sa = vsext(acc, es);
        if (op == 0) acc += x;
        else if (op == 3) acc += u ? x : (uint64_t)sx;
        else if (op == 1) acc = u ? (x > acc ? x : acc) : (sx > sa ? x : acc);
        else acc = u ? (x < acc ? x : acc) : (sx < sa ? x : acc);
    }
    vzero(d);
    vset(d, op == 3 ? es + 1 : es, 0, acc);
}

/* FRINTN (0), FRINTM (1), FRINTP (2), FRINTZ (3), FRINTA (4), vector. */
static inline void v_frint(V128 *d, const V128 *n, int mode, int dbl, int count) {
    V128 r; vzero(&r);
    for (int i = 0; i < count; i++) {
        double x = dbl ? n->df[i] : (double)n->f[i];
        double y = mode == 0 ? nearbyint(x) : mode == 1 ? floor(x) : mode == 2 ? ceil(x) : mode == 3 ? trunc(x) : round(x);
        if (dbl) r.df[i] = y; else r.f[i] = (float)y;
    }
    *d = r;
}

/* FCVTAS / FCVTAU: round to nearest, ties away from zero, saturate; NaN gives 0. */
static inline void v_fcvta(V128 *d, const V128 *n, int dbl, int u, int count) {
    V128 r; vzero(&r);
    for (int i = 0; i < count; i++) {
        double x = round(dbl ? n->df[i] : (double)n->f[i]);
        uint64_t y;
        if (x != x) y = 0;
        else if (dbl) {
            if (u) y = x <= 0.0 ? 0 : x >= 18446744073709551616.0 ? ~0ull : (uint64_t)x;
            else y = (uint64_t)(x >= 9223372036854775808.0 ? INT64_MAX : x < -9223372036854775808.0 ? INT64_MIN : (int64_t)x);
        } else {
            if (u) y = x <= 0.0 ? 0 : x >= 4294967296.0 ? 0xffffffffull : (uint64_t)x;
            else y = (uint32_t)(x >= 2147483648.0 ? INT32_MAX : x <= -2147483649.0 ? INT32_MIN : (int32_t)x);
        }
        vset(&r, dbl ? 3 : 2, i, y);
    }
    *d = r;
}

/* {S,U}ADDLP, and with acc {S,U}ADALP: pairs of es elements summed into es + 1 elements. */
static inline void v_addlp(V128 *d, const V128 *n, int es, int u, int q, int acc) {
    V128 r; vzero(&r);
    int cnt = (q ? 16 : 8) >> es >> 1;
    for (int i = 0; i < cnt; i++) {
        uint64_t a = vget(n, es, 2 * i), b = vget(n, es, 2 * i + 1);
        uint64_t s = u ? a + b : (uint64_t)(vsext(a, es) + vsext(b, es));
        vset(&r, es + 1, i, s + (acc ? vget(d, es + 1, i) : 0));
    }
    *d = r;
}

/* {S,U}SRA: shift right by sh (1..element bits) and add to d. */
static inline void v_sra(V128 *d, const V128 *n, int sh, int es, int u, int count) {
    V128 r; vzero(&r);
    for (int i = 0; i < count; i++) {
        uint64_t x = vget(n, es, i);
        uint64_t y = u ? (sh >= 64 ? 0 : x >> sh) : (uint64_t)(vsext(x, es) >> (sh >= 64 ? 63 : sh));
        vset(&r, es, i, vget(d, es, i) + y);
    }
    *d = r;
}

/* SHRN / RSHRN{2}: 2x-wide source shifted right by sh, truncated to es; q as v_qxtn. */
static inline void v_shrn(V128 *d, const V128 *n, int sh, int es, int q, int round) {
    V128 r = *d;
    int cnt = 8 >> es;
    if (!q) r.d[1] = 0;
    for (int i = 0; i < cnt; i++) {
        unsigned __int128 v = vget(n, es + 1, i);
        if (round) v += (unsigned __int128)1 << (sh - 1);
        vset(&r, es, i + q * cnt, (uint64_t)(v >> sh));
    }
    *d = r;
}

/* MUL (op 0), MLA (1), MLS (2), SQDMULH (3), SQRDMULH (4) by element: y is the selected element of m. */
static inline void v_ielem(V128 *d, const V128 *n, uint64_t y, int op, int es, int count) {
    V128 r; vzero(&r);
    int bits = 8 << es;
    int64_t sy = vsext(y, es);
    for (int i = 0; i < count; i++) {
        uint64_t x = vget(n, es, i), z;
        if (op == 0) z = x * y;
        else if (op == 1) z = vget(d, es, i) + x * y;
        else if (op == 2) z = vget(d, es, i) - x * y;
        else {
            int64_t p = 2 * vsext(x, es) * sy + (op == 4 ? (int64_t)1 << (bits - 1) : 0);   /* es <= 2: fits in 64 bits */
            z = sat_s(p >> bits, es);
        }
        vset(&r, es, i, z);
    }
    *d = r;
}

#endif
