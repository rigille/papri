#include "line_index.h"

#include <stdlib.h>
#include <string.h>

/* One node shape for both levels, the way the rope has one.
 *
 * At height 0 the node IS a run of blocks: `bytes[i]` and `newlines[i]` are
 * that block's own counts and `children` is unused. Above it they are
 * cumulative sums over the subtrees in `children`, so a descent is a scan of
 * one array — and a short one, because the branching is wide enough that a
 * gigabyte is three levels.
 */
struct LineIndexNode {
    uint32_t height;
    uint32_t count;
    void    *children[LINE_INDEX_BRANCHING];
    size_t   bytes[LINE_INDEX_BRANCHING];
    size_t   newlines[LINE_INDEX_BRANCHING];
};

/* The window the last-mile scan reads through. One block at a time, so a
 * query touches a few kilobytes of sequential memory rather than chasing a
 * pointer per line. */
#define SCAN_WINDOW 1024

/* requires: holds a read share of `length` bytes at `bytes`.
 * ensures:  the read share is returned; the result is how many are 0x0A.
 */
static size_t count_newlines(const unsigned char *bytes, size_t length)
{
    size_t        index;
    size_t        total;
    unsigned char value;

    total = 0;
    index = 0;
    while (index < length) {
        value = bytes[index];
        if (value == 0x0A) {
            total = total + 1;
        }
        index = index + 1;
    }
    return total;
}

/* requires: rope(rope, bytes, share); [from, from + length) is within bytes.
 * ensures:  rope(rope, bytes, share); the result is how many newlines that
 *           range holds.
 */
static size_t count_newlines_in_rope(const Rope *rope, size_t from,
                                     size_t length)
{
    unsigned char window[SCAN_WINDOW];
    size_t        position;
    size_t        remaining;
    size_t        span;
    size_t        total;
    size_t        found;
    int           ok;

    total = 0;
    position = from;
    remaining = length;
    while (remaining > 0) {
        span = remaining;
        if (span > SCAN_WINDOW) {
            span = SCAN_WINDOW;
        }
        ok = rope_copy_range(rope, position, span, window);
        if (ok == 0) {
            return total;
        }
        found = count_newlines(window, span);
        total = total + found;
        position = position + span;
        remaining = remaining - span;
    }
    return total;
}

/* requires: node_pool(pool, live, residual).
 * ensures:  node_pool(pool, live', residual) with one fresh zeroed node at
 *           `height`, and the result points at it; or the result is null.
 */
static LineIndexNode *allocate_node(Pool *pool, uint32_t height)
{
    void          *allocation;
    LineIndexNode *node;

    allocation = pool_allocate(pool, sizeof(LineIndexNode));
    if (allocation == NULL) {
        return NULL;
    }
    node = allocation;
    node->height = height;
    node->count = 0;
    return node;
}

/* requires: *index is allocated and writable.
 * ensures:  as line_index.h.
 */
void line_index_initialize(LineIndex *index)
{
    index->root = NULL;
    index->height = 0;
    index->byte_count = 0;
    index->newline_count = 0;
}

/* requires: line_index(index, bytes, share).
 * ensures:  as line_index.h.
 */
size_t line_index_newline_count(const LineIndex *index)
{
    size_t total;

    total = index->newline_count;
    return total;
}

/* Build one level above a run of nodes, and keep going until there is one.
 *
 * requires: node_pool(pool, live, residual); `nodes` holds `count` sealed
 *           nodes at `height` with the given measures; the arrays are
 *           writable scratch.
 * ensures:  node_pool(pool, live', residual); *index describes the same
 *           blocks, and the result is 1; or the result is 0.
 */
