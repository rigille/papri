/* The second foreign boundary. See structure.h for what it is for.
 *
 * A grammar is loaded with dlopen at run time, which is how editors load
 * them and what lets papri work with a language it has never heard of. The
 * house naming and spec rules still apply here; `make lint` still checks
 * them.
 */

#include "structure.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tree_sitter/api.h>

/* Windows, never the whole buffer: tree-sitter reads through a callback, so
 * a large file is parsed without ever being materialized. */
#define PARSE_WINDOW 4096
#define PATH_CAPACITY 2048
#define NAME_CAPACITY 256

struct Structure {
    void          *handle;      /* the grammar's shared object */
    const TSLanguage *language;
    TSQuery       *tags;
    TSParser      *parser;
};

/* What the read callback needs: which rope, and somewhere to put the window
 * it hands back. The pointer it returns must stay valid until the next
 * call, which one reusable window satisfies. */
typedef struct ReadState {
    const Rope   *rope;
    unsigned char window[PARSE_WINDOW];
} ReadState;

/* requires: `payload` is a ReadState whose rope is readable.
 * ensures:  *bytes_read is how many bytes were produced, and the result
 *           points at them; at the end of the buffer *bytes_read is 0.
 */
static const char *read_rope(void *payload, uint32_t byte_index,
                             TSPoint position, uint32_t *bytes_read)
{
    ReadState *state;
    uint32_t   total;
    uint32_t   span;
    int        ok;

    (void)position;

    state = payload;
    total = rope_byte_count(state->rope);

    if (byte_index >= total) {
        *bytes_read = 0;
        return NULL;
    }

    span = total - byte_index;
    if (span > PARSE_WINDOW) {
        span = PARSE_WINDOW;
    }

    ok = rope_copy_range(state->rope, byte_index, span, state->window);
    if (ok == 0) {
        *bytes_read = 0;
        return NULL;
    }

    *bytes_read = span;
    return (const char *)state->window;
}

/* requires: `directory` and `name` are NUL-terminated; *out writable.
 * ensures:  *out holds the contents of directory/queries/tags.scm,
 *           NUL-terminated, and the result is its length; or the file could
 *           not be read and the result is 0. The caller frees *out.
 */
static uint32_t read_tags_query(const char *directory, char **out)
{
    char   path[PATH_CAPACITY];
    FILE  *file;
    char  *contents;
    long   size;
    size_t read_count;

    snprintf(path, sizeof(path), "%s/queries/tags.scm", directory);
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
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
    contents[read_count] = '\0';

    *out = contents;
    return (uint32_t)read_count;
}

/* requires: as structure.h.
 * ensures:  as structure.h.
 */
Structure *structure_create(const char *directory, const char *language)
{
    char       path[PATH_CAPACITY];
    char       symbol[NAME_CAPACITY];
    char      *query_source;
    Structure *structure;
    const TSLanguage *(*entry)(void);
    void      *address;
    uint32_t   query_length;
    uint32_t   error_offset;
    TSQueryError error_type;

    structure = malloc(sizeof(Structure));
    if (structure == NULL) {
        return NULL;
    }
    structure->handle = NULL;
    structure->language = NULL;
    structure->tags = NULL;
    structure->parser = NULL;

    snprintf(path, sizeof(path), "%s/parser", directory);
    structure->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (structure->handle == NULL) {
        free(structure);
        return NULL;
    }

    snprintf(symbol, sizeof(symbol), "tree_sitter_%s", language);
    address = dlsym(structure->handle, symbol);
    if (address == NULL) {
        dlclose(structure->handle);
        free(structure);
        return NULL;
    }

    /* dlsym hands back a void *, and converting it to a function pointer is
     * the one cast POSIX requires here. */
    *(void **)(&entry) = address;
    structure->language = entry();
    if (structure->language == NULL) {
        dlclose(structure->handle);
        free(structure);
        return NULL;
    }

    query_source = NULL;
    query_length = read_tags_query(directory, &query_source);
    if (query_length == 0) {
        dlclose(structure->handle);
        free(structure);
        return NULL;
    }

    error_offset = 0;
    error_type = TSQueryErrorNone;
    structure->tags = ts_query_new(structure->language, query_source,
                                   query_length, &error_offset, &error_type);
    free(query_source);
    if (structure->tags == NULL) {
        dlclose(structure->handle);
        free(structure);
        return NULL;
    }

    structure->parser = ts_parser_new();
    if (structure->parser == NULL) {
        ts_query_delete(structure->tags);
        dlclose(structure->handle);
        free(structure);
        return NULL;
    }
    ts_parser_set_language(structure->parser, structure->language);

    return structure;
}

