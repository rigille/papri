#include "command.h"

#include <stdio.h>
#include <string.h>

/* The command layer, driven the way a person drives it: a sequence of
 * command lines, then a check on what the buffer became. */

#define SCRATCH_CAPACITY 65536u

static int     failures;
static Editor  editor;
static char    scratch[SCRATCH_CAPACITY];

/* requires: `condition` is 1 when the property holds; `description` is
 *           NUL-terminated.
 * ensures:  `failures` grows by one and a line is written when it does not
 *           hold; otherwise nothing happens.
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

/* requires: editor(editor); `expected` is NUL-terminated.
 * ensures:  editor(editor); `failures` grows by one when the buffer's bytes
 *           differ from `expected`, and the difference is reported.
 */
static void expect_buffer(const char *expected, const char *where)
{
    uint32_t length;
    uint32_t total;
    size_t   wanted;
    int      ok;
    int      same;

    wanted = strlen(expected);
    total = rope_byte_count(&editor.text);
    length = (uint32_t)wanted;

    if (total != length) {
        failures = failures + 1;
        printf("  FAIL  %s: buffer is %u bytes, expected %u\n", where, total,
               length);
        return;
    }
    if (length == 0) {
        return;
    }

    ok = rope_copy_range(&editor.text, 0, length, (unsigned char *)scratch);
    if (ok == 0) {
        failures = failures + 1;
        printf("  FAIL  %s: could not read the buffer\n", where);
        return;
    }
    same = memcmp(scratch, expected, length);
    if (same != 0) {
        scratch[length] = '\0';
        failures = failures + 1;
        printf("  FAIL  %s: got \"%s\"\n", where, scratch);
    }
}

/* requires: editor(editor); `text` is NUL-terminated.
 * ensures:  editor(editor) with that command applied; the outcome is
 *           whatever the editor returned.
 */
static int run(const char *text)
{
    int ok;

    ok = editor_execute(&editor, text);
    return ok;
}

/* requires: nothing.
 * ensures:  editor(editor) holding `text`, with no name and no history.
 */
static void load_text(const char *text)
{
    Rope     built;
    uint32_t length;
    size_t   wanted;
    int      ok;

    wanted = strlen(text);
    length = (uint32_t)wanted;
    ok = rope_from_bytes(&editor.pool, (const unsigned char *)text, length,
                         &built);
    if (ok == 0) {
        failures = failures + 1;
        printf("  FAIL  could not build the starting buffer\n");
        return;
    }
    memcpy(&editor.text, &built, sizeof(Rope));
    editor.current_line = 1;
    editor.modified = 0;
}

/* requires: node_pool is live.
 * ensures:  `failures` counts the line addressing properties that did not
 *           hold.
 */
static void test_line_addresses(void)
{
    int before;
    int ok;

    before = failures;

    load_text("one\ntwo\nthree\nfour\n");

    ok = run("2d");
    expect(ok == 1, "deleting line 2 succeeds");
    expect_buffer("one\nthree\nfour\n", "2d");

    load_text("one\ntwo\nthree\nfour\n");
    ok = run("2,3d");
    expect(ok == 1, "deleting a line range succeeds");
    expect_buffer("one\nfour\n", "2,3d");

    load_text("one\ntwo\nthree\n");
    ok = run("$d");
    expect(ok == 1, "deleting the last line succeeds");
    expect_buffer("one\ntwo\n", "$d");

    load_text("one\ntwo\nthree\n");
    ok = run("%d");
    expect(ok == 1, "deleting everything succeeds");
    expect_buffer("", "%d");

    load_text("one\ntwo\n");
    ok = run("9d");
    expect(ok == 0, "an address past the end is refused");
    expect_buffer("one\ntwo\n", "9d refused");

    report("line addresses select what they name", before);
}

/* requires: node_pool is live.
 * ensures:  `failures` counts the byte addressing properties that did not
 *           hold. A buffer is bytes, so it must be addressable as bytes.
 */
static void test_byte_addresses(void)
{
    int before;
    int ok;

    before = failures;

    load_text("abcdefgh");
    ok = run("#2,#5d");
    expect(ok == 1, "deleting a byte range succeeds");
    expect_buffer("abfgh", "#2,#5d");

    load_text("abcdefgh");
    ok = run("#3c XY");
    expect(ok == 1, "changing an empty span inserts");
    expect_buffer("abcXYdefgh", "#3c");

    report("byte addresses reach inside a line", before);
}

/* requires: node_pool is live.
 * ensures:  `failures` counts the insertion properties that did not hold.
 */
