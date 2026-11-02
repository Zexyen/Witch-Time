/* runtime/presence.c: the activity text for each kind of state, and the IPC framing against a socket that answers as Discord does.
 *   clang -O1 -w -I runtime -I runtime/posix '-D__declspec(x)=' tests/presence_test.c -lpthread -o /tmp/presence_test && /tmp/presence_test
 * (Linux; the Windows pipe calls are not exercised.) */
#include <assert.h>
#include <signal.h>
int gfx_ini_get(const char *key, char *out, size_t n) { (void)key; (void)out; (void)n; return 0; }
volatile int game_presence[4];
#include "presence.c"
HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *id) { return NULL; }
BOOL CloseHandle(HANDLE h) { return 1; }

static void has(const char *s, const char *want, int yes) {
    if (!strstr(s, want) != !yes) { printf("%s: %s %s\n", s, yes ? "lacks" : "has", want); exit(1); }
}
static void answer(int fd, uint32_t op, uint32_t size) {
    static char body[2048];
    uint32_t head[2] = {op, size};
    assert(write(fd, head, 8) == 8 && write(fd, body, size) == (ssize_t)size);
}

int main(void) {
    char json[512];
    presence_json(json, sizeof json, (int[]){0x4, 3, 2, 11}, 1234, 77);
    has(json, "\"details\":\"Chapter 3: A Sinking Feeling - Verse 2\"", 1);
    has(json, "\"state\":\"\xe2\x88\x9e Climax - Tokyo: Shibuya\"", 1);
    has(json, "\"timestamps\":{\"start\":1234}", 1);
    has(json, "\"pid\":77", 1);
    has(json, "\"large_image\":\"c03\"", 1);
    presence_json(json, sizeof json, (int[]){0x2, 1, 0, 3}, 1, 1);      /* no verse running, a banner the game has no text for */
    has(json, "\"details\":\"Chapter 1: Scrambling for Answers\"", 1);
    has(json, "\"state\":\"Standard\"", 1);
    presence_json(json, sizeof json, (int[]){-1, 0, 0, 0}, 1, 1);
    has(json, "In the menus", 1);
    has(json, "\"large_image\":\"logo\"", 1);
    has(json, "timestamps", 0);
    has(json, "state", 0);
    assert(presence_json(json, sizeof json, (int[]){0x10000000, 9, 99, 99}, 1, 1) > 0);      /* numbers off the tables */
    has(json, "\"details\":\"Devil's Duel - Verse 99\"", 1);

    int g[4] = {1, 1, 0, 2};
    assert(presence_track(g, 100) == 100 && g[3] == 2);
    g[3] = 0;                                 /* a new phase: the HUD forgot the banner, the player has not moved */
    assert(presence_track(g, 200) == 100 && g[3] == 2);
    g[0] = 2; g[3] = 0;                       /* the next chapter: its clock, and no area until its banner */
    assert(presence_track(g, 300) == 300 && g[3] == 0);
    g[3] = 11;
    assert(presence_track(g, 400) == 300 && g[3] == 11);
    g[0] = -1; g[3] = 0; presence_track(g, 500);      /* out to the menus and back to the same chapter: a new visit */
    g[0] = 2;
    assert(presence_track(g, 600) == 600);

    int sv[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    answer(sv[1], 1, 2000);                   /* larger than the buffer it is read through */
    answer(sv[1], 1, 0);
    answer(sv[1], 2, 40);
    assert(ipc_frame(sv[0], 0, "hello") == 1);
    uint32_t head[2]; char body[8] = {0};
    assert(read(sv[1], head, 8) == 8 && head[0] == 0 && head[1] == 5 && read(sv[1], body, 5) == 5 && !strcmp(body, "hello"));
    assert(ipc_frame(sv[0], 1, "x") == 1);
    assert(ipc_frame(sv[0], 1, "y") == 0);    /* CLOSE */
    close(sv[1]);
    signal(SIGPIPE, SIG_DFL);                 /* MSG_NOSIGNAL must keep a gone Discord from killing the game */
    assert(ipc_frame(sv[0], 1, "z") == 0);
    puts("presence_test: ok");
    return 0;
}
