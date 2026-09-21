#include "address.h"

#include <string.h>

/* The window the literal search slides over the rope. Copying a window at a
 * time keeps the search linear without materializing the whole buffer, which
 * would defeat the point of having a rope at all. */
#define SEARCH_WINDOW 4096

/* requires: *result writable; the span is within the buffer.
 * ensures:  *result has one more span appended and the outcome is 1; or it
 *           was already full and the outcome is 0.
 */
static int decomposition_append(Decomposition *result, uint32_t start,
                                uint32_t end)
{
    uint32_t count;

    count = result->count;
    if (count >= DECOMPOSITION_CAPACITY) {
        return 0;
    }
    result->focus[count].start = start;
    result->focus[count].end = end;
    result->count = count + 1;
    return 1;
}

/* requires: rope(rope, bytes, share); line counts from 1; the span
 *           out-parameters are writable.
 * ensures:  rope(rope, bytes, share). When that line exists, *start and *end
 *           bound it — the newline that terminates it included, so that
 *           deleting a line removes its separator too — and the result is 1;
 *           otherwise the result is 0.
 */
static int line_bounds(const Rope *rope, uint32_t line, uint32_t *start,
                       uint32_t *end)
{
    uint32_t total;
    uint32_t newlines;
    uint32_t line_count;
    uint32_t begin;
    uint32_t finish;
    uint32_t begin_slot;
    uint32_t finish_slot;
    int      ok;

    total = rope->byte_count;
    newlines = rope->newline_count;

    /* A buffer not ending in a newline still has a final line. */
    line_count = newlines;
    if (total > 0) {
        ok = rope_line_start(rope, newlines, &begin_slot);
        if (ok == 1) {
            begin = begin_slot;
            if (begin < total) {
                line_count = newlines + 1;
            }
        }
    }
    if (line_count == 0) {
        return 0;
    }
    if (line == 0) {
        return 0;
    }
    if (line > line_count) {
        return 0;
    }

    ok = rope_line_start(rope, line - 1, &begin_slot);
    if (ok == 0) {
        return 0;
    }
    begin = begin_slot;

    ok = rope_line_start(rope, line, &finish_slot);
    if (ok == 0) {
        finish = total;
    } else {
        finish = finish_slot;
    }

    *start = begin;
    *end = finish;
    return 1;
}

/* requires: rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); the result is the number of lines,
 *           counting a trailing fragment with no newline as a line. No
 *           memory is written.
 */
static uint32_t line_count_of(const Rope *rope)
{
    uint32_t total;
    uint32_t newlines;
    uint32_t begin;
    uint32_t begin_slot;
    int      ok;

    total = rope->byte_count;
    if (total == 0) {
        return 0;
    }
    newlines = rope->newline_count;
    ok = rope_line_start(rope, newlines, &begin_slot);
    if (ok == 1) {
        begin = begin_slot;
        if (begin < total) {
            return newlines + 1;
        }
    }
    return newlines;
}

/* requires: as address.h.
 * ensures:  as address.h.
 */
int address_find_literal(const Rope *rope, uint32_t from,
                         const unsigned char *pattern, uint32_t pattern_length,
                         uint32_t *found)
{
    unsigned char window[SEARCH_WINDOW];
    uint32_t      total;
    uint32_t      position;
    uint32_t      span;
    uint32_t      limit;
    uint32_t      index;
    uint32_t      compared;
    unsigned char left;
    unsigned char right;
    int           ok;
    int           matched;

    total = rope->byte_count;
    if (pattern_length == 0) {
        return 0;
    }
    if (pattern_length > total) {
        return 0;
    }

    position = from;
    while (position + pattern_length <= total) {
        span = total - position;
        if (span > SEARCH_WINDOW) {
            span = SEARCH_WINDOW;
        }
        ok = rope_copy_range(rope, position, span, window);
        if (ok == 0) {
            return 0;
        }

        /* Only start a match where the whole pattern fits in this window;
         * the next window overlaps by pattern_length - 1 so nothing is
         * missed at the seam. */
        limit = span - pattern_length;
        index = 0;
        while (index <= limit) {
            matched = 1;
            compared = 0;
            while (compared < pattern_length) {
                left = window[index + compared];
                right = pattern[compared];
                if (left != right) {
                    matched = 0;
                    compared = pattern_length;
                } else {
                    compared = compared + 1;
                }
            }
            if (matched == 1) {
                *found = position + index;
                return 1;
            }
            index = index + 1;
        }

        if (span < SEARCH_WINDOW) {
            return 0;
        }
        position = position + span - (pattern_length - 1);
    }
    return 0;
}

