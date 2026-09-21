#include "papri.h"

#include <string.h>

/* Every condition below tests a plain local, never a load: the subset admits
 * only pure operands in `if`/`while`, so each field is read into its own
 * local first. See CLAUDE.md § Verifiable C subset.
 */

/* requires: *ring is allocated and writable.
 * ensures:  byte_ring(ring, contents') where contents' is the empty sequence.
 */
void byte_ring_initialize(ByteRing *ring)
{
    ring->head = 0;
    ring->length = 0;
}

/* requires: byte_ring(ring, contents).
 * ensures:  byte_ring(ring, contents'). If contents has length
 *           BYTE_RING_CAPACITY then contents' is contents and the result is
 *           0; otherwise contents' is contents with `value` appended at the
 *           newest end and the result is 1.
 */
int byte_ring_push(ByteRing *ring, unsigned char value)
{
    unsigned int length;
    unsigned int head;
    unsigned int index;

    length = ring->length;
    if (length == BYTE_RING_CAPACITY) {
        return 0;
    }

    head = ring->head;
    index = head + length;
    if (index >= BYTE_RING_CAPACITY) {
        index = index - BYTE_RING_CAPACITY;
    }

    ring->storage[index] = value;
    length = length + 1;
    ring->length = length;
    return 1;
}

/* requires: byte_ring(ring, contents); *value is writable.
 * ensures:  byte_ring(ring, contents'). If contents is empty then contents'
 *           is contents, *value is unchanged, and the result is 0; otherwise
 *           contents' is contents without its oldest byte, *value is that
 *           byte, and the result is 1.
 */
int byte_ring_pop(ByteRing *ring, unsigned char *value)
{
    unsigned int length;
    unsigned int head;
    unsigned char oldest;

    length = ring->length;
    if (length == 0) {
        return 0;
    }

    head = ring->head;
    oldest = ring->storage[head];
    *value = oldest;

    head = head + 1;
    if (head == BYTE_RING_CAPACITY) {
        head = 0;
    }
    ring->head = head;

    length = length - 1;
    ring->length = length;
    return 1;
}

/* requires: byte_ring(ring, contents).
 * ensures:  byte_ring(ring, contents); the result is the length of contents.
 *           No memory is written.
 */
unsigned int byte_ring_length(const ByteRing *ring)
{
    unsigned int length;

    length = ring->length;
    return length;
}

/* requires: byte_ring(source, contents); *destination is allocated and
 *           writable, and does not overlap *source.
 * ensures:  byte_ring(source, contents) and byte_ring(destination, contents)
 *           — the two rings buffer equal sequences and are thereafter
 *           independent.
 */
void byte_ring_copy(ByteRing *destination, const ByteRing *source)
{
    /* An explicit memcpy, never `*destination = *source;`. */
    memcpy(destination, source, sizeof(ByteRing));
}
