#ifndef PAPRI_HISTORY_H
#define PAPRI_HISTORY_H

#include <stdint.h>

#include "pool.h"
#include "rope.h"

/* The version history, and the only place a version is ever dropped.
 *
 * History is strictly linear and retired oldest first. Retiring a version
 * does not free anything — it clears the slot, and the collector takes the
 * nodes that no surviving version still reaches. That division of labour is
 * the point: this module decides WHICH versions papri keeps, and never has
 * to work out which nodes that implies. Working it out is what the pairwise
 * diff walk tried to do, and it got the answer wrong; see CLAUDE.md, "The
 * shortcut that does not work".
 *
 * The one thing that can hold a version alive besides the history is an
 * async job that took a snapshot. Those are counted as PINS, per version,
 * not per node — there are tens of versions and millions of nodes, and that
 * asymmetry is why this is cheaper than reference counting. A pin stops the
 * retirement frontier from advancing past it, and since the history slot is
 * what holds the reference, the pin is also what keeps the collector's
 * hands off the snapshot.
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
 *           already full, the oldest unpinned version is retired first,
 *           which makes its unshared nodes collectable. The result is 1, or 0
 *           when the window is full of pinned versions and the new one
 *           cannot be recorded.
 */
int history_push(History *history, Pool *pool, const Rope *version);

/* requires: history(history, versions); node_pool(pool, allocated).
 * ensures:  when there are at least two versions and the oldest is unpinned,
 *           history(history, versions minus its first) and the result is 1;
 *           the retired version's reference is dropped, so every node it
 *           alone reached becomes collectable — though not necessarily
 *           collected, which happens whenever the collector next runs.
 *           Otherwise nothing changes and the result is 0 — in particular a
 *           pinned oldest version stalls the frontier, which is exactly what
 *           keeps an async snapshot readable.
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

/* requires: history(history, versions); index counts from 0 at the oldest.
 * ensures:  history(history, versions); *out holds that version and the
 *           result is 1; or there is no such version and the result is 0.
 */
int history_at(const History *history, uint32_t index, Rope *out);

#endif /* PAPRI_HISTORY_H */
