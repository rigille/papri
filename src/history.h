#ifndef PAPRI_HISTORY_H
#define PAPRI_HISTORY_H

#include <stdint.h>

#include "pool.h"
#include "rope.h"

/* The version history, and the only place memory is ever given back.
 *
 * History is strictly linear and retired oldest first. That is what makes
 * the cheap reclamation sound: everything reachable from v(k+2) is either
 * fresh or reachable from v(k+1), so by induction the nodes reachable from
 * the oldest version and not from its successor are reachable from no
 * surviving version at all.
 *
 * The one thing that can hold a version alive besides the history is an
 * async job that took a snapshot. Those are counted as PINS, per version,
 * not per node — there are tens of versions and millions of nodes, and that
 * asymmetry is the whole reason this is cheaper than reference counting. A
 * pin does not complicate the diff; it simply stops the retirement frontier
 * from advancing past it.
 */

#define HISTORY_CAPACITY 64

typedef struct Version {
    Rope     text;
    uint32_t pins;
} Version;

typedef struct History {
    Version  versions[HISTORY_CAPACITY];
    uint32_t first;
    uint32_t count;
    uint32_t leaked;    /* retirements whose diff was too wide to walk */
} History;

/* ── Abstract predicates ────────────────────────────────────────────────────
 * history(history, versions)
 *   *history holds `versions`, oldest first, each a rope holding read shares
 *   of pool nodes. Consecutive versions overlap: v(k+1) was derived from
 *   v(k) and shares most of its nodes. A version with a nonzero pin count is
 *   held by something outside the history and may not be retired, nor may
 *   anything older than it.
 */

/* requires: *history is allocated and writable.
 * ensures:  history(history, versions) with `versions` empty.
 */
void history_initialize(History *history);

/* requires: history(history, versions); node_pool(pool, live, residual);
 *           rope(version, bytes, share) derived from the newest version.
 * ensures:  history(history, versions ++ [version]). When the window was
 *           already full, the oldest unpinned version is retired first and
 *           its unshared nodes returned to the pool. The result is 1, or 0
 *           when the window is full of pinned versions and the new one
 *           cannot be recorded.
 */
int history_push(History *history, Pool *pool, const Rope *version);

/* requires: history(history, versions); node_pool(pool, live, residual).
 * ensures:  when there are at least two versions and the oldest is unpinned,
 *           history(history, versions minus its first), every node reachable
 *           from the retired version but not from its successor is returned
 *           to the pool, and the result is 1. Otherwise nothing changes and
 *           the result is 0 — in particular a pinned oldest version stalls
 *           the frontier, which is exactly what keeps an async snapshot
 *           readable.
 */
int history_retire_oldest(History *history, Pool *pool);

/* requires: history(history, versions); index counts from 0 at the oldest.
 * ensures:  history(history, versions) with that version's pin count one
 *           higher, and the result is 1; or no such version, and 0.
 */
int history_pin(History *history, uint32_t index);

/* requires: history(history, versions) where that version is pinned.
 * ensures:  history(history, versions) with its pin count one lower, and the
 *           result is 1; or no such version or it was unpinned, and 0.
 */
int history_unpin(History *history, uint32_t index);

/* requires: history(history, versions).
 * ensures:  history(history, versions); the result is how many there are. No
 *           memory is written.
 */
uint32_t history_count(const History *history);

/* requires: history(history, versions).
 * ensures:  history(history, versions); the result is how many retirements
 *           had to leak because the difference was too wide to walk within
 *           bounded memory. Should be 0; worth asserting in tests. No
 *           memory is written.
 */
uint32_t history_leaked(const History *history);

/* requires: history(history, versions); index counts from 0 at the oldest.
 * ensures:  history(history, versions); *out holds that version and the
 *           result is 1; or there is no such version and the result is 0.
 */
int history_at(const History *history, uint32_t index, Rope *out);

#endif /* PAPRI_HISTORY_H */
