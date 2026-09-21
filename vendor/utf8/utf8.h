#ifndef PAPRI_VENDOR_UTF8_H
#define PAPRI_VENDOR_UTF8_H

/* Declarations for the vendored UTF-8 decoder in utf8.c.
 *
 * utf8.c is vendored byte-identical and is NOT edited — see README.md in this
 * directory. It carries no header of its own, so this one is ours: the
 * declarations and the specs below are written here, against the funspecs
 * proved in ../tools/coq/VerifiableC/Utf8.v.
 *
 * Because utf8.c does not include this header, the enum below is a second
 * definition of the one in utf8.c. They must agree. If utf8.c is ever
 * re-vendored from a newer upstream, check this enum first.
 */

#include <stdint.h>

#define UTF8_ACCEPT 0
#define UTF8_REJECT 12

enum utf8_decode_error {
    UTF8_OK = 0,
    UTF8_ERROR,
    UTF8_NOT_ENOUGH_BYTES,
    UTF8_TOO_MANY_BYTES,
    UTF8_INVALID_START_HEADER,
    UTF8_INVALID_CONTINUATION_HEADER,
    UTF8_CODEPOINT_TOO_BIG,
};

/* ── Abstract predicates ────────────────────────────────────────────────────
 * decoder_state(state, carry, partial)
 *   A pure value, owning nothing. `state` is a DFA state — UTF8_ACCEPT when
 *   no code point is in progress, UTF8_REJECT once the input is known
 *   invalid, and otherwise an intermediate state. `carry` holds the bits
 *   accumulated so far for the code point `partial` under construction.
 *   The pair advances only through next_state.
 */

/* The resumable primitive. This is the one to build on: it carries no
 * buffer, so a caller holding (state, carry) can decode across a chunk
 * boundary without copying bytes into a contiguous staging area.
 *
 * requires: decoder_state(state, carry, partial); byte is a byte value;
 *           holds a write share of one uint32_t at codepoint_pointer whose
 *           value is carry.
 * ensures:  decoder_state(result, carry', partial') is the DFA's successor
 *           for `byte`, *codepoint_pointer is carry', and result is
 *           UTF8_REJECT exactly when byte cannot continue partial. When
 *           result is UTF8_ACCEPT, carry' is the completed code point.
 */
uint32_t next_state(uint32_t state, uint8_t byte, uint32_t *codepoint_pointer);

/* Decode one code point from a flat buffer.
 *
 * Note the resumption hazard, which is why next_state exists above: when the
 * buffer ends part way through a code point this returns
 * UTF8_NOT_ENOUGH_BYTES having ALREADY advanced *index past the bytes it
 * consumed, and its DFA state is a local that is then lost. It cannot be
 * called again to finish that code point.
 *
 * requires: holds any read share of `length` bytes at text; holds a write
 *           share of one uint64_t at index whose value is at most length;
 *           holds a write share of one uint32_t at codepoint_out.
 * ensures:  the read share of text is returned unchanged. When the bytes
 *           from *index decode to a code point, *codepoint_out is that code
 *           point, *index advances past exactly its bytes, and the result is
 *           UTF8_OK. When they are not valid UTF-8 the result is UTF8_ERROR;
 *           when the buffer ends first the result is UTF8_NOT_ENOUGH_BYTES.
 *           In both failing cases *index may have advanced and
 *           *codepoint_out is unspecified.
 */
enum utf8_decode_error next_codepoint(uint8_t *text, uint64_t length,
                                      uint64_t *index, uint32_t *codepoint_out);

/* requires: holds any read share of `length` bytes at text.
 * ensures:  the read share is returned unchanged; the result is the number of
 *           code points in the longest valid UTF-8 prefix of those bytes.
 */
uint64_t count_codepoints(uint8_t *text, uint64_t length);

#endif /* PAPRI_VENDOR_UTF8_H */
