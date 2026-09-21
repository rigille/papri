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

CFLAGS  := $(CSTD) $(COPT) $(CWARN) $(INCLUDE) -fno-common

# Vendored verified code is exempt from the subset and from -Werror. It has a
# machine-checked proof instead of our proxy for one; see vendor/utf8/README.md.
VENDOR_CFLAGS := $(CSTD) $(COPT) -Wall -Wextra $(INCLUDE) -fno-common

# Compile-only warning flags are unused at link time, and -Werror turns
# "argument unused during compilation" into an error. Link without them.
LDFLAGS := $(CSTD) $(COPT)

LIBRARY_SOURCES := $(wildcard src/*.c)
LIBRARY_SOURCES := $(filter-out src/main.c,$(LIBRARY_SOURCES))
VENDOR_SOURCES  := vendor/utf8/utf8.c
PROGRAM_SOURCES := src/main.c
TEST_SOURCES    := $(wildcard test/*.c)

LIBRARY_OBJECTS := $(LIBRARY_SOURCES:%.c=$(OBJ)/%.o)
VENDOR_OBJECTS  := $(VENDOR_SOURCES:%.c=$(OBJ)/%.o)
PROGRAM_OBJECTS := $(PROGRAM_SOURCES:%.c=$(OBJ)/%.o)
TEST_OBJECTS    := $(TEST_SOURCES:%.c=$(OBJ)/%.o)

LIBRARY := $(BUILD)/libpapri.a
PROGRAM := $(BUILD)/papri
TESTS   := $(BUILD)/test_papri

.PHONY: all test lint normalform check vendor-check asan compiledb clean

all: $(PROGRAM)

$(LIBRARY): $(LIBRARY_OBJECTS) $(VENDOR_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(PROGRAM): $(PROGRAM_OBJECTS) $(LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(PROGRAM_OBJECTS) $(LIBRARY)

$(TESTS): $(TEST_OBJECTS) $(LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJECTS) $(LIBRARY)

$(OBJ)/vendor/%.o: vendor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(VENDOR_CFLAGS) -MMD -MP -c -o $@ $<

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

test: $(TESTS)
	./$(TESTS)

# A hand-rolled reclamation scheme needs these; neither sibling repo has them.
asan:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory test \
	    COPT="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	          -fno-sanitize-recover=all"

# Grep gate: the rules no compiler flag and no AST check can see.
# vendor/ is exempt on purpose — see vendor/utf8/README.md.
lint:
	@tools/check_subset.sh $(LIBRARY_SOURCES) $(PROGRAM_SOURCES) $(TEST_SOURCES) \
	    $(wildcard src/*.h)

# clightgen gate: the source is already in the program logic's normal form,
# and no struct crosses a function boundary by value. vendor/ is exempt.
# See tools/check_clight.sh.
NORMALFORM := $(BUILD)/normalform

normalform:
	@CLIGHT_INCLUDE="$(INCLUDE)" tools/check_clight.sh $(NORMALFORM) $(LIBRARY_SOURCES) $(PROGRAM_SOURCES)

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
	bear -- $(MAKE) -B all $(TESTS)

clean:
	rm -rf $(BUILD)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
