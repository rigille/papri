#include "rope.h"

#include <stdlib.h>
#include <string.h>

/* Leaves are headerless: a leaf is ROPE_LEAF_BYTES of payload and nothing
 * else. A child's byte count
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
    size_t   cumulative_bytes[ROPE_BRANCHING];
};

/* A merge can hold both parents' children plus the one or two nodes their
 * adjoining edges produced: (32-1) + 2 + (32-1). */
#define CHILD_LIST_CAPACITY (ROPE_BRANCHING * 2 + 2)

typedef struct ChildList {
    void    *nodes[CHILD_LIST_CAPACITY];
    size_t   bytes[CHILD_LIST_CAPACITY];
    uint32_t count;
} ChildList;

/* One or two nodes at a common height — what a concatenation step yields
 * before its parent decides whether they need a new level above them. */
typedef struct NodePair {
    void    *nodes[2];
    size_t   bytes[2];
    uint32_t count;
    uint32_t height;
} NodePair;

/* The RRB rebalance invariant, from immer, which takes it from the Bagwell
 * and Rompf paper. After a concatenation the merged children are
 * redistributed until their number is within RRB_EXTRAS of the minimum that
 * could hold them, skipping any node already at least RRB_INVARIANT short of
 * full.
 *
 * This is the "Balanced" in Relaxed Radix Balanced, and without it the other
 * two letters do not work either: nothing bounds how far a node can drift
 * from the radix layout, so nothing bounds how far the radix guess in
 * child_for_offset can be wrong, and repeated concatenation of small pieces
 * leaves ever thinner nodes with nothing to repair them.
 */
#define RRB_EXTRAS    2
#define RRB_INVARIANT 1

/* Recursive helpers return 1 when they produced a node, 0 when the result is
 * empty, and -1 when allocation failed. Distinguishing the last two matters:
 * an empty result is ordinary, a failure must propagate. */
#define PRODUCED 1
#define EMPTY    0
#define FAILED   (-1)

/* requires: rope_node(node, contents, share); index < node->child_count.
 * ensures:  rope_node(node, contents, share); the result is that child's own
 *           byte count. No memory is written.
 */
static size_t node_child_bytes(const RopeNode *node, uint32_t index)
{
    size_t   current;
    size_t   previous;

    current = node->cumulative_bytes[index];
    if (index == 0) {
        return current;
    }
    previous = node->cumulative_bytes[index - 1];
    return current - previous;
}

/* Atomic, not scanned. A leaf is ROPE_LEAF_BYTES of file content; scanning
 * it would read 32 arbitrary words as candidate pointers, and any one that
 * happened to land inside the heap would hold a dead node alive. Text is
 * exactly the kind of data a conservative collector must be told to ignore.
 *
 * requires: node_pool(pool, allocated).
 * ensures:  node_pool(pool, allocated + ROPE_LEAF_BYTES) with one fresh
 *           zeroed leaf that the collector never scans, and the result
 *           points at it; or the result is null.
 */
static unsigned char *allocate_leaf(Pool *pool)
{
    void *allocation;

    allocation = pool_allocate_atomic(pool, ROPE_LEAF_BYTES);
    return allocation;
}

/* requires: node_pool(pool, live, residual); holds read shares of the `count`
 *           children described by the three parallel arrays;
 *           count <= ROPE_BRANCHING; *out_bytes is writable.
 * ensures:  node_pool(pool, live', residual) with one fresh sealed node whose
 *           children are those, *out_bytes is its total,
 *           and the result points at it; or the result is null and the
 *           outputs are unspecified.
 */
static RopeNode *make_node(Pool *pool, uint32_t height,
                           void *const *children,
                           const size_t *child_bytes,
                           uint32_t count,
                           size_t *out_bytes)
{
    void     *allocation;
    RopeNode *node;
    uint32_t  index;
    size_t    running_bytes;
    size_t    own_bytes;
    void     *child;

    allocation = pool_allocate(pool, sizeof(RopeNode));
    if (allocation == NULL) {
        return NULL;
    }
    node = allocation;

    node->height = height;
    node->child_count = count;

    running_bytes = 0;
    index = 0;
    while (index < count) {
        child = children[index];
        own_bytes = child_bytes[index];

        running_bytes = running_bytes + own_bytes;

        node->children[index] = child;
        node->cumulative_bytes[index] = running_bytes;
        index = index + 1;
    }

    *out_bytes = running_bytes;
    return node;
}


