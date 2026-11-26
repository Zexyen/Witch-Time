/* rl-achievements, SDK format version 1. Recomp Achievements License 1.0, see LICENSE. */
#include "rl_achievements.h"

#include "rc_runtime.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef HANDLE rl_conn;
#  define RL_NO_CONN INVALID_HANDLE_VALUE
static CRITICAL_SECTION g_lock;
static int g_lock_ready;
static void lock_init(void) { if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; } }
static void lock(void) { EnterCriticalSection(&g_lock); }
static void unlock(void) { LeaveCriticalSection(&g_lock); }
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <pthread.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
typedef int rl_conn;
#  define RL_NO_CONN (-1)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_init(void) {}
static void lock(void) { pthread_mutex_lock(&g_lock); }
static void unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif

#define RL_MAX_LINE (256 * 1024)   /* a whole list arrives as one line */
#define RL_MAX_PEEK 256

static struct {
  int ready;            /* rl_ach_init ran */
  rl_conn conn;
  rc_runtime_t runtime;
  int runtime_ready;
  int rules;            /* memory rules loaded */
  int boards;           /* leaderboards loaded */
  int has_rp;           /* a rich presence script is loaded */
  char presence[256];   /* the last presence text sent */
  uint64_t presence_at; /* frame it was last checked */
  rl_ach_read_fn read;
  void* read_ud;
  uint64_t frame;
  int quiet;
  char game[96];
  char version[48];
  char console[16];
  char* inbuf;
  size_t inlen;
  rl_ach_unlock_fn on_unlock;
  void* on_unlock_ud;
  cJSON* natives;       /* native_list entries, sent once connected */
  int natives_dirty;
} g;
/* g.conn starts as RL_NO_CONN: set in rl_ach_init before any use, and by conn_close. */

/* ---- transport: one JSON object per line ------------------------------- */

static rl_conn conn_open(const char* name) {
#ifdef _WIN32
  char path[300];
  if (strncmp(name, "\\\\.\\pipe\\", 9) == 0) snprintf(path, sizeof path, "%s", name);
  else snprintf(path, sizeof path, "\\\\.\\pipe\\%s", name);
  HANDLE h = INVALID_HANDLE_VALUE;
  int tries;
  /* The launcher keeps a spare pipe instance ready; while it swaps, the pipe can be busy. */
  for (tries = 0; tries < 4; tries++) {
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) break;
    if (!WaitNamedPipeA(path, 1000)) break;
  }
  if (h != INVALID_HANDLE_VALUE) {
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);
  }
  return h;
#else
  struct sockaddr_un sa;
  int fd;
  if (strlen(name) >= sizeof sa.sun_path) return RL_NO_CONN;
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return RL_NO_CONN;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, name);
  if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) { close(fd); return RL_NO_CONN; }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fd;
#endif
}

static void conn_close(void) {
  if (g.conn == RL_NO_CONN) return;
#ifdef _WIN32
  CloseHandle(g.conn);
#else
  close(g.conn);
#endif
  g.conn = RL_NO_CONN;
}

/* Sends one line; caller holds the lock. A broken connection is closed quietly:
 * the game keeps running, achievements simply stop reporting. */
static void send_line_locked(const char* json) {
  size_t n = strlen(json);
  const char nl = '\n';
  if (g.conn == RL_NO_CONN) return;
#ifdef _WIN32
  DWORD w;
  if (!WriteFile(g.conn, json, (DWORD)n, &w, NULL) || !WriteFile(g.conn, &nl, 1, &w, NULL)) conn_close();
#else
  {
    size_t off = 0;
    while (off < n) {
      ssize_t w = send(g.conn, json + off, n - off, MSG_NOSIGNAL);
      if (w > 0) { off += (size_t)w; continue; }
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
      conn_close();
      return;
    }
    if (send(g.conn, &nl, 1, MSG_NOSIGNAL) != 1) conn_close();
  }
#endif
}

static void send_obj(cJSON* o) {
  char* s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  if (!s) return;
  lock();
  send_line_locked(s);
  unlock();
  free(s);
}

static cJSON* msg(const char* type) {
  cJSON* o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "t", type);
  return o;
}

