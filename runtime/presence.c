/* Discord Rich Presence: chapter, verse, difficulty, area and the time in the chapter, sent to the Discord client on this machine
 * over its local IPC (docs/RE_NOTES.md section 10). The game's thread publishes game_presence (game_shim_eh.c); this thread only
 * reads it, so a slow or missing Discord never touches a frame. graphics.ini: discord_presence = off stops it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <windows.h>
#include "gfx_options.h"
#include "presence_names.h"

/* The Application ID of the app made at discord.com/developers: its name is what Discord shows after "Playing". Empty: no presence. */
#define APP_ID "1566672081414791206"
#define POLL_MS 5000                     /* also keeps us under Discord's 5 updates in 20 s */

__declspec(dllimport) extern volatile int game_presence[4];      /* game_shim_eh.c */

/* The SET_ACTIVITY frame of a game_presence snapshot. The texts are the tables in presence_names.h, so nothing needs JSON escaping. */
static int presence_json(char *out, size_t n, const int g[4], long long since, unsigned long pid) {
    const char *chapter = NULL, *art = NULL;
    for (size_t i = 0; i < sizeof wt_chapters / sizeof *wt_chapters; i++)
        if (wt_chapters[i].id == g[0]) { chapter = wt_chapters[i].name; art = wt_chapters[i].art; }
    if (!chapter)
        return snprintf(out, n, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{\"details\":\"In the menus\","
                        "\"assets\":{\"large_image\":\"logo\"}}},\"nonce\":\"%lld\"}", pid, (long long)time(NULL));
    char verse[16] = "";
    if (g[2] > 0) snprintf(verse, sizeof verse, " - Verse %d", g[2]);
    const char *difficulty = g[1] >= 0 && g[1] < 4 ? wt_difficulties[g[1]] : "";
    const char *area = g[3] > 0 && g[3] < 26 ? wt_areas[g[3]] : NULL;
    return snprintf(out, n, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{\"details\":\"%s%s\",\"state\":\"%s%s%s\","
                    "\"assets\":{\"large_image\":\"%s\"},\"timestamps\":{\"start\":%lld}}},\"nonce\":\"%lld\"}",
                    pid, chapter, verse, difficulty, area ? " - " : "", area ? area : "", art, since, (long long)time(NULL));
}

#ifdef _WIN32
typedef HANDLE Ipc;
#define IPC_NONE INVALID_HANDLE_VALUE
static Ipc ipc_open(void) {
    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return IPC_NONE;
}
static int ipc_write(Ipc p, const void *b, uint32_t n) { DWORD w; return WriteFile(p, b, n, &w, NULL) && w == n; }
static int ipc_read(Ipc p, void *b, uint32_t n) {
    for (DWORD r; n; n -= r, b = (char *)b + r) if (!ReadFile(p, b, n, &r, NULL) || !r) return 0;
    return 1;
}
#define ipc_close CloseHandle
#else
typedef int Ipc;
#define IPC_NONE (-1)
static Ipc ipc_open(void) {
#ifdef __APPLE__
    const char *dir = getenv("TMPDIR");              /* macOS: Discord's socket is in the user's temporary folder */
#else
    const char *dir = getenv("XDG_RUNTIME_DIR");
#endif
    static const char *const sub[] = {"", "app/com.discordapp.Discord/", "snap.discord/"};      /* the Flatpak (Steam Deck) and Snap clients */
    for (int s = 0; s < 3; s++) for (int i = 0; i < 10; i++) {
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        snprintf(a.sun_path, sizeof a.sun_path, "%s/%sdiscord-ipc-%d", dir ? dir : "/tmp", sub[s], i);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return IPC_NONE;
        if (!connect(fd, (struct sockaddr *)&a, sizeof a)) return fd;
        close(fd);
    }
    return IPC_NONE;
}
static int ipc_write(Ipc p, const void *b, uint32_t n) { return send(p, b, n, MSG_NOSIGNAL) == (ssize_t)n; }      /* a closed Discord must not raise SIGPIPE */
static int ipc_read(Ipc p, void *b, uint32_t n) { return recv(p, b, n, MSG_WAITALL) == (ssize_t)n; }
#define ipc_close close
#endif

/* One frame out and its answer: the handshake's READY, the activity's echo. An answer left unread would fill the socket. */
static int ipc_frame(Ipc p, uint32_t op, const char *json) {
    uint32_t head[2] = {op, (uint32_t)strlen(json)};
    if (!ipc_write(p, head, sizeof head) || !ipc_write(p, json, head[1]) || !ipc_read(p, head, sizeof head)) return 0;
    if (head[0] == 2) return 0;          /* CLOSE: a wrong client id, or Discord shutting down */
    for (char junk[512]; head[1];) {
        uint32_t n = head[1] < sizeof junk ? head[1] : sizeof junk;
        if (!ipc_read(p, junk, n)) return 0;
        head[1] -= n;
    }
    return 1;
}

/* The start of the time in the chapter: the chapter stays through a load, so a change means a new one or the menus. The HUD forgets
 * the banner at each phase, and many phases have none of their own: the area is the chapter's last one. */
static long long presence_track(int g[4], long long clock) {
    static int chapter = -2, area;
    static long long since;
    if (g[0] != chapter) { chapter = g[0]; since = clock; area = 0; }
    if (g[3]) area = g[3];
    g[3] = area;
    return since;
}

static DWORD WINAPI presence_thread(LPVOID unused) {
    (void)unused;
    Ipc p = IPC_NONE;
    int sent[4];
    char json[512];
    for (;; Sleep(POLL_MS)) {
        int now[4];
        for (int i = 0; i < 4; i++) now[i] = game_presence[i];
        long long since = presence_track(now, (long long)time(NULL));
        if (p == IPC_NONE) {
            if ((p = ipc_open()) == IPC_NONE) continue;
            snprintf(json, sizeof json, "{\"v\":1,\"client_id\":\"%s\"}", APP_ID);
            if (!ipc_frame(p, 0, json)) { ipc_close(p); p = IPC_NONE; continue; }
            sent[0] = -2;                /* nothing the game publishes */
        }
        if (!memcmp(now, sent, sizeof now)) continue;
#ifdef _WIN32
        presence_json(json, sizeof json, now, since, GetCurrentProcessId());
#else
        presence_json(json, sizeof json, now, since, (unsigned long)getpid());
#endif
        if (ipc_frame(p, 1, json)) memcpy(sent, now, sizeof now);
        else { ipc_close(p); p = IPC_NONE; }
    }
    return 0;
}

void presence_start(void) {
    char on[8] = "on";
    gfx_ini_get("discord_presence", on, sizeof on);
    if (APP_ID[0] && _stricmp(on, "off")) CloseHandle(CreateThread(NULL, 0, presence_thread, NULL, 0, NULL));
}
