/* [witchtime] Recomp Achievements integration (github.com/elripalda/RecompAchievements).
 *
 * Exposes Witch Time's achievement set (story chapter clears, difficulty completion, and Platinum mastery;
 * docs/ACHIEVEMENTS.md, docs/RE_NOTES.md section 12) to RecompLauncher over RL_SESSION.
 *
 * Started outside RecompLauncher (no RL_SESSION in the environment), everything is a no-op, so this is safe
 * to ship in every build. The list itself (titles, points) is hosted by RecompLauncher from docs/achievements.json,
 * an "events" list: an unlock goes out as its row's event name (wt_ach_defs). */
#include "achievements.h"
#include "presence_names.h"
#include "rl_achievements.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WT_VERSION
#define WT_VERSION ""
#endif

/* game_shim_eh.c: {chapter (-1: menus), difficulty (0..3), verse (0: none), area (0: none)} */
__declspec(dllimport) extern volatile int game_presence[4];

static volatile int g_ach_enabled;         /* RL_SESSION is present */
static volatile int g_ach_inited;          /* rl_ach_init ran */
static volatile int g_ach_pending_save;    /* save_loaded happened before rl_ach_init */
static volatile int g_ach_save_grace;      /* frames after a save read during which unlocks come from the save */
static unsigned g_ach_frame;
static int g_ach_rp_chapter = -2, g_ach_rp_area;

/* Strip a leading 'v' from release tags (e.g. "v0.15.0" -> "0.15.0") so RecompLauncher's semver check matches. */
static const char *ach_version_string(const char *raw) {
    if (!raw || !raw[0]) return "";
    if (raw[0] == 'v' && raw[1] >= '0' && raw[1] <= '9') return raw + 1;
    if (raw[0] >= '0' && raw[0] <= '9') return raw;
    return "";
}

int achievements_format_presence(char *out, size_t n, const int g[4]) {
    const char *chapter = NULL;
    for (size_t i = 0; i < sizeof wt_chapters / sizeof *wt_chapters; i++)
        if (wt_chapters[i].id == g[0]) { chapter = wt_chapters[i].name; break; }
    if (!chapter) return snprintf(out, n, "In the menus");
    char verse[20] = "";
    if (g[2] > 0) snprintf(verse, sizeof verse, " - Verse %d", g[2]);
    const char *difficulty = g[1] >= 0 && g[1] < 4 ? wt_difficulties[g[1]] : "";
    const char *area = g[3] > 0 && g[3] < 26 ? wt_areas[g[3]] : NULL;
    if (difficulty[0] && area)
        return snprintf(out, n, "%s%s (%s - %s)", chapter, verse, difficulty, area);
    if (difficulty[0])
        return snprintf(out, n, "%s%s (%s)", chapter, verse, difficulty);
    if (area)
        return snprintf(out, n, "%s%s (%s)", chapter, verse, area);
    return snprintf(out, n, "%s%s", chapter, verse);
}

static void ach_ensure_init(void) {
    if (!g_ach_enabled || g_ach_inited) return;
    g_ach_inited = 1;
    rl_ach_set_info("witch-time", ach_version_string(WT_VERSION));
    int connected = rl_ach_init("switch", NULL, NULL);
    if (connected) {
        printf("achievements: connected to RecompLauncher\n");
        if (__atomic_exchange_n(&g_ach_pending_save, 0, __ATOMIC_ACQ_REL)) rl_ach_save_loaded();
    }
}

static void ach_update_presence(void) {
    int now[4];
    for (int i = 0; i < 4; i++) now[i] = game_presence[i];
    if (now[0] != g_ach_rp_chapter) { g_ach_rp_chapter = now[0]; g_ach_rp_area = 0; }
    if (now[3]) g_ach_rp_area = now[3];
    now[3] = g_ach_rp_area;
    char text[256];
    achievements_format_presence(text, sizeof text, now);
    rl_ach_presence(text);
}

void achievements_start(void) {
    const char *session = getenv("RL_SESSION");
    if (!session || !session[0]) return;
    g_ach_enabled = 1;
    atexit(achievements_shutdown);
}

void achievements_shutdown(void) {
    g_ach_enabled = 0;
    if (!g_ach_inited) return;
    g_ach_inited = 0;
    rl_ach_shutdown();
}

void achievements_save_loaded(void) {
    if (!g_ach_enabled) return;
    /* Hold a 180-frame grace window so progress loaded from SlotData_*.dat on the save worker thread
     * is classified as pre-existing (earlier) on the next periodic milestone poll. */
    __atomic_store_n(&g_ach_save_grace, 180, __ATOMIC_RELEASE);
    if (g_ach_inited) rl_ach_save_loaded();
    else __atomic_store_n(&g_ach_pending_save, 1, __ATOMIC_RELEASE);
}

/* An event has no "earlier" flag: the launcher keeps an unlock that follows save_loaded as Casual, so
 * progress read from a save outside the grace window is announced with one more save_loaded. */
static void ach_fire(uint32_t id, int from_save) {
    if (id < WT_ACH_BASE || id >= WT_ACH_BASE + WT_ACH_COUNT) return;
    ach_ensure_init();
    if (from_save && __atomic_load_n(&g_ach_save_grace, __ATOMIC_ACQUIRE) <= 0) achievements_save_loaded();
    rl_ach_event(wt_ach_defs[id - WT_ACH_BASE].event);
}

void achievements_event(int kind, uint32_t id, const char *title, const char *desc, uint32_t score) {
    (void)desc;
    if (!g_ach_enabled) return;
    switch (kind) {
    case ACH_EV_FRAME: {
        int grace = __atomic_load_n(&g_ach_save_grace, __ATOMIC_ACQUIRE);
        if (grace > 0) __atomic_compare_exchange_n(&g_ach_save_grace, &grace, grace - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        if (!g_ach_inited) {
            /* Fallback: connect even if ACH_EV_READY was delayed. */
            if (++g_ach_frame >= 300) ach_ensure_init();
            return;
        }
        rl_ach_frame();
        if (rl_ach_connected() && (g_ach_frame++ & 63) == 0) ach_update_presence();
        break;
    }
    case ACH_EV_READY:
        ach_ensure_init();
        break;
    case ACH_EV_UNLOCK:
        ach_fire(id, 0);
        break;
    case ACH_EV_EARLIER:
        ach_fire(id, 1);
        break;
    case ACH_EV_CASUAL:
        ach_ensure_init();
        rl_ach_casual(title ? title : "debug");
        break;
    case ACH_EV_SAVE_LOADED:
        achievements_save_loaded();
        break;
    case ACH_EV_BITFIELD: {
        /* A milestone appeared during periodic save-state polling without going through a live hook.
         * During a save load, in the menus (no chapter active), or when multiple bits appear at once,
         * it came from a save file (the launcher keeps it Casual). Otherwise it is a live unlock on an
         * older lifted build whose chapter-save hooks were not yet re-lifted. */
        ach_fire(id, game_presence[0] < 0 || score > 1);
        break;
    }
    default:
        break;
    }
}
