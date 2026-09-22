#include "pool.h"
#include "rope.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The rope is checked against a naive byte array doing the same edits, in the
 * spirit of ciska's sparse-versus-dense equivalence gate: the model is
 * obviously correct and slow, the real thing is fast and subtle, and they
 * must agree after every single operation. */

#define MODEL_CAPACITY 65536u

static int      failures;
static Pool     pool;
static uint8_t  model[MODEL_CAPACITY];
static size_t   model_length;
static uint8_t  scratch[MODEL_CAPACITY];
static uint64_t random_state;

/* requires: `condition` is 1 when the property under test holds and 0 when it
 *           does not; `description` is a NUL-terminated string.
 * ensures:  `failures` is unchanged when condition is 1 and one greater when
 *           it is 0; a line naming the outcome is written when it fails, and
 *           nothing is written when it passes.
 */
static void expect(int condition, const char *description)
{
    if (condition == 0) {
        failures = failures + 1;
        printf("  FAIL  %s\n", description);
    }
}

/* requires: nothing.
 * ensures:  a line naming the outcome of a whole test is written; `failures`
 *           is unchanged.
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

/* requires: nothing.
 * ensures:  `random_state` advances and the result is the next value of an
 *           xorshift64 sequence; the sequence is fixed by the seed, so a
 *           failure reproduces.
 */
static uint64_t next_random(void)
{
    uint64_t value;

    value = random_state;
    value = value ^ (value << 13);
    value = value ^ (value >> 7);
    value = value ^ (value << 17);
    random_state = value;
    return value;
}

/* requires: bound > 0.
 * ensures:  the result is less than bound; `random_state` advances.
 */
static size_t random_below(size_t bound)
{
    uint64_t value;
    size_t   reduced;

    value = next_random();
    reduced = (size_t)(value % bound);
    return reduced;
}

/* requires: holds a read share of `length` bytes at `bytes`.
 * ensures:  the read share is returned; the result is the number of newlines.
 */
static size_t model_newlines(const uint8_t *bytes, size_t length)
{
    size_t   index;
    size_t   total;
    uint8_t  value;

    total = 0;
    index = 0;
    while (index < length) {
        value = bytes[index];
        if (value == 0x0A) {
            total = total + 1;
        }
        index = index + 1;
    }
    return total;
}

/* requires: rope(rope, bytes, share); the model holds `model_length` bytes.
 * ensures:  rope(rope, bytes, share); `failures` counts every way the rope
 *           disagrees with the model or breaks its own invariants.
 */
static void expect_matches_model(const Rope *rope, const char *where)
{
    size_t   length;
    size_t   newlines;
    size_t   expected_newlines;
    int      ok;
    int      same;

    length = rope_byte_count(rope);
    if (length != model_length) {
        failures = failures + 1;
        printf("  FAIL  %s: length %zu, model %zu\n", where, length,
               model_length);
        return;
    }

    newlines = rope_newline_count(rope);
    expected_newlines = model_newlines(model, model_length);
    if (newlines != expected_newlines) {
        failures = failures + 1;
        printf("  FAIL  %s: newlines %zu, model %zu\n", where, newlines,
               expected_newlines);
        return;
    }

    ok = rope_check_invariants(rope);
    if (ok == 0) {
        failures = failures + 1;
        printf("  FAIL  %s: structural invariants broken\n", where);
        return;
    }

    if (model_length == 0) {
        return;
    }

    memset(scratch, 0, model_length);
    ok = rope_copy_range(rope, 0, model_length, scratch);
    if (ok == 0) {
        failures = failures + 1;
        printf("  FAIL  %s: whole-range copy refused\n", where);
        return;
    }

    same = memcmp(scratch, model, model_length);
    if (same != 0) {
        failures = failures + 1;
        printf("  FAIL  %s: contents differ from the model\n", where);
    }
}

/* requires: start <= end <= model_length; holds a read share of
 *           `replacement_length` bytes at `replacement`; the result fits in
 *           MODEL_CAPACITY.
 * ensures:  the model holds its bytes with [start, end) replaced.
 */
