/* runtime/language_pick.h through the game's own tables: whatever Windows calls its display language, the save's defaults
 * must come out in that language, and never in Japanese unless Windows is Japanese (issue #160: en-GB did).
 *   clang -O1 -w -I runtime -I runtime/posix tests/language_test.c -o /tmp/language_test && /tmp/language_test */
#include <assert.h>
#include <stdio.h>
#include <windows.h>
#include "language_pick.h"

/* [game] 0x71018574f4: the nn::settings::Language of each slot, compared in this order at 0x7100ce3a00 */
static const int slot_language[15] = {0, 1, 12, 3, 2, 5, 4, 9, 8, 10, 13, 14, 15, 16, 7};
/* [game] 0x71018541e4: the slot the save's defaults use (0x7100b300a0) */
static const int slot_default[15] = {0, 1, 0, 3, 4, 5, 6, 7, 8, 9, 0, 0, 12, 13, 14};
/* [game] 0x7101852748: the Display / Subtitle Language value of a slot (0x7100b210c0) */
static const int slot_option[15] = {0, 1, 1, 4, 2, 5, 3, 10, 8, 11, 1, 1, 7, 6, 9};
enum { JA, EN, FR, IT, DE, ES, ZH_HANT, ZH_HANS, NL, KO, PT, RU };   /* the Options values */

static int option_for(const char *name) {
    char pick[16];
    int language = -1, slot = -1;
    language_pick(name, pick);
    for (int i = 0; i < (int)(sizeof g_language_codes / sizeof g_language_codes[0]); i++) if (!strcmp(pick, g_language_codes[i])) language = i;
    for (int i = 0; i < 15; i++) if (slot_language[i] == language) slot = i;
    assert(slot >= 0);                       /* a code the game does not compare leaves its slot at 0 = Japanese */
    return slot_option[slot_default[slot]];
}

int main(void) {
    static const struct { const char *name; int option; } cases[] = {
        {"ja-JP", JA}, {"en-US", EN}, {"en-GB", EN}, {"en-AU", EN}, {"en", EN}, {"fr-FR", FR}, {"fr-CA", FR}, {"de-DE", DE},
        {"de-AT", DE}, {"it-IT", IT}, {"es-ES", ES}, {"es-MX", ES}, {"es-419", ES}, {"zh-CN", ZH_HANS}, {"zh-Hans-CN", ZH_HANS},
        {"zh-TW", ZH_HANT}, {"zh-HK", ZH_HANT}, {"ko-KR", KO}, {"ru-RU", RU},
        {"nl-NL", EN}, {"pt-BR", EN}, {"pl-PL", EN}, {"", EN},                  /* empty text or no text: English */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int got = option_for(cases[i].name);
        if (got != cases[i].option) { printf("FAIL %s: option %d, expected %d\n", cases[i].name, got, cases[i].option); return 1; }
    }
    puts("language_test: ok");
    return 0;
}
