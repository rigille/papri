#include "command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARGUMENT_CAPACITY 8192
#define PATTERN_CAPACITY  1024
#define OUTPUT_WINDOW     4096

/* Passed to adopt as `start` when the buffer was replaced wholesale and the
 * index cannot be derived from the old one. */
#define REBUILD_INDEX     SIZE_MAX

/* Returned by the parsing helpers when the text did not fit or was
 * malformed, and used as the open end of a `N,$` line range. */
#define PARSE_FAILED     SIZE_MAX
#define LINE_RANGE_OPEN  SIZE_MAX

/* Defined below. Works out the region an edit touched and hands it to
 * adopt, so the line index can be carried forward rather than rebuilt.
 *
 * requires: editor(editor); rope(edited, bytes2, share);
 *           decomposition(foci, spans, bytes) against the version `edited`
 *           came from.
 * ensures:  editor(editor) advanced to that version with its index updated.
 */
static void adopt_edit(Editor *editor, const Rope *edited,
                       const Decomposition *foci);

/* Defined below, beside the rest of the grammar machinery. Addresses need
 * it, and it needs everything the editor knows about buffers, so the
 * declaration comes up here and the definition stays down there.
 *
 * requires: editor(editor).
 * ensures:  editor(editor); the result is the grammar governing this
 *           buffer's name, loaded, or null with a reason written.
 */
static Structure *grammar_for_buffer(Editor *editor);

/* Defined below, beside the rest of the job machinery.
 *
 * requires: editor(editor); `path` is NUL-terminated.
 * ensures:  editor(editor) with one more job in flight and the result 1; or
 *           the job could not be started, a reason is written, and the
 *           result is 0.
 */
static int start_job(Editor *editor, JobKind kind, const char *path);

/* One parsed command line. A pure value: the address is data, the verb is a
 * byte, and the operand text has already had its escapes resolved. */
typedef struct Command {
    Address       address;
    int           addressed;      /* 1 when the line carried an address */
    char          verb;
    unsigned char pattern[PATTERN_CAPACITY];
    unsigned char operand[ARGUMENT_CAPACITY];
} Command;

/* requires: holds a read share of `length` bytes at `bytes`.
 * ensures:  the read share is returned; those bytes are written to standard
 *           output.
 */
static void write_bytes(const unsigned char *bytes, uint32_t length)
{
    FILE *stream;

    /* `stdout` is an extern pointer, so reading it is a load and may not sit
     * inside a call argument. */
    stream = stdout;
    fwrite(bytes, 1, length, stream);
}

/* requires: `text` is NUL-terminated.
 * ensures:  it is written to standard output followed by a newline.
 */
static void write_line(const char *text)
{
    FILE *stream;

    stream = stdout;
    fputs(text, stream);
    fputc('\n', stream);
}

/* requires: nothing.
 * ensures:  the result is 1 when the byte is a space or a tab.
 */
static int is_blank(char value)
{
    if (value == ' ') {
        return 1;
    }
    if (value == '\t') {
        return 1;
    }
    return 0;
}

/* requires: nothing.
 * ensures:  the result is 1 when the byte is an ASCII digit.
 */
static int is_digit(char value)
{
    if (value < '0') {
        return 0;
    }
    if (value > '9') {
        return 0;
    }
    return 1;
}

/* Returns the new position rather than writing through a pointer. Taking a
 * local cursor's address would move it to memory and make every later use
 * of it a load, which the subset forbids inside conditions and arguments.
 *
 * requires: `line` is NUL-terminated; `position` indexes into it.
 * ensures:  the result is `position` advanced past any run of blanks. No
 *           memory is written.
 */
static size_t skip_blanks(const char *line, size_t position)
{
    size_t   at;
    char     value;
    int      blank;

    at = position;
    value = line[at];
    blank = is_blank(value);
    while (blank == 1) {
        at = at + 1;
        value = line[at];
        blank = is_blank(value);
    }
    return at;
}

/* requires: `line` is NUL-terminated; `position` indexes into it;
 *           *value_slot is writable and is read back by the caller only in
 *           a statement of its own.
 * ensures:  when a digit run started at `position`, *value_slot is the
 *           number and the result is the position past it; otherwise
 *           *value_slot is unchanged and the result is `position`.
 */
static size_t read_number(const char *line, size_t position,
                          size_t *value_slot)
{
    size_t   at;
    size_t   total;
    char     digit;
    int      ok;

    at = position;
    digit = line[at];
    ok = is_digit(digit);
    if (ok == 0) {
        return position;
    }

    total = 0;
    while (ok == 1) {
        total = total * 10;
        total = total + (uint32_t)(digit - 0x30);
        at = at + 1;
        digit = line[at];
        ok = is_digit(digit);
    }

    *value_slot = total;
    return at;
}

/* requires: `line` is NUL-terminated and *position indexes the byte after an
 *           opening delimiter; `destination` holds `capacity` bytes.
 * ensures:  `destination` holds the bytes up to the next unescaped
 *           delimiter, *length is how many, *position is past the closing
 *           delimiter when there was one, and the result is 1; or the text
 *           did not fit and the result is 0.
 */
static size_t read_delimited(const char *line, size_t position,
                             char delimiter, unsigned char *destination,
                             size_t capacity, size_t *length_slot)
{
    size_t   at;
    size_t   written;
    char     value;
    char     escaped;

    at = position;
    written = 0;

    value = line[at];
    while (value != '\0') {
        if (value == delimiter) {
            at = at + 1;
            *length_slot = written;
            return at;
        }
        if (written >= capacity) {
            return PARSE_FAILED;
        }
        if (value == '\\') {
            escaped = line[at + 1];
            if (escaped == 'n') {
                destination[written] = '\n';
                at = at + 2;
            } else if (escaped == 't') {
                destination[written] = '\t';
                at = at + 2;
            } else if (escaped == '\\') {
                destination[written] = '\\';
                at = at + 2;
            } else if (escaped == '\0') {
                destination[written] = '\\';
                at = at + 1;
            } else {
                destination[written] = (unsigned char)escaped;
                at = at + 2;
            }
        } else {
            destination[written] = (unsigned char)value;
            at = at + 1;
        }
        written = written + 1;
        value = line[at];
    }

    *length_slot = written;
    return at;
}

/* The replacement text of a verb that replaces, where `\1` stands for the
 * text being replaced.
 *
 * This cannot go through read_delimited, and the reason is worth stating.
 * read_delimited resolves every escape to a byte, and `\1` does not denote a
 * byte — it denotes "whatever was there". Resolving it to some sentinel byte
 * would make a replacement mean something different depending on what the
 * user happened to type, so it is kept as a break BETWEEN literal runs
 * instead, which nothing typed can imitate.
 *
 * requires: `line` is NUL-terminated and `position` indexes the byte after
 *           an opening delimiter; *replacement writable.
 * ensures:  replacement(replacement, runs) where the runs are the text up to
 *           the next unescaped delimiter — or to the end of the line when
 *           the delimiter is NUL — split at every `\1`, with `\n`, `\t` and
 *           `\\` resolved and any other `\X` standing for X; the result is
 *           the position past the closing delimiter when there was one. Or
 *           the text or its runs did not fit, and the result is
 *           PARSE_FAILED.
 */
