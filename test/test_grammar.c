#include "grammar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The suffix table, tested without loading anything.
 *
 * Which grammar a name selects is the whole of this module's decision, and
 * it is a decision about strings — so none of it needs tree-sitter, a
 * grammar on disk, or a dlopen. That separation is the point of the module:
 * the loading lives behind structure.h, and what is left here can be checked
 * anywhere, including where no grammar is installed.
 */

#define CONFIGURATION_PATH "build/test_grammar_config.txt"

static int         failures;
static GrammarTable table;

/* requires: `condition` is 1 when the property holds; `description` is
 *           NUL-terminated.
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

/* requires: grammar_table(&table, registrations); `name` and `expected` are
 *           NUL-terminated.
 * ensures:  `failures` grows by one when the grammar `name` selects is not
 *           the one registered for `expected`, or when `expected` is empty
 *           and some grammar was selected anyway.
 */
static void expect_language(const char *name, const char *expected,
                            const char *where)
{
    const char *language;
    size_t      count;
    size_t      index;
    size_t      wanted;
    int         difference;

    count = grammar_count(&table);
    index = grammar_lookup(&table, name);
    wanted = strlen(expected);

    if (wanted == 0) {
        if (index != count) {
            failures = failures + 1;
            printf("  FAIL  %s: %s found a grammar it should not have\n",
                   where, name);
        }
        return;
    }

    if (index == count) {
        failures = failures + 1;
        printf("  FAIL  %s: %s found no grammar, wanted %s\n", where, name,
               expected);
        return;
    }

    language = grammar_language_at(&table, index);
    difference = strcmp(language, expected);
    if (difference != 0) {
        failures = failures + 1;
        printf("  FAIL  %s: %s chose %s, wanted %s\n", where, name, language,
               expected);
    }
}

/* requires: nothing.
 * ensures:  `failures` counts the ways the suffix rule misbehaved.
 */
static void test_the_longest_suffix_wins(void)
{
    int before;
    int ok;

    before = failures;
    grammar_initialize(&table);

    ok = grammar_register(&table, ".c", "c", "/grammars/c");
    expect(ok == 1, "registering a suffix succeeds");
    grammar_register(&table, ".h", "c", "/grammars/c");
    grammar_register(&table, ".py", "python", "/grammars/python");

    expect_language("main.c", "c", "exact suffix");
    expect_language("main.h", "c", "a second suffix for one grammar");
    expect_language("script.py", "python", "another language");
    expect_language("/a/long/path/to/main.c", "c", "a suffix in a path");

    /* Nothing claims these, and no catch-all is registered. */
    expect_language("notes.txt", "", "an unclaimed suffix");
    expect_language("Makefile", "", "a name with no suffix");
    expect_language("", "", "a buffer with no name");

    /* `.test.c` is longer than `.c` and must win where both match, which is
     * the whole reason the rule is "longest" and not "first". */
    grammar_register(&table, ".test.c", "cpp", "/grammars/cpp");
    expect_language("rope.test.c", "cpp", "the longer suffix");
    expect_language("rope.c", "c", "the shorter one still applies");

    grammar_release(&table);
    report("the longest registered suffix selects the grammar", before);
}

/* The catch-all exists so that PAPRI_GRAMMAR keeps meaning what it meant —
 * "parse everything with this" — without outranking a suffix registered on
 * purpose.
 *
 * requires: nothing.
 * ensures:  `failures` counts the ways the catch-all misbehaved.
 */
static void test_the_catch_all_loses_to_a_suffix(void)
{
    int before;

    before = failures;
    grammar_initialize(&table);

    grammar_register(&table, "*", "c", "/grammars/c");
    expect_language("notes.txt", "c", "the catch-all claims what nothing else does");
    expect_language("Makefile", "c", "and a name with no suffix");
    expect_language("", "c", "and a buffer with no name");

    grammar_register(&table, ".py", "python", "/grammars/python");
    expect_language("script.py", "python", "a suffix outranks the catch-all");
    expect_language("notes.txt", "c", "the catch-all still takes the rest");

    /* Registration order must not matter: the catch-all registered last
     * still loses. */
    grammar_initialize(&table);
    grammar_register(&table, ".py", "python", "/grammars/python");
    grammar_register(&table, "*", "c", "/grammars/c");
    expect_language("script.py", "python",
                    "a suffix outranks a later catch-all");

    grammar_release(&table);
    report("the catch-all is consulted only when no suffix matches", before);
}

