#!/bin/sh
# Run the portable EPT suites, then measure what they can actually detect.
#
# A suite that passes tells you nothing on its own: it may be asserting only what
# the implementation already does. This harness injects a defect into a *copy* of
# each header and requires the suite to fail. The repository copies are never
# modified.
#
# Each mutation is also run against the suite as committed, so added coverage is
# reported as a number rather than asserted. Mutations both versions kill are not
# evidence for new cases. A suite with no committed baseline reports "new".
#
# Usage: tools/hvm_paper/Test-EptLeaseMutation.sh [output-directory]
# Needs cc/gcc and git. Writes ept-lease-unit.txt and ept-lease-mutation.txt.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$root/docs/next/paper-data/20260916-lease-largepage}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || CC=gcc
base_rev=${BASE_REV:-HEAD}
hvm=$root/KswordARKDriver/src/features/hvm

mkdir -p "$out"
: > "$out/ept-lease-unit.txt"
total=0
unique=0

# Build one suite against the current headers and record its own output.
# $1 header basename, $2 suite basename
prepare() {
    cp "$hvm/$1" "$work/$1.orig"
    cp "$work/$1.orig" "$work/$1"
    sed "s#\.\./\.\./KswordARKDriver/src/features/hvm/$1#$1#" \
        "$root/tools/hvm_paper/$2" > "$work/new.c"
    if git -C "$root" cat-file -e "$base_rev:tools/hvm_paper/$2" 2>/dev/null; then
        git -C "$root" show "$base_rev:tools/hvm_paper/$2" \
            | sed "s#\.\./\.\./KswordARKDriver/src/features/hvm/$1#$1#" > "$work/old.c"
    else
        rm -f "$work/old.c"
    fi
    "$CC" -O2 -Wall -Wextra -I"$work" -o "$work/base.exe" "$work/new.c"
    "$work/base.exe" >> "$out/ept-lease-unit.txt"
}

# Print killed/survived for one suite source under the current mutated header.
run_one() {
    [ -f "$1" ] || { echo "new"; return; }
    if ! "$CC" -O2 -I"$work" -o "$work/$2.exe" "$1" >"$work/$2.cc" 2>&1; then
        echo "compile-error"
        return
    fi
    if "$work/$2.exe" >/dev/null 2>&1; then echo "survived"; else echo "killed"; fi
}

# $1 header basename, $2 mutation name, $3 sed expression
mutate() {
    cp "$work/$1.orig" "$work/$1"
    sed -i "$3" "$work/$1"
    if cmp -s "$work/$1" "$work/$1.orig"; then
        # Fatal, not a note. A mutation whose expression no longer matches the
        # source silently removes itself from the matrix, and the total at the
        # bottom keeps counting down without saying which defect stopped being
        # tested. That reads as coverage and is the opposite.
        printf '%-48s %s\n' "$2" "NOT APPLIED - source text changed"
        echo "HARNESS FAILED: mutation no longer applies: $2" >&2
        exit 1
    fi
    old=$(run_one "$work/old.c" old)
    new=$(run_one "$work/new.c" new)
    printf '%-48s %-13s %-13s\n' "$2" "$old" "$new"
    total=$((total + 1))
    [ "$new" = "killed" ] || { echo "SUITE FAILED: survived: $2" >&2; exit 1; }
    [ "$old" = "killed" ] || unique=$((unique + 1))
}

