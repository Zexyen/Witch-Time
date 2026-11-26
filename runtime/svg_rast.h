/* [witchtime] Minimal zero-dependency SVG rasterizer (C and C++), shared by runtime/pcmenu.c and launcher/src/ui_render.cpp.
 * Renders a clean SVG subset (<svg viewBox>, <path d="M L H V Q C Z">, <circle>, <line>, <polygon>, <polyline>, <rect>,
 * hex colours, fill/stroke opacity, nonzero/evenodd fill rules, round joins/caps) into premultiplied RGBA or BGRA pixels
 * at any requested height, with 8x vertical supersampled analytic scanline fills and 2x2 subpixel capsule-SDF strokes. */
#pragma once
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y; } SvgPt;
typedef struct { float x0, y0, x1, y1; int dir; } SvgEdge;
typedef struct {
    SvgPt *pts; int n, cap;
    int *sub_end; int nsub, sub_cap;
    uint8_t *sub_closed;
} SvgPoly;

static void svg_poly_free(SvgPoly *p) { free(p->pts); free(p->sub_end); free(p->sub_closed); memset(p, 0, sizeof *p); }
static int svg_poly_pt(SvgPoly *p, float x, float y) {
    if (p->n >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 256;
        SvgPt *np = (SvgPt *)realloc(p->pts, (size_t)nc * sizeof *np);
        if (!np) return 0;
        p->pts = np; p->cap = nc;
    }
    p->pts[p->n].x = x; p->pts[p->n].y = y; p->n++;
    return 1;
}
static int svg_poly_end_sub(SvgPoly *p, int closed) {
    int start = p->nsub ? p->sub_end[p->nsub - 1] : 0;
    if (p->n <= start) return 1;
    if (p->nsub >= p->sub_cap) {
        int nc = p->sub_cap ? p->sub_cap * 2 : 16;
        int *ne = (int *)realloc(p->sub_end, (size_t)nc * sizeof *ne);
        uint8_t *cl = (uint8_t *)realloc(p->sub_closed, (size_t)nc * sizeof *cl);
        if (!ne || !cl) { if (ne) p->sub_end = ne; if (cl) p->sub_closed = cl; return 0; }
        p->sub_end = ne; p->sub_closed = cl; p->sub_cap = nc;
    }
    p->sub_end[p->nsub] = p->n;
    p->sub_closed[p->nsub] = (uint8_t)(closed != 0);
    p->nsub++;
    return 1;
}

static void svg_flatten_cubic(SvgPoly *p, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, int depth) {
    float dx = x3 - x0, dy = y3 - y0;
    float d2 = fabsf((x1 - x3) * dy - (y1 - y3) * dx);
    float d3 = fabsf((x2 - x3) * dy - (y2 - y3) * dx);
    float d = d2 + d3;
    if (depth >= 10 || d * d <= 0.0064f * (dx * dx + dy * dy + 1e-6f)) {
        svg_poly_pt(p, x3, y3);
        return;
    }
    float x01 = (x0 + x1) * 0.5f, y01 = (y0 + y1) * 0.5f;
    float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
    float x23 = (x2 + x3) * 0.5f, y23 = (y2 + y3) * 0.5f;
    float x012 = (x01 + x12) * 0.5f, y012 = (y01 + y12) * 0.5f;
    float x123 = (x12 + x23) * 0.5f, y123 = (y12 + y23) * 0.5f;
    float x0123 = (x012 + x123) * 0.5f, y0123 = (y012 + y123) * 0.5f;
    svg_flatten_cubic(p, x0, y0, x01, y01, x012, y012, x0123, y0123, depth + 1);
    svg_flatten_cubic(p, x0123, y0123, x123, y123, x23, y23, x3, y3, depth + 1);
}
static void svg_flatten_quad(SvgPoly *p, float x0, float y0, float x1, float y1, float x2, float y2) {
    svg_flatten_cubic(p, x0, y0, x0 + (x1 - x0) * (2.0f / 3.0f), y0 + (y1 - y0) * (2.0f / 3.0f),
                      x2 + (x1 - x2) * (2.0f / 3.0f), y2 + (y1 - y2) * (2.0f / 3.0f), x2, y2, 0);
}