static size_t read_replacement(const char *line, size_t position,
                               char delimiter, Replacement *replacement)
{
    size_t        at;
    size_t        written;
    size_t        begin;
    uint32_t      runs;
    char          value;
    char          escaped;
    unsigned char byte;
    int           quote;

    at = position;
    written = 0;
    runs = 1;
    replacement->run_start[0] = 0;
    replacement->run_length[0] = 0;

    value = line[at];
    while (value != '\0') {
        if (value == delimiter) {
            begin = replacement->run_start[runs - 1];
            replacement->run_length[runs - 1] = written - begin;
            replacement->run_count = runs;
            return at + 1;
        }

        quote = 0;
        byte = (unsigned char)value;

        if (value == '\\') {
            escaped = line[at + 1];
            if (escaped == 'n') {
                byte = '\n';
                at = at + 2;
            } else if (escaped == 't') {
                byte = '\t';
                at = at + 2;
            } else if (escaped == '\\') {
                byte = '\\';
                at = at + 2;
            } else if (escaped == '1') {
                quote = 1;
                at = at + 2;
            } else if (escaped == '\0') {
                byte = '\\';
                at = at + 1;
            } else {
                byte = (unsigned char)escaped;
                at = at + 2;
            }
        } else {
            at = at + 1;
        }

        if (quote == 1) {
            if (runs >= REPLACEMENT_RUN_CAPACITY) {
                return PARSE_FAILED;
            }
            begin = replacement->run_start[runs - 1];
            replacement->run_length[runs - 1] = written - begin;
            replacement->run_start[runs] = written;
            replacement->run_length[runs] = 0;
            runs = runs + 1;
        } else {
            if (written >= REPLACEMENT_TEXT_CAPACITY) {
                return PARSE_FAILED;
            }
            replacement->text[written] = byte;
            written = written + 1;
        }

        value = line[at];
    }

    begin = replacement->run_start[runs - 1];
    replacement->run_length[runs - 1] = written - begin;
    replacement->run_count = runs;
    return at;
}

/* requires: `line` is NUL-terminated; `position` indexes into it;
 *           *command writable.
 * ensures:  command->address and command->addressed describe whatever
 *           address the line opened with, and the result is the position
 *           past it; or the address was malformed and the result is
 *           PARSE_FAILED.
 */
static size_t parse_address(const char *line, size_t position,
                            Command *command)
{
    size_t   at;
    size_t   first;
    size_t   second;
    size_t   next;
    size_t   number_slot;
    size_t   length_slot;
    size_t   captured;
    char     value;
    char     following;
    int      ok;

    at = position;
    value = line[at];

    command->addressed = 0;
    command->address.kind = ADDRESS_CURRENT;
    command->address.first = 0;
    command->address.last = 0;
    command->address.pattern = NULL;
    command->address.pattern_length = 0;

    if (value == 0x25) {                      /* %  the whole buffer */
        command->address.kind = ADDRESS_ALL;
        command->addressed = 1;
        return at + 1;
    }

    if (value == 0x2E) {                      /* .  the current line */
        command->address.kind = ADDRESS_CURRENT;
        command->addressed = 1;
        return at + 1;
    }

    if (value == 0x24) {                      /* $  the last line */
        command->address.kind = ADDRESS_LAST;
        command->addressed = 1;
        return at + 1;
    }

    if (value == 0x2F) {                      /* /text/  every match */
        at = at + 1;
        at = read_delimited(line, at, 0x2F, command->pattern,
                            PATTERN_CAPACITY, &length_slot);
        if (at == PARSE_FAILED) {
            return PARSE_FAILED;
        }
        captured = length_slot;
        command->address.kind = ADDRESS_MATCH;
        command->address.pattern = command->pattern;
        command->address.pattern_length = captured;
        command->addressed = 1;
        return at;
    }

    if (value == 0x7B) {                      /* {sel}  every such node */
        at = at + 1;
        at = read_delimited(line, at, 0x7D, command->pattern,
                            PATTERN_CAPACITY, &length_slot);
        if (at == PARSE_FAILED) {
            return PARSE_FAILED;
        }
        captured = length_slot;
        command->address.kind = ADDRESS_STRUCTURE;
        command->address.pattern = command->pattern;
        command->address.pattern_length = captured;
        command->addressed = 1;
        return at;
    }

    if (value == 0x67) {                      /* g/text/  matching lines */
        following = line[at + 1];
        if (following == 0x2F) {
            at = at + 2;
            at = read_delimited(line, at, 0x2F, command->pattern,
                                PATTERN_CAPACITY, &length_slot);
            if (at == PARSE_FAILED) {
                return PARSE_FAILED;
            }
            captured = length_slot;
            command->address.kind = ADDRESS_LINES_MATCHING;
            command->address.pattern = command->pattern;
            command->address.pattern_length = captured;
            command->addressed = 1;
            return at;
        }
    }

    if (value == 0x23) {                      /* #N or #N,#M  byte offsets */
        at = at + 1;
        next = read_number(line, at, &number_slot);
        if (next == at) {
            return PARSE_FAILED;
        }
        at = next;
        first = number_slot;

        value = line[at];
        if (value == 0x2C) {
            at = at + 1;
            value = line[at];
            if (value == 0x23) {
                at = at + 1;
            }
            next = read_number(line, at, &number_slot);
            if (next == at) {
                return PARSE_FAILED;
            }
            at = next;
            second = number_slot;
            command->address.kind = ADDRESS_BYTE_RANGE;
            command->address.first = first;
            command->address.last = second;
        } else {
            command->address.kind = ADDRESS_BYTE;
            command->address.first = first;
        }
        command->addressed = 1;
        return at;
    }

    ok = is_digit(value);
    if (ok == 1) {
        next = read_number(line, at, &number_slot);
        if (next == at) {
            return PARSE_FAILED;
        }
        at = next;
        first = number_slot;

        value = line[at];
        if (value == 0x2C) {
            at = at + 1;
            value = line[at];
            if (value == 0x24) {
                at = at + 1;
                command->address.kind = ADDRESS_LINE_RANGE;
                command->address.first = first;
                command->address.last = LINE_RANGE_OPEN;
            } else {
                next = read_number(line, at, &number_slot);
                if (next == at) {
                    return PARSE_FAILED;
                }
                at = next;
                second = number_slot;
                command->address.kind = ADDRESS_LINE_RANGE;
                command->address.first = first;
                command->address.last = second;
            }
        } else {
            command->address.kind = ADDRESS_LINE;
            command->address.first = first;
        }
        command->addressed = 1;
        return at;
    }

    return at;
}

