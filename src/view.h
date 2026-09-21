#ifndef PAPRI_VIEW_H
#define PAPRI_VIEW_H

#include <stdint.h>

#include "rope.h"

/* Views: how a range of the buffer is rendered when it is printed.
 *
 * A buffer is bytes. It is not assumed to be text, because sometimes it is
 * not. So decoding is a VIEW over the bytes rather than a property of the
 * buffer — an optic, partial where the bytes are not valid UTF-8 — and a
 * binary buffer simply never gets that one. Text, hex and code points are
 * three ways of looking at one sequence, and nothing about the buffer
 * changes when you switch between them.
 *
 * Every view streams in windows and never materializes the range, which is
 * the whole reason the buffer is a rope. The code point view decodes across
 * window boundaries with nothing carried but the DFA state and the partial
 * code point; see vendor/utf8.
 */

/* ── Abstract predicates ────────────────────────────────────────────────────
 * Views own nothing. Each takes a read share of the rope, writes to
 * standard output, and returns the share.
 */

/* requires: rope(rope, bytes, share); start <= end <= |bytes|.
 * ensures:  rope(rope, bytes, share); those bytes are written verbatim to
 *           standard output.
 */
void view_write_text(const Rope *rope, uint32_t start, uint32_t end);

/* requires: rope(rope, bytes, share); start <= end <= |bytes|.
 * ensures:  rope(rope, bytes, share); those bytes are written to standard
 *           output as a hex dump — sixteen per line, each line prefixed with
 *           its absolute byte offset and followed by the printable ASCII.
 */
void view_write_hex(const Rope *rope, uint32_t start, uint32_t end);

/* requires: rope(rope, bytes, share); start <= end <= |bytes|.
 * ensures:  rope(rope, bytes, share); those bytes are decoded as UTF-8 and
 *           written to standard output one code point per line, as its
 *           offset, its U+ value, and the bytes it occupied. A byte that
 *           cannot continue a code point is reported as invalid and
 *           decoding resumes at the byte after it.
 */
void view_write_codepoints(const Rope *rope, uint32_t start, uint32_t end);

#endif /* PAPRI_VIEW_H */