static void svg_skip_sep(const char **s) {
    while (**s && (isspace((unsigned char)**s) || **s == ',')) (*s)++;
}
static int svg_read_num(const char **s, float *out) {
    svg_skip_sep(s);
    if (!**s) return 0;
    char *e = NULL;
    float v = strtof(*s, &e);
    if (e == *s) return 0;
    *s = e; *out = v;
    return 1;
}
static void svg_parse_path(SvgPoly *p, const char *d, float ox, float oy, float sx, float sy) {
    const char *s = d;
    char cmd = 0;
    float cx = 0, cy = 0, mx = 0, my = 0;
    while (*s) {
        svg_skip_sep(&s);
        if (!*s) break;
        if (isalpha((unsigned char)*s)) { cmd = *s++; }
        else if (!cmd) break;
        int rel = islower((unsigned char)cmd);
        char uc = (char)toupper((unsigned char)cmd);
        if (uc == 'Z') {
            svg_poly_end_sub(p, 1);
            cx = mx; cy = my;
            cmd = 0;
            continue;
        }
        if (uc == 'M') {
            float x, y;
            if (!svg_read_num(&s, &x) || !svg_read_num(&s, &y)) break;
            svg_poly_end_sub(p, 0);
            cx = rel ? cx + x : x; cy = rel ? cy + y : y;
            mx = cx; my = cy;
            svg_poly_pt(p, (cx - ox) * sx, (cy - oy) * sy);
            cmd = rel ? 'l' : 'L';
        } else if (uc == 'L') {
            float x, y;
            if (!svg_read_num(&s, &x) || !svg_read_num(&s, &y)) break;
            cx = rel ? cx + x : x; cy = rel ? cy + y : y;
            svg_poly_pt(p, (cx - ox) * sx, (cy - oy) * sy);
        } else if (uc == 'H') {
            float x;
            if (!svg_read_num(&s, &x)) break;
            cx = rel ? cx + x : x;
            svg_poly_pt(p, (cx - ox) * sx, (cy - oy) * sy);
        } else if (uc == 'V') {
            float y;
            if (!svg_read_num(&s, &y)) break;
            cy = rel ? cy + y : y;
            svg_poly_pt(p, (cx - ox) * sx, (cy - oy) * sy);
        } else if (uc == 'Q') {
            float x1, y1, x2, y2;
            if (!svg_read_num(&s, &x1) || !svg_read_num(&s, &y1) || !svg_read_num(&s, &x2) || !svg_read_num(&s, &y2)) break;
            if (rel) { x1 += cx; y1 += cy; x2 += cx; y2 += cy; }
            svg_flatten_quad(p, (cx - ox) * sx, (cy - oy) * sy, (x1 - ox) * sx, (y1 - oy) * sy, (x2 - ox) * sx, (y2 - oy) * sy);
            cx = x2; cy = y2;
        } else if (uc == 'C') {
            float x1, y1, x2, y2, x3, y3;
            if (!svg_read_num(&s, &x1) || !svg_read_num(&s, &y1) || !svg_read_num(&s, &x2) || !svg_read_num(&s, &y2) ||
                !svg_read_num(&s, &x3) || !svg_read_num(&s, &y3)) break;
            if (rel) { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x3 += cx; y3 += cy; }
            svg_flatten_cubic(p, (cx - ox) * sx, (cy - oy) * sy, (x1 - ox) * sx, (y1 - oy) * sy,
                              (x2 - ox) * sx, (y2 - oy) * sy, (x3 - ox) * sx, (y3 - oy) * sy, 0);
            cx = x3; cy = y3;
        } else {
            break;
        }
    }
    svg_poly_end_sub(p, 0);
}

