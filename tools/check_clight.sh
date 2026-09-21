#!/usr/bin/env bash
# The clightgen gate. For each translation unit it checks two things against
# the Clight AST that Verifiable C actually reasons about.
#
# 1. Normal form. Generate the AST with and without -normalize and compare.
#    Normalization hoists loads out of expressions into temporaries, so any
#    temporary it has to introduce marks a load hiding inside a condition, a
#    call argument, or a second dereference — the rules in CLAUDE.md that no
#    compiler flag can see.
#
#    The comparison is PER FUNCTION. clightgen numbers temporaries per
#    translation unit, so in a file with many functions the set of _t' names
#    is usually unchanged even when a function gains one; comparing whole
#    files misses exactly the case the gate exists for.
#
#    One class of hoist is exempt. C promotes every sub-int lvalue to int and
#    converts back, so `value = bytes[index];` on a byte-typed array emits the
#    load under an Ecast and -normalize hoists it. No way of writing that
#    statement in C avoids the cast, so a byte-exact diff is a gate correct
#    code cannot pass. A hoisted temporary whose every use sits directly under
#    an Ecast is therefore reported and allowed; every other hoist fails.
#
#    Known limit of that exemption: a genuine violation whose hoisted load is
#    also consumed by a cast — `f((int)byte_in_memory)` — is tolerated.
#
# 2. No struct crosses a boundary by value. In Clight a by-value struct
#    parameter or return is a bare `Tstruct`/`Tunion`; passing it by pointer
#    wraps it in `tptr`. clightgen refuses such a program outright without
#    -fstruct-passing, so this is a second line of defence rather than the
#    first.

set -u

include_flags="-std=c11 ${CLIGHT_INCLUDE:--Isrc}"

output_directory="$1"
shift

status=0
mkdir -p "$output_directory"

# Derived lines that always differ once a temporary appears, and the flag
# clightgen stamps into its own output. Neither is signal.
strip_derived='/Definition normalized :=/d; /^Definition _t'"'"'[0-9]* : ident :=/d'

# Write each `Definition f_<name> := {| … |}.` block to its own file.
split_functions() {
    awk -v prefix="$2" '
        /^Definition f_[A-Za-z0-9_]+ := \{\|/ {
            name = $2
            sub(/^f_/, "", name)
            target = prefix "." name
            emitting = 1
        }
        emitting { print > target }
        emitting && /^\|\}\.$/ { emitting = 0; close(target) }
    ' "$1"
}

# Is every temporary this function gained consumed directly by a cast?
# Prints the offending temporaries, or nothing when all are promotions.
classify_hoists() {
    local plain_function="$1"
    local normalized_function="$2"
    local flattened
    local introduced
    local temporary
    local uses
    local casts

    flattened=$(tr '\n' ' ' < "$normalized_function" | tr -s ' ')
    introduced=$(comm -13 \
        <(grep -oE "_t'[0-9]+" "$plain_function" | sort -u) \
        <(grep -oE "_t'[0-9]+" "$normalized_function" | sort -u))

    for temporary in $introduced; do
        uses=$(echo "$flattened" | grep -o "Etempvar $temporary " | wc -l)
        casts=$(echo "$flattened" | grep -o "Ecast (Etempvar $temporary " | wc -l)
        if [ "$uses" -ne "$casts" ]; then
            printf '%s ' "$temporary"
        fi
    done
}

for source in "$@"; do
    name=$(basename "$source" .c)
    plain="$output_directory/$name.plain.v"
    normalized="$output_directory/$name.normalized.v"
    work="$output_directory/$name.functions"

    clightgen $include_flags -o "$plain" "$source" || exit 1
    clightgen $include_flags -normalize -o "$normalized" "$source" || exit 1

    if diff -q <(sed "$strip_derived" "$plain") \
               <(sed "$strip_derived" "$normalized") >/dev/null; then
        echo "  normal form ok    $source"
    else
        rm -rf "$work"
        mkdir -p "$work"
        split_functions "$plain" "$work/plain"
        split_functions "$normalized" "$work/normalized"

        promotions=0
        differing=0
        violations=""

        for plain_function in "$work"/plain.*; do
            [ -e "$plain_function" ] || continue
            function_name=${plain_function#"$work"/plain.}
            normalized_function="$work/normalized.$function_name"
            [ -e "$normalized_function" ] || continue

            if diff -q "$plain_function" "$normalized_function" >/dev/null; then
                continue
            fi
            differing=$((differing + 1))

            offenders=$(classify_hoists "$plain_function" "$normalized_function")
            if [ -z "$offenders" ]; then
                promotions=$((promotions + 1))
            else
                violations="$violations
    $function_name: $offenders"
            fi
        done

        if [ "$differing" -eq 0 ]; then
            echo "  UNEXPECTED DIFF   $source"
            echo "    -normalize changed the AST outside any function body."
            echo "    Inspect by hand:"
            diff -u <(sed "$strip_derived" "$plain") \
                    <(sed "$strip_derived" "$normalized") \
                | sed -n '1,40p' | sed 's/^/    /'
            status=1
        elif [ -z "$violations" ]; then
            echo "  normal form ok    $source  ($promotions sub-int promotion hoist(s))"
        else
            echo "  NOT NORMAL        $source"
            echo "    -normalize hoisted a load out of an expression in:$violations"
            echo "    A load may not sit inside a call argument, a condition,"
            echo "    or alongside another dereference. Split the statement."
            echo "    Remember that taking a local's address moves it to"
            echo "    memory, so every later mention of it becomes a load."
            status=1
        fi
    fi

    flattened_plain=$(tr '\n' ' ' < "$plain" | tr -s ' ')

    by_value=0
    while read -r signature; do
        [ -n "$signature" ] || continue
        stripped=${signature//tptr (Tstruct/tptr (STRUCT}
        stripped=${stripped//tptr (Tunion/tptr (UNION}
        case "$stripped" in
            *Tstruct*|*Tunion*)
                echo "  BY-VALUE STRUCT   $source"
                echo "    $(echo "$signature" | cut -c1-200)"
                by_value=1
                ;;
        esac
    done < <(echo "$flattened_plain" | grep -oE 'fn_(return|params) := [^;]*;' || true)

    if [ "$by_value" -eq 0 ]; then
        echo "  by pointer ok     $source"
    else
        status=1
    fi
done

exit "$status"
