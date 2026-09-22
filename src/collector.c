/* The garbage collector boundary. See src/collector.h for why this file
 * exists and why it is exempt from `make normalform`.
 *
 * GC_THREADS is deliberately NOT defined. papri runs on one thread — see
 * src/io.h — and a single-threaded Boehm never signals anybody, so the
 * usual hazard of a stop-the-world suspending a blocking syscall does not
 * arise. If papri ever grows a second thread, three things change together:
 * define GC_THREADS here (which also redirects dlopen, and src/structure.c
 * dlopens grammars), create threads through pthread_create so the collector
 * learns their stacks, and make io_wait retry on EINTR.
 */

#include "collector.h"

#include <string.h>

#include <gc.h>

/* requires: as collector.h.
 * ensures:  as collector.h.
 */
void collector_initialize(void)
{
    GC_INIT();
}

/* requires: as collector.h.
 * ensures:  as collector.h.
 */
void *collector_allocate(size_t size)
{
    /* GC_malloc clears what it returns, and the block is scanned. */
    return GC_MALLOC(size);
}

/* requires: as collector.h.
 * ensures:  as collector.h.
 */
void *collector_allocate_atomic(size_t size)
{
    void *block;

    /* GC_malloc_atomic does not clear — unlike GC_malloc — so do it here.
     * The zeroed guarantee is part of the contract: a leaf that is only
     * partly written must not expose whatever the last dead leaf held. */
    block = GC_MALLOC_ATOMIC(size);
    if (block == NULL) {
        return NULL;
    }
    memset(block, 0, size);
    return block;
}

/* requires: as collector.h.
 * ensures:  as collector.h.
 */
void collector_collect(void)
{
    GC_gcollect();
}

/* requires: as collector.h.
 * ensures:  as collector.h.
 */
size_t collector_heap_bytes(void)
{
    return GC_get_heap_size();
}

/* requires: as collector.h.
 * ensures:  as collector.h.
 */
size_t collector_live_bytes(void)
{
    size_t heap;
    size_t free_bytes;

    heap = GC_get_heap_size();
    free_bytes = GC_get_free_bytes();
    if (free_bytes > heap) {
        return 0;
    }
    return heap - free_bytes;
}
