#include "view.h"

#include <stdio.h>

#include "utf8.h"

/* Windows, never the whole range: the buffer is a rope so that a large file
 * costs nothing to look at, and materializing a range to print it would
 * throw that away. */
#define VIEW_WINDOW 4096
#define HEX_COLUMNS 16

/* requires: holds a read share of `length` bytes at `bytes`.
 * ensures:  the read share is returned; they are written to standard output.
 */
static void put_bytes(const unsigned char *bytes, size_t length)
{
    FILE *stream;

    stream = stdout;
    fwrite(bytes, 1, length, stream);
}

/* requires: `text` is NUL-terminated.
 * ensures:  it is written to standard output.
 */
static void put_text(const char *text)
{
    FILE *stream;

    stream = stdout;
    fputs(text, stream);
}

/* requires: as view.h.
 * ensures:  as view.h.
 */
void view_write_text(const Rope *rope, size_t start, size_t end)
{
    unsigned char window[VIEW_WINDOW];
    size_t        position;
    size_t        span;
    int           ok;

    position = start;
    while (position < end) {
        span = end - position;
        if (span > VIEW_WINDOW) {
            span = VIEW_WINDOW;
        }
        ok = rope_copy_range(rope, position, span, window);
        if (ok == 0) {
            return;
        }
        put_bytes(window, span);
        position = position + span;
    }
}

/* requires: as view.h.
 * ensures:  as view.h.
 */
void view_write_hex(const Rope *rope, size_t start, size_t end)
{
    unsigned char row[HEX_COLUMNS];
    char          line[128];
    size_t        position;
    size_t        span;
    uint32_t      index;
    size_t        written;
    unsigned char value;
    int           ok;

    position = start;
    while (position < end) {
        span = end - position;
        if (span > HEX_COLUMNS) {
            span = HEX_COLUMNS;
        }
        ok = rope_copy_range(rope, position, span, row);
        if (ok == 0) {
            return;
        }

        written = (uint32_t)snprintf(line, sizeof(line), "%08x  ", position);
        index = 0;
        while (index < HEX_COLUMNS) {
            if (index < span) {
                value = row[index];
                written = written + (uint32_t)snprintf(line + written,
                                                       sizeof(line) - written,
                                                       "%02x ", value);
            } else {
                written = written + (uint32_t)snprintf(line + written,
                                                       sizeof(line) - written,
                                                       "   ");
            }
            if (index == 7) {
                written = written + (uint32_t)snprintf(line + written,
                                                       sizeof(line) - written,
                                                       " ");
            }
            index = index + 1;
        }
        written = written + (uint32_t)snprintf(line + written,
                                               sizeof(line) - written, " |");
        index = 0;
        while (index < span) {
            value = row[index];
            if (value < 0x20) {
                value = 0x2E;
            }
            if (value > 0x7E) {
                value = 0x2E;
            }
            line[written] = (char)value;
            written = written + 1;
            index = index + 1;
        }
        line[written] = '|';
        written = written + 1;
        line[written] = '\n';
        written = written + 1;
        line[written] = '\0';

        put_text(line);
        position = position + span;
    }
}

/* requires: as view.h.
 * ensures:  as view.h.
 */
void view_write_codepoints(const Rope *rope, size_t start, size_t end)
{
    unsigned char window[VIEW_WINDOW];
    char          line[128];
    size_t        position;
    size_t        span;
    uint32_t      index;
    uint32_t      state;
    uint32_t      codepoint;
    uint32_t      decoded;
    size_t        began;
    unsigned char value;
    int           ok;

    /* The entire resumption state. A code point split across two windows —
     * or across two rope leaves, which is the same problem — costs nothing
     * to carry, because the decoder is a DFA. That is why the buffer can be
     * chunked at a cache line without text ever noticing. */
    state = UTF8_ACCEPT;
    codepoint = 0;
    began = start;

    position = start;
    while (position < end) {
        span = end - position;
        if (span > VIEW_WINDOW) {
            span = VIEW_WINDOW;
        }
        ok = rope_copy_range(rope, position, span, window);
        if (ok == 0) {
            return;
        }

        index = 0;
        while (index < span) {
            value = window[index];
            state = next_state(state, value, &codepoint);

            if (state == UTF8_ACCEPT) {
                /* `codepoint` is addressable, so reading it is a load and
                 * may not sit inside a call argument. */
                decoded = codepoint;
                snprintf(line, sizeof(line), "%u\tU+%04X\t%u byte(s)\n",
                         began, decoded, position + index + 1 - began);
                put_text(line);
                began = position + index + 1;
            } else if (state == UTF8_REJECT) {
                snprintf(line, sizeof(line), "%u\tinvalid byte 0x%02x\n",
                         position + index, value);
                put_text(line);
                state = UTF8_ACCEPT;
                codepoint = 0;
                began = position + index + 1;
            }

            index = index + 1;
        }
        position = position + span;
    }

    if (state != UTF8_ACCEPT) {
        snprintf(line, sizeof(line), "%u\ttruncated code point\n", began);
        put_text(line);
    }
}