/* requires: nothing.
 * ensures:  `failures` counts the ways re-registering a suffix misbehaved.
 */
static void test_a_suffix_can_be_re_registered(void)
{
    const char *directory;
    size_t      count;
    size_t      index;
    int         before;
    int         difference;

    before = failures;
    grammar_initialize(&table);

    grammar_register(&table, ".c", "c", "/grammars/old");
    count = grammar_count(&table);
    expect(count == 1, "one registration");

    grammar_register(&table, ".c", "c", "/grammars/new");
    count = grammar_count(&table);
    expect(count == 1, "re-registering replaces rather than appends");

    index = grammar_lookup(&table, "main.c");
    directory = grammar_directory_at(&table, index);
    difference = strcmp(directory, "/grammars/new");
    expect(difference == 0, "and the new directory is the one in force");

    grammar_release(&table);
    count = grammar_count(&table);
    expect(count == 0, "releasing empties the table");

    report("re-registering a suffix replaces it", before);
}

/* requires: nothing.
 * ensures:  `failures` counts the ways one line of configuration was
 *           misread.
 */
static void test_configuration_lines(void)
{
    int before;
    int ok;

    before = failures;
    grammar_initialize(&table);

    ok = grammar_register_line(&table, ".c c /grammars/c\n");
    expect(ok == 1, "a well-formed line registers");
    expect_language("main.c", "c", "and takes effect");

    /* A directory may contain blanks, because it is the rest of the line. */
    ok = grammar_register_line(&table, ".py\tpython\t/two words/python  \n");
    expect(ok == 1, "a line with tabs and a spaced directory registers");
    expect_language("a.py", "python", "and takes effect");

    ok = grammar_register_line(&table, "");
    expect(ok == 0, "an empty line registers nothing");
    ok = grammar_register_line(&table, "   \n");
    expect(ok == 0, "a blank line registers nothing");
    ok = grammar_register_line(&table, "# .rs rust /grammars/rust\n");
    expect(ok == 0, "a comment registers nothing");
    ok = grammar_register_line(&table, ".rs\n");
    expect(ok == 0, "a line with no language registers nothing");
    ok = grammar_register_line(&table, ".rs rust\n");
    expect(ok == 0, "a line with no directory registers nothing");

    expect_language("main.rs", "", "and none of those took effect");

    grammar_release(&table);
    report("one line of configuration is read the same everywhere", before);
}

/* requires: standard output is writable and build/ exists.
 * ensures:  `failures` counts the ways a configuration file was misread.
 */
static void test_configuration_file(void)
{
    FILE *file;
    int   before;
    int   applied;

    before = failures;
    grammar_initialize(&table);

    file = fopen(CONFIGURATION_PATH, "wb");
    if (file == NULL) {
        failures = failures + 1;
        printf("  FAIL  could not write %s\n", CONFIGURATION_PATH);
        return;
    }
    fputs("# suffix  language  directory\n", file);
    fputs("\n", file);
    fputs(".c    c       /grammars/c\n", file);
    fputs(".h    c       /grammars/c\n", file);
    fputs(".py   python  /grammars/python\n", file);
    fputs("nonsense\n", file);
    fclose(file);

    applied = grammar_read_configuration(&table, CONFIGURATION_PATH);
    expect(applied == 3, "three of the five lines are registrations");

    expect_language("rope.c", "c", "from the file");
    expect_language("rope.h", "c", "from the file");
    expect_language("build.py", "python", "from the file");
    expect_language("notes.txt", "", "the malformed line registered nothing");

    applied = grammar_read_configuration(&table, "build/no-such-file");
    expect(applied == -1, "a missing file is reported, not fatal");

    grammar_release(&table);
    report("a configuration file registers one grammar per line", before);
}

/* requires: standard output is writable.
 * ensures:  every test above has run and reported; the result is 0 when
 *           `failures` is 0 and 1 otherwise.
 */
int main(void)
{
    failures = 0;

    test_the_longest_suffix_wins();
    test_the_catch_all_loses_to_a_suffix();
    test_a_suffix_can_be_re_registered();
    test_configuration_lines();
    test_configuration_file();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
