/* Self-test of the PC settings menu without the game: graphics.ini rewritten in place (gfx_option_set, gfx_ini_get) and the
 * panel drawn at two window sizes (pcmenu.c), the pictures written as BMP into the folder given (for a look).
 *   clang -D_GNU_SOURCE -D'__declspec(x)=' -Iruntime/posix -Iruntime -Iruntime/gen tests/pcmenu_selftest.c runtime/gfx_options.c \
 *         runtime/plat_posix.c -lm -o /tmp/pcmenu_selftest && /tmp/pcmenu_selftest /tmp/pcmenu_test */
#include "../runtime/pcmenu.c"
#include <assert.h>
#include <sys/stat.h>

volatile LONG g_guest_frozen, g_display_close;
volatile int game_cutscene_fps;
volatile int game_hud_centre;
static int g_vsync_applied, g_win_applied;
void nnhid_menu_read(uint64_t *b, int *lx, int *ly) { *b = 0; *lx = *ly = 0; }
int nnhid_menu_key(int vk) { (void)vk; return 0; }
int nvn_front_post_pcmenu(void) { return 1; }
void display_vsync_apply(void) { g_vsync_applied++; }
int display_window_apply(void) { g_win_applied++; return 1; }
void display_mouse_want(int on) { (void)on; }
int nnpad_mouse_cfg(float *s, int *i) { (void)s; (void)i; return 0; }
static const char *g_prompts = "playstation";
const char *nnpad_prompts_setting(void) { return g_prompts; }
int nnpad_prompts_set(const char *val) { g_prompts = val; return 1; }
int display_mouse_pixels(int *x, int *y) { *x = *y = -1; return 0; }
SHORT GetAsyncKeyState(int vk) { (void)vk; return 0; }

