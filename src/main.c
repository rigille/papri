#include "command.h"
#include "io.h"

#include <stdio.h>
#include <string.h>

/* The command loop. It reads a line, runs it, prints whatever that produced,
 * and reads the next one. There is no screen to manage, no cursor to place
 * and nothing to repaint: the terminal's scrollback is the interface, and
 * everything already printed stays exactly as it was printed.
 *
 * Underneath it is one io_uring and one thread, blocking in exactly one
 * place. Input and background jobs arrive through the same completion
 * queue, dispatched by the token their submitter chose — the shape kelci's
 * frame pacer uses over ALooper.
 *
 * The transcript invariant is the part that needs care. A job's output must
 * never interleave with a command's, or the scrollback stops being a
 * faithful record of what happened. So a completion is only ever reported
 * between commands, never during one, which falls out of the loop below:
 * input completions run commands, job completions print, and the two are
 * handled one at a time.
 */

#define INPUT_CHUNK    4096
#define LINE_CAPACITY  8192
#define QUEUE_DEPTH    64

/* File scope rather than locals: the subset prefers nonaddressable locals,
 * and a buffer handed to io_submit_read must stay put until the read that
 * owns it completes. */
static Editor editor;
static char   chunk[INPUT_CHUNK];
static char   line[LINE_CAPACITY];
static uint32_t line_length;

/* requires: editor(editor); holds `length` bytes at `text`.
 * ensures:  every complete line in `text` has been executed in order, and
 *           any trailing partial line is kept for the next call. The result
 *           is 1, or 0 once a command has asked to quit.
 */
static int consume(const char *text, uint32_t length)
{
    uint32_t index;
    uint32_t used;
    char     value;
    int      quit;

    index = 0;
    while (index < length) {
        value = text[index];
        if (value == 0x0A) {
            used = line_length;
            line[used] = 0x00;
            editor_execute(&editor, line);
            line_length = 0;

            quit = editor.quit;
            if (quit == 1) {
                return 0;
            }
        } else {
            used = line_length;
            if (used + 1 < LINE_CAPACITY) {
                line[used] = value;
                line_length = used + 1;
            }
        }
        index = index + 1;
    }
    return 1;
}

/* requires: standard input is readable and standard output is writable;
 *           argv[1], when present, names a file to load.
 * ensures:  every command read from standard input has been executed in
 *           order, every background job that finished has reported between
 *           commands, and the result is 0; or a file named on the command
 *           line could not be read and the result is 1.
 */
int main(int argc, char **argv)
{
    IoCompletion completion;
    IoLoop      *loop;
    char        *path;
    uint32_t     length;
    uint64_t     token;
    int32_t      outcome;
    uint32_t     outstanding;
    int          ok;
    int          running;

    ok = editor_initialize(&editor);
    if (ok == 0) {
        return 1;
    }

    loop = io_create(QUEUE_DEPTH);
    if (loop == NULL) {
        printf("?  cannot create an io_uring; is this kernel too old?\n");
        editor_release(&editor);
        return 1;
    }
    editor_attach_loop(&editor, loop);

    if (argc > 1) {
        path = argv[1];
        ok = editor_load(&editor, path);
        if (ok == 0) {
            printf("?  cannot read %s\n", path);
            io_destroy(loop);
            editor_release(&editor);
            return 1;
        }
    }

    line_length = 0;
    ok = io_submit_read(loop, IO_TOKEN_INPUT, 0, chunk, INPUT_CHUNK, -1);
    if (ok == 0) {
        io_destroy(loop);
        editor_release(&editor);
        return 1;
    }

    running = 1;
    while (running == 1) {
        ok = io_wait(loop, &completion);
        if (ok == 0) {
            running = 0;
        } else {
            token = completion.token;
            outcome = completion.result;

            if (token == IO_TOKEN_INPUT) {
                if (outcome <= 0) {
                    running = 0;
                } else {
                    length = (uint32_t)outcome;
                    ok = consume(chunk, length);
                    if (ok == 0) {
                        running = 0;
                    } else {
                        ok = io_submit_read(loop, IO_TOKEN_INPUT, 0, chunk,
                                            INPUT_CHUNK, -1);
                        if (ok == 0) {
                            running = 0;
                        }
                    }
                }
            } else {
                /* Between commands, never during one. */
                editor_complete(&editor, token, outcome);
            }
        }
    }

    /* Quitting does not silently drop work in flight. Whatever was started
     * still gets to report, which is the same rule as everywhere else: a
     * job's outcome belongs in the transcript. */
    outstanding = io_pending(loop);
    while (outstanding > 0) {
        ok = io_wait(loop, &completion);
        if (ok == 0) {
            outstanding = 0;
        } else {
            token = completion.token;
            outcome = completion.result;
            if (token != IO_TOKEN_INPUT) {
                editor_complete(&editor, token, outcome);
            }
            outstanding = io_pending(loop);
        }
    }

    io_destroy(loop);
    editor_release(&editor);
    return 0;
}
