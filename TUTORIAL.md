# A papri tutorial

papri is a line editor in the tradition of `ed`. It never clears your screen,
never moves your cursor, and never repaints anything. You type a command, it
prints something, and what it printed stays printed.

That sounds like a limitation. It is the point. Because the transcript is
never rewritten, it can accumulate: three regions of three files, the output
of a search, and a list of every function in a header, all visible at once,
because they are simply things that were printed. And because papri is not
obliged to show you the buffer, it can show you something *derived from* the
buffer instead — the definitions in a file, a hex dump, a decoded code point
listing. There is no folding, because nothing is being hidden.

This tutorial assumes nothing about `ed`. Work through it with a real file.

```sh
nix develop
make
export PATH="$PWD/build:$PATH"      # so `papri` works from anywhere

mkdir /tmp/papri-tutorial && cd /tmp/papri-tutorial
printf 'alpha\nbeta\ngamma\ndelta\n' > poem.txt
```

Keep the `nix develop` shell: section 8 needs the tree-sitter grammar it
sets up for you.

Now start it:

```sh
papri poem.txt
```

There is no prompt and no greeting. papri is waiting.

## 1. Printing

Type `%p` and press return.

```
%p
alpha
beta
gamma
delta
```

`%` means *the whole buffer*; `p` means *print*. Every command in papri has
that shape: **an address, then a verb**. The address chooses a region, the
verb does something to it.

Ask for one line:

```
2p
beta
```

The verb can be left off — an address alone prints, because that is
overwhelmingly what you want:

```
2
beta
```

And the address can be left off too, in which case papri uses the current
line:

```
p
alpha
```

Numbered output is `n` rather than `p`:

```
%n
1	alpha
2	beta
3	gamma
4	delta
```

Note that `%n` numbers *every line in the focus*, not just the first. A
focus can span many lines; numbering only where it began would be useless.

## 2. Addresses select a span, not a cursor

This is the one idea worth slowing down for. papri has no cursor. An address
names a **region of bytes**, and everything else follows from that.

| Address | Selects |
|---|---|
| `%` | the whole buffer |
| `.` | the current line |
| `$` | the last line |
| `3` | line 3 |
| `2,3` | lines 2 through 3, as one region |
| `2,$` | line 2 to the end |
| `#7` | the empty span at byte 7 |
| `#2,#5` | bytes 2 up to (not including) 5 |
| `/text/` | every occurrence of `text` |
| `g/text/` | every line containing `text` |
| `{sel}` | every node the grammar calls `sel` — section 8 |

Try the byte forms. They reach inside a line, which line-numbered addresses
cannot:

```
#0,#5p
alpha
```

And try the pattern forms:

```
g/a/n
1	alpha
2	beta
3	gamma
4	delta
```

Every line contains an `a`, so every line matched. Something more selective:

```
g/mm/n
3	gamma
```

To see exactly what an address selected, use `=`, which prints extents
rather than contents:

```
g/a/=
#0,#6	line 1
#6,#11	line 2
#11,#17	line 3
#17,#23	line 4
```

Read those numbers. Line 1 is bytes 0 through 6 — six bytes, for five
letters. **A line's region includes the newline that ends it.** That is not
an accident, and section 4 is about what it means.

## 3. Changing things

| Verb | Does |
|---|---|
| `d` | delete the region |
| `c TEXT` | replace the region with TEXT |
| `i TEXT` | insert TEXT before the region |
| `a TEXT` | insert TEXT after the region |
| `s/pat/rep/` | replace `pat` with `rep`, within the region |

In TEXT you can write `\n` for a newline, `\t` for a tab, and `\\` for a
backslash. In `c` and `s`, which both *replace* something, `\1` stands for
whatever is being replaced — section 5 comes back to that.

```
2d
%p
alpha
gamma
delta
```

```
1c FIRST
%p
FIRSTgamma
delta
```

That is probably not what you expected, and it is section 4's subject. Quit
without saving — `q` — and start again so we have the original file back.

## 4. The thing that will surprise you

`1c FIRST` replaced line 1 *including its newline*, so `FIRST` ran into the
next line. Once you know that a line's region includes its terminator,
everything is predictable:

