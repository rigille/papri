#include "rope.h"

#include <stdlib.h>
#include <string.h>

/* Leaves are headerless: a leaf is ROPE_LEAF_BYTES of payload and nothing
 * else. Everything known about a child — its byte count, its newline count —
 * lives in its parent's size table below. The root's measures live in the
 * Rope value itself, which is how a height-0 rope knows its own length.
 *
 * Size tables are cumulative so a descent is a scan of one array. They are
 * kept on every node, regular or relaxed, so that a leaf never has to carry
 * its own length.
 */
struct RopeNode {
    uint32_t height;
    uint32_t child_count;
    void    *children[ROPE_BRANCHING];
    uint32_t cumulative_bytes[ROPE_BRANCHING];
    uint32_t cumulative_newlines[ROPE_BRANCHING];
};

/* A merge can hold both parents' children plus the one or two nodes their
 * adjoining edges produced: (32-1) + 2 + (32-1). */
#define CHILD_LIST_CAPACITY (ROPE_BRANCHING * 2 + 2)

typedef struct ChildList {
    void    *nodes[CHILD_LIST_CAPACITY];
    uint32_t bytes[CHILD_LIST_CAPACITY];
    uint32_t newlines[CHILD_LIST_CAPACITY];
    uint32_t count;
} ChildList;

/* One or two nodes at a common height — what a concatenation step yields
 * before its parent decides whether they need a new level above them. */
typedef struct NodePair {
    void    *nodes[2];
    uint32_t bytes[2];
    uint32_t newlines[2];
    uint32_t count;
    uint32_t height;
} NodePair;

/* Recursive helpers return 1 when they produced a node, 0 when the result is
 * empty, and -1 when allocation failed. Distinguishing the last two matters:
 * an empty result is ordinary, a failure must propagate. */
#define PRODUCED 1
#define EMPTY    0
#define FAILED   (-1)

/* requires: holds a read share of `length` bytes at `bytes`.
 * ensures:  the read share is returned; the result is the number of 0x0A
 *           bytes among them.
 */
