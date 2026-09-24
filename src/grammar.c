/* The suffix table. See grammar.h for why the grammar belongs to the buffer
 * rather than to the process.
 *
 * Nothing here knows what tree-sitter is. A registration is three strings
 * and a lazily loaded handle; the handle is made and destroyed through
 * src/structure.h, which is where the foreign boundary lives. That is what
 * keeps this file inside the subset and inside `make normalform`.
 */

#include "grammar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRAMMAR_LINE_CAPACITY 2048

/* requires: `destination` holds `capacity` bytes; `source` is
 *           NUL-terminated.
 * ensures:  when the source fits, `destination` holds it NUL-terminated and
 *           the result is 1; otherwise `destination` is unspecified and the
 *           result is 0.
 */
static int copy_field(char *destination, size_t capacity, const char *source)
{
    size_t length;

    length = strlen(source);
    if (length + 1 > capacity) {
        return 0;
    }
    memcpy(destination, source, length + 1);
    return 1;
}

/* requires: `name` and `suffix` are NUL-terminated.
 * ensures:  the result is 1 when `name` ends with `suffix`, comparing bytes.
 *           An empty suffix matches every name. No memory is written.
 */
static int ends_with(const char *name, const char *suffix)
{
    size_t name_length;
    size_t suffix_length;
    size_t offset;
    int    difference;

    name_length = strlen(name);
    suffix_length = strlen(suffix);
    if (suffix_length > name_length) {
        return 0;
    }
    offset = name_length - suffix_length;
    difference = strcmp(name + offset, suffix);
    if (difference == 0) {
        return 1;
    }
    return 0;
}

/* The catch-all is spelled `*` and behaves as the empty suffix — it matches
 * every name — but must lose to any suffix that matches for a reason.
 *
 * requires: `suffix` is NUL-terminated.
 * ensures:  the result is 1 when it is the catch-all. No memory is written.
 */