```sh
papri poem.txt
```

```
1c FIRST\n
%p
FIRST
beta
gamma
delta
```

The same rule explains `i` and `a`:

```
2i >> 
%p
alpha
>> beta
gamma
delta
```

`i` inserted before line 2's region, which is the start of line 2.

```
1a X
%p
alpha
Xbeta
gamma
delta
```

`a` inserted *after* line 1's region — and line 1's region ends after its
newline, so the text landed at the start of line 2. To append a whole new
line, say so:

```
1a X\n
%p
alpha
X
beta
gamma
delta
```

This is different from `ed`, where `a` always appends a new line. papri is
more literal: there is one operation, "replace this span of bytes with
those", and `i`, `a`, `c` and `d` are all that operation with the span and
the text chosen differently. Nothing special-cases lines.

## 5. Substitution, and why it is safe

`s` looks like a special form and is not. `%s/at/og/` means: take the
address `%`, narrow it to every occurrence of `at` inside it, and change each
one to `og`. It is composition, not a separate feature.

```sh
printf 'the cat sat on the mat\n' > cats.txt
papri cats.txt
```

```
%s/at/og/
%p
the cog sog on the mog
```

Now the part that matters. Try a replacement *longer* than the pattern:

```
%s/og/ELONGATED/
%p
the cELONGATED sELONGATED on the mELONGATED
```

In an editor built on a mutable buffer this is where bugs live: you replace
the first match, every later match is now at the wrong offset, and unless
the implementation carefully walks backwards or tracks a running delta,
something goes wrong. papri cannot have that bug. Every address is resolved
against one immutable snapshot of the buffer *before* any edit is applied,
and the result is then assembled in one pass. There are no stale offsets
because no offsets were ever used twice.

A pattern that does not occur changes nothing and says so:

```
%s/zzz/x/
?  no match
```

Matching is **literal bytes**. There is no regular expression engine yet.

### Keeping what you replaced

A replacement can quote the text it is replacing, by writing `\1`:

```sh
printf 'the cat sat on the mat\n' > cats.txt
papri cats.txt
```

```
%s/at/[\1]/
%p
the c[at] s[at] on the m[at]
```

Write it twice and you get it twice:

```
%s/at/<\1|\1>/
```

`ed` spells this `&`, and numbers its back-references because a regular
expression has groups to number. papri has no groups: the address already
decided what the focus is, so there is exactly one thing `\1` could mean and
no numbering to invent. It works in `c` for the same reason — `c` replaces
the focus, so `\1` there is the focus:

```
1c/* \1 */
```

A literal backslash-one is still `\\1`, as you would expect.

## 6. The buffer is bytes

papri does not assume your file is text, because sometimes it is not.
Decoding is a *view* over the bytes, not a property of the buffer, so you can
look at the same region three ways.

```sh
printf 'caf\xc3\xa9\n' > utf.txt
papri utf.txt
```

Text, which is what `p` does:

```
1p
café
```

Hex:

```
1x
00000000  63 61 66 c3 a9 0a                                 |caf...|
```

Code points:

```
1u
0	U+0063	1 byte(s)
1	U+0061	1 byte(s)
2	U+0066	1 byte(s)
3	U+00E9	2 byte(s)
5	U+000A	1 byte(s)
```

`é` is one code point occupying two bytes at offset 3. The byte addresses
from section 2 address those bytes directly, which is how you edit binary
files: open one and use `#N,#M` with `x` and `c`.

Invalid UTF-8 is reported rather than fatal — a stray byte is named, and
decoding resumes after it.

## 7. Several files at once

This is what the never-repainting transcript buys you.

```sh
papri poem.txt
```

Load a second file into a new buffer with `E`:

```
E cats.txt
b
 0	23 bytes, 4 lines	poem.txt
*1	23 bytes, 1 lines	cats.txt
```

`b` lists the buffers; `*` marks the current one. `bN` switches:

```
b0
1p
alpha
```

But you often do not want to switch. `@N` runs a single command against
another buffer and comes straight back:

