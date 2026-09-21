#ifndef PAPRI_POOL_H
#define PAPRI_POOL_H

#include <stddef.h>

/* Slab allocator for rope nodes and leaves.
 *
 * Every allocation is POOL_ALIGNMENT-aligned, because a rope leaf is exactly
 * one cache line and must never straddle two. That is a correctness property
 * of this module, not a nicety.
 *
 * M1 hands memory out and never takes it back; the pool grows until released.
 * M3 adds reclamation, at which point `residual` below stops being trivially
 * the full share of everything.
 */

#define POOL_ALIGNMENT 64

/* ── Abstract predicates ────────────────────────────────────────────────────
 * node_pool(pool, live, residual)
 *   Owns its slabs and the slab directory. `live` is the set of allocations
 *   handed out, each POOL_ALIGNMENT-aligned and pairwise disjoint. For each
 *   allocation in `live`, `residual` is the complement of every share handed
 *   out; an allocation whose residual is the full share is dead and may be
 *   freed. Until M3 nothing is freed, so `residual` is the full share of
 *   nothing and `live` only grows.
 */

typedef struct Pool {
    unsigned char **slabs;
    size_t          slab_count;
    size_t          slab_capacity;
    unsigned char  *current;
    size_t          current_used;
    size_t          current_size;
    size_t          handed_out;
} Pool;

/* requires: *pool is allocated and writable.
 * ensures:  node_pool(pool, live, residual) with `live` empty, and the result
 *           is 1; or nothing is allocated and the result is 0.
 */
int pool_initialize(Pool *pool);

/* requires: node_pool(pool, live, residual).
 * ensures:  every slab is freed and *pool is reset to the state
 *           pool_initialize leaves it in. Pointers previously handed out are
 *           dangling; the caller must have dropped them.
 */
void pool_release(Pool *pool);

/* requires: node_pool(pool, live, residual); 0 < size <= POOL_SLAB_BYTES.
 * ensures:  node_pool(pool, live', residual) where live' is live plus one
 *           fresh allocation of at least `size` bytes, aligned to
 *           POOL_ALIGNMENT and disjoint from every other allocation, and the
 *           result points at it; or the allocation failed, live' is live, and
 *           the result is null. The bytes are zeroed.
 */
void *pool_allocate(Pool *pool, size_t size);

/* requires: node_pool(pool, live, residual).
 * ensures:  node_pool(pool, live, residual); the result is the total size of
 *           `live`, rounded up per allocation to POOL_ALIGNMENT. No memory is
 *           written.
 */
size_t pool_handed_out(const Pool *pool);

/* requires: node_pool(pool, live, residual).
 * ensures:  node_pool(pool, live, residual); the result is the total size of
 *           the slabs held, which is at least pool_handed_out. No memory is
 *           written.
 */
size_t pool_reserved(const Pool *pool);

#endif /* PAPRI_POOL_H */
