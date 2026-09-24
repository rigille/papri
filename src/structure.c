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

    /* A grammar with no tags query is still a grammar: its node types are
     * what `{…}` selects by, and tree-sitter-json ships no tags.scm at all.
     * Only `F` and a dotted selector need the query, and they ask. */
    query_source = NULL;
    query_length = read_tags_query(directory, &query_source);
    if (query_length > 0) {
        error_offset = 0;
        error_type = TSQueryErrorNone;
        structure->tags = ts_query_new(structure->language, query_source,
                                       query_length, &error_offset,
                                       &error_type);
        free(query_source);
    }

    structure->parser = ts_parser_new();
    if (structure->parser == NULL) {
        if (structure->tags != NULL) {
            ts_query_delete(structure->tags);
        }
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
int structure_has_tags(const Structure *structure)
{
    TSQuery *tags;

    tags = structure->tags;
    if (tags == NULL) {
        return 0;
    }
    return 1;
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

/* requires: structure(structure, language); rope(rope, bytes, share);
 *           *state is writable and stays valid until the parse returns.
 * ensures:  rope(rope, bytes, share); the result is the parse tree, which
 *           the caller deletes, or null when the buffer could not be parsed.
 *           The rope is read through a callback and never materialized.
 */
static TSTree *parse_rope(Structure *structure, const Rope *rope,
                          ReadState *state)
{
    TSInput input;
    TSTree *tree;

    state->rope = rope;

    memset(&input, 0, sizeof(input));
    input.payload = state;
    input.read = read_rope;
    input.encoding = TSInputEncodingUTF8;

    tree = ts_parser_parse(structure->parser, NULL, input);
    return tree;
}

/* Insert a span, keeping the decomposition a decomposition.
 *
 * Ascending and pairwise disjoint is a property the type promises, and
 * neither the tree walk nor a query guarantees it on its own — a query may
 * report matches in any order, and two captures may nest. So the span goes
 * in at its sorted position, and one that would overlap a neighbour is
 * dropped rather than allowed to break the invariant.
 *
 * requires: *result writable and holding a decomposition.
 * ensures:  *result holds a decomposition again, with [start, end) added
 *           when it overlapped nothing already there, and the result is 1;
 *           or the decomposition was full and the result is 0.
 */
static int insert_span(Decomposition *result, size_t start, size_t end)
{
    uint32_t count;
    uint32_t index;
    uint32_t position;
    size_t   neighbour_start;
    size_t   neighbour_end;

    count = result->count;
    if (count >= DECOMPOSITION_CAPACITY) {
        return 0;
    }

    index = count;
    while (index > 0) {
        neighbour_start = result->focus[index - 1].start;
        if (neighbour_start <= start) {
            break;
        }
        index = index - 1;
    }

    if (index > 0) {
        neighbour_end = result->focus[index - 1].end;
        if (neighbour_end > start) {
            return 1;
        }
    }
    if (index < count) {
        neighbour_start = result->focus[index].start;
        if (end > neighbour_start) {
            return 1;
        }
    }

    position = count;
    while (position > index) {
        result->focus[position].start = result->focus[position - 1].start;
        result->focus[position].end = result->focus[position - 1].end;
        position = position - 1;
    }
    result->focus[index].start = start;
    result->focus[index].end = end;
    result->count = count + 1;
    return 1;
}

/* requires: structure(structure, language); `selector` is NUL-terminated;
 *           *result writable and holding a decomposition.
 * ensures:  *result also holds the extent of every node the tags query
 *           captures under that name, and the result is 1; or there were
 *           more than a decomposition can hold and the result is 0.
 */
static int select_by_capture(Structure *structure, TSNode root,
                             const char *selector, Decomposition *result)
{
    TSQueryCursor *cursor;
    TSQueryMatch   match;
    TSNode         captured;
    const char    *capture_name;
    size_t         selector_length;
    uint32_t       capture_length;
    uint32_t       index;
    uint32_t       begin;
    uint32_t       finish;
    int            more;
    int            difference;
    int            ok;

    selector_length = strlen(selector);

    cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, structure->tags, root);

    more = ts_query_cursor_next_match(cursor, &match);
    while (more == 1) {
        index = 0;
        while (index < match.capture_count) {
            captured = match.captures[index].node;
            capture_name = ts_query_capture_name_for_id(
                structure->tags, match.captures[index].index,
                &capture_length);

            difference = 1;
            if (capture_length == selector_length) {
                difference = memcmp(capture_name, selector, selector_length);
            }
            if (difference == 0) {
                begin = ts_node_start_byte(captured);
                finish = ts_node_end_byte(captured);
                ok = insert_span(result, begin, finish);
                if (ok == 0) {
                    ts_query_cursor_delete(cursor);
                    return 0;
                }
            }
            index = index + 1;
        }
        more = ts_query_cursor_next_match(cursor, &match);
    }

    ts_query_cursor_delete(cursor);
    return 1;
}

/* A pre-order walk that does not descend into what it selected, so nested
 * nodes of one type yield the outermost and the spans come out ascending
 * and disjoint without any sorting.
 *
 * requires: `selector` is NUL-terminated; *result writable and holding a
 *           decomposition.
 * ensures:  *result also holds the extent of every outermost node whose type
 *           is `selector`, and the result is 1; or there were more than a
 *           decomposition can hold and the result is 0.
 */
static int select_by_node_type(TSNode root, const char *selector,
                               Decomposition *result)
{
    TSTreeCursor cursor;
    TSNode       node;
    const char  *type;
    uint32_t     begin;
    uint32_t     finish;
    int          running;
    int          matched;
    int          moved;
    int          difference;
    int          ok;

    cursor = ts_tree_cursor_new(root);

    running = 1;
    while (running == 1) {
        node = ts_tree_cursor_current_node(&cursor);
        type = ts_node_type(node);
        difference = strcmp(type, selector);

        matched = 0;
        if (difference == 0) {
            matched = 1;
            begin = ts_node_start_byte(node);
            finish = ts_node_end_byte(node);
            ok = insert_span(result, begin, finish);
            if (ok == 0) {
                ts_tree_cursor_delete(&cursor);
                return 0;
            }
        }

        moved = 0;
        if (matched == 0) {
            moved = ts_tree_cursor_goto_first_child(&cursor);
        }
        while (moved == 0 && running == 1) {
            moved = ts_tree_cursor_goto_next_sibling(&cursor);
            if (moved == 0) {
                moved = ts_tree_cursor_goto_parent(&cursor);
                if (moved == 0) {
                    running = 0;
                } else {
                    /* At the parent, which has already been visited: keep
                     * climbing until a sibling turns up. */
                    moved = 0;
                }
            }
        }
    }

    ts_tree_cursor_delete(&cursor);
    return 1;
}

/* requires: as structure.h.
 * ensures:  as structure.h.
 */
int structure_select(Structure *structure, const Rope *rope,
                     const char *selector, Decomposition *result)
{
    ReadState  *state;
    TSTree     *tree;
    TSNode      root;
    const char *dot;
    TSQuery    *tags;
    uint32_t    count;
    int         ok;

    result->count = 0;

    /* No tree-sitter node type contains a dot, so the dot is what tells a
     * tags-query capture from a grammar node type. */
    dot = strchr(selector, '.');
    if (dot != NULL) {
        tags = structure->tags;
        if (tags == NULL) {
            return -2;
        }
    }

    state = malloc(sizeof(ReadState));
    if (state == NULL) {
        return -1;
    }

    tree = parse_rope(structure, rope, state);
    if (tree == NULL) {
        free(state);
        return -1;
    }
    root = ts_tree_root_node(tree);

    if (dot == NULL) {
        ok = select_by_node_type(root, selector, result);
    } else {
        ok = select_by_capture(structure, root, selector, result);
    }

    ts_tree_delete(tree);
    free(state);

    if (ok == 0) {
        result->count = 0;
        return -1;
    }
    count = result->count;
    return (int)count;
}

/* requires: as structure.h.
 * ensures:  as structure.h.
 */
int structure_list_definitions(Structure *structure, const Rope *rope)
{
    ReadState     *state;
    TSTree        *tree;
    TSNode         root;
    TSQueryCursor *cursor;
    TSQueryMatch   match;
    TSNode         captured;
    TSPoint        start;
    TSQuery       *tags;
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

    tags = structure->tags;
    if (tags == NULL) {
        return -2;
    }

    state = malloc(sizeof(ReadState));
    if (state == NULL) {
        return -1;
    }

    tree = parse_rope(structure, rope, state);
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
