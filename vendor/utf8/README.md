# vendor/utf8

Björn Höhrmann's DFA UTF-8 decoder, as formally verified in the sibling
`../tools` repository.

- `utf8.c` — vendored **byte-identical** from `../tools/verifiable-c/utf8.c`.
  Do not edit it, not even for style.
- `utf8.h` — ours. Upstream ships no header; the declarations and specs there
  are written against the funspecs in `../tools/coq/VerifiableC/Utf8.v`.

## Why it is exempt from the gates

`make lint` and `make normalform` skip this directory, and it compiles with
relaxed warnings. That is deliberate, not an oversight.

papri's Verifiable C subset is a *proxy* for verifiability: we write C that
needs no normalization so that what you verify is exactly what you read. This
file has the real thing instead of the proxy — `next_state_spec`,
`next_codepoint_spec` and `count_codepoints_spec` are proved in Coq against a
reference decoder, to full functional correctness, not merely memory safety.
Where a machine-checked proof exists, the proof supersedes the proxy.

It would in fact fail the gates. `next_state` writes through a pointer from a
`?:` whose branches load, which the subset forbids; `../tools/Makefile` runs
`clightgen -normalize` precisely because it does not need the source to be
pre-normalized. Rewriting the file to pass our gates would discard the proof,
which is the opposite of the point.

## Keeping the proof honest

The proof is against `clightgen -normalize` of this exact file. Any edit voids
it. `make vendor-check` verifies the copy still matches `$(TOOLS)`, which
defaults to `../tools`.

## License

MIT, Copyright (c) 2008-2009 Björn Höhrmann. The notice at the top of
`utf8.c` must be preserved.