static void model_replace(size_t start, size_t end,                          const uint8_t *replacement,
                          size_t replacement_length)
{
    size_t   tail_length;
    size_t   new_length;

    tail_length = model_length - end;
    new_length = start + replacement_length + tail_length;

    memmove(model + start + replacement_length, model + end, tail_length);
    if (replacement_length > 0) {
        memcpy(model + start, replacement, replacement_length);
    }
    model_length = new_length;
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the pool alignment guarantees that did not
 *           hold. The rope relies on this: a leaf is one cache line and must
 *           never straddle two.
 */
static void test_pool_alignment(void)
{
    size_t    index;
    void     *block;
    uintptr_t address;
    uintptr_t remainder;
    int       before;

    before = failures;
    index = 0;
    while (index < 200) {
        block = pool_allocate(&pool, ROPE_LEAF_BYTES);
        expect(block != NULL, "the pool hands out a leaf");
        /* A pointer-to-integer cast, which the subset forbids in shipped
         * code. Tests are where the alignment guarantee gets checked. */
        address = (uintptr_t)block;
        remainder = address % POOL_ALIGNMENT;
        expect(remainder == 0, "every allocation is cache-line aligned");
        index = index + 1;
    }
    report("pool allocations are cache-line aligned", before);
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the round-trip properties that did not hold.
 */
static void test_from_bytes_round_trip(void)
{
    static uint8_t source[4096];
    Rope           rope;
    size_t         index;
    uint32_t       lengths[8];
    uint32_t       which;
    size_t         length;
    int            ok;
    int            before;

    before = failures;

    index = 0;
    while (index < 4096) {
        source[index] = (uint8_t)(index % 251);
        if (index % 37 == 0) {
            source[index] = 0x0A;
        }
        index = index + 1;
    }

    lengths[0] = 0;
    lengths[1] = 1;
    lengths[2] = 63;
    lengths[3] = 64;
    lengths[4] = 65;
    lengths[5] = 1000;
    lengths[6] = 2048;
    lengths[7] = 4096;

    which = 0;
    while (which < 8) {
        length = lengths[which];
        ok = rope_from_bytes(&pool, source, length, &rope);
        expect(ok == 1, "a rope is built from bytes");

        memcpy(model, source, length);
        model_length = length;
        expect_matches_model(&rope, "from_bytes");

        which = which + 1;
    }
    report("from_bytes round-trips at every boundary", before);
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the split and concat properties that did not
 *           hold.
 */
static void test_split_then_concat_is_identity(void)
{
    static uint8_t source[2000];
    Rope           rope;
    Rope           left;
    Rope           right;
    Rope           rejoined;
    size_t         index;
    size_t         offset;
    size_t         length;
    int            ok;
    int            before;

    before = failures;

    index = 0;
    while (index < 2000) {
        source[index] = (uint8_t)(index % 97);
        if (index % 11 == 0) {
            source[index] = 0x0A;
        }
        index = index + 1;
    }

    ok = rope_from_bytes(&pool, source, 2000, &rope);
    expect(ok == 1, "the source rope is built");

    offset = 0;
    while (offset <= 2000) {
        ok = rope_split(&pool, &rope, offset, &left, &right);
        expect(ok == 1, "a split succeeds");

        length = rope_byte_count(&left);
        expect(length == offset, "the prefix has the split length");
        length = rope_byte_count(&right);
        expect(length == 2000 - offset, "the suffix has the rest");

        ok = rope_check_invariants(&left);
        expect(ok == 1, "the prefix is structurally sound");
        ok = rope_check_invariants(&right);
        expect(ok == 1, "the suffix is structurally sound");

        ok = rope_concat(&pool, &left, &right, &rejoined);
        expect(ok == 1, "the halves concatenate");

        memcpy(model, source, 2000);
        model_length = 2000;
        expect_matches_model(&rejoined, "split then concat");

        offset = offset + 61;
    }
    report("split then concat is the identity", before);
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the line addressing properties that did not
 *           hold.
 */
static void test_line_addressing(void)
{
    static const uint8_t source[] = "alpha\nbeta\n\ngamma\ndelta";
    Rope                 rope;
    size_t               length;
    size_t               offset;
    size_t               line;
    int                  ok;
    int                  before;

    before = failures;
    length = (uint32_t)(sizeof(source) - 1);

    ok = rope_from_bytes(&pool, source, length, &rope);
    expect(ok == 1, "the line-addressing rope is built");

    line = rope_newline_count(&rope);
    expect(line == 4, "four newlines are counted");

    ok = rope_line_start(&rope, 0, &offset);
    expect(ok == 1 && offset == 0, "line 0 starts at 0");
    ok = rope_line_start(&rope, 1, &offset);
    expect(ok == 1 && offset == 6, "line 1 starts after the first newline");
    ok = rope_line_start(&rope, 2, &offset);
    expect(ok == 1 && offset == 11, "line 2 starts after the second");
    ok = rope_line_start(&rope, 3, &offset);
    expect(ok == 1 && offset == 12, "an empty line still starts somewhere");
    ok = rope_line_start(&rope, 4, &offset);
    expect(ok == 1 && offset == 18, "line 4 starts after the fourth newline");
    ok = rope_line_start(&rope, 5, &offset);
    expect(ok == 0, "a line past the end is refused");

    ok = rope_line_of_offset(&rope, 0, &line);
    expect(ok == 1 && line == 0, "offset 0 is on line 0");
    ok = rope_line_of_offset(&rope, 5, &line);
    expect(ok == 1 && line == 0, "the newline itself is on its own line");
    ok = rope_line_of_offset(&rope, 6, &line);
    expect(ok == 1 && line == 1, "just past a newline is the next line");
    ok = rope_line_of_offset(&rope, 20, &line);
    expect(ok == 1 && line == 4, "a late offset is on the last line");
    ok = rope_line_of_offset(&rope, length + 1, &line);
    expect(ok == 0, "an offset past the end is refused");

    report("line addressing agrees with the text", before);
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the ways a long random edit sequence made the
 *           rope disagree with the model.
 */
static void test_random_edits_track_the_model(void)
{
    static uint8_t replacement[300];
    Rope           rope;
    Rope           edited;
    size_t         step;
    size_t         index;
    size_t         start;
    size_t         end;
    size_t         span;
    size_t         length;
    size_t         room;
    int            ok;
    int            before;

    before = failures;
    random_state = 0x243F6A8885A308D3u;

    model_length = 0;
    ok = rope_from_bytes(&pool, model, 0, &rope);
    expect(ok == 1, "the empty rope is built");

    step = 0;
    while (step < 400) {
        start = 0;
        if (model_length > 0) {
            start = random_below(model_length + 1);
        }
        span = 0;
        if (model_length > start) {
            span = random_below(model_length - start + 1);
        }
        end = start + span;

        length = random_below(200);
        room = MODEL_CAPACITY - (model_length - span);
        if (length > room) {
            length = 0;
        }

        index = 0;
        while (index < length) {
            replacement[index] = (uint8_t)(32 + random_below(90));
            if (index % 13 == 0) {
                replacement[index] = 0x0A;
            }
            index = index + 1;
        }

        ok = rope_replace_span(&pool, &rope, start, end, replacement, length,
                               &edited);
        if (ok == 0) {
            failures = failures + 1;
            printf("  FAIL  replace_span refused at step %zu\n", step);
            report("random edits track the model", before);
            return;
        }

        model_replace(start, end, replacement, length);
        expect_matches_model(&edited, "random edit");

        memcpy(&rope, &edited, sizeof(Rope));

        if (failures != before) {
            printf("  ...   first divergence at step %zu\n", step);
            report("random edits track the model", before);
            return;
        }
        step = step + 1;
    }
    report("random edits track the model over 400 steps", before);
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the sharing properties that did not hold. A
 *           small edit to a large rope must copy a spine, not the text.
 */
static void test_edits_share_structure(void)
{
    static uint8_t source[262144];
    Rope           rope;
    Rope           larger;
    Rope           edited;
    size_t         index;
    size_t         before_bytes;
    size_t         after_bytes;
    size_t         spent;
    size_t         spent_larger;
    int            ok;
    int            before;

    before = failures;

    index = 0;
    while (index < 262144) {
        source[index] = (uint8_t)(index % 89);
        index = index + 1;
    }

    ok = rope_from_bytes(&pool, source, 32768, &rope);
    expect(ok == 1, "the large rope is built");

    before_bytes = pool_handed_out(&pool);
    ok = rope_replace_span(&pool, &rope, 16384, 16385, source, 1, &edited);
    expect(ok == 1, "a one-byte edit succeeds");
    after_bytes = pool_handed_out(&pool);
    spent = after_bytes - before_bytes;

    expect(spent < 32768, "a one-byte edit does not copy the whole buffer");

    /* The real property is that the cost is logarithmic, so state it that
     * way rather than against a byte count that goes stale whenever a node
     * changes size. An eightfold larger buffer is one level deeper, so the
     * same edit may cost a little more — never eight times more. */
    ok = rope_from_bytes(&pool, source, 262144, &larger);
    expect(ok == 1, "the eightfold larger rope is built");

    before_bytes = pool_handed_out(&pool);
    ok = rope_replace_span(&pool, &larger, 131072, 131073, source, 1,
                           &edited);
    expect(ok == 1, "a one-byte edit to the larger rope succeeds");
    after_bytes = pool_handed_out(&pool);
    spent_larger = after_bytes - before_bytes;

    expect(spent_larger < spent * 2,
           "eight times the text does not cost twice the edit");

    printf("  note  a 1-byte edit cost %zu bytes at 32 KiB, %zu at 256 KiB\n",
           spent, spent_larger);

    report("edits share structure with their source", before);
}

/* Repeated concatenation of small pieces is what degrades an RRB that does
 * not rebalance: every concat leaves thin nodes and nothing ever repairs
 * them, so the tree deepens and the descent lengthens. This is the test the
 * rebalance exists for.
 *
 * requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the ways the tree degraded.
 */
static void test_many_concats_stay_balanced(void)
{
    static uint8_t piece[200];
    Rope           rope;
    Rope           small;
    Rope           joined;
    size_t         step;
    size_t         index;
    size_t         length;
    uint32_t       height;
    int            before;
    int            ok;

    before = failures;

    /* 200 bytes: two of them will not fit in one 256-byte leaf, so the
     * leaf-merge in concat cannot tidy them up and each piece stays its own
     * under-full leaf. That is exactly the drift the rebalance repairs. */
    index = 0;
    while (index < 200) {
        piece[index] = (uint8_t)(0x61 + (index % 26));
        index = index + 1;
    }

    rope_initialize_empty(&rope);
    step = 0;
    while (step < 300) {
        ok = rope_from_bytes(&pool, piece, 200, &small);
        if (ok == 0) {
            failures = failures + 1;
            printf("  FAIL  could not build piece %zu\n", step);
            report("many concatenations stay balanced", before);
            return;
        }
        ok = rope_concat(&pool, &rope, &small, &joined);
        if (ok == 0) {
            failures = failures + 1;
            printf("  FAIL  concat %zu refused\n", step);
            report("many concatenations stay balanced", before);
            return;
        }
        memcpy(&rope, &joined, sizeof(Rope));
        step = step + 1;
    }

    length = rope_byte_count(&rope);
    expect(length == 60000, "the concatenated rope has every byte");

    ok = rope_check_invariants(&rope);
    expect(ok == 1, "it is structurally sound");

    ok = rope_check_fill(&rope);
    expect(ok == 1, "every node is within RRB_EXTRAS of optimal fill");

    /* 20000 bytes at 256 per leaf is 79 leaves, which two levels of
     * branching 32 hold comfortably. An unbalanced tree would be deeper. */
    height = rope.height;
    expect(height <= 2, "the tree did not deepen");
    printf("  note  300 concatenations of 200 bytes gave height %u for %zu\n",
           height, length);

    report("many concatenations stay balanced", before);
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

    test_pool_alignment();
    test_from_bytes_round_trip();
    test_split_then_concat_is_identity();
    test_line_addressing();
    test_random_edits_track_the_model();
    test_edits_share_structure();
    test_many_concats_stay_balanced();

    printf("  note  pool reserved %zu bytes for %zu handed out\n",
           pool_reserved(&pool), pool_handed_out(&pool));
    pool_release(&pool);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
