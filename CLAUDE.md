# papri — Conventions

A modernized `ed`. It keeps `ed`'s essential property — no terminal control, no
cursor addressing, just reading commands and printing text — because that is
what lets the terminal's own scrollback hold several regions of several files at
once, and what frees the editor to print *derived* views (the functions in a
file, a slice, a hex dump) instead of always printing the buffer. Buffers are
byte sequences backed by an immutable RRB tree, so every version is retained and
every background job holds a stable snapshot; IO is `io_uring` under a strictly
serialized transcript; and the command language is built from optics, where an
address is an isomorphism `Buffer ≅ (prefix, focus, suffix)` and a command is a
function on the focus.

These are the same rules the sibling project `../kelci` follows: the code is
written to stay mechanically verifiable, and every function carries a contract.
Two sections at the end — on vendored verified code, and on shares — are papri's
own, and are load-bearing here in a way they were not for `kelci`.

## Code style

- All identifiers: `snake_case`, composed of valid English words, no abbreviations.
  - Wrong: `vx`, `invM`, `idx`, `ext`, `vel`, `Pn`, `dt_us`, `freq`, `buf`, `len`
  - Right: `velocity_x`, `inverse_mass`, `index`, `external`, `velocity`,
    `normal_impulse`, `time_step`, `frequency`, `buffer`, `length`
- C macros follow the same rule but in `SCREAMING_SNAKE_CASE`.

## Sizes are `size_t`

Anything that counts **bytes** is `size_t`: a length, an offset, a capacity,
a measure in a node's size table, a parameter naming any of those. Never
`uint32_t`, never `int`, never `long`.

The rule exists because the alternative was tried and failed quietly. papri
once carried `uint32_t` measures, which capped a buffer at 4 GiB — and did
not enforce the cap, it *truncated* to it. `(uint32_t)read_count` on a 5 GiB
file silently produced 0.7 GiB of it and reported success. A width that is
"obviously enough" is a width that narrows somewhere, and narrowing a size is
indistinguishable from success until the data is gone.

It also happens to be what immer does — `size_t sizes[]` in `relaxed_data_t`
— so following it keeps the rope layout honest against the structure it is
modelled on.

Scope: the rule is about *bytes*. Counts of other things keep the narrowest
type that obviously cannot overflow — a node's `child_count` and `height` are
bounded by the branching factor and the maximum height, so they stay
`uint32_t`. When in doubt about whether a quantity is bounded, it is not:
use `size_t`.

## Verifiable C subset — mandatory for all code

We write C in the subset accepted by **Verifiable C** (the VST program logic;
see the Verifiable C reference manual, §4 "Verifiable C and clightgen"), so the
code stays mechanically verifiable. `clightgen` can normalize some violations
away, but we write code that needs no normalization — what you verify is then
exactly what you read.

Hard restrictions (from the manual):

- **No struct passing, returning, or assignment by value.** Structs cross
  function boundaries only as pointers (`const T *` in, `T *` out). Copy
  structs with an explicit `memcpy`, never with `=`.
