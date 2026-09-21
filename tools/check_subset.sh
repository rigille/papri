#!/usr/bin/env bash
# Grep gate for the rules in CLAUDE.md that the other two gates cannot see.
# Deliberately crude: it strips whole-line comments and then matches patterns,
# so a keyword inside a string or a trailing comment can fool it, and a
# function whose parameter list spans lines is not counted. It is a backstop
# for review, not a parser.
#
# Covered elsewhere, and skipped here:
#   integer <-> pointer casts       -> -Werror=int-to-pointer-cast etc.
#   struct passing by value         -> clightgen rejects it outright
#   loads in conditions, double
#     dereference per statement     -> make normalform
#
# Not yet covered by any gate: calls nested in subexpressions.
# clightgen factors calls out of expressions whether or not -normalize is
# given, so the normal-form diff is blind to them.
#
# Review-only, caught by nothing: struct assignment between two objects
# (`*destination = *source;` must be an explicit memcpy).

set -u

status=0

report() {
    printf '  %-18s %s:%s: %s\n' "$1" "$2" "$3" "$4"
    status=1
}

# Drop full-line comments so a rule named in a spec comment is not a hit.
strip_comments() {
    sed -e 's://.*::' -e '/^[[:space:]]*\*/d' -e '/^[[:space:]]*\/\*/d' "$1"
}

# Blank out the control keywords and operators that look like calls, so what
# is left followed by `(` is a genuine function call.
blank_keyword_calls() {
    sed -E 's/\b(if|while|for|switch|return|sizeof|defined|_Alignof|_Static_assert)[[:space:]]*\(/ (/g'
}

scan() {
    local label="$1" file="$2" pattern="$3" filter="${4:-cat}"
    local line text
    while IFS=: read -r line text; do
        [ -n "${line:-}" ] || continue
        report "$label" "$file" "$line" "$(echo "$text" | sed 's/^[[:space:]]*//')"
    done < <(echo "$BODY" | grep -nE "$pattern" | eval "$filter" || true)
}

for file in "$@"; do
    if [ ! -f "$file" ]; then
        printf '  %-18s %s: no such file\n' "MISSING" "$file"
        status=1
        continue
    fi

    BODY=$(strip_comments "$file")

    scan "GOTO"      "$file" '\bgoto\b'
    scan "VOLATILE"  "$file" '\bvolatile\b'
    scan "SETJMP"    "$file" '\b(setjmp|longjmp|sigsetjmp|siglongjmp)\b'
    scan "VARIADIC"  "$file" '\b(va_list|va_start|va_arg|va_end)\b'

    # Increment or decrement anywhere but alone on its own statement.
    scan "EMBEDDED ++/--" "$file" '(\+\+|--)' \
        "grep -vE ':[[:space:]]*[A-Za-z_][A-Za-z0-9_]*(\+\+|--);[[:space:]]*\$' \
         | grep -vE ':[[:space:]]*(\+\+|--)[A-Za-z_][A-Za-z0-9_]*;[[:space:]]*\$' \
         | grep -vE 'for[[:space:]]*\('"

    # TODO: calls nested in subexpressions. No other gate sees them —
    # clightgen factors calls out of expressions with or without -normalize.
    # A first attempt at a grep check false-positived on string literals
    # containing parentheses ("%u byte(s):") and missed a lone call inside an
    # arithmetic expression. Parked; review-only until it is rewritten to
    # blank string literals and to require the statement be exactly
    # `f(args);` or `lhs = f(args);`.

    # Spec coverage: every function needs a requires/ensures block. Counts
    # headers at column 0 (brace on the same line or the next) plus
    # prototypes, against the number of `requires:` lines.
    definitions=$(echo "$BODY" | grep -cE '^[A-Za-z_][^;]*\)[[:space:]]*\{?[[:space:]]*$' || true)
    declarations=$(echo "$BODY" | grep -cE '^[A-Za-z_].*\(.*\)[[:space:]]*;[[:space:]]*$' || true)
    contracts=$(grep -c 'requires:' "$file" || true)
    expected=$((definitions + declarations))

    if [ "$expected" -gt "$contracts" ]; then
        printf '  %-18s %s: %s function(s) declared or defined, %s requires: block(s)\n' \
               "MISSING SPEC" "$file" "$expected" "$contracts"
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    echo "  subset ok         $*"
fi

exit "$status"
