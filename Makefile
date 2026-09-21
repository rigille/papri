# papri — see CLAUDE.md for the rules this build enforces.

CC    ?= clang
AR    ?= ar

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

CFLAGS  := $(CSTD) $(COPT) $(CWARN) -Isrc -fno-common

# Compile-only warning flags are unused at link time, and -Werror turns
# "argument unused during compilation" into an error. Link without them.
LDFLAGS := $(CSTD) $(COPT)

LIBRARY_SOURCES := src/papri.c
PROGRAM_SOURCES := src/main.c
TEST_SOURCES    := test/test_papri.c

LIBRARY_OBJECTS := $(LIBRARY_SOURCES:%.c=$(OBJ)/%.o)
PROGRAM_OBJECTS := $(PROGRAM_SOURCES:%.c=$(OBJ)/%.o)
TEST_OBJECTS    := $(TEST_SOURCES:%.c=$(OBJ)/%.o)

LIBRARY := $(BUILD)/libpapri.a
PROGRAM := $(BUILD)/papri
TESTS   := $(BUILD)/test_papri

.PHONY: all test lint normalform check compiledb clean

all: $(PROGRAM)

$(LIBRARY): $(LIBRARY_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(PROGRAM): $(PROGRAM_OBJECTS) $(LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(PROGRAM_OBJECTS) $(LIBRARY)

$(TESTS): $(TEST_OBJECTS) $(LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJECTS) $(LIBRARY)

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

test: $(TESTS)
	./$(TESTS)

# Grep gate: the rules no compiler flag and no AST check can see.
lint:
	@tools/check_subset.sh $(LIBRARY_SOURCES) $(PROGRAM_SOURCES) $(TEST_SOURCES) src/papri.h

# clightgen gate: the source is already in the program logic's normal form,
# and no struct crosses a function boundary by value. See tools/check_clight.sh.
NORMALFORM := $(BUILD)/normalform

normalform:
	@tools/check_clight.sh $(NORMALFORM) $(LIBRARY_SOURCES) $(PROGRAM_SOURCES)

check: lint normalform test

compiledb:
	bear -- $(MAKE) -B all $(TESTS)

clean:
	rm -rf $(BUILD)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
