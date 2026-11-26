/* [witchtime] Chapter, difficulty and area names shared by Discord Rich Presence (presence.c) and
 * Recomp Achievements rich presence (achievements.c); docs/RE_NOTES.md section 10. */
#ifndef WT_PRESENCE_NAMES_H
#define WT_PRESENCE_NAMES_H

/* The game's texts (CORE_CHAPTER_NAME_*, in the order of 0x7100983ec0) under its chapter numbers, and each chapter's picture: the key
 * of an image uploaded to the Discord app (Rich Presence, Art Assets), by the game's own choice of result screen, 0x7100b22610. The
 * pictures are the game's, so they live in the app and not here. The side chapters have none of their own: "logo". */
static const struct { int id; const char *name, *art; } wt_chapters[] = {
    {0x0, "Phenomenal Affirmation", "logo"},                {0x1, "Prologue: A Chaotic Encounter", "prologue"},
    {0x2, "Chapter 1: Scrambling for Answers", "c01"},      {0x3, "Chapter 2: City on Fire", "c02"},
    {0x4, "Chapter 3: A Sinking Feeling", "c03"},           {0x5, "Side Chapter 1: The Crimson Shadow", "logo"},
    {0x6, "Chapter 4: Worlds Apart", "c04"},                {0x7, "Chapter 5: Hot Pursuit", "c05"},
    {0x8, "Chapter 6: Off the Rails", "c06"},               {0x9, "Side Chapter 2: The Lynx Strikes", "logo"},
    {0xa, "Chapter 7: Burning Sands", "c07"},               {0xb, "Chapter 8: A Croaking Chorus", "c08"},
    {0xc, "Chapter 9: Learning to Fly", "c09"},             {0xd, "Side Chapter 3: High Stakes", "logo"},
    {0xe, "Chapter 10: Cover of Night", "c10"},             {0xf, "Chapter 11: A Familiar Dance", "c11"},
    {0x10, "Chapter 12: The Edge of Madness", "c12"},       {0x11, "Side Chapter 4: The Doctor is Out", "logo"},
    {0x12, "Chapter 13: Kingdom of One", "c13"},            {0x13, "Chapter 14: The Single Truth", "cmax"},
    {0x100, "Phenomenal Remnant 1", "u01"},                 {0x101, "Phenomenal Remnant 2", "u02"},
    {0x102, "Phenomenal Remnant 3", "u03"},                 {0x103, "Phenomenal Remnant 4", "u04"},
    {0x104, "Phenomenal Remnant 5", "u05"},                 {0x105, "Phenomenal Remnant 6", "u06"},
    {0x106, "Phenomenal Remnant 7", "u07"},                 {0x107, "Phenomenal Remnant 8", "u08"},
    {0x108, "Phenomenal Remnant 9", "u09"},                 {0x109, "Phenomenal Remnant 10", "u10"},
    {0x10a, "Phenomenal Remnant 11", "u11"},                {0x10b, "Phenomenal Remnant 12", "u12"},
    {0x10c, "Phenomenal Remnant 13", "u13"},                {0x10d, "Phenomenal Remnant 14", "umax"},
    {0x100000, "Lost Chapter: Witch Trial 1", "witch01"},   {0x100001, "Lost Chapter: Witch Trial 2", "witch02"},
    {0x100002, "Lost Chapter: Witch Trial 3", "witch03"},   {0x1000000, "Extra: The Artifact Unleashed", "golem"},
    {0x10000000, "Devil's Duel", "rodin"},
};

static const char *const wt_difficulties[4] = {"Casual", "Standard", "Expert", "\xe2\x88\x9e Climax"};

/* The location banner's number -> HUD_LOCNAME_MAIN_* (0x71009c3ddc); 3 and 8 name messages the game does not have. */
static const char *const wt_areas[26] = {
    [1] = "New York: Manhattan", [2] = "New York: Manhattan", [4] = "Thule: Passage of Mysteries", [5] = "Thule: Cliffs of Time",
    [6] = "Thule: Secret Hills", [7] = "Thule: Frosted Peak", [9] = "New York: Manhattan", [10] = "Ginnungagap, the Chaotic Rift",
    [11] = "Tokyo: Shibuya", [12] = "Tokyo: Shinjuku", [13] = "Tokyo: Taito", [14] = "Qin Lan: Xi Chang An",
    [15] = "Qin Lan: Shuang Cha Ling", [16] = "Qin Lan: Liang Jie Shan", [17] = "Cairo: Tura", [18] = "Cairo: Zamalek",
    [19] = "Cairo: Venom's Caress", [20] = "Giza: Al Haram", [21] = "Giza: Temple of Tempests", [22] = "Paris: 16th Arrondissement",
    [23] = "Paris: 8th Arrondissement", [24] = "Paris: 1st Arrondissement", [25] = "Alphaverse",
};

#endif