static void svg_blend_pre(uint8_t *dst, int idx, float r, float g, float b, float a, int bgra) {
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    uint8_t *p = dst + (size_t)idx * 4;
    float dr = (bgra ? p[2] : p[0]) / 255.0f, dg = p[1] / 255.0f, db = (bgra ? p[0] : p[2]) / 255.0f, da = p[3] / 255.0f;
    float k = 1.0f - a;
    float or_ = r * a + dr * k, og = g * a + dg * k, ob = b * a + db * k, oa = a + da * k;
    uint8_t ur = (uint8_t)(or_ * 255.0f + 0.5f), ug = (uint8_t)(og * 255.0f + 0.5f), ub = (uint8_t)(ob * 255.0f + 0.5f), ua = (uint8_t)(oa * 255.0f + 0.5f);
    p[0] = bgra ? ub : ur; p[1] = ug; p[2] = bgra ? ur : ub; p[3] = ua;
}

typedef struct { float x; int dir; } SvgHit;
static void svg_sort_hits(SvgHit *h, int n) {
    for (int i = 1; i < n; i++) {
        SvgHit v = h[i]; int j = i - 1;
        while (j >= 0 && h[j].x > v.x) { h[j + 1] = h[j]; j--; }
        h[j + 1] = v;
    }
}

/* 8x vertical supersampled scanline fill with exact analytic horizontal coverage */
static void svg_fill_poly(uint8_t *dst, int W, int H, const SvgPoly *p, float r, float g, float b, float alpha, int evenodd, int bgra) {
    if (!p->nsub || alpha <= 0.0f) return;
    int ecap = p->n + p->nsub, ne = 0;
    SvgEdge *edges = (SvgEdge *)malloc((size_t)ecap * sizeof *edges);
    float *cov = (float *)malloc((size_t)W * sizeof *cov);
    SvgHit *hits = (SvgHit *)malloc((size_t)(ecap + 4) * sizeof *hits);
    if (!edges || !cov || !hits) { free(edges); free(cov); free(hits); return; }
    float miny = (float)H, maxy = 0.0f;
    int s0 = 0;
    for (int si = 0; si < p->nsub; si++) {
        int s1 = p->sub_end[si], m = s1 - s0;
        if (m >= 2) {
            for (int i = 0; i < m; i++) {
                SvgPt a = p->pts[s0 + i], c = p->pts[s0 + ((i + 1) % m)];
                if (fabsf(a.y - c.y) < 1e-6f) continue;
                SvgEdge e;
                if (a.y < c.y) { e.x0 = a.x; e.y0 = a.y; e.x1 = c.x; e.y1 = c.y; e.dir = 1; }
                else { e.x0 = c.x; e.y0 = c.y; e.x1 = a.x; e.y1 = a.y; e.dir = -1; }
                if (e.y0 < miny) miny = e.y0;
                if (e.y1 > maxy) maxy = e.y1;
                edges[ne++] = e;
            }
        }
        s0 = s1;
    }
    int y0 = (int)floorf(miny); if (y0 < 0) y0 = 0;
    int y1 = (int)ceilf(maxy); if (y1 > H) y1 = H;
    const int nsamp = 8;
    for (int y = y0; y < y1; y++) {
        memset(cov, 0, (size_t)W * sizeof *cov);
        int row_hit = 0;
        for (int sub = 0; sub < nsamp; sub++) {
            float sy = y + (sub + 0.5f) / (float)nsamp;
            int nh = 0;
            for (int i = 0; i < ne; i++) {
                if (sy >= edges[i].y0 && sy < edges[i].y1) {
                    float t = (sy - edges[i].y0) / (edges[i].y1 - edges[i].y0);
                    hits[nh].x = edges[i].x0 + (edges[i].x1 - edges[i].x0) * t;
                    hits[nh].dir = edges[i].dir;
                    nh++;
                }
            }
            if (nh < 2) continue;
            svg_sort_hits(hits, nh);
            int w = 0;
            for (int i = 0; i + 1 < nh; i++) {
                w += hits[i].dir;
                int inside = evenodd ? (w & 1) : (w != 0);
                if (!inside) continue;
                float xa = hits[i].x, xb = hits[i + 1].x;
                if (xb <= 0.0f || xa >= (float)W || xb <= xa) continue;
                if (xa < 0.0f) xa = 0.0f;
                if (xb > (float)W) xb = (float)W;
                int ix0 = (int)floorf(xa), ix1 = (int)floorf(xb);
                if (ix0 >= W) continue;
                if (ix1 >= W) ix1 = W - 1;
                row_hit = 1;
                if (ix0 == ix1) {
                    cov[ix0] += (xb - xa) / (float)nsamp;
                } else {
                    cov[ix0] += ((float)(ix0 + 1) - xa) / (float)nsamp;
                    for (int x = ix0 + 1; x < ix1; x++) cov[x] += 1.0f / (float)nsamp;
                    cov[ix1] += (xb - (float)ix1) / (float)nsamp;
                }
            }
        }
        if (row_hit) {
            for (int x = 0; x < W; x++) if (cov[x] > 1e-4f) svg_blend_pre(dst, y * W + x, r, g, b, cov[x] * alpha, bgra);
        }
    }
    free(edges); free(cov); free(hits);
}

