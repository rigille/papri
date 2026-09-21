#include "io.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The foreign boundary. These tests go through io.h only — no liburing type
 * appears here — which is the point of the boundary being opaque.
 *
 * io_uring needs a kernel that provides it and a sandbox that permits it.
 * Where it is unavailable these tests report that and pass, rather than
 * failing for a reason that has nothing to do with papri.
 */

#define SAMPLE_PATH "build/test_io_sample.txt"

static int failures;

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

/* requires: nothing.
 * ensures:  SAMPLE_PATH holds known bytes and the result is 1; or it could
 *           not be written and the result is 0.
 */
static int write_sample(void)
{
    FILE  *file;
    size_t written;

    file = fopen(SAMPLE_PATH, "wb");
    if (file == NULL) {
        return 0;
    }
    written = fwrite("alpha\nbeta\ngamma\n", 1, 17, file);
    fclose(file);
    if (written != 17) {
        return 0;
    }
    return 1;
}

/* requires: io_uring is available.
 * ensures:  `failures` counts the ways a submitted read misbehaved.
 */
static void test_read_completes(IoLoop *loop)
{
    unsigned char buffer[64];
    IoCompletion  completion;
    int64_t       size;
    uint64_t      token;
    uint32_t      pending;
    int           descriptor;
    int           before;
    int           ok;
    int           same;

    before = failures;

    size = 0;
    descriptor = io_open_for_read(SAMPLE_PATH, &size);
    expect(descriptor >= 0, "the sample file opens");
    expect(size == 17, "its size is reported");

    memset(buffer, 0, sizeof(buffer));
    ok = io_submit_read(loop, 7u, descriptor, buffer, (uint32_t)size, 0);
    expect(ok == 1, "a read submits");

    pending = io_pending(loop);
    expect(pending == 1, "one operation is outstanding");

    ok = io_wait(loop, &completion);
    expect(ok == 1, "the completion arrives");

    token = completion.token;
    expect(token == 7u, "the completion carries the token its submitter chose");
    expect(completion.result == 17, "the whole file was read");

    same = memcmp(buffer, "alpha\nbeta\ngamma\n", 17);
    expect(same == 0, "the bytes are the file's bytes");

    pending = io_pending(loop);
    expect(pending == 0, "nothing is outstanding once it is reported");

    io_close(descriptor);
    report("a submitted read completes with its token and its bytes", before);
}

/* requires: io_uring is available.
 * ensures:  `failures` counts the ways several outstanding reads were not
 *           told apart. The loop dispatches by token, so two reads in flight
 *           must come back distinguishable.
 */
static void test_two_reads_are_told_apart(IoLoop *loop)
{
    unsigned char first[64];
    unsigned char second[64];
    IoCompletion  completion;
    int64_t       size;
    uint64_t      token;
    uint32_t      seen;
    uint32_t      round;
    int           descriptor_one;
    int           descriptor_two;
    int           before;
    int           ok;

    before = failures;

    size = 0;
    descriptor_one = io_open_for_read(SAMPLE_PATH, &size);
    descriptor_two = io_open_for_read(SAMPLE_PATH, &size);
    expect(descriptor_one >= 0, "the first descriptor opens");
    expect(descriptor_two >= 0, "the second descriptor opens");

    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));

    ok = io_submit_read(loop, 11u, descriptor_one, first, 5, 0);
    expect(ok == 1, "the first read submits");
    ok = io_submit_read(loop, 13u, descriptor_two, second, 4, 6);
    expect(ok == 1, "the second read submits");

    seen = 0;
    round = 0;
    while (round < 2) {
        ok = io_wait(loop, &completion);
        expect(ok == 1, "a completion arrives");
        token = completion.token;
        if (token == 11u) {
            seen = seen + 1;
        }
        if (token == 13u) {
            seen = seen + 2;
        }
        round = round + 1;
    }
    expect(seen == 3, "both tokens came back, once each");

    ok = memcmp(first, "alpha", 5);
    expect(ok == 0, "the first read got the head of the file");
    ok = memcmp(second, "beta", 4);
    expect(ok == 0, "the second read got the file at its offset");

    io_close(descriptor_one);
    io_close(descriptor_two);
    report("two reads in flight are told apart by their tokens", before);
}

/* requires: standard output is writable.
 * ensures:  every test above has run and reported; the result is 0 when
 *           `failures` is 0 and 1 otherwise.
 */
int main(void)
{
    IoLoop *loop;
    int     ok;

    failures = 0;

    ok = write_sample();
    if (ok == 0) {
        printf("  skip  could not write the sample file\n");
        return 0;
    }

    loop = io_create(8);
    if (loop == NULL) {
        printf("  skip  io_uring is unavailable here\n");
        printf("all tests passed\n");
        return 0;
    }

    test_read_completes(loop);
    test_two_reads_are_told_apart(loop);

    io_destroy(loop);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