/* Reads what has arrived without waiting. Returns bytes read, 0 for none, -1 when closed. */
static int conn_read(char* buf, int cap) {
#ifdef _WIN32
  DWORD avail = 0, got = 0;
  if (!PeekNamedPipe(g.conn, NULL, 0, NULL, &avail, NULL)) return -1;
  if (avail == 0) return 0;
  if (avail > (DWORD)cap) avail = (DWORD)cap;
  if (!ReadFile(g.conn, buf, avail, &got, NULL)) return -1;
  return (int)got;
#else
  ssize_t r = recv(g.conn, buf, (size_t)cap, 0);
  if (r > 0) return (int)r;
  if (r == 0) return -1;
  return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
#endif
}

/* ---- the memory rules (rcheevos) ---------------------------------------- */

static uint32_t peek(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud) {
  (void)ud;
  return g.read ? g.read(address, buffer, num_bytes, g.read_ud) : 0;
}

static void report_unlock(uint32_t id) {
  cJSON* o = msg("unlock");
  cJSON_AddNumberToObject(o, "id", id);
  cJSON_AddNumberToObject(o, "frame", (double)g.frame);
  send_obj(o);
  if (g.on_unlock) g.on_unlock(id, g.on_unlock_ud);
}

static void on_event(const rc_runtime_event_t* e) {
  if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) {
    report_unlock(e->id);
    rc_runtime_deactivate_achievement(&g.runtime, e->id);
    if (g.rules > 0) g.rules--;
  } else if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_PROGRESS_UPDATED) {
    unsigned value = 0, target = 0;
    if (rc_runtime_get_achievement_measured(&g.runtime, e->id, &value, &target) && target) {
      cJSON* o = msg("progress");
      cJSON_AddNumberToObject(o, "id", e->id);
      cJSON_AddNumberToObject(o, "value", value);
      cJSON_AddNumberToObject(o, "target", target);
      send_obj(o);
    }
  } else if (e->type == RC_RUNTIME_EVENT_LBOARD_STARTED || e->type == RC_RUNTIME_EVENT_LBOARD_CANCELED) {
    cJSON* o = msg(e->type == RC_RUNTIME_EVENT_LBOARD_STARTED ? "lboard_start" : "lboard_cancel");
    cJSON_AddNumberToObject(o, "id", e->id);
    cJSON_AddNumberToObject(o, "frame", (double)g.frame);
    send_obj(o);
  } else if (e->type == RC_RUNTIME_EVENT_LBOARD_TRIGGERED) {
    cJSON* o = msg("lboard");
    cJSON_AddNumberToObject(o, "id", e->id);
    cJSON_AddNumberToObject(o, "value", e->value);
    cJSON_AddNumberToObject(o, "frame", (double)g.frame);
    send_obj(o);
  } else if (e->type == RC_RUNTIME_EVENT_LBOARD_DISABLED) {
    cJSON* o = msg("broken");
    cJSON_AddNumberToObject(o, "board", e->id);
    send_obj(o);
  } else if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_DISABLED) {
    /* An address the console doesn't have: the rule is broken for this build. */
    cJSON* o = msg("broken");
    cJSON_AddNumberToObject(o, "id", e->id);
    send_obj(o);
  }
}

static void runtime_reset(void) {
  if (g.runtime_ready) rc_runtime_destroy(&g.runtime);
  rc_runtime_init(&g.runtime);
  g.runtime_ready = 1;
  g.rules = 0;
  g.boards = 0;
  g.has_rp = 0;
  g.presence[0] = 0;
}

/* {"achievements":[{"id":1,"trigger":"0xH0010=7"}, ...]} -- entries without a
 * trigger (native or event ones) are skipped. Already-unlocked ids can be listed
 * in "unlocked":[...] so they aren't checked again. */
