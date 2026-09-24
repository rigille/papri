#ifndef PAPRI_GRAMMAR_H
#define PAPRI_GRAMMAR_H

#include <stddef.h>

#include "structure.h"

/* Which grammar a buffer gets, decided by the name the buffer came from.
 *
 * One grammar for the whole session was a placeholder. A transcript that
 * holds three files at once is the point of papri, and three files are
 * routinely three languages; `F` in buffer 0 and `F` in buffer 1 must be
 * able to disagree about what a definition is. So the grammar is a property
 * of the buffer's name, not of the process.
 *
 * The table is data, registered from a configuration file or from a command,
 * and papri still knows nothing about any language: a registration is a
 * suffix, the entry point's name, and a directory holding `parser` and
 * `queries/tags.scm`. The suffix `*` registers a catch-all, used only when
 * no specific suffix matches — which is what keeps a buffer with no name,
 * or one named `Makefile`, from silently having no grammar when the user
 * configured a default.
 *
 * Loading is deferred to first use, per entry. A session that never asks for
 * structure never dlopens anything, and a session that asks about C does not
 * pay for the Python grammar it also configured.
 *
 * Entries do not share a loaded grammar, even when two suffixes name one
 * directory: `.c` and `.h` get a parser each. dlopen refcounts the shared
 * object so the mapping is still one mapping, and owning the handle outright
 * is what lets an entry be re-registered — or released — without asking who
 * else is pointing at it.
 */

#define GRAMMAR_CAPACITY          32
#define GRAMMAR_SUFFIX_CAPACITY   32
#define GRAMMAR_LANGUAGE_CAPACITY 64
#define GRAMMAR_PATH_CAPACITY     1024

typedef struct GrammarEntry {
    char       suffix[GRAMMAR_SUFFIX_CAPACITY];
    char       language[GRAMMAR_LANGUAGE_CAPACITY];
    char       directory[GRAMMAR_PATH_CAPACITY];
    Structure *loaded;      /* null until the entry is first used */
    int        failed;      /* 1 once loading has been tried and failed */
} GrammarEntry;

typedef struct GrammarTable {
    GrammarEntry entry[GRAMMAR_CAPACITY];
    size_t       count;
} GrammarTable;

/* ── Abstract predicates ────────────────────────────────────────────────────
 * grammar_table(table, registrations)
 *   Owns *table. `registrations` is a sequence of (suffix, language,
 *   directory) triples, at most GRAMMAR_CAPACITY of them, with no suffix
 *   appearing twice. An entry additionally owns structure(loaded, language)
 *   once it has been loaded, and the table owns it until released.
 */

/* requires: *table is allocated and writable.
 * ensures:  grammar_table(table, registrations) with `registrations` empty.
 */
void grammar_initialize(GrammarTable *table);

/* requires: grammar_table(table, registrations).
 * ensures:  every grammar the table had loaded is unloaded and
 *           grammar_table(table, empty). The table may be used again.
 */
void grammar_release(GrammarTable *table);

/* requires: grammar_table(table, registrations); `suffix`, `language` and
 *           `directory` are NUL-terminated and each fits its field.
 * ensures:  grammar_table(table, registrations') where registrations' is
 *           registrations with the triple for `suffix` replaced, or appended
 *           when that suffix was not registered, and the result is 1; a
 *           grammar previously loaded for that suffix is unloaded. Or the
 *           table was full or a field did not fit, registrations is
 *           unchanged, and the result is 0.
 */
int grammar_register(GrammarTable *table, const char *suffix,
                     const char *language, const char *directory);

/* One line of configuration, wherever it came from — a file or the `G`
 * command. Having one parser is what keeps the two spellings the same
 * spelling.
 *
 * requires: grammar_table(table, registrations); `line` is NUL-terminated.
 * ensures:  grammar_table(table, registrations') where registrations' is
 *           registrations with the triple that line describes applied —
 *           `suffix language directory`, whitespace-separated, the directory
 *           running to the end of the line with trailing whitespace removed
 *           — and the result is 1. A blank line, a line whose first
 *           non-blank is `#`, a line missing a field, or a full table leaves
 *           registrations unchanged and the result is 0.
 */
int grammar_register_line(GrammarTable *table, const char *line);

/* requires: grammar_table(table, registrations); `path` is NUL-terminated.
 * ensures:  grammar_table(table, registrations') where registrations' has
 *           one registration applied per non-blank, non-comment line of that
 *           file — `suffix language directory`, whitespace-separated, the
 *           directory running to the end of the line — and the result is how
 *           many were applied; or the file could not be read and the result
 *           is -1. A malformed line is skipped, not fatal.
 */
int grammar_read_configuration(GrammarTable *table, const char *path);

/* The environment, in increasing order of specificity, so that a file may
 * override the catch-all and never the other way round.
 *
 * requires: grammar_table(table, registrations).
 * ensures:  grammar_table(table, registrations') where registrations' also
 *           holds the catch-all `*` naming $PAPRI_GRAMMAR with $PAPRI_LANGUAGE
 *           (defaulting to `c`) when PAPRI_GRAMMAR is set, and every
 *           registration in the file named by $PAPRI_GRAMMARS when that is
 *           set. The result is 1 when anything at all was registered.
 */
int grammar_configure_from_environment(GrammarTable *table);

/* requires: grammar_table(table, registrations).
 * ensures:  grammar_table(table, registrations); the result is how many
 *           registrations there are. No memory is written.
 */
size_t grammar_count(const GrammarTable *table);

/* The suffix rule, stated once: the longest registered suffix that `name`
 * ends with wins, and `*` — which every name ends with vacuously — is the
 * shortest of all, so it is consulted only when nothing else matched.
 *
 * requires: grammar_table(table, registrations); `name` is NUL-terminated
 *           and may be empty.
 * ensures:  grammar_table(table, registrations); the result is the index of
 *           the registration governing `name`, or grammar_count(table) when
 *           none does. No memory is written.
 */
size_t grammar_lookup(const GrammarTable *table, const char *name);

/* requires: grammar_table(table, registrations); index < count.
 * ensures:  grammar_table(table, registrations); the result is that
 *           registration's suffix, valid while the registration stands. No
 *           memory is written.
 */
const char *grammar_suffix_at(const GrammarTable *table, size_t index);

/* requires: grammar_table(table, registrations); index < count.
 * ensures:  grammar_table(table, registrations); the result is that
 *           registration's language. No memory is written.
 */
const char *grammar_language_at(const GrammarTable *table, size_t index);

/* requires: grammar_table(table, registrations); index < count.
 * ensures:  grammar_table(table, registrations); the result is that
 *           registration's directory. No memory is written.
 */
const char *grammar_directory_at(const GrammarTable *table, size_t index);

/* requires: grammar_table(table, registrations); index < count.
 * ensures:  grammar_table(table, registrations) with that entry's grammar
 *           loaded, and the result points at structure(loaded, language)
 *           which the table keeps owning; or the grammar could not be
 *           loaded, the entry is marked so it is not tried again, and the
 *           result is null.
 */
Structure *grammar_structure_at(GrammarTable *table, size_t index);

/* requires: grammar_table(table, registrations); `name` is NUL-terminated.
 * ensures:  grammar_table(table, registrations) with the grammar governing
 *           `name` loaded, and the result points at it; or no registration
 *           governs `name`, or its grammar would not load, and the result is
 *           null.
 */
Structure *grammar_for_name(GrammarTable *table, const char *name);

#endif /* PAPRI_GRAMMAR_H */