/* Defined below, beside the rest of the child-list helpers.
 *
 * requires: *list writable; list->count < CHILD_LIST_CAPACITY; holds a read
 *           share of `node`.
 * ensures:  *list has that child appended.
 */
static void child_list_append(ChildList *list, void *node, size_t bytes);

/* requires: rope_node(node, contents, share) at `height` above 0; offset is
 *           within its byte count.
 * ensures:  rope_node(node, contents, share); the result is the index of the
 *           child holding that offset. No memory is written.
 *
 * The descent immer uses. A full child of a node at `height` covers
 * 2^child_shift bytes, so no child can hold more than that, so the radix
 * quotient is a lower bound on the answer and the scan only ever moves
 * forward from it. On a tree that has not been relaxed the guess is exact;
 * the rebalance invariant is what keeps it close otherwise.
 */
static uint32_t child_for_offset(const RopeNode *node, uint32_t height,
                                 size_t offset)
{
    uint32_t child_shift;
    uint32_t index;
    uint32_t child_count;
    size_t   boundary;

    child_shift = ROPE_LEAF_BITS + ROPE_BRANCH_BITS * (height - 1);
    child_count = node->child_count;

    index = (uint32_t)(offset >> child_shift);
    if (index >= child_count) {
        index = child_count - 1;
    }

    boundary = node->cumulative_bytes[index];
    while (boundary <= offset) {
        index = index + 1;
        if (index >= child_count) {
            return child_count - 1;
        }
        boundary = node->cumulative_bytes[index];
    }
    return index;
}

/* requires: holds a read share of the node at `node`, at `height`, whose
 *           byte count is `node_bytes`.
 * ensures:  the read share is returned; the result is how many slots it
 *           holds — bytes for a leaf, children for anything above.
 */
static uint32_t node_slot_count(const void *node, uint32_t height,
                                size_t node_bytes)
{
    const RopeNode *source;
    uint32_t        total;

    if (height == 0) {
        return (uint32_t)node_bytes;
    }
    source = node;
    total = source->child_count;
    return total;
}

/* Work out how the slots should be spread, without moving anything yet.
 *
 * requires: `counts` holds `count` slot counts summing to `total`; *planned
 *           is writable and at least as long.
 * ensures:  *planned holds the new spread and the result is how many nodes
 *           it needs, which is within RRB_EXTRAS of the minimum; or the
 *           spread is already good enough and the result is `count`.
 */
static uint32_t plan_rebalance(const uint32_t *counts, uint32_t count,
                               uint32_t branches, uint32_t *planned)
{
    uint32_t total;
    uint32_t optimal;
    uint32_t remaining;
    uint32_t following;
    uint32_t taken;
    uint32_t index;
    uint32_t position;
    uint32_t live;
    uint32_t threshold;
    uint32_t slots;

    total = 0;
    index = 0;
    while (index < count) {
        slots = counts[index];
        total = total + slots;
        planned[index] = slots;
        index = index + 1;
    }

    if (total == 0) {
        return count;
    }
    optimal = (total + branches - 1) / branches;
    live = count;
    threshold = optimal + RRB_EXTRAS;
    if (live < threshold) {
        return count;
    }

    threshold = branches - RRB_INVARIANT;
    position = 0;
    live = count;
    while (live >= optimal + RRB_EXTRAS) {
        /* Skip the nodes that are already full enough to leave alone. */
        taken = planned[position];
        while (taken > threshold) {
            position = position + 1;
            taken = planned[position];
        }

        /* Pour this short node into the ones after it. */
        remaining = planned[position];
        while (remaining > 0) {
            following = planned[position + 1];
            taken = remaining + following;
            if (taken > branches) {
                taken = branches;
            }
            planned[position] = taken;
            remaining = remaining + following - taken;
            position = position + 1;
        }

        /* It has been emptied into its neighbours; drop it. */
        index = position;
        while (index + 1 < live) {
            slots = planned[index + 1];
            planned[index] = slots;
            index = index + 1;
        }
        live = live - 1;
        position = position - 1;
    }
    return live;
}

/* Move the slots to match the plan.
 *
 * requires: node_pool(pool, live, residual); *list holds nodes at `height`;
 *           `planned` holds `live_count` slot counts summing to the slots
 *           the list already holds.
 * ensures:  node_pool(pool, live', residual); *list holds `live_count` fresh
 *           nodes at `height` denoting the same sequence, and the result is
 *           PRODUCED; or FAILED.
 */
