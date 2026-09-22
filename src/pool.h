#ifndef PAPRI_POOL_H
#define PAPRI_POOL_H

#include <stddef.h>

/* Where rope nodes, leaves and index nodes come from.
 *
 * This was a slab allocator: 1 MiB slabs carved into aligned blocks, with a
 * free list per size class. It is now a thin layer over the collector in
 * src/collector.h, and the change was forced rather than chosen. A slab
 * keeps every block in it reachable from the slab directory, so under a
 * tracing collector nothing in a slab is ever collectable. Bump allocation
 * and garbage collection do not compose; one of them had to go, and it was
 * not going to be the one that makes the leak stop.
 *
 * What is left here is accounting. `handed_out` is the cumulative cost of an
 * edit sequence, which is what the tests measure and what a slab allocator
 * gave for free; the collector cannot report it, so the pool keeps counting.
 *
 * Two entry points, not one, and the difference is load-bearing:
 * pool_allocate is scanned for pointers, pool_allocate_atomic is not. An
 * interior node holds child pointers and must be scanned. A leaf holds text
 * and must not be — 256 bytes of arbitrary file content would otherwise be
 * 32 words of accidental pointers, each pinning a dead node.
 *
 * Alignment is no longer promised. It was, back when a leaf was exactly one
 * 64-byte cache line and the point was that it could never straddle two;
 * leaves are 256 bytes now, so that argument retired with the constant, and
 * nothing in the rope ever depended on the alignment for correctness — only
 * `rope_allocated_bytes` read it, to predict what the slab rounding cost.
 * The collector gives 16-byte alignment, which is what any allocator gives.
 */

/* ── Abstract predicates ────────────────────────────────────────────────────
 * node_pool(pool, allocated)
 *   Owns *pool, which is counters and nothing else — it holds no memory and
 *   frees none. `allocated` is the total the pool has ever asked the
 *   collector for. Blocks handed out are collected(block, size) per
 *   src/collector.h: they live as long as something reachable points at
 *   them, and neither the pool nor its caller can end that.
 */

typedef struct Pool {
    size_t handed_out;    /* cumulative bytes requested, never decreasing */
    size_t requests;      /* cumulative allocations, never decreasing */
} Pool;

/* requires: *pool is allocated and writable.
 * ensures:  node_pool(pool, 0), the collector is initialized, and the result
 *           is 1.
 */
int pool_initialize(Pool *pool);

/* requires: node_pool(pool, allocated).
 * ensures:  node_pool(pool, 0). Nothing is freed — the pool never owned
 *           anything to free. Blocks handed out stay alive exactly as long
 *           as something still points at them, so unlike the slab version
 *           this leaves no dangling pointers behind and a caller that kept
 *           one is still correct.
 */
void pool_release(Pool *pool);

/* requires: node_pool(pool, allocated); size > 0.
 * ensures:  node_pool(pool, allocated + size) and the result is a zeroed
 *           block of `size` bytes that the collector SCANS, so pointers
 *           stored in it keep their targets alive; or the allocation failed,
 *           `allocated` is unchanged, and the result is null.
 */
void *pool_allocate(Pool *pool, size_t size);

/* requires: node_pool(pool, allocated); size > 0.
 * ensures:  node_pool(pool, allocated + size) and the result is a zeroed
 *           block of `size` bytes that the collector NEVER scans — the
 *           caller must store no pointer in it; or the allocation failed,
 *           `allocated` is unchanged, and the result is null.
 */
void *pool_allocate_atomic(Pool *pool, size_t size);

/* requires: node_pool(pool, allocated).
 * ensures:  node_pool(pool, allocated); the result is `allocated`, the total
 *           ever requested. This is a cost meter, not a footprint: it counts
 *           blocks long since collected. No memory is written.
 */
size_t pool_handed_out(const Pool *pool);

/* requires: node_pool(pool, allocated).
 * ensures:  node_pool(pool, allocated); the result is the number of
 *           allocations ever made. No memory is written.
 */
size_t pool_requests(const Pool *pool);

/* requires: node_pool(pool, allocated).
 * ensures:  node_pool(pool, allocated); every block unreachable from a root
 *           has been collected, conservatively. This is the only reason a
 *           caller ever needs to name the collector: tests that assert
 *           memory comes back, and the retirement path when it wants the
 *           space now rather than at the next allocation.
 */
void pool_collect(Pool *pool);

/* requires: node_pool(pool, allocated).
 * ensures:  node_pool(pool, allocated); the result is what the collector
 *           currently holds as live, exact only just after pool_collect.
 *           No memory is written.
 */
size_t pool_live(const Pool *pool);

/* requires: node_pool(pool, allocated).
 * ensures:  node_pool(pool, allocated); the result is the collector's whole
 *           heap, live and free together, which is what papri actually costs
 *           the operating system. No memory is written.
 */
size_t pool_reserved(const Pool *pool);

#endif /* PAPRI_POOL_H */
