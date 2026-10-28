// The harness's heap: the board's allocator (dlmalloc, configured as in
// port/m9b_malloc.c) over an arena of the board's heap size, so what is lost
// to holes and per-block overhead counts as it does there. Prefixed (dlmalloc,
// dlfree, ...): host.cc's malloc wrappers call it.
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#define USE_DL_PREFIX 1
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define MORECORE host_morecore
#define MORECORE_CANNOT_TRIM 1
#define USE_LOCKS 0
#define NO_MALLOC_STATS 1
#define DEFAULT_GRANULARITY 4096
#define malloc_getpagesize 4096

static char *arena;
static size_t arena_size, arena_top;

// sbrk over the arena: its size from U7_HEAP_KB (default: the board's
// profile build's, 0xFE0000 - 0xA4B018; the release build has 256 KB
// more), made on first use (static constructors allocate before main).
static void *host_morecore(intptr_t inc) {
    if (!arena) {
        const char *kb = getenv("U7_HEAP_KB");
        arena_size = kb ? (size_t)atoi(kb) * 1024u : 0x594FE8u;
        arena = mmap(NULL, arena_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (arena == MAP_FAILED) abort();
    }
    if (inc < 0 || arena_top + (size_t)inc > arena_size) return (void *)-1;
    void *p = arena + arena_top;
    arena_top += inc;
    return p;
}

size_t host_heap_top(void) { return arena_top; }
size_t host_heap_size(void) { return arena_size; }

#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "../../port/dlmalloc/malloc.c"
