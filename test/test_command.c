#include "command.h"
#include "line_index.h"

#include <stdio.h>
#include <string.h>

/* The command layer, driven the way a person drives it: a sequence of
 * command lines, then a check on what the buffer became. */

#define SCRATCH_CAPACITY 262144u

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
    size_t   length;
    size_t   total;
    size_t   wanted;
    int      ok;
    int      same;

    wanted = strlen(expected);
    total = rope_byte_count(&editor.text);
    length = (uint32_t)wanted;

    if (total != length) {
        failures = failures + 1;
        printf("  FAIL  %s: buffer is %zu bytes, expected %zu\n", where, total,
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
    size_t   length;
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
    /* The index is part of the buffer now, so a helper that installs one
     * has to install the other. */
    ok = line_index_build(&editor.pool, &editor.text, &editor.index);
    if (ok == 0) {
        failures = failures + 1;
        printf("  FAIL  could not build the starting index\n");
        return;
    }
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

/* `\1` in a replacement stands for the text being replaced. It is not a
 * byte, so the interesting cases are the ones where treating it as a byte
 * would go wrong: naming it twice, naming it in a verb that is not `s`, and
 * a literal backslash-one that must still mean a literal backslash-one.
 *
 * requires: node_pool is live.
 * ensures:  `failures` counts the quoting properties that did not hold.
 */
static void test_replacement_quotes_the_match(void)
{
    int before;
    int ok;

    before = failures;

    load_text("the cat sat on the mat\n");
    ok = run("%s/at/[\\1]/");
    expect(ok == 1, "quoting the match succeeds");
    expect_buffer("the c[at] s[at] on the m[at]\n", "%s/at/[\\1]/");

    /* Twice in one replacement. Each quotation must be its own copy; one
     * subtree reached by two paths would make the version a DAG. */
    load_text("ab\n");
    ok = run("%s/a/<\\1|\\1>/");
    expect(ok == 1, "quoting the match twice succeeds");
    expect_buffer("<a|a>b\n", "two quotations");

    /* `c` replaces the focus, so `\1` there is the focus. */
    load_text("one\ntwo\n");
    ok = run("1c[\\1]");
    expect(ok == 1, "quoting the focus in c succeeds");
    expect_buffer("[one\n]two\n", "1c[\\1]");

    /* A quotation at either end leaves an empty run beside it. */
    load_text("xay\n");
    ok = run("%s/a/\\1\\1/");
    expect(ok == 1, "a replacement that is nothing but quotations succeeds");
    expect_buffer("xaay\n", "empty runs at both ends");

    /* `\\` resolves to one backslash, so `\\1` is a literal backslash-one
     * and must not be read as a quotation. */
    load_text("cat\n");
    ok = run("%s/cat/a\\\\1b/");
    expect(ok == 1, "an escaped backslash before a 1 succeeds");
    expect_buffer("a\\1b\n", "literal backslash-one");

    /* The whole buffer as the focus, quoted: the copy path has to handle a
     * span longer than one leaf. */
    load_text("0123456789abcdef0123456789abcdef");
    ok = run("%c<\\1>");
    expect(ok == 1, "quoting the whole buffer succeeds");
    expect_buffer("<0123456789abcdef0123456789abcdef>", "whole buffer quoted");

    report("a replacement can quote the text it replaces", before);
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
    size_t   length;
    size_t   after;
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

/* requires: node_pool is live.
 * ensures:  `failures` counts the ways buffers leaked into one another.
 *           Seeing several files at once is the point, so they must not.
 */
static void test_buffers_are_independent(void)
{
    int before;
    int ok;

    before = failures;

    load_text("first buffer\n");
    expect_buffer("first buffer\n", "buffer 0 starts out right");

    ok = run("b1");
    expect(ok == 1, "switching to an unused buffer succeeds");
    expect_buffer("", "an unused buffer starts empty");

    load_text("second buffer\n");
    expect_buffer("second buffer\n", "buffer 1 holds its own text");

    ok = run("b0");
    expect(ok == 1, "switching back succeeds");
    expect_buffer("first buffer\n", "buffer 0 kept its text");

    /* @N runs a command against another buffer and returns. */
    ok = run("@1 1c changed\\n");
    expect(ok == 1, "a command against another buffer succeeds");
    expect_buffer("first buffer\n",
                  "the current buffer is untouched by @");

    ok = run("b1");
    expect(ok == 1, "switching to the edited buffer succeeds");
    expect_buffer("changed\n", "@ edited the buffer it named");

    ok = run("b99");
    expect(ok == 0, "a buffer past the end is refused");

    report("buffers hold their own text and do not leak", before);
}

/* The bug this exists for: an edit through the editor corrupted buffers
 * over about ten kilobytes, and every existing test missed it. The rope's
 * model test drives rope_replace_span, not the editor's path through
 * address_replace_all; the command tests drive that path but only on
 * buffers of a few dozen bytes. The hole was the intersection.
 *
 * requires: node_pool is live.
 * ensures:  `failures` counts the bytes an edit to a large buffer got wrong.
 */
static void test_large_buffer_edits_keep_their_bytes(void)
{
    static char source[120000];
    static char expected[120000];
    size_t      length;
    size_t      index;
    size_t      count;
    int         before;
    int         ok;
    int         same;

    before = failures;

    length = 0;
    index = 1;
    while (length < 100000) {
        length = length + (size_t)snprintf(source + length,
                                           sizeof(source) - length,
                                           "%zu\n", index);
        index = index + 1;
    }
    memcpy(expected, source, length);

    load_text(source);

    ok = run("%s/4242/XXXX/");
    expect(ok == 1, "a substitution in a large buffer succeeds");

    count = rope_byte_count(&editor.text);
    expect(count == length, "the buffer keeps its length");

    ok = rope_copy_range(&editor.text, 0, count,
                         (unsigned char *)scratch);
    expect(ok == 1, "the whole buffer can be read back");

    /* The same edit in the model. `%s` replaces EVERY occurrence, not
     * the first, and in a buffer of ascending numbers "4242" occurs
     * inside 14242 and 42420 as well as on its own line. */
    index = 0;
    while (index + 4 <= length) {
        same = memcmp(expected + index, "4242", 4);
        if (same == 0) {
            memcpy(expected + index, "XXXX", 4);
            index = index + 4;
        } else {
            index = index + 1;
        }
    }
    same = memcmp(scratch, expected, length);
    if (same != 0) {
        index = 0;
        while (index < length) {
            if (scratch[index] != expected[index]) {
                break;
            }
            index = index + 1;
        }
        failures = failures + 1;
        printf("  FAIL  large buffer differs from byte %zu\n", index);
        printf("  ...   got  %.16s\n", scratch + index);
        printf("  ...   want %.16s\n", expected + index);
    }

    report("edits to a large buffer keep every byte", before);
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
    test_replacement_quotes_the_match();
    test_pattern_addresses();
    test_old_versions_survive_edits();
    test_buffers_are_independent();
    test_large_buffer_edits_keep_their_bytes();

    editor_release(&editor);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