static int build_levels(Pool *pool, void **nodes, size_t *bytes,
                        size_t *newlines, size_t count, uint32_t height,
                        LineIndex *index)
{
    LineIndexNode *parent;
    size_t         read_index;
    size_t         write_index;
    size_t         level_count;
    size_t         running_bytes;
    size_t         running_newlines;
    size_t         own_bytes;
    size_t         own_newlines;
    size_t         remaining;
    uint32_t       group;
    uint32_t       slot;
    void          *child;

    level_count = count;
    while (level_count > 1) {
        height = height + 1;
        read_index = 0;
        write_index = 0;
        while (read_index < level_count) {
            remaining = level_count - read_index;
            if (remaining > LINE_INDEX_BRANCHING) {
                remaining = LINE_INDEX_BRANCHING;
            }
            group = (uint32_t)remaining;

            parent = allocate_node(pool, height);
            if (parent == NULL) {
                return 0;
            }
            parent->count = group;

            running_bytes = 0;
            running_newlines = 0;
            slot = 0;
            while (slot < group) {
                child = nodes[read_index + slot];
                own_bytes = bytes[read_index + slot];
                own_newlines = newlines[read_index + slot];
                running_bytes = running_bytes + own_bytes;
                running_newlines = running_newlines + own_newlines;
                parent->children[slot] = child;
                parent->bytes[slot] = running_bytes;
                parent->newlines[slot] = running_newlines;
                slot = slot + 1;
            }

            nodes[write_index] = parent;
            bytes[write_index] = running_bytes;
            newlines[write_index] = running_newlines;
            write_index = write_index + 1;
            read_index = read_index + group;
        }
        level_count = write_index;
    }

    child = nodes[0];
    running_bytes = bytes[0];
    running_newlines = newlines[0];
    index->root = child;
    index->height = height;
    index->byte_count = running_bytes;
    index->newline_count = running_newlines;
    return 1;
}

/* requires: as line_index.h.
 * ensures:  as line_index.h.
 */
int line_index_build(Pool *pool, const Rope *rope, LineIndex *index)
{
    void          **nodes;
    size_t         *bytes;
    size_t         *newlines;
    LineIndexNode  *leaf;
    size_t          total;
    size_t          block_total;
    size_t          position;
    size_t          span;
    size_t          found;
    size_t          leaf_count;
    size_t          running_bytes;
    size_t          running_newlines;
    uint32_t        slot;
    uint32_t        filled;
    int             ok;

    line_index_initialize(index);
    total = rope_byte_count(rope);
    if (total == 0) {
        return 1;
    }

    block_total = total / LINE_INDEX_BLOCK_BYTES;
    if (total % LINE_INDEX_BLOCK_BYTES > 0) {
        block_total = block_total + 1;
    }
    leaf_count = block_total / LINE_INDEX_BRANCHING;
    if (block_total % LINE_INDEX_BRANCHING > 0) {
        leaf_count = leaf_count + 1;
    }

    /* From the pool, not from malloc: `nodes` holds the only reference to
     * every index node built so far, and the collector does not scan
     * malloc'd memory. The two measure arrays hold no pointers. */
    nodes = pool_allocate(pool, leaf_count * sizeof(void *));
    bytes = pool_allocate_atomic(pool, leaf_count * sizeof(size_t));
    newlines = pool_allocate_atomic(pool, leaf_count * sizeof(size_t));
    if (nodes == NULL || bytes == NULL || newlines == NULL) {
        return 0;
    }

    position = 0;
    leaf_count = 0;
    while (position < total) {
        leaf = allocate_node(pool, 0);
        if (leaf == NULL) {
            return 0;
        }

        running_bytes = 0;
        running_newlines = 0;
        slot = 0;
        while (slot < LINE_INDEX_BRANCHING) {
            if (position >= total) {
                slot = LINE_INDEX_BRANCHING;
            } else {
                span = total - position;
                if (span > LINE_INDEX_BLOCK_BYTES) {
                    span = LINE_INDEX_BLOCK_BYTES;
                }
                found = count_newlines_in_rope(rope, position, span);
                running_bytes = running_bytes + span;
                running_newlines = running_newlines + found;
                /* Cumulative at leaves as well as above, so one descent
                 * works at every level. */
                filled = leaf->count;
                leaf->bytes[filled] = running_bytes;
                leaf->newlines[filled] = running_newlines;
                filled = filled + 1;
                leaf->count = filled;
                position = position + span;
                slot = slot + 1;
            }
        }

        nodes[leaf_count] = leaf;
        bytes[leaf_count] = running_bytes;
        newlines[leaf_count] = running_newlines;
        leaf_count = leaf_count + 1;
    }

    ok = build_levels(pool, nodes, bytes, newlines, leaf_count, 0, index);
    return ok;
}

/* Walk down to the block holding a position, by whichever column.
 *
 * requires: line_index(index, bytes, share) with a root; `by_newlines` says
 *           which cumulative column to compare `target` against; the
 *           out-parameters are writable.
 * ensures:  the share is returned; *base is the absolute byte offset where
 *           the found block starts, *block_bytes its length, *before_bytes
 *           and *before_newlines the totals preceding it.
 */
