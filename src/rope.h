#ifndef PAPRI_ROPE_H
#define PAPRI_ROPE_H

#include <stdint.h>

#include "pool.h"

/* An immutable byte sequence: a relaxed radix balanced tree over cache-line
 * chunks.
 *
 * Leaves are exactly ROPE_LEAF_BYTES of payload, aligned to that same
 * boundary, so a leaf occupies one cache line and never straddles two. They
 * are headerless — no length, no measures, no tag — which is what makes
 * "exactly one line" exact. A child's byte and newline counts live in its
 * parent's size table, which a relaxed tree needs regardless. Size tables are
 * kept even on regular nodes so a leaf never has to know its own length.
 *
 * Nodes are sealed on construction and never written again. That immutability
 * is what lets two versions hold read shares of one node, which is what makes
 * the whole design work; see CLAUDE.md § Shares.
 */

#define ROPE_LEAF_BYTES 64
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
    size_t   newline_count;
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

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is the number of newlines in
 *           bytes. No memory is written.
 */
size_t rope_newline_count(const Rope *rope);

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

/* requires: rope(rope, bytes, share); *offset is writable.
 * ensures:  rope(rope, bytes, share). Lines are numbered from 0 and a line
 *           begins just after the preceding newline. When line_index names a
 *           line that begins within bytes, *offset is where it begins and the
 *           result is 1; otherwise *offset is unchanged and the result is 0.
 */
int rope_line_start(const Rope *rope, size_t line_index, size_t *offset);

/* requires: rope(rope, bytes, share); *line_index is writable.
 * ensures:  rope(rope, bytes, share). When offset is at most |bytes|,
 *           *line_index is the number of newlines strictly before it — the
 *           line the offset falls on — and the result is 1; otherwise
 *           *line_index is unchanged and the result is 0.
 */
int rope_line_of_offset(const Rope *rope, size_t offset,
                        size_t *line_index);

/* Free exactly the nodes the retiring version holds and the survivor does
 * not — the runtime half of the reclamation story.
 *
 * Shares say WHEN freeing is permitted: a node may go back only once every
 * share of it has rejoined to the full share. They are ghost state, so they
 * cannot say WHICH nodes those are. This walk is what finds them, and the
 * obligation connecting the two is that it returns exactly the nodes whose
 * shares have rejoined.
 *
 * It descends both versions by height, pruning wherever a node is reached by
 * both — pointer identity, so a shared subtree costs one comparison however
 * large it is. Positions are not compared, because drop and concat shift a
 * shared child to a different index; only identity at a common height is
 * trusted.
 *
 * Soundness rests on each version being a TREE — no node twice within one
 * version — so that every node is reached, and freed, at most once.
 *
 * Both sides are SETS of roots, not single versions. Retirement passes one
 * of each; an edit passes its discarded intermediates as dead and the
 * version it started from plus the one it produced as live, which is how
 * the garbage a single edit leaves behind gets collected at all — that
 * garbage is reachable from no version, so version-to-version diffing alone
 * would never see it.
 *
 * requires: node_pool(pool, live, residual); every rope in `dead` is one
 *           nothing outside this call still holds a share of; every rope in
 *           `live` must survive.
 * ensures:  node_pool(pool, live', residual) with every node reachable from
 *           some rope in `dead` and from none in `live` returned to the
 *           pool, each freed exactly once, and the result is 1; or the
 *           difference was too wide to walk within bounded memory and the
 *           result is 0. Every rope in `live` is untouched and readable.
 */
int rope_free_difference(Pool *pool,
                         const Rope *dead, uint32_t dead_count,
                         const Rope *live, uint32_t live_count);

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is the total pool memory
 *           the version occupies, counting each node once. For tests. No
 *           memory is written.
 */
size_t rope_allocated_bytes(const Rope *rope);

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is 1 when every structural
 *           invariant holds — leaves aligned, child counts within bounds,
 *           size tables agreeing with subtree measures, every leaf at the
 *           same depth — and 0 otherwise. For tests; no memory is written.
 */
int rope_check_invariants(const Rope *rope);

#endif /* PAPRI_ROPE_H */
