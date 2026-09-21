#include "papri.h"

#include <stdio.h>

/* No test framework: a framework is a dependency, and the whole point of the
 * subset is that there is nothing in the build we cannot read. */

static int failures;
static ByteRing ring;
static ByteRing duplicate;

/* requires: `condition` is 1 when the property under test holds and 0 when it
 *           does not; `description` is a NUL-terminated string.
 * ensures:  `failures` is unchanged when condition is 1 and one greater when
 *           it is 0; one line naming the outcome is written to standard
 *           output.
 */
static void expect(int condition, const char *description)
{
    if (condition == 0) {
        failures = failures + 1;
        printf("  FAIL  %s\n", description);
        return;
    }
    printf("  ok    %s\n", description);
}

/* requires: nothing.
 * ensures:  `failures` counts the properties of byte_ring_push and
 *           byte_ring_pop that did not hold; the outcomes are reported.
 */
static void test_push_then_pop_returns_oldest_first(void)
{
    unsigned char value;
    unsigned int length;
    int result;

    byte_ring_initialize(&ring);

    result = byte_ring_push(&ring, 10);
    expect(result == 1, "push onto an empty ring succeeds");
    result = byte_ring_push(&ring, 20);
    expect(result == 1, "push onto a nonempty ring succeeds");

    length = byte_ring_length(&ring);
    expect(length == 2, "length counts the buffered bytes");

    value = 0;
    result = byte_ring_pop(&ring, &value);
    expect(result == 1, "pop from a nonempty ring succeeds");
    expect(value == 10, "pop returns the oldest byte first");

    value = 0;
    result = byte_ring_pop(&ring, &value);
    expect(value == 20, "pop then returns the next-oldest byte");

    length = byte_ring_length(&ring);
    expect(length == 0, "draining the ring empties it");
}

/* requires: nothing.
 * ensures:  `failures` counts the boundary properties that did not hold; the
 *           outcomes are reported.
 */
static void test_boundaries(void)
{
    unsigned char value;
    unsigned int index;
    int result;

    byte_ring_initialize(&ring);

    value = 0;
    result = byte_ring_pop(&ring, &value);
    expect(result == 0, "pop from an empty ring is refused");
    expect(value == 0, "a refused pop leaves the out-parameter alone");

    index = 0;
    while (index < BYTE_RING_CAPACITY) {
        value = (unsigned char)index;
        result = byte_ring_push(&ring, value);
        if (result == 0) {
            break;
        }
        index = index + 1;
    }
    expect(index == BYTE_RING_CAPACITY, "the ring accepts exactly its capacity");

    result = byte_ring_push(&ring, 99);
    expect(result == 0, "push onto a full ring is refused");
}

/* requires: nothing.
 * ensures:  `failures` counts the properties of byte_ring_copy that did not
 *           hold; the outcomes are reported.
 */
static void test_copy_is_independent(void)
{
    unsigned char value;
    unsigned int length;
    int result;

    byte_ring_initialize(&ring);
    result = byte_ring_push(&ring, 7);
    expect(result == 1, "the source ring accepts a byte");

    byte_ring_copy(&duplicate, &ring);

    value = 0;
    result = byte_ring_pop(&duplicate, &value);
    expect(value == 7, "the copy buffers the same sequence");

    length = byte_ring_length(&ring);
    expect(length == 1, "draining the copy leaves the source alone");
}

/* requires: standard output is writable.
 * ensures:  every test above has run and reported; the result is 0 when
 *           `failures` is 0 and 1 otherwise.
 */
int main(void)
{
    failures = 0;

    test_push_then_pop_returns_oldest_first();
    test_boundaries();
    test_copy_is_independent();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }

    printf("%d test(s) failed\n", failures);
    return 1;
}
