#include "pool.h"
#include "rope.h"
#include "structure.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Structural printing. The grammar is loaded at run time from
 * PAPRI_GRAMMAR, so where none is configured these tests say so and pass
 * rather than failing for a reason that has nothing to do with papri.
 *
 * The property worth pinning down is that the parse reads the rope through
 * a callback and never materializes it — which is only observable by
 * parsing a buffer bigger than the read window and checking that
 * definitions past the window boundary are still found.
 */

#define CAPTURE_PATH "build/test_structure_out.txt"
#define CAPTURE_CAPACITY 262144u

static int   failures;
static Pool  pool;
static char  captured[CAPTURE_CAPACITY];
static char  source[262144];
static int   saved_output;

/* requires: `condition` is 1 when the property holds.
 * ensures:  `failures` grows by one and a line is written when it does not.
 */
static void expect(int condition, const char *description)
{
    if (condition == 0) {
        failures = failures + 1;
        printf("  FAIL  %s\n", description);
    }
}

/* requires: nothing.
 * ensures:  a line naming the outcome of a whole test is written.
 */
static void report(const char *name, int before)
{
    int now;

    now = failures;
    if (now == before) {
        printf("  ok    %s\n", name);
        return;
    }
    printf("  FAIL  %s\n", name);
}

/* requires: standard output is not already captured.
 * ensures:  standard output is redirected and the result is 1, or 0.
 */
static int capture_begin(void)
{
    int target;

    fflush(stdout);
    saved_output = dup(1);
    if (saved_output < 0) {
        return 0;
    }
    target = open(CAPTURE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (target < 0) {
        return 0;
    }
    dup2(target, 1);
    close(target);
    return 1;
}

/* requires: capture_begin succeeded.
 * ensures:  standard output is restored and `captured` holds what was
 *           written, NUL-terminated.
 */
static void capture_end(void)
{
    FILE  *file;
    size_t length;

    fflush(stdout);
    dup2(saved_output, 1);
    close(saved_output);

    captured[0] = '\0';
    file = fopen(CAPTURE_PATH, "rb");
    if (file == NULL) {
        return;
    }
    length = fread(captured, 1, CAPTURE_CAPACITY - 1, file);
    captured[length] = '\0';
    fclose(file);
}

/* requires: `needle` is NUL-terminated.
 * ensures:  the result is 1 when the captured output contains it.
 */
static int captured_has(const char *needle)
{
    const char *found;

    found = strstr(captured, needle);
    if (found == NULL) {
        return 0;
    }
    return 1;
}

/* requires: structure(structure, language); node_pool is live.
 * ensures:  `failures` counts the definitions that were missed or
 *           misreported.
 */
static void test_definitions_are_found(Structure *structure)
{
    static const char text[] =
        "#include <stdio.h>\n"
        "\n"
        "struct Point { int x; int y; };\n"
        "\n"
        "static int add(int left, int right)\n"
        "{\n"
        "    return left + right;\n"
        "}\n"
        "\n"
        "int main(void)\n"
        "{\n"
        "    return add(1, 2);\n"
        "}\n";
    Rope     rope;
    uint32_t length;
    int      before;
    int      ok;

    before = failures;
    length = (uint32_t)(sizeof(text) - 1);

    ok = rope_from_bytes(&pool, (const unsigned char *)text, length, &rope);
    expect(ok == 1, "the source rope is built");

    ok = capture_begin();
    expect(ok == 1, "output can be captured");
    ok = structure_list_definitions(structure, &rope);
    capture_end();

    expect(ok >= 3, "at least the struct and both functions are found");
    expect(captured_has("add"), "a static function is found");
    expect(captured_has("main"), "main is found");
    expect(captured_has("Point"), "a struct is found");
    expect(captured_has("5\tfunction\tadd"),
           "a definition carries its line and its kind");

    report("definitions are found with their lines and kinds", before);
}

/* The parse reads the rope through a callback in windows, so a definition
 * past the first window is the case that proves the buffer is never
 * materialized.
 *
 * requires: structure(structure, language); node_pool is live.
 * ensures:  `failures` counts the ways a large buffer was mis-parsed.
 */
static void test_parsing_spans_many_windows(Structure *structure)
{
    Rope     rope;
    uint32_t length;
    uint32_t index;
    int      written;
    int      before;
    int      ok;

    before = failures;

    /* Enough padding functions to run well past any single read window. */
    length = 0;
    index = 0;
    while (index < 900) {
        written = snprintf(source + length, sizeof(source) - length,
                           "static int padding_%u(void) { return %u; }\n",
                           index, index);
        length = length + (uint32_t)written;
        index = index + 1;
    }
    written = snprintf(source + length, sizeof(source) - length,
                       "int the_last_one(void) { return 0; }\n");
    length = length + (uint32_t)written;

    expect(length > 16384, "the source is much larger than a read window");

    ok = rope_from_bytes(&pool, (const unsigned char *)source, length,
                         &rope);
    expect(ok == 1, "the large rope is built");

    ok = capture_begin();
    expect(ok == 1, "output can be captured");
    ok = structure_list_definitions(structure, &rope);
    capture_end();

    expect(ok >= 901, "every definition is found");
    expect(captured_has("padding_0"), "the first one is found");
    expect(captured_has("padding_899"), "one deep in the buffer is found");
    expect(captured_has("the_last_one"),
           "the last one, far past the first window, is found");

    report("a buffer spanning many read windows parses whole", before);
}

/* requires: standard output is writable.
 * ensures:  every test above has run and reported; the result is 0 when
 *           `failures` is 0 and 1 otherwise.
 */
int main(void)
{
    Structure  *structure;
    const char *directory;
    const char *language;
    int         ok;

    failures = 0;

    directory = getenv("PAPRI_GRAMMAR");
    if (directory == NULL) {
        printf("  skip  PAPRI_GRAMMAR is not set; no grammar to test with\n");
        printf("all tests passed\n");
        return 0;
    }
    language = getenv("PAPRI_LANGUAGE");
    if (language == NULL) {
        language = "c";
    }

    ok = pool_initialize(&pool);
    if (ok == 0) {
        printf("could not initialize the pool\n");
        return 1;
    }

    structure = structure_create(directory, language);
    if (structure == NULL) {
        printf("  skip  the grammar at PAPRI_GRAMMAR would not load\n");
        printf("all tests passed\n");
        pool_release(&pool);
        return 0;
    }

    test_definitions_are_found(structure);
    test_parsing_spans_many_windows(structure);

    structure_destroy(structure);
    pool_release(&pool);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
