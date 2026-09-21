#ifndef PAPRI_STRUCTURE_H
#define PAPRI_STRUCTURE_H

#include <stdint.h>

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
 * a grammar is a shared object plus a `queries/tags.scm`, so any language
 * that ships one works without papri knowing anything about it. That is
 * also why this is the second foreign boundary, alongside src/io.c.
 *
 * The buffer is never materialized to parse it. tree-sitter reads through a
 * callback, so it is fed rope windows directly.
 */

typedef struct Structure Structure;

/* ── Abstract predicates ────────────────────────────────────────────────────
 * structure(structure, language)
 *   Owns *structure: a loaded grammar, its tags query, and a parser. The
 *   grammar's shared object stays mapped for the lifetime of the handle.
 */

/* requires: `directory` holds a tree-sitter grammar — a `parser` shared
 *           object and a `queries/tags.scm`; `language` names it, so that
 *           the entry point is tree_sitter_<language>.
 * ensures:  structure(structure, language) and the result points at it; or
 *           the grammar could not be loaded and the result is null.
 */
Structure *structure_create(const char *directory, const char *language);

/* requires: structure(structure, language).
 * ensures:  the grammar is unloaded and *structure freed.
 */
void structure_destroy(Structure *structure);

/* requires: structure(structure, language); rope(rope, bytes, share).
 * ensures:  rope(rope, bytes, share); every definition the grammar's tags
 *           query finds is written to standard output as its line number,
 *           its kind and its name, in source order. The result is how many
 *           were found, or -1 when the buffer could not be parsed.
 */
int structure_list_definitions(Structure *structure, const Rope *rope);

#endif /* PAPRI_STRUCTURE_H */
