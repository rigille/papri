#ifndef PAPRI_ROPE_H
#define PAPRI_ROPE_H

#include <stdint.h>

#include "pool.h"

/* An immutable byte sequence: a relaxed radix balanced tree over byte chunks.
 *
 * Leaves are exactly ROPE_LEAF_BYTES of payload, which is immer's own sizing
 * rule — big enough that the per-leaf cost of the tree above it disappears,
 * small enough that a path copy stays cheap. They are headerless: no length,
 * no measures, no tag, nothing but the bytes. A child's byte count lives in
 * its parent's size table, which a relaxed tree needs regardless, so nothing
 * is lost by leaving it out of the child. Size tables are kept even on
 * regular nodes so a leaf never has to know its own length.
 *
 * That headerlessness is worth about two points of memory: papri spends 8%
 * on structure where immer spends 10.1%, because immer's leaves carry a
 * reference count and a kind tag and papri's carry neither. It is also why
 * papri cannot reference-count — there is nowhere to put the count — and so
 * why reclamation is a tracing collector's job. See src/collector.h.
 *
 * The rope counts bytes and nothing else. It does not know what a newline
 * is; line addressing lives in src/line_index.h, one layer up, which is
 * what keeps this a plain RRB in immer's shape rather than a bespoke
 * measured tree.
 *
 * Nodes are sealed on construction and never written again. That immutability
 * is what lets two versions hold read shares of one node, which is what makes
 * the whole design work; see CLAUDE.md § Shares.
 */

#define ROPE_LEAF_BYTES 256

/* The same two numbers as powers of two, for the radix descent: a full
 * child of a node at height h covers 2^(ROPE_LEAF_BITS + ROPE_BRANCH_BITS *
 * (h-1)) bytes, which is what makes `offset >> that` a valid lower bound on
 * the child index. */
#define ROPE_LEAF_BITS   8
#define ROPE_BRANCH_BITS 5
#define ROPE_BRANCHING  32
#define ROPE_MAX_HEIGHT 16

/* ── Abstract predicates ────────────────────────────────────────────────────
 * rope_node(node, contents, share)
 *   Holds `share` of *node, sealed and immutable. Read shares split:
 *   rope_node(n, c, s1 ⊕ s2) is rope_node(n, c, s1) * rope_node(n, c, s2).
 *   This is what lets two paths reach one node; a mutable node could not.
 *
 * rope(rope, bytes, share)
 *   *rope is a pure value — a root, a height and the measures of `bytes`.
 *   Holds `share` of every node reachable from that root. `bytes` is the byte
 *   sequence denoted. Two ropes may hold shares of the same nodes; that is
 *   the point.
 *
 * Every operation below takes shares of its inputs and returns them. A
 * derived rope holds shares of whatever nodes it reuses, so no input may be
 * freed while a result derived from it lives — the pool cannot free a node
 * until every share of it has come back.
 */

typedef struct RopeNode RopeNode;

typedef struct Rope {
    void    *root;          /* RopeNode* above height 0, leaf bytes at 0 */
    uint32_t height;        /* 0 when the root is a single leaf or empty */
    size_t   byte_count;
} Rope;

/* requires: *rope is allocated and writable.
 * ensures:  rope(rope, bytes, share) where bytes is empty.
 */
void rope_initialize_empty(Rope *rope);

/* requires: node_pool(pool, live, residual); holds a read share of `length`
 *           bytes at `bytes`; *rope is writable.
 * ensures:  the read share of `bytes` is returned; rope(rope, contents, share)
 *           where contents is a copy of those bytes, and the result is 1; or
 *           allocation failed, *rope is empty, and the result is 0.
 */
int rope_from_bytes(Pool *pool, const unsigned char *bytes, size_t length,
                    Rope *rope);

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is the length of bytes. No
 *           memory is written.
 */
size_t rope_byte_count(const Rope *rope);