static void descend(const LineIndex *index, size_t target, int by_newlines,
                    size_t *base, size_t *block_bytes,
                    size_t *before_newlines)
{
    const LineIndexNode *node;
    const void          *child;
    uint32_t             height;
    uint32_t             slot;
    uint32_t             count;
    size_t               running;
    size_t               seen_bytes;
    size_t               seen_newlines;
    size_t               prior_bytes;
    size_t               prior_newlines;
    size_t               boundary;
    int                  descending;

    node = index->root;
    height = index->height;
    seen_bytes = 0;
    seen_newlines = 0;
    running = 0;

    descending = 1;
    while (descending == 1) {
        count = node->count;

        slot = 0;
        while (slot < count) {
            if (by_newlines == 1) {
                boundary = node->newlines[slot];
                running = seen_newlines;
            } else {
                boundary = node->bytes[slot];
                running = seen_bytes;
            }
            if (target < running + boundary) {
                break;
            }
            slot = slot + 1;
        }
        if (slot >= count) {
            slot = count - 1;
        }

        prior_bytes = 0;
        prior_newlines = 0;
        if (slot > 0) {
            prior_bytes = node->bytes[slot - 1];
            prior_newlines = node->newlines[slot - 1];
        }
        seen_bytes = seen_bytes + prior_bytes;
        seen_newlines = seen_newlines + prior_newlines;

        if (height == 0) {
            boundary = node->bytes[slot];
            *block_bytes = boundary - prior_bytes;
            descending = 0;
        } else {
            child = node->children[slot];
            node = child;
            height = height - 1;
        }
    }

    *base = seen_bytes;
    *before_newlines = seen_newlines;
}

/* requires: rope(rope, bytes, share); [from, from+length) within bytes; the
 *           range holds more than `which` newlines; *position writable.
 * ensures:  rope(rope, bytes, share); *position is the offset of the
 *           newline with that zero-based index, and the result is 1; or
 *           there were not that many and the result is 0.
 */
static int find_newline(const Rope *rope, size_t from, size_t length,
                        size_t which, size_t *position)
{
    unsigned char window[SCAN_WINDOW];
    size_t        cursor;
    size_t        remaining;
    size_t        span;
    size_t        index;
    size_t        seen;
    unsigned char value;
    int           ok;

    seen = 0;
    cursor = from;
    remaining = length;
    while (remaining > 0) {
        span = remaining;
        if (span > SCAN_WINDOW) {
            span = SCAN_WINDOW;
        }
        ok = rope_copy_range(rope, cursor, span, window);
        if (ok == 0) {
            return 0;
        }
        index = 0;
        while (index < span) {
            value = window[index];
            if (value == 0x0A) {
                if (seen == which) {
                    *position = cursor + index;
                    return 1;
                }
                seen = seen + 1;
            }
            index = index + 1;
        }
        cursor = cursor + span;
        remaining = remaining - span;
    }
    return 0;
}

/* requires: as line_index.h.
 * ensures:  as line_index.h.
 */
int line_index_line_of_offset(const LineIndex *index, const Rope *rope,
                              size_t offset, size_t *line)
{
    size_t total;
    size_t base;
    size_t before_newlines;
    size_t base_slot;
    size_t block_slot;
    size_t newlines_slot;
    size_t span;
    size_t found;
    void  *root;

    total = index->byte_count;
    if (offset > total) {
        return 0;
    }
    root = index->root;
    if (root == NULL) {
        *line = 0;
        return 1;
    }

    base_slot = 0;
    block_slot = 0;
    newlines_slot = 0;
    descend(index, offset, 0, &base_slot, &block_slot, &newlines_slot);
    base = base_slot;
    before_newlines = newlines_slot;

    span = offset - base;
    found = count_newlines_in_rope(rope, base, span);
    *line = before_newlines + found;
    return 1;
}

/* requires: as line_index.h.
 * ensures:  as line_index.h.
 */
int line_index_line_start(const LineIndex *index, const Rope *rope,
                          size_t line, size_t *offset)
{
    size_t newlines;
    size_t which;
    size_t base;
    size_t block_bytes;
    size_t before_newlines;
    size_t base_slot;
    size_t block_slot;
    size_t newlines_slot;
    size_t position_slot;
    size_t target;
    size_t position;
    void  *root;
    int    ok;

    if (line == 0) {
        *offset = 0;
        return 1;
    }

    newlines = index->newline_count;
    if (line > newlines) {
        return 0;
    }
    root = index->root;
    if (root == NULL) {
        return 0;
    }

    which = line - 1;
    base_slot = 0;
    block_slot = 0;
    newlines_slot = 0;
    descend(index, which, 1, &base_slot, &block_slot, &newlines_slot);
    base = base_slot;
    block_bytes = block_slot;
    before_newlines = newlines_slot;

    target = which - before_newlines;
    ok = find_newline(rope, base, block_bytes, target, &position_slot);
    if (ok == 0) {
        return 0;
    }
    position = position_slot;
    *offset = position + 1;
    return 1;
}

