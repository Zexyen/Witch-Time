/* [witchtime] PC settings in the game's own Options screen (page Screen Display): eight rows of the game's own kind that show
 * and change keys of graphics.ini, and a "PC Settings" row that opens the PC settings panel (pcmenu.c) for the rest.
 * docs/RE_NOTES.md section 8 has the Options screen as found in main.
 *
 * The screen's rows are objects behind a table of pointers (ROWS: 25 rows, slot 25 is 0); a page shows rows by number,
 * Screen Display the 7 of a table in main's read-only data. The lift (lifter/recomp_eh.py, _options_row_patches) reads that
 * page's numbers and the row table from OPTX_BASE instead, a page mapped here after main's image:
 *   +0x000  Screen Display's row numbers: 18 .. 23, our rows 26 .. 33, 25, Restore Defaults (24); +0xffc their count
 *   +0x100  the row table: the game's 25, row 25, rows 26 .. 33
 *   +0x300  row 25's method table: the Brightness row's (an action row: label, Confirm / Back, no value) with two methods of
 *           ours, the label (MENU_OPTION_LIST_PC_SETTINGS; without the text mod the pause menu's "Options") and "decide",
 *           which opens the panel where the Brightness row opens its screen
 *   +0x360  rows 26 .. 33's method table: the Display Language row's (a list row: left / right go round its values, with
 *           the game's sounds; a value is a message of a bank, the text by hash) with its value methods ours (+0x38 ..)
 *   +0x400  the row objects (8 bytes: the method table)
 * The texts (labels in the menu bank, values in the core bank, where the digits are) are added to the game's messages by
 * scripts/pc_settings_mod.py; the names here are theirs (the internal resolution's and the frame rate's labels say they apply at the next start; the frame rate's that it is experimental). A value of graphics.ini that is none of a row's is "Custom" (one
 * more value, until left / right leave it). The game's Restore Defaults does not touch these rows (its reset is the game's
 * own settings); the list row's reset button sets the row's default. Values change through pcmenu.c (pcmenu_key_set), so
 * the file is written and the live keys applied as from the panel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"

#define OPTX_BASE   0x7105a00000ULL      /* lifter/recomp_eh.py OPTX_PAGE */
#define ROWS        0x7101c2b1a8ULL      /* the Options screen's row objects */
#define SCREEN_ROWS 0x71018511bcULL      /* Screen Display's 7 row numbers (18 .. 24) in main */
#define BRIGHTNESS  18
#define LANGUAGE    19                   /* Display Language: the list row copied */
#define PC_ROW      25
#define FIRST       26                   /* our list rows */
#define BANK_CORE   0x6b8d854fu          /* crc32("core") & 0x7fffffff: messcore.mcd (the menu bank: "menu") */
#define PC_LABEL_ALT 0x5c8952aeu         /* PAUSEMENU_CATEGORY_002, the pause menu's "Options": when the label's text is missing */
#define DECIDE_MASK 0x7100983930ULL      /* -> w0: the pad bits of "confirm" (A, or B with the buttons swapped) */
#define TRIGGERED   0x7100983660ULL      /* (w0 bits) -> w0: pressed this frame */
#define MENU_SOUND  0x710095a110ULL      /* (w0 sound): 2 confirm, 8 refused */
#define LIST_BUILD  0x7100a32390ULL      /* (x0 screen): the page's rows filled again (what Restore Defaults does after its reset) */

void pcmenu_open_game(void);             /* pcmenu.c */
int pcmenu_is_open(void);
const char *pcmenu_key_get(const char *key, int *pending);
void pcmenu_key_set(const char *key, const char *val);
int fs_rom_mod_serves(const char *rel, const char *mod);      /* nnfs.c */

