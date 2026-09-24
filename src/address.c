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
static int decomposition_append(Decomposition *result, size_t start,
                                size_t end)
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
static int line_bounds(const Rope *rope, const LineIndex *index,
                       size_t line, size_t *start, size_t *end)
{
    size_t   total;
    size_t   newlines;
    size_t   line_count;
    size_t   begin;
    size_t   finish;
    size_t   begin_slot;
    size_t   finish_slot;
    int      ok;

    total = rope->byte_count;
    newlines = line_index_newline_count(index);

    /* A buffer not ending in a newline still has a final line. */
    line_count = newlines;
    if (total > 0) {
        ok = line_index_line_start(index, rope, newlines, &begin_slot);
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

    ok = line_index_line_start(index, rope, line - 1, &begin_slot);
    if (ok == 0) {
        return 0;
    }
    begin = begin_slot;

    ok = line_index_line_start(index, rope, line, &finish_slot);
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
static size_t line_count_of(const Rope *rope, const LineIndex *index)
{
    size_t   total;
    size_t   newlines;
    size_t   begin;
    size_t   begin_slot;
    int      ok;

    total = rope->byte_count;
    if (total == 0) {
        return 0;
    }
    newlines = line_index_newline_count(index);
    ok = line_index_line_start(index, rope, newlines, &begin_slot);
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
int address_find_literal(const Rope *rope, size_t from,
                         const unsigned char *pattern, size_t pattern_length,
                         size_t *found)
{
    unsigned char window[SEARCH_WINDOW];
    size_t        total;
    size_t        position;
    size_t        span;
    size_t        limit;
    uint32_t      index;
    size_t        compared;
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
    size_t               pattern_length;
    size_t               position;
    size_t               total;
    size_t               at;
    size_t               at_slot;
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
static int resolve_lines_matching(const Rope *rope, const LineIndex *index,
                                  const Address *address,
                                  Decomposition *result)
{
    const unsigned char *pattern;
    size_t               pattern_length;
    size_t               position;
    size_t               total;
    size_t               at;
    size_t               line;
    size_t               start;
    size_t               end;
    size_t               previous_end;
    size_t               at_slot;
    size_t               line_slot;
    size_t               start_slot;
    size_t               end_slot;
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

        ok = line_index_line_of_offset(index, rope, at, &line_slot);
        if (ok == 0) {
            return 0;
        }
        line = line_slot;

        ok = line_bounds(rope, index, line + 1, &start_slot, &end_slot);
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
int address_resolve(const Rope *rope, const LineIndex *index,
                    const Address *address, size_t current_line,
                    Decomposition *result)
{
    AddressKind kind;
    size_t      total;
    size_t      first;
    size_t      last;
    size_t      start;
    size_t      end;
    size_t      lines;
    size_t      line;
    size_t      span_start;
    size_t      span_end;
    size_t      start_slot;
    size_t      end_slot;
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
        ok = resolve_lines_matching(rope, index, address, result);
        return ok;
    }

    if (kind == ADDRESS_STRUCTURE) {
        /* Needs a grammar, which this layer has no business knowing about.
         * See the spec in address.h. */
        return 0;
    }

    lines = line_count_of(rope, index);

    if (kind == ADDRESS_CURRENT) {
        ok = line_bounds(rope, index, current_line, &start_slot, &end_slot);
        if (ok == 0) {
            return 0;
        }
        start = start_slot;
        end = end_slot;
        ok = decomposition_append(result, start, end);
        return ok;
    }

    if (kind == ADDRESS_LAST) {
        ok = line_bounds(rope, index, lines, &start_slot, &end_slot);
        if (ok == 0) {
            return 0;
        }
        start = start_slot;
        end = end_slot;
        ok = decomposition_append(result, start, end);
        return ok;
    }

    if (kind == ADDRESS_LINE) {
        ok = line_bounds(rope, index, first, &start_slot, &end_slot);
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
        ok = line_bounds(rope, index, first, &start_slot, &end_slot);
        if (ok == 0) {
            return 0;
        }
        span_start = start_slot;

        ok = line_bounds(rope, index, last, &start_slot, &end_slot);
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
    ok = line_bounds(rope, index, line, &start_slot, &end_slot);
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
                        size_t replacement_length,
                        Rope *result)
{
    Rope     built;
    Rope     joined;
    Rope     gap;
    Rope     inserted;
    uint32_t count;
    uint32_t index;
    size_t   position;
    size_t   total;
    size_t   start;
    size_t   end;
    int      ok;

    count = decomposition->count;
    total = rope->byte_count;

    rope_initialize_empty(&built);

    position = 0;
    index = 0;
    while (index < count) {
        start = decomposition->focus[index].start;
        end = decomposition->focus[index].end;

        /* Fresh each time, never spliced twice: see the note above. */
        ok = rope_from_bytes(pool, replacement, replacement_length,
                             &inserted);
        if (ok == 0) {
            return 0;
        }

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
    if (ok == 0) {
        return 0;
    }

    return 1;
}

/* requires: as address.h.
 * ensures:  as address.h.
 */
int address_insert_all(Pool *pool, const Rope *rope,
                       const Decomposition *decomposition, int before,
                       const unsigned char *insertion,
                       size_t insertion_length,
                       Rope *result)
{
    Rope     built;
    Rope     joined;
    Rope     gap;
    Rope     inserted;
    uint32_t count;
    uint32_t index;
    size_t   position;
    size_t   total;
    size_t   start;
    size_t   end;
    size_t   at;
    int      ok;

    count = decomposition->count;
    total = rope->byte_count;

    rope_initialize_empty(&built);

    position = 0;
    index = 0;
    while (index < count) {
        start = decomposition->focus[index].start;
        end = decomposition->focus[index].end;

        /* Fresh each time, never spliced twice: see the note above. */
        ok = rope_from_bytes(pool, insertion, insertion_length, &inserted);
        if (ok == 0) {
            return 0;
        }

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
    if (ok == 0) {
        return 0;
    }

    return 1;
}

/* The window the quoting copy slides over the rope, for the same reason the
 * search has one: a focus may be the whole buffer, and materializing it
 * would defeat the rope. */
#define QUOTE_WINDOW 4096

/* Copy a span into a rope of its own.
 *
 * rope_slice would be cheaper and is wrong here: a replacement naming the
 * focus twice would then reach one subtree by two paths within a single
 * version, and a version must be a tree. Copying is what makes each
 * quotation a distinct subtree.
 *
 * requires: node_pool(pool, live, residual); rope(rope, bytes, share);
 *           start <= end <= |bytes|; *quoted writable and not *rope.
 * ensures:  rope(rope, bytes, share) is returned; rope(quoted, s, share')
 *           where s is bytes[start, end) and share' is of fresh nodes only,
 *           sharing nothing with *rope, and the result is 1; or allocation
 *           failed and the result is 0.
 */
static int quote_span(Pool *pool, const Rope *rope, size_t start, size_t end,
                      Rope *quoted)
{
    unsigned char window[QUOTE_WINDOW];
    Rope          built;
    Rope          piece;
    Rope          joined;
    size_t        position;
    size_t        span;
    int           ok;

    rope_initialize_empty(&built);

    position = start;
    while (position < end) {
        span = end - position;
        if (span > QUOTE_WINDOW) {
            span = QUOTE_WINDOW;
        }
        ok = rope_copy_range(rope, position, span, window);
        if (ok == 0) {
            return 0;
        }
        ok = rope_from_bytes(pool, window, span, &piece);
        if (ok == 0) {
            return 0;
        }
        ok = rope_concat(pool, &built, &piece, &joined);
        if (ok == 0) {
            return 0;
        }
        memcpy(&built, &joined, sizeof(Rope));
        position = position + span;
    }

    memcpy(quoted, &built, sizeof(Rope));
    return 1;
}

/* requires: as address.h.
 * ensures:  as address.h.
 */
int address_replace_each(Pool *pool, const Rope *rope,
                         const Decomposition *decomposition,
                         const Replacement *replacement,
                         Rope *result)
{
    const unsigned char *text;
    Rope     built;
    Rope     joined;
    Rope     gap;
    Rope     piece;
    uint32_t count;
    uint32_t index;
    uint32_t runs;
    uint32_t run;
    size_t   position;
    size_t   total;
    size_t   start;
    size_t   end;
    size_t   run_start;
    size_t   run_length;
    int      ok;

    count = decomposition->count;
    total = rope->byte_count;
    runs = replacement->run_count;
    text = replacement->text;
    if (runs == 0) {
        return 0;
    }

    rope_initialize_empty(&built);

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
        memcpy(&built, &joined, sizeof(Rope));

        run = 0;
        while (run < runs) {
            run_start = replacement->run_start[run];
            run_length = replacement->run_length[run];

            /* Fresh each time, never spliced twice: see the note above. */
            ok = rope_from_bytes(pool, text + run_start, run_length, &piece);
            if (ok == 0) {
                return 0;
            }
            ok = rope_concat(pool, &built, &piece, &joined);
            if (ok == 0) {
                return 0;
            }
            memcpy(&built, &joined, sizeof(Rope));

            /* Between two runs stands one quotation of the focus. */
            if (run + 1 < runs) {
                ok = quote_span(pool, rope, start, end, &piece);
                if (ok == 0) {
                    return 0;
                }
                ok = rope_concat(pool, &built, &piece, &joined);
                if (ok == 0) {
                    return 0;
                }
                memcpy(&built, &joined, sizeof(Rope));
            }

            run = run + 1;
        }

        position = end;
        index = index + 1;
    }

    ok = rope_slice(pool, rope, position, total, &gap);
    if (ok == 0) {
        return 0;
    }
    ok = rope_concat(pool, &built, &gap, result);
    if (ok == 0) {
        return 0;
    }

    return 1;
}