/* Exact union-of-capsules stroke (2x2 subpixel antialiased; exact round joins & caps, zero double-blending) */
static float svg_seg_dist2(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    float t = l2 > 1e-10f ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    float rx = px - (ax + dx * t), ry = py - (ay + dy * t);
    return rx * rx + ry * ry;
}
static void svg_stroke_poly(uint8_t *dst, int W, int H, const SvgPoly *p, float sw, float r, float g, float b, float alpha, int bgra) {
    if (!p->nsub || sw <= 0.0f || alpha <= 0.0f) return;
    float hw = sw * 0.5f, pad = hw + 1.5f;
    /* build segment list and overall bounding box */
    int scap = p->n + p->nsub, ns = 0;
    float *segs = (float *)malloc((size_t)scap * 4 * sizeof *segs);
    if (!segs) return;
    float bx0 = (float)W, by0 = (float)H, bx1 = 0.0f, by1 = 0.0f;
    int s0 = 0;
    for (int si = 0; si < p->nsub; si++) {
        int s1 = p->sub_end[si], m = s1 - s0;
        if (m >= 2) {
            int nseg = p->sub_closed[si] ? m : m - 1;
            for (int i = 0; i < nseg; i++) {
                SvgPt a = p->pts[s0 + i], c = p->pts[s0 + ((i + 1) % m)];
                segs[ns * 4 + 0] = a.x; segs[ns * 4 + 1] = a.y;
                segs[ns * 4 + 2] = c.x; segs[ns * 4 + 3] = c.y;
                ns++;
                if (a.x < bx0) bx0 = a.x; if (a.x > bx1) bx1 = a.x;
                if (c.x < bx0) bx0 = c.x; if (c.x > bx1) bx1 = c.x;
                if (a.y < by0) by0 = a.y; if (a.y > by1) by1 = a.y;
                if (c.y < by0) by0 = c.y; if (c.y > by1) by1 = c.y;
            }
        }
        s0 = s1;
    }
    int x0 = (int)floorf(bx0 - pad), y0 = (int)floorf(by0 - pad);
    int x1 = (int)ceilf(bx1 + pad), y1 = (int)ceilf(by1 + pad);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    int bw = x1 - x0, bh = y1 - y0;
    if (bw <= 0 || bh <= 0) { free(segs); return; }
    /* per-pixel minimum squared distance at 2x2 subpixel grid (stored as 4 floats per pixel) */
    float *d2 = (float *)malloc((size_t)bw * bh * 4 * sizeof *d2);
    if (!d2) { free(segs); return; }
    float big = (pad + 2.0f) * (pad + 2.0f);
    for (size_t i = 0; i < (size_t)bw * bh * 4; i++) d2[i] = big;
    static const float off[4][2] = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.75f}};
    for (int i = 0; i < ns; i++) {
        float ax = segs[i * 4], ay = segs[i * 4 + 1], bx = segs[i * 4 + 2], by = segs[i * 4 + 3];
        int sx0 = (int)floorf((ax < bx ? ax : bx) - pad), sy0 = (int)floorf((ay < by ? ay : by) - pad);
        int sx1 = (int)ceilf((ax > bx ? ax : bx) + pad), sy1 = (int)ceilf((ay > by ? ay : by) + pad);
        if (sx0 < x0) sx0 = x0; if (sy0 < y0) sy0 = y0; if (sx1 > x1) sx1 = x1; if (sy1 > y1) sy1 = y1;
        for (int y = sy0; y < sy1; y++) {
            for (int x = sx0; x < sx1; x++) {
                float *q = d2 + ((size_t)(y - y0) * bw + (x - x0)) * 4;
                for (int k = 0; k < 4; k++) {
                    float dd = svg_seg_dist2(x + off[k][0], y + off[k][1], ax, ay, bx, by);
                    if (dd < q[k]) q[k] = dd;
                }
            }
        }
    }
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float *q = d2 + ((size_t)(y - y0) * bw + (x - x0)) * 4;
            float cov = 0.0f;
            for (int k = 0; k < 4; k++) {
                float c = 0.5f + (hw - sqrtf(q[k])) * 2.0f;
                if (c > 0.0f) cov += c > 1.0f ? 0.25f : c * 0.25f;
            }
            if (cov > 1e-4f) svg_blend_pre(dst, y * W + x, r, g, b, cov * alpha, bgra);
        }
    }
    free(d2); free(segs);
}

