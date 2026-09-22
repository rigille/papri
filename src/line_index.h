#ifndef PAPRI_LINE_INDEX_H
#define PAPRI_LINE_INDEX_H

#include <stddef.h>
#include <stdint.h>

#include "pool.h"
#include "rope.h"

/* Line addressing, kept outside the rope.
 *
 * The rope is a plain RRB over bytes and has never heard of a newline. What
 * line addressing needs is rank and select over the newline positions, and
 * the succinct-structures answer to that has been the same since Jacobson:
 * do not store one entry per set bit, store COUNTS PER BLOCK and resolve the
 * last mile by scanning.
 *
 * So this is a two-level structure. A block covers a few kilobytes of the
 * buffer and records two numbers: how many bytes it holds and how many
 * newlines. The upper level is a shallow persistent B+-tree over those
 * blocks, carrying cumulative sums so either query is one descent. The last
 * mile is a scan of one block through the rope.
 *
 * That granularity is the whole point. One entry per line would be one per
 * forty bytes; one per block is one per few thousand, and the scan that
 * replaces the missing precision costs a few hundred nanoseconds on hardware
 * that reads sequential memory at ten gigabytes a second while a pointer
 * chase costs a stall.
 *
 * Blocks store COUNTS, NOT OFFSETS. That is what makes an edit local: it
 * changes the two numbers of the blocks it touched and nothing else, because
 * every later block's numbers are relative. An array of absolute newline
 * offsets would have to shift everything after the edit, which is why the
 * obvious structure is the wrong one.
 *
 * The tree is persistent, path-copied like the rope, so every version keeps
 * its own index and a background job holding an old snapshot still has
 * O(log n) line addressing. Nothing is freed yet; the collector will take
 * these nodes along with the rope's.
 */

#define LINE_INDEX_BRANCHING   64
#define LINE_INDEX_BLOCK_BYTES 4096
#define LINE_INDEX_BLOCK_MAX   8192

typedef struct LineIndexNode LineIndexNode;

typedef struct LineIndex {
    void   *root;          /* LineIndexNode*, or null when empty */
    uint32_t height;       /* 0 when the root is a leaf of blocks */
    size_t  byte_count;
    size_t  newline_count;
} LineIndex;

/* ── Abstract predicates ────────────────────────────────────────────────────
 * line_index_node(node, blocks, share)
 *   Holds `share` of *node, sealed and immutable. `blocks` is the sequence
 *   of (byte count, newline count) pairs beneath it, in buffer order. Read
 *   shares split exactly as the rope's do.
 *
 * line_index(index, bytes, share)
 *   *index is a pure value. Holds `share` of every node reachable from its
 *   root. `bytes` is the byte sequence it describes: the blocks partition
 *   it in order, their byte counts sum to |bytes|, and their newline counts
 *   sum to the number of newlines in it.
 *
 * An index is only meaningful against the rope it was built from. Every
 * query below takes that rope, because the last mile is a scan of it.
 */

/* requires: *index is allocated and writable.
 * ensures:  line_index(index, bytes, share) where bytes is empty.
 */
void line_index_initialize(LineIndex *index);

/* requires: node_pool(pool, live, residual); rope(rope, bytes, share);
 *           *index writable.
 * ensures:  the rope's share is returned; line_index(index, bytes, share')
 *           describing it, and the result is 1; or allocation failed and
 *           the result is 0. Costs one pass over the rope, so this is for
 *           loading a buffer, not for maintaining one.
 */
int line_index_build(Pool *pool, const Rope *rope, LineIndex *index);

/* requires: line_index(index, bytes, share).
 * ensures:  line_index(index, bytes, share); the result is the number of
 *           newlines in bytes. No memory is written.
 */
size_t line_index_newline_count(const LineIndex *index);

/* requires: line_index(index, bytes, share); rope(rope, bytes, share2) — the
 *           same bytes the index describes; *offset writable.
 * ensures:  both shares are returned. Lines are numbered from 0 and a line
 *           begins just after the preceding newline. When `line` names a
 *           line that begins within bytes, *offset is where it begins and
 *           the result is 1; otherwise *offset is unchanged and the result
 *           is 0.
 */
int line_index_line_start(const LineIndex *index, const Rope *rope,
                          size_t line, size_t *offset);

/* requires: line_index(index, bytes, share); rope(rope, bytes, share2);
 *           *line writable.
 * ensures:  both shares are returned. When offset is at most |bytes|, *line
 *           is the number of newlines strictly before it and the result is
 *           1; otherwise *line is unchanged and the result is 0.
 */
int line_index_line_of_offset(const LineIndex *index, const Rope *rope,
                              size_t offset, size_t *line);

/* Derive the index of an edited buffer from the index of the one it came
 * from.
 *
 * requires: node_pool(pool, live, residual); line_index(before, old, share);
 *           rope(after, new, share2) where new is old with [start, end)
 *           replaced by `inserted` bytes; *result writable.
 * ensures:  every input share is returned; line_index(result, new, share')
 *           and the result is 1; or allocation failed and the result is 0.
 *           When the edit falls inside one block and leaves it within
 *           LINE_INDEX_BLOCK_MAX the cost is one path copy and one block
 *           rescan; otherwise the index is rebuilt from the rope.
 */
int line_index_update(Pool *pool, const LineIndex *before, const Rope *after,
                      size_t start, size_t end, size_t inserted,
                      LineIndex *result);

/* requires: line_index(index, bytes, share).
 * ensures:  line_index(index, bytes, share); the result is 1 when the
 *           blocks' counts sum to the recorded totals, every block is within
 *           its size bounds, and every node's cumulative sums agree with its
 *           children. For tests; no memory is written.
 */
int line_index_check(const LineIndex *index);

#endif /* PAPRI_LINE_INDEX_H */