/* The one address form whose meaning is not a function of the bytes: it
 * needs the buffer's grammar, so it is resolved here rather than in
 * address.c. See the note on ADDRESS_STRUCTURE in address.h.
 *
 * requires: editor(editor); the command's address is ADDRESS_STRUCTURE;
 *           *decomposition writable.
 * ensures:  editor(editor); decomposition(decomposition, spans, bytes) where
 *           spans are the extents the selector names, and the result is 1;
 *           or there is no grammar, the selector was malformed, the buffer
 *           would not parse, or nothing matched, and the result is 0. Every
 *           failure writes its own reason — which is why the caller does not
 *           add one.
 */
static int resolve_structure(Editor *editor, const Command *command,
                             Decomposition *decomposition)
{
    char       selector[PATTERN_CAPACITY];
    Structure *loaded;
    size_t     length;
    int        found;

    decomposition->count = 0;

    length = command->address.pattern_length;
    if (length == 0) {
        write_line("?  {} wants a selector");
        return 0;
    }
    if (length >= PATTERN_CAPACITY) {
        write_line("?  selector too long");
        return 0;
    }
    memcpy(selector, command->pattern, length);
    selector[length] = '\0';

    loaded = grammar_for_buffer(editor);
    if (loaded == NULL) {
        return 0;
    }

    found = structure_select(loaded, &editor->text, selector, decomposition);
    if (found == -2) {
        write_line("?  that grammar ships no queries/tags.scm; "
                   "select by node type instead");
        return 0;
    }
    if (found < 0) {
        write_line("?  could not parse the buffer");
        return 0;
    }
    if (found == 0) {
        write_line("?  no match");
        return 0;
    }
    return 1;
}

/* requires: editor(editor); *decomposition writable.
 * ensures:  editor(editor); *decomposition holds the foci the command's
 *           address selects, with ADDRESS_LINE_RANGE's open end resolved
 *           against the buffer, and the result is 1; or 0.
 */
static int resolve(Editor *editor, Command *command,
                   Decomposition *decomposition)
{
    AddressKind kind;
    size_t      last;
    size_t      newlines;
    size_t      total;
    size_t      begin;
    size_t      line_total;
    size_t      current;
    size_t      begin_slot;
    int         ok;

    kind = command->address.kind;
    last = command->address.last;

    if (kind == ADDRESS_STRUCTURE) {
        ok = resolve_structure(editor, command, decomposition);
        return ok;
    }

    if (kind == ADDRESS_LINE_RANGE) {
        if (last == LINE_RANGE_OPEN) {
            total = editor->text.byte_count;
            newlines = line_index_newline_count(&editor->index);
            line_total = newlines;
            if (total > 0) {
                ok = line_index_line_start(&editor->index, &editor->text,
                                           newlines, &begin_slot);
                if (ok == 1) {
                    begin = begin_slot;
                    if (begin < total) {
                        line_total = newlines + 1;
                    }
                }
            }
            command->address.last = line_total;
        }
    }

    current = editor->current_line;
    ok = address_resolve(&editor->text, &editor->index, &command->address,
                         current,
                         decomposition);
    return ok;
}

/* requires: editor(editor); decomposition(decomposition, spans, bytes).
 * ensures:  editor(editor); every focus is written to standard output. When
 *           `numbered`, every LINE within each focus is prefixed with its
 *           number — a focus may span many lines, as `%` does, and numbering
 *           only its first would be useless.
 */
static void print_foci(const Editor *editor,
                       const Decomposition *decomposition, int numbered)
{
    unsigned char window[OUTPUT_WINDOW];
    char          label[32];
    uint32_t      count;
    size_t        index;
    size_t        start;
    size_t        end;
    size_t        position;
    size_t        span;
    uint32_t      cursor;
    uint32_t      run;
    size_t        line;
    size_t        line_slot;
    unsigned char value;
    FILE         *stream;
    int           ok;
    int           want_label;
    int           ended_line;

    stream = stdout;
    count = decomposition->count;
    index = 0;
    while (index < count) {
        start = decomposition->focus[index].start;
        end = decomposition->focus[index].end;

        line = 0;
        if (numbered == 1) {
            ok = line_index_line_of_offset(&editor->index, &editor->text,
                                           start, &line_slot);
            if (ok == 1) {
                line = line_slot;
            }
        }
        want_label = numbered;

        position = start;
        while (position < end) {
            span = end - position;
            if (span > OUTPUT_WINDOW) {
                span = OUTPUT_WINDOW;
            }
            ok = rope_copy_range(&editor->text, position, span, window);
            if (ok == 0) {
                return;
            }

            cursor = 0;
            while (cursor < span) {
                if (want_label == 1) {
                    snprintf(label, sizeof(label), "%u\t", line + 1);
                    fputs(label, stream);
                    want_label = 0;
                }

                /* Write up to and including the next newline, so the label
                 * for the following line lands in the right place. */
                run = 0;
                ended_line = 0;
                while (cursor + run < span) {
                    value = window[cursor + run];
                    run = run + 1;
                    if (value == 0x0A) {
                        ended_line = 1;
                        break;
                    }
                }

                write_bytes(window + cursor, run);
                cursor = cursor + run;

                if (ended_line == 1) {
                    line = line + 1;
                    want_label = numbered;
                }
            }
            position = position + span;
        }
        index = index + 1;
    }
}

/* requires: editor(editor); decomposition(decomposition, spans, bytes).
 * ensures:  editor(editor); one line per focus giving its byte extent and
 *           line number is written to standard output.
 */
static void print_extents(const Editor *editor,
                          const Decomposition *decomposition)
{
    char     label[128];
    size_t   count;
    size_t   index;
    size_t   start;
    size_t   end;
    size_t   line;
    size_t   line_slot;
    FILE    *stream;
    int      ok;

    count = decomposition->count;
    stream = stdout;
    index = 0;
    while (index < count) {
        start = decomposition->focus[index].start;
        end = decomposition->focus[index].end;
        line = 0;
        ok = line_index_line_of_offset(&editor->index, &editor->text,
                                       start, &line_slot);
        if (ok == 1) {
            line = line_slot;
        }
        snprintf(label, sizeof(label), "#%u,#%u\tline %u\n", start, end,
                 line + 1);
        fputs(label, stream);
        index = index + 1;
    }
    if (count == 0) {
        fputs("no match\n", stream);
    }
}

/* Compose an address with a match traversal: the foci of `outer` restricted
 * to occurrences of a pattern inside them. This is what makes `s` an
 * ordinary change rather than a special form.
 *
 * requires: editor(editor); decomposition(outer, spans, bytes); holds a read
 *           share of the pattern; *inner writable.
 * ensures:  editor(editor); *inner holds every occurrence of the pattern
 *           lying wholly within a focus of `outer`, ascending, and the
 *           result is 1; or there were too many and the result is 0.
 */