static uint32_t count_newlines(const unsigned char *bytes, uint32_t length)
{
    uint32_t index;
    uint32_t total;
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

/* requires: rope_node(node, contents, share); index < node->child_count.
 * ensures:  rope_node(node, contents, share); the result is that child's own
 *           byte count. No memory is written.
 */
static uint32_t node_child_bytes(const RopeNode *node, uint32_t index)
{
    uint32_t current;
    uint32_t previous;

    current = node->cumulative_bytes[index];
    if (index == 0) {
        return current;
    }
    previous = node->cumulative_bytes[index - 1];
    return current - previous;
}

/* requires: rope_node(node, contents, share); index < node->child_count.
 * ensures:  rope_node(node, contents, share); the result is that child's own
 *           newline count. No memory is written.
 */
static uint32_t node_child_newlines(const RopeNode *node, uint32_t index)
{
    uint32_t current;
    uint32_t previous;

    current = node->cumulative_newlines[index];
    if (index == 0) {
        return current;
    }
    previous = node->cumulative_newlines[index - 1];
    return current - previous;
}

/* requires: node_pool(pool, live, residual).
 * ensures:  node_pool(pool, live', residual) with one fresh leaf, and the
 *           result points at it; or the result is null.
 */
static unsigned char *allocate_leaf(Pool *pool)
{
    void *allocation;

    allocation = pool_allocate(pool, ROPE_LEAF_BYTES);
    return allocation;
}

/* requires: node_pool(pool, live, residual); holds read shares of the `count`
 *           children described by the three parallel arrays;
 *           count <= ROPE_BRANCHING; *out_bytes and *out_newlines writable.
 * ensures:  node_pool(pool, live', residual) with one fresh sealed node whose
 *           children are those, *out_bytes and *out_newlines are its totals,
 *           and the result points at it; or the result is null and the
 *           outputs are unspecified.
 */
static RopeNode *make_node(Pool *pool, uint32_t height,
                           void *const *children,
                           const uint32_t *child_bytes,
                           const uint32_t *child_newlines,
                           uint32_t count,
                           uint32_t *out_bytes, uint32_t *out_newlines)
{
    void     *allocation;
    RopeNode *node;
    uint32_t  index;
    uint32_t  running_bytes;
    uint32_t  running_newlines;
    uint32_t  own_bytes;
    uint32_t  own_newlines;
    void     *child;

    allocation = pool_allocate(pool, sizeof(RopeNode));
    if (allocation == NULL) {
        return NULL;
    }
    node = allocation;

    node->height = height;
    node->child_count = count;

    running_bytes = 0;
    running_newlines = 0;
    index = 0;
    while (index < count) {
        child = children[index];
        own_bytes = child_bytes[index];
        own_newlines = child_newlines[index];

        running_bytes = running_bytes + own_bytes;
        running_newlines = running_newlines + own_newlines;

        node->children[index] = child;
        node->cumulative_bytes[index] = running_bytes;
        node->cumulative_newlines[index] = running_newlines;
        index = index + 1;
    }

    *out_bytes = running_bytes;
    *out_newlines = running_newlines;
    return node;
}

/* requires: node_pool(pool, live, residual); holds read shares of list's
 *           children; 0 < list->count <= CHILD_LIST_CAPACITY; *result
 *           writable.
 * ensures:  node_pool(pool, live', residual); *result holds one node when the
 *           children fit in a single node and two roughly equal ones when
 *           they do not, all at `height`, and the outcome is PRODUCED; or
 *           FAILED.
 */
static int pack_children(Pool *pool, uint32_t height, const ChildList *list,
                         NodePair *result)
{
    uint32_t         count;
    uint32_t         left_count;
    uint32_t         right_count;
    RopeNode        *node;
    void *const     *child_slice;
    const uint32_t  *bytes_slice;
    const uint32_t  *newlines_slice;

    count = list->count;

    if (count <= ROPE_BRANCHING) {
        node = make_node(pool, height, list->nodes, list->bytes,
                         list->newlines, count,
                         &result->bytes[0], &result->newlines[0]);
        if (node == NULL) {
            return FAILED;
        }
        result->nodes[0] = node;
        result->count = 1;
        result->height = height;
        return PRODUCED;
    }

    left_count = count / 2;
    right_count = count - left_count;

    node = make_node(pool, height, list->nodes, list->bytes, list->newlines,
                     left_count, &result->bytes[0], &result->newlines[0]);
    if (node == NULL) {
        return FAILED;
    }
    result->nodes[0] = node;

    child_slice = list->nodes + left_count;
    bytes_slice = list->bytes + left_count;
    newlines_slice = list->newlines + left_count;

    node = make_node(pool, height, child_slice, bytes_slice, newlines_slice,
                     right_count, &result->bytes[1], &result->newlines[1]);
    if (node == NULL) {
        return FAILED;
    }
    result->nodes[1] = node;
    result->count = 2;
    result->height = height;
    return PRODUCED;
}

/* requires: *list is writable; list->count < CHILD_LIST_CAPACITY; holds a
 *           read share of `node`.
 * ensures:  *list has that child appended.
 */
static void child_list_append(ChildList *list, void *node, uint32_t bytes,
                              uint32_t newlines)
{
    uint32_t count;

    count = list->count;
    list->nodes[count] = node;
    list->bytes[count] = bytes;
    list->newlines[count] = newlines;
    list->count = count + 1;
}

/* requires: rope(rope, bytes, share) where the root is a chain of
 *           single-child nodes above the real content.
 * ensures:  rope(rope, bytes, share) with those levels removed; the denoted
 *           bytes are unchanged.
 */
static void collapse_root(Rope *rope)
{
    void     *root;
    RopeNode *node;
    uint32_t  height;
    uint32_t  child_count;

    height = rope->height;
    while (height > 0) {
        root = rope->root;
        node = root;
        child_count = node->child_count;
        if (child_count != 1) {
            return;
        }
        root = node->children[0];
        height = height - 1;
        rope->root = root;
        rope->height = height;
    }
}

/* Take the first `count` bytes of a subtree.
 *
 * The result is at exactly `height`, even when that leaves single-child
 * nodes, because every child of a node must sit at the same depth. Only the
 * finished rope's root is collapsed.
 *
 * requires: node_pool(pool, live, residual); holds a read share of the
 *           subtree at `node`, whose measures are node_bytes and
 *           node_newlines; the out-parameters are writable.
 * ensures:  node_pool(pool, live', residual); on PRODUCED the outputs hold a
 *           subtree at `height` denoting the first `count` bytes, sharing
 *           whatever nodes it can with the input; on EMPTY count was zero; on
 *           FAILED allocation failed.
 */
static int take_node(Pool *pool, void *node, uint32_t height,
                     uint32_t node_bytes, uint32_t node_newlines,
                     uint32_t count,
                     void **out_node, uint32_t *out_bytes,
                     uint32_t *out_newlines)
{
    const RopeNode *source;
    ChildList       list;
    unsigned char  *leaf;
    const unsigned char *source_bytes;
    uint32_t        index;
    uint32_t        child_total;
    uint32_t        running;
    uint32_t        next_running;
    uint32_t        own_bytes;
    uint32_t        own_newlines;
    uint32_t        remainder;
    void           *child;
    void           *sub_node;
    uint32_t        sub_bytes;
    uint32_t        sub_newlines;
    void           *taken_node;
    uint32_t        taken_bytes;
    uint32_t        taken_newlines;
    RopeNode       *built;
    int             outcome;

    if (count == 0) {
        return EMPTY;
    }
    if (count >= node_bytes) {
        *out_node = node;
        *out_bytes = node_bytes;
        *out_newlines = node_newlines;
        return PRODUCED;
    }

    if (height == 0) {
        leaf = allocate_leaf(pool);
        if (leaf == NULL) {
            return FAILED;
        }
        source_bytes = node;
        memcpy(leaf, source_bytes, count);
        *out_node = leaf;
        *out_bytes = count;
        *out_newlines = count_newlines(leaf, count);
        return PRODUCED;
    }

    source = node;
    child_total = source->child_count;
    list.count = 0;
    running = 0;
    index = 0;
    while (index < child_total) {
        own_bytes = node_child_bytes(source, index);
        own_newlines = node_child_newlines(source, index);
        next_running = running + own_bytes;
        child = source->children[index];

        if (next_running <= count) {
            child_list_append(&list, child, own_bytes, own_newlines);
            running = next_running;
            if (running == count) {
                index = child_total;
            } else {
                index = index + 1;
            }
        } else {
            remainder = count - running;
            outcome = take_node(pool, child, height - 1, own_bytes,
                                own_newlines, remainder,
                                &sub_node, &sub_bytes, &sub_newlines);
            if (outcome == FAILED) {
                return FAILED;
            }
            if (outcome == PRODUCED) {
                taken_node = sub_node;
                taken_bytes = sub_bytes;
                taken_newlines = sub_newlines;
                child_list_append(&list, taken_node, taken_bytes,
                                  taken_newlines);
            }
            index = child_total;
        }
    }

    child_total = list.count;
    if (child_total == 0) {
        return EMPTY;
    }

    built = make_node(pool, height, list.nodes, list.bytes, list.newlines,
                      child_total, out_bytes, out_newlines);
    if (built == NULL) {
        return FAILED;
    }
    *out_node = built;
    return PRODUCED;
}

/* Drop the first `count` bytes of a subtree. Mirrors take_node.
 *
 * requires: node_pool(pool, live, residual); holds a read share of the
 *           subtree at `node`, whose measures are node_bytes and
 *           node_newlines; the out-parameters are writable.
 * ensures:  node_pool(pool, live', residual); on PRODUCED the outputs hold a
 *           subtree at `height` denoting the bytes from `count` onward,
 *           sharing whatever it can; on EMPTY nothing remained; on FAILED
 *           allocation failed.
 */
static int drop_node(Pool *pool, void *node, uint32_t height,
                     uint32_t node_bytes, uint32_t node_newlines,
                     uint32_t count,
                     void **out_node, uint32_t *out_bytes,
                     uint32_t *out_newlines)
{
    const RopeNode *source;
    ChildList       list;
    unsigned char  *leaf;
    const unsigned char *source_bytes;
    uint32_t        index;
    uint32_t        child_total;
    uint32_t        running;
    uint32_t        next_running;
    uint32_t        own_bytes;
    uint32_t        own_newlines;
    uint32_t        remainder;
    uint32_t        kept;
    void           *child;
    void           *sub_node;
    uint32_t        sub_bytes;
    uint32_t        sub_newlines;
    void           *taken_node;
    uint32_t        taken_bytes;
    uint32_t        taken_newlines;
    RopeNode       *built;
    int             outcome;

    if (count == 0) {
        *out_node = node;
        *out_bytes = node_bytes;
        *out_newlines = node_newlines;
        return PRODUCED;
    }
    if (count >= node_bytes) {
        return EMPTY;
    }

    if (height == 0) {
        leaf = allocate_leaf(pool);
        if (leaf == NULL) {
            return FAILED;
        }
        source_bytes = node;
        kept = node_bytes - count;
        memcpy(leaf, source_bytes + count, kept);
        *out_node = leaf;
        *out_bytes = kept;
        *out_newlines = count_newlines(leaf, kept);
        return PRODUCED;
    }

    source = node;
    child_total = source->child_count;
    list.count = 0;
    running = 0;
    index = 0;
    while (index < child_total) {
        own_bytes = node_child_bytes(source, index);
        own_newlines = node_child_newlines(source, index);
        next_running = running + own_bytes;
        child = source->children[index];

        if (next_running <= count) {
            /* Entirely before the cut: drop it. */
            running = next_running;
        } else {
            if (running >= count) {
                child_list_append(&list, child, own_bytes, own_newlines);
            } else {
                remainder = count - running;
                outcome = drop_node(pool, child, height - 1, own_bytes,
                                    own_newlines, remainder,
                                    &sub_node, &sub_bytes, &sub_newlines);
                if (outcome == FAILED) {
                    return FAILED;
                }
                if (outcome == PRODUCED) {
                    taken_node = sub_node;
                    taken_bytes = sub_bytes;
                    taken_newlines = sub_newlines;
                    child_list_append(&list, taken_node, taken_bytes,
                                      taken_newlines);
                }
                running = next_running;
            }
        }
        index = index + 1;
    }

    child_total = list.count;
    if (child_total == 0) {
        return EMPTY;
    }

    built = make_node(pool, height, list.nodes, list.bytes, list.newlines,
                      child_total, out_bytes, out_newlines);
    if (built == NULL) {
        return FAILED;
    }
    *out_node = built;
    return PRODUCED;
}

/* Concatenate two non-empty subtrees.
 *
 * Invariant that makes the recursion work: the result is one or two nodes at
 * exactly max(left_height, right_height). The caller adds a level only when
 * two came back.
 *
 * requires: node_pool(pool, live, residual); holds read shares of both
 *           subtrees, both non-empty; *result writable.
 * ensures:  node_pool(pool, live', residual); on PRODUCED *result holds one
 *           or two sealed nodes at max(left_height, right_height) together
 *           denoting the concatenation; on FAILED allocation failed.
 */
static int concat_rec(Pool *pool,
                      void *left, uint32_t left_height,
                      uint32_t left_bytes, uint32_t left_newlines,
                      void *right, uint32_t right_height,
                      uint32_t right_bytes, uint32_t right_newlines,
                      NodePair *result)
{
    const RopeNode *left_node;
    const RopeNode *right_node;
    ChildList       list;
    NodePair        middle;
    uint32_t        middle_count;
    unsigned char  *leaf;
    const unsigned char *left_source;
    const unsigned char *right_source;
    uint32_t        total;
    uint32_t        left_count;
    uint32_t        right_count;
    uint32_t        index;
    uint32_t        edge_bytes;
    uint32_t        edge_newlines;
    uint32_t        other_bytes;
    uint32_t        other_newlines;
    void           *edge_child;
    void           *other_child;
    int             outcome;

    if (left_height == 0) {
        if (right_height == 0) {
            total = left_bytes + right_bytes;
            if (total <= ROPE_LEAF_BYTES) {
                leaf = allocate_leaf(pool);
                if (leaf == NULL) {
                    return FAILED;
                }
                left_source = left;
                right_source = right;
                memcpy(leaf, left_source, left_bytes);
                memcpy(leaf + left_bytes, right_source, right_bytes);
                result->nodes[0] = leaf;
                result->bytes[0] = total;
                result->newlines[0] = left_newlines + right_newlines;
                result->count = 1;
                result->height = 0;
                return PRODUCED;
            }
            result->nodes[0] = left;
            result->bytes[0] = left_bytes;
            result->newlines[0] = left_newlines;
            result->nodes[1] = right;
            result->bytes[1] = right_bytes;
            result->newlines[1] = right_newlines;
            result->count = 2;
            result->height = 0;
            return PRODUCED;
        }
    }

    list.count = 0;

    if (left_height > right_height) {
        left_node = left;
        left_count = left_node->child_count;
        edge_child = left_node->children[left_count - 1];
        edge_bytes = node_child_bytes(left_node, left_count - 1);
        edge_newlines = node_child_newlines(left_node, left_count - 1);

        outcome = concat_rec(pool, edge_child, left_height - 1, edge_bytes,
                             edge_newlines, right, right_height, right_bytes,
                             right_newlines, &middle);
        if (outcome == FAILED) {
            return FAILED;
        }

        index = 0;
        while (index + 1 < left_count) {
            other_child = left_node->children[index];
            other_bytes = node_child_bytes(left_node, index);
            other_newlines = node_child_newlines(left_node, index);
            child_list_append(&list, other_child, other_bytes, other_newlines);
            index = index + 1;
        }
        index = 0;
        middle_count = middle.count;
        while (index < middle_count) {
            other_child = middle.nodes[index];
            other_bytes = middle.bytes[index];
            other_newlines = middle.newlines[index];
            child_list_append(&list, other_child, other_bytes, other_newlines);
            index = index + 1;
        }
        outcome = pack_children(pool, left_height, &list, result);
        return outcome;
    }

    if (left_height < right_height) {
        right_node = right;
        right_count = right_node->child_count;
        edge_child = right_node->children[0];
        edge_bytes = node_child_bytes(right_node, 0);
        edge_newlines = node_child_newlines(right_node, 0);

        outcome = concat_rec(pool, left, left_height, left_bytes,
                             left_newlines, edge_child, right_height - 1,
                             edge_bytes, edge_newlines, &middle);
        if (outcome == FAILED) {
            return FAILED;
        }

        index = 0;
        middle_count = middle.count;
        while (index < middle_count) {
            other_child = middle.nodes[index];
            other_bytes = middle.bytes[index];
            other_newlines = middle.newlines[index];
            child_list_append(&list, other_child, other_bytes, other_newlines);
            index = index + 1;
        }
        index = 1;
        while (index < right_count) {
            other_child = right_node->children[index];
            other_bytes = node_child_bytes(right_node, index);
            other_newlines = node_child_newlines(right_node, index);
            child_list_append(&list, other_child, other_bytes, other_newlines);
            index = index + 1;
        }
        outcome = pack_children(pool, right_height, &list, result);
        return outcome;
    }

    /* Equal heights, both above the leaves: merge the adjoining edges. */
    left_node = left;
    right_node = right;
    left_count = left_node->child_count;
    right_count = right_node->child_count;

    edge_child = left_node->children[left_count - 1];
    edge_bytes = node_child_bytes(left_node, left_count - 1);
    edge_newlines = node_child_newlines(left_node, left_count - 1);
    other_child = right_node->children[0];
    other_bytes = node_child_bytes(right_node, 0);
    other_newlines = node_child_newlines(right_node, 0);

    outcome = concat_rec(pool, edge_child, left_height - 1, edge_bytes,
                         edge_newlines, other_child, right_height - 1,
                         other_bytes, other_newlines, &middle);
    if (outcome == FAILED) {
        return FAILED;
    }

    index = 0;
    while (index + 1 < left_count) {
        other_child = left_node->children[index];
        other_bytes = node_child_bytes(left_node, index);
        other_newlines = node_child_newlines(left_node, index);
        child_list_append(&list, other_child, other_bytes, other_newlines);
        index = index + 1;
    }
    index = 0;
    middle_count = middle.count;
    while (index < middle_count) {
        other_child = middle.nodes[index];
        other_bytes = middle.bytes[index];
        other_newlines = middle.newlines[index];
        child_list_append(&list, other_child, other_bytes, other_newlines);
        index = index + 1;
    }
    index = 1;
    while (index < right_count) {
        other_child = right_node->children[index];
        other_bytes = node_child_bytes(right_node, index);
        other_newlines = node_child_newlines(right_node, index);
        child_list_append(&list, other_child, other_bytes, other_newlines);
        index = index + 1;
    }

    outcome = pack_children(pool, left_height, &list, result);
    return outcome;
}

/* requires: *rope is allocated and writable.
 * ensures:  rope(rope, bytes, share) where bytes is empty.
 */
void rope_initialize_empty(Rope *rope)
{
    rope->root = NULL;
    rope->height = 0;
    rope->byte_count = 0;
    rope->newline_count = 0;
}

/* requires: node_pool(pool, live, residual); holds a read share of `length`
 *           bytes at `bytes`; *rope writable.
 * ensures:  the read share is returned; rope(rope, contents, share) copying
 *           those bytes, result 1; or *rope is empty and the result is 0.
 */
int rope_from_bytes(Pool *pool, const unsigned char *bytes, uint32_t length,
                    Rope *rope)
{
    void          **nodes;
    uint32_t       *sizes;
    uint32_t       *newlines;
    unsigned char  *leaf;
    RopeNode       *built;
    uint32_t        leaf_total;
    uint32_t        index;
    uint32_t        offset;
    uint32_t        chunk;
    uint32_t        remaining;
    uint32_t        level_count;
    uint32_t        read_index;
    uint32_t        write_index;
    uint32_t        group;
    uint32_t        height;
    uint32_t        slot_bytes;
    uint32_t        slot_newlines;
    uint32_t        built_bytes;
    uint32_t        built_newlines;
    void           *root_node;
    uint32_t        root_bytes;
    uint32_t        root_newlines;

    rope_initialize_empty(rope);
    if (length == 0) {
        return 1;
    }

    leaf_total = length / ROPE_LEAF_BYTES;
    remaining = length % ROPE_LEAF_BYTES;
    if (remaining > 0) {
        leaf_total = leaf_total + 1;
    }

    /* Transient scaffolding for the bottom-up build, not part of the tree. */
    nodes = malloc(leaf_total * sizeof(void *));
    sizes = malloc(leaf_total * sizeof(uint32_t));
    newlines = malloc(leaf_total * sizeof(uint32_t));
    if (nodes == NULL || sizes == NULL || newlines == NULL) {
        free(nodes);
        free(sizes);
        free(newlines);
        return 0;
    }

    offset = 0;
    index = 0;
    while (index < leaf_total) {
        chunk = length - offset;
        if (chunk > ROPE_LEAF_BYTES) {
            chunk = ROPE_LEAF_BYTES;
        }
        leaf = allocate_leaf(pool);
        if (leaf == NULL) {
            free(nodes);
            free(sizes);
            free(newlines);
            return 0;
        }
        memcpy(leaf, bytes + offset, chunk);
        nodes[index] = leaf;
        sizes[index] = chunk;
        newlines[index] = count_newlines(leaf, chunk);
        offset = offset + chunk;
        index = index + 1;
    }

    level_count = leaf_total;
    height = 0;
    while (level_count > 1) {
        height = height + 1;
        if (height > ROPE_MAX_HEIGHT) {
            free(nodes);
            free(sizes);
            free(newlines);
            return 0;
        }
        read_index = 0;
        write_index = 0;
        while (read_index < level_count) {
            group = level_count - read_index;
            if (group > ROPE_BRANCHING) {
                group = ROPE_BRANCHING;
            }
            built = make_node(pool, height, nodes + read_index,
                              sizes + read_index, newlines + read_index,
                              group, &slot_bytes, &slot_newlines);
            if (built == NULL) {
                free(nodes);
                free(sizes);
                free(newlines);
                return 0;
            }
            built_bytes = slot_bytes;
            built_newlines = slot_newlines;
            nodes[write_index] = built;
            sizes[write_index] = built_bytes;
            newlines[write_index] = built_newlines;
            write_index = write_index + 1;
            read_index = read_index + group;
        }
        level_count = write_index;
    }

    root_node = nodes[0];
    root_bytes = sizes[0];
    root_newlines = newlines[0];
    rope->root = root_node;
    rope->height = height;
    rope->byte_count = root_bytes;
    rope->newline_count = root_newlines;

    free(nodes);
    free(sizes);
    free(newlines);
    return 1;
}

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is |bytes|. No memory is
 *           written.
 */
uint32_t rope_byte_count(const Rope *rope)
{
    uint32_t total;

    total = rope->byte_count;
    return total;
}

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is the newline count. No
 *           memory is written.
 */
uint32_t rope_newline_count(const Rope *rope)
{
    uint32_t total;

    total = rope->newline_count;
    return total;
}

/* requires: holds a read share of the subtree at `node`; offset is within it;
 *           *value writable.
 * ensures:  the read share is returned; *value is the byte at that offset.
 */
static void byte_at_node(const void *node, uint32_t height, uint32_t offset,
                         unsigned char *value)
{
    unsigned char        found;
    const RopeNode      *source;
    const unsigned char *leaf;
    uint32_t             index;
    uint32_t             child_count;
    uint32_t             boundary;
    uint32_t             previous;
    const void          *child;

    if (height == 0) {
        leaf = node;
        found = leaf[offset];
        *value = found;
        return;
    }

    source = node;
    child_count = source->child_count;
    index = 0;
    previous = 0;
    while (index < child_count) {
        boundary = source->cumulative_bytes[index];
        if (offset < boundary) {
            child = source->children[index];
            byte_at_node(child, height - 1, offset - previous, value);
            return;
        }
        previous = boundary;
        index = index + 1;
    }
}

/* requires: rope(rope, bytes, share); *value writable.
 * ensures:  rope(rope, bytes, share); on an in-range offset *value is that
 *           byte and the result is 1, otherwise the result is 0.
 */
int rope_byte_at(const Rope *rope, uint32_t offset, unsigned char *value)
{
    uint32_t    total;
    uint32_t    height;
    const void *root;

    total = rope->byte_count;
    if (offset >= total) {
        return 0;
    }
    root = rope->root;
    height = rope->height;
    byte_at_node(root, height, offset, value);
    return 1;
}

/* requires: holds a read share of the subtree at `node`; [offset, offset +
 *           length) lies within it; holds a write share of `length` bytes at
 *           `destination`.
 * ensures:  the read share is returned; `destination` holds that slice.
 */
static void copy_from_node(const void *node, uint32_t height, uint32_t offset,
                           uint32_t length, unsigned char *destination)
{
    const RopeNode      *source;
    const unsigned char *leaf;
    uint32_t             index;
    uint32_t             child_count;
    uint32_t             boundary;
    uint32_t             previous;
    uint32_t             start;
    uint32_t             available;
    uint32_t             portion;
    uint32_t             written;
    const void          *child;

    if (length == 0) {
        return;
    }

    if (height == 0) {
        leaf = node;
        memcpy(destination, leaf + offset, length);
        return;
    }

    source = node;
    child_count = source->child_count;
    index = 0;
    previous = 0;
    written = 0;
    while (index < child_count) {
        boundary = source->cumulative_bytes[index];
        if (offset < boundary) {
            start = offset - previous;
            available = boundary - offset;
            portion = length - written;
            if (portion > available) {
                portion = available;
            }
            child = source->children[index];
            copy_from_node(child, height - 1, start, portion,
                           destination + written);
            written = written + portion;
            offset = offset + portion;
            if (written == length) {
                return;
            }
        }
        previous = boundary;
        index = index + 1;
    }
}

/* requires: rope(rope, bytes, share); holds a write share of `length` bytes
 *           at `destination`.
 * ensures:  rope(rope, bytes, share); on an in-range slice `destination`
 *           holds it and the result is 1, otherwise the result is 0.
 */
int rope_copy_range(const Rope *rope, uint32_t offset, uint32_t length,
                    unsigned char *destination)
{
    uint32_t    total;
    uint32_t    height;
    uint32_t    limit;
    const void *root;

    total = rope->byte_count;
    limit = offset + length;
    if (limit > total) {
        return 0;
    }
    if (length == 0) {
        return 1;
    }
    root = rope->root;
    height = rope->height;
    copy_from_node(root, height, offset, length, destination);
    return 1;
}

/* requires: node_pool(pool, live, residual); rope(rope, bytes, share); *left
 *           and *right writable.
 * ensures:  as rope.h.
 */
int rope_split(Pool *pool, const Rope *rope, uint32_t offset,
               Rope *left, Rope *right)
{
    void    *root;
    uint32_t height;
    uint32_t total;
    uint32_t newlines;
    void    *produced_node;
    uint32_t produced_bytes;
    uint32_t produced_newlines;
    void    *stored_node;
    uint32_t stored_bytes;
    uint32_t stored_newlines;
    int      outcome;

    total = rope->byte_count;
    newlines = rope->newline_count;
    root = rope->root;
    height = rope->height;

    rope_initialize_empty(left);
    rope_initialize_empty(right);

    if (total == 0) {
        return 1;
    }

    outcome = take_node(pool, root, height, total, newlines, offset,
                        &produced_node, &produced_bytes, &produced_newlines);
    if (outcome == FAILED) {
        return 0;
    }
    if (outcome == PRODUCED) {
        stored_node = produced_node;
        stored_bytes = produced_bytes;
        stored_newlines = produced_newlines;
        left->root = stored_node;
        left->height = height;
        left->byte_count = stored_bytes;
        left->newline_count = stored_newlines;
        collapse_root(left);
    }

    outcome = drop_node(pool, root, height, total, newlines, offset,
                        &produced_node, &produced_bytes, &produced_newlines);
    if (outcome == FAILED) {
        return 0;
    }
    if (outcome == PRODUCED) {
        stored_node = produced_node;
        stored_bytes = produced_bytes;
        stored_newlines = produced_newlines;
        right->root = stored_node;
        right->height = height;
        right->byte_count = stored_bytes;
        right->newline_count = stored_newlines;
        collapse_root(right);
    }

    return 1;
}

/* requires: node_pool(pool, live, residual); rope(left, a, s) and
 *           rope(right, b, t); *result writable.
 * ensures:  as rope.h.
 */
int rope_concat(Pool *pool, const Rope *left, const Rope *right, Rope *result)
{
    NodePair  pair;
    ChildList list;
    RopeNode *root;
    uint32_t  left_bytes;
    uint32_t  right_bytes;
    uint32_t  left_newlines;
    uint32_t  right_newlines;
    uint32_t  left_height;
    uint32_t  right_height;
    uint32_t  total_bytes;
    uint32_t  total_newlines;
    uint32_t  slot_bytes;
    uint32_t  slot_newlines;
    uint32_t  height;
    uint32_t  pair_count;
    void     *first_node;
    uint32_t  first_bytes;
    uint32_t  first_newlines;
    void     *second_node;
    uint32_t  second_bytes;
    uint32_t  second_newlines;
    void     *left_root;
    void     *right_root;
    int       outcome;

    left_bytes = left->byte_count;
    right_bytes = right->byte_count;

    if (left_bytes == 0) {
        memcpy(result, right, sizeof(Rope));
        return 1;
    }
    if (right_bytes == 0) {
        memcpy(result, left, sizeof(Rope));
        return 1;
    }

    total_bytes = left_bytes + right_bytes;
    if (total_bytes < left_bytes) {
        return 0;
    }

    left_newlines = left->newline_count;
    right_newlines = right->newline_count;
    left_height = left->height;
    right_height = right->height;
    left_root = left->root;
    right_root = right->root;

    outcome = concat_rec(pool, left_root, left_height, left_bytes,
                         left_newlines, right_root, right_height, right_bytes,
                         right_newlines, &pair);
    if (outcome == FAILED) {
        return 0;
    }

    total_newlines = left_newlines + right_newlines;
    height = pair.height;
    pair_count = pair.count;

    first_node = pair.nodes[0];
    first_bytes = pair.bytes[0];
    first_newlines = pair.newlines[0];

    if (pair_count == 1) {
        rope_initialize_empty(result);
        result->root = first_node;
        result->height = height;
        result->byte_count = total_bytes;
        result->newline_count = total_newlines;
        return 1;
    }

    height = height + 1;
    if (height > ROPE_MAX_HEIGHT) {
        return 0;
    }

    second_node = pair.nodes[1];
    second_bytes = pair.bytes[1];
    second_newlines = pair.newlines[1];

    list.count = 0;
    child_list_append(&list, first_node, first_bytes, first_newlines);
    child_list_append(&list, second_node, second_bytes, second_newlines);

    root = make_node(pool, height, list.nodes, list.bytes, list.newlines, 2,
                     &slot_bytes, &slot_newlines);
    if (root == NULL) {
        return 0;
    }

    rope_initialize_empty(result);
    result->root = root;
    result->height = height;
    result->byte_count = total_bytes;
    result->newline_count = total_newlines;
    return 1;
}

/* requires: as rope.h.
 * ensures:  as rope.h.
 */
int rope_replace_span(Pool *pool, const Rope *rope,
                      uint32_t start, uint32_t end,
                      const unsigned char *replacement,
                      uint32_t replacement_length,
                      Rope *result)
{
    Rope     prefix;
    Rope     suffix;
    Rope     middle;
    Rope     joined;
    uint32_t total;
    uint32_t discard;
    int      outcome;

    total = rope->byte_count;
    if (start > end) {
        return 0;
    }
    if (end > total) {
        return 0;
    }

    /* The decomposition the whole command language is built on:
     * Buffer = prefix ++ focus ++ suffix, with a new focus put back. */
    rope_initialize_empty(&prefix);
    rope_initialize_empty(&suffix);
    rope_initialize_empty(&middle);

    outcome = rope_split(pool, rope, start, &prefix, &suffix);
    if (outcome == 0) {
        return 0;
    }

    discard = end - start;
    outcome = rope_split(pool, &suffix, discard, &middle, &suffix);
    if (outcome == 0) {
        return 0;
    }

    outcome = rope_from_bytes(pool, replacement, replacement_length, &middle);
    if (outcome == 0) {
        return 0;
    }

    outcome = rope_concat(pool, &prefix, &middle, &joined);
    if (outcome == 0) {
        return 0;
    }

    outcome = rope_concat(pool, &joined, &suffix, result);
    return outcome;
}

/* requires: holds a read share of the subtree at `node`; offset is at most
 *           its byte count.
 * ensures:  the read share is returned; the result is the number of newlines
 *           strictly before that offset.
 */
static uint32_t rank_newlines(const void *node, uint32_t height,
                              uint32_t offset)
{
    const RopeNode      *source;
    const unsigned char *leaf;
    uint32_t             index;
    uint32_t             child_count;
    uint32_t             boundary;
    uint32_t             previous;
    uint32_t             previous_newlines;
    uint32_t             deeper;
    const void          *child;

    if (height == 0) {
        leaf = node;
        return count_newlines(leaf, offset);
    }

    source = node;
    child_count = source->child_count;
    index = 0;
    previous = 0;
    previous_newlines = 0;
    while (index < child_count) {
        boundary = source->cumulative_bytes[index];
        if (offset <= boundary) {
            child = source->children[index];
            deeper = rank_newlines(child, height - 1, offset - previous);
            return previous_newlines + deeper;
        }
        previous = boundary;
        previous_newlines = source->cumulative_newlines[index];
        index = index + 1;
    }
    return previous_newlines;
}

/* requires: holds a read share of the subtree at `node`, which contains more
 *           than `which` newlines; *offset writable.
 * ensures:  the read share is returned; *offset is the position of the
 *           newline with that zero-based index.
 */
static void select_newline(const void *node, uint32_t height, uint32_t which,
                           uint32_t *offset)
{
    uint32_t             resolved;
    const RopeNode      *source;
    const unsigned char *leaf;
    uint32_t             index;
    uint32_t             child_count;
    uint32_t             boundary;
    uint32_t             previous;
    uint32_t             previous_bytes;
    uint32_t             seen;
    uint32_t             deeper;
    unsigned char        value;
    const void          *child;

    if (height == 0) {
        leaf = node;
        index = 0;
        seen = 0;
        while (index < ROPE_LEAF_BYTES) {
            value = leaf[index];
            if (value == 0x0A) {
                if (seen == which) {
                    *offset = index;
                    return;
                }
                seen = seen + 1;
            }
            index = index + 1;
        }
        return;
    }

    source = node;
    child_count = source->child_count;
    index = 0;
    previous = 0;
    previous_bytes = 0;
    while (index < child_count) {
        boundary = source->cumulative_newlines[index];
        if (which < boundary) {
            child = source->children[index];
            deeper = 0;
            select_newline(child, height - 1, which - previous, &deeper);
            resolved = deeper;
            *offset = previous_bytes + resolved;
            return;
        }
        previous = boundary;
        previous_bytes = source->cumulative_bytes[index];
        index = index + 1;
    }
}

/* requires: rope(rope, bytes, share); *offset writable.
 * ensures:  as rope.h.
 */
int rope_line_start(const Rope *rope, uint32_t line_index, uint32_t *offset)
{
    uint32_t    newlines;
    uint32_t    resolved;
    uint32_t    height;
    uint32_t    position;
    const void *root;

    if (line_index == 0) {
        *offset = 0;
        return 1;
    }

    newlines = rope->newline_count;
    if (line_index > newlines) {
        return 0;
    }

    root = rope->root;
    height = rope->height;
    position = 0;
    select_newline(root, height, line_index - 1, &position);
    resolved = position;
    *offset = resolved + 1;
    return 1;
}

/* requires: rope(rope, bytes, share); *line_index writable.
 * ensures:  as rope.h.
 */
int rope_line_of_offset(const Rope *rope, uint32_t offset,
                        uint32_t *line_index)
{
    uint32_t    total;
    uint32_t    height;
    uint32_t    rank;
    const void *root;

    total = rope->byte_count;
    if (offset > total) {
        return 0;
    }
    if (total == 0) {
        *line_index = 0;
        return 1;
    }

    root = rope->root;
    height = rope->height;
    rank = rank_newlines(root, height, offset);
    *line_index = rank;
    return 1;
}

/* requires: holds a read share of the subtree at `node`; the out-parameters
 *           are writable.
 * ensures:  the read share is returned; the result is 1 when every structural
 *           invariant holds beneath `node`, and the measures are reported.
 */
static int check_node(const void *node, uint32_t height, uint32_t *out_bytes,
                      uint32_t *out_newlines)
{
    uint32_t        slot_bytes;
    uint32_t        slot_newlines;
    uint32_t        measured_bytes;
    uint32_t        measured_newlines;
    const RopeNode *source;
    uint32_t        index;
    uint32_t        child_count;
    uint32_t        running_bytes;
    uint32_t        running_newlines;
    uint32_t        child_bytes;
    uint32_t        child_newlines;
    uint32_t        recorded;
    const void     *child;
    int             ok;

    if (height == 0) {
        /* A leaf's own length is not recorded anywhere beneath its parent,
         * so there is nothing to check here; the parent checked it. */
        return 1;
    }

    source = node;
    child_count = source->child_count;
    if (child_count == 0) {
        return 0;
    }
    if (child_count > ROPE_BRANCHING) {
        return 0;
    }
    recorded = source->height;
    if (recorded != height) {
        return 0;
    }

    running_bytes = 0;
    running_newlines = 0;
    index = 0;
    while (index < child_count) {
        child = source->children[index];
        if (child == NULL) {
            return 0;
        }
        child_bytes = node_child_bytes(source, index);
        child_newlines = node_child_newlines(source, index);

        if (height == 1) {
            if (child_bytes > ROPE_LEAF_BYTES) {
                return 0;
            }
            if (child_bytes == 0) {
                return 0;
            }
        } else {
            ok = check_node(child, height - 1, &slot_bytes, &slot_newlines);
            if (ok == 0) {
                return 0;
            }
            measured_bytes = slot_bytes;
            measured_newlines = slot_newlines;
            if (measured_bytes != child_bytes) {
                return 0;
            }
            if (measured_newlines != child_newlines) {
                return 0;
            }
        }

        running_bytes = running_bytes + child_bytes;
        running_newlines = running_newlines + child_newlines;
        index = index + 1;
    }

    *out_bytes = running_bytes;
    *out_newlines = running_newlines;
    return 1;
}

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is 1 when every structural
 *           invariant holds. No memory is written.
 */
int rope_check_invariants(const Rope *rope)
{
    uint32_t    total;
    uint32_t    newlines;
    uint32_t    height;
    uint32_t    measured_bytes;
    uint32_t    measured_newlines;
    uint32_t    counted_bytes;
    uint32_t    counted_newlines;
    const void *root;
    int         ok;

    total = rope->byte_count;
    height = rope->height;
    root = rope->root;

    if (total == 0) {
        if (root != NULL) {
            return 0;
        }
        return 1;
    }
    if (root == NULL) {
        return 0;
    }
    if (height > ROPE_MAX_HEIGHT) {
        return 0;
    }
    if (height == 0) {
        if (total > ROPE_LEAF_BYTES) {
            return 0;
        }
        return 1;
    }

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
    newlines = rope->newline_count;
    if (counted_newlines != newlines) {
        return 0;
    }
    return 1;
}

/* requires: as rope.h.
 * ensures:  as rope.h.
 */
int rope_slice(Pool *pool, const Rope *rope, uint32_t start, uint32_t end,
               Rope *slice)
{
    Rope     head;
    Rope     tail;
    Rope     discarded;
    uint32_t total;
    int      outcome;

    total = rope->byte_count;
    if (start > end) {
        return 0;
    }
    if (end > total) {
        return 0;
    }

    outcome = rope_split(pool, rope, end, &head, &discarded);
    if (outcome == 0) {
        return 0;
    }
    outcome = rope_split(pool, &head, start, &discarded, &tail);
    if (outcome == 0) {
        return 0;
    }
    memcpy(slice, &tail, sizeof(Rope));
    return 1;
}
