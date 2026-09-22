#include "history.h"

#include <string.h>

/* requires: index < HISTORY_CAPACITY + first.
 * ensures:  the result is the ring slot that logical index names. No memory
 *           is accessed.
 */
static uint32_t slot_of(uint32_t first, uint32_t index)
{
    uint32_t position;

    position = first + index;
    if (position >= HISTORY_CAPACITY) {
        position = position - HISTORY_CAPACITY;
    }
    return position;
}

/* requires: *history is allocated and writable.
 * ensures:  history(history, versions) with `versions` empty.
 */
void history_initialize(History *history)
{
    history->first = 0;
    history->count = 0;
}

/* requires: history(history, versions).
 * ensures:  as history.h.
 */
uint32_t history_count(const History *history)
{
    uint32_t total;

    total = history->count;
    return total;
}

/* requires: history(history, versions); *out writable.
 * ensures:  as history.h.
 */
int history_at(const History *history, uint32_t index, Rope *out)
{
    uint32_t total;
    uint32_t first;
    uint32_t slot;

    total = history->count;
    if (index >= total) {
        return 0;
    }
    first = history->first;
    slot = slot_of(first, index);
    memcpy(out, &history->versions[slot].text, sizeof(Rope));
    return 1;
}

/* requires: history(history, versions).
 * ensures:  as history.h.
 */
int history_pin(History *history, uint32_t index)
{
    uint32_t total;
    uint32_t first;
    uint32_t slot;
    uint32_t pins;

    total = history->count;
    if (index >= total) {
        return 0;
    }
    first = history->first;
    slot = slot_of(first, index);
    pins = history->versions[slot].pins;
    history->versions[slot].pins = pins + 1;
    return 1;
}

/* requires: history(history, versions).
 * ensures:  as history.h.
 */
int history_unpin(History *history, uint32_t index)
{
    uint32_t total;
    uint32_t first;
    uint32_t slot;
    uint32_t pins;

    total = history->count;
    if (index >= total) {
        return 0;
    }
    first = history->first;
    slot = slot_of(first, index);
    pins = history->versions[slot].pins;
    if (pins == 0) {
        return 0;
    }
    history->versions[slot].pins = pins - 1;
    return 1;
}

/* requires: history(history, versions); node_pool(pool, allocated).
 * ensures:  as history.h.
 */
int history_retire_oldest(History *history, Pool *pool)
{
    uint32_t total;
    uint32_t first;
    uint32_t slot;
    uint32_t next_slot;
    uint32_t pins;

    total = history->count;
    if (total < 2) {
        return 0;
    }

    first = history->first;
    slot = slot_of(first, 0);

    /* A pinned version stalls the frontier. The history slot is what holds
     * the reference to a snapshot's nodes, so dropping it while a job still
     * wants it would hand those nodes to the collector. */
    pins = history->versions[slot].pins;
    if (pins > 0) {
        return 0;
    }

    next_slot = slot_of(first, 1);

    /* Clearing the slot is the whole of the reclamation. Whatever the
     * retired version alone reached is now unreachable, and the collector
     * finds it without being told where to look — which is precisely what
     * the diff walk could not do correctly for a set of dead roots. */
    (void)pool;

    rope_initialize_empty(&history->versions[slot].text);
    history->versions[slot].pins = 0;
    history->first = next_slot;
    history->count = total - 1;
    return 1;
}

/* requires: history(history, versions); node_pool(pool, live, residual);
 *           rope(version, bytes, share).
 * ensures:  as history.h.
 */
int history_push(History *history, Pool *pool, const Rope *version)
{
    uint32_t total;
    uint32_t first;
    uint32_t slot;
    int      ok;

    total = history->count;
    if (total >= HISTORY_CAPACITY) {
        ok = history_retire_oldest(history, pool);
        if (ok == 0) {
            return 0;
        }
        total = history->count;
    }

    first = history->first;
    slot = slot_of(first, total);
    memcpy(&history->versions[slot].text, version, sizeof(Rope));
    history->versions[slot].pins = 0;
    history->count = total + 1;
    return 1;
}
