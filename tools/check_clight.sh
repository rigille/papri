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
#    One class of hoist is exempt. C promotes every sub-int lvalue to int and
#    converts back, so `oldest = ring->storage[head];` with byte-typed
#    storage emits the load under an Ecast, and -normalize hoists it. No way
#    of writing that statement in C avoids the cast, so a byte-exact diff is
#    a gate that correct code cannot pass. A hoisted temporary whose every
#    use sits directly under an Ecast is therefore reported and allowed;
#    every other hoist fails the build.
#
#    Known limit of that exemption: a genuine violation whose hoisted load is
#    also consumed by a cast — `f((int)byte_in_memory)` — is tolerated.
#
# 2. No struct crosses a boundary by value. In Clight a by-value struct
#    parameter or return is a bare `Tstruct`/`Tunion`; passing it by pointer
#    wraps it in `tptr`. So every occurrence inside `fn_return` or
#    `fn_params` must be under a `tptr`. This is exact, unlike clang's
#    -Wlarge-by-value-copy, which thresholds on size and cannot tell a
#    struct from a long double.

set -u

output_directory="$1"
shift

status=0
mkdir -p "$output_directory"

# clightgen stamps its own setting into the AST; that line is not signal.
strip_metadata='/Definition normalized :=/d'

for source in "$@"; do
    name=$(basename "$source" .c)
    plain="$output_directory/$name.plain.v"
    normalized="$output_directory/$name.normalized.v"

    clightgen -Isrc -o "$plain" "$source" || exit 1
    clightgen -Isrc -normalize -o "$normalized" "$source" || exit 1

    if diff -q <(sed "$strip_metadata" "$plain") \
               <(sed "$strip_metadata" "$normalized") >/dev/null; then
        echo "  normal form ok    $source"
    else
        flattened=$(tr '\n' ' ' < "$normalized" | tr -s ' ')

        # Temporaries -normalize had to invent: present in its AST, absent
        # from the plain one.
        introduced=$(comm -13 \
            <(grep -oE "_t'[0-9]+" "$plain" | sort -u) \
            <(grep -oE "_t'[0-9]+" "$normalized" | sort -u))

        if [ -z "$introduced" ]; then
            echo "  UNEXPECTED DIFF   $source"
            echo "    -normalize changed the AST without introducing a"
            echo "    temporary. Inspect by hand:"
            diff -u <(sed "$strip_metadata" "$plain") \
                    <(sed "$strip_metadata" "$normalized") \
                | sed -n '1,40p' | sed 's/^/    /'
            status=1
            continue
        fi

        promotions=0
        violations=""
        for temporary in $introduced; do
            uses=$(echo "$flattened" | grep -o "Etempvar $temporary " | wc -l)
            casts=$(echo "$flattened" | grep -o "Ecast (Etempvar $temporary " | wc -l)
            if [ "$uses" -eq "$casts" ]; then
                promotions=$((promotions + 1))
            else
                violations="$violations $temporary"
            fi
        done

        if [ -z "$violations" ]; then
            echo "  normal form ok    $source  ($promotions sub-int promotion hoist(s))"
        else
            echo "  NOT NORMAL        $source"
            echo "    -normalize hoisted a load out of an expression:$violations"
            echo "    A load may not sit inside a call argument, a condition,"
            echo "    or alongside another dereference. Split the statement."
            diff -u <(sed "$strip_metadata" "$plain") \
                    <(sed "$strip_metadata" "$normalized") \
                | sed -n '1,40p' | sed 's/^/    /'
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
