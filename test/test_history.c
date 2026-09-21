#include "address.h"
#include "command.h"
#include "history.h"
#include "pool.h"
#include "rope.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Reclamation. The share algebra says WHEN a node may go back — once every
 * share of it has rejoined to the full share — but shares are ghost state
 * and cannot say WHICH nodes those are. The reclamation walk is what finds
 * them, and the obligation connecting the two is that it returns exactly the
 * nodes whose shares have rejoined.
 *
 * The test below is that obligation made executable: after retiring the
 * history down to the versions still held, the pool's live bytes must equal
 * the memory those versions actually occupy. Not less, which would mean a
 * node was freed while still reachable; not more, which would mean a dead
 * node was kept.
 */

static int      failures;
static Pool     pool;
static History  history;
static Editor   editor;

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

/* requires: node_pool(pool, live, residual); `text` is NUL-terminated.
 * ensures:  rope(out, bytes, share) holding those bytes.
 */
static void build(Pool *target, const char *text, Rope *out)
{
    uint32_t length;
    size_t   wanted;
    int      ok;

    wanted = strlen(text);
    length = (uint32_t)wanted;
    ok = rope_from_bytes(target, (const unsigned char *)text, length, out);
    if (ok == 0) {
        failures = failures + 1;
        printf("  FAIL  could not build a rope\n");
    }
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the reclamation properties that did not hold.
 */
static void test_retirement_frees_the_difference(void)
{
    Rope          version;
    Rope          edited;
    Rope          survivor;
    Decomposition foci;
    unsigned char replacement[8];
    size_t        live_bytes;
    size_t        occupied;
    uint32_t      step;
    uint32_t      remaining;
    uint32_t      leaked;
    int           before;
    int           ok;

    before = failures;

    ok = pool_initialize(&pool);
    expect(ok == 1, "the pool initializes");
    history_initialize(&history);

    build(&pool, "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n", &version);
    ok = history_push(&history, &pool, &version);
    expect(ok == 1, "the first version is recorded");

    replacement[0] = 'X';

    step = 0;
    while (step < 50) {
        foci.count = 1;
        foci.focus[0].start = 0;
        foci.focus[0].end = 1;

        ok = address_replace_all(&pool, &version, &foci, replacement, 1,
                                 &edited);
        if (ok == 0) {
            failures = failures + 1;
            printf("  FAIL  edit %u refused\n", step);
            report("retirement frees exactly the difference", before);
            return;
        }
        ok = history_push(&history, &pool, &edited);
        expect(ok == 1, "each version is recorded");
        memcpy(&version, &edited, sizeof(Rope));
        step = step + 1;
    }

    /* Retire everything but the newest. */
    remaining = history_count(&history);
    while (remaining > 1) {
        ok = history_retire_oldest(&history, &pool);
        if (ok == 0) {
            break;
        }
        remaining = history_count(&history);
    }

    remaining = history_count(&history);
    expect(remaining == 1, "the history retires down to one version");

    leaked = history_leaked(&history);
    expect(leaked == 0, "no retirement had to leak");

    ok = history_at(&history, 0, &survivor);
    expect(ok == 1, "the survivor is readable");

    ok = rope_check_invariants(&survivor);
    expect(ok == 1, "the survivor is structurally sound after reclamation");

    live_bytes = pool_live(&pool);
    occupied = rope_allocated_bytes(&survivor);

    if (live_bytes != occupied) {
        failures = failures + 1;
        printf("  FAIL  pool holds %zu live bytes, the survivor occupies %zu\n",
               live_bytes, occupied);
        printf("  ...   %s\n",
               live_bytes > occupied ? "dead nodes were kept"
                                     : "a reachable node was freed");
    }

    pool_release(&pool);
    report("retirement frees exactly the difference", before);
}

/* requires: node_pool(pool, live, residual).
 * ensures:  `failures` counts the pin properties that did not hold. This is
 *           what keeps an async job's snapshot readable.
 */
static void test_a_pin_stalls_the_frontier(void)
{
    Rope          version;
    Rope          edited;
    Rope          pinned;
    Rope          check;
    Decomposition foci;
    unsigned char replacement[8];
    unsigned char seen[64];
    uint32_t      step;
    uint32_t      count;
    uint32_t      length;
    int           before;
    int           ok;
    int           same;

    before = failures;

    ok = pool_initialize(&pool);
    expect(ok == 1, "the pool initializes");
    history_initialize(&history);

    build(&pool, "alpha\nbeta\ngamma\n", &version);
    history_push(&history, &pool, &version);

    /* An async job takes a snapshot of version 0. */
    ok = history_pin(&history, 0);
    expect(ok == 1, "the oldest version can be pinned");
    ok = history_at(&history, 0, &pinned);
    expect(ok == 1, "the pinned version is readable");
    length = rope_byte_count(&pinned);

    replacement[0] = 'Z';
    step = 0;
    while (step < 10) {
        foci.count = 1;
        foci.focus[0].start = 0;
        foci.focus[0].end = 1;
        ok = address_replace_all(&pool, &version, &foci, replacement, 1,
                                 &edited);
        expect(ok == 1, "an edit over a pinned history succeeds");
        history_push(&history, &pool, &edited);
        memcpy(&version, &edited, sizeof(Rope));
        step = step + 1;
    }

    ok = history_retire_oldest(&history, &pool);
    expect(ok == 0, "a pinned oldest version refuses to retire");

    count = history_count(&history);
    expect(count == 11, "nothing was retired while the pin was held");

    /* The snapshot must still read correctly: its nodes cannot have been
     * recycled under it. */
    ok = rope_copy_range(&pinned, 0, length, seen);
    expect(ok == 1, "the snapshot is still readable");
    same = memcmp(seen, "alpha\nbeta\ngamma\n", length);
    expect(same == 0, "the snapshot still holds its own bytes");
    ok = rope_check_invariants(&pinned);
    expect(ok == 1, "the snapshot is still structurally sound");

    ok = history_unpin(&history, 0);
    expect(ok == 1, "the pin can be released");
    ok = history_retire_oldest(&history, &pool);
    expect(ok == 1, "retirement resumes once the pin is gone");

    ok = history_at(&history, 0, &check);
    expect(ok == 1, "a version remains after retiring");

    pool_release(&pool);
    report("a pinned version stalls the retirement frontier", before);
}

/* requires: node_pool is live.
 * ensures:  `failures` counts the ways the editor's memory grew without
 *           bound over a long editing session.
 */
static void test_editor_memory_stays_bounded(void)
{
    char     line[64];
    size_t   after_warmup;
    size_t   at_end;
    size_t   growth;
    uint32_t step;
    int      before;
    int      ok;

    before = failures;

    ok = editor_initialize(&editor);
    expect(ok == 1, "the editor initializes");

    ok = editor_execute(&editor, "%c seed line one\\nseed line two\\n");
    expect(ok == 1, "the buffer is seeded");

    /* Fill the history window first, so what follows is steady state. */
    step = 0;
    while (step < HISTORY_CAPACITY + 10) {
        snprintf(line, sizeof(line), "1i x");
        editor_execute(&editor, line);
        step = step + 1;
    }
    after_warmup = pool_live(&editor.pool);

    step = 0;
    while (step < 400) {
        snprintf(line, sizeof(line), "1i y");
        editor_execute(&editor, line);
        step = step + 1;
    }
    at_end = pool_live(&editor.pool);

    printf("  note  live bytes after warmup %zu, after 400 more edits %zu\n",
           after_warmup, at_end);

    /* Each edit adds a character, so the buffer itself grows; what must not
     * grow is the per-edit residue. 400 edits adding one byte each cannot
     * legitimately cost more than a few hundred KiB. */
    growth = at_end - after_warmup;
    expect(at_end >= after_warmup, "memory accounting does not go backwards");
    expect(growth < 4u * 1024u * 1024u,
           "a long editing session does not grow without bound");

    editor_release(&editor);
    report("editor memory stays bounded over a long session", before);
}

/* requires: standard output is writable.
 * ensures:  every test above has run and reported; the result is 0 when
 *           `failures` is 0 and 1 otherwise.
 */
int main(void)
{
    failures = 0;

    test_retirement_frees_the_difference();
    test_a_pin_stalls_the_frontier();
    test_editor_memory_stays_bounded();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