static int apply_rebalance(Pool *pool, uint32_t height, ChildList *list,
                           const uint32_t *planned, uint32_t live_count)
{
    ChildList       rebuilt;
    void           *gathered_nodes[ROPE_BRANCHING];
    size_t          gathered_bytes[ROPE_BRANCHING];
    const RopeNode *source_node;
    const unsigned char *source_leaf;
    unsigned char  *leaf;
    RopeNode       *built;
    uint32_t        source_index;
    uint32_t        slot_offset;
    uint32_t        produced;
    uint32_t        want;
    uint32_t        filled;
    uint32_t        available;
    uint32_t        portion;
    uint32_t        step;
    uint32_t        child_total;
    size_t          source_bytes;
    size_t          own_bytes;
    size_t          built_bytes;
    size_t          total_bytes;
    void           *child;

    rebuilt.count = 0;
    source_index = 0;
    slot_offset = 0;

    produced = 0;
    while (produced < live_count) {
        want = planned[produced];

        if (height == 0) {
            leaf = allocate_leaf(pool);
            if (leaf == NULL) {
                return FAILED;
            }
            filled = 0;
            while (filled < want) {
                source_bytes = list->bytes[source_index];
                available = (uint32_t)source_bytes - slot_offset;
                portion = want - filled;
                if (portion > available) {
                    portion = available;
                }
                source_leaf = list->nodes[source_index];
                memcpy(leaf + filled, source_leaf + slot_offset, portion);
                filled = filled + portion;
                slot_offset = slot_offset + portion;
                if (slot_offset == source_bytes) {
                    source_index = source_index + 1;
                    slot_offset = 0;
                }
            }
            child_list_append(&rebuilt, leaf, want);
        } else {
            filled = 0;
            while (filled < want) {
                source_node = list->nodes[source_index];
                child_total = source_node->child_count;
                available = child_total - slot_offset;
                portion = want - filled;
                if (portion > available) {
                    portion = available;
                }
                step = 0;
                while (step < portion) {
                    child = source_node->children[slot_offset + step];
                    gathered_nodes[filled + step] = child;
                    own_bytes = node_child_bytes(source_node,
                                                slot_offset + step);
                    gathered_bytes[filled + step] = own_bytes;
                    step = step + 1;
                }
                filled = filled + portion;
                slot_offset = slot_offset + portion;
                if (slot_offset == child_total) {
                    source_index = source_index + 1;
                    slot_offset = 0;
                }
            }
            built = make_node(pool, height, gathered_nodes, gathered_bytes, want, &total_bytes);
            if (built == NULL) {
                return FAILED;
            }
            built_bytes = total_bytes;
            child_list_append(&rebuilt, built, built_bytes);
        }

        produced = produced + 1;
    }

    memcpy(list, &rebuilt, sizeof(ChildList));
    return PRODUCED;
}

/* requires: node_pool(pool, live, residual); *list holds nodes at `height`.
 * ensures:  node_pool(pool, live', residual); *list denotes the same
 *           sequence with its nodes' count within RRB_EXTRAS of the minimum
 *           that could hold them, and the result is PRODUCED; or FAILED.
 *           When the list is already good enough it is left alone, which is
 *           the common case and costs one pass over the counts.
 */
static int rebalance(Pool *pool, uint32_t height, ChildList *list)
{
    uint32_t counts[CHILD_LIST_CAPACITY];
    uint32_t planned[CHILD_LIST_CAPACITY];
    uint32_t branches;
    uint32_t count;
    uint32_t live_count;
    uint32_t index;
    size_t   own_bytes;
    void    *node;
    int      outcome;

    count = list->count;
    if (count < 2) {
        return PRODUCED;
    }

    branches = ROPE_BRANCHING;
    if (height == 0) {
        branches = ROPE_LEAF_BYTES;
    }

    index = 0;
    while (index < count) {
        node = list->nodes[index];
        own_bytes = list->bytes[index];
        counts[index] = node_slot_count(node, height, own_bytes);
        index = index + 1;
    }

    live_count = plan_rebalance(counts, count, branches, planned);
    if (live_count == count) {
        return PRODUCED;
    }

    outcome = apply_rebalance(pool, height, list, planned, live_count);
    return outcome;
}