static int compose_matches(const Editor *editor, const Decomposition *outer,
                           const unsigned char *pattern,
                           size_t pattern_length, Decomposition *inner)
{
    size_t   count;
    size_t   index;
    size_t   start;
    size_t   end;
    size_t   position;
    size_t   at;
    size_t   at_slot;
    uint32_t written;
    int      ok;

    inner->count = 0;
    if (pattern_length == 0) {
        return 0;
    }

    count = outer->count;
    index = 0;
    while (index < count) {
        start = outer->focus[index].start;
        end = outer->focus[index].end;

        position = start;
        while (position < end) {
            ok = address_find_literal(&editor->text, position, pattern,
                                      pattern_length, &at_slot);
            if (ok == 0) {
                position = end;
            } else {
                at = at_slot;
                if (at + pattern_length > end) {
                    position = end;
                } else {
                    written = inner->count;
                    if (written >= DECOMPOSITION_CAPACITY) {
                        return 0;
                    }
                    inner->focus[written].start = at;
                    inner->focus[written].end = at + pattern_length;
                    inner->count = written + 1;
                    position = at + pattern_length;
                }
            }
        }
        index = index + 1;
    }
    return 1;
}

/* requires: editor(editor); rope(replacement, bytes', share').
 * ensures:  editor(editor) whose buffer is now that rope, marked modified,
 *           with current_line clamped into it.
 */
/* Take a new version as the current buffer, and carry the line index
 * forward with it.
 *
 * [start, end) is the region of the OLD buffer the edit touched, and the
 * whole length change falls inside it, so everything outside is unchanged
 * and the index only has to be told about that one region. Passing
 * REBUILD_INDEX as `start` says the buffer was replaced wholesale and
 * the index must be built from scratch.
 *
 * requires: editor(editor); rope(replacement, bytes, share).
 * ensures:  editor(editor) whose buffer is that rope, whose index describes
 *           it, marked modified, with current_line clamped into it and the
 *           version recorded in the history.
 */
static void adopt(Editor *editor, const Rope *replacement, size_t start,
                  size_t end, size_t inserted)
{
    LineIndex derived;
    uint32_t serial;
    int      built;
    size_t   total;
    size_t   newlines;
    size_t   lines;
    size_t   begin;
    size_t   current;
    size_t   begin_slot;
    int      ok;

    /* Derive the new index BEFORE the rope is swapped in, because the
     * update reads the old index and the new rope. */
    line_index_initialize(&derived);
    if (start == REBUILD_INDEX) {
        built = line_index_build(&editor->pool, replacement, &derived);
    } else {
        built = line_index_update(&editor->pool, &editor->index, replacement,
                                  start, end, inserted, &derived);
    }
    if (built == 0) {
        /* Out of memory for the index. Rebuilding is the only honest
         * fallback, and if that fails too the buffer is left alone. */
        built = line_index_build(&editor->pool, replacement, &derived);
        if (built == 0) {
            return;
        }
    }

    memcpy(&editor->text, replacement, sizeof(Rope));
    memcpy(&editor->index, &derived, sizeof(LineIndex));
    editor->modified = 1;

    /* Every version gets a number, which is how a job that lands late can
     * say how far the buffer has moved since it was launched. */
    serial = editor->serial;
    editor->serial = serial + 1;

    /* The new version joins the history, which retires the oldest when the
     * window is full and hands its unshared nodes back to the pool. */
    history_push(&editor->history, &editor->pool, replacement);

    total = editor->text.byte_count;
    newlines = line_index_newline_count(&editor->index);
    lines = newlines;
    if (total > 0) {
        ok = line_index_line_start(&editor->index, &editor->text, newlines,
                                   &begin_slot);
        if (ok == 1) {
            begin = begin_slot;
            if (begin < total) {
                lines = newlines + 1;
            }
        }
    }
    if (lines == 0) {
        editor->current_line = 0;
        return;
    }
    current = editor->current_line;
    if (current == 0) {
        editor->current_line = 1;
        return;
    }
    if (current > lines) {
        editor->current_line = lines;
    }
}

/* requires: editor(editor); `path` is NUL-terminated.
 * ensures:  editor(editor); the buffer's bytes are written to that file and
 *           the result is 1; or the file could not be written and the result
 *           is 0.
 */
static int write_to_file(Editor *editor, const char *path)
{
    unsigned char window[OUTPUT_WINDOW];
    FILE         *file;
    size_t        total;
    size_t        position;
    size_t        span;
    size_t        written;
    int           ok;

    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }

    total = editor->text.byte_count;
    position = 0;
    while (position < total) {
        span = total - position;
        if (span > OUTPUT_WINDOW) {
            span = OUTPUT_WINDOW;
        }
        ok = rope_copy_range(&editor->text, position, span, window);
        if (ok == 0) {
            fclose(file);
            return 0;
        }
        written = fwrite(window, 1, span, file);
        if (written != span) {
            fclose(file);
            return 0;
        }
        position = position + span;
    }

    fclose(file);
    editor->modified = 0;
    return 1;
}

/* requires: as command.h.
 * ensures:  as command.h.
 */
int editor_load(Editor *editor, const char *path)
{
    FILE          *file;
    unsigned char *contents;
    long           size;
    size_t         read_count;
    Rope           loaded;
    int            ok;

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size < 0) {
        fclose(file);
        return 0;
    }

    contents = malloc((size_t)size + 1);
    if (contents == NULL) {
        fclose(file);
        return 0;
    }

    read_count = fread(contents, 1, (size_t)size, file);
    fclose(file);

    ok = rope_from_bytes(&editor->pool, contents, (uint32_t)read_count,
                         &loaded);
    free(contents);
    if (ok == 0) {
        return 0;
    }

    editor->current_line = 0;
    snprintf(editor->name, NAME_CAPACITY, "%s", path);

    adopt(editor, &loaded, REBUILD_INDEX, 0, 0);
    editor->modified = 0;
    return 1;
}

/* requires: *editor is allocated and writable.
 * ensures:  as command.h.
 */
int editor_initialize(Editor *editor)
{
    size_t   index;
    int      ok;

    ok = pool_initialize(&editor->pool);
    if (ok == 0) {
        return 0;
    }
    history_initialize(&editor->history);
    rope_initialize_empty(&editor->text);
    line_index_initialize(&editor->index);
    line_index_initialize(&editor->index);
    editor->loop = NULL;
    grammar_initialize(&editor->grammars);
    /* The environment is read once, here, rather than on first use: a `G`
     * with no argument should be able to show what the session started
     * with, and a registration made by hand should not be undone by a
     * later first use re-reading the environment on top of it. */
    grammar_configure_from_environment(&editor->grammars);
    editor->current = 0;
    index = 0;
    while (index < BUFFER_CAPACITY) {
        editor->slots[index].used = 0;
        rope_initialize_empty(&editor->slots[index].text);
        line_index_initialize(&editor->slots[index].index);
        line_index_initialize(&editor->slots[index].index);
        line_index_initialize(&editor->slots[index].index);
        history_initialize(&editor->slots[index].history);
        editor->slots[index].name[0] = 0x00;
        index = index + 1;
    }
    editor->slots[0].used = 1;
    editor->next_job_id = 1;
    editor->serial = 0;
    index = 0;
    while (index < JOB_CAPACITY) {
        editor->jobs[index].kind = JOB_IDLE;
        editor->jobs[index].buffer = NULL;
        editor->jobs[index].descriptor = -1;
        index = index + 1;
    }
    editor->current_line = 0;
    editor->modified = 0;
    editor->quit = 0;
    editor->name[0] = 0x00;
    return 1;
}

