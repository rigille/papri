#include "utf8.h"

#include <stdint.h>
#include <stdio.h>

/* No test framework: a framework is a dependency, and the whole point of the
 * subset is that there is nothing in the build we cannot read. */

static int failures;

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
 * ensures:  `failures` counts the properties of count_codepoints that did not
 *           hold; the outcomes are reported.
 */
static void test_count_codepoints(void)
{
    static uint8_t ascii[3] = { 0x61, 0x62, 0x63 };
    static uint8_t accented[2] = { 0xC3, 0xA9 };            /* U+00E9 */
    static uint8_t euro[3] = { 0xE2, 0x82, 0xAC };          /* U+20AC */
    static uint8_t emoji[4] = { 0xF0, 0x9F, 0x92, 0xA9 };   /* U+1F4A9 */
    static uint8_t invalid[2] = { 0xFF, 0x61 };
    static uint8_t truncated[3] = { 0x61, 0xE2, 0x82 };
    uint64_t count;

    count = count_codepoints(ascii, 0);
    expect(count == 0, "an empty buffer holds no code points");

    count = count_codepoints(ascii, 3);
    expect(count == 3, "three ASCII bytes are three code points");

    count = count_codepoints(accented, 2);
    expect(count == 1, "a two-byte sequence is one code point");

    count = count_codepoints(euro, 3);
    expect(count == 1, "a three-byte sequence is one code point");

    count = count_codepoints(emoji, 4);
    expect(count == 1, "a four-byte sequence is one code point");

    count = count_codepoints(invalid, 2);
    expect(count == 0, "an invalid lead byte ends the count immediately");

    count = count_codepoints(truncated, 3);
    expect(count == 1, "a truncated tail counts only the complete prefix");
}

/* The property M1 depends on: a code point split across two chunks decodes
 * without copying the bytes into one contiguous buffer, because (state,
 * codepoint) is the entire resumption state.
 *
 * requires: nothing.
 * ensures:  `failures` counts the resumption properties that did not hold;
 *           the outcomes are reported.
 */
static void test_next_state_resumes_across_a_chunk_boundary(void)
{
    uint32_t state;
    uint32_t codepoint;

    state = UTF8_ACCEPT;
    codepoint = 0;

    /* First chunk ends here, after one byte of U+20AC (E2 82 AC). */
    state = next_state(state, 0xE2, &codepoint);
    expect(state != UTF8_ACCEPT, "a lead byte leaves the decoder mid-sequence");
    expect(state != UTF8_REJECT, "a valid lead byte is not a rejection");

    /* Second chunk. Nothing crossed the boundary but state and codepoint. */
    state = next_state(state, 0x82, &codepoint);
    expect(state != UTF8_REJECT, "a valid continuation byte is accepted");

    state = next_state(state, 0xAC, &codepoint);
    expect(state == UTF8_ACCEPT, "the final byte completes the code point");
    expect(codepoint == 0x20AC, "the code point survives the chunk boundary");
}

/* requires: nothing.
 * ensures:  `failures` counts the rejection properties that did not hold; the
 *           outcomes are reported.
 */
static void test_next_state_rejects_bad_input(void)
{
    uint32_t state;
    uint32_t codepoint;

    state = UTF8_ACCEPT;
    codepoint = 0;

    /* A continuation byte with no lead byte before it. */
    state = next_state(state, 0x82, &codepoint);
    expect(state == UTF8_REJECT, "a stray continuation byte is rejected");

    state = UTF8_ACCEPT;
    codepoint = 0;
    state = next_state(state, 0xE2, &codepoint);
    state = next_state(state, 0x41, &codepoint);
    expect(state == UTF8_REJECT, "an ASCII byte cannot continue a sequence");
}

/* requires: standard output is writable.
 * ensures:  every test above has run and reported; the result is 0 when
 *           `failures` is 0 and 1 otherwise.
 */
int main(void)
{
    failures = 0;

    test_count_codepoints();
    test_next_state_resumes_across_a_chunk_boundary();
    test_next_state_rejects_bad_input();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }

    printf("%d test(s) failed\n", failures);
    return 1;
}
