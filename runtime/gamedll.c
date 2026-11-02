/* Host side of the exe / game.dll split (see game_shim.c).
 *
 * build.py compiles every host file with
 *     -Dfn_table=(*game_fn_table_p) -Dfn_table_count=(*game_fn_table_count_p)
 *     -Ddyn_syms=(*game_dyn_syms_p) -Ddyn_sym_count=(*game_dyn_sym_count_p)
 * so the tables declared in runtime.h become pointers into the DLL, filled in here before main(). */
#include "runtime.h"

typedef struct {
    void (*call_indirect)(Ctx *, uint64_t);
    void (*host_import)(Ctx *, int);
    void (*guest_trap)(const char *, uint64_t);
    void (*guest_unimpl)(uint64_t, uint32_t, const char *);
    void (*guest_svc)(Ctx *, uint32_t);
    void (*stx_diag)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
    void (*sdk_dispatch)(Ctx *, uint64_t);           /* [witchtime] game_shim.c */
    void (*sdk_host_call)(Ctx *, int);
    void (*sdk_trap_call)(Ctx *, const char *);
} GameHost;

typedef struct {
    const FnEntry *table;
    const int *table_count;
    const DynSym *syms;
    const int *sym_count;
} GameTables;

void sdk_dispatch(Ctx *c, uint64_t addr);     /* sdklift_rt.c */
void sdk_host_call(Ctx *c, int index);
void sdk_trap_call(Ctx *c, const char *name);
__declspec(dllimport) void game_bind(const GameHost *h, GameTables *t);

#undef fn_table
#undef fn_table_count
#undef dyn_syms
#undef dyn_sym_count
const FnEntry (*game_fn_table_p)[];
const int *game_fn_table_count_p;
const DynSym (*game_dyn_syms_p)[];
const int *game_dyn_sym_count_p;

__attribute__((constructor)) static void bind_game(void) {
    GameHost h = {call_indirect, host_import, guest_trap, guest_unimpl, guest_svc, stx_diag, sdk_dispatch, sdk_host_call, sdk_trap_call};
    GameTables t;
    game_bind(&h, &t);
    game_fn_table_p = (const FnEntry (*)[])t.table;
    game_fn_table_count_p = t.table_count;
    game_dyn_syms_p = (const DynSym (*)[])t.syms;
    game_dyn_sym_count_p = t.sym_count;
}