/* requires: editor(editor).
 * ensures:  as command.h.
 */
void editor_release(Editor *editor)
{
    size_t         index;
    unsigned char *buffer;
    int            descriptor;
    JobKind        kind;

    index = 0;
    while (index < JOB_CAPACITY) {
        kind = editor->jobs[index].kind;
        if (kind != JOB_IDLE) {
            buffer = editor->jobs[index].buffer;
            free(buffer);
            descriptor = editor->jobs[index].descriptor;
            io_close(descriptor);
            editor->jobs[index].kind = JOB_IDLE;
        }
        index = index + 1;
    }
    grammar_release(&editor->grammars);
    pool_release(&editor->pool);
    rope_initialize_empty(&editor->text);
}

/* requires: editor(editor); `line` is NUL-terminated.
 * ensures:  as command.h.
 */
int editor_execute(Editor *editor, const char *line)
{
    Command       command;
    Replacement   replacement;
    Decomposition foci;
    Decomposition matches;
    Rope          edited;
    AddressKind   kind;
    char          path[NAME_CAPACITY];
    size_t        position;
    size_t        replacement_length;
    size_t        length_slot;
    size_t        pattern_length;
    uint32_t      count;
    size_t        index;
    size_t        span_start;
    size_t        span_end;
    size_t        next;
    size_t        previous;
    char          verb;
    char          value;
    int           ok;
    int           addressed;

    position = 0;
    position = skip_blanks(line, position);

    value = line[position];
    if (value == 0x00) {
        return 1;
    }

    /* @N runs the rest of the line against another buffer and comes back.
     * This is what makes several files visible in one transcript without
     * losing your place in any of them. */
    if (value == 0x40) {
        position = position + 1;
        next = read_number(line, position, &length_slot);
        if (next == position) {
            write_line("?  @ needs a buffer number");
            return 0;
        }
        position = next;
        index = length_slot;
        previous = editor->current;
        ok = editor_select_buffer(editor, index);
        if (ok == 0) {
            write_line("?  no such buffer");
            return 0;
        }
        position = skip_blanks(line, position);
        ok = editor_execute(editor, line + position);
        editor_select_buffer(editor, previous);
        return ok;
    }

    position = parse_address(line, position, &command);
    if (position == PARSE_FAILED) {
        write_line("?  malformed address");
        return 0;
    }

    position = skip_blanks(line, position);
    verb = line[position];
    if (verb == '\0') {
        verb = 'p';
    } else {
        position = position + 1;
    }

    if (verb == 0x26) {                       /* &  start or list jobs */
        position = skip_blanks(line, position);
        value = line[position];
        if (value == 0x00) {
            editor_report_jobs(editor);
            return 1;
        }
        position = position + 1;
        position = skip_blanks(line, position);
        snprintf(path, NAME_CAPACITY, "%s", line + position);
        if (value == 0x65) {                  /* &e path  load in the background */
            ok = start_job(editor, JOB_LOAD, path);
            return ok;
        }
        if (value == 0x63) {                  /* &c path  count in the background */
            ok = start_job(editor, JOB_COUNT, path);
            return ok;
        }
        write_line("?  unknown job");
        return 0;
    }

    if (verb == 0x46) {                       /* F  the definitions here */
        ok = editor_list_definitions(editor);
        return ok;
    }

    if (verb == 0x47) {                       /* G  list or register grammars */
        position = skip_blanks(line, position);
        value = line[position];
        if (value == 0x00) {
            editor_report_grammars(editor);
            return 1;
        }
        ok = editor_register_grammar(editor, line + position);
        return ok;
    }

    if (verb == 0x62) {                       /* b  list or switch buffers */
        position = skip_blanks(line, position);
        value = line[position];
        if (value == 0x00) {
            editor_report_buffers(editor);
            return 1;
        }
        next = read_number(line, position, &length_slot);
        if (next == position) {
            write_line("?  b needs a buffer number");
            return 0;
        }
        index = length_slot;
        ok = editor_select_buffer(editor, index);
        if (ok == 0) {
            write_line("?  no such buffer");
            return 0;
        }
        return 1;
    }

    if (verb == 0x45) {                       /* E  load into a new buffer */
        position = skip_blanks(line, position);
        snprintf(path, NAME_CAPACITY, "%s", line + position);
        index = editor_free_buffer(editor);
        if (index == BUFFER_CAPACITY) {
            write_line("?  every buffer is in use");
            return 0;
        }
        editor_select_buffer(editor, index);
        ok = editor_load(editor, path);
        if (ok == 0) {
            write_line("?  cannot read that file");
            return 0;
        }
        return 1;
    }

    if (verb == 0x71) {                       /* q */
        editor->quit = 1;
        return 1;
    }

    if (verb == 'e') {
        position = skip_blanks(line, position);
        snprintf(path, NAME_CAPACITY, "%s", line + position);
        ok = editor_load(editor, path);
        if (ok == 0) {
            write_line("?  cannot read that file");
            return 0;
        }
        return 1;
    }

    if (verb == 'w') {
        position = skip_blanks(line, position);
        value = line[position];
        if (value == '\0') {
            snprintf(path, NAME_CAPACITY, "%s", editor->name);
        } else {
            snprintf(path, NAME_CAPACITY, "%s", line + position);
        }
        value = path[0];
        if (value == '\0') {
            write_line("?  no file name");
            return 0;
        }
        ok = write_to_file(editor, path);
        if (ok == 0) {
            write_line("?  cannot write that file");
            return 0;
        }
        return 1;
    }

    /* Everything below addresses the buffer. An unaddressed command acts on
     * the current line, as ed does. */
    addressed = command.addressed;
    if (addressed == 0) {
        if (verb == 'a') {
            command.address.kind = ADDRESS_CURRENT;
        }
    }

    kind = command.address.kind;
    ok = resolve(editor, &command, &foci);
    if (ok == 0) {
        /* A structural address has already said why it failed — it has more
         * to say than "no such address", since it can also mean there is no
         * grammar for this buffer. */
        if (kind != ADDRESS_STRUCTURE) {
            write_line("?  no such address");
        }
        return 0;
    }

    if (verb == 'p') {
        print_foci(editor, &foci, 0);
        return 1;
    }
    if (verb == 'n') {
        print_foci(editor, &foci, 1);
        return 1;
    }
    if (verb == 0x3D) {                       /* =  extents */
        print_extents(editor, &foci);
        return 1;
    }

    if (verb == 0x78) {                       /* x  hex */
        count = foci.count;
        index = 0;
        while (index < count) {
            span_start = foci.focus[index].start;
            span_end = foci.focus[index].end;
            view_write_hex(&editor->text, span_start, span_end);
            index = index + 1;
        }
        return 1;
    }

    if (verb == 0x75) {                       /* u  code points */
        count = foci.count;
        index = 0;
        while (index < count) {
            span_start = foci.focus[index].start;
            span_end = foci.focus[index].end;
            view_write_codepoints(&editor->text, span_start, span_end);
            index = index + 1;
        }
        return 1;
    }

    if (verb == 'd') {
        ok = address_replace_all(&editor->pool, &editor->text, &foci, NULL, 0,
                                 &edited);
        if (ok == 0) {
            write_line("?  out of memory");
            return 0;
        }
        adopt_edit(editor, &edited, &foci);
        return 1;
    }

    if (verb == 'c') {
        position = skip_blanks(line, position);
        position = read_replacement(line, position, 0x00, &replacement);
        if (position == PARSE_FAILED) {
            write_line("?  replacement too long");
            return 0;
        }
        ok = address_replace_each(&editor->pool, &editor->text, &foci,
                                  &replacement, &edited);
        if (ok == 0) {
            write_line("?  out of memory");
            return 0;
        }
        adopt_edit(editor, &edited, &foci);
        return 1;
    }

    if (verb == 'i' || verb == 'a') {
        position = skip_blanks(line, position);
        position = read_delimited(line, position, 0x00, command.operand,
                                  ARGUMENT_CAPACITY, &length_slot);
        if (position == PARSE_FAILED) {
            write_line("?  insertion too long");
            return 0;
        }
        replacement_length = length_slot;
        ok = 0;
        if (verb == 'i') {
            ok = address_insert_all(&editor->pool, &editor->text, &foci, 1,
                                    command.operand, replacement_length,
                                    &edited);
        } else {
            ok = address_insert_all(&editor->pool, &editor->text, &foci, 0,
                                    command.operand, replacement_length,
                                    &edited);
        }
        if (ok == 0) {
            write_line("?  out of memory");
            return 0;
        }
        adopt_edit(editor, &edited, &foci);
        return 1;
    }

    if (verb == 's') {
        value = line[position];
        if (value == '\0') {
            write_line("?  s needs a pattern");
            return 0;
        }
        position = position + 1;
        position = read_delimited(line, position, value, command.pattern,
                                  PATTERN_CAPACITY, &length_slot);
        if (position == PARSE_FAILED) {
            write_line("?  pattern too long");
            return 0;
        }
        pattern_length = length_slot;

        position = read_replacement(line, position, value, &replacement);
        if (position == PARSE_FAILED) {
            write_line("?  replacement too long");
            return 0;
        }
        ok = compose_matches(editor, &foci, command.pattern, pattern_length,
                             &matches);
        if (ok == 0) {
            write_line("?  too many matches");
            return 0;
        }
        count = matches.count;
        if (count == 0) {
            write_line("?  no match");
            return 0;
        }

        ok = address_replace_each(&editor->pool, &editor->text, &matches,
                                  &replacement, &edited);
        if (ok == 0) {
            write_line("?  out of memory");
            return 0;
        }
        adopt_edit(editor, &edited, &foci);
        return 1;
    }

    write_line("?  unknown command");
    return 0;
}