static void svg_draw_circle(uint8_t *dst, int W, int H, float cx, float cy, float rad,
                            int has_fill, float fr, float fg, float fb, float fa,
                            int has_stroke, float sw, float sr, float sg, float sb, float sa, int bgra) {
    float hw = has_stroke ? sw * 0.5f : 0.0f;
    float pad = rad + hw + 1.5f;
    int x0 = (int)floorf(cx - pad), y0 = (int)floorf(cy - pad);
    int x1 = (int)ceilf(cx + pad), y1 = (int)ceilf(cy + pad);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float d = hypotf(x + 0.5f - cx, y + 0.5f - cy);
            if (has_fill && fa > 0.0f) {
                float c = 0.5f + rad - d;
                if (c > 0.0f) svg_blend_pre(dst, y * W + x, fr, fg, fb, (c > 1.0f ? 1.0f : c) * fa, bgra);
            }
            if (has_stroke && sw > 0.0f && sa > 0.0f) {
                float c = 0.5f + hw - fabsf(d - rad);
                if (c > 0.0f) svg_blend_pre(dst, y * W + x, sr, sg, sb, (c > 1.0f ? 1.0f : c) * sa, bgra);
            }
        }
    }
}

static int svg_parse_color(const char *s, float *r, float *g, float *b) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s || !strncmp(s, "none", 4) || !strncmp(s, "transparent", 11)) return 0;
    if (!strcmp(s, "white")) { *r = *g = *b = 1.0f; return 1; }
    if (!strcmp(s, "black")) { *r = *g = *b = 0.0f; return 1; }
    if (s[0] == '#') {
        size_t n = strlen(s + 1);
        unsigned long v = strtoul(s + 1, NULL, 16);
        if (n == 6) { *r = ((v >> 16) & 255) / 255.0f; *g = ((v >> 8) & 255) / 255.0f; *b = (v & 255) / 255.0f; return 1; }
        if (n == 3) { *r = ((v >> 8) & 15) / 15.0f; *g = ((v >> 4) & 15) / 15.0f; *b = (v & 15) / 15.0f; return 1; }
    }
    return 0;
}

static int svg_attr(const char *tag, const char *end, const char *name, char *out, size_t cap) {
    size_t nlen = strlen(name);
    const char *p = tag;
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) p++;
        const char *k0 = p;
        while (p < end && *p != '=' && !isspace((unsigned char)*p) && *p != '>' && *p != '/') p++;
        const char *k1 = p;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != '=') { while (p < end && !isspace((unsigned char)*p)) p++; continue; }
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) break;
        char q = (*p == '"' || *p == '\'') ? *p++ : 0;
        const char *v0 = p;
        while (p < end && (q ? *p != q : !isspace((unsigned char)*p) && *p != '>' && *p != '/')) p++;
        const char *v1 = p;
        if (q && p < end) p++;
        if ((size_t)(k1 - k0) == nlen && !memcmp(k0, name, nlen)) {
            size_t len = (size_t)(v1 - v0);
            if (len >= cap) len = cap - 1;
            memcpy(out, v0, len); out[len] = 0;
            return 1;
        }
    }
    return 0;
}