/* requires: node_pool(pool, live, residual); holds read shares of list's
 *           children; 0 < list->count <= CHILD_LIST_CAPACITY; *result
 *           writable.
 * ensures:  node_pool(pool, live', residual); *result holds one node when the
 *           children fit in a single node and two roughly equal ones when
 *           they do not, all at `height`, and the outcome is PRODUCED; or
 *           FAILED.
 */
static int pack_children(Pool *pool, uint32_t height, ChildList *list,
                         NodePair *result)
{
    int              rebalanced;
    uint32_t         count;
    uint32_t         left_count;
    uint32_t         right_count;
    RopeNode        *node;
    void *const     *child_slice;
    const size_t    *bytes_slice;

    /* Redistribute before grouping: this is where the "Balanced" happens,
     * and it is what keeps the radix guess in child_for_offset close to the
     * answer. */
    rebalanced = rebalance(pool, height - 1, list);
    if (rebalanced == FAILED) {
        return FAILED;
    }

    count = list->count;

    if (count <= ROPE_BRANCHING) {
        node = make_node(pool, height, list->nodes, list->bytes, count,
                         &result->bytes[0]);
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

    node = make_node(pool, height, list->nodes, list->bytes,
                     left_count, &result->bytes[0]);
    if (node == NULL) {
        return FAILED;
    }
    result->nodes[0] = node;

    child_slice = list->nodes + left_count;
    bytes_slice = list->bytes + left_count;

    node = make_node(pool, height, child_slice, bytes_slice,
                     right_count, &result->bytes[1]);
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
static void child_list_append(ChildList *list, void *node, size_t bytes)
{
    uint32_t count;

    count = list->count;
    list->nodes[count] = node;
    list->bytes[count] = bytes;
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
 *           the out-parameters are writable.
 * ensures:  node_pool(pool, live', residual); on PRODUCED the outputs hold a
 *           subtree at `height` denoting the first `count` bytes, sharing
 *           whatever nodes it can with the input; on EMPTY count was zero; on
 *           FAILED allocation failed.
 */
static int take_node(Pool *pool, void *node, uint32_t height,
                     size_t node_bytes,
                     size_t count,
                     void **out_node, size_t *out_bytes)
{
    const RopeNode *source;
    ChildList       list;
    unsigned char  *leaf;
    const unsigned char *source_bytes;
    uint32_t        index;
    uint32_t        child_total;
    size_t          running;
    size_t          next_running;
    size_t          own_bytes;
    size_t          remainder;
    void           *child;
    void           *sub_node;
    size_t          sub_bytes;
    void           *taken_node;
    size_t          taken_bytes;
    RopeNode       *built;
    int             outcome;

    if (count == 0) {
        return EMPTY;
    }
    if (count >= node_bytes) {
        *out_node = node;
        *out_bytes = node_bytes;
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
        return PRODUCED;
    }

    source = node;
    child_total = source->child_count;
    list.count = 0;
    running = 0;
    index = 0;
    while (index < child_total) {
        own_bytes = node_child_bytes(source, index);
        next_running = running + own_bytes;
        child = source->children[index];

        if (next_running <= count) {
            child_list_append(&list, child, own_bytes);
            running = next_running;
            if (running == count) {
                index = child_total;
            } else {
                index = index + 1;
            }
        } else {
            remainder = count - running;
            outcome = take_node(pool, child, height - 1, own_bytes,
                                remainder,
                                &sub_node, &sub_bytes);
            if (outcome == FAILED) {
                return FAILED;
            }
            if (outcome == PRODUCED) {
                taken_node = sub_node;
                taken_bytes = sub_bytes;
                child_list_append(&list, taken_node, taken_bytes);
            }
            index = child_total;
        }
    }

    child_total = list.count;
    if (child_total == 0) {
        return EMPTY;
    }

    built = make_node(pool, height, list.nodes, list.bytes,
                      child_total, out_bytes);
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
 *           the out-parameters are writable.
 * ensures:  node_pool(pool, live', residual); on PRODUCED the outputs hold a
 *           subtree at `height` denoting the bytes from `count` onward,
 *           sharing whatever it can; on EMPTY nothing remained; on FAILED
 *           allocation failed.
 */
static int drop_node(Pool *pool, void *node, uint32_t height,
                     size_t node_bytes,
                     size_t count,
                     void **out_node, size_t *out_bytes)
{
    const RopeNode *source;
    ChildList       list;
    unsigned char  *leaf;
    const unsigned char *source_bytes;
    uint32_t        index;
    uint32_t        child_total;
    size_t          running;
    size_t          next_running;
    size_t          own_bytes;
    size_t          remainder;
    size_t          kept;
    void           *child;
    void           *sub_node;
    size_t          sub_bytes;
    void           *taken_node;
    size_t          taken_bytes;
    RopeNode       *built;
    int             outcome;

    if (count == 0) {
        *out_node = node;
        *out_bytes = node_bytes;
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
        return PRODUCED;
    }

    source = node;
    child_total = source->child_count;
    list.count = 0;
    running = 0;
    index = 0;
    while (index < child_total) {
        own_bytes = node_child_bytes(source, index);
        next_running = running + own_bytes;
        child = source->children[index];

        if (next_running <= count) {
            /* Entirely before the cut: drop it. */
            running = next_running;
        } else {
            if (running >= count) {
                child_list_append(&list, child, own_bytes);
            } else {
                remainder = count - running;
                outcome = drop_node(pool, child, height - 1, own_bytes,
                                    remainder,
                                    &sub_node, &sub_bytes);
                if (outcome == FAILED) {
                    return FAILED;
                }
                if (outcome == PRODUCED) {
                    taken_node = sub_node;
                    taken_bytes = sub_bytes;
                    child_list_append(&list, taken_node, taken_bytes);
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

    built = make_node(pool, height, list.nodes, list.bytes,
                      child_total, out_bytes);
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
                      size_t left_bytes,
                      void *right, uint32_t right_height,
                      size_t right_bytes,
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
    size_t          total;
    uint32_t        left_count;
    uint32_t        right_count;
    uint32_t        index;
    size_t          edge_bytes;
    size_t          other_bytes;
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
                result->count = 1;
                result->height = 0;
                return PRODUCED;
            }
            result->nodes[0] = left;
            result->bytes[0] = left_bytes;
            result->nodes[1] = right;
            result->bytes[1] = right_bytes;
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

        outcome = concat_rec(pool, edge_child, left_height - 1, edge_bytes,
                             right, right_height, right_bytes, &middle);
        if (outcome == FAILED) {
            return FAILED;
        }

        index = 0;
        while (index + 1 < left_count) {
            other_child = left_node->children[index];
            other_bytes = node_child_bytes(left_node, index);
            child_list_append(&list, other_child, other_bytes);
            index = index + 1;
        }
        index = 0;
        middle_count = middle.count;
        while (index < middle_count) {
            other_child = middle.nodes[index];
            other_bytes = middle.bytes[index];
            child_list_append(&list, other_child, other_bytes);
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

        outcome = concat_rec(pool, left, left_height, left_bytes,
                             edge_child, right_height - 1, edge_bytes, &middle);
        if (outcome == FAILED) {
            return FAILED;
        }

        index = 0;
        middle_count = middle.count;
        while (index < middle_count) {
            other_child = middle.nodes[index];
            other_bytes = middle.bytes[index];
            child_list_append(&list, other_child, other_bytes);
            index = index + 1;
        }
        index = 1;
        while (index < right_count) {
            other_child = right_node->children[index];
            other_bytes = node_child_bytes(right_node, index);
            child_list_append(&list, other_child, other_bytes);
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
    other_child = right_node->children[0];
    other_bytes = node_child_bytes(right_node, 0);

    outcome = concat_rec(pool, edge_child, left_height - 1, edge_bytes,
                         other_child, right_height - 1, other_bytes, &middle);
    if (outcome == FAILED) {
        return FAILED;
    }

    index = 0;
    while (index + 1 < left_count) {
        other_child = left_node->children[index];
        other_bytes = node_child_bytes(left_node, index);
        child_list_append(&list, other_child, other_bytes);
        index = index + 1;
    }
    index = 0;
    middle_count = middle.count;
    while (index < middle_count) {
        other_child = middle.nodes[index];
        other_bytes = middle.bytes[index];
        child_list_append(&list, other_child, other_bytes);
        index = index + 1;
    }
    index = 1;
    while (index < right_count) {
        other_child = right_node->children[index];
        other_bytes = node_child_bytes(right_node, index);
        child_list_append(&list, other_child, other_bytes);
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
}

/* requires: node_pool(pool, live, residual); holds a read share of `length`
 *           bytes at `bytes`; *rope writable.
 * ensures:  the read share is returned; rope(rope, contents, share) copying
 *           those bytes, result 1; or *rope is empty and the result is 0.
 */
int rope_from_bytes(Pool *pool, const unsigned char *bytes, size_t length,
                    Rope *rope)
{
    void          **nodes;
    size_t         *sizes;
    unsigned char  *leaf;
    RopeNode       *built;
    size_t          leaf_total;
    uint32_t        index;
    size_t          offset;
    size_t          chunk;
    size_t          remaining;
    size_t          level_count;
    size_t          read_index;
    size_t          write_index;
    uint32_t        group;
    uint32_t        height;
    size_t          slot_bytes;
    size_t          built_bytes;
    void           *root_node;
    size_t          root_bytes;

    rope_initialize_empty(rope);
    if (length == 0) {
        return 1;
    }

    leaf_total = length / ROPE_LEAF_BYTES;
    remaining = length % ROPE_LEAF_BYTES;
    if (remaining > 0) {
        leaf_total = leaf_total + 1;
    }

    /* Transient scaffolding for the bottom-up build, not part of the tree —
     * but it must come from the pool, not from malloc. While this loop runs,
     * `nodes` holds the ONLY reference to every leaf and every node built so
     * far, and malloc'd memory is not scanned: a collection triggered by the
     * next allocate_leaf would free the tree being built out from under it.
     * `sizes` holds no pointers, so it is atomic. */
    nodes = pool_allocate(pool, leaf_total * sizeof(void *));
    sizes = pool_allocate_atomic(pool, leaf_total * sizeof(size_t));
    if (nodes == NULL || sizes == NULL) {
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
            return 0;
        }
        memcpy(leaf, bytes + offset, chunk);
        nodes[index] = leaf;
        sizes[index] = chunk;
        offset = offset + chunk;
        index = index + 1;
    }

    level_count = leaf_total;
    height = 0;
    while (level_count > 1) {
        height = height + 1;
        if (height > ROPE_MAX_HEIGHT) {
            return 0;
        }
        read_index = 0;
        write_index = 0;
        while (read_index < level_count) {
            remaining = level_count - read_index;
            if (remaining > ROPE_BRANCHING) {
                remaining = ROPE_BRANCHING;
            }
            group = (uint32_t)remaining;
            built = make_node(pool, height, nodes + read_index,
                              sizes + read_index, group, &slot_bytes);
            if (built == NULL) {
                return 0;
            }
            built_bytes = slot_bytes;
            nodes[write_index] = built;
            sizes[write_index] = built_bytes;
            write_index = write_index + 1;
            read_index = read_index + group;
        }
        level_count = write_index;
    }

    root_node = nodes[0];
    root_bytes = sizes[0];
    rope->root = root_node;
    rope->height = height;
    rope->byte_count = root_bytes;

    return 1;
}

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is |bytes|. No memory is
 *           written.
 */
size_t rope_byte_count(const Rope *rope)
{
    size_t   total;

    total = rope->byte_count;
    return total;
}

/* requires: holds a read share of the subtree at `node`; offset is within it;
 *           *value writable.
 * ensures:  the read share is returned; *value is the byte at that offset.
 */
static void byte_at_node(const void *node, uint32_t height, size_t offset,
                         unsigned char *value)
{
    unsigned char        found;
    const RopeNode      *source;
    const unsigned char *leaf;
    uint32_t             index;
    size_t               previous;
    const void          *child;

    if (height == 0) {
        leaf = node;
        found = leaf[offset];
        *value = found;
        return;
    }

    source = node;
    index = child_for_offset(source, height, offset);
    previous = 0;
    if (index > 0) {
        previous = source->cumulative_bytes[index - 1];
    }
    child = source->children[index];
    byte_at_node(child, height - 1, offset - previous, value);
}

/* requires: rope(rope, bytes, share); *value writable.
 * ensures:  rope(rope, bytes, share); on an in-range offset *value is that
 *           byte and the result is 1, otherwise the result is 0.
 */
int rope_byte_at(const Rope *rope, size_t offset, unsigned char *value)
{
    size_t      total;
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
static void copy_from_node(const void *node, uint32_t height, size_t offset,
                           size_t length, unsigned char *destination)
{
    const RopeNode      *source;
    const unsigned char *leaf;
    uint32_t             index;
    uint32_t             child_count;
    size_t               boundary;
    size_t               previous;
    size_t               start;
    size_t               available;
    size_t               portion;
    size_t               written;
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
int rope_copy_range(const Rope *rope, size_t offset, size_t length,
                    unsigned char *destination)
{
    size_t      total;
    uint32_t    height;
    size_t      limit;
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
int rope_split(Pool *pool, const Rope *rope, size_t offset,
               Rope *left, Rope *right)
{
    void    *root;
    uint32_t height;
    size_t   total;
    void    *produced_node;
    size_t   produced_bytes;
    void    *stored_node;
    size_t   stored_bytes;
    int      outcome;

    total = rope->byte_count;
    root = rope->root;
    height = rope->height;

    rope_initialize_empty(left);
    rope_initialize_empty(right);

    if (total == 0) {
        return 1;
    }

    outcome = take_node(pool, root, height, total, offset,
                        &produced_node, &produced_bytes);
    if (outcome == FAILED) {
        return 0;
    }
    if (outcome == PRODUCED) {
        stored_node = produced_node;
        stored_bytes = produced_bytes;
        left->root = stored_node;
        left->height = height;
        left->byte_count = stored_bytes;
        collapse_root(left);
    }

    outcome = drop_node(pool, root, height, total, offset,
                        &produced_node, &produced_bytes);
    if (outcome == FAILED) {
        return 0;
    }
    if (outcome == PRODUCED) {
        stored_node = produced_node;
        stored_bytes = produced_bytes;
        right->root = stored_node;
        right->height = height;
        right->byte_count = stored_bytes;
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
    size_t    left_bytes;
    size_t    right_bytes;
    uint32_t  left_height;
    uint32_t  right_height;
    size_t    total_bytes;
    size_t    slot_bytes;
    uint32_t  height;
    uint32_t  pair_count;
    void     *first_node;
    size_t    first_bytes;
    void     *second_node;
    size_t    second_bytes;
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

    left_height = left->height;
    right_height = right->height;
    left_root = left->root;
    right_root = right->root;

    outcome = concat_rec(pool, left_root, left_height, left_bytes,
                         right_root, right_height, right_bytes,
                         &pair);
    if (outcome == FAILED) {
        return 0;
    }

    height = pair.height;
    pair_count = pair.count;

    first_node = pair.nodes[0];
    first_bytes = pair.bytes[0];

    if (pair_count == 1) {
        rope_initialize_empty(result);
        result->root = first_node;
        result->height = height;
        result->byte_count = total_bytes;
        return 1;
    }

    height = height + 1;
    if (height > ROPE_MAX_HEIGHT) {
        return 0;
    }

    second_node = pair.nodes[1];
    second_bytes = pair.bytes[1];

    list.count = 0;
    child_list_append(&list, first_node, first_bytes);
    child_list_append(&list, second_node, second_bytes);

    root = make_node(pool, height, list.nodes, list.bytes, 2, &slot_bytes);
    if (root == NULL) {
        return 0;
    }

    rope_initialize_empty(result);
    result->root = root;
    result->height = height;
    result->byte_count = total_bytes;
    return 1;
}

/* requires: as rope.h.
 * ensures:  as rope.h.
 */
int rope_replace_span(Pool *pool, const Rope *rope,
                      size_t start, size_t end,
                      const unsigned char *replacement,
                      size_t replacement_length,
                      Rope *result)
{
    Rope     prefix;
    Rope     suffix;
    Rope     middle;
    Rope     joined;
    size_t   total;
    size_t   discard;
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

/* requires: holds a read share of the subtree at `node`; the out-parameters
 *           are writable.
 * ensures:  the read share is returned; the result is 1 when every structural
 *           invariant holds beneath `node`, and the measures are reported.
 */
static int check_node(const void *node, uint32_t height,
                      size_t *out_bytes)
{
    size_t          slot_bytes;
    size_t          measured_bytes;
    const RopeNode *source;
    uint32_t        index;
    uint32_t        child_count;
    size_t          running_bytes;
    size_t          child_bytes;
    size_t          recorded;
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
    index = 0;
    while (index < child_count) {
        child = source->children[index];
        if (child == NULL) {
            return 0;
        }
        child_bytes = node_child_bytes(source, index);

        if (height == 1) {
            if (child_bytes > ROPE_LEAF_BYTES) {
                return 0;
            }
            if (child_bytes == 0) {
                return 0;
            }
        } else {
            ok = check_node(child, height - 1, &slot_bytes);
            if (ok == 0) {
                return 0;
            }
            measured_bytes = slot_bytes;
            if (measured_bytes != child_bytes) {
                return 0;
            }
        }

        running_bytes = running_bytes + child_bytes;
        index = index + 1;
    }

    *out_bytes = running_bytes;
    return 1;
}

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is 1 when every structural
 *           invariant holds. No memory is written.
 */
int rope_check_invariants(const Rope *rope)
{
    size_t      total;
    uint32_t    height;
    size_t      measured_bytes;
    size_t      counted_bytes;
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
    ok = check_node(root, height, &measured_bytes);
    if (ok == 0) {
        return 0;
    }
    counted_bytes = measured_bytes;
    if (counted_bytes != total) {
        return 0;
    }
    return 1;
}

/* requires: as rope.h.
 * ensures:  as rope.h.
 */
int rope_slice(Pool *pool, const Rope *rope, size_t start, size_t end,
               Rope *slice)
{
    Rope     head;
    Rope     tail;
    Rope     trailing;
    Rope     leading;
    size_t   total;
    int      outcome;

    total = rope->byte_count;
    if (start > end) {
        return 0;
    }
    if (end > total) {
        return 0;
    }

    outcome = rope_split(pool, rope, end, &head, &trailing);
    if (outcome == 0) {
        return 0;
    }
    outcome = rope_split(pool, &head, start, &leading, &tail);
    if (outcome == 0) {
        return 0;
    }
    memcpy(slice, &tail, sizeof(Rope));

    /* The other two thirds and the intermediate they were cut from are
     * garbage the moment we return, and nothing collects them. See the note
     * on rope_free_difference's removal in the commit that took it out:
     * the pairwise walk it used was unsound for a set of dead roots. */
    return 1;
}

/* requires: holds a read share of the subtree at `node`.
 * ensures:  the read share is returned; the result is the pool memory that
 *           subtree occupies.
 */
static size_t measure_node(const void *node, uint32_t height)
{
    const RopeNode *source;
    uint32_t        count;
    uint32_t        index;
    const void     *child;
    size_t          total;
    size_t          deeper;

    if (height == 0) {
        return ROPE_LEAF_BYTES;
    }

    source = node;
    count = source->child_count;
    total = sizeof(RopeNode);
    index = 0;
    while (index < count) {
        child = source->children[index];
        deeper = measure_node(child, height - 1);
        total = total + deeper;
        index = index + 1;
    }
    return total;
}

/* requires: as rope.h.
 * ensures:  as rope.h.
 */
size_t rope_allocated_bytes(const Rope *rope)
{
    const void *root;
    uint32_t    height;
    size_t      total;

    root = rope->root;
    if (root == NULL) {
        return 0;
    }
    height = rope->height;
    total = measure_node(root, height);
    return total;
}

/* requires: holds a read share of the subtree at `node`, at `height`.
 * ensures:  the read share is returned; the result is 1 when this node and
 *           every node beneath it satisfies the RRB balance invariant.
 */
static int check_fill_node(const void *node, uint32_t height)
{
    const RopeNode *source;
    const RopeNode *inner;
    const void     *child;
    uint32_t        count;
    uint32_t        index;
    uint32_t        slots;
    uint32_t        optimal;
    uint32_t        branches;
    uint32_t        threshold;
    uint32_t        children_here;
    size_t          own_bytes;
    int             ok;

    if (height == 0) {
        return 1;
    }

    source = node;
    count = source->child_count;

    branches = ROPE_BRANCHING;
    if (height == 1) {
        branches = ROPE_LEAF_BYTES;
    }

    slots = 0;
    index = 0;
    while (index < count) {
        if (height == 1) {
            own_bytes = node_child_bytes(source, index);
            slots = slots + (uint32_t)own_bytes;
        } else {
            child = source->children[index];
            inner = child;
            children_here = inner->child_count;
            slots = slots + children_here;
        }
        index = index + 1;
    }

    optimal = (slots + branches - 1) / branches;
    threshold = optimal + RRB_EXTRAS;
    if (count >= threshold) {
        return 0;
    }

    index = 0;
    while (index < count) {
        child = source->children[index];
        ok = check_fill_node(child, height - 1);
        if (ok == 0) {
            return 0;
        }
        index = index + 1;
    }
    return 1;
}

/* requires: as rope.h.
 * ensures:  as rope.h.
 */
int rope_check_fill(const Rope *rope)
{
    const void *root;
    uint32_t    height;
    int         ok;

    root = rope->root;
    if (root == NULL) {
        return 1;
    }
    height = rope->height;
    ok = check_fill_node(root, height);
    return ok;
}
