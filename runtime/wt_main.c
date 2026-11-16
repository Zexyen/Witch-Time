/* witch-time-game: the runtime is libwitchtime.so beside this program (boot.c wt_host_main), so the whole of it can be
 * prebuilt; the launcher starts this when it cannot load the library itself. */
int wt_host_main(int argc, char **argv);
#ifdef __ANDROID__
/* [witchtime] Android: this file is part of libwitchtime.so, and the app's SDLActivity calls SDL_main (SDL_main.h renames
 * main). The app's external storage folder (Android/data/<app>/files on the shared storage) is a Witch Time folder, as
 * the launcher runs the game from on a PC (launcher/src/game_host.cpp): targets/exefs (main.elf, sdk.elf), targets/romfs,
 * config/, and build/, whose witch-time-game the runtime takes for its program (plat_posix.c GetModuleFileNameA). */
#define _GNU_SOURCE
#include <sched.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    static char exe[1024], elf[1024], rom[1024], run[1024];
    const char *dir = SDL_GetAndroidExternalStoragePath();
    if (!dir) { SDL_Log("witch-time: no external storage folder: %s", SDL_GetError()); return 2; }
    snprintf(exe, sizeof exe, "%s/build/witch-time-game", dir);
    snprintf(elf, sizeof elf, "%s/targets/exefs/main.elf", dir);
    snprintf(rom, sizeof rom, "%s/targets/romfs", dir);
    snprintf(run, sizeof run, "%s/build", dir);
    mkdir(run, 0777);
    snprintf(run, sizeof run, "%s/build/run", dir);
    mkdir(run, 0777);
    setenv("WT_ROMFS", rom, 0);
    setenv("WT_TITLE", "Witch Time", 0);
    setenv("WT_VK_MISSING", "warn", 0);
    snprintf(exe, sizeof exe, "%s/config/env.txt", dir);   /* KEY=value lines: the variables a PC sets in its shell */
    FILE *env = fopen(exe, "r");
    for (char line[1024]; env && fgets(line, sizeof line, env);) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (eq && line[0] != '#') { *eq = 0; setenv(line, eq + 1, 1); }
    }
    if (env) fclose(env);
    if (getenv("WT_AFFINITY")) {                    /* [witchtime] TEST: WT_AFFINITY=<hex mask of cores>: this thread and every one made after it */
        cpu_set_t set; unsigned long m = strtoul(getenv("WT_AFFINITY"), NULL, 16);
        CPU_ZERO(&set);
        for (int i = 0; i < 32; i++) if (m >> i & 1) CPU_SET(i, &set);
        SDL_Log("witch-time: WT_AFFINITY %lx: %s", m, sched_setaffinity(0, sizeof set, &set) ? "refused" : "set");
    }
    snprintf(exe, sizeof exe, "%s/build/witch-time-game", dir);
    if (chdir(run)) { SDL_Log("witch-time: %s: cannot open it", run); return 2; }
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");   /* the window is windowed and resizable (display.c) */
    SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0");          /* SDL's Android event wait wakes itself on its own sentinel: without this the window thread spins a whole core */
    freopen("witch-time-game.log", "w", stdout);   /* the runtime reports on stdout and stderr: logcat shows neither */
    setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(fileno(stdout), 2);
    char *args[] = { exe, elf, NULL };
    return wt_host_main(2, args);
}
#else
int main(int argc, char **argv) { return wt_host_main(argc, argv); }
#endif
