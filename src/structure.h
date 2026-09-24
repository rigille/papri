#ifndef PAPRI_STRUCTURE_H
#define PAPRI_STRUCTURE_H

#include <stdint.h>

#include "address.h"
#include "rope.h"

/* Structural printing: "show me the functions in this file" without folding
 * anything.
 *
 * This is the reason the transcript model earns its keep. A screen editor
 * needs folding because it must show you the buffer and the buffer is too
 * big; a teletype shows you whatever you asked for, so asking for the
 * definitions is just another thing to print. Nothing is hidden, because
 * nothing was being shown.
 *
 * The parse comes from tree-sitter, loaded at run time rather than linked:
 * a grammar is a shared object plus, usually, a `queries/tags.scm`, so any
 * language that ships one works without papri knowing anything about it.
 * That is also why this is the second foreign boundary, alongside src/io.c.
 *
 * Which grammar a buffer gets is not decided here — see src/grammar.h. This
 * module is one loaded grammar and what can be asked of it.
 *
 * The buffer is never materialized to parse it. tree-sitter reads through a
 * callback, so it is fed rope windows directly.
 */

typedef struct Structure Structure;

/* ── Abstract predicates ────────────────────────────────────────────────────
 * structure(structure, language, tagged)
 *   Owns *structure: a loaded grammar, a parser, and — when `tagged` — the
 *   grammar's tags query. The grammar's shared object stays mapped for the
 *   lifetime of the handle.
 */

/* The tags query is optional, and that is not a convenience: a grammar with
 * no `queries/tags.scm` (tree-sitter-json ships none) still has a complete
 * node vocabulary, so `{expression_statement}` works against it. Only the
 * two things that read the query — `F`, and a dotted selector — need it.
 *
 * requires: `directory` holds a tree-sitter grammar: a `parser` shared
 *           object, and optionally a `queries/tags.scm`; `language` names
 *           it, so that the entry point is tree_sitter_<language>.
 * ensures:  structure(structure, language, tagged) where `tagged` says
 *           whether a tags query was found and compiled, and the result
 *           points at it; or the shared object or its entry point could not
 *           be found and the result is null.
 */
Structure *structure_create(const char *directory, const char *language);

/* requires: structure(structure, language, tagged).
 * ensures:  structure(structure, language, tagged); the result is 1 when
 *           `tagged`. No memory is written.
 */
int structure_has_tags(const Structure *structure);

/* requires: structure(structure, language, tagged).
 * ensures:  the grammar is unloaded and *structure freed.
 */
void structure_destroy(Structure *structure);

/* requires: structure(structure, language, tagged); rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); every definition the grammar's tags
 *           query finds is written to standard output as its line number,
 *           its kind and its name, in source order. The result is how many
 *           were found; or -1 when the buffer could not be parsed, or -2
 *           when the grammar is not `tagged` and so has no query to run.
 */
int structure_list_definitions(Structure *structure, const Rope *rope);

/* Selecting by grammar: the address form that names a shape rather than a
 * position.
 *
 * `/text/` finds bytes that look alike; this finds bytes that MEAN alike,
 * which is the thing a byte search cannot do however good its patterns get.
 * The result is an ordinary decomposition, so every verb already works on
 * it: `{definition.function}p` prints the functions, `{comment}d` deletes
 * the comments, `{string_literal}c L\1` prefixes every string literal.
 *
 * A selector is read one of two ways, told apart by a dot, which no
 * tree-sitter node type contains:
 *
 *   `definition.function`  a capture in the grammar's queries/tags.scm
 *   `function_definition`  a node type in the grammar itself
 *
 * The first is portable across languages — every tags query defines
 * definition.function — and the second reaches anything at all, at the price
 * of naming a particular grammar's vocabulary.
 *
 * requires: structure(structure, language, tagged); rope(rope, bytes, share);
 *           `selector` is NUL-terminated and non-empty; *result writable.
 * ensures:  rope(rope, bytes, share); decomposition(result, spans, bytes)
 *           where spans are the extents of the nodes the selector names,
 *           ascending; a node whose extent overlaps one already selected is
 *           dropped, and a selected node is not descended into, so nested
 *           nodes of one type select the outermost. The result is how many
 *           spans were selected; or -1 when the buffer could not be parsed
 *           or more nodes were found than a decomposition can hold, or -2
 *           when the selector names a capture and the grammar is not
 *           `tagged`, so there is no query to look it up in.
 */
int structure_select(Structure *structure, const Rope *rope,
                     const char *selector, Decomposition *result);

#endif /* PAPRI_STRUCTURE_H */
