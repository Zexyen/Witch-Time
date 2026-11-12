/* macOS build only (-I runtime/macos): glibc's <malloc.h> names the runtime uses, over the system's allocator. Its free
 * and malloc_size accept memory from posix_memalign / aligned_alloc too, as glibc's do. */
#ifndef WT_MACOS_MALLOC_H
#define WT_MACOS_MALLOC_H
#include <stdlib.h>
#include <malloc/malloc.h>
#define malloc_usable_size(p) malloc_size(p)
#endif
