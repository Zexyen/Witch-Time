/* runtime/achievements.c: presence formatting, version normalization, no-op without RL_SESSION,
 * and full channel round-trip against a local fake RecompLauncher Unix socket.
 * See .github/workflows/wtbuild.yml for the compile command. */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

volatile int game_presence[4] = {-1, 0, 0, 0};
#include "achievements.c"

static void has(const char *s, const char *want, int yes) {
    if (!strstr(s, want) != !yes) {
        fprintf(stderr, "line: %s\n  expected %s: %s\n", s, yes ? "to contain" : "NOT to contain", want);
        exit(1);
    }
}

static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    struct pollfd p = {fd, POLLIN, 0};
    while (n + 1 < cap) {
        int r = poll(&p, 1, 2000);
        if (r <= 0) return -1;
        char ch;
        ssize_t k = read(fd, &ch, 1);
        if (k <= 0) return -1;
        if (ch == '\n') { buf[n] = 0; return (int)n; }
        buf[n++] = ch;
    }
    buf[n] = 0;
    return (int)n;
}

int main(void) {
    /* 1. Version string normalization */
    assert(!strcmp(ach_version_string("v0.15.0"), "0.15.0"));
    assert(!strcmp(ach_version_string("1.2.3"), "1.2.3"));
    assert(!strcmp(ach_version_string(""), ""));
    assert(!strcmp(ach_version_string("dev"), ""));
    assert(!strcmp(ach_version_string(NULL), ""));

    /* 1b. Achievement evaluation: chapters, difficulties, Platinum, Infinite Climax Pure Platinum, bosses, weapons/demons */
    {
        WtAchSnapshot s;
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        assert(wt_ach_eval(&s) == 0ull);
        for (uint32_t i = 0; i < WT_ACH_COUNT; i++) assert(wt_ach_defs[i].id == WT_ACH_BASE + i);

        /* Clearing a single chapter on Casual unlocks only that chapter */
        s.story[0][0] = 0;
        assert(wt_ach_eval(&s) == (1ull << 0));

        /* Pure Platinum on Expert (diff 2) does NOT unlock the Infinite Climax Pure Platinum achievement */
        s.story[0][2] = WT_ACH_MEDAL_PURE_PLATINUM;
        assert(wt_ach_eval(&s) == (1ull << 0));

        /* Platinum (5) on Infinite Climax (diff 3) does NOT unlock Pure Platinum on Infinite Climax */
        s.story[0][3] = WT_ACH_MEDAL_PLATINUM;
        assert(wt_ach_eval(&s) == (1ull << 0));

        /* Pure Platinum (6) on Infinite Climax (diff 3) unlocks #24 (bit 23) */
        s.story[0][3] = WT_ACH_MEDAL_PURE_PLATINUM;
        assert(wt_ach_eval(&s) == ((1ull << 0) | (1ull << 23)));

        /* Clearing all 19 chapters on Casual unlocks all 19 chapters + Complete Game (#20), NOT Hard/Climax/Platinum */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        for (uint32_t i = 0; i < WT_ACH_STORY_CHAPTERS; i++) s.story[i][0] = 0;
        uint64_t all_ch = (1ull << WT_ACH_STORY_CHAPTERS) - 1ull;
        assert(wt_ach_eval(&s) == (all_ch | (1ull << 19)));

        /* Clearing all 19 chapters on Infinite Climax (highest difficulty) retroactively unlocks Complete Game (#20),
         * Hard/Expert (#21), and Infinite Climax (#22); Gold (4) does NOT unlock Platinum Witch (#23) or Pure Climax */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        for (uint32_t i = 0; i < WT_ACH_STORY_CHAPTERS; i++) s.story[i][3] = 4;
        assert(wt_ach_eval(&s) == (all_ch | (1ull << 19) | (1ull << 20) | (1ull << 21)));

        /* Mixed Expert (2) and Infinite Climax (3) unlocks Complete Game + Hard, NOT Infinite Climax */
        s.story[5][3] = -1;
        s.story[5][2] = 5;
        assert(wt_ach_eval(&s) == (all_ch | (1ull << 19) | (1ull << 20)));

        /* All Platinums (5) / Pure Platinums (6) on Standard unlocks Platinum Witch (#23), NOT any Pure Climax (#24..#42) */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        for (uint32_t i = 0; i < WT_ACH_STORY_CHAPTERS; i++) s.story[i][1] = (i & 1) ? 6 : 5;
        assert(wt_ach_eval(&s) == (all_ch | (1ull << 19) | (1ull << 22)));

        /* One chapter short of Platinum does NOT unlock Platinum Witch (#23) */
        s.story[18][1] = 4;
        assert(wt_ach_eval(&s) == (all_ch | (1ull << 19)));

        /* All 19 chapters Pure Platinum on Infinite Climax unlocks chapters, all 3 difficulties, Platinum Witch,
         * all 19 Pure Climax (#24..#42), and Pure Platinum Witch (#50, bit 49) */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        for (uint32_t i = 0; i < WT_ACH_STORY_CHAPTERS; i++) s.story[i][3] = 6;
        uint64_t all_pure = ((1ull << WT_ACH_STORY_CHAPTERS) - 1ull) << 23;
        assert(wt_ach_eval(&s) == (all_ch | (1ull << 19) | (1ull << 20) | (1ull << 21) | (1ull << 22) | all_pure | (1ull << 49)));

        /* One chapter at regular Platinum (5) on Infinite Climax drops #42 and #50, keeps Platinum Witch (#23) */
        s.story[18][3] = 5;
        assert(wt_ach_eval(&s) == (all_ch | (1ull << 19) | (1ull << 20) | (1ull << 21) | (1ull << 22) | (all_pure & ~(1ull << 41))));

        /* Secret bosses: Golem (#43, bit 42) and Rodin (#44, bit 43) */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        s.golem_cleared = 1;
        assert(wt_ach_eval(&s) == (1ull << 42));
        s.rodin_cleared = 1;
        assert(wt_ach_eval(&s) == ((1ull << 42) | (1ull << 43)));

        /* Weapons (#45, bit 44) and Infernal Demons (#46, bit 45); Bayo 1/2 save bonus bits (12, 13) not required */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        s.weapons_mask = WT_ACH_ALL_WEAPONS_MASK & ~(1u << 17); /* missing Handguns (id 14) */
        s.demons_mask = WT_ACH_ALL_DEMONS_MASK & ~(1u << 20);   /* missing Rodin (id 11) */
        assert(wt_ach_eval(&s) == 0ull);
        s.weapons_mask = WT_ACH_ALL_WEAPONS_MASK;
        assert(wt_ach_eval(&s) == (1ull << 44));
        s.demons_mask = WT_ACH_ALL_DEMONS_MASK;
        assert(wt_ach_eval(&s) == ((1ull << 44) | (1ull << 45)));

        /* Witch Trials 1-3 (#47..#49, bits 46..48) */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        s.witch_trial_cleared[0] = 1;
        assert(wt_ach_eval(&s) == (1ull << 46));
        s.witch_trial_cleared[1] = 1;
        s.witch_trial_cleared[2] = 1;
        assert(wt_ach_eval(&s) == ((1ull << 46) | (1ull << 47) | (1ull << 48)));
    }

    /* 2. Presence formatting */
    char buf[256];
    achievements_format_presence(buf, sizeof buf, (int[]){-1, 0, 0, 0});
    assert(!strcmp(buf, "In the menus"));
    achievements_format_presence(buf, sizeof buf, (int[]){0x4, 3, 2, 11});
    assert(!strcmp(buf, "Chapter 3: A Sinking Feeling - Verse 2 (\xe2\x88\x9e Climax - Tokyo: Shibuya)"));
    achievements_format_presence(buf, sizeof buf, (int[]){0x2, 1, 0, 3});
    assert(!strcmp(buf, "Chapter 1: Scrambling for Answers (Standard)"));
    achievements_format_presence(buf, sizeof buf, (int[]){0x10000000, 9, 99, 99});
    assert(!strcmp(buf, "Devil's Duel - Verse 99"));

    /* 3. No-op when RL_SESSION is unset */
    unsetenv("RL_SESSION");
    achievements_start();
    assert(!g_ach_enabled && !g_ach_inited);
    achievements_event(ACH_EV_DEFINE, 1, "Prologue Clear", "Test", 15);
    achievements_event(ACH_EV_READY, 0, NULL, NULL, 0);
    achievements_event(ACH_EV_UNLOCK, 1, NULL, NULL, 0);
    achievements_event(ACH_EV_FRAME, 0, NULL, NULL, 0);
    assert(!rl_ach_connected());

    /* 4. Full Unix-socket round-trip against a fake RecompLauncher */
    char sock_path[108];
    snprintf(sock_path, sizeof sock_path, "/tmp/wt_ach_test_%d.sock", (int)getpid());
    unlink(sock_path);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(srv >= 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock_path);
    assert(bind(srv, (struct sockaddr *)&sa, sizeof sa) == 0);
    assert(listen(srv, 1) == 0);

    setenv("RL_SESSION", sock_path, 1);

    achievements_start();
    assert(g_ach_enabled && !g_ach_inited);

    /* Queue definitions before ready so hello + one complete native_list go out together */
    achievements_event(ACH_EV_DEFINE, 1, "Prologue Clear", "Complete Prologue: A Chaotic Encounter on any difficulty.", 15);
    achievements_event(ACH_EV_DEFINE, 20, "The Single Truth", "Complete all story chapters on any difficulty.", 50);
    achievements_event(ACH_EV_READY, 0, NULL, NULL, 0);
    assert(g_ach_inited && rl_ach_connected());

    int cli = accept(srv, NULL, NULL);
    assert(cli >= 0);

    char line[4096];
    /* First two messages MUST be hello, then one native_list */
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"hello\"", 1);
    has(line, "\"console\":\"switch\"", 1);
    has(line, "\"game\":\"witch-time\"", 1);
    has(line, "\"version\":\"0.15.0\"", 1);

    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"native_list\"", 1);
    has(line, "\"id\":1", 1);
    has(line, "\"title\":\"Prologue Clear\"", 1);
    has(line, "\"description\":\"Complete Prologue: A Chaotic Encounter on any difficulty.\"", 1);
    has(line, "\"id\":20", 1);
    has(line, "\"gamerscore\":50", 1);

    /* Save loaded -> save_loaded message + grace window classifies polled unlock as earlier */
    achievements_save_loaded();
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"save_loaded\"", 1);

    achievements_event(ACH_EV_BITFIELD, 1, NULL, NULL, 1);
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"native\"", 1);
    has(line, "\"id\":1", 1);
    has(line, "\"earlier\":true", 1);

    /* Live unlock after save-load grace window ends */
    __atomic_store_n(&g_ach_save_grace, 0, __ATOMIC_RELEASE);
    achievements_event(ACH_EV_UNLOCK, 20, NULL, NULL, 0);
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"native\"", 1);
    has(line, "\"id\":20", 1);
    has(line, "\"earlier\":true", 0);

    /* Casual voiding (e.g. debug command) */
    achievements_event(ACH_EV_CASUAL, 0, "debug", NULL, 0);
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"casual\"", 1);
    has(line, "\"reason\":\"debug\"", 1);

    /* Presence update on frame tick */
    game_presence[0] = 0x4; game_presence[1] = 2; game_presence[2] = 1; game_presence[3] = 11;
    achievements_event(ACH_EV_FRAME, 0, NULL, NULL, 0);
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"presence\"", 1);
    has(line, "Chapter 3: A Sinking Feeling - Verse 1 (Expert - Tokyo: Shibuya)", 1);

    /* Launcher -> game message: quiet mode */
    assert(!rl_ach_quiet_toasts());
    const char *quiet_msg = "{\"t\":\"quiet\"}\n";
    assert(write(cli, quiet_msg, strlen(quiet_msg)) == (ssize_t)strlen(quiet_msg));
    for (int i = 0; i < 10 && !rl_ach_quiet_toasts(); i++) {
        usleep(5000);
        achievements_event(ACH_EV_FRAME, 0, NULL, NULL, 0);
    }
    assert(rl_ach_quiet_toasts());

    achievements_shutdown();
    close(cli);
    close(srv);
    unlink(sock_path);

    puts("achievements_test: ok");
    return 0;
}