typedef struct {
    const char *key, *label, *val[6], *text[6];      /* text: the value's message */
    int dflt;
} PcRow;
static const PcRow k_pc[] = {
    {"resolution_scale", "MENU_OPTION_LIST_PC_RES", {"1.0", "1.2", "1.6", "2.4"}, {"CORE_PC_RES_10", "CORE_PC_RES_12", "CORE_PC_RES_16", "CORE_PC_RES_24"}, 0},
    {"cutscene_fps", "MENU_OPTION_LIST_PC_CUTSCENE", {"30", "60"}, {"CORE_PC_FPS_30", "CORE_PC_FPS_60"}, 0},
    {"frame_rate", "MENU_OPTION_LIST_PC_FPS", {"60", "90", "120"}, {"CORE_PC_FPS_60", "CORE_PC_FPS_90", "CORE_PC_FPS_120"}, 0},
    {"vsync", "MENU_OPTION_LIST_PC_VSYNC", {"auto", "display", "clock", "on", "off"}, {"CORE_PC_AUTO", "CORE_PC_MONITOR", "CORE_PC_TIMER", "CORE_SETTING_003", "CORE_SETTING_004"}, 0},
    {"window", "MENU_OPTION_LIST_PC_WINDOW", {"windowed", "borderless", "fullscreen"}, {"CORE_PC_WINDOWED", "CORE_PC_BORDERLESS", "CORE_PC_FULLSCREEN"}, 0},
    {"anisotropic_filtering", "MENU_OPTION_LIST_PC_ANISO", {"game", "2", "4", "8", "16"}, {"CORE_PC_DEFAULT", "CORE_PC_AF_2", "CORE_PC_AF_4", "CORE_PC_AF_8", "CORE_PC_AF_16"}, 0},
    {"texture_lod_bias", "MENU_OPTION_LIST_PC_TEXTURE", {"-1", "-0.5", "0", "0.5", "1"}, {"CORE_PC_LOD_M1", "CORE_PC_LOD_M05", "CORE_PC_LOD_0", "CORE_PC_LOD_P05", "CORE_PC_LOD_P1"}, 2},
    {"aspect_ratio", "MENU_OPTION_LIST_PC_ASPECT", {"16:9", "auto"}, {"CORE_PC_FIXED", "CORE_PC_UNLOCKED"}, 0},      /* [witchtime] aspect ratios: Fixed / Unlocked */
    {"hud_area", "MENU_OPTION_LIST_PC_HUD", {"screen", "16:9"}, {"CORE_PC_HUD_FULL", "CORE_PC_HUD_CENTER"}, 0},      /* [witchtime] aspect ratios */
};
#define NPC ((int)(sizeof k_pc / sizeof k_pc[0]))
static uint32_t g_label[NPC], g_text[NPC][6], g_custom;
static int g_text_mod;
static uint64_t g_obj0;                  /* the guest address of row 26's object */

static uint32_t msg_hash(const char *name, const char *suffix) {      /* crc32 of the lower-case name, as the MCD's events */
    char s[64];
    uint32_t c = ~0u;
    snprintf(s, sizeof s, "%s%s", name, suffix ? suffix : "");
    for (const char *p = s; *p; p++) {
        c ^= (uint8_t)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1));
    }
    return ~c & 0x7fffffff;
}

static void pc_row_label(Ctx *c) { c->x[0] = g_text_mod ? msg_hash("MENU_OPTION_LIST_PC_SETTINGS", NULL) : PC_LABEL_ALT; }
/* method +0x08 (row, screen, row state, widget) -> 1 = the input is used; called every frame for the row under the cursor */
static int g_panel;                      /* the panel was opened from the row: the rows show its changes when it closes */
static void pc_row_decide(Ctx *c) {
    uint64_t screen = c->x[1], state = c->x[2], lr = c->x[30];
    if (g_panel && !pcmenu_is_open()) { g_panel = 0; c->x[0] = screen; call_guest(c, LIST_BUILD); c->x[1] = screen; c->x[2] = state; }
    call_guest(c, DECIDE_MASK);
    call_guest(c, TRIGGERED);
    int on = (uint32_t)c->x[0] != 0, greyed = on && rd32(state + 0x28);
    if (on) { c->x[0] = greyed ? 8 : 2; call_guest(c, MENU_SOUND); }
    if (on && !greyed) { pcmenu_open_game(); g_panel = 1; }
    c->x[0] = on && !greyed;
    c->x[30] = lr;
}

/* the list rows' value methods; x0 = the row object */
static int nval(int r) { int n = 0; while (n < 6 && k_pc[r].val[n]) n++; return n; }
static int row_of(Ctx *c) { return (int)((c->x[0] - g_obj0) / 8); }
static int cur_val(int r) {                              /* the value's index; nval(r) = "Custom" */
    const char *v = pcmenu_key_get(k_pc[r].key, NULL);
    int n = nval(r), k = 0;
    while (k < n && strcmp(k_pc[r].val[k], v)) k++;
    return k;
}
static void pc_list_label(Ctx *c) { c->x[0] = g_label[row_of(c)]; }                                  /* +0x38 */
static void pc_list_get(Ctx *c) { c->x[0] = (uint32_t)cur_val(row_of(c)); }                  /* +0x40 */
static void pc_list_set(Ctx *c) {                                                                 /* +0x48 (w1 value) */
    int r = row_of(c), k = (int)(uint32_t)c->x[1];
    if (k >= 0 && k < nval(r)) pcmenu_key_set(k_pc[r].key, k_pc[r].val[k]);
}
static void pc_list_bank(Ctx *c) { c->x[0] = BANK_CORE; }                                         /* +0x50 */
static void pc_list_text(Ctx *c) { int r = row_of(c), k = (int)(uint32_t)c->x[1]; c->x[0] = k >= 0 && k < nval(r) ? g_text[r][k] : g_custom; }      /* +0x58 (w1 value) */
static void pc_list_count(Ctx *c) { int r = row_of(c); c->x[0] = (uint32_t)(nval(r) + (cur_val(r) == nval(r))); }      /* +0x60 */
static void pc_list_reset(Ctx *c) { int r = row_of(c); pcmenu_key_set(k_pc[r].key, k_pc[r].val[k_pc[r].dflt]); }        /* +0x68 */

