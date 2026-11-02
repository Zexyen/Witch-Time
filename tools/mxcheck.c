/* [witchtime] Offline check of the Maxwell -> Vulkan GLSL translator (runtime/mx_translate.c) on the stages a run dumped
 * (WT_SHADERDUMP_ALL=<dir>: one s<program>_<index>.mxd per stage, the game's own code: keep the folder out of the repo).
 * Every stage (vertex, fragment, tessellation control / evaluation, compute) is translated as nvn_vk_draw.c does (target 1, its bindings) and compiled by glslang to SPIR-V; the failures
 * are counted by instruction / glslang message.
 *
 *   clang -O1 -I runtime tools/mxcheck.c runtime/mx_translate.c -lglslang -lglslang-default-resource-limits -o build/mxcheck
 *   build/mxcheck <dir> [-v] [-o <dir for the GLSL of failing stages>]
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include "mx_translate.h"

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int compile(int stage, const char *src, char *log, size_t n) {
    glslang_input_t in;
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = stage == 0 ? GLSLANG_STAGE_VERTEX : stage == 1 ? GLSLANG_STAGE_FRAGMENT : stage == 3 ? GLSLANG_STAGE_TESSCONTROL : stage == 4 ? GLSLANG_STAGE_TESSEVALUATION : GLSLANG_STAGE_COMPUTE;
    in.client = GLSLANG_CLIENT_VULKAN; in.client_version = GLSLANG_TARGET_VULKAN_1_3;
    in.target_language = GLSLANG_TARGET_SPV; in.target_language_version = GLSLANG_TARGET_SPV_1_6;
    in.code = src; in.default_version = 460; in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT | GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT;
    in.resource = glslang_default_resource();
    glslang_shader_t *sh = glslang_shader_create(&in);
    int ok = glslang_shader_preprocess(sh, &in) && glslang_shader_parse(sh, &in);
    if (!ok) snprintf(log, n, "%s", glslang_shader_get_info_log(sh));
    else {
        glslang_program_t *pr = glslang_program_create();
        glslang_program_add_shader(pr, sh);
        ok = glslang_program_link(pr, in.messages);
        if (!ok) snprintf(log, n, "%s", glslang_program_get_info_log(pr));
        glslang_program_delete(pr);
    }
    glslang_shader_delete(sh);
    return ok;
}

#define MAXK 256
static char g_key[MAXK][120];
static int g_cnt[MAXK], g_nk;
static void count(const char *k) {
    for (int i = 0; i < g_nk; i++) if (!strcmp(g_key[i], k)) { g_cnt[i]++; return; }
    if (g_nk < MAXK) { snprintf(g_key[g_nk], sizeof g_key[0], "%s", k); g_cnt[g_nk++] = 1; }
}

int main(int argc, char **argv) {
    static const int8_t stage_of_type[16] = {-1, 0, 3, 4, 2, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    const char *dir = argc > 1 ? argv[1] : ".", *outdir = NULL;
    int verbose = 0, total = 0, ok_n = 0, skipped = 0;
    for (int a = 2; a < argc; a++) { if (!strcmp(argv[a], "-v")) verbose = 1; else if (!strcmp(argv[a], "-o") && a + 1 < argc) outdir = argv[++a]; }
    glslang_initialize_process();
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return 2; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strstr(de->d_name, ".mxd")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)sz);
        if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); continue; }
        fclose(f);
        uint32_t ctl_len = rd32(b + 12), data_len = rd32(b + 16);
        const uint8_t *ctl = b + 20, *data = ctl + ctl_len;
        if (20 + ctl_len + data_len > (uint32_t)sz || data_len < 0x80 || rd32(ctl + 0x18)) { skipped++; free(b); continue; }   /* no code, or assembly text */
        MxIn in;
        MxOut out;
        memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
        uint32_t len = rd32(ctl + 0x6f8), csize = rd32(ctl + 0x6fc), coff = rd32(ctl + 0x700), off = rd32(ctl + 0x708);
        int s;
        if (rd32(ctl + 0x714) == 5) { s = 5; in.code = data + off; }
        else { s = rd32(data) == 0x12345678u ? stage_of_type[(rd32(data + 0x30) >> 10) & 15] : -1; in.code = data + 0x30; }
        if (s < 0 || s == 2 || off + len > data_len) { skipped++; free(b); continue; }
        in.stage = s; in.code_len = (int)len;
        if (csize && coff && coff + csize <= data_len) { in.cdata = data + coff; in.cdata_size = csize; }
        if (s == 4) { in.tess_prim = rd32(ctl + 0x738); in.tess_spacing = rd32(ctl + 0x73c); in.tess_point = rd32(ctl + 0x740); in.tess_order = rd32(ctl + 0x744); }
        in.target = 1; in.binding_base = 40u * (unsigned)s; in.ssbo_base = 40u * (unsigned)s + 20; in.tex_base = 40u * (unsigned)s;
        in.image_base = 40u * (unsigned)s + 32; in.wmask_binding = 40u * (unsigned)s + 38;
        if (s == 5) {
            in.binding_base = 0; in.ssbo_base = 20; in.tex_base = 0; in.image_base = 32; in.wmask_binding = 38; in.track_writes = 1;
            for (int k = 0; k < 3; k++) in.local_size[k] = rd32(ctl + 0x748 + 4 * k);
            in.smem_bytes = rd32(ctl + 0x754); in.lmem_bytes = rd32(ctl + 0x758);
        }
        total++;
        char why[512];
        if (!mx_translate(&in, &out)) {
            snprintf(why, sizeof why, "translate: %s", out.op[0] ? out.op : out.why);
            count(why);
            if (verbose) printf("%s stage %d: %s\n", de->d_name, s, out.why);
        } else {
            char log[4096];
            if (out.images) { char k[64]; snprintf(k, sizeof k, "uses images (stage %d)", s); count(k); }
            if (compile(s, out.glsl, log, sizeof log)) ok_n++;
            else {
                char *e = strstr(log, "ERROR: "), *nl;
                snprintf(why, sizeof why, "glslang: %.100s", e ? e + 7 : log);
                if ((nl = strchr(why, '\n'))) *nl = 0;
                { int a_, b_, used = 0; if (sscanf(why + 9, "%d:%d: %n", &a_, &b_, &used) == 2 && used) memmove(why + 9, why + 9 + used, strlen(why + 9 + used) + 1); }   /* drop "0:123: " */
                count(why);
                if (verbose) printf("%s stage %d: %s\n", de->d_name, s, e ? e : log);
                if (outdir) { char p2[1024]; snprintf(p2, sizeof p2, "%s/%s.glsl", outdir, de->d_name); FILE *g = fopen(p2, "w"); if (g) { fputs(out.glsl, g); fclose(g); } }
            }
            free(out.glsl);
        }
        free(b);
    }
    closedir(d);
    printf("%d stages: %d translated and compiled, %d failed (%d files skipped: no code / assembly text / other stages)\n", total, ok_n, total - ok_n, skipped);
    for (int i = 0; i < g_nk; i++) for (int j = i + 1; j < g_nk; j++) if (g_cnt[j] > g_cnt[i]) { int t = g_cnt[i]; g_cnt[i] = g_cnt[j]; g_cnt[j] = t; char k[120]; memcpy(k, g_key[i], 120); memcpy(g_key[i], g_key[j], 120); memcpy(g_key[j], k, 120); }
    for (int i = 0; i < g_nk; i++) printf("  %6d  %s\n", g_cnt[i], g_key[i]);
    glslang_finalize_process();
    return total != ok_n;
}