/* requires: as command.h.
 * ensures:  as command.h.
 */
void editor_attach_loop(Editor *editor, IoLoop *loop)
{
    editor->loop = loop;
}

/* requires: editor(editor).
 * ensures:  editor(editor); the result is the index of an idle job slot, or
 *           JOB_CAPACITY when every slot is busy. No memory is written.
 */
static size_t idle_slot(const Editor *editor)
{
    size_t   index;
    JobKind  kind;

    index = 0;
    while (index < JOB_CAPACITY) {
        kind = editor->jobs[index].kind;
        if (kind == JOB_IDLE) {
            return index;
        }
        index = index + 1;
    }
    return JOB_CAPACITY;
}

/* requires: editor(editor); `path` is NUL-terminated.
 * ensures:  editor(editor) with one more job in flight, a line naming it
 *           written, and the result 1; or the job could not be started, a
 *           reason written, and the result 0.
 */
static int start_job(Editor *editor, JobKind kind, const char *path)
{
    char           label[NAME_CAPACITY + 64];
    IoLoop        *loop;
    unsigned char *buffer;
    int64_t        size;
    int64_t        length;
    int            descriptor;
    size_t         slot;
    uint32_t       identifier;
    uint32_t       serial;
    uint64_t       token;
    FILE          *stream;
    int            ok;

    loop = editor->loop;
    if (loop == NULL) {
        write_line("?  no event loop; jobs need one");
        return 0;
    }

    slot = idle_slot(editor);
    if (slot == JOB_CAPACITY) {
        write_line("?  too many jobs in flight");
        return 0;
    }

    size = 0;
    descriptor = io_open_for_read(path, &size);
    if (descriptor < 0) {
        write_line("?  cannot open that file");
        return 0;
    }
    /* `size` is addressable, so every later mention of it would be a load.
     * Take it into a register once. */
    length = size;
    if (length < 0) {
        io_close(descriptor);
        return 0;
    }

    buffer = malloc((size_t)length + 1);
    if (buffer == NULL) {
        io_close(descriptor);
        write_line("?  out of memory");
        return 0;
    }

    identifier = editor->next_job_id;
    editor->next_job_id = identifier + 1;

    editor->jobs[slot].kind = kind;
    editor->jobs[slot].id = identifier;
    editor->jobs[slot].descriptor = descriptor;
    editor->jobs[slot].buffer = buffer;
    editor->jobs[slot].capacity = (uint32_t)length;
    serial = editor->serial;
    editor->jobs[slot].launched_at = serial;
    snprintf(editor->jobs[slot].path, NAME_CAPACITY, "%s", path);

    token = (uint64_t)identifier + 1u;
    ok = io_submit_read(loop, token, descriptor, buffer, (uint32_t)length, 0);
    if (ok == 0) {
        free(buffer);
        io_close(descriptor);
        editor->jobs[slot].kind = JOB_IDLE;
        write_line("?  could not submit the read");
        return 0;
    }

    stream = stdout;
    snprintf(label, sizeof(label), "[%u] reading %s\n", identifier, path);
    fputs(label, stream);
    return 1;
}

/* requires: editor(editor).
 * ensures:  editor(editor); the slot's buffer and descriptor are released
 *           and it is marked idle.
 */