/* requires: rope(rope, bytes, share); *result writable.
 * ensures:  *result gains one focus per occurrence of the pattern; the
 *           outcome is 1, or 0 when there were too many to record.
 */
static int resolve_matches(const Rope *rope, const Address *address,
                           Decomposition *result)
{
    const unsigned char *pattern;
    uint32_t             pattern_length;
    uint32_t             position;
    uint32_t             total;
    uint32_t             at;
    uint32_t             at_slot;
    int                  ok;

    pattern = address->pattern;
    pattern_length = address->pattern_length;
    total = rope->byte_count;

    if (pattern_length == 0) {
        return 0;
    }

    position = 0;
    while (position <= total) {
        ok = address_find_literal(rope, position, pattern, pattern_length,
                                  &at_slot);
        if (ok == 0) {
            return 1;
        }
        at = at_slot;
        position = at;
        ok = decomposition_append(result, position, position + pattern_length);
        if (ok == 0) {
            return 0;
        }
        position = position + pattern_length;
    }
    return 1;
}

/* requires: rope(rope, bytes, share); *result writable.
 * ensures:  *result gains one focus per line containing the pattern; the
 *           outcome is 1, or 0 when there were too many to record.
 */
static int resolve_lines_matching(const Rope *rope, const Address *address,
                                  Decomposition *result)
{
    const unsigned char *pattern;
    uint32_t             pattern_length;
    uint32_t             position;
    uint32_t             total;
    uint32_t             at;
    uint32_t             line;
    uint32_t             start;
    uint32_t             end;
    uint32_t             previous_end;
    uint32_t             at_slot;
    uint32_t             line_slot;
    uint32_t             start_slot;
    uint32_t             end_slot;
    int                  ok;

    pattern = address->pattern;
    pattern_length = address->pattern_length;
    total = rope->byte_count;
    if (pattern_length == 0) {
        return 0;
    }

    position = 0;
    previous_end = 0;
    while (position <= total) {
        ok = address_find_literal(rope, position, pattern, pattern_length,
                                  &at_slot);
        if (ok == 0) {
            return 1;
        }
        at = at_slot;

        ok = rope_line_of_offset(rope, at, &line_slot);
        if (ok == 0) {
            return 0;
        }
        line = line_slot;

        ok = line_bounds(rope, line + 1, &start_slot, &end_slot);
        if (ok == 0) {
            return 0;
        }
        start = start_slot;
        end = end_slot;
        /* Several matches on one line select that line once. */
        if (start >= previous_end) {
            ok = decomposition_append(result, start, end);
            if (ok == 0) {
                return 0;
            }
            previous_end = end;
        }
        position = end;
        if (end <= at) {
            position = at + pattern_length;
        }
    }
    return 1;
}

/* requires: as address.h.
 * ensures:  as address.h.
 */
int address_resolve(const Rope *rope, const Address *address,
                    uint32_t current_line, Decomposition *result)
{
    AddressKind kind;
    uint32_t    total;
    uint32_t    first;
    uint32_t    last;
    uint32_t    start;
    uint32_t    end;
    uint32_t    lines;
    uint32_t    line;
    uint32_t    span_start;
    uint32_t    span_end;
    uint32_t    start_slot;
    uint32_t    end_slot;
    int         ok;

    result->count = 0;

    total = rope->byte_count;
    kind = address->kind;
    first = address->first;
    last = address->last;

    if (kind == ADDRESS_ALL) {
        ok = decomposition_append(result, 0, total);
        return ok;
    }

    if (kind == ADDRESS_BYTE) {
        if (first > total) {
            return 0;
        }
        ok = decomposition_append(result, first, first);
        return ok;
    }

    if (kind == ADDRESS_BYTE_RANGE) {
        if (first > last) {
            return 0;
        }
        if (last > total) {
            return 0;
        }
        ok = decomposition_append(result, first, last);
        return ok;
    }

    if (kind == ADDRESS_MATCH) {
        ok = resolve_matches(rope, address, result);
        return ok;
    }

    if (kind == ADDRESS_LINES_MATCHING) {
        ok = resolve_lines_matching(rope, address, result);
        return ok;
    }

    lines = line_count_of(rope);

    if (kind == ADDRESS_CURRENT) {
        ok = line_bounds(rope, current_line, &start_slot, &end_slot);
        if (ok == 0) {
            return 0;
        }
        start = start_slot;
        end = end_slot;
        ok = decomposition_append(result, start, end);
        return ok;
    }

    if (kind == ADDRESS_LAST) {
        ok = line_bounds(rope, lines, &start_slot, &end_slot);
        if (ok == 0) {
            return 0;
        }
        start = start_slot;
        end = end_slot;
        ok = decomposition_append(result, start, end);
        return ok;
    }

    if (kind == ADDRESS_LINE) {
        ok = line_bounds(rope, first, &start_slot, &end_slot);
        if (ok == 0) {
            return 0;
        }
        start = start_slot;
        end = end_slot;
        ok = decomposition_append(result, start, end);
        return ok;
    }

    if (kind == ADDRESS_LINE_RANGE) {
        if (first > last) {
            return 0;
        }
        ok = line_bounds(rope, first, &start_slot, &end_slot);
        if (ok == 0) {
            return 0;
        }
        span_start = start_slot;

        ok = line_bounds(rope, last, &start_slot, &end_slot);
        if (ok == 0) {
            return 0;
        }
        span_end = end_slot;
        /* One focus spanning the whole run: a range of lines is a single
         * contiguous region, not a list of separate ones. */
        ok = decomposition_append(result, span_start, span_end);
        return ok;
    }

    line = current_line;
    ok = line_bounds(rope, line, &start_slot, &end_slot);
    if (ok == 0) {
        return 0;
    }
    start = start_slot;
    end = end_slot;
    ok = decomposition_append(result, start, end);
    return ok;
}

