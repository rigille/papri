#ifndef PAPRI_IO_H
#define PAPRI_IO_H

#include <stdint.h>

/* The foreign boundary: io_uring, and the only part of papri that cannot go
 * through the normal-form gate.
 *
 * `liburing.h` pulls in `stdatomic.h`, and CompCert's front end does not
 * accept `_Atomic` — clightgen stops at the typedef. So src/io.c is exempt
 * from `make normalform`, and this interface exists to keep that exemption
 * as small as it can be: the IoLoop is opaque, no liburing type appears
 * here, and every caller above this line stays inside the subset and stays
 * gated.
 *
 * Everything runs on one thread. The loop blocks in exactly one place —
 * io_wait — and dispatches by a token the submitter chose, which is the
 * shape kelci's frame pacer uses over ALooper.
 */

#define IO_TOKEN_INPUT 0u

typedef struct IoLoop IoLoop;

typedef struct IoCompletion {
    uint64_t token;
    int32_t  result;   /* bytes transferred, or the negated errno */
} IoCompletion;

/* ── Abstract predicates ────────────────────────────────────────────────────
 * io_loop(loop, pending)
 *   Owns *loop and its submission and completion rings. `pending` is the set
 *   of submitted operations that have not yet been reported by io_wait, each
 *   identified by the token its submitter chose and each holding a write
 *   share of the buffer it was given. That share is returned to the caller
 *   when io_wait reports the operation, and not before — a buffer handed to
 *   io_submit_read must stay put and stay untouched until then.
 */

/* requires: depth is a power of two, at least 1.
 * ensures:  io_loop(loop, pending) with `pending` empty and the result
 *           points at it; or the ring could not be created and the result is
 *           null.
 */
IoLoop *io_create(uint32_t depth);

/* requires: io_loop(loop, pending).
 * ensures:  the ring is torn down and *loop freed. Any buffers still held by
 *           `pending` are released back to the caller unreported.
 */
void io_destroy(IoLoop *loop);

/* requires: io_loop(loop, pending); `descriptor` is open for reading; holds
 *           a write share of `capacity` bytes at `buffer`, which the caller
 *           gives up until io_wait reports this token.
 * ensures:  io_loop(loop, pending ++ [token]) and the result is 1; or the
 *           ring was full, `pending` is unchanged, the buffer share is
 *           returned, and the result is 0.
 */
int io_submit_read(IoLoop *loop, uint64_t token, int descriptor,
                   void *buffer, uint32_t capacity, int64_t offset);

/* requires: io_loop(loop, pending) with `pending` non-empty; *completion
 *           writable.
 * ensures:  blocks until an operation finishes; io_loop(loop, pending minus
 *           that one), *completion names it and its outcome, the write share
 *           of its buffer is returned to the caller, and the result is 1; or
 *           the wait failed and the result is 0.
 */
int io_wait(IoLoop *loop, IoCompletion *completion);

/* requires: io_loop(loop, pending).
 * ensures:  io_loop(loop, pending); the result is how many operations are
 *           still outstanding. No memory is written.
 */
uint32_t io_pending(const IoLoop *loop);

/* Opening and sizing are synchronous. They are two syscalls on a path, not
 * the bulk of the work, and doing them through the ring would double the
 * state machine for no measurable gain.
 *
 * requires: `path` is NUL-terminated; *size_slot writable.
 * ensures:  the file is open for reading, *size_slot is its size in bytes,
 *           and the result is the descriptor; or it could not be opened and
 *           the result is negative.
 */
int io_open_for_read(const char *path, int64_t *size_slot);

/* requires: `descriptor` was returned by io_open_for_read and no submitted
 *           operation still names it.
 * ensures:  it is closed.
 */
void io_close(int descriptor);

#endif /* PAPRI_IO_H */
