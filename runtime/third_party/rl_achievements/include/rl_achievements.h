/*
 * rl-achievements: Recomp Achievements for RecompLauncher. SDK format version 1.
 * Recomp Achievements License 1.0 (see LICENSE): free for any game. Uses rcheevos (MIT) for memory rules.
 *
 * Started outside RecompLauncher (no RL_SESSION in the environment), every call
 * does nothing, so a recomp can ship it always.
 *
 *   rl_ach_init("n64", read_ram, NULL);   // once, at start
 *   rl_ach_frame();                       // once per frame (memory lists)
 *   rl_ach_native(id);                    // from the runtime's own unlock hook (Xbox 360)
 *   rl_ach_event("beat_final_boss");      // from the port's code (decomp ports)
 *   rl_ach_save_loaded();                 // whenever a save is loaded
 *   rl_ach_lboard(1, score);              // a leaderboard entry from the port's code
 *   rl_ach_presence("World 3-2");          // what the player is doing
 */
#ifndef RL_ACHIEVEMENTS_H
#define RL_ACHIEVEMENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RL_ACH_SDK_VERSION 1

/* Reads num_bytes of console memory at address into buffer. Returns how many
 * bytes it read (0 for an address the console doesn't have). Addresses are the
 * console's own, as the list's rules use them. */
typedef uint32_t (*rl_ach_read_fn)(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud);

/* Optional, before rl_ach_init: the recomp's catalog id and its version. */
void rl_ach_set_info(const char* game_id, const char* version);

/* console: "n64", "snes", "gb", "gba", "psx", "ps2", "dc", "xbox", "x360", or "" for none.
 * read: NULL when the recomp has no console memory (native or code events only).
 * Returns 1 when connected to RecompLauncher, 0 otherwise. */
int rl_ach_init(const char* console, rl_ach_read_fn read, void* ud);

/* Once per frame, from the recomp's VI or vblank hook. Checks the memory rules
 * and reads messages from the launcher. Cheap when no list is loaded. */
void rl_ach_frame(void);

/* The game unlocked a native achievement (Xbox 360 XGIUserWriteAchievements,
 * later PS3 and PS4 trophies). id = the console's own achievement id. */
void rl_ach_native(uint32_t id);

/* The game's native achievement list (Xbox 360: read from the XEX, which only the runtime can
 * decrypt). Call once per achievement, before or after rl_ach_init; the library sends the whole
 * list to the launcher as one native_list message. title and description may be NULL. */
void rl_ach_native_define(uint32_t id, const char* title, const char* description, uint32_t gamerscore);

/* A native achievement the runtime already had from before it was connected
 * (its own saved unlocks). Sent once at start; the launcher keeps these as Casual. */
void rl_ach_native_earlier(uint32_t id);

/* A code event the port's developer fired. The launcher maps names to achievements. */
void rl_ach_event(const char* name);

/* A leaderboard entry from the port's own code (decomps): a score, a time in frames or
 * milliseconds, as the list's leaderboard says. Memory lists' leaderboards submit themselves. */
void rl_ach_lboard(uint32_t id, int32_t value);

/* What the player is doing, in a few words ("World 3-2, 47 stars"). Memory lists with a
 * rich_presence script set it themselves; ports can set it from their own code. Sent only
 * when it changes. */
void rl_ach_presence(const char* text);

/* A save was loaded. Ranked rules use it (native unlocks right after a load). */
void rl_ach_save_loaded(void);

/* Unlocks from now on can't count as Ranked, for example a cheat menu was opened.
 * reason: a short word such as "cheats" or "save_state". */
void rl_ach_casual(const char* reason);

/* 1 when the launcher asked the runtime to hide its own unlock toast,
 * because the launcher shows one. Runtimes with their own toast check this. */
int rl_ach_quiet_toasts(void);

/* 1 when connected to RecompLauncher. */
int rl_ach_connected(void);

/* For tools and tests: load a memory list from JSON yourself instead of
 * receiving it from the launcher. Returns how many rules were loaded. */
int rl_ach_load_list(const char* json);

/* Optional: called for every unlock (any source), on the caller's thread. */
typedef void (*rl_ach_unlock_fn)(uint32_t id, void* ud);
void rl_ach_on_unlock(rl_ach_unlock_fn fn, void* ud);

void rl_ach_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
