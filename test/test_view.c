#include "pool.h"
#include "rope.h"
#include "view.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Views render a range of the buffer. The one that matters most here is the
 * code point view, because it is where the two halves of the design have to
 * agree: leaves are exactly one cache line, so a multi-byte code point
 * routinely straddles a leaf boundary, and the decoder must not care. It
 * does not, because it is a DFA — the whole resumption state is (state,
 * codepoint).
 */

#define CAPTURE_PATH "build/test_view_out.txt"
#define CAPTURE_CAPACITY 65536u

static int  failures;
static Pool pool;
static char captured[CAPTURE_CAPACITY];
static int  saved_output;

/* requires: `condition` is 1 when the property holds.
 * ensures:  `failures` grows by one and a line is written when it does not.
 */
static void expect(int condition, const char *description)
{
    if (condition == 0) {
        failures = failures + 1;
        printf("  FAIL  %s\n", description);
    }
}

/* requires: nothing.
 * ensures:  a line naming the outcome of a whole test is written.
 */
static void report(const char *name, int before)
{
    int now;

    now = failures;
    if (now == before) {
        printf("  ok    %s\n", name);
        return;
    }
    printf("  FAIL  %s\n", name);
}

/* Views write to standard output, so to check one we have to catch it.
 *
 * requires: standard output is not already captured.
 * ensures:  standard output is redirected to CAPTURE_PATH and the result is
 *           1; or it could not be redirected and the result is 0.
 */
static int capture_begin(void)
{
    int target;

    fflush(stdout);
    saved_output = dup(1);
    if (saved_output < 0) {
        return 0;
    }
    target = open(CAPTURE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (target < 0) {
        return 0;
    }
    dup2(target, 1);
    close(target);
    return 1;
}

/* requires: capture_begin succeeded.
 * ensures:  standard output is restored and `captured` holds what was
 *           written, NUL-terminated.
 */
static void capture_end(void)
{
    FILE  *file;
    size_t length;

    fflush(stdout);
    dup2(saved_output, 1);
    close(saved_output);

    captured[0] = '\0';
    file = fopen(CAPTURE_PATH, "rb");
    if (file == NULL) {
        return;
    }
    length = fread(captured, 1, CAPTURE_CAPACITY - 1, file);
    captured[length] = '\0';
    fclose(file);
}

/* requires: `needle` is NUL-terminated.
 * ensures:  the result is 1 when the captured output contains it.
 */
static int captured_has(const char *needle)
{
    const char *found;

    found = strstr(captured, needle);
    if (found == NULL) {
        return 0;
    }
    return 1;
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the hex view properties that did not hold.
 */
static void test_hex_view(void)
{
    static const unsigned char source[] = {
        0x00, 0x01, 0x02, 0xFF, 0xFE, 0x20, 0x41, 0x42
    };
    Rope rope;
    int  before;
    int  ok;

    before = failures;

    ok = rope_from_bytes(&pool, source, 8, &rope);
    expect(ok == 1, "the binary rope is built");

    ok = capture_begin();
    expect(ok == 1, "output can be captured");
    view_write_hex(&rope, 0, 8);
    capture_end();

    expect(captured_has("00000000"), "the line carries its byte offset");
    expect(captured_has("00 01 02 ff fe 20 41 42"), "the bytes are in hex");
    expect(captured_has("|..... AB|"),
           "unprintable bytes show as dots beside the hex");

    report("the hex view renders bytes that are not text", before);
}

/* The decisive one: a code point that does not fit in the leaf it starts
 * in. Leaves are ROPE_LEAF_BYTES, so this is the ordinary case, not a
 * corner one.
 *
 * requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the ways decoding failed across a boundary.
 */
static void test_codepoint_across_a_leaf_boundary(void)
{
    unsigned char source[200];
    Rope          rope;
    size_t        index;
    size_t        length;
    int           before;
    int           ok;

    before = failures;

    /* Fill so that U+20AC (E2 82 AC) begins at offset 63 — one byte before
     * the end of the first leaf, so its continuation bytes land in the
     * next one. */
    index = 0;
    while (index < 63) {
        source[index] = 0x61;
        index = index + 1;
    }
    source[63] = 0xE2;
    source[64] = 0x82;
    source[65] = 0xAC;
    source[66] = 0x0A;
    length = 67;

    ok = rope_from_bytes(&pool, source, length, &rope);
    expect(ok == 1, "the straddling rope is built");

    /* Confirm the premise rather than assuming it: the code point really
     * does cross a leaf boundary. */
    expect(length > ROPE_LEAF_BYTES, "the buffer spans more than one leaf");

    ok = capture_begin();
    expect(ok == 1, "output can be captured");
    view_write_codepoints(&rope, 0, length);
    capture_end();

    expect(captured_has("63\tU+20AC\t3 byte(s)"),
           "the straddling code point decodes, at the right offset");
    expect(captured_has("66\tU+000A"), "decoding continues past it");

    report("a code point straddling a leaf boundary still decodes", before);
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the ways invalid bytes were mishandled. A
 *           buffer is bytes, so a text view must survive bytes that are not
 *           text.
 */
static void test_invalid_utf8_is_reported(void)
{
    static const unsigned char source[] = { 0x41, 0xFF, 0x42, 0x0A };
    Rope rope;
    int  before;
    int  ok;

    before = failures;

    ok = rope_from_bytes(&pool, source, 4, &rope);
    expect(ok == 1, "the rope with an invalid byte is built");

    ok = capture_begin();
    expect(ok == 1, "output can be captured");
    view_write_codepoints(&rope, 0, 4);
    capture_end();

    expect(captured_has("0\tU+0041"), "the valid byte before decodes");
    expect(captured_has("1\tinvalid byte 0xff"), "the invalid byte is named");
    expect(captured_has("2\tU+0042"), "decoding resumes after it");

    report("invalid bytes are reported, not fatal", before);
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the text view properties that did not hold.
 */
static void test_text_view(void)
{
    static const unsigned char source[] = "alpha\nbeta\n";
    Rope rope;
    int  before;
    int  ok;

    before = failures;

    ok = rope_from_bytes(&pool, source, 11, &rope);
    expect(ok == 1, "the text rope is built");

    ok = capture_begin();
    expect(ok == 1, "output can be captured");
    view_write_text(&rope, 6, 11);
    capture_end();

    ok = strcmp(captured, "beta\n");
    expect(ok == 0, "the text view writes exactly the range asked for");

    report("the text view writes its range verbatim", before);
}

/* requires: standard output is writable.
 * ensures:  every test above has run and reported; the result is 0 when
 *           `failures` is 0 and 1 otherwise.
 */
int main(void)
{
    int ok;

    failures = 0;
    ok = pool_initialize(&pool);
    if (ok == 0) {
        printf("could not initialize the pool\n");
        return 1;
    }

    test_hex_view();
    test_codepoint_across_a_leaf_boundary();
    test_invalid_utf8_is_reported();
    test_text_view();

    pool_release(&pool);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
