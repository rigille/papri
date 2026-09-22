# io_uring. src/io.c is the only file that includes liburing.h, so only it
# gets these flags — every other file sees the opaque interface in src/io.h
# and stays inside the subset. Kept as -isystem so the library's own
# headers, which use zero-length arrays and C23 enum values, do not trip
# -Wpedantic -Werror.
URING_CFLAGS := $(shell pkg-config --cflags liburing 2>/dev/null)
URING_LIBS   := $(shell pkg-config --libs liburing 2>/dev/null || echo -luring)
URING_SYSTEM := $(URING_CFLAGS:-I%=-isystem %)

# tree-sitter, the second foreign boundary. Grammars are dlopened at run
# time, so only the library itself is linked.
TS_CFLAGS := $(shell pkg-config --cflags tree-sitter 2>/dev/null)
TS_LIBS   := $(shell pkg-config --libs tree-sitter 2>/dev/null || echo -ltree-sitter)
TS_SYSTEM := $(TS_CFLAGS:-I%=-isystem %)

# The garbage collector, the third foreign boundary. src/collector.c is the
# only file that includes gc.h; everything else sees src/collector.h, which
# is plain C and stays inside the subset.
GC_CFLAGS := $(shell pkg-config --cflags bdw-gc 2>/dev/null)
GC_LIBS   := $(shell pkg-config --libs bdw-gc 2>/dev/null || echo -lgc)
GC_SYSTEM := $(GC_CFLAGS:-I%=-isystem %)

# papri — see CLAUDE.md for the rules this build enforces.

CC    ?= clang
AR    ?= ar

# The sibling repo holding the verified sources we vendor. `make vendor-check`
# verifies our copy still matches it.
TOOLS ?= ../tools

BUILD := build
OBJ   := $(BUILD)/obj

CSTD  := -std=c17
COPT  ?= -O2 -g

# Warnings that mechanically enforce parts of the Verifiable C subset.
CWARN := -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wvla \
         -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition \
         -Werror=int-to-pointer-cast -Werror=pointer-to-int-cast \
         -Werror=int-conversion

# clang only, and only a fast partial catch for "no struct passing or
# returning by value": the threshold is a size, so it cannot tell a struct
# from a scalar, and 17 is the smallest bound that clears every scalar type
# (long double and __int128 are 16 bytes). A struct of 16 bytes or less slips
# through here; `make normalform` catches it exactly, off the Clight AST.
ifneq (,$(findstring clang,$(shell $(CC) --version 2>/dev/null | head -1)))
  CWARN += -Wlarge-by-value-copy=17
endif

INCLUDE := -Isrc -Ivendor/utf8

# io_uring. src/io.c is the only file that touches it, and the only one
# exempt from the normal-form gate; see src/io.h.
URING_CFLAGS := $(shell pkg-config --cflags liburing 2>/dev/null)
URING_LIBS   := $(shell pkg-config --libs liburing 2>/dev/null || echo -luring)

CFLAGS  := $(CSTD) $(COPT) $(CWARN) $(INCLUDE) -fno-common

# The foreign boundary needs gnu17 for sigset_t and relaxed warnings for
# liburing's headers. Nothing else in the tree is built this way.
IO_CFLAGS := -std=gnu17 $(COPT) -Wall -Wextra $(INCLUDE) $(URING_SYSTEM) \
             -fno-common

# Same treatment for the tree-sitter boundary.
STRUCTURE_CFLAGS := -std=gnu17 $(COPT) -Wall -Wextra $(INCLUDE) $(TS_SYSTEM) \
                    -fno-common

# And for the collector boundary. gc.h declares its allocators with
# __attribute__((malloc, alloc_size)), which -Wpedantic -Werror will not have.
COLLECTOR_CFLAGS := -std=gnu17 $(COPT) -Wall -Wextra $(INCLUDE) $(GC_SYSTEM) \
                    -fno-common

# Vendored verified code is exempt from the subset and from -Werror. It has a
# machine-checked proof instead of our proxy for one; see vendor/utf8/README.md.
VENDOR_CFLAGS := $(CSTD) $(COPT) -Wall -Wextra $(INCLUDE) -fno-common

# Compile-only warning flags are unused at link time, and -Werror turns
# "argument unused during compilation" into an error. Link without them.
LDFLAGS := $(CSTD) $(COPT)
LDLIBS  := $(URING_LIBS) $(TS_LIBS) $(GC_LIBS) -ldl