static int load_list(const cJSON* root) {
  const cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "achievements");
  const cJSON* done = cJSON_GetObjectItemCaseSensitive(root, "unlocked");
  const cJSON* a;
  runtime_reset();
  cJSON_ArrayForEach(a, list) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(a, "id");
    const cJSON* trig = cJSON_GetObjectItemCaseSensitive(a, "trigger");
    const cJSON* u;
    int skip = 0;
    if (!cJSON_IsNumber(id) || !cJSON_IsString(trig) || !trig->valuestring[0]) continue;
    cJSON_ArrayForEach(u, done) if (cJSON_IsNumber(u) && (uint32_t)u->valuedouble == (uint32_t)id->valuedouble) skip = 1;
    if (skip) continue;
    if (rc_runtime_activate_achievement(&g.runtime, (uint32_t)id->valuedouble, trig->valuestring, NULL, 0) == RC_OK) {
      g.rules++;
    } else {
      cJSON* o = msg("broken");
      cJSON_AddNumberToObject(o, "id", id->valuedouble);
      cJSON_AddStringToObject(o, "why", "rule didn't parse");
      send_obj(o);
    }
  }
  /* Leaderboards: {"id", "mem"} (rcheevos' STA:..::CAN:..::SUB:..::VAL:..) or the four parts on their own. */
  cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(root, "leaderboards")) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(a, "id");
    const cJSON* mem = cJSON_GetObjectItemCaseSensitive(a, "mem");
    const cJSON* sta = cJSON_GetObjectItemCaseSensitive(a, "start");
    const cJSON* can = cJSON_GetObjectItemCaseSensitive(a, "cancel");
    const cJSON* sub = cJSON_GetObjectItemCaseSensitive(a, "submit");
    const cJSON* val = cJSON_GetObjectItemCaseSensitive(a, "value");
    char* built = NULL;
    const char* m = NULL;
    if (!cJSON_IsNumber(id)) continue;
    if (cJSON_IsString(mem) && mem->valuestring[0]) m = mem->valuestring;
    else if (cJSON_IsString(sta) && cJSON_IsString(can) && cJSON_IsString(sub) && cJSON_IsString(val)) {
      size_t n = strlen(sta->valuestring) + strlen(can->valuestring) + strlen(sub->valuestring) + strlen(val->valuestring) + 32;
      built = (char*)malloc(n);
      if (built) { snprintf(built, n, "STA:%s::CAN:%s::SUB:%s::VAL:%s", sta->valuestring, can->valuestring, sub->valuestring, val->valuestring); m = built; }
    }
    if (!m) continue; /* an events leaderboard: the port submits it with rl_ach_lboard */
    if (rc_runtime_activate_lboard(&g.runtime, (uint32_t)id->valuedouble, m, NULL, 0) == RC_OK) g.boards++;
    else {
      cJSON* o = msg("broken");
      cJSON_AddNumberToObject(o, "board", id->valuedouble);
      cJSON_AddStringToObject(o, "why", "leaderboard didn't parse");
      send_obj(o);
    }
    free(built);
  }
  {
    const cJSON* rp = cJSON_GetObjectItemCaseSensitive(root, "rich_presence");
    if (cJSON_IsString(rp) && rp->valuestring[0]) {
      if (rc_runtime_activate_richpresence(&g.runtime, rp->valuestring, NULL, 0) == RC_OK) g.has_rp = 1;
      else {
        cJSON* o = msg("broken");
        cJSON_AddStringToObject(o, "why", "rich presence didn't parse");
        send_obj(o);
      }
    }
  }
  return g.rules;
}

/* ---- messages from the launcher ----------------------------------------- */