static char *slurp(const char *path) { FILE *f = fopen(path, "rb"); static char b[16384]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; return strdup(b); }
static void spit(const char *path, const char *s) { FILE *f = fopen(path, "wb"); fputs(s, f); fclose(f); }
static void save_bmp(const char *path, const uint8_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    uint8_t head[54] = {'B', 'M'};
    uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
    memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
    fwrite(head, 1, 54, f);
    uint8_t *r = calloc(1, row);
    for (int y = h - 1; y >= 0; y--) { for (int x = 0; x < w; x++) memcpy(r + x * 3, px + ((size_t)y * w + x) * 4, 3); fwrite(r, 1, row, f); }
    free(r); fclose(f);
}
static void panel(const char *dir, const char *name, int W, int H) {
    int r[4]; unsigned gen;
    assert(pcmenu_panel_size(W, H, r, &gen));
    assert(r[0] >= 0 && r[1] >= 0 && r[0] + r[2] <= W && r[1] + r[3] <= H && r[2] > 100 && r[3] > 100);
    uint8_t *px = malloc((size_t)r[2] * r[3] * 4);
    pcmenu_panel_read(px, r[2], r[3]);
    char path[300]; snprintf(path, sizeof path, "%s/%s.bmp", dir, name);
    save_bmp(path, px, r[2], r[3]);
    printf("panel %dx%d in a %dx%d window: %s\n", r[2], r[3], W, H, path);
    free(px);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    char ini[300], buf[64];
    snprintf(ini, sizeof ini, "%s/graphics_test.ini", dir);
    mkdir(dir, 0755);
    /* a file of CRLF lines with comments, an unknown key, a key twice, no newline at the end */
    spit(ini, "; comment = not a key\r\nwindow = windowed\r\nvsync   =   auto   \r\nmystery = 7\r\naspect_ratio = 21:9\r\nhud_area = 16:9\r\n[section]\r\nvsync = clock\r\nresolution_scale = 1.0");
    setenv("SSBU_GRAPHICS_CONFIG", ini, 1);
    assert(gfx_options()->present == 0);                   /* the last vsync line wins: clock */
    assert(gfx_ini_get("vsync", buf, sizeof buf) && !strcmp(buf, "clock"));
    assert(fabsf(gfx_options()->aspect_ratio - 21.0f / 9.0f) < 1e-5f && fabs(atof(getenv("RECOMPS_ASPECT")) - 21.0 / 9.0) < 1e-5);      /* <w>:<h>, handed on */
    assert(gfx_options()->hud_centre == 1 && !strcmp(getenv("RECOMPS_HUD_CENTRE"), "1"));
    assert(!gfx_ini_get("aspect", buf, sizeof buf));
    assert(gfx_option_set("vsync", "off") && gfx_options()->present == 2);
    char *s = slurp(ini);
    assert(!strcmp(s, "; comment = not a key\r\nwindow = windowed\r\nvsync   =   off   \r\nmystery = 7\r\naspect_ratio = 21:9\r\nhud_area = 16:9\r\n[section]\r\nvsync = off\r\nresolution_scale = 1.0"));
    free(s);
    assert(gfx_option_set("aspect", "stretch") && gfx_options()->aspect_stretch == 1);
    s = slurp(ini);
    assert(strstr(s, "resolution_scale = 1.0\r\naspect = stretch\r\n") && strstr(s, "mystery = 7\r\n"));
    free(s);
    assert(gfx_option_set("resolution_scale", "2.4") && gfx_options()->scale == 1.0f);      /* not a live key: the running value stays */
    spit(ini, "window = windowed\nwidth = 1280\nheight = 720\nvsync = auto\nresolution_scale = 1.6\nanisotropic_filtering = 8\n");      /* LF file */
    assert(gfx_option_set("width", "1920") && gfx_option_set("height", "1080") && gfx_options()->width == 1920 && gfx_options()->height == 1080);
    s = slurp(ini);
    assert(!strcmp(s, "window = windowed\nwidth = 1920\nheight = 1080\nvsync = auto\nresolution_scale = 1.6\nanisotropic_filtering = 8\n"));
    free(s);
    /* opening from the pad: Start + Select together, not one of them and not the sticks */
    assert(pcmenu_gate(0, 0x30) == 0 && pcmenu_gate(0, 0) == 0 && pcmenu_gate(0, 1u << 10) == 0 && pcmenu_gate(0, 1u << 11) == 0 && !g_open);
    assert(pcmenu_gate(0, 0) == 0 && pcmenu_gate(0, 0xc00) == 1 && g_open);
    InterlockedExchange(&g_open, 0); Sleep(200);
    /* the menu's rows from that file; changes through the menu */
    menu_open("test");                                    /* (its thread runs on with the stubs' idle input) */
    assert(g_cur[5] == 7 && !strcmp(val_name(5), "1.6 \xE2\x80\x94 2560 \xC3\x97 1440"));
    assert(g_shown[7] && g_cur[7] == 2);                   /* texture_lod_bias: not in the file, its default 0 */
    change(7, -1, 0); assert(gfx_options()->texture_lod_bias == -0.5f && (g_work & 1) && !row_restart(7));
    change(2, 1, 0); assert(gfx_options()->present == 4 && g_vsync_applied == 1 && (g_work & 2));      /* auto -> display */
    change(3, 1, 0); assert(game_cutscene_fps == 60);
    change(5, 1, 0); assert(row_restart(5));                  /* 1.6 -> 2.0: applies at the next start */
    change(6, -1, 0); assert(gfx_options()->aniso == 4 && (g_work & 1));
    change(5, 1, 0); change(5, 1, 0); change(5, 1, 0); assert(g_cur[5] == 10);      /* 3.0, the last: right stops there */
    change(5, 1, 1); assert(g_cur[5] == 0); change(5, 1, 0); change(5, 1, 0); change(5, 1, 0); change(5, 1, 0); change(5, 1, 0);      /* A goes round */
    g_sel = 5; g_ver++;
    panel(dir, "pcmenu_1280x720", 1280, 720);
    panel(dir, "pcmenu_3840x2160", 3840, 2160);
    g_sel = 2; g_ver++;
    panel(dir, "pcmenu_1920x1080", 1920, 1080);
    /* closing: every player reads an idle pad until that player has let go (player 2 = a keyboard holding Esc = Plus) */
    InterlockedExchange(&g_open, 0); Sleep(200);           /* the menu's thread ends: what it does at a close */
    assert(g_release == 0xff && !g_guest_frozen);
    assert(pcmenu_gate(1, 1u << 10) == 1);                 /* player 2 still holds Plus: not given */
    assert(pcmenu_gate(0, 0) == 0);                        /* player 1 let go: its input is back at once */
    assert(pcmenu_gate(1, 1u << 10) == 1);                 /* player 1's release does not free player 2 */
    assert(pcmenu_gate(1, 0) == 0 && pcmenu_gate(1, 1u << 10) == 0);      /* let go, then a new press reaches the game */
    assert(pcmenu_gate(0, 1u << 0) == 0);
    /* the game's Options rows (options_row.c): by key, through the same change path */
    int pending;
    pcmenu_key_set("aspect", "keep"); assert(gfx_options()->aspect_stretch == 0 && !strcmp(pcmenu_key_get("aspect", NULL), "keep"));
    pcmenu_key_set("anisotropic_filtering", "16"); assert(gfx_options()->aniso == 16 && !strcmp(pcmenu_key_get("anisotropic_filtering", NULL), "16"));
    pcmenu_key_set("resolution_scale", "1.6"); assert(!strcmp(pcmenu_key_get("resolution_scale", &pending), "1.6") && !pending);      /* the running one */
    pcmenu_key_set("resolution_scale", "2.4"); assert(!strcmp(pcmenu_key_get("resolution_scale", &pending), "2.4") && pending);
    pcmenu_key_set("resolution_scale", "7"); assert(!strcmp(pcmenu_key_get("resolution_scale", NULL), "2.4"));      /* none of the row's */
    s = slurp(ini); assert(strstr(s, "resolution_scale = 2.4\n") && strstr(s, "anisotropic_filtering = 16\n")); free(s);
    assert(!strcmp(pcmenu_key_get("no_such_key", NULL), ""));
    /* a new version over a player's file: its comments and keys, the player's values and keys, the old file kept */
    char old[320], bak[330];
    snprintf(old, sizeof old, "%s/graphics_old.ini", dir); snprintf(bak, sizeof bak, "%s.bak", old);
    const char *player = "; my note\r\n;   The small \"PC SETTINGS  F1 L3 + R3\" note\r\nvsync = off\r\nVSYNC = on\r\nmystery = 7\r\nwindow=borderless\r\nanisotropic_filtering = 8";
    remove(old);
    assert(gfx_ini_refresh(old) == 0);                     /* no file: nothing to do */
    spit(old, player);
    assert(gfx_ini_refresh(old) == 1);
    s = slurp(old);
    assert(strstr(s, "Start + Select") && !strstr(s, "L3 + R3") && !strstr(s, "my note"));
    assert(strstr(s, "\r\nvsync = on\r\n") && strstr(s, "\r\nwindow = borderless\r\n") && strstr(s, "\r\nanisotropic_filtering = 8\r\n"));      /* the last line with a key won */
    assert(strstr(s, "\r\nhdr = auto\r\n") && strstr(s, "\r\n\r\nmystery = 7\r\n") && !strcmp(slurp(bak), player));
    assert(gfx_ini_refresh(old) == 0 && !strcmp(slurp(old), s));      /* once */
    spit(old, "; my note\r\n"); { FILE *f = fopen(old, "ab"); fputs(s, f); fclose(f); }
    assert(gfx_ini_refresh(old) == 0 && strstr(slurp(old), "my note"));      /* the player's comment stays while the defaults stand */
    s = slurp("config/graphics.ini");                      /* the shipped file: LF, and nothing to do at a new install's first start */
    assert(!strstr(s, "\r\n"));
    spit(old, s);
    assert(gfx_ini_refresh(old) == 0);
    { char *d = strstr(s, "\nvsync = auto\n") + 1; memmove(d, d + 13, strlen(d + 13) + 1); }      /* a key gone: LF stays */
    spit(old, s);
    assert(gfx_ini_refresh(old) == 1 && !strstr(slurp(old), "\r\n") && strstr(slurp(old), "\nvsync = auto\n"));
    printf("pcmenu self-test: all checks passed\n");
    return 0;
}
