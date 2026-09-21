#ifndef PAPRI_H
#define PAPRI_H

/* papri — core module.
 *
 * PLACEHOLDER. `ByteRing` is here to carry the conventions in CLAUDE.md, not
 * because papri needs a byte ring: it is the smallest thing with real
 * ownership, a real invariant, and a spec worth writing. Replace it with the
 * actual core module and keep the shape — predicates block, then one
 * requires/ensures per declaration.
 */

#include <stddef.h>

#define BYTE_RING_CAPACITY 64

/* ── Abstract predicates ────────────────────────────────────────────────────
 * byte_ring(ring, contents)
 *   Owns *ring. `contents` is the sequence of buffered bytes, oldest first,
 *   with length <= BYTE_RING_CAPACITY. The storage outside that window holds
 *   unconstrained bytes and is never read.
 */

typedef struct ByteRing {
    unsigned char storage[BYTE_RING_CAPACITY];
    unsigned int  head;
    unsigned int  length;
} ByteRing;

/* requires: *ring is allocated and writable.
 * ensures:  byte_ring(ring, contents') where contents' is the empty sequence.
 */
void byte_ring_initialize(ByteRing *ring);

/* requires: byte_ring(ring, contents).
 * ensures:  byte_ring(ring, contents'). If contents has length
 *           BYTE_RING_CAPACITY then contents' is contents and the result is
 *           0; otherwise contents' is contents with `value` appended at the
 *           newest end and the result is 1.
 */
int byte_ring_push(ByteRing *ring, unsigned char value);

/* requires: byte_ring(ring, contents); *value is writable.
 * ensures:  byte_ring(ring, contents'). If contents is empty then contents'
 *           is contents, *value is unchanged, and the result is 0; otherwise
 *           contents' is contents without its oldest byte, *value is that
 *           byte, and the result is 1.
 */
int byte_ring_pop(ByteRing *ring, unsigned char *value);

/* requires: byte_ring(ring, contents).
 * ensures:  byte_ring(ring, contents); the result is the length of contents.
 *           No memory is written.
 */
unsigned int byte_ring_length(const ByteRing *ring);

/* requires: byte_ring(source, contents); *destination is allocated and
 *           writable, and does not overlap *source.
 * ensures:  byte_ring(source, contents) and byte_ring(destination, contents)
 *           — the two rings buffer equal sequences and are thereafter
 *           independent.
 */
void byte_ring_copy(ByteRing *destination, const ByteRing *source);

#endif /* PAPRI_H */
