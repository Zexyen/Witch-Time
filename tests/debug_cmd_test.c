/* runtime/debug_cmd.h: the names the debug commands take.
 *   clang -O1 -Wall -I runtime tests/debug_cmd_test.c -o /tmp/debug_cmd_test && /tmp/debug_cmd_test */
#include <assert.h>
#include <stdio.h>
#include "debug_cmd.h"

int main(void) {
    assert(dbg_obj_id("em0010") == 0x20010);
    assert(dbg_obj_id("pl3000") == 0x13000);
    assert(dbg_obj_id("ba0000") == 0xf0000);
    assert(dbg_obj_id("20010") == 0x20010);
    assert(dbg_obj_id("ef001") == 0xef001 && dbg_obj_id("em") == 0 && dbg_obj_id("em00100") == 0 && dbg_obj_id("emzz") == 0 && dbg_obj_id("10") == 0 && dbg_obj_id("") == 0);
    assert(dbg_phase_of("p9a0_training") == 0x9a0);
    assert(dbg_phase_of("pc20_ChapterSelect") == 0xc20);
    assert(dbg_phase_of("training") == -1 && dbg_phase_of("p9") == -1 && dbg_phase_of("pxyz_a") == -1);
    puts("debug_cmd: ok");
    return 0;
}
