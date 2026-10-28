// The heap: Doug Lea's malloc (port/dlmalloc/malloc.c, 2.8.6, MIT-0) in
// place of picolibc's nano-malloc. Exult makes tens of thousands of small
// objects and frees and remakes buffers of every size as it trims shapes
// and usecode; nano-malloc's first fit over one free list lost about 540 KB
// of the 4.6 MB to holes and per-block overhead (out of memory starting a
// game that needs 4.06 MB). dlmalloc keeps free blocks in size bins (best
// fit), with 4 bytes of overhead a block.
//
// Over sbrk alone (picolibc's, up to the stack), in small steps so the
// last of the heap is usable; single-threaded, no locks.
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define MORECORE_CANNOT_TRIM 1
#define USE_LOCKS 0
#define NO_MALLOC_STATS 1
#define LACKS_TIME_H 1
#define LACKS_SCHED_H 1
#define LACKS_SYS_PARAM_H 1
// (dlmalloc's own struct mallinfo: the same fields as picolibc's <malloc.h>)
#define DEFAULT_GRANULARITY 4096
#define malloc_getpagesize 4096

#pragma GCC diagnostic ignored "-Warray-bounds"   // (its chunk arithmetic: false alarms)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "dlmalloc/malloc.c"

void *aligned_alloc(size_t align, size_t size) { return memalign(align, size); }
