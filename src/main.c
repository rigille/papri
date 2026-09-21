#include "command.h"

#include <stdio.h>
#include <string.h>

/* The command loop. It reads a line, runs it, prints whatever that produced,
 * and reads the next one. There is no screen to manage, no cursor to place
 * and nothing to repaint: the terminal's scrollback is the interface, and
 * everything already printed stays exactly as it was printed.
 */

#define LINE_CAPACITY 8192

/* File scope rather than locals: the subset prefers nonaddressable locals,
 * and these are large besides. */
static Editor editor;
static char   line[LINE_CAPACITY];

/* requires: `text` is NUL-terminated and writable.
 * ensures:  a trailing newline or carriage return, if any, is replaced by a
 *           terminator.
 */
static void strip_newline(char *text)
{
    size_t length;
    char   value;

    length = strlen(text);
    while (length > 0) {
        value = text[length - 1];
        if (value == '\n') {
            text[length - 1] = '\0';
            length = length - 1;
        } else if (value == '\r') {
            text[length - 1] = '\0';
            length = length - 1;
        } else {
            return;
        }
    }
}

/* requires: standard input is readable and standard output is writable;
 *           argv[1], when present, names a file to load.
 * ensures:  every command read from standard input has been executed in
 *           order, with its output written to standard output, until `q` or
 *           end of input; the result is 0, or 1 when a file named on the
 *           command line could not be read.
 */
int main(int argc, char **argv)
{
    FILE *input_stream;
    char *path;
    char *result;
    int   ok;
    int   quit;

    ok = editor_initialize(&editor);
    if (ok == 0) {
        return 1;
    }

    if (argc > 1) {
        path = argv[1];
        ok = editor_load(&editor, path);
        if (ok == 0) {
            printf("?  cannot read %s\n", path);
            editor_release(&editor);
            return 1;
        }
    }

    input_stream = stdin;
    result = fgets(line, LINE_CAPACITY, input_stream);
    while (result != NULL) {
        strip_newline(line);
        editor_execute(&editor, line);

        quit = editor.quit;
        if (quit == 1) {
            editor_release(&editor);
            return 0;
        }

        result = fgets(line, LINE_CAPACITY, input_stream);
    }

    editor_release(&editor);
    return 0;
}
