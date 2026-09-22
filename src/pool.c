#include "pool.h"

#include "collector.h"

/* requires: as pool.h.
 * ensures:  as pool.h.
 */
int pool_initialize(Pool *pool)
{
    collector_initialize();
    pool->handed_out = 0;
    pool->requests = 0;
    return 1;
}

/* requires: as pool.h.
 * ensures:  as pool.h.
 */
void pool_release(Pool *pool)
{
    pool->handed_out = 0;
    pool->requests = 0;
}

/* requires: as pool.h.
 * ensures:  as pool.h.
 */
void *pool_allocate(Pool *pool, size_t size)
{
    void  *block;
    size_t total;
    size_t count;

    if (size == 0) {
        return NULL;
    }

    block = collector_allocate(size);
    if (block == NULL) {
        return NULL;
    }

    total = pool->handed_out;
    pool->handed_out = total + size;
    count = pool->requests;
    pool->requests = count + 1;
    return block;
}

/* requires: as pool.h.
 * ensures:  as pool.h.
 */
void *pool_allocate_atomic(Pool *pool, size_t size)
{
    void  *block;
    size_t total;
    size_t count;

    if (size == 0) {
        return NULL;
    }

    block = collector_allocate_atomic(size);
    if (block == NULL) {
        return NULL;
    }

    total = pool->handed_out;
    pool->handed_out = total + size;
    count = pool->requests;
    pool->requests = count + 1;
    return block;
}

/* requires: as pool.h.
 * ensures:  as pool.h.
 */
size_t pool_handed_out(const Pool *pool)
{
    size_t total;

    total = pool->handed_out;
    return total;
}

/* requires: as pool.h.
 * ensures:  as pool.h.
 */
size_t pool_requests(const Pool *pool)
{
    size_t count;

    count = pool->requests;
    return count;
}

/* requires: as pool.h.
 * ensures:  as pool.h.
 */
void pool_collect(Pool *pool)
{
    (void)pool;
    collector_collect();
}

/* requires: as pool.h.
 * ensures:  as pool.h.
 */
size_t pool_live(const Pool *pool)
{
    size_t bytes;

    (void)pool;
    bytes = collector_live_bytes();
    return bytes;
}

/* requires: as pool.h.
 * ensures:  as pool.h.
 */
size_t pool_reserved(const Pool *pool)
{
    size_t bytes;

    (void)pool;
    bytes = collector_heap_bytes();
    return bytes;
}