{
    echo "EPT_MUTATION: portable suites versus injected defects, current vs $base_rev"
    echo

    echo "hvm_nested_lease_walk.h - capture and drift of a source translation"
    printf '%-48s %-13s %-13s\n' "mutation" "committed" "current"
    prepare hvm_nested_lease_walk.h test_ept_lease.c
    H=hvm_nested_lease_walk.h
    mutate $H "large-leaf reserved address bits unchecked" \
        's/(entry \& KSW_LEASE_FRAME \& offsetMask) != 0ULL/0/'
    mutate $H "guest offset not folded into large frame" \
        's/ | (GuestPage \& offsetMask)//'
    mutate $H "dirty treated as metadata only at level 3" \
        's/Level == 3U || ((Level == 1U || Level == 2U) \&\& (Entry \& 0x80ULL) != 0ULL)/Level == 3U/'
    mutate $H "large-page bit accepted at the PTE level" \
        's/(level == 0U || level == 3U)/(level == 0U)/'
    mutate $H "validation always walks four levels" \
        's/level < Translation->EntryCount/level < 4U/'
    mutate $H "validation refuses short captures" \
        's/Translation->EntryCount == 0U/Translation->EntryCount < 3U/'
    mutate $H "empty permission intersection accepted" \
        's/ || permissions == 0U//'
    mutate $H "offset mask always 4 KiB" \
        's/(1ULL << shifts\[level\]) - 1ULL/(1ULL << 12) - 1ULL/'
    mutate $H "capture always reports four entries" \
        's/Translation->EntryCount = level + 1U;/Translation->EntryCount = 4U;/'
    cp "$work/$H.orig" "$work/$H"

    echo
    echo "hvm_nested_leaf_plan.h - which regions may be published as a large leaf"
    printf '%-48s %-13s %-13s\n' "mutation" "committed" "current"
    prepare hvm_nested_leaf_plan.h test_ept_leaf_plan.c
    P=hvm_nested_leaf_plan.h
    mutate $P "source-granularity test removed" 's/sourceShift < LeafShift/0/'
    mutate $P "source-granularity test inverted" \
        's/sourceShift < LeafShift/sourceShift > LeafShift/'
    mutate $P "guest base checked only for page alignment" \
        's/(GuestPage \& (regionBytes - 1ULL))/(GuestPage \& 0xFFFULL)/'
    mutate $P "backing checked only for page alignment" \
        's/(BackingPhysical \& (regionBytes - 1ULL))/(BackingPhysical \& 0xFFFULL)/'
    mutate $P "short backing accepted unless empty" \
        's/BackingBytes < regionBytes/BackingBytes == 0ULL/'
    mutate $P "region ownership off by one" 's/< Plan->RegionBytes/<= Plan->RegionBytes/'
    mutate $P "staging index off by one" \
        's/PageIndex >= Plan->PageCount/PageIndex > Plan->PageCount/'
    mutate $P "guest base bound removed" 's/GuestPage >= KSW_PLAN_GPA_LIMIT/0/'
    mutate $P "source path length mapped to wrong granularity" \
        's/EntryCount == 3U ? KSW_PLAN_SHIFT_2M/EntryCount == 3U ? KSW_PLAN_SHIFT_1G/'
    mutate $P "installed leaf carries a page offset" \
        's/Plan->BackingBase;$/Plan->BackingBase + 0x1000ULL;/'
    mutate $P "region always one page" \
        's/Plan->PageCount = regionBytes >> KSW_PLAN_SHIFT_4K;/Plan->PageCount = 1ULL;/'
    # The uniformity scan is what lets a region be published over a source that
    # maps it one page at a time, so it is the one place where a defect turns a
    # refusal into a hole rather than into a failure.
    mutate $P "proven uniformity ignored" \
        's/sourceShift < LeafShift \&\& !SourceUniform/sourceShift < LeafShift/'
    mutate $P "unproven uniformity admitted" \
        's/sourceShift < LeafShift \&\& !SourceUniform/0/'
    mutate $P "disagreeing source leaf accepted" 's/leafBits != shared/0/'
    mutate $P "accessed and dirty counted as disagreement" \
        's/entry \& 0x7FULL/entry \& 0x3FFULL/'
    mutate $P "unreadable source reported uniform" \
        's|return 1; /\* Scan is valid; Complete stays zero. \*/|{ Scan->Uniform = 1; Scan->Complete = 1; return 1; }|'
    mutate $P "absent interior table reported uniform" \
        's|if (table == 0ULL) { return 1; }|if (table == 0ULL) { Scan->Uniform = 1; Scan->Complete = 1; return 1; }|'
    mutate $P "oversized region scan bound removed" \
        's/if ((regionBytes >> Scan->Shift) >/if (0 \&\& (regionBytes >> Scan->Shift) >/'
    mutate $P "region shift read from the request not the source" \
        's|(regionBytes >> Scan->Shift)|(regionBytes >> LeafShift)|'
    cp "$work/$P.orig" "$work/$P"

    echo
    echo "mutations: $total, all killed by the current suites,"
    echo "of which the committed suites do not kill: $unique"
    echo
    echo "Scope: the headers are compiled exactly as the driver compiles them,"
    echo "against synthetic tables and synthetic geometry. This measures the"
    echo "suites, not the hardware. A 2-MiB override has since been published and"
    echo "removed on a live descendant, but that is a separate measurement kept"
    echo "with the paper evidence; nothing here executes on hardware, and real"
    echo "source-EPT mutation by a live intermediate VMM is not simulated."
} > "$out/ept-lease-mutation.txt"

cat "$out/ept-lease-unit.txt"
tail -n 9 "$out/ept-lease-mutation.txt"