- **No casting between integers and pointers.**
- **No `goto`.**
- **Only structured `switch` statements** — every `case` is a plain block at
  the top level of the switch; no jumping into loop bodies (no Duff's device).

Program-logic restrictions (write these explicitly rather than relying on
clightgen's factoring):

- **No side effects inside subexpressions.** Every assignment and every
  function call is its own statement. Never nest calls (`f(g(x))` becomes
  `temporary = g(x); f(temporary);`). `i++` / `--i` only as standalone
  statements, never inside a larger expression.
- **One memory dereference per statement** (the `-normalize` style): split
  `x = a[b[i]];` into `temporary = b[i]; x = a[temporary];`.
- **Pure operands only in `&&`, `||`, `?:` and `if`/`while` conditions** — no
  calls, stores, or (per the previous rule) loads buried inside them; the
  logic factors short-circuit operators into `if`s.
- **Do not take the address of a local variable** unless unavoidable — an
  addressable local becomes heap-resident and far heavier to reason about.
  Prefer plain (nonaddressable) locals.
- **No `volatile`** (the logic assumes types are not volatile), **no variadic
  function definitions**, **no `setjmp`/`longjmp`**.

## Specifications — mandatory on every function

We document functions in the style of the Verified Software Toolchain's
**Verified Software Units**: each module exports an *Abstract Specification
Interface* built from *Abstract Predicate Declarations*. The only relaxation is
that predicate definitions and assertions are written in precise natural
language instead of Coq — the discipline stays, the boilerplate goes.

**Non-negotiable rule: every function — public or `static` — carries a spec
comment. If you change what a function does, change its spec in the same edit.
The spec is the contract; the body merely implements it.**

### Abstract predicates (one block per module, top of the `.h`)

Each header opens with the module's representation predicates. A predicate
names a piece of *ownership + meaning*: which memory it claims and what must be
true of it. Clients may only use these predicates as opaque tokens in specs;
only the implementing `.c` may rely on their definition (fields, invariants).

```c
/* ── Abstract predicates ────────────────────────────────────────────────────
 * event_ring(ring, contents)
 *   Owns *ring. `contents` is the sequence of pending events, oldest first,
 *   each timestamped, timestamps nondecreasing, length <= EVENT_RING_CAPACITY.
 */
```

Guidelines:
- Parameters of a predicate separate the *owned location* from its *abstract
  value* (`event_ring(ring, contents)` — clients reason about `contents`,
  never about `head`/`tail`).
- State the invariants inside the predicate once; specs then get them for free
  by mentioning the predicate.
- Scalar/value types with no ownership still get a predicate if they carry
  invariants; plain data needs none.

### Function specs

Every declaration (and every `static` definition in a `.c`) is preceded by:

```c
/* requires: <what the caller must provide: predicates over the arguments,
 *            pure facts (ranges, orderings), and ownership handed in>
 * ensures:  <what holds on return: predicates over outputs, how the new
 *            abstract values relate to the old ones, ownership handed back>
 */
```

- **Frame rule is implicit**: memory not mentioned in `requires` is neither
  read nor written. Never write "does not touch X" — silence already says it.
- Ownership mentioned in `requires` is returned in `ensures` unless the spec
  says it is consumed.
- Relate outputs to inputs *functionally* ("`contents'` is `contents` minus
  every event with time < `time`"), not operationally ("loops advancing head").
- Name the abstract values, prime the new ones (`contents` → `contents'`), and
  use those names in the ensures clause.
- A pure function's spec must say so: `ensures: result depends only on the
  arguments; no memory is written`.

Worked example:

```c
/* requires: event_ring(ring, contents); time is a valid Microseconds.
 * ensures:  event_ring(ring, contents') where contents' is contents with
 *           every event timestamped strictly before `time` removed; the
 *           surviving events keep their order and payloads.
 */
void ring_dequeue_before(EventRing *ring, Microseconds time);
```

When touching an undocumented function, write its spec first (from the current
behaviour), then make your change and update the spec together with the body.

## Vendored verified code is exempt from the gates

`vendor/` is skipped by `make lint` and `make normalform` and compiles with
relaxed warnings. That is deliberate.

The subset is a **proxy** for verifiability: we write C that needs no
normalization so that what you verify is exactly what you read. Where a
machine-checked proof already exists, the proof supersedes the proxy. Holding
such a file to the subset would mean rewriting it, which discards the proof —
the opposite of the point.

`vendor/utf8/utf8.c` is the current case: Höhrmann's DFA decoder, proved to
full functional correctness in `../tools/coq/VerifiableC/Utf8.v`. It would fail
our gates (`next_state` writes through a pointer from a `?:` whose branches
load), and `../tools` runs `clightgen -normalize` precisely because it does not
need pre-normalized source.

The rules for anything under `vendor/`:

- **Vendored byte-identical, never edited** — not even for naming or style. The
  proof is against `clightgen -normalize` of that exact file, so any edit voids
  it. `make vendor-check` enforces this against a recorded SHA-256 and, when
  the sibling checkout is present, against `$(TOOLS)` itself.
- **The header is ours.** Upstream files here ship no header; we write one, with
  specs in the house style stating what the Coq funspecs prove.
- **Record the provenance and the license** in a `README.md` beside it.

## Shares — how to write predicates about immutable sharing

`kelci` and `ciska` phrase read-only aliasing in English as "borrowed,
non-owning". That cannot express a DAG, and papri's buffers *are* a DAG: two
versions of a buffer own overlapping sets of RRB nodes.

The property that makes this tractable is that **a node is immutable once
sealed**. Read shares over immutable data split and rejoin freely, which is
exactly what lets two paths reach one node. Predicates therefore name a share:

```c
/* ── Abstract predicates ────────────────────────────────────────────────────
 * rope_node(node, contents, share)
 *   Holds `share` of *node, sealed and immutable. Read shares split:
 *   rope_node(n, c, s1 ⊕ s2) is rope_node(n, c, s1) * rope_node(n, c, s2).
 *   This is what lets two paths reach one node; a mutable node could not.
 *
 * version(root, bytes, share)
 *   Holds `share` of every node reachable from `root`. `bytes` is the byte
 *   sequence it denotes.
 *
 * node_pool(pool, live, residual)
 *   Owns the slabs. For each node in `live`, holds the complement of every
 *   share handed out. A node whose residual share is the full share is dead
 *   and may be freed.
 */
```

Three consequences worth stating, because they are what the design rests on:

- **Deallocation needs the full share**, so "no other reference survives" is
  enforced by the logic rather than remembered by convention. A background job
  holding a snapshot holds a share; you therefore cannot free under it.
- **Shares are ghost state** with no runtime footprint. They say *when* freeing
  is permitted; they cannot find the nodes. That is the diff walk's job. The
  obligation tying the two together, to be written down now and proved later:
  *the diff walk returns exactly the nodes whose shares have rejoined to the
  full share.*
- **Fix a scheme for how a node's share divides** among k referents rather than
  letting it be ad hoc. VST shares are infinitely divisible so there is no
  overflow hazard, but arbitrary lattice elements mean share arithmetic in every
  proof.

### The shortcut that does not work

The first implementation walked a retiring version against its successor and
freed the difference, pruning wherever a node was reached by both sides. That
is sound for ONE dead root against one survivor, because a version is a tree
and there is only one path to any node.

It is NOT sound for a SET of dead roots, and an edit produces a set — every
intermediate rope a splice discards. Two dead roots can reach one shared
subtree by different paths. The first prunes it, and pruning stops the walk
from recording the subtree's interior as live; the second then reaches a node
inside it by another route, finds it absent from the live frontier, and frees
it while it is still reachable. Buffers over about ten kilobytes came back
corrupted.

Expanding the live side fully restores soundness and costs a walk of every
surviving node per edit, which is the thing the design existed to avoid. So
the walk was removed and papri leaks until the collector is wired in. If you
are tempted to reintroduce a pairwise diff, the question to answer first is
how it records liveness for a subtree it pruned.

## Build

Nix flake for the toolchain (`nix develop`), plain `make` inside it.

| Target | What it does |
|---|---|
| `make` | `build/libpapri.a` and `build/papri` |
| `make test` | build and run the tests |
| `make lint` | grep gate — `goto`, `volatile`, varargs, embedded `++`, missing specs |
| `make normalform` | clightgen gate — source is already in the logic's normal form, no by-value structs |
| `make vendor-check` | vendored verified code is unedited |
| `make asan` | the tests under `-fsanitize=address,undefined` |
| `make check` | all of the above |
