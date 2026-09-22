#ifndef PAPRI_COLLECTOR_H
#define PAPRI_COLLECTOR_H

#include <stddef.h>

/* The garbage collector, behind an opaque boundary.
 *
 * papri's buffers are a DAG: versions share nodes, and a node dies only when
 * the last version reaching it retires. Finding those nodes by walking the
 * retiring version against its successor was tried and is unsound for a set
 * of dead roots — see CLAUDE.md, "The shortcut that does not work". So the
 * job goes to a tracing collector, which has the one thing the walk lacked:
 * it sees every root at once.
 *
 * The collector is Boehm-Demers-Weiser, conservative and non-moving. Two
 * properties earn it the job over a precise one:
 *
 *   - It finds roots by scanning the stack, the registers and the data
 *     segment. papri's roots — the Editor, its slots, its histories, its job
 *     snapshots — are inline in a static Editor, so there is nothing to
 *     register and nothing to keep in step. A precise collector would want
 *     every root mirrored into an array before each collection.
 *
 *   - It does not move objects, so a node's address is stable and the
 *     layout we chose stays the layout we get: 256-byte leaves, size tables
 *     inline in the node. A moving collector would need a header word per
 *     object and tagged integers, which is to say a different rope.
 *
 * What it costs: conservatism. A `size_t` in a node's size table that
 * happens to read as a heap address keeps one object alive that is dead.
 * That is false retention, never corruption — the direction of the error
 * that matters. Leaves avoid even that, being allocated atomic and never
 * scanned.
 *
 * This file is one of three foreign boundaries — see src/io.h and
 * src/structure.h — and like them it is exempt from `make normalform` and
 * kept as thin as it can be: nothing here but the wrappers. Everything that
 * reasons about memory lives in src/pool.c, which stays in the subset.
 *
 * ── Abstract predicates ────────────────────────────────────────────────────
 * collector_ready()
 *   The collector has been initialized. Holds from the first
 *   collector_initialize() to the end of the process, and is idempotent:
 *   initializing twice is initializing once.
 *
 * collected(block, size)
 *   `block` points at `size` bytes owned by the collector, and holds them
 *   for as long as any reachable word in the stack, the registers, the data
 *   segment, or another collected block points at it or into it. Unlike an
 *   owned allocation this is not returned — there is no way to give it
 *   back, and no need to.
 */

/* requires: nothing.
 * ensures:  collector_ready(). Safe to call more than once; the second call
 *           and later ones change nothing.
 */
void collector_initialize(void);

/* requires: collector_ready(); size > 0.
 * ensures:  collected(result, size) with every byte zero, and the collector
 *           SCANS those bytes for pointers — so a block allocated this way
 *           may hold references to other collected blocks and will keep them
 *           alive. The result is null when the heap cannot grow.
 */
void *collector_allocate(size_t size);

/* requires: collector_ready(); size > 0.
 * ensures:  collected(result, size) with every byte zero, and the collector
 *           NEVER scans those bytes — so the block must hold no pointer to a
 *           collected block, or that block will be freed underneath it. For
 *           leaf bytes, which are text and nothing else. The result is null
 *           when the heap cannot grow.
 */
void *collector_allocate_atomic(size_t size);

/* requires: collector_ready().
 * ensures:  a full collection has run: every collected block not reachable
 *           from a root is available again. Reachability is decided
 *           conservatively, so some unreachable blocks may survive.
 */
void collector_collect(void);

/* requires: collector_ready().
 * ensures:  the result is the bytes the collector holds from the operating
 *           system, live and free together. No memory is written.
 */
size_t collector_heap_bytes(void);

/* requires: collector_ready().
 * ensures:  the result is the bytes currently spoken for inside that heap.
 *           It is exact only just after collector_collect(); at any other
 *           moment it counts blocks that are already garbage but not yet
 *           found. No memory is written.
 */
size_t collector_live_bytes(void);

#endif /* PAPRI_COLLECTOR_H */
