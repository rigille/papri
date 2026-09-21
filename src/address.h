#ifndef PAPRI_ADDRESS_H
#define PAPRI_ADDRESS_H

#include <stdint.h>

#include "rope.h"

/* Addresses, as defunctionalized optics.
 *
 * An address is not a lens. A lens whose setter changes the focus's length
 * breaks the set-set law: with a fixed span [i, j), setting a longer value
 * and then setting again replaces the wrong region. The lawful thing is an
 * isomorphism into a decomposition,
 *
 *     Buffer  ≅  (gap0, focus1, gap1, …, focusN, gapN)
 *
 * which is exactly what split and concat implement, and which is lawful
 * under arbitrary length change. Resolving an address yields the foci; the
 * gaps are whatever lies between them.
 *
 * They are data, not function pointers: no higher-kinded types to encode,
 * nothing for VST to choke on, and an address that is ordinary data can be
 * printed, stored and inspected. One interpreter walks the form.
 *
 * Every address is resolved against ONE immutable version before any edit is
 * applied. That is what makes the offset-invalidation bug of multi-match
 * substitution — edit match 1, and match 2's coordinates are stale —
 * structurally impossible rather than merely avoided.
 */

#define DECOMPOSITION_CAPACITY 4096

typedef enum AddressKind {
    ADDRESS_ALL,            /* the whole buffer, as one focus */
    ADDRESS_CURRENT,        /* the current line */
    ADDRESS_LAST,           /* the last line */
    ADDRESS_LINE,           /* line `first`, counting from 1 */
    ADDRESS_LINE_RANGE,     /* lines `first` through `last` */
    ADDRESS_BYTE,           /* the empty span at byte `first` */
    ADDRESS_BYTE_RANGE,     /* bytes [first, last) */
    ADDRESS_MATCH,          /* every occurrence of the pattern */
    ADDRESS_LINES_MATCHING  /* every line containing the pattern */
} AddressKind;

/* M2 matches literal bytes. A regex engine is later work; when it lands it
 * becomes two more AddressKinds and the rest of this file does not move. */
typedef struct Address {
    AddressKind          kind;
    uint32_t             first;
    uint32_t             last;
    const unsigned char *pattern;
    uint32_t             pattern_length;
} Address;

typedef struct Span {
    uint32_t start;
    uint32_t end;
} Span;

typedef struct Decomposition {
    Span     focus[DECOMPOSITION_CAPACITY];
    uint32_t count;
} Decomposition;

/* ── Abstract predicates ────────────────────────────────────────────────────
 * address(address, form)
 *   *address is a pure value naming `form`. When the form carries a pattern,
 *   holds a read share of its bytes, which must outlive the address.
 *
 * decomposition(decomposition, spans, bytes)
 *   *decomposition is a pure value: `spans` is a sequence of byte ranges of
 *   `bytes`, pairwise disjoint, in ascending order, none extending past
 *   |bytes|. The gaps between them are implied, so the pair (spans, bytes)
 *   determines the isomorphism above.
 */

/* requires: rope(rope, bytes, share); address(address, form); current_line
 *           counts from 1 and names the current line; *result writable.
 * ensures:  rope(rope, bytes, share) and the address are returned;
 *           decomposition(result, spans, bytes) where spans are the foci the
 *           form selects, and the outcome is 1; or the form named something
 *           outside the buffer, or selected more than
 *           DECOMPOSITION_CAPACITY foci, and the outcome is 0.
 */
int address_resolve(const Rope *rope, const Address *address,
                    uint32_t current_line, Decomposition *result);

/* requires: rope(rope, bytes, share); holds a read share of
 *           `pattern_length` bytes at `pattern`, which is non-empty;
 *           *found writable.
 * ensures:  rope(rope, bytes, share) and the read share are returned. When
 *           the pattern occurs at or after `from`, *found is the offset of
 *           the earliest such occurrence and the result is 1; otherwise
 *           *found is unchanged and the result is 0.
 */
int address_find_literal(const Rope *rope, uint32_t from,
                         const unsigned char *pattern, uint32_t pattern_length,
                         uint32_t *found);

/* A version must be a TREE, not a DAG: no node may appear twice within one
 * version. The reclamation walk frees each node it reaches exactly once, so
 * a node reached twice would be freed twice. That is why the replacement is
 * rebuilt at every focus below rather than one rope being spliced into all
 * of them. Sharing BETWEEN versions is the whole design; sharing WITHIN one
 * is a bug.
 */

/* The isomorphism, put back together with a new focus everywhere.
 *
 * Built left to right as gap ++ replacement ++ gap ++ replacement ++ … so no
 * offset ever has to be adjusted for an edit made earlier in the same pass.
 *
 * requires: node_pool(pool, live, residual); rope(rope, bytes, share);
 *           decomposition(decomposition, spans, bytes); holds a read share
 *           of `replacement_length` bytes at `replacement`, which may be
 *           null when that length is zero; *result writable and not *rope.
 * ensures:  every input share is returned; rope(result, bytes', share')
 *           where bytes' is bytes with every span replaced by the
 *           replacement, and the result is 1; or allocation failed and the
 *           result is 0.
 */
int address_replace_all(Pool *pool, const Rope *rope,
                        const Decomposition *decomposition,
                        const unsigned char *replacement,
                        uint32_t replacement_length,
                        Rope *result);

/* requires: node_pool(pool, live, residual); rope(rope, bytes, share);
 *           decomposition(decomposition, spans, bytes); `before` says
 *           whether to insert before each focus rather than after it; holds
 *           a read share of the insertion; *result writable.
 * ensures:  every input share is returned; rope(result, bytes', share')
 *           where bytes' is bytes with the insertion placed at the start of
 *           each span when `before`, and at its end otherwise, and the
 *           result is 1; or allocation failed and the result is 0.
 */
int address_insert_all(Pool *pool, const Rope *rope,
                       const Decomposition *decomposition, int before,
                       const unsigned char *insertion,
                       uint32_t insertion_length,
                       Rope *result);

#endif /* PAPRI_ADDRESS_H */
