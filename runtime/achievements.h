/* [witchtime] Recomp Achievements bridge (github.com/elripalda/RecompAchievements).
 * Exposes Witch Time's achievement set (story chapter clears, difficulty completion, Platinum & Infinite Climax
 * Pure Platinum mastery, secret bosses, and weapon/demon unlocks; see docs/ACHIEVEMENTS.md) to RecompLauncher
 * over RL_SESSION. Started without RL_SESSION, every call is a no-op. */
#ifndef WT_ACHIEVEMENTS_H
#define WT_ACHIEVEMENTS_H

#include <stddef.h>
#include <stdint.h>

#define WT_ACH_STORY_CHAPTERS 19u
#define WT_ACH_BASE           1u
#define WT_ACH_COUNT          50u

/* Chapter award enum in Bayonetta 3 save records (f_7100b2a6c0 / f_7100b29be0):
 * 0 = Casual clear (no medal), 1 = Stone, 2 = Bronze, 3 = Silver, 4 = Gold, 5 = Platinum, 6 = Pure Platinum, 7 = unplayed. */
#define WT_ACH_MEDAL_PLATINUM      5
#define WT_ACH_MEDAL_PURE_PLATINUM 6

/* Secret boss & Witch Trial chapter IDs (main.elf 0x7101853954). */
#define WT_ACH_CH_WITCH_TRIAL_1 0x100000u   /* Lost Chapter: Witch Trial 1 (Standard) */
#define WT_ACH_CH_WITCH_TRIAL_2 0x100001u   /* Lost Chapter: Witch Trial 2 (Expert) */
#define WT_ACH_CH_WITCH_TRIAL_3 0x100002u   /* Lost Chapter: Witch Trial 3 (Infinite Climax) */
#define WT_ACH_CH_GOLEM         0x1000000u  /* Extra: The Artifact Unleashed (any difficulty) */
#define WT_ACH_CH_RODIN         0x10000000u /* Devil's Duel (always Infinite Climax) */

/* Weapon & Infernal Demon save bitmasks (save_obj + 6492 / + 6496, MSB-first: 0x80000000u >> id).
 * Weapons 0..11 + 14 (13 total) are all base-game Bayonetta 3 weapons; 12 (Scarborough Fair) and 13 (Love Is Blue)
 * require Bayonetta 1/2 save data on Switch and are intentionally excluded. Demons 0..11 are all 12 Infernal Demons. */
#define WT_ACH_ALL_WEAPONS_MASK 0xfff20000u
#define WT_ACH_ALL_DEMONS_MASK  0xfff00000u

/* List ids (1-based; Recomp Achievements reserves 0). */
#define WT_ACH_ID_CHAPTER_BASE      1u  /*  1..19: Story chapter clears (any difficulty) */
#define WT_ACH_ID_COMPLETE_GAME     20u /* Complete all 19 story chapters on any difficulty */
#define WT_ACH_ID_COMPLETE_HARD     21u /* Complete all 19 story chapters on Expert (Hard) or higher */
#define WT_ACH_ID_COMPLETE_CLIMAX   22u /* Complete all 19 story chapters on Infinite Climax (highest difficulty) */
#define WT_ACH_ID_ALL_PLATINUM      23u /* Earn Platinum or Pure Platinum on all 19 story chapters */
#define WT_ACH_ID_PURE_CLIMAX_BASE  24u /* 24..42: Pure Platinum on each story chapter (Infinite Climax only) */
#define WT_ACH_ID_BOSS_GOLEM        43u /* Defeat Golem (Extra: The Artifact Unleashed) */
#define WT_ACH_ID_BOSS_RODIN        44u /* Defeat Rodin (Devil's Duel) */
#define WT_ACH_ID_ALL_WEAPONS       45u /* Unlock all 13 Bayonetta 3 weapons */
#define WT_ACH_ID_ALL_DEMONS        46u /* Unlock all 12 Infernal Demons */
#define WT_ACH_ID_WITCH_TRIAL_1     47u /* Complete Lost Chapter: Witch Trial 1 */
#define WT_ACH_ID_WITCH_TRIAL_2     48u /* Complete Lost Chapter: Witch Trial 2 */
#define WT_ACH_ID_WITCH_TRIAL_3     49u /* Complete Lost Chapter: Witch Trial 3 */
#define WT_ACH_ID_ALL_PURE_CLIMAX   50u /* Earn Pure Platinum on all 19 story chapters on Infinite Climax */