static void test_insert_and_append(void)
{
    int before;
    int ok;

    before = failures;

    load_text("one\ntwo\n");
    ok = run("1i >");
    expect(ok == 1, "inserting before line 1 succeeds");
    expect_buffer(">one\ntwo\n", "1i");

    load_text("one\ntwo\n");
    ok = run("1a <");
    expect(ok == 1, "appending after line 1 succeeds");
    expect_buffer("one\n<two\n", "1a");

    load_text("one\ntwo\n");
    ok = run("1c first\\n");
    expect(ok == 1, "changing a line succeeds");
    expect_buffer("first\ntwo\n", "1c with an escape");

    report("insert and append place text where named", before);
}

/* The composition that makes `s` an ordinary change rather than a special
 * form, and the property that makes multi-match editing safe: every address
 * is resolved against one version before any edit is applied.
 *
 * requires: node_pool is live.
 * ensures:  `failures` counts the substitution properties that did not hold.
 */
static void test_substitution(void)
{
    int before;
    int ok;

    before = failures;

    load_text("the cat sat on the mat\n");
    ok = run("%s/at/og/");
    expect(ok == 1, "substituting across the buffer succeeds");
    expect_buffer("the cog sog on the mog\n", "%s/at/og/");

    /* The replacement is longer than the pattern, so every match after the
     * first sits at a stale offset unless all of them were resolved up
     * front. This is the test that would fail under mutable buffers. */
    load_text("a-a-a-a\n");
    ok = run("%s/a/LONGER/");
    expect(ok == 1, "a lengthening substitution succeeds");
    expect_buffer("LONGER-LONGER-LONGER-LONGER\n", "lengthening substitution");

    /* And shorter, which invalidates offsets the other way. */
    load_text("xxAyyAzzA\n");
    ok = run("%s/A//");
    expect(ok == 1, "a deleting substitution succeeds");
    expect_buffer("xxyyzz\n", "shortening substitution");

    load_text("one\ntwo\nthree\n");
    ok = run("2s/two/2/");
    expect(ok == 1, "substitution inside one line succeeds");
    expect_buffer("one\n2\nthree\n", "2s/two/2/");

    load_text("one\ntwo\n");
    ok = run("%s/zzz/x/");
    expect(ok == 0, "a pattern that does not occur is refused");
    expect_buffer("one\ntwo\n", "no match leaves the buffer alone");

    report("substitution composes addressing with matching", before);
}

/* requires: node_pool is live.
 * ensures:  `failures` counts the pattern addressing properties that did not
 *           hold.
 */
static void test_pattern_addresses(void)
{
    int before;
    int ok;

    before = failures;

    load_text("keep\ndrop me\nkeep\ndrop me too\n");
    ok = run("g/drop/d");
    expect(ok == 1, "deleting every matching line succeeds");
    expect_buffer("keep\nkeep\n", "g/drop/d");

    load_text("aXbXc");
    ok = run("/X/d");
    expect(ok == 1, "deleting every match succeeds");
    expect_buffer("abc", "/X/d");

    load_text("one two one\n");
    ok = run("/one/c1");
    expect(ok == 1, "changing every match succeeds");
    expect_buffer("1 two 1\n", "/one/c1");

    report("pattern addresses select matches and their lines", before);
}

/* requires: node_pool is live.
 * ensures:  `failures` counts the ways editing disturbed an older version.
 *           This is the property the whole design exists for.
 */
static void test_old_versions_survive_edits(void)
{
    Rope     original;
    uint32_t length;
    uint32_t after;
    int      before;
    int      ok;
    int      same;

    before = failures;

    load_text("the original text\n");
    memcpy(&original, &editor.text, sizeof(Rope));
    length = rope_byte_count(&original);

    ok = run("%c something else entirely\\n");
    expect(ok == 1, "replacing the whole buffer succeeds");

    after = rope_byte_count(&original);
    expect(after == length, "the old version still has its length");

    ok = rope_copy_range(&original, 0, length, (unsigned char *)scratch);
    expect(ok == 1, "the old version can still be read");
    same = memcmp(scratch, "the original text\n", length);
    expect(same == 0, "the old version still holds its own bytes");

    ok = rope_check_invariants(&original);
    expect(ok == 1, "the old version is still structurally sound");

    report("an edit leaves every older version intact", before);
}

/* requires: standard output is writable.
 * ensures:  every test above has run and reported; the result is 0 when
 *           `failures` is 0 and 1 otherwise.
 */
int main(void)
{
    int ok;

    failures = 0;
    ok = editor_initialize(&editor);
    if (ok == 0) {
        printf("could not initialize the editor\n");
        return 1;
    }

    test_line_addresses();
    test_byte_addresses();
    test_insert_and_append();
    test_substitution();
    test_pattern_addresses();
    test_old_versions_survive_edits();

    editor_release(&editor);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