```
@1 %p
the cat sat on the mat
%n
1	alpha
2	beta
3	gamma
4	delta
```

Both files are now on your screen, and you never left buffer 0. Scroll up and
they are all still there — that is the whole idea. Each buffer keeps its own
text, its own name and its own history.

## 8. Asking what is in a file

For this one, go back to papri's own source tree so there is C to look at:

```sh
cd ~/repositories/hobby/papri
papri src/pool.c
```

```
F
13	function	round_up_to_alignment
31	function	reserve_directory_slot
66	function	add_slab
98	function	pool_initialize
122	function	pool_release
146	function	class_of
163	function	pool_allocate
225	function	pool_free
254	function	pool_live
266	function	pool_handed_out
278	function	pool_reserved
```

`F` prints every definition in the buffer, with its line and its kind. This
is the command the whole design exists for. A screen editor needs folding
because it must show you the buffer and the buffer is too big; papri shows
you whatever you asked for, and you asked for the definitions.

It combines with everything else. Find a function, then go to its line:

```
F
...
163	function	pool_allocate
163,170p
```

### Addressing by grammar

`F` prints. The same parse can also *address*, and that is the more
interesting half. `{…}` is an address like any other, and it selects the
nodes the grammar calls by that name:

```
{definition.function}=
#99,#126	line 8
#281,#305	line 19
#419,#457	line 28
...
```

Since it is an ordinary address, every verb already works on it. No verb had
to learn anything:

```
{comment}d               delete every comment
{string_literal}p        print every string literal
{definition.function}n   print every function, numbered
{string_literal}c L\1    make every string literal a wide one
```

That last line is where `\1` earns its keep: the address found the spans, and
the replacement puts them back with something around them.

A selector is read one of two ways, and the dot is what tells them apart,
since no tree-sitter node type contains one:

| Selector | Means |
|---|---|
| `definition.function` | a capture in the grammar's `queries/tags.scm` |
| `function_definition` | a node type in the grammar itself |

Captures are portable — every tags query defines `definition.function`, so
that address means the same thing in C, Python and Go. Node types reach
anything at all, at the price of naming one grammar's vocabulary. `tree-sitter`
will print a grammar's node types for you, and highlights files are full of
them.

Nesting selects the outermost: `{compound_statement}` in a C function gives
you the function body, not the body and every block inside it. An address has
to be a *decomposition* — disjoint spans in order — and overlapping foci are
not one.

### Which grammar, for which file

The parse comes from tree-sitter, loaded at run time, and which grammar a
buffer gets is decided by its **name's suffix**. `G` shows the table:

```
G
 *	c	/nix/store/…-tree-sitter-c
*.c	c	/nix/store/…-tree-sitter-c
 .h	c	/nix/store/…-tree-sitter-c
 .py	python	/nix/store/…-tree-sitter-python
 .go	go	/nix/store/…-tree-sitter-go
 .rs	rust	/nix/store/…-tree-sitter-rust
 .nix	nix	/nix/store/…-tree-sitter-nix
 .json	json	/nix/store/…-tree-sitter-json
```

`*` marks the row this buffer's name selects — here `.c`, because the buffer
is `src/pool.c`. The row whose *suffix* is `*` is the catch-all, used only
when nothing more specific matches. Open a Python file in another buffer and
`@1 F` parses it as Python while `F` still parses this one as C.

The table comes from the file named by `$PAPRI_GRAMMARS`, one registration
per line — suffix, language, directory — with `#` for comments:

```
# suffix  language  directory
.c    c       /path/to/tree-sitter-c
.py   python  /path/to/tree-sitter-python
```

The devShell writes one and points `$PAPRI_GRAMMARS` at it. You can also add
a row mid-session, which is the fastest way to try a new grammar:

```
G .rb ruby /path/to/tree-sitter-ruby
```

`$PAPRI_GRAMMAR` and `$PAPRI_LANGUAGE`, which used to be the whole story,
still work: they register the catch-all row. A grammar is loaded the first
time a buffer actually needs it, so a table of ten costs nothing until you
open ten languages.