static int is_catch_all(const char *suffix)
{
    int difference;

    difference = strcmp(suffix, "*");
    if (difference == 0) {
        return 1;
    }
    return 0;
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

/* requires: `line` is NUL-terminated; `position` indexes into it.
 * ensures:  the result is `position` advanced past any run of blanks. No
 *           memory is written.
 */
static size_t skip_blanks(const char *line, size_t position)
{
    size_t at;
    char   value;
    int    blank;

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

/* requires: `line` is NUL-terminated; `position` indexes into it; `field`
 *           holds `capacity` bytes and capacity > 0.
 * ensures:  `field` holds the run of non-blank bytes beginning at the first
 *           non-blank at or after `position`, NUL-terminated and truncated
 *           to capacity - 1 bytes; the result is the position just past that
 *           run. An empty `field` means the line held no further field.
 */
static size_t read_field(const char *line, size_t position, char *field,
                         size_t capacity)
{
    size_t at;
    size_t written;
    size_t limit;
    char   value;
    int    blank;
    int    running;

    at = skip_blanks(line, position);
    limit = capacity - 1;
    written = 0;

    running = 1;
    while (running == 1) {
        value = line[at];
        blank = is_blank(value);
        if (value == '\0') {
            running = 0;
        } else if (blank == 1) {
            running = 0;
        } else {
            if (written < limit) {
                field[written] = value;
                written = written + 1;
            }
            at = at + 1;
        }
    }

    field[written] = '\0';
    return at;
}

/* The last field runs to the end of the line, so a directory may contain
 * blanks. Trailing whitespace — the newline fgets leaves behind above all —
 * is not part of it.
 *
 * requires: `line` is NUL-terminated; `position` indexes into it; `field`
 *           holds `capacity` bytes and capacity > 0.
 * ensures:  `field` holds the rest of the line from the first non-blank at
 *           or after `position`, with trailing blanks, carriage returns and
 *           newlines removed, NUL-terminated and truncated to capacity - 1
 *           bytes.
 */
static void read_rest(const char *line, size_t position, char *field,
                      size_t capacity)
{
    size_t at;
    size_t written;
    size_t limit;
    char   value;
    int    blank;
    int    running;

    at = skip_blanks(line, position);
    limit = capacity - 1;
    written = 0;

    value = line[at];
    while (value != '\0') {
        if (written < limit) {
            field[written] = value;
            written = written + 1;
        }
        at = at + 1;
        value = line[at];
    }

    running = 1;
    while (running == 1) {
        if (written == 0) {
            running = 0;
        } else {
            value = field[written - 1];
            blank = is_blank(value);
            if (value == '\n') {
                blank = 1;
            }
            if (value == '\r') {
                blank = 1;
            }
            if (blank == 0) {
                running = 0;
            } else {
                written = written - 1;
            }
        }
    }

    field[written] = '\0';
}

/* requires: *table is allocated and writable.
 * ensures:  as grammar.h.
 */
void grammar_initialize(GrammarTable *table)
{
    size_t index;

    index = 0;
    while (index < GRAMMAR_CAPACITY) {
        table->entry[index].suffix[0] = '\0';
        table->entry[index].language[0] = '\0';
        table->entry[index].directory[0] = '\0';
        table->entry[index].loaded = NULL;
        table->entry[index].failed = 0;
        index = index + 1;
    }
    table->count = 0;
}

/* requires: grammar_table(table, registrations).
 * ensures:  as grammar.h.
 */
void grammar_release(GrammarTable *table)
{
    Structure *loaded;
    size_t     count;
    size_t     index;

    count = table->count;
    index = 0;
    while (index < count) {
        loaded = table->entry[index].loaded;
        if (loaded != NULL) {
            structure_destroy(loaded);
            table->entry[index].loaded = NULL;
        }
        table->entry[index].failed = 0;
        table->entry[index].suffix[0] = '\0';
        index = index + 1;
    }
    table->count = 0;
}

/* requires: grammar_table(table, registrations); `suffix` is NUL-terminated.
 * ensures:  grammar_table(table, registrations); the result is the index
 *           registered for exactly that suffix, or the count when there is
 *           none. No memory is written.
 */
static size_t slot_of_suffix(const GrammarTable *table, const char *suffix)
{
    const char *registered;
    size_t      count;
    size_t      index;
    int         difference;

    count = table->count;
    index = 0;
    while (index < count) {
        registered = table->entry[index].suffix;
        difference = strcmp(registered, suffix);
        if (difference == 0) {
            return index;
        }
        index = index + 1;
    }
    return count;
}

/* requires: grammar_table(table, registrations).
 * ensures:  as grammar.h.
 */
int grammar_register(GrammarTable *table, const char *suffix,
                     const char *language, const char *directory)
{
    Structure *loaded;
    size_t     count;
    size_t     slot;
    int        ok;

    count = table->count;
    slot = slot_of_suffix(table, suffix);
    if (slot == count) {
        if (count >= GRAMMAR_CAPACITY) {
            return 0;
        }
    }

    ok = copy_field(table->entry[slot].suffix, GRAMMAR_SUFFIX_CAPACITY,
                    suffix);
    if (ok == 0) {
        return 0;
    }
    ok = copy_field(table->entry[slot].language, GRAMMAR_LANGUAGE_CAPACITY,
                    language);
    if (ok == 0) {
        return 0;
    }
    ok = copy_field(table->entry[slot].directory, GRAMMAR_PATH_CAPACITY,
                    directory);
    if (ok == 0) {
        return 0;
    }

    /* Re-registering a suffix drops whatever it had loaded. The entry owns
     * its handle outright, so there is nobody to ask. */
    loaded = table->entry[slot].loaded;
    if (loaded != NULL) {
        structure_destroy(loaded);
    }
    table->entry[slot].loaded = NULL;
    table->entry[slot].failed = 0;

    if (slot == count) {
        table->count = count + 1;
    }
    return 1;
}

/* requires: grammar_table(table, registrations).
 * ensures:  as grammar.h.
 */
int grammar_register_line(GrammarTable *table, const char *line)
{
    char   suffix[GRAMMAR_SUFFIX_CAPACITY];
    char   language[GRAMMAR_LANGUAGE_CAPACITY];
    char   directory[GRAMMAR_PATH_CAPACITY];
    size_t position;
    char   value;
    int    ok;

    position = skip_blanks(line, 0);
    value = line[position];

    /* Blank lines and comments are how a configuration file explains
     * itself; neither is an error, and neither is a registration. */
    if (value == '\0') {
        return 0;
    }
    if (value == '#') {
        return 0;
    }
    if (value == '\n') {
        return 0;
    }
    if (value == '\r') {
        return 0;
    }

    position = read_field(line, position, suffix, GRAMMAR_SUFFIX_CAPACITY);
    position = read_field(line, position, language,
                          GRAMMAR_LANGUAGE_CAPACITY);
    read_rest(line, position, directory, GRAMMAR_PATH_CAPACITY);

    value = language[0];
    if (value == '\0') {
        return 0;
    }
    value = directory[0];
    if (value == '\0') {
        return 0;
    }

    ok = grammar_register(table, suffix, language, directory);
    return ok;
}

/* requires: grammar_table(table, registrations).
 * ensures:  as grammar.h.
 */
int grammar_read_configuration(GrammarTable *table, const char *path)
{
    char   line[GRAMMAR_LINE_CAPACITY];
    FILE  *file;
    char  *read;
    int    applied;
    int    ok;

    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }

    applied = 0;
    read = fgets(line, GRAMMAR_LINE_CAPACITY, file);
    while (read != NULL) {
        ok = grammar_register_line(table, line);
        if (ok == 1) {
            applied = applied + 1;
        }
        read = fgets(line, GRAMMAR_LINE_CAPACITY, file);
    }

    fclose(file);
    return applied;
}

/* requires: grammar_table(table, registrations).
 * ensures:  as grammar.h.
 */
int grammar_configure_from_environment(GrammarTable *table)
{
    const char *directory;
    const char *language;
    const char *configuration;
    size_t      count;

    /* The single-grammar environment papri started with, kept as the
     * catch-all: it said "parse everything with this", and that is exactly
     * what a catch-all registration means. */
    directory = getenv("PAPRI_GRAMMAR");
    if (directory != NULL) {
        language = getenv("PAPRI_LANGUAGE");
        if (language == NULL) {
            language = "c";
        }
        grammar_register(table, "*", language, directory);
    }

    /* Read second, so a file's specific suffixes override the catch-all
     * rather than the other way round — and so that registering `*` in the
     * file is a deliberate way to change the default. */
    configuration = getenv("PAPRI_GRAMMARS");
    if (configuration != NULL) {
        grammar_read_configuration(table, configuration);
    }

    count = table->count;
    if (count > 0) {
        return 1;
    }
    return 0;
}

/* requires: grammar_table(table, registrations).
 * ensures:  as grammar.h.
 */
size_t grammar_count(const GrammarTable *table)
{
    size_t count;

    count = table->count;
    return count;
}

/* requires: grammar_table(table, registrations).
 * ensures:  as grammar.h.
 */
size_t grammar_lookup(const GrammarTable *table, const char *name)
{
    const char *suffix;
    size_t      count;
    size_t      index;
    size_t      best;
    size_t      best_length;
    size_t      length;
    int         catch_all;
    int         matched;

    count = table->count;
    best = count;
    best_length = 0;

    index = 0;
    while (index < count) {
        suffix = table->entry[index].suffix;
        catch_all = is_catch_all(suffix);
        if (catch_all == 1) {
            if (best == count) {
                best = index;
                best_length = 0;
            }
        } else {
            matched = ends_with(name, suffix);
            if (matched == 1) {
                length = strlen(suffix);
                if (length > best_length) {
                    best = index;
                    best_length = length;
                }
            }
        }
        index = index + 1;
    }

    return best;
}

/* requires: grammar_table(table, registrations); index < count.
 * ensures:  as grammar.h.
 */
const char *grammar_suffix_at(const GrammarTable *table, size_t index)
{
    const char *text;

    text = table->entry[index].suffix;
    return text;
}

/* requires: grammar_table(table, registrations); index < count.
 * ensures:  as grammar.h.
 */
const char *grammar_language_at(const GrammarTable *table, size_t index)
{
    const char *text;

    text = table->entry[index].language;
    return text;
}

/* requires: grammar_table(table, registrations); index < count.
 * ensures:  as grammar.h.
 */
const char *grammar_directory_at(const GrammarTable *table, size_t index)
{
    const char *text;

    text = table->entry[index].directory;
    return text;
}

/* requires: grammar_table(table, registrations); index < count.
 * ensures:  as grammar.h.
 */
Structure *grammar_structure_at(GrammarTable *table, size_t index)
{
    Structure  *loaded;
    const char *directory;
    const char *language;
    int         failed;

    loaded = table->entry[index].loaded;
    if (loaded != NULL) {
        return loaded;
    }
    failed = table->entry[index].failed;
    if (failed == 1) {
        return NULL;
    }

    directory = table->entry[index].directory;
    language = table->entry[index].language;
    loaded = structure_create(directory, language);
    if (loaded == NULL) {
        /* Marked, so a session with a bad registration does not pay a
         * dlopen for every command that touches structure. */
        table->entry[index].failed = 1;
        return NULL;
    }

    table->entry[index].loaded = loaded;
    return loaded;
}

/* requires: grammar_table(table, registrations).
 * ensures:  as grammar.h.
 */
Structure *grammar_for_name(GrammarTable *table, const char *name)
{
    Structure *loaded;
    size_t     count;
    size_t     index;

    index = grammar_lookup(table, name);
    count = table->count;
    if (index == count) {
        return NULL;
    }
    loaded = grammar_structure_at(table, index);
    return loaded;
}
