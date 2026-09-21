#include "papri.h"

#include <stdio.h>

/* A file-scope ring rather than a local. The subset prefers nonaddressable
 * locals, and a struct can only cross a function boundary by pointer, so a
 * local ring would force `&ring` for no reason. */
static ByteRing ring;

/* requires: standard output is writable.
 * ensures:  the bytes 1..5 are pushed, then drained oldest first and written
 *           to standard output as decimal numbers on one line; the result is
 *           0, or 1 if a push was refused.
 */
int main(void)
{
    /* The only addressable local: byte_ring_pop needs an out-parameter, so
     * its address is unavoidable. Taking it costs more than it looks — the
     * variable moves to memory, so every mention of it becomes a load, and
     * a load may not appear inside a call argument or a condition. Hence
     * `number` below, which exists only to hold the loaded byte. */
    unsigned char popped_value;

    unsigned char value;
    unsigned int  index;
    unsigned int  number;
    int pushed;
    int popped;

    byte_ring_initialize(&ring);

    index = 0;
    while (index < 5) {
        value = (unsigned char)(index + 1);
        pushed = byte_ring_push(&ring, value);
        if (pushed == 0) {
            return 1;
        }
        index = index + 1;
    }

    number = byte_ring_length(&ring);
    printf("buffered %u byte(s):", number);

    popped = byte_ring_pop(&ring, &popped_value);
    while (popped == 1) {
        number = popped_value;
        printf(" %u", number);
        popped = byte_ring_pop(&ring, &popped_value);
    }

    printf("\n");
    return 0;
}
