/* Track V, session 11: the CONTAINER STORE made by the host itself (included by nvn_vk_m4.c; not a build unit of its own).
 * While a build is under way the folder holds "store_building.txt"; a start that finds it builds again. */
#include "hostrt.h"

/* [witchtime] B3's store: the shader archives of root_000.pkz (every entry whose name holds "shader": shader.dat,
 * shadereff*.dat, ...: 58 MB) decoded by the game's own Oodle decoder (pkz_dump.c) into `dir`; prep_main walks them with
 * dat_walk. Returns 1 when every one was written; the building marker stays otherwise, so the next start builds again. */
int pkz_dump(const char *out, char **filters, int nfilters);
static int vks_build_pkz(const char *dir) {
    char mark[400], *filter = "shader";
    snprintf(mark, sizeof mark, "%s/store_building.txt", dir);
    CreateDirectoryA("../cache", NULL); CreateDirectoryA(dir, NULL);
    FILE *f = fopen(mark, "wb");
    if (!f) { printf("nvn_vk_store: the folder %s cannot be written: no container store\n", dir); return 0; }
    fputs("the container store is being written\n", f); fclose(f);
    if (pkz_dump(dir, &filter, 1)) { printf("nvn_vk_store: the shader archives of the game's data could not all be read (pkz-dump lines above): the next start tries again\n"); return 0; }
    DeleteFileA(mark);
    return 1;
}