static void retire_job(Editor *editor, uint32_t slot)
{
    unsigned char *buffer;
    int            descriptor;

    buffer = editor->jobs[slot].buffer;
    free(buffer);
    descriptor = editor->jobs[slot].descriptor;
    io_close(descriptor);

    editor->jobs[slot].kind = JOB_IDLE;
    editor->jobs[slot].buffer = NULL;
    editor->jobs[slot].descriptor = -1;
}

/* requires: as command.h.
 * ensures:  as command.h.
 */
int editor_complete(Editor *editor, uint64_t token, int32_t result)
{
    char     label[NAME_CAPACITY + 160];
    Rope     loaded;
    FILE    *stream;
    uint32_t slot;
    uint32_t identifier;
    uint32_t launched;
    uint32_t now;
    uint32_t moved;
    size_t   lines;
    size_t   index;
    size_t   length;
    unsigned char *buffer;
    unsigned char  value;
    JobKind  kind;
    int      ok;

    if (token == IO_TOKEN_INPUT) {
        return 0;
    }
    identifier = (uint32_t)(token - 1u);

    slot = 0;
    while (slot < JOB_CAPACITY) {
        kind = editor->jobs[slot].kind;
        if (kind != JOB_IDLE) {
            now = editor->jobs[slot].id;
            if (now == identifier) {
                break;
            }
        }
        slot = slot + 1;
    }
    if (slot == JOB_CAPACITY) {
        return 0;
    }

    stream = stdout;
    kind = editor->jobs[slot].kind;
    launched = editor->jobs[slot].launched_at;
    now = editor->serial;
    moved = now - launched;

    if (result < 0) {
        snprintf(label, sizeof(label), "[%u] read failed\n", identifier);
        fputs(label, stream);
        retire_job(editor, slot);
        return 1;
    }

    length = (uint32_t)result;
    buffer = editor->jobs[slot].buffer;

    if (kind == JOB_COUNT) {
        lines = 0;
        index = 0;
        while (index < length) {
            value = buffer[index];
            if (value == 0x0A) {
                lines = lines + 1;
            }
            index = index + 1;
        }
        snprintf(label, sizeof(label),
                 "[%u] %s: %u bytes, %u lines  (launched at version %u,"
                 " now %u)\n",
                 identifier, editor->jobs[slot].path, length, lines,
                 launched, now);
        fputs(label, stream);
        retire_job(editor, slot);
        return 1;
    }

    /* A load races the buffer. The version stamp is what makes the race
     * visible instead of silent: if anything was edited since the read was
     * launched, the result is stale and applying it would throw that work
     * away. */
    if (moved > 0) {
        snprintf(label, sizeof(label),
                 "[%u] %s read, but the buffer moved on %u version(s) since;"
                 " discarded\n",
                 identifier, editor->jobs[slot].path, moved);
        fputs(label, stream);
        retire_job(editor, slot);
        return 1;
    }

    ok = rope_from_bytes(&editor->pool, buffer, length, &loaded);
    if (ok == 0) {
        snprintf(label, sizeof(label), "[%u] out of memory\n", identifier);
        fputs(label, stream);
        retire_job(editor, slot);
        return 1;
    }

    snprintf(editor->name, NAME_CAPACITY, "%s", editor->jobs[slot].path);
    editor->current_line = 0;
    adopt(editor, &loaded, REBUILD_INDEX, 0, 0);
    editor->modified = 0;

    snprintf(label, sizeof(label), "[%u] %s loaded, %u bytes\n", identifier,
             editor->name, length);
    fputs(label, stream);
    retire_job(editor, slot);
    return 1;
}

/* requires: as command.h.
 * ensures:  as command.h.
 */
void editor_report_jobs(const Editor *editor)
{
    char     label[NAME_CAPACITY + 64];
    FILE    *stream;
    uint32_t slot;
    uint32_t identifier;
    uint32_t launched;
    uint32_t shown;
    JobKind  kind;

    stream = stdout;
    shown = 0;
    slot = 0;
    while (slot < JOB_CAPACITY) {
        kind = editor->jobs[slot].kind;
        if (kind != JOB_IDLE) {
            identifier = editor->jobs[slot].id;
            launched = editor->jobs[slot].launched_at;
            snprintf(label, sizeof(label),
                     "[%u] %s  (launched at version %u)\n", identifier,
                     editor->jobs[slot].path, launched);
            fputs(label, stream);
            shown = shown + 1;
        }
        slot = slot + 1;
    }
    if (shown == 0) {
        write_line("no jobs in flight");
    }
}

/* requires: editor(editor).
 * ensures:  editor(editor) with the live fields copied into the slot they
 *           belong to, so another buffer can be loaded over them.
 */
static void save_current_buffer(Editor *editor)
{
    size_t   index;
    size_t   line_number;
    uint32_t serial;
    int      modified;

    index = editor->current;
    memcpy(&editor->slots[index].text, &editor->text, sizeof(Rope));
    memcpy(&editor->slots[index].index, &editor->index, sizeof(LineIndex));
    memcpy(&editor->slots[index].index, &editor->index, sizeof(LineIndex));
    memcpy(&editor->slots[index].history, &editor->history, sizeof(History));

    /* One dereference per statement: a field-to-field copy is a load and a
     * store, which is two. */
    line_number = editor->current_line;
    editor->slots[index].current_line = line_number;
    serial = editor->serial;
    editor->slots[index].serial = serial;
    modified = editor->modified;
    editor->slots[index].modified = modified;
    editor->slots[index].used = 1;
    snprintf(editor->slots[index].name, NAME_CAPACITY, "%s", editor->name);
}

/* requires: editor(editor); index < BUFFER_CAPACITY.
 * ensures:  editor(editor) whose live fields are that slot's, and `current`
 *           is index. The previous buffer must already have been saved.
 */
static void load_buffer(Editor *editor, size_t index)
{
    size_t   line_number;
    uint32_t serial;
    int      modified;
    int      used;

    used = editor->slots[index].used;
    if (used == 0) {
        rope_initialize_empty(&editor->slots[index].text);
        line_index_initialize(&editor->slots[index].index);
        history_initialize(&editor->slots[index].history);
        editor->slots[index].current_line = 0;
        editor->slots[index].serial = 0;
        editor->slots[index].modified = 0;
        editor->slots[index].name[0] = 0x00;
        editor->slots[index].used = 1;
    }

    memcpy(&editor->text, &editor->slots[index].text, sizeof(Rope));
    memcpy(&editor->index, &editor->slots[index].index, sizeof(LineIndex));
    memcpy(&editor->index, &editor->slots[index].index, sizeof(LineIndex));
    memcpy(&editor->history, &editor->slots[index].history, sizeof(History));

    line_number = editor->slots[index].current_line;
    editor->current_line = line_number;
    serial = editor->slots[index].serial;
    editor->serial = serial;
    modified = editor->slots[index].modified;
    editor->modified = modified;
    snprintf(editor->name, NAME_CAPACITY, "%s",
             editor->slots[index].name);
    editor->current = index;
}

