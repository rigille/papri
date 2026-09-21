#include "command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARGUMENT_CAPACITY 8192
#define PATTERN_CAPACITY  1024
#define OUTPUT_WINDOW     4096

/* Returned by the parsing helpers when the text did not fit or was
 * malformed, and used as the open end of a `N,$` line range. */
#define PARSE_FAILED     0xFFFFFFFFu
#define LINE_RANGE_OPEN  0xFFFFFFFFu

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
static uint32_t skip_blanks(const char *line, uint32_t position)
{
    uint32_t at;
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
static uint32_t read_number(const char *line, uint32_t position,
                            uint32_t *value_slot)
{
    uint32_t at;
    uint32_t total;
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
static uint32_t read_delimited(const char *line, uint32_t position,
                               char delimiter, unsigned char *destination,
                               uint32_t capacity, uint32_t *length_slot)
{
    uint32_t at;
    uint32_t written;
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

/* requires: `line` is NUL-terminated; `position` indexes into it;
 *           *command writable.
 * ensures:  command->address and command->addressed describe whatever
 *           address the line opened with, and the result is the position
 *           past it; or the address was malformed and the result is
 *           PARSE_FAILED.
 */
static uint32_t parse_address(const char *line, uint32_t position,
                              Command *command)
{
    uint32_t at;
    uint32_t first;
    uint32_t second;
    uint32_t next;
    uint32_t number_slot;
    uint32_t length_slot;
    uint32_t captured;
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

    if (value == 0x24) {                      /*  last line */
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

/* requires: editor(editor); *decomposition writable.
 * ensures:  editor(editor); *decomposition holds the foci the command's
 *           address selects, with ADDRESS_LINE_RANGE's open end resolved
 *           against the buffer, and the result is 1; or 0.
 */
static int resolve(const Editor *editor, Command *command,
                   Decomposition *decomposition)
{
    AddressKind kind;
    uint32_t    last;
    uint32_t    newlines;
    uint32_t    total;
    uint32_t    begin;
    uint32_t    line_total;
    uint32_t    current;
    uint32_t    begin_slot;
    int         ok;

    kind = command->address.kind;
    last = command->address.last;

    if (kind == ADDRESS_LINE_RANGE) {
        if (last == LINE_RANGE_OPEN) {
            total = editor->text.byte_count;
            newlines = editor->text.newline_count;
            line_total = newlines;
            if (total > 0) {
                ok = rope_line_start(&editor->text, newlines, &begin_slot);
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
    ok = address_resolve(&editor->text, &command->address, current,
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
    uint32_t      index;
    uint32_t      start;
    uint32_t      end;
    uint32_t      position;
    uint32_t      span;
    uint32_t      cursor;
    uint32_t      run;
    uint32_t      line;
    uint32_t      line_slot;
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
            ok = rope_line_of_offset(&editor->text, start, &line_slot);
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
    uint32_t count;
    uint32_t index;
    uint32_t start;
    uint32_t end;
    uint32_t line;
    uint32_t line_slot;
    FILE    *stream;
    int      ok;

    count = decomposition->count;
    stream = stdout;
    index = 0;
    while (index < count) {
        start = decomposition->focus[index].start;
        end = decomposition->focus[index].end;
        line = 0;
        ok = rope_line_of_offset(&editor->text, start, &line_slot);
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
                           uint32_t pattern_length, Decomposition *inner)
{
    uint32_t count;
    uint32_t index;
    uint32_t start;
    uint32_t end;
    uint32_t position;
    uint32_t at;
    uint32_t at_slot;
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
static void adopt(Editor *editor, const Rope *replacement)
{
    uint32_t serial;
    uint32_t total;
    uint32_t newlines;
    uint32_t lines;
    uint32_t begin;
    uint32_t current;
    uint32_t begin_slot;
    int      ok;

    memcpy(&editor->text, replacement, sizeof(Rope));
    editor->modified = 1;

    /* Every version gets a number, which is how a job that lands late can
     * say how far the buffer has moved since it was launched. */
    serial = editor->serial;
    editor->serial = serial + 1;

    /* The new version joins the history, which retires the oldest when the
     * window is full and hands its unshared nodes back to the pool. */
    history_push(&editor->history, &editor->pool, replacement);

    total = editor->text.byte_count;
    newlines = editor->text.newline_count;
    lines = newlines;
    if (total > 0) {
        ok = rope_line_start(&editor->text, newlines, &begin_slot);
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
    uint32_t      total;
    uint32_t      position;
    uint32_t      span;
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

    adopt(editor, &loaded);
    editor->modified = 0;
    return 1;
}

/* requires: *editor is allocated and writable.
 * ensures:  as command.h.
 */
int editor_initialize(Editor *editor)
{
    uint32_t index;
    int      ok;

    ok = pool_initialize(&editor->pool);
    if (ok == 0) {
        return 0;
    }
    history_initialize(&editor->history);
    rope_initialize_empty(&editor->text);
    editor->loop = NULL;
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
    uint32_t       index;
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
    pool_release(&editor->pool);
    rope_initialize_empty(&editor->text);
}

/* requires: editor(editor); `line` is NUL-terminated.
 * ensures:  as command.h.
 */
int editor_execute(Editor *editor, const char *line)
{
    Command       command;
    Decomposition foci;
    Decomposition matches;
    Rope          edited;
    char          path[NAME_CAPACITY];
    uint32_t      position;
    uint32_t      replacement_length;
    uint32_t      length_slot;
    uint32_t      pattern_length;
    uint32_t      count;
    char          verb;
    char          value;
    int           ok;
    int           addressed;

    position = 0;
    position = skip_blanks(line, position);

    value = line[position];
    if (value == '\0') {
        return 1;
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

    ok = resolve(editor, &command, &foci);
    if (ok == 0) {
        write_line("?  no such address");
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
    if (verb == '=') {
        print_extents(editor, &foci);
        return 1;
    }

    if (verb == 'd') {
        ok = address_replace_all(&editor->pool, &editor->text, &foci, NULL, 0,
                                 &edited);
        if (ok == 0) {
            write_line("?  out of memory");
            return 0;
        }
        adopt(editor, &edited);
        return 1;
    }

    if (verb == 'c') {
        position = skip_blanks(line, position);
        position = read_delimited(line, position, 0x00, command.operand,
                                  ARGUMENT_CAPACITY, &length_slot);
        if (position == PARSE_FAILED) {
            write_line("?  replacement too long");
            return 0;
        }
        replacement_length = length_slot;
        ok = address_replace_all(&editor->pool, &editor->text, &foci,
                                 command.operand, replacement_length,
                                 &edited);
        if (ok == 0) {
            write_line("?  out of memory");
            return 0;
        }
        adopt(editor, &edited);
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
        adopt(editor, &edited);
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

        position = read_delimited(line, position, value, command.operand,
                                  ARGUMENT_CAPACITY, &length_slot);
        if (position == PARSE_FAILED) {
            write_line("?  replacement too long");
            return 0;
        }
        replacement_length = length_slot;
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

        ok = address_replace_all(&editor->pool, &editor->text, &matches,
                                 command.operand, replacement_length,
                                 &edited);
        if (ok == 0) {
            write_line("?  out of memory");
            return 0;
        }
        adopt(editor, &edited);
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
static uint32_t idle_slot(const Editor *editor)
{
    uint32_t index;
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
    uint32_t       slot;
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
    uint32_t lines;
    uint32_t index;
    uint32_t length;
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
    adopt(editor, &loaded);
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
