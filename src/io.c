/* The foreign boundary. See io.h for why this file is exempt from the
 * normal-form gate: liburing.h reaches stdatomic.h, and CompCert's front end
 * stops at `_Atomic`. Everything here stays as thin as it can be so that the
 * exemption covers as little as possible; the house naming and spec rules
 * still apply, and `make lint` still checks them.
 */

#include "io.h"

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

struct IoLoop {
    struct io_uring ring;
    uint32_t        pending;
};

/* requires: as io.h.
 * ensures:  as io.h.
 */
IoLoop *io_create(uint32_t depth)
{
    IoLoop *loop;
    int     outcome;

    loop = malloc(sizeof(IoLoop));
    if (loop == NULL) {
        return NULL;
    }

    outcome = io_uring_queue_init(depth, &loop->ring, 0);
    if (outcome < 0) {
        free(loop);
        return NULL;
    }

    loop->pending = 0;
    return loop;
}

/* requires: as io.h.
 * ensures:  as io.h.
 */
void io_destroy(IoLoop *loop)
{
    if (loop == NULL) {
        return;
    }
    io_uring_queue_exit(&loop->ring);
    free(loop);
}

/* requires: as io.h.
 * ensures:  as io.h.
 */
int io_submit_read(IoLoop *loop, uint64_t token, int descriptor,
                   void *buffer, uint32_t capacity, int64_t offset)
{
    struct io_uring_sqe *entry;
    uint32_t             pending;
    int                  submitted;

    entry = io_uring_get_sqe(&loop->ring);
    if (entry == NULL) {
        return 0;
    }

    io_uring_prep_read(entry, descriptor, buffer, capacity,
                       (unsigned long long)offset);
    io_uring_sqe_set_data64(entry, token);

    submitted = io_uring_submit(&loop->ring);
    if (submitted < 0) {
        return 0;
    }

    pending = loop->pending;
    loop->pending = pending + 1;
    return 1;
}

/* requires: as io.h.
 * ensures:  as io.h.
 */
int io_wait(IoLoop *loop, IoCompletion *completion)
{
    struct io_uring_cqe *entry;
    uint32_t             pending;
    int                  outcome;

    /* A signal turns the wait into -EINTR with no completion consumed. The
     * old code read any negative outcome as "nothing arrived" and returned
     * 0, which retires the loop and strands the read that was in flight —
     * its completion never seen, `pending` never decremented, and a `q`
     * that waits for outstanding work hanging forever. Nothing in a
     * single-threaded papri sends signals today, so it never fired; a
     * second thread would make the collector's stop-the-world do it on the
     * first collection during a read. Retry instead. */
    entry = NULL;
    outcome = io_uring_wait_cqe(&loop->ring, &entry);
    while (outcome == -EINTR) {
        entry = NULL;
        outcome = io_uring_wait_cqe(&loop->ring, &entry);
    }
    if (outcome < 0) {
        return 0;
    }
    if (entry == NULL) {
        return 0;
    }

    completion->token = io_uring_cqe_get_data64(entry);
    completion->result = entry->res;
    io_uring_cqe_seen(&loop->ring, entry);

    pending = loop->pending;
    if (pending > 0) {
        loop->pending = pending - 1;
    }
    return 1;
}

/* requires: as io.h.
 * ensures:  as io.h.
 */
uint32_t io_pending(const IoLoop *loop)
{
    uint32_t total;

    total = loop->pending;
    return total;
}

/* requires: as io.h.
 * ensures:  as io.h.
 */
int io_open_for_read(const char *path, int64_t *size_slot)
{
    struct stat details;
    int         descriptor;
    int         outcome;

    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return -1;
    }

    outcome = fstat(descriptor, &details);
    if (outcome < 0) {
        close(descriptor);
        return -1;
    }

    *size_slot = (int64_t)details.st_size;
    return descriptor;
}

/* requires: as io.h.
 * ensures:  as io.h.
 */
void io_close(int descriptor)
{
    if (descriptor < 0) {
        return;
    }
    close(descriptor);
}