static void handle_line(const char* line) {
  cJSON* m = cJSON_Parse(line);
  const cJSON* t;
  if (!m) return;
  t = cJSON_GetObjectItemCaseSensitive(m, "t");
  if (cJSON_IsString(t)) {
    if (strcmp(t->valuestring, "list") == 0) {
      int n = load_list(m);
      cJSON* o = msg("list_ok");
      cJSON_AddNumberToObject(o, "rules", n);
      cJSON_AddNumberToObject(o, "boards", g.boards);
      cJSON_AddBoolToObject(o, "presence", g.has_rp);
      send_obj(o);
    } else if (strcmp(t->valuestring, "quiet") == 0) {
      g.quiet = 1;
    } else if (strcmp(t->valuestring, "peek") == 0 && g.read) {
      /* The launcher's dev tools: read a little memory for the memory viewer. */
      const cJSON* ad = cJSON_GetObjectItemCaseSensitive(m, "address");
      const cJSON* sz = cJSON_GetObjectItemCaseSensitive(m, "size");
      const cJSON* sq = cJSON_GetObjectItemCaseSensitive(m, "seq");
      uint8_t buf[RL_MAX_PEEK];
      char hex[RL_MAX_PEEK * 2 + 1];
      uint32_t n = cJSON_IsNumber(sz) ? (uint32_t)sz->valuedouble : 16, got, i;
      if (n > RL_MAX_PEEK) n = RL_MAX_PEEK;
      got = cJSON_IsNumber(ad) ? g.read((uint32_t)ad->valuedouble, buf, n, g.read_ud) : 0;
      for (i = 0; i < got; i++) sprintf(hex + i * 2, "%02x", buf[i]);
      hex[got * 2] = 0;
      {
        cJSON* o = msg("mem");
        if (cJSON_IsNumber(sq)) cJSON_AddNumberToObject(o, "seq", sq->valuedouble);
        cJSON_AddStringToObject(o, "hex", hex);
        send_obj(o);
      }
    }
  }
  cJSON_Delete(m);
}

static void pump(void) {
  char chunk[8192];
  int r;
  if (!g.inbuf || g.conn == RL_NO_CONN) return;
  for (;;) {
    lock();
    r = g.conn == RL_NO_CONN ? -1 : conn_read(chunk, (int)sizeof chunk);
    if (r < 0) conn_close();
    unlock();
    if (r <= 0) return;
    if (g.inlen + (size_t)r > RL_MAX_LINE) { g.inlen = 0; continue; } /* oversized: drop it */
    memcpy(g.inbuf + g.inlen, chunk, (size_t)r);
    g.inlen += (size_t)r;
    for (;;) {
      char* nl = memchr(g.inbuf, '\n', g.inlen);
      size_t len;
      if (!nl) break;
      *nl = 0;
      len = (size_t)(nl - g.inbuf) + 1;
      handle_line(g.inbuf);
      memmove(g.inbuf, g.inbuf + len, g.inlen - len);
      g.inlen -= len;
    }
  }
}

static void send_natives(void) {
  cJSON* o;
  if (!g.natives_dirty || !g.natives || g.conn == RL_NO_CONN) return;
  o = msg("native_list");
  cJSON_AddItemToObject(o, "achievements", cJSON_Duplicate(g.natives, 1));
  send_obj(o);
  g.natives_dirty = 0;
}

/* ---- public calls -------------------------------------------------------- */

void rl_ach_set_info(const char* game_id, const char* version) {
  snprintf(g.game, sizeof g.game, "%s", game_id ? game_id : "");
  snprintf(g.version, sizeof g.version, "%s", version ? version : "");
}

int rl_ach_init(const char* console, rl_ach_read_fn read, void* ud) {
  const char* session = getenv("RL_SESSION");
  lock_init();
  if (g.ready) return g.conn != RL_NO_CONN;
  g.conn = RL_NO_CONN;
  g.ready = 1;
  g.read = read;
  g.read_ud = ud;
  snprintf(g.console, sizeof g.console, "%s", console ? console : "");
  runtime_reset();
  if (!session || !*session) return 0;
  g.conn = conn_open(session);
  if (g.conn == RL_NO_CONN) return 0;
  g.inbuf = (char*)malloc(RL_MAX_LINE);
  if (!g.inbuf) { conn_close(); return 0; }
  {
    cJSON* o = msg("hello");
    cJSON_AddNumberToObject(o, "sdk", RL_ACH_SDK_VERSION);
    cJSON_AddStringToObject(o, "console", g.console);
    cJSON_AddBoolToObject(o, "memory", read != NULL);
    if (g.game[0]) cJSON_AddStringToObject(o, "game", g.game);
    if (g.version[0]) cJSON_AddStringToObject(o, "version", g.version);
    send_obj(o);
  }
  send_natives();
  return 1;
}

void rl_ach_frame(void) {
  if (!g.ready) return;
  g.frame++;
  pump();
  send_natives();
  if ((g.rules > 0 || g.boards > 0 || g.has_rp) && g.read) rc_runtime_do_frame(&g.runtime, on_event, peek, NULL, NULL);
  /* Rich presence: checked about every 5 seconds (300 frames), sent only when it changes. */
  if (g.has_rp && g.read && g.frame - g.presence_at >= 300) {
    char buf[256];
    g.presence_at = g.frame;
    if (rc_runtime_get_richpresence(&g.runtime, buf, sizeof buf, peek, NULL, NULL) > 0) rl_ach_presence(buf);
  }
}