void options_row_install(void) {
    uint8_t *page = plat_map_fixed(OPTX_BASE, 0x1000);
    if (!page) { fprintf(stderr, "options row: cannot map 0x%llx (the Options screen reads its Screen Display rows there)\n", OPTX_BASE); exit(1); }
    uint32_t *rows = (uint32_t *)page;
    uint64_t *table = (uint64_t *)(page + 0x100), *obj = (uint64_t *)(page + 0x400);
    for (int i = 0; i < PC_ROW; i++) table[i] = rd64(ROWS + 8 * (uint64_t)i);
    uint64_t bright = table[BRIGHTNESS], vt = bright ? rd64(bright) : 0, lang = table[LANGUAGE], lvt = lang ? rd64(lang) : 0;
    int known = rd64(ROWS + 8 * PC_ROW) == 0 && vt && rd64(vt + 0x38) == GUEST_BASE + 0xa72c2c       /* Brightness's label method */
             && lvt && rd64(lvt) == GUEST_BASE + 0xa725ac && rd64(lvt + 0x10) == GUEST_BASE + 0xa72640      /* a list row: fill, decide */
             && rd64(lvt + 0x50) == GUEST_BASE + 0xa72ce0;                                            /* its bank method */
    for (int i = 0; i < 7; i++) known &= rd32(SCREEN_ROWS + 4 * i) == (uint32_t)(BRIGHTNESS + i);
    int n = 0;
    for (int i = 0; i < 6; i++) rows[n++] = (uint32_t)(BRIGHTNESS + i);
    if (!known) {                            /* not known: the game's 7 */
        rows[n++] = BRIGHTNESS + 6;
        rows[0x3ff] = (uint32_t)n;
        printf("options row: the Options screen's rows are not the ones known: no PC settings rows\n");
        return;
    }
    g_text_mod = fs_rom_mod_serves("ui_000.pkz", "witchtime-pc-settings");
    if (!g_text_mod)
        printf("options row: the rows' text is not there (mods/witchtime-pc-settings: python scripts/setup.py --only text): "
               "only the \"PC Settings\" row (called \"Options\"), which opens the panel\n");
    uint64_t *mvt = (uint64_t *)(page + 0x310), *lmvt = (uint64_t *)(page + 0x370);      /* each after the table's two header words, 0 */
    for (int i = 0; i < 9; i++) mvt[i] = rd64(vt + 8 * (uint64_t)i);
    mvt[1] = hostfn_address("options row: PC Settings, decide", pc_row_decide);
    mvt[7] = hostfn_address("options row: PC Settings, label", pc_row_label);
    obj[0] = (uint64_t)(uintptr_t)mvt;
    table[PC_ROW] = (uint64_t)(uintptr_t)&obj[0];
    if (g_text_mod) {                        /* without the text the rows would show no words: the panel has them all */
        static void (*const fn[7])(Ctx *) = {pc_list_label, pc_list_get, pc_list_set, pc_list_bank, pc_list_text, pc_list_count, pc_list_reset};
        static const char *const what[7] = {"label", "get", "set", "bank", "value text", "count", "reset"};
        for (int i = 0; i < 14; i++) lmvt[i] = rd64(lvt + 8 * (uint64_t)i);
        for (int i = 0; i < 7; i++) { char nm[64]; snprintf(nm, sizeof nm, "options row: list row, %s", what[i]); lmvt[7 + i] = hostfn_address(_strdup(nm), fn[i]); }
        g_obj0 = (uint64_t)(uintptr_t)&obj[1];
        for (int r = 0; r < NPC; r++) {
            obj[1 + r] = (uint64_t)(uintptr_t)lmvt;
            table[FIRST + r] = (uint64_t)(uintptr_t)&obj[1 + r];
            rows[n++] = (uint32_t)(FIRST + r);
            g_label[r] = msg_hash(k_pc[r].label, NULL);
            for (int k = 0; k < nval(r); k++) g_text[r][k] = msg_hash(k_pc[r].text[k], NULL);
        }
        g_custom = msg_hash("CORE_PC_CUSTOM", NULL);
    }
    rows[n++] = PC_ROW;
    rows[n++] = BRIGHTNESS + 6;              /* Restore Defaults */
    rows[0x3ff] = (uint32_t)n;
}
