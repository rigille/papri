# papri

A modernized `ed`.

It keeps `ed`'s essential property — no terminal control, no cursor
addressing, just reading commands and printing text. That is what lets the
terminal's own scrollback hold several regions of several files at once, and
what frees the editor to print *derived* views: the functions defined in a
file, a slice, a hex dump. Folding and windows exist to work around a screen
the editor must repaint; a teletype has neither problem.

What makes it modern rather than nostalgic:

- **Immutable buffers.** A byte sequence backed by an RRB tree of 64-byte,
  cache-line-aligned chunks. Every version is retained, so a background job
  holds a stable snapshot and its result can say which version it was computed
  against.
- **Async IO.** `io_uring` on a single thread, with output strictly serialized
  at command boundaries so the transcript never interleaves.
- **Commands from optics.** An address is an isomorphism
  `Buffer ≅ (prefix, focus, suffix)`; a command is a function on the focus.
  Addresses resolve against one immutable version before any edit applies,
  which makes multi-match offset invalidation structurally impossible.
- **Not just text.** The buffer is bytes, so binary editing is in scope.
  Decoding is a view over the bytes, not a property of the buffer.

Status: early. See `CLAUDE.md` for the conventions and the build targets.

## Build

```sh
nix develop
make check
```

## Why the odd-looking C

Every function is written in the subset of C accepted by Verifiable C, and
every function carries a `requires:` / `ensures:` contract. The rules are in
[CLAUDE.md](CLAUDE.md); three gates hold the line, and `vendor/` is exempt
because the code there has a machine-checked proof rather than our proxy for
one.
