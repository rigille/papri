#include "line_index.h"
#include "pool.h"
#include "rope.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The index is checked against a scan of the same bytes. The model is
 * obviously correct and linear; the index is logarithmic and subtle, and
 * they must agree on every line and every offset.
 *
 * The test that matters most is the last one: an index derived
 * incrementally through a long edit sequence must equal the index built
 * from scratch against the same buffer. That is the property the whole
 * structure exists to provide, and the one an incremental update is most
 * likely to get wrong.
 */

#define TEXT_CAPACITY 200000u

static int      failures;
static Pool     pool;
static uint8_t  text[TEXT_CAPACITY];
static size_t   text_length;
static uint64_t random_state;

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

/* requires: nothing.
 * ensures:  `random_state` advances; the result is the next xorshift value.
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
 * ensures:  the result is less than bound.
 */
static size_t random_below(size_t bound)
{
    uint64_t value;

    value = next_random();
    return (size_t)(value % bound);
}

/* The model: where does line `line` begin, by scanning?
 *
 * requires: the model holds `text_length` bytes; *offset writable.
 * ensures:  *offset is where that line begins and the result is 1; or no
 *           such line and the result is 0.
 */
static int model_line_start(size_t line, size_t *offset)
{
    size_t  index;
    size_t  seen;
    uint8_t value;

    if (line == 0) {
        *offset = 0;
        return 1;
    }
    seen = 0;
    index = 0;
    while (index < text_length) {
        value = text[index];
        if (value == 0x0A) {
            seen = seen + 1;
            if (seen == line) {
                *offset = index + 1;
                return 1;
            }
        }
        index = index + 1;
    }
    return 0;
}

/* requires: offset is at most text_length.
 * ensures:  the result is how many newlines precede it.
 */
static size_t model_line_of_offset(size_t offset)
{
    size_t  index;
    size_t  seen;
    uint8_t value;

    seen = 0;
    index = 0;
    while (index < offset) {
        value = text[index];
        if (value == 0x0A) {
            seen = seen + 1;
        }
        index = index + 1;
    }
    return seen;
}

/* requires: node_pool is live; the model holds text_length bytes.
 * ensures:  `failures` counts the lines or offsets on which the index and
 *           the model disagreed.
 */
static void compare_against_model(const LineIndex *index, const Rope *rope,
                                  const char *where)
{
    size_t lines;
    size_t line;
    size_t offset;
    size_t expected;
    size_t reported;
    size_t probe;
    size_t step;
    int    ok;
    int    model_ok;

    ok = line_index_check(index);
    if (ok == 0) {
        failures = failures + 1;
        printf("  FAIL  %s: the index is internally inconsistent\n", where);
        return;
    }

    lines = line_index_newline_count(index);
    expected = model_line_of_offset(text_length);
    if (lines != expected) {
        failures = failures + 1;
        printf("  FAIL  %s: %zu newlines, model says %zu\n", where, lines,
               expected);
        return;
    }

    line = 0;
    while (line <= lines) {
        model_ok = model_line_start(line, &expected);
        ok = line_index_line_start(index, rope, line, &offset);
        if (ok != model_ok) {
            failures = failures + 1;
            printf("  FAIL  %s: line %zu answered %d, model %d\n", where,
                   line, ok, model_ok);
            return;
        }
        if (ok == 1) {
            if (offset != expected) {
                failures = failures + 1;
                printf("  FAIL  %s: line %zu starts at %zu, model %zu\n",
                       where, line, offset, expected);
                return;
            }
        }
        line = line + 1;
    }

    step = text_length / 500;
    if (step == 0) {
        step = 1;
    }
    probe = 0;
    while (probe <= text_length) {
        ok = line_index_line_of_offset(index, rope, probe, &reported);
        expected = model_line_of_offset(probe);
        if (ok == 0 || reported != expected) {
            failures = failures + 1;
            printf("  FAIL  %s: offset %zu is on line %zu, model %zu\n",
                   where, probe, reported, expected);
            return;
        }
        probe = probe + step;
    }
}