/* Rasterize an SVG document (bytes, size) into a newly malloc'd premultiplied RGBA (bgra=0) or BGRA (bgra=1) buffer.
 * target_h sets the output height in pixels; output width follows the viewBox aspect ratio (rounded to an even pixel).
 * alpha_scale scales the final premultiplied image alpha (1.0f = unchanged). Returns NULL on failure. */
static uint8_t *svg_rasterize(const void *data, size_t size, int target_h, float alpha_scale, int bgra, int *out_w, int *out_h) {
    if (!data || !size || target_h <= 0) return NULL;
    char *doc = (char *)malloc(size + 1);
    if (!doc) return NULL;
    memcpy(doc, data, size); doc[size] = 0;

    float vbx = 0, vby = 0, vbw = 100, vbh = 100;
    const char *p = doc;
    /* find <svg ...> for viewBox */
    while ((p = strchr(p, '<')) != NULL) {
        if (!strncmp(p, "<!--", 4)) { const char *e = strstr(p + 4, "-->"); p = e ? e + 3 : p + 4; continue; }
        const char *gt = strchr(p, '>');
        if (!gt) break;
        if (!strncmp(p + 1, "svg", 3) && (isspace((unsigned char)p[4]) || p[4] == '>')) {
            char buf[128];
            if (svg_attr(p + 4, gt, "viewBox", buf, sizeof buf)) {
                const char *q = buf;
                svg_read_num(&q, &vbx); svg_read_num(&q, &vby); svg_read_num(&q, &vbw); svg_read_num(&q, &vbh);
            } else {
                if (svg_attr(p + 4, gt, "width", buf, sizeof buf)) vbw = strtof(buf, NULL);
                if (svg_attr(p + 4, gt, "height", buf, sizeof buf)) vbh = strtof(buf, NULL);
            }
            p = gt + 1;
            break;
        }
        p = gt + 1;
    }
    if (vbw <= 0 || vbh <= 0) { free(doc); return NULL; }
    int H = target_h;
    int W = (int)floorf((float)H * (vbw / vbh) + 0.5f);
    if (W < 2) W = 2;
    if (W & 1) W++;
    float sx = (float)W / vbw, sy = (float)H / vbh, sc = (sx + sy) * 0.5f;

    uint8_t *px = (uint8_t *)calloc((size_t)W * H, 4);
    if (!px) { free(doc); return NULL; }

    char buf[4096], abuf[128];
    while (p && (p = strchr(p, '<')) != NULL) {
        if (!strncmp(p, "<!--", 4)) { const char *e = strstr(p + 4, "-->"); p = e ? e + 3 : NULL; continue; }
        const char *gt = strchr(p, '>');
        if (!gt) break;
        const char *t0 = p + 1;
        while (t0 < gt && isspace((unsigned char)*t0)) t0++;
        const char *t1 = t0;
        while (t1 < gt && !isspace((unsigned char)*t1) && *t1 != '/' && *t1 != '>') t1++;
        size_t tlen = (size_t)(t1 - t0);
        int is_path = tlen == 4 && !memcmp(t0, "path", 4);
        int is_circle = tlen == 6 && !memcmp(t0, "circle", 6);
        int is_line = tlen == 4 && !memcmp(t0, "line", 4);
        int is_poly = (tlen == 7 && !memcmp(t0, "polygon", 7)) || (tlen == 8 && !memcmp(t0, "polyline", 8));
        int is_rect = tlen == 4 && !memcmp(t0, "rect", 4);
        if (is_path || is_circle || is_line || is_poly || is_rect) {
            float op = svg_attr(t1, gt, "opacity", abuf, sizeof abuf) ? strtof(abuf, NULL) : 1.0f;
            float fr = 0, fg = 0, fb = 0, fa = op;
            int has_fill = 1;
            if (svg_attr(t1, gt, "fill", abuf, sizeof abuf)) has_fill = svg_parse_color(abuf, &fr, &fg, &fb);
            if (svg_attr(t1, gt, "fill-opacity", abuf, sizeof abuf)) fa *= strtof(abuf, NULL);
            int evenodd = svg_attr(t1, gt, "fill-rule", abuf, sizeof abuf) && !strcmp(abuf, "evenodd");
            float sr = 0, sg = 0, sb = 0, sa = op, sw = 1.0f;
            int has_stroke = 0;
            if (svg_attr(t1, gt, "stroke", abuf, sizeof abuf)) has_stroke = svg_parse_color(abuf, &sr, &sg, &sb);
            if (svg_attr(t1, gt, "stroke-width", abuf, sizeof abuf)) sw = strtof(abuf, NULL);
            if (svg_attr(t1, gt, "stroke-opacity", abuf, sizeof abuf)) sa *= strtof(abuf, NULL);

            if (is_circle) {
                float cx = svg_attr(t1, gt, "cx", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                float cy = svg_attr(t1, gt, "cy", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                float cr = svg_attr(t1, gt, "r", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                svg_draw_circle(px, W, H, (cx - vbx) * sx, (cy - vby) * sy, cr * sc,
                                has_fill, fr, fg, fb, fa, has_stroke, sw * sc, sr, sg, sb, sa, bgra);
            } else {
                SvgPoly poly;
                memset(&poly, 0, sizeof poly);
                if (is_path && svg_attr(t1, gt, "d", buf, sizeof buf)) {
                    svg_parse_path(&poly, buf, vbx, vby, sx, sy);
                } else if (is_line) {
                    float x1 = svg_attr(t1, gt, "x1", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                    float y1 = svg_attr(t1, gt, "y1", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                    float x2 = svg_attr(t1, gt, "x2", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                    float y2 = svg_attr(t1, gt, "y2", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                    svg_poly_pt(&poly, (x1 - vbx) * sx, (y1 - vby) * sy);
                    svg_poly_pt(&poly, (x2 - vbx) * sx, (y2 - vby) * sy);
                    svg_poly_end_sub(&poly, 0);
                    has_fill = 0;
                } else if (is_poly && svg_attr(t1, gt, "points", buf, sizeof buf)) {
                    const char *q = buf; float x, y;
                    while (svg_read_num(&q, &x) && svg_read_num(&q, &y)) svg_poly_pt(&poly, (x - vbx) * sx, (y - vby) * sy);
                    svg_poly_end_sub(&poly, tlen == 7);
                } else if (is_rect) {
                    float rx = svg_attr(t1, gt, "x", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                    float ry = svg_attr(t1, gt, "y", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                    float rw = svg_attr(t1, gt, "width", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                    float rh = svg_attr(t1, gt, "height", abuf, sizeof abuf) ? strtof(abuf, NULL) : 0;
                    svg_poly_pt(&poly, (rx - vbx) * sx, (ry - vby) * sy);
                    svg_poly_pt(&poly, (rx + rw - vbx) * sx, (ry - vby) * sy);
                    svg_poly_pt(&poly, (rx + rw - vbx) * sx, (ry + rh - vby) * sy);
                    svg_poly_pt(&poly, (rx - vbx) * sx, (ry + rh - vby) * sy);
                    svg_poly_end_sub(&poly, 1);
                }
                if (has_fill) svg_fill_poly(px, W, H, &poly, fr, fg, fb, fa, evenodd, bgra);
                if (has_stroke) svg_stroke_poly(px, W, H, &poly, sw * sc, sr, sg, sb, sa, bgra);
                svg_poly_free(&poly);
            }
        }
        p = gt + 1;
    }

    if (alpha_scale < 0.999f) {
        if (alpha_scale < 0.0f) alpha_scale = 0.0f;
        size_t n = (size_t)W * H * 4;
        for (size_t i = 0; i < n; i++) px[i] = (uint8_t)(px[i] * alpha_scale + 0.5f);
    }
    free(doc);
    *out_w = W; *out_h = H;
    return px;
}