typedef struct {
    uint32_t id;
    const char *title;
    const char *desc;
    uint32_t score;     /* 5, 10, 25 or 50: the only values a Recomp Achievements list takes */
    const char *event;  /* fired over the channel, matched by docs/achievements.json; never rename a shipped one */
} WtAchDef;

/* The 19 story chapters (main.elf 0x7101853904, matching Play History's overall story record 0x15). */
static const uint32_t wt_ach_story_chapters[WT_ACH_STORY_CHAPTERS] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
};

static const WtAchDef wt_ach_defs[WT_ACH_COUNT] = {
    /* 1..19: Story Chapter Clears (any difficulty) */
    { 1, "Prologue Clear",              "Complete Prologue: A Chaotic Encounter on any difficulty.",                        10, "prologue_clear"},
    { 2, "Chapter 1 Clear",             "Complete Chapter 1: Scrambling for Answers on any difficulty.",                    10, "chapter_1_clear"},
    { 3, "Chapter 2 Clear",             "Complete Chapter 2: City on Fire on any difficulty.",                              10, "chapter_2_clear"},
    { 4, "Chapter 3 Clear",             "Complete Chapter 3: A Sinking Feeling on any difficulty.",                         10, "chapter_3_clear"},
    { 5, "Side Chapter 1 Clear",        "Complete Side Chapter 1: The Crimson Shadow on any difficulty.",                   10, "side_chapter_1_clear"},
    { 6, "Chapter 4 Clear",             "Complete Chapter 4: Worlds Apart on any difficulty.",                              10, "chapter_4_clear"},
    { 7, "Chapter 5 Clear",             "Complete Chapter 5: Hot Pursuit on any difficulty.",                               10, "chapter_5_clear"},
    { 8, "Chapter 6 Clear",             "Complete Chapter 6: Off the Rails on any difficulty.",                             10, "chapter_6_clear"},
    { 9, "Side Chapter 2 Clear",        "Complete Side Chapter 2: The Lynx Strikes on any difficulty.",                     10, "side_chapter_2_clear"},
    {10, "Chapter 7 Clear",             "Complete Chapter 7: Burning Sands on any difficulty.",                             10, "chapter_7_clear"},
    {11, "Chapter 8 Clear",             "Complete Chapter 8: A Croaking Chorus on any difficulty.",                         10, "chapter_8_clear"},
    {12, "Chapter 9 Clear",             "Complete Chapter 9: Learning to Fly on any difficulty.",                           10, "chapter_9_clear"},
    {13, "Side Chapter 3 Clear",        "Complete Side Chapter 3: High Stakes on any difficulty.",                          10, "side_chapter_3_clear"},
    {14, "Chapter 10 Clear",            "Complete Chapter 10: Cover of Night on any difficulty.",                           10, "chapter_10_clear"},
    {15, "Chapter 11 Clear",            "Complete Chapter 11: A Familiar Dance on any difficulty.",                         10, "chapter_11_clear"},
    {16, "Chapter 12 Clear",            "Complete Chapter 12: The Edge of Madness on any difficulty.",                      10, "chapter_12_clear"},
    {17, "Side Chapter 4 Clear",        "Complete Side Chapter 4: The Doctor is Out on any difficulty.",                    10, "side_chapter_4_clear"},
    {18, "Chapter 13 Clear",            "Complete Chapter 13: Kingdom of One on any difficulty.",                           10, "chapter_13_clear"},
    {19, "Chapter 14 Clear",            "Complete Chapter 14: The Single Truth on any difficulty.",                         10, "chapter_14_clear"},
    /* 20..23: Game Completion & Platinum Mastery */
    {20, "The Single Truth",            "Complete all story chapters on any difficulty.",                                   50, "the_single_truth"},
    {21, "Umbran Expert",               "Complete all story chapters on Expert or higher.",                                 50, "umbran_expert"},
    {22, "Infinite Climax",             "Complete all story chapters on \xe2\x88\x9e Climax.",                             50, "infinite_climax"},
    {23, "Platinum Witch",              "Earn a Platinum or Pure Platinum award on all story chapters.",                   50, "platinum_witch"},
    /* 24..42: Pure Platinum on Infinite Climax per Chapter */
    {24, "Prologue: Pure Climax",       "Earn a Pure Platinum award in Prologue: A Chaotic Encounter on \xe2\x88\x9e Climax.",      25, "prologue_pure_climax"},
    {25, "Chapter 1: Pure Climax",      "Earn a Pure Platinum award in Chapter 1: Scrambling for Answers on \xe2\x88\x9e Climax.",  25, "chapter_1_pure_climax"},
    {26, "Chapter 2: Pure Climax",      "Earn a Pure Platinum award in Chapter 2: City on Fire on \xe2\x88\x9e Climax.",            25, "chapter_2_pure_climax"},
    {27, "Chapter 3: Pure Climax",      "Earn a Pure Platinum award in Chapter 3: A Sinking Feeling on \xe2\x88\x9e Climax.",       25, "chapter_3_pure_climax"},
    {28, "Side Chapter 1: Pure Climax", "Earn a Pure Platinum award in Side Chapter 1: The Crimson Shadow on \xe2\x88\x9e Climax.", 25, "side_chapter_1_pure_climax"},
    {29, "Chapter 4: Pure Climax",      "Earn a Pure Platinum award in Chapter 4: Worlds Apart on \xe2\x88\x9e Climax.",            25, "chapter_4_pure_climax"},
    {30, "Chapter 5: Pure Climax",      "Earn a Pure Platinum award in Chapter 5: Hot Pursuit on \xe2\x88\x9e Climax.",             25, "chapter_5_pure_climax"},
    {31, "Chapter 6: Pure Climax",      "Earn a Pure Platinum award in Chapter 6: Off the Rails on \xe2\x88\x9e Climax.",           25, "chapter_6_pure_climax"},
    {32, "Side Chapter 2: Pure Climax", "Earn a Pure Platinum award in Side Chapter 2: The Lynx Strikes on \xe2\x88\x9e Climax.",   25, "side_chapter_2_pure_climax"},
    {33, "Chapter 7: Pure Climax",      "Earn a Pure Platinum award in Chapter 7: Burning Sands on \xe2\x88\x9e Climax.",           25, "chapter_7_pure_climax"},
    {34, "Chapter 8: Pure Climax",      "Earn a Pure Platinum award in Chapter 8: A Croaking Chorus on \xe2\x88\x9e Climax.",       25, "chapter_8_pure_climax"},
    {35, "Chapter 9: Pure Climax",      "Earn a Pure Platinum award in Chapter 9: Learning to Fly on \xe2\x88\x9e Climax.",         25, "chapter_9_pure_climax"},
    {36, "Side Chapter 3: Pure Climax", "Earn a Pure Platinum award in Side Chapter 3: High Stakes on \xe2\x88\x9e Climax.",        25, "side_chapter_3_pure_climax"},
    {37, "Chapter 10: Pure Climax",     "Earn a Pure Platinum award in Chapter 10: Cover of Night on \xe2\x88\x9e Climax.",         25, "chapter_10_pure_climax"},
    {38, "Chapter 11: Pure Climax",     "Earn a Pure Platinum award in Chapter 11: A Familiar Dance on \xe2\x88\x9e Climax.",       25, "chapter_11_pure_climax"},
    {39, "Chapter 12: Pure Climax",     "Earn a Pure Platinum award in Chapter 12: The Edge of Madness on \xe2\x88\x9e Climax.",    25, "chapter_12_pure_climax"},
    {40, "Side Chapter 4: Pure Climax", "Earn a Pure Platinum award in Side Chapter 4: The Doctor is Out on \xe2\x88\x9e Climax.",  25, "side_chapter_4_pure_climax"},
    {41, "Chapter 13: Pure Climax",     "Earn a Pure Platinum award in Chapter 13: Kingdom of One on \xe2\x88\x9e Climax.",         25, "chapter_13_pure_climax"},
    {42, "Chapter 14: Pure Climax",     "Earn a Pure Platinum award in Chapter 14: The Single Truth on \xe2\x88\x9e Climax.",       25, "chapter_14_pure_climax"},
    /* 43..44: Secret Bosses */
    {43, "The Artifact Unleashed",      "Defeat Golem in Extra: The Artifact Unleashed.",                                   50, "the_artifact_unleashed"},
    {44, "Devil's Duel",                "Defeat Rodin in Devil's Duel.",                                                   50, "devils_duel"},
    /* 45..46: Arsenal & Infernal Demons */
    {45, "Arsenal of Darkness",         "Unlock all 13 Bayonetta 3 weapons.",                                               50, "arsenal_of_darkness"},
    {46, "Infernal Pact",               "Unlock all 12 Infernal Demons.",                                                   50, "infernal_pact"},
    /* 47..49: Lost Chapters (Witch Trials) */
    {47, "Witch Trial 1 Clear",         "Complete Lost Chapter: Witch Trial 1.",                                            25, "witch_trial_1_clear"},
    {48, "Witch Trial 2 Clear",         "Complete Lost Chapter: Witch Trial 2.",                                            50, "witch_trial_2_clear"},
    {49, "Witch Trial 3 Clear",         "Complete Lost Chapter: Witch Trial 3.",                                            50, "witch_trial_3_clear"},
    /* 50: Ultimate Mastery */
    {50, "Pure Platinum Witch",         "Earn a Pure Platinum award on all story chapters on \xe2\x88\x9e Climax.",        50, "pure_platinum_witch"},
};