/* requires: node_pool is live.
 * ensures:  `failures` counts the ways a freshly built index disagreed with
 *           a scan.
 */
static void test_built_index_matches_a_scan(void)
{
    Rope      rope;
    LineIndex index;
    size_t    lengths[5];
    size_t    which;
    size_t    length;
    size_t    position;
    int       before;
    int       ok;

    before = failures;
    random_state = 0x853C49E6748FEA9Bu;

    lengths[0] = 0;
    lengths[1] = 1;
    lengths[2] = 5000;
    lengths[3] = 40000;
    lengths[4] = 150000;

    which = 0;
    while (which < 5) {
        length = lengths[which];

        position = 0;
        while (position < length) {
            text[position] = (uint8_t)(0x61 + random_below(26));
            if (random_below(40) == 0) {
                text[position] = 0x0A;
            }
            position = position + 1;
        }
        text_length = length;

        ok = rope_from_bytes(&pool, text, text_length, &rope);
        expect(ok == 1, "the rope is built");
        ok = line_index_build(&pool, &rope, &index);
        expect(ok == 1, "the index is built");

        compare_against_model(&index, &rope, "fresh build");
        which = which + 1;
    }

    report("a freshly built index agrees with a scan", before);
}

/* The decisive one. An index carried forward through many edits must equal
 * the index that would be built from scratch against the same buffer.
 *
 * requires: node_pool is live.
 * ensures:  `failures` counts the ways the incremental index drifted.
 */
static void test_incremental_update_matches_a_rebuild(void)
{
    static uint8_t replacement[300];
    Rope           rope;
    Rope           edited;
    LineIndex      index;
    LineIndex      updated;
    size_t         step;
    size_t         position;
    size_t         start;
    size_t         end;
    size_t         span;
    size_t         length;
    size_t         tail;
    int            before;
    int            ok;

    before = failures;
    random_state = 0x2545F4914F6CDD1Du;

    position = 0;
    while (position < 20000) {
        text[position] = (uint8_t)(0x61 + random_below(26));
        if (random_below(40) == 0) {
            text[position] = 0x0A;
        }
        position = position + 1;
    }
    text_length = 20000;

    ok = rope_from_bytes(&pool, text, text_length, &rope);
    expect(ok == 1, "the starting rope is built");
    ok = line_index_build(&pool, &rope, &index);
    expect(ok == 1, "the starting index is built");

    step = 0;
    while (step < 120) {
        start = random_below(text_length);
        span = random_below(200);
        if (start + span > text_length) {
            span = text_length - start;
        }
        end = start + span;

        length = random_below(200);
        position = 0;
        while (position < length) {
            replacement[position] = (uint8_t)(0x61 + random_below(26));
            if (random_below(30) == 0) {
                replacement[position] = 0x0A;
            }
            position = position + 1;
        }

        ok = rope_replace_span(&pool, &rope, start, end, replacement, length,
                               &edited);
        if (ok == 0) {
            failures = failures + 1;
            printf("  FAIL  edit %zu refused\n", step);
            report("an incremental index agrees with a rebuild", before);
            return;
        }

        ok = line_index_update(&pool, &index, &edited, start, end, length,
                               &updated);
        if (ok == 0) {
            failures = failures + 1;
            printf("  FAIL  index update %zu refused\n", step);
            report("an incremental index agrees with a rebuild", before);
            return;
        }

        /* Keep the model in step. */
        tail = text_length - end;
        memmove(text + start + length, text + end, tail);
        if (length > 0) {
            memcpy(text + start, replacement, length);
        }
        text_length = start + length + tail;

        memcpy(&rope, &edited, sizeof(Rope));
        memcpy(&index, &updated, sizeof(LineIndex));

        compare_against_model(&index, &rope, "after an edit");
        if (failures != before) {
            printf("  ...   first divergence at edit %zu\n", step);
            report("an incremental index agrees with a rebuild", before);
            return;
        }
        step = step + 1;
    }

    report("an incremental index agrees with a rebuild over 120 edits",
           before);
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

    test_built_index_matches_a_scan();
    test_incremental_update_matches_a_rebuild();

    pool_release(&pool);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
