#include "pool.h"

#include <stdlib.h>
#include <string.h>

#define POOL_SLAB_BYTES      (1024u * 1024u)
#define POOL_DIRECTORY_FLOOR 16u

/* requires: size > 0.
 * ensures:  the result is size rounded up to a multiple of POOL_ALIGNMENT.
 *           No memory is accessed.
 */
static size_t round_up_to_alignment(size_t size)
{
    size_t remainder;
    size_t padding;

    remainder = size % POOL_ALIGNMENT;
    if (remainder == 0) {
        return size;
    }
    padding = POOL_ALIGNMENT - remainder;
    return size + padding;
}

/* requires: node_pool(pool, live, residual).
 * ensures:  node_pool(pool, live, residual) with room in the slab directory
 *           for one more slab, and the result is 1; or the directory is
 *           unchanged and the result is 0.
 */
static int reserve_directory_slot(Pool *pool)
{
    size_t count;
    size_t capacity;
    size_t wanted;
    unsigned char **grown;
    unsigned char **existing;

    count = pool->slab_count;
    capacity = pool->slab_capacity;
    if (count < capacity) {
        return 1;
    }

    wanted = capacity * 2;
    if (wanted < POOL_DIRECTORY_FLOOR) {
        wanted = POOL_DIRECTORY_FLOOR;
    }

    existing = pool->slabs;
    grown = realloc(existing, wanted * sizeof(unsigned char *));
    if (grown == NULL) {
        return 0;
    }

    pool->slabs = grown;
    pool->slab_capacity = wanted;
    return 1;
}

/* requires: node_pool(pool, live, residual).
 * ensures:  node_pool(pool, live, residual) with a fresh empty slab installed
 *           as the current one, and the result is 1; or the pool is unchanged
 *           and the result is 0.
 */
static int add_slab(Pool *pool)
{
    unsigned char *slab;
    unsigned char **directory;
    size_t count;
    int reserved;

    reserved = reserve_directory_slot(pool);
    if (reserved == 0) {
        return 0;
    }

    /* aligned_alloc, not malloc: a leaf must start on a cache line. */
    slab = aligned_alloc(POOL_ALIGNMENT, POOL_SLAB_BYTES);
    if (slab == NULL) {
        return 0;
    }

    count = pool->slab_count;
    directory = pool->slabs;
    directory[count] = slab;
    pool->slab_count = count + 1;
    pool->current = slab;
    pool->current_used = 0;
    pool->current_size = POOL_SLAB_BYTES;
    return 1;
}

/* requires: *pool is allocated and writable.
 * ensures:  node_pool(pool, live, residual) with `live` empty, and the result
 *           is 1; or nothing is allocated and the result is 0.
 */
int pool_initialize(Pool *pool)
{
    size_t index;

    pool->slabs = NULL;
    pool->slab_count = 0;
    pool->slab_capacity = 0;
    pool->current = NULL;
    pool->current_used = 0;
    pool->current_size = 0;
    pool->handed_out = 0;
    pool->live = 0;
    index = 0;
    while (index < POOL_SIZE_CLASSES) {
        pool->free_list[index] = NULL;
        index = index + 1;
    }
    return 1;
}

/* requires: node_pool(pool, live, residual).
 * ensures:  every slab is freed and *pool is reset to the state
 *           pool_initialize leaves it in.
 */
void pool_release(Pool *pool)
{
    size_t index;
    size_t count;
    unsigned char *slab;
    unsigned char **slabs;

    count = pool->slab_count;
    index = 0;
    while (index < count) {
        slabs = pool->slabs;
        slab = slabs[index];
        free(slab);
        index = index + 1;
    }

    slabs = pool->slabs;
    free(slabs);
    pool_initialize(pool);
}

/* requires: 0 < size, and its class is below POOL_SIZE_CLASSES.
 * ensures:  the result is that class index. No memory is accessed.
 */
static size_t class_of(size_t size)
{
    size_t rounded;
    size_t index;

    rounded = round_up_to_alignment(size);
    index = rounded / POOL_ALIGNMENT;
    return index - 1;
}

/* requires: node_pool(pool, live, residual); 0 < size <= POOL_SLAB_BYTES.
 * ensures:  node_pool(pool, live', residual) where live' is live plus one
 *           fresh zeroed allocation of at least `size` bytes, aligned to
 *           POOL_ALIGNMENT, reusing a freed block of the same class when one
 *           is available, and the result points at it; or the allocation
 *           failed, live' is live, and the result is null.
 */
void *pool_allocate(Pool *pool, size_t size)
{
    size_t wanted;
    size_t used;
    size_t available;
    size_t capacity;
    size_t class_index;
    unsigned char *base;
    unsigned char *block;
    void *recycled;
    void *next;
    int added;

    if (size == 0) {
        return NULL;
    }

    wanted = round_up_to_alignment(size);
    if (wanted > POOL_SLAB_BYTES) {
        return NULL;
    }

    class_index = class_of(size);
    if (class_index < POOL_SIZE_CLASSES) {
        recycled = pool->free_list[class_index];
        if (recycled != NULL) {
            next = *(void **)recycled;
            pool->free_list[class_index] = next;
            used = pool->live;
            pool->live = used + wanted;
            memset(recycled, 0, wanted);
            return recycled;
        }
    }

    used = pool->current_used;
    capacity = pool->current_size;
    available = capacity - used;
    if (available < wanted) {
        added = add_slab(pool);
        if (added == 0) {
            return NULL;
        }
        used = 0;
    }

    base = pool->current;
    block = base + used;
    pool->current_used = used + wanted;

    used = pool->handed_out;
    pool->handed_out = used + wanted;
    used = pool->live;
    pool->live = used + wanted;

    memset(block, 0, wanted);
    return block;
}

/* requires: as pool.h.
 * ensures:  as pool.h.
 */
void pool_free(Pool *pool, void *block, size_t size)
{
    size_t wanted;
    size_t class_index;
    size_t used;
    void  *head;

    if (block == NULL) {
        return;
    }
    wanted = round_up_to_alignment(size);
    class_index = class_of(size);
    if (class_index >= POOL_SIZE_CLASSES) {
        return;
    }

    /* The free-list link lives inside the block, which is at least one
     * alignment wide, so it always fits. */
    head = pool->free_list[class_index];
    *(void **)block = head;
    pool->free_list[class_index] = block;

    used = pool->live;
    pool->live = used - wanted;
}

/* requires: node_pool(pool, live, residual).
 * ensures:  as pool.h.
 */
size_t pool_live(const Pool *pool)
{
    size_t total;

    total = pool->live;
    return total;
}

/* requires: node_pool(pool, live, residual).
 * ensures:  node_pool(pool, live, residual); the result is the total size
 *           handed out. No memory is written.
 */
size_t pool_handed_out(const Pool *pool)
{
    size_t total;

    total = pool->handed_out;
    return total;
}

/* requires: node_pool(pool, live, residual).
 * ensures:  node_pool(pool, live, residual); the result is the total size of
 *           the slabs held. No memory is written.
 */
size_t pool_reserved(const Pool *pool)
{
    size_t count;

    count = pool->slab_count;
    return count * POOL_SLAB_BYTES;
}