/* Copy the path down to the block holding `offset_in_node`, replacing that
 * block's two numbers and repairing every cumulative sum above it.
 *
 * requires: node_pool(pool, live, residual); line_index_node(node, blocks,
 *           share) at `height`; offset_in_node is within it; the
 *           out-parameters are writable.
 * ensures:  node_pool(pool, live', residual); the result is a fresh sealed
 *           node denoting `blocks` with that one block's counts replaced,
 *           sharing every subtree the edit did not touch; *out_bytes and
 *           *out_newlines are its new totals. Null when allocation failed.
 */
static LineIndexNode *rewrite_block(Pool *pool, const LineIndexNode *node,
                                    uint32_t height, size_t offset_in_node,
                                    size_t new_bytes, size_t new_newlines,
                                    size_t *out_bytes, size_t *out_newlines)
{
    LineIndexNode *copy;
    LineIndexNode *rebuilt;
    const void    *child;
    void          *child_pointer;
    uint32_t       count;
    uint32_t       slot;
    uint32_t       index;
    size_t         boundary;
    size_t         prior_bytes;
    size_t         prior_newlines;
    size_t         old_child_bytes;
    size_t         old_child_newlines;
    size_t         child_bytes;
    size_t         child_newlines;
    size_t         bytes_slot;
    size_t         newlines_slot;
    size_t         value;

    count = node->count;

    slot = 0;
    while (slot < count) {
        boundary = node->bytes[slot];
        if (offset_in_node < boundary) {
            break;
        }
        slot = slot + 1;
    }
    if (slot >= count) {
        slot = count - 1;
    }

    prior_bytes = 0;
    prior_newlines = 0;
    if (slot > 0) {
        prior_bytes = node->bytes[slot - 1];
        prior_newlines = node->newlines[slot - 1];
    }
    boundary = node->bytes[slot];
    old_child_bytes = boundary - prior_bytes;
    boundary = node->newlines[slot];
    old_child_newlines = boundary - prior_newlines;

    copy = allocate_node(pool, height);
    if (copy == NULL) {
        return NULL;
    }
    copy->count = count;

    if (height == 0) {
        child_bytes = new_bytes;
        child_newlines = new_newlines;
    } else {
        child = node->children[slot];
        value = offset_in_node - prior_bytes;
        rebuilt = rewrite_block(pool, child, height - 1, value, new_bytes,
                                new_newlines, &bytes_slot, &newlines_slot);
        if (rebuilt == NULL) {
            return NULL;
        }
        child_bytes = bytes_slot;
        child_newlines = newlines_slot;
        copy->children[slot] = rebuilt;
    }

    index = 0;
    while (index < count) {
        if (height > 0) {
            if (index != slot) {
                child_pointer = node->children[index];
                copy->children[index] = child_pointer;
            }
        }

        value = node->bytes[index];
        if (index >= slot) {
            value = value - old_child_bytes;
            value = value + child_bytes;
        }
        copy->bytes[index] = value;

        value = node->newlines[index];
        if (index >= slot) {
            value = value - old_child_newlines;
            value = value + child_newlines;
        }
        copy->newlines[index] = value;

        index = index + 1;
    }

    value = copy->bytes[count - 1];
    *out_bytes = value;
    value = copy->newlines[count - 1];
    *out_newlines = value;
    return copy;
}

/* requires: as line_index.h.
 * ensures:  as line_index.h.
 */