/* requires: as structure.h.
 * ensures:  as structure.h.
 */
void structure_destroy(Structure *structure)
{
    if (structure == NULL) {
        return;
    }
    if (structure->parser != NULL) {
        ts_parser_delete(structure->parser);
    }
    if (structure->tags != NULL) {
        ts_query_delete(structure->tags);
    }
    if (structure->handle != NULL) {
        dlclose(structure->handle);
    }
    free(structure);
}

/* requires: as structure.h.
 * ensures:  as structure.h.
 */
int structure_list_definitions(Structure *structure, const Rope *rope)
{
    ReadState     *state;
    TSInput        input;
    TSTree        *tree;
    TSNode         root;
    TSQueryCursor *cursor;
    TSQueryMatch   match;
    TSNode         captured;
    TSPoint        start;
    char           name[NAME_CAPACITY];
    char           label[NAME_CAPACITY * 2];
    const char    *capture_name;
    const char    *kind;
    FILE          *stream;
    uint32_t       capture_length;
    uint32_t       index;
    uint32_t       begin;
    uint32_t       finish;
    uint32_t       span;
    int            found;
    int            ok;
    int            more;

    state = malloc(sizeof(ReadState));
    if (state == NULL) {
        return -1;
    }
    state->rope = rope;

    memset(&input, 0, sizeof(input));
    input.payload = state;
    input.read = read_rope;
    input.encoding = TSInputEncodingUTF8;

    tree = ts_parser_parse(structure->parser, NULL, input);
    if (tree == NULL) {
        free(state);
        return -1;
    }

    root = ts_tree_root_node(tree);
    cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, structure->tags, root);

    stream = stdout;
    found = 0;

    more = ts_query_cursor_next_match(cursor, &match);
    while (more == 1) {
        kind = NULL;
        name[0] = '\0';

        index = 0;
        while (index < match.capture_count) {
            captured = match.captures[index].node;
            capture_name = ts_query_capture_name_for_id(
                structure->tags, match.captures[index].index,
                &capture_length);

            if (strncmp(capture_name, "name", 4) == 0) {
                begin = ts_node_start_byte(captured);
                finish = ts_node_end_byte(captured);
                span = finish - begin;
                if (span >= NAME_CAPACITY) {
                    span = NAME_CAPACITY - 1;
                }
                ok = rope_copy_range(rope, begin, span,
                                     (unsigned char *)name);
                if (ok == 0) {
                    span = 0;
                }
                name[span] = '\0';
            } else if (strncmp(capture_name, "definition.", 11) == 0) {
                kind = capture_name + 11;
                start = ts_node_start_point(captured);
            }
            index = index + 1;
        }

        if (kind != NULL) {
            if (name[0] != '\0') {
                snprintf(label, sizeof(label), "%u\t%s\t%s\n",
                         start.row + 1, kind, name);
                fputs(label, stream);
                found = found + 1;
            }
        }

        more = ts_query_cursor_next_match(cursor, &match);
    }

    ts_query_cursor_delete(cursor);
    ts_tree_delete(tree);
    free(state);
    return found;
}