/* requires: as address.h.
 * ensures:  as address.h.
 */
int address_replace_all(Pool *pool, const Rope *rope,
                        const Decomposition *decomposition,
                        const unsigned char *replacement,
                        uint32_t replacement_length,
                        Rope *result)
{
    Rope     built;
    Rope     joined;
    Rope     gap;
    Rope     inserted;
    uint32_t count;
    uint32_t index;
    uint32_t position;
    uint32_t total;
    uint32_t start;
    uint32_t end;
    int      ok;

    count = decomposition->count;
    total = rope->byte_count;

    rope_initialize_empty(&built);
    ok = rope_from_bytes(pool, replacement, replacement_length, &inserted);
    if (ok == 0) {
        return 0;
    }

    position = 0;
    index = 0;
    while (index < count) {
        start = decomposition->focus[index].start;
        end = decomposition->focus[index].end;

        ok = rope_slice(pool, rope, position, start, &gap);
        if (ok == 0) {
            return 0;
        }
        ok = rope_concat(pool, &built, &gap, &joined);
        if (ok == 0) {
            return 0;
        }
        ok = rope_concat(pool, &joined, &inserted, &built);
        if (ok == 0) {
            return 0;
        }

        position = end;
        index = index + 1;
    }

    ok = rope_slice(pool, rope, position, total, &gap);
    if (ok == 0) {
        return 0;
    }
    ok = rope_concat(pool, &built, &gap, result);
    return ok;
}

/* requires: as address.h.
 * ensures:  as address.h.
 */
int address_insert_all(Pool *pool, const Rope *rope,
                       const Decomposition *decomposition, int before,
                       const unsigned char *insertion,
                       uint32_t insertion_length,
                       Rope *result)
{
    Rope     built;
    Rope     joined;
    Rope     gap;
    Rope     inserted;
    uint32_t count;
    uint32_t index;
    uint32_t position;
    uint32_t total;
    uint32_t start;
    uint32_t end;
    uint32_t at;
    int      ok;

    count = decomposition->count;
    total = rope->byte_count;

    rope_initialize_empty(&built);
    ok = rope_from_bytes(pool, insertion, insertion_length, &inserted);
    if (ok == 0) {
        return 0;
    }

    position = 0;
    index = 0;
    while (index < count) {
        start = decomposition->focus[index].start;
        end = decomposition->focus[index].end;

        at = end;
        if (before == 1) {
            at = start;
        }

        ok = rope_slice(pool, rope, position, at, &gap);
        if (ok == 0) {
            return 0;
        }
        ok = rope_concat(pool, &built, &gap, &joined);
        if (ok == 0) {
            return 0;
        }
        ok = rope_concat(pool, &joined, &inserted, &built);
        if (ok == 0) {
            return 0;
        }

        position = at;
        index = index + 1;
    }

    ok = rope_slice(pool, rope, position, total, &gap);
    if (ok == 0) {
        return 0;
    }
    ok = rope_concat(pool, &built, &gap, result);
    return ok;
}
