# papri

A pure C project. <!-- TODO: what papri is. -->

## Build

```sh
nix develop          # clang, make, clightgen, bear, ccls
make                 # build/libpapri.a + build/papri
make test            # build and run the tests
make check           # lint + normalform + test
```

`nix build` produces the binary and the static library; `nix flake check`
runs the subset gates.

## Why the odd-looking C

Every function is written in the subset of C accepted by Verifiable C, and
every function carries a `requires:` / `ensures:` contract. The rules, and the
reasoning behind them, are in [CLAUDE.md](CLAUDE.md).

Two gates hold part of the line:

| Gate | Catches |
|---|---|
| `make` | integer↔pointer casts, the usual warnings, all as errors, plus `-Wlarge-by-value-copy=17` — a size threshold, so only a partial catch for by-value structs |
| `make lint` | `goto`, `volatile`, `setjmp`, varargs, `++`/`--` inside a larger expression, and any function missing a spec |
| `make normalform` | loads hidden inside conditions or alongside a second dereference; by-value structs, exactly |

`make normalform` runs `clightgen` twice over each translation unit, with and
without `-normalize`, and compares the two ASTs. Normalization is what hoists
loads out of expressions, so a temporary it has to invent marks a load the
subset does not allow there.

One class of hoist is exempt. C promotes every sub-`int` lvalue to `int` and
converts back, so a byte-typed load like `oldest = ring->storage[head];`
always sits under a cast and is always hoisted — no way of writing that
statement avoids it. A hoisted temporary whose every use is directly under a
cast is reported and allowed; every other hoist fails the build. The known
cost of that exemption is that `f((int)byte_in_memory)` slips through.

By-value structs are caught twice over: `clightgen` refuses them outright
without `-fstruct-passing`, and the gate also checks that every `Tstruct` in a
signature sits under a `tptr`.

## Not yet gated

- **Calls nested in subexpressions.** `clightgen` factors calls out of
  expressions with or without `-normalize`, so the normal-form diff is blind
  to them. A grep check was attempted and backed out — see the TODO in
  `tools/check_subset.sh`. Review-only for now.
- **Struct assignment.** `*destination = *source;` instead of an explicit
  `memcpy` is caught by nothing. Review-only.