/* requires: rope(rope, bytes, share); *value is writable.
 * ensures:  rope(rope, bytes, share). When offset is within bytes, *value is
 *           the byte there and the result is 1; otherwise *value is unchanged
 *           and the result is 0.
 */
int rope_byte_at(const Rope *rope, size_t offset, unsigned char *value);

/* requires: rope(rope, bytes, share); holds a write share of `length` bytes
 *           at `destination`.
 * ensures:  rope(rope, bytes, share). When offset + length is within bytes,
 *           `destination` holds that slice of bytes and the result is 1;
 *           otherwise `destination` is unchanged and the result is 0.
 */
int rope_copy_range(const Rope *rope, size_t offset, size_t length,
                    unsigned char *destination);

/* requires: node_pool(pool, live, residual); rope(rope, bytes, share); *left
 *           and *right are writable and are not *rope.
 * ensures:  rope(rope, bytes, share) is returned; rope(left, prefix, share')
 *           and rope(right, suffix, share'') where bytes = prefix ++ suffix
 *           and prefix has length min(offset, |bytes|), and the result is 1;
 *           or allocation failed and the result is 0. The results hold shares
 *           of nodes `rope` also holds; nothing is copied that can be shared.
 */
int rope_split(Pool *pool, const Rope *rope, size_t offset,
               Rope *left, Rope *right);

/* requires: node_pool(pool, live, residual); rope(rope, bytes, share);
 *           start <= end <= |bytes|; *slice is writable and is not *rope.
 * ensures:  rope(rope, bytes, share) is returned; rope(slice, s, share2)
 *           where s is bytes[start, end), sharing what it can, and the
 *           result is 1; or the range was out of order or allocation failed
 *           and the result is 0.
 */
int rope_slice(Pool *pool, const Rope *rope, size_t start, size_t end,
               Rope *slice);

/* requires: node_pool(pool, live, residual); rope(left, a, share_a) and
 *           rope(right, b, share_b); *result is writable.
 * ensures:  both inputs are returned; rope(result, a ++ b, share') and the
 *           result is 1; or the concatenation would exceed UINT32_MAX bytes
 *           or ROPE_MAX_HEIGHT, or allocation failed, and the result is 0.
 */
int rope_concat(Pool *pool, const Rope *left, const Rope *right, Rope *result);

/* The one mutator, and the shape every command routes through: an address
 * decomposes a rope into (prefix, focus, suffix) and this puts it back with a
 * new focus, of any length.
 *
 * requires: node_pool(pool, live, residual); rope(rope, bytes, share);
 *           start <= end <= |bytes|; holds a read share of
 *           `replacement_length` bytes at `replacement`, which may be null
 *           when the length is zero; *result is writable and is not *rope.
 * ensures:  rope(rope, bytes, share) and the read share of `replacement` are
 *           returned; rope(result, bytes', share') where bytes' is bytes with
 *           the slice [start, end) replaced by the replacement bytes, and the
 *           result is 1; or the arguments were out of range or allocation
 *           failed, and the result is 0.
 */
int rope_replace_span(Pool *pool, const Rope *rope,
                      size_t start, size_t end,
                      const unsigned char *replacement,
                      size_t replacement_length,
                      Rope *result);

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is the total pool memory
 *           the version occupies, counting each node once. For tests. No
 *           memory is written.
 */
size_t rope_allocated_bytes(const Rope *rope);

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is 1 when every interior
 *           node holds its children within RRB_EXTRAS of the fewest that
 *           could hold them — the RRB balance invariant. Nothing else
 *           checks this, and without it a tree degrades silently under
 *           repeated concatenation. For tests; no memory is written.
 */
int rope_check_fill(const Rope *rope);

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is 1 when every structural
 *           invariant holds — child counts within bounds,
 *           size tables agreeing with subtree measures, every leaf at the
 *           same depth — and 0 otherwise. For tests; no memory is written.
 */
int rope_check_invariants(const Rope *rope);

#endif /* PAPRI_ROPE_H */