int line_index_update(Pool *pool, const LineIndex *before, const Rope *after,
                      size_t start, size_t end, size_t inserted,
                      LineIndex *result)
{
    LineIndexNode *root;
    void          *old_root;
    size_t         base;
    size_t         block_bytes;
    size_t         block_end;
    size_t         new_block_bytes;
    size_t         new_block_newlines;
    size_t         total_bytes;
    size_t         total_newlines;
    size_t         base_slot;
    size_t         block_slot;
    size_t         newlines_slot;
    size_t         bytes_slot;
    uint32_t       height;
    int            ok;

    old_root = before->root;
    if (old_root == NULL) {
        ok = line_index_build(pool, after, result);
        return ok;
    }

    base_slot = 0;
    block_slot = 0;
    newlines_slot = 0;
    descend(before, start, 0, &base_slot, &block_slot, &newlines_slot);
    base = base_slot;
    block_bytes = block_slot;
    block_end = base + block_bytes;

    /* The cheap case: the edit fell inside one block and left it within
     * bounds, so only that block's two numbers change and everything after
     * it is untouched — the counts are relative, which is the whole reason
     * blocks hold counts rather than offsets. */
    new_block_bytes = block_bytes;
    if (end > block_end) {
        ok = line_index_build(pool, after, result);
        return ok;
    }
    new_block_bytes = new_block_bytes - (end - start);
    new_block_bytes = new_block_bytes + inserted;
    if (new_block_bytes > LINE_INDEX_BLOCK_MAX) {
        ok = line_index_build(pool, after, result);
        return ok;
    }

    new_block_newlines = count_newlines_in_rope(after, base, new_block_bytes);

    height = before->height;
    root = rewrite_block(pool, old_root, height, start, new_block_bytes,
                         new_block_newlines, &bytes_slot, &newlines_slot);
    if (root == NULL) {
        return 0;
    }
    total_bytes = bytes_slot;
    total_newlines = newlines_slot;

    result->root = root;
    result->height = height;
    result->byte_count = total_bytes;
    result->newline_count = total_newlines;
    return 1;
}

/* requires: line_index_node(node, blocks, share) at `height`; the
 *           out-parameters are writable.
 * ensures:  the share is returned; the result is 1 when this node and every
 *           node beneath it has cumulative sums agreeing with its children,
 *           and its totals are reported.
 */
static int check_node(const void *node, uint32_t height, size_t *out_bytes,
                      size_t *out_newlines)
{
    const LineIndexNode *source;
    const void          *child;
    uint32_t             count;
    uint32_t             index;
    size_t               running_bytes;
    size_t               running_newlines;
    size_t               own_bytes;
    size_t               own_newlines;
    size_t               recorded;
    size_t               prior_bytes;
    size_t               prior_newlines;
    size_t               bytes_slot;
    size_t               newlines_slot;
    int                  ok;

    source = node;
    count = source->count;
    if (count == 0) {
        return 0;
    }
    if (count > LINE_INDEX_BRANCHING) {
        return 0;
    }

    running_bytes = 0;
    running_newlines = 0;
    index = 0;
    while (index < count) {
        prior_bytes = 0;
        prior_newlines = 0;
        if (index > 0) {
            prior_bytes = source->bytes[index - 1];
            prior_newlines = source->newlines[index - 1];
        }
        recorded = source->bytes[index];
        own_bytes = recorded - prior_bytes;
        recorded = source->newlines[index];
        own_newlines = recorded - prior_newlines;

        if (height == 0) {
            if (own_bytes == 0) {
                return 0;
            }
            if (own_bytes > LINE_INDEX_BLOCK_MAX) {
                return 0;
            }
        } else {
            child = source->children[index];
            ok = check_node(child, height - 1, &bytes_slot, &newlines_slot);
            if (ok == 0) {
                return 0;
            }
            own_bytes = bytes_slot;
            own_newlines = newlines_slot;
            recorded = source->bytes[index];
            recorded = recorded - prior_bytes;
            if (recorded != own_bytes) {
                return 0;
            }
            recorded = source->newlines[index];
            recorded = recorded - prior_newlines;
            if (recorded != own_newlines) {
                return 0;
            }
        }

        running_bytes = running_bytes + own_bytes;
        running_newlines = running_newlines + own_newlines;
        index = index + 1;
    }

    *out_bytes = running_bytes;
    *out_newlines = running_newlines;
    return 1;
}

/* requires: as line_index.h.
 * ensures:  as line_index.h.
 */
int line_index_check(const LineIndex *index)
{
    const void *root;
    uint32_t    height;
    size_t      measured_bytes;
    size_t      measured_newlines;
    size_t      counted_bytes;
    size_t      counted_newlines;
    size_t      total;
    int         ok;

    root = index->root;
    total = index->byte_count;
    if (root == NULL) {
        if (total == 0) {
            return 1;
        }
        return 0;
    }

    height = index->height;
    measured_bytes = 0;
    measured_newlines = 0;
    ok = check_node(root, height, &measured_bytes, &measured_newlines);
    if (ok == 0) {
        return 0;
    }
    counted_bytes = measured_bytes;
    counted_newlines = measured_newlines;
    if (counted_bytes != total) {
        return 0;
    }
    total = index->newline_count;
    if (counted_newlines != total) {
        return 0;
    }
    return 1;
}
