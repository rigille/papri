#ifndef PAPRI_COMMAND_H
#define PAPRI_COMMAND_H

#include "address.h"
#include "history.h"
#include "pool.h"
#include "rope.h"

/* The command layer: parse one line, resolve its address against the current
 * version, apply a verb to every focus.
 *
 * The shape is ed's — address, then verb — but the grammar is built around
 * the decomposition rather than retrofitted onto it, so a verb never learns
 * about addressing and an address never learns about verbs. `s/pat/rep/` is
 * not a special form: it is the address composed with a match traversal, and
 * then an ordinary change.
 *
 * Output goes only to standard output, in whole lines, in response to a
 * command. Nothing is ever repainted, because nothing may be: the terminal's
 * scrollback is the interface, and rewriting it would be lying about what
 * happened.
 */

#define NAME_CAPACITY 1024

typedef struct Editor {
    Pool     pool;
    History  history;
    Rope     text;
    uint32_t current_line;
    int      modified;
    int      quit;
    char     name[NAME_CAPACITY];
} Editor;

/* ── Abstract predicates ────────────────────────────────────────────────────
 * editor(editor)
 *   Owns *editor: its pool, its history, and the current version of the
 *   buffer, which is rope(&editor->text, bytes, share) holding shares drawn
 *   from that pool and is also the newest version in the history. Older
 *   versions stay readable until the history retires them, at which point
 *   the nodes they alone held go back to the pool.
 *   `current_line` counts from 1 and names a line of `bytes`, or is 0 when
 *   the buffer is empty. `name` is the NUL-terminated file the buffer came
 *   from, empty when it came from nowhere.
 */

/* requires: *editor is allocated and writable.
 * ensures:  editor(editor) with an empty buffer and no name, and the result
 *           is 1; or nothing was allocated and the result is 0.
 */
int editor_initialize(Editor *editor);

/* requires: editor(editor).
 * ensures:  the pool and every version in it are freed; *editor must not be
 *           used again without another editor_initialize.
 */
void editor_release(Editor *editor);

/* requires: editor(editor); `line` is a NUL-terminated command, without its
 *           newline.
 * ensures:  editor(editor) advanced by that command — a new version when the
 *           verb edited, the same one when it only printed — and any output
 *           written to standard output. The result is 1 when the command was
 *           understood and applied, and 0 when it was not; a refused command
 *           leaves the buffer as it was.
 */
int editor_execute(Editor *editor, const char *line);

/* requires: editor(editor); `path` is a NUL-terminated file name.
 * ensures:  editor(editor) whose buffer is that file's bytes and whose name
 *           is `path`, and the result is 1; or the file could not be read,
 *           the buffer is unchanged, and the result is 0.
 */
int editor_load(Editor *editor, const char *path);

#endif /* PAPRI_COMMAND_H */