/* requires: as command.h.
 * ensures:  as command.h.
 */
int editor_select_buffer(Editor *editor, size_t index)
{
    size_t   now;

    if (index >= BUFFER_CAPACITY) {
        return 0;
    }
    now = editor->current;
    if (now == index) {
        return 1;
    }
    save_current_buffer(editor);
    load_buffer(editor, index);
    return 1;
}

/* requires: as command.h.
 * ensures:  as command.h.
 */
void editor_report_buffers(Editor *editor)
{
    char     label[NAME_CAPACITY + 96];
    FILE    *stream;
    size_t   index;
    size_t   now;
    size_t   length;
    size_t   lines;
    int      used;
    int      modified;
    char     mark;

    save_current_buffer(editor);
    stream = stdout;
    now = editor->current;

    index = 0;
    while (index < BUFFER_CAPACITY) {
        used = editor->slots[index].used;
        if (used == 1) {
            mark = ' ';
            if (index == now) {
                mark = '*';
            }
            length = editor->slots[index].text.byte_count;
            lines = line_index_newline_count(&editor->slots[index].index);
            modified = editor->slots[index].modified;
            snprintf(label, sizeof(label), "%c%u\t%u bytes, %u lines%s\t%s\n",
                     mark, index, length, lines,
                     modified == 1 ? "  (modified)" : "",
                     editor->slots[index].name);
            fputs(label, stream);
        }
        index = index + 1;
    }
}

/* requires: as command.h.
 * ensures:  as command.h.
 */
size_t editor_free_buffer(const Editor *editor)
{
    size_t   index;
    int      used;

    index = 0;
    while (index < BUFFER_CAPACITY) {
        used = editor->slots[index].used;
        if (used == 0) {
            return index;
        }
        index = index + 1;
    }
    return BUFFER_CAPACITY;
}

/* The grammar this buffer's name asks for, loaded on first use.
 *
 * Loading is deferred because most sessions never ask for structure, and a
 * session that asks about one language should not pay for every grammar the
 * user has configured. Which grammar it is comes from the buffer's name, so
 * `@1 F` in a Python buffer and `F` in a C one are two different parsers and
 * neither has to be told.
 *
 * requires: editor(editor).
 * ensures:  editor(editor) with that grammar loaded, and the result points
 *           at it; or no registration governs this buffer's name, or its
 *           grammar would not load, a reason is written, and the result is
 *           null.
 */
static Structure *grammar_for_buffer(Editor *editor)
{
    char        label[NAME_CAPACITY + 64];
    Structure  *loaded;
    const char *name;
    const char *shown;
    size_t      count;
    size_t      index;
    char        value;

    name = editor->name;
    count = grammar_count(&editor->grammars);
    if (count == 0) {
        write_line("?  no grammar registered; see G, or set PAPRI_GRAMMARS");
        return NULL;
    }

    index = grammar_lookup(&editor->grammars, name);
    if (index == count) {
        shown = name;
        value = shown[0];
        if (value == '\0') {
            shown = "a buffer with no name";
        }
        snprintf(label, sizeof(label), "?  no grammar registered for %s",
                 shown);
        write_line(label);
        return NULL;
    }

    loaded = grammar_structure_at(&editor->grammars, index);
    if (loaded == NULL) {
        shown = grammar_directory_at(&editor->grammars, index);
        snprintf(label, sizeof(label), "?  the grammar at %s would not load",
                 shown);
        write_line(label);
        return NULL;
    }
    return loaded;
}

/* requires: as command.h.
 * ensures:  as command.h.
 */
void editor_report_grammars(const Editor *editor)
{
    char        label[GRAMMAR_PATH_CAPACITY + 128];
    const char *name;
    const char *suffix;
    const char *language;
    const char *directory;
    FILE       *stream;
    size_t      count;
    size_t      index;
    size_t      current;
    char        mark;

    stream = stdout;
    count = grammar_count(&editor->grammars);
    if (count == 0) {
        write_line("no grammar registered; see G, or set PAPRI_GRAMMARS");
        return;
    }

    name = editor->name;
    current = grammar_lookup(&editor->grammars, name);

    index = 0;
    while (index < count) {
        suffix = grammar_suffix_at(&editor->grammars, index);
        language = grammar_language_at(&editor->grammars, index);
        directory = grammar_directory_at(&editor->grammars, index);
        mark = ' ';
        if (index == current) {
            /* The one this buffer's name selects. */
            mark = '*';
        }
        snprintf(label, sizeof(label), "%c%s\t%s\t%s\n", mark, suffix,
                 language, directory);
        fputs(label, stream);
        index = index + 1;
    }
}

/* requires: as command.h.
 * ensures:  as command.h.
 */
int editor_register_grammar(Editor *editor, const char *text)
{
    int ok;

    ok = grammar_register_line(&editor->grammars, text);
    if (ok == 0) {
        write_line("?  G wants SUFFIX LANGUAGE DIRECTORY");
        return 0;
    }
    return 1;
}

/* requires: as command.h.
 * ensures:  as command.h.
 */
int editor_list_definitions(Editor *editor)
{
    Structure *loaded;
    int        found;

    loaded = grammar_for_buffer(editor);
    if (loaded == NULL) {
        return 0;
    }

    found = structure_list_definitions(loaded, &editor->text);
    if (found == -2) {
        write_line("?  that grammar ships no queries/tags.scm; "
                   "it has no notion of a definition");
        return 0;
    }
    if (found < 0) {
        write_line("?  could not parse the buffer");
        return 0;
    }
    if (found == 0) {
        write_line("no definitions found");
    }
    return 1;
}

/* Adopt an edited version, working out the region the edit touched.
 *
 * Every focus lies inside [first start, last end), and nothing outside that
 * region moved, so the whole length change falls within it. That is all the
 * index needs: one region, its old extent and its new one.
 *
 * requires: editor(editor); rope(edited, bytes', share);
 *           decomposition(foci, spans, bytes) resolved against the buffer
 *           `edited` was derived from, with at least one focus.
 * ensures:  editor(editor) advanced to that version, its index carried
 *           forward.
 */
static void adopt_edit(Editor *editor, const Rope *edited,
                       const Decomposition *foci)
{
    size_t count;
    size_t start;
    size_t end;
    size_t old_total;
    size_t new_total;
    size_t region;

    count = foci->count;
    if (count == 0) {
        adopt(editor, edited, REBUILD_INDEX, 0, 0);
        return;
    }

    start = foci->focus[0].start;
    end = foci->focus[count - 1].end;

    old_total = rope_byte_count(&editor->text);
    new_total = rope_byte_count(edited);

    region = end - start;
    if (new_total >= old_total) {
        region = region + (new_total - old_total);
    } else {
        region = region - (old_total - new_total);
    }

    adopt(editor, edited, start, end, region);
}