A grammar with no `queries/tags.scm` — tree-sitter-json ships none — still
works for node types; only `F` and a dotted selector need the query, and they
say so rather than failing quietly. Without any grammar for this buffer's
name, `F` and `{…}` say that too, and do nothing else.

## 9. Work that happens while you type

Reading a large file should not stop you editing. `&c` counts a file in the
background; `&e` loads one.

```
&c poem.txt
[1] reading poem.txt
1p
alpha
[1] poem.txt: 23 bytes, 4 lines  (launched at version 1, now 1)
```

Notice where the job's output landed: *after* the command that was running,
never in the middle of it. Background results are only ever printed between
commands. If they could interrupt a command's output the transcript would
stop being a faithful record of what happened, and the transcript is the
interface.

`&` on its own lists what is still in flight, and quitting waits for
outstanding work rather than dropping it silently.

Now the interesting case. Start a background load and then edit before it
lands:

```
&e cats.txt
[1] reading cats.txt
1d
[1] cats.txt read, but the buffer moved on 1 version(s) since; discarded
```

The load was refused. Every job records the version of the buffer it was
launched against; applying a stale read would have thrown away the edit you
just made. papri would rather tell you than guess. This is what immutable
buffers are actually for — not undo, but giving background work a stable
thing to have been computed against, and a way to say so.

## 10. Saving and quitting

```
w              write back to the file it came from
w other.txt    write somewhere else
e other.txt    load a file into the current buffer
q              quit
```

`w` with no name uses the name the buffer was loaded with.

## 11. Scripting it

papri reads commands from standard input, so a session is a shell pipeline:

```sh
printf 'g/TODO/n\nq\n' | papri src/rope.c
```

```sh
printf '%%s/old/new/\nw\nq\n' | papri config.txt
```

This is not a scripting mode bolted on. It is the same loop; there was never
anything interactive about it to begin with.

## Rough edges

Things that are genuinely missing or surprising, so you do not lose time
discovering them:

- **No regular expressions.** `/text/`, `g/text/` and `s/pat/rep/` all match
  literal bytes.
- **No undo command.** Every version of the buffer is retained internally,
  which is what makes background snapshots work — but no verb exposes it
  yet. Write before you experiment.
- **`.` does not follow your edits.** It is the current line, but no command
  moves it; it starts at line 1 and is only clamped when the buffer shrinks.
  Address lines explicitly.
- **`q` does not warn about unsaved changes.** It quits.
- **Addresses do not compose arbitrarily.** There is no `/start/,/end/`, and
  no `{definition.function}/name/` either — `s` is the only form that narrows
  one address with another.
- **A structural address re-parses the buffer** every time it is used. Fine
  for a file, noticeable in a loop over a large one.

## Command reference

An address, then a verb. An address alone prints. No address means the
current line.

```
Addresses
  %            the whole buffer
  .            the current line
  $            the last line
  N            line N
  N,M   N,$    a run of lines
  #N           the empty span at byte N
  #N,#M        bytes N up to M
  /text/       every occurrence of text
  g/text/      every line containing text
  {sel}        every node the grammar calls sel

Verbs
  p  n         print; print with line numbers
  x  u         print as hex; print as code points
  =            print the extents of each region
  d            delete
  c TEXT       replace with TEXT
  i TEXT       insert TEXT before
  a TEXT       insert TEXT after
  s/pat/rep/   substitute within the region
  F            print the definitions in the buffer

Grammars
  G            list the grammars, marking this buffer's
  G SUF LANG DIR
               register a grammar for files ending in SUF

Files and buffers
  e FILE       load into the current buffer
  E FILE       load into a new buffer
  w [FILE]     write
  b            list buffers
  bN           switch to buffer N
  @N CMD       run CMD against buffer N, then come back

Background
  &c FILE      count a file
  &e FILE      load a file
  &            list jobs in flight

  q            quit

Escapes in TEXT: \n  \t  \\
In the replacement of c and s, \1 is the text being replaced.
```

## Where to go next

[README.md](README.md) explains what papri is built out of and why.
[CLAUDE.md](CLAUDE.md) has the conventions the code follows — the Verifiable
C subset, the specs on every function, and the three gates that enforce them.
