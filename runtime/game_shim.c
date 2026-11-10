/* Linked into game.dll together with the lifted game code (obj\*.obj).
 *
 * The lifted code is 700+ MB of objects that change only when the lifter changes; keeping it in a
 * DLL means the host runtime (the part edited all day) links in a second or two. The DLL cannot
 * import from the exe, so the six host functions the lifted code calls are forwarded through
 * pointers the exe hands over once at start-up (gamedll.c). */
#include "runtime.h"

typedef struct {
    void (*call_indirect)(Ctx *, uint64_t);
    void (*host_import)(Ctx *, int);
    void (*guest_trap)(const char *, uint64_t);
    void (*guest_unimpl)(uint64_t, uint32_t, const char *);
    void (*guest_svc)(Ctx *, uint32_t);
    void (*stx_diag)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
    void (*sdk_dispatch)(Ctx *, uint64_t);           /* [witchtime] for gen/sdklift_gen.c, which is in the game library now */
    void (*sdk_host_call)(Ctx *, int);
    void (*sdk_trap_call)(Ctx *, const char *);
} GameHost;

typedef struct {
    const FnEntry *fn_table;
    const int *fn_table_count;
    const DynSym *dyn_syms;
    const int *dyn_sym_count;
} GameTables;

static GameHost host;

void call_indirect(Ctx *c, uint64_t addr) { host.call_indirect(c, addr); }
void host_import(Ctx *c, int sym) { host.host_import(c, sym); }
void guest_trap(const char *what, uint64_t addr) { host.guest_trap(what, addr); }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { host.guest_unimpl(pc, insn, what); }
void guest_svc(Ctx *c, uint32_t number) { host.guest_svc(c, number); }
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) { host.stx_diag(c, a, expected, seen, v, bits); }
#ifdef _WIN32
/* [witchtime] the lifted sdk (gen/sdklift_gen.c) is linked into game.dll with the player's other tables (runtime/split);
 * on Linux game.so finds these three in the runtime when it is loaded */
void sdk_dispatch(Ctx *c, uint64_t addr) { host.sdk_dispatch(c, addr); }
void sdk_host_call(Ctx *c, int index) { host.sdk_host_call(c, index); }
void sdk_trap_call(Ctx *c, const char *name) { host.sdk_trap_call(c, name); }
#endif
#ifdef WT_GAME_NOSTDLIB
/* [witchtime] game.dll as wtbuild links it on the player's PC: without the C runtime's start-up code (-nostdlib), so its
 * entry point is this; nothing in it needs initialising (no constructors, no thread-local data) */
int __stdcall DllMainCRTStartup(void *module, unsigned long reason, void *reserved) {
    (void)module; (void)reason; (void)reserved;
    return 1;
}
#endif

#if defined(__x86_64__)
#include "soft_fmaf.h"
#include <stdlib.h>
/* [witchtime] every fmaf call of the lifted code (soft_fmaf.h). Hidden: on ELF a call from inside game.so would otherwise go to the C
 * library's. WT_NO_FMA3=1 (test): the path of a CPU without FMA3 on one that has it. */
__attribute__((visibility("hidden"))) float fmaf(float x, float y, float z) {
    static int hw = -1;     /* whichever thread comes first sets it, each to the same value */
    if (hw < 0) hw = wt_has_fma3() && !getenv("WT_NO_FMA3");
    return hw ? wt_fmaf_hw(x, y, z) : wt_fmaf_soft(x, y, z);
}
#endif

__declspec(dllexport) void game_bind(const GameHost *h, GameTables *t) {
    host = *h;
    t->fn_table = fn_table;
    t->fn_table_count = &fn_table_count;
    t->dyn_syms = dyn_syms;
    t->dyn_sym_count = &dyn_sym_count;
}