void rl_ach_lboard(uint32_t id, int32_t value) {
  cJSON* o;
  if (!g.ready) return;
  o = msg("lboard");
  cJSON_AddNumberToObject(o, "id", id);
  cJSON_AddNumberToObject(o, "value", value);
  cJSON_AddNumberToObject(o, "frame", (double)g.frame);
  send_obj(o);
}

void rl_ach_presence(const char* text) {
  cJSON* o;
  if (!g.ready || !text) return;
  if (strncmp(text, g.presence, sizeof g.presence - 1) == 0) return;
  strncpy(g.presence, text, sizeof g.presence - 1);
  g.presence[sizeof g.presence - 1] = 0;
  o = msg("presence");
  cJSON_AddStringToObject(o, "text", g.presence);
  send_obj(o);
}

void rl_ach_native(uint32_t id) {
  cJSON* o;
  if (!g.ready) return;
  o = msg("native");
  cJSON_AddNumberToObject(o, "id", id);
  cJSON_AddNumberToObject(o, "frame", (double)g.frame);
  send_obj(o);
  if (g.on_unlock) g.on_unlock(id, g.on_unlock_ud);
}

void rl_ach_native_define(uint32_t id, const char* title, const char* description, uint32_t gamerscore) {
  cJSON* a;
  lock_init();
  if (!g.natives) g.natives = cJSON_CreateArray();
  if (!g.natives) return;
  a = cJSON_CreateObject();
  cJSON_AddNumberToObject(a, "id", id);
  cJSON_AddStringToObject(a, "title", title ? title : "");
  cJSON_AddStringToObject(a, "description", description ? description : "");
  cJSON_AddNumberToObject(a, "gamerscore", gamerscore);
  cJSON_AddItemToArray(g.natives, a);
  g.natives_dirty = 1;
  if (g.ready) send_natives();
}

void rl_ach_native_earlier(uint32_t id) {
  cJSON* o;
  if (!g.ready) return;
  o = msg("native");
  cJSON_AddNumberToObject(o, "id", id);
  cJSON_AddBoolToObject(o, "earlier", 1);
  send_obj(o);
}

void rl_ach_event(const char* name) {
  cJSON* o;
  if (!g.ready || !name) return;
  o = msg("event");
  cJSON_AddStringToObject(o, "name", name);
  cJSON_AddNumberToObject(o, "frame", (double)g.frame);
  send_obj(o);
}

void rl_ach_save_loaded(void) {
  cJSON* o;
  if (!g.ready) return;
  o = msg("save_loaded");
  cJSON_AddNumberToObject(o, "frame", (double)g.frame);
  send_obj(o);
}

void rl_ach_casual(const char* reason) {
  cJSON* o;
  if (!g.ready) return;
  o = msg("casual");
  cJSON_AddStringToObject(o, "reason", reason ? reason : "");
  send_obj(o);
}

int rl_ach_quiet_toasts(void) { return g.quiet; }
int rl_ach_connected(void) { return g.ready && g.conn != RL_NO_CONN; }

int rl_ach_load_list(const char* json) {
  cJSON* m = cJSON_Parse(json);
  int n;
  if (!m) return 0;
  if (!g.ready) { lock_init(); runtime_reset(); }
  n = load_list(m);
  cJSON_Delete(m);
  return n;
}

void rl_ach_on_unlock(rl_ach_unlock_fn fn, void* ud) { g.on_unlock = fn; g.on_unlock_ud = ud; }

void rl_ach_shutdown(void) {
  lock();
  conn_close();
  unlock();
  if (g.runtime_ready) rc_runtime_destroy(&g.runtime);
  g.runtime_ready = 0;
  free(g.inbuf);
  g.inbuf = NULL;
  cJSON_Delete(g.natives);
  g.natives = NULL;
  g.natives_dirty = 0;
  g.inlen = 0;
  g.ready = 0;
  g.rules = 0;
}
