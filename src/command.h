#ifndef PAPRI_COMMAND_H
#define PAPRI_COMMAND_H

#include "address.h"
#include "history.h"
#include "io.h"
#include "pool.h"
#include "rope.h"
#include "structure.h"
#include "view.h"

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

#define NAME_CAPACITY   1024
#define JOB_CAPACITY    8
#define BUFFER_CAPACITY 16

/* A buffer that is not the current one. The current buffer's state lives
 * directly in the Editor, and switching saves it here and loads another
 * back, so every command below keeps working on `editor->text` without
 * knowing that several buffers exist. */
typedef struct BufferSlot {
    Rope     text;
    History  history;
    size_t   current_line;
    uint32_t serial;
    int      modified;
    int      used;
    char     name[NAME_CAPACITY];
} BufferSlot;

/* Work that runs while you keep editing. A job records the version it was
 * launched against, so when it lands it can say whether the buffer has
 * moved on — and refuse to clobber it if it has. */
typedef enum JobKind {
    JOB_IDLE,
    JOB_LOAD,    /* read a file and replace the buffer, if it still can */
    JOB_COUNT    /* read a file and report its size and line count */
} JobKind;

typedef struct Job {
    JobKind        kind;
    uint32_t       id;
    int            descriptor;
    unsigned char *buffer;
    size_t         capacity;
    uint32_t       launched_at;
    char           path[NAME_CAPACITY];
} Job;

typedef struct Editor {
    Pool       pool;          /* shared by every buffer */
    BufferSlot slots[BUFFER_CAPACITY];
    size_t     current;
    History  history;
    Rope     text;
    IoLoop    *loop;
    Structure *structure;      /* the grammar, loaded on first use */
    Job      jobs[JOB_CAPACITY];
    uint32_t next_job_id;
    uint32_t serial;
    size_t   current_line;
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
 *   the nodes they alone held go back to the pool. The other buffers live
 *   in `slots`, each with its own history and its own versions, all drawn
 *   from the one pool; `current` says which slot the live fields belong to.
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

/* The command that makes folding unnecessary: ask for the definitions and
 * they are printed, rather than hiding the rest of the buffer to reveal
 * them.
 *
 * requires: editor(editor).
 * ensures:  editor(editor); every definition in the current buffer is
 *           written as its line, its kind and its name, and the result is
 *           1; or no grammar was available and the result is 0.
 */
int editor_list_definitions(Editor *editor);

/* requires: editor(editor); index < BUFFER_CAPACITY.
 * ensures:  editor(editor) with that buffer current, its own text, history
 *           and name live, and the result 1; or no such buffer and the
 *           result is 0. An unused slot becomes an empty buffer.
 */
int editor_select_buffer(Editor *editor, size_t index);

/* requires: editor(editor).
 * ensures:  editor(editor); one line per buffer in use is written, the
 *           current one marked.
 */
void editor_report_buffers(Editor *editor);

/* requires: editor(editor).
 * ensures:  editor(editor); the result is the lowest unused buffer index, or
 *           BUFFER_CAPACITY when they are all in use. No memory is written.
 */
size_t editor_free_buffer(const Editor *editor);

/* requires: editor(editor); io_loop(loop, pending) which must outlive the
 *           editor.
 * ensures:  editor(editor) able to start jobs on that loop. Without one,
 *           every job command is refused and the editor is simply
 *           synchronous.
 */
void editor_attach_loop(Editor *editor, IoLoop *loop);

/* Report a finished job and act on it.
 *
 * Called only BETWEEN commands, never during one. That is the whole
 * transcript discipline: the scrollback is the interface, so nothing may
 * interleave its output with a command's.
 *
 * requires: editor(editor); `token` and `result` come from io_wait.
 * ensures:  editor(editor), advanced if the job applied an edit; the
 *           outcome is written to standard output, naming the version the
 *           job was launched against and how far the buffer has moved since.
 *           The result is 1 when the token named a live job.
 */
int editor_complete(Editor *editor, uint64_t token, int32_t result);

/* requires: editor(editor).
 * ensures:  editor(editor); one line per job still in flight is written. No
 *           memory is written.
 */
void editor_report_jobs(const Editor *editor);

#endif /* PAPRI_COMMAND_H */