LIBRARY_SOURCES := $(wildcard src/*.c)
LIBRARY_SOURCES := $(filter-out src/main.c,$(LIBRARY_SOURCES))
VENDOR_SOURCES  := vendor/utf8/utf8.c
PROGRAM_SOURCES := src/main.c
TEST_SOURCES    := $(wildcard test/*.c)

LIBRARY_OBJECTS := $(LIBRARY_SOURCES:%.c=$(OBJ)/%.o)
VENDOR_OBJECTS  := $(VENDOR_SOURCES:%.c=$(OBJ)/%.o)
PROGRAM_OBJECTS := $(PROGRAM_SOURCES:%.c=$(OBJ)/%.o)
TEST_BINARIES   := $(TEST_SOURCES:test/%.c=$(BUILD)/%)

LIBRARY := $(BUILD)/libpapri.a
PROGRAM := $(BUILD)/papri

.PHONY: all test lint normalform check vendor-check asan compiledb clean

all: $(PROGRAM)

$(LIBRARY): $(LIBRARY_OBJECTS) $(VENDOR_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(PROGRAM): $(PROGRAM_OBJECTS) $(LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(PROGRAM_OBJECTS) $(LIBRARY) $(LDLIBS)

# One binary per test file, so each keeps its own main.
$(BUILD)/test_%: $(OBJ)/test/test_%.o $(LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $< $(LIBRARY) $(LDLIBS)

$(OBJ)/src/io.o: src/io.c
	@mkdir -p $(dir $@)
	$(CC) $(IO_CFLAGS) -MMD -MP -c -o $@ $<

$(OBJ)/src/structure.o: src/structure.c
	@mkdir -p $(dir $@)
	$(CC) $(STRUCTURE_CFLAGS) -MMD -MP -c -o $@ $<

$(OBJ)/src/collector.o: src/collector.c
	@mkdir -p $(dir $@)
	$(CC) $(COLLECTOR_CFLAGS) -MMD -MP -c -o $@ $<

$(OBJ)/vendor/%.o: vendor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(VENDOR_CFLAGS) -MMD -MP -c -o $@ $<

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

test: $(TEST_BINARIES)
	@status=0; \
	for binary in $(TEST_BINARIES); do \
	  echo "== $$binary"; \
	  ./$$binary || status=1; \
	done; \
	exit $$status
# Built into its own directory: a sanitized object file cannot be linked by
# an ordinary build, so the two trees must not share one.
#
# detect_stack_use_after_return must stay off. It moves locals onto a "fake
# stack" off to the side, and the collector finds its roots by scanning the
# real one — a rope root living in a fake frame would be invisible, and the
# tree under it would be collected while still in use. The sanitizer would
# be creating the bug it is there to find.
asan:
	@ASAN_OPTIONS=detect_stack_use_after_return=0 \
	 $(MAKE) --no-print-directory test BUILD=$(BUILD)/asan \
	    COPT="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	          -fno-sanitize-recover=all"

# Invoked through bash rather than the shebang: a Nix build sandbox has no
# /usr/bin/env, so `nix flake check` cannot resolve one.
# Grep gate: the rules no compiler flag and no AST check can see.
# vendor/ is exempt on purpose — see vendor/utf8/README.md.
lint:
	@bash tools/check_subset.sh $(LIBRARY_SOURCES) $(PROGRAM_SOURCES) $(TEST_SOURCES) \
	    $(wildcard src/*.h)

# clightgen gate: the source is already in the program logic's normal form,
# and no struct crosses a function boundary by value. vendor/ is exempt.
# See tools/check_clight.sh.
# Three files cannot go through this gate, all foreign boundaries kept
# deliberately thin and all still covered by `make lint`:
#   src/io.c        liburing.h reaches stdatomic.h; CompCert stops at _Atomic
#   src/structure.c tree-sitter and dlfcn, same story
#   src/collector.c gc.h, whose allocator attributes CompCert will not parse
# See src/io.h, src/structure.h and src/collector.h. Each is a handful of
# wrappers and nothing else, which is the price of the exemption: everything
# that reasons lives on our side of the boundary, inside the gate.
NORMALFORM_SOURCES := $(filter-out src/io.c src/structure.c src/collector.c,\
                      $(LIBRARY_SOURCES)) $(PROGRAM_SOURCES)

NORMALFORM := $(BUILD)/normalform

normalform:
	@CLIGHT_INCLUDE="$(INCLUDE)" bash tools/check_clight.sh $(NORMALFORM) $(NORMALFORM_SOURCES)

# The proof in $(TOOLS) is against clightgen -normalize of that exact file, so
# any drift in our copy silently voids it.
vendor-check:
	@status=0; \
	recorded=$$(cat vendor/utf8/utf8.c.sha256); \
	actual=$$(sha256sum vendor/utf8/utf8.c | cut -d' ' -f1); \
	if [ "$$recorded" = "$$actual" ]; then \
	  echo "  vendor ok         vendor/utf8/utf8.c matches its recorded hash"; \
	else \
	  echo "  VENDOR EDITED     vendor/utf8/utf8.c does not match its hash"; \
	  echo "    The Coq proof in $(TOOLS) is against this exact file. Revert"; \
	  echo "    the edit, or re-vendor and re-prove; do not edit ours."; \
	  status=1; \
	fi; \
	if [ -f $(TOOLS)/verifiable-c/utf8.c ]; then \
	  if cmp -s vendor/utf8/utf8.c $(TOOLS)/verifiable-c/utf8.c; then \
	    echo "  vendor ok         and matches $(TOOLS)"; \
	  else \
	    echo "  VENDOR DRIFT      upstream $(TOOLS) has moved on"; \
	    status=1; \
	  fi; \
	else \
	  echo "  vendor note       $(TOOLS) not present, hash check only"; \
	fi; \
	exit $$status


check: lint normalform vendor-check test

compiledb:
	bear -- $(MAKE) -B all $(TEST_BINARIES)

clean:
	rm -rf $(BUILD)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
