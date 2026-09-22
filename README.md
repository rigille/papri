# papri

A modernized `ed`.

It keeps `ed`'s essential property — no terminal control, no cursor
addressing, just reading commands and printing text. That is what lets the
terminal's own scrollback hold several regions of several files at once, and
what frees the editor to print *derived* views instead of always printing the
buffer. Ask for the functions defined in a file and it prints them; there is
nothing to fold, because nothing was being hidden.

```
$ papri src/pool.c
F
13	function	round_up_to_alignment
31	function	reserve_directory_slot
66	function	add_slab
98	function	pool_initialize
122	function	pool_release
```

## What makes it modern rather than nostalgic

**Immutable buffers.** A byte sequence backed by an RRB tree with 256-byte
leaves, sized the way immer sizes its own. Every version is retained, so a
background job holds a stable snapshot and can say which version its answer
belongs to. Leaves are headerless, which is what puts papri's memory
overhead at 8% where immer pays 10.1% — and which rules out reference
counting, since there is nowhere to put the count.

Reclamation is a conservative tracing collector (Boehm). It was going to be
a walk of the difference between a retired version and its successor; that
turned out to be unsound for a *set* of dead roots, which is what every edit
produces, and `CLAUDE.md` records the argument so it does not come back. The
collector is the thing the walk could not be: it sees every root at once.

**Async IO.** One `io_uring`, one thread, blocking in exactly one place.
Output is strictly serialized at command boundaries, so a job's result never
interleaves with a command's and the scrollback stays a faithful record.

**Commands from optics.** An address is an isomorphism
`Buffer ≅ (prefix, focus, suffix)`; a command is a function on the focus.
Addresses resolve against one immutable version before any edit applies,
which makes multi-match offset invalidation structurally impossible rather
than carefully avoided. `s/pat/rep/` is not a special form — it is an address
composed with a match traversal, then an ordinary change.

**Not just text.** The buffer is bytes, so binary editing is in scope.
Decoding is a view over the bytes rather than a property of the buffer: text,
hex and code points are three ways of looking at one sequence.

## Build

```sh
nix develop
make check          # lint + normal form + vendor + tests
make asan           # the same tests under address and UB sanitizers
./build/papri FILE
```

## Commands

An address, then a verb. An address alone prints.

| Address | Selects |
|---|---|
| `%` | the whole buffer |
| `.` | the current line |
| `$` | the last line |
| `N` / `N,M` / `N,$` | line N, or a run of lines |
| `#N` / `#N,#M` | a byte offset, or a byte range |
| `/text/` | every occurrence of `text` |
| `g/text/` | every line containing `text` |

| Verb | Does |
|---|---|
| `p` `n` | print, plain or with line numbers |
| `x` `u` | print as hex, or as code points |
| `=` | print the extents of each focus |
| `d` `c TEXT` | delete, or change to TEXT |
| `i TEXT` `a TEXT` | insert before, or append after |
| `s/pat/rep/` | substitute within the focus |
| `F` | print the definitions in the buffer |
| `e` `E` `w` | load, load into a new buffer, write |
| `b` `bN` `@N cmd` | list buffers, switch, or run a command against one |
| `&e f` `&c f` `&` | load or count in the background, list jobs |
| `q` | quit |

Escapes in TEXT: `\n`, `\t`, `\\`.

Matching is literal bytes today. A regex engine is later work, and lands as
two more address forms without moving anything else.

## Why the odd-looking C

Every function is written in the subset of C accepted by Verifiable C, and
every function carries a `requires:` / `ensures:` contract. The rules are in
[CLAUDE.md](CLAUDE.md).

| Gate | Catches |
|---|---|
| `make` | integer↔pointer casts, the usual warnings as errors, and `-Wlarge-by-value-copy` as a partial by-value-struct check |
| `make lint` | `goto`, `volatile`, `setjmp`, varargs, embedded `++`/`--`, and any function missing a spec |
| `make normalform` | loads hidden inside conditions, call arguments or beside another dereference; by-value structs, exactly |
| `make vendor-check` | vendored verified code is unedited |

`make normalform` runs `clightgen` twice per translation unit, with and
without `-normalize`, and compares the two ASTs **per function** — clightgen
numbers temporaries per translation unit, so comparing whole files misses a
function gaining one. A temporary it had to invent marks a load the subset
does not allow there.

One class of hoist is exempt: C promotes every sub-`int` lvalue to `int` and
converts back, so a byte-typed load always sits under a cast and is always
hoisted. No way of writing that statement avoids it, so a hoist whose every
use is directly under a cast is reported and allowed.

The rule this gate teaches, over and over: **taking a local's address moves
it to memory, and every later mention of it is then a load.**

## Boundaries

Three parts of the tree are deliberately outside the gate, each for a reason
recorded next to it:

- `vendor/utf8/` — Höhrmann's DFA decoder, proved to full functional
  correctness in `../tools`. The subset is a *proxy* for verifiability; where
  a machine-checked proof exists, the proof supersedes the proxy. Vendored
  byte-identical and checked against a recorded hash.
- `src/io.c` — `liburing.h` reaches `stdatomic.h` and CompCert stops at
  `_Atomic`.
- `src/structure.c` — tree-sitter and `dlfcn`, same story.

Both boundary files are kept thin and expose opaque handles, so everything
above them stays inside the subset and stays gated.

## Not yet done

- **Regex.** Matching is literal bytes.
- **Calls nested in subexpressions** are gated by nothing: clightgen factors
  calls out of expressions with or without `-normalize`, so the normal-form
  diff is blind to them. See the TODO in `tools/check_subset.sh`.
- **Struct assignment** (`*destination = *source;` instead of an explicit
  `memcpy`) is caught by nothing. Review-only.
- **Proofs.** The discipline is in place so that proving the core in
  Coq/VST stays possible. None are written yet.