typedef struct {
    int8_t story[WT_ACH_STORY_CHAPTERS][4]; /* -1 uncleared, or 0..6 medal */
    int golem_cleared;                      /* non-zero if 0x1000000 cleared on any difficulty */
    int rodin_cleared;                      /* non-zero if 0x10000000 cleared */
    int witch_trial_cleared[3];             /* non-zero if 0x100000..0x100002 cleared */
    uint32_t weapons_mask;                  /* save_obj + 6492 (MSB-first) */
    uint32_t demons_mask;                   /* save_obj + 6496 (MSB-first) */
} WtAchSnapshot;

/* Evaluate all 50 achievement bits (bit i -> id WT_ACH_BASE + i) from a save snapshot.
 * Clearing on a higher difficulty retroactively satisfies lower difficulty game-completion achievements.
 * Per-chapter and full Pure Platinum achievements require Pure Platinum (6) strictly on Infinite Climax (diff 3). */
static inline uint64_t wt_ach_eval(const WtAchSnapshot *s) {
    uint64_t mask = 0;
    int all_any = 1, all_hard = 1, all_climax = 1, all_plat = 1, all_pure_climax = 1;
    for (uint32_t i = 0; i < WT_ACH_STORY_CHAPTERS; i++) {
        int cleared_any = 0, cleared_hard = 0, cleared_climax = 0, has_plat = 0;
        for (int d = 0; d < 4; d++) {
            int8_t m = s->story[i][d];
            if (m < 0) continue;
            cleared_any = 1;
            if (d >= 2) cleared_hard = 1;
            if (d >= 3) cleared_climax = 1;
            if (m >= WT_ACH_MEDAL_PLATINUM) has_plat = 1;
        }
        if (cleared_any) mask |= (1ull << i);
        else all_any = 0;
        if (!cleared_hard) all_hard = 0;
        if (!cleared_climax) all_climax = 0;
        if (!has_plat) all_plat = 0;
        if (s->story[i][3] == WT_ACH_MEDAL_PURE_PLATINUM) mask |= (1ull << (23u + i));
        else all_pure_climax = 0;
    }
    if (all_any) mask |= (1ull << 19);
    if (all_hard) mask |= (1ull << 20);
    if (all_climax) mask |= (1ull << 21);
    if (all_plat) mask |= (1ull << 22);
    if (s->golem_cleared) mask |= (1ull << 42);
    if (s->rodin_cleared) mask |= (1ull << 43);
    if ((s->weapons_mask & WT_ACH_ALL_WEAPONS_MASK) == WT_ACH_ALL_WEAPONS_MASK) mask |= (1ull << 44);
    if ((s->demons_mask & WT_ACH_ALL_DEMONS_MASK) == WT_ACH_ALL_DEMONS_MASK) mask |= (1ull << 45);
    for (uint32_t i = 0; i < 3; i++)
        if (s->witch_trial_cleared[i]) mask |= (1ull << (46u + i));
    if (all_pure_climax) mask |= (1ull << 49);
    return mask;
}

/* Event kinds passed from game.so/game.dll (game_shim_eh.c) to the host runtime via GameHost.ach_event. */
enum {
    ACH_EV_FRAME = 0,       /* once per game frame (main thread) */
    ACH_EV_DEFINE = 1,      /* unused: game builds up to v0.16.0 sent the list here, it is hosted now (docs/achievements.json) */
    ACH_EV_READY = 2,       /* connect to RL_SESSION now */
    ACH_EV_UNLOCK = 3,      /* live unlock: id */
    ACH_EV_EARLIER = 4,     /* already unlocked in loaded save: id */
    ACH_EV_CASUAL = 5,      /* session voided to Casual: title = reason */
    ACH_EV_SAVE_LOADED = 6, /* save slot loaded */
    ACH_EV_BITFIELD = 7,    /* periodic save-state diff: id, score = count newly seen this poll */
};

void achievements_start(void);
void achievements_shutdown(void);
void achievements_save_loaded(void);
void achievements_event(int kind, uint32_t id, const char *title, const char *desc, uint32_t score);
int achievements_format_presence(char *out, size_t n, const int g[4]);

#endif
