#!/usr/bin/env bash
#
# Runs the Mooneye test suite's ROMs that apply to a DMG through rom_test and
# compares the result with the failures this emulator is known to have.
#
# Usage: tools/mooneye.sh [rom_test binary]      (default: build/rom_test)
#
# The suite is downloaded once into roms/mooneye/ (ignored by git), from a
# fixed release and checked against a SHA-256, so every run sees the same
# ROMs. Needs bash, curl, tar and sha256sum.
#
# tools/mooneye-expected-failures.txt lists the ROMs that are known to fail,
# one path per line relative to the suite (# starts a comment). The script
# exits 1 only when a ROM fails that is not on the list, so a regression
# shows up while known gaps do not; a listed ROM that now passes is
# reported so the line can be removed.
#
# Environment:
#   MOONEYE_DIR     where the suite lives (default roms/mooneye)
#   MOONEYE_CYCLES  T-cycle budget per ROM (default 40000000)

set -u

VERSION="mts-20260714-0944-31510e1"
URL="https://gekkio.fi/files/mooneye-test-suite/$VERSION/$VERSION.tar.gz"
SHA256="5f0694e4e9e58441db23ff057a5ab681870d4cfaed91267e99885936db8e6f1d"

root="$(cd "$(dirname "$0")/.." && pwd)"
rom_test="${1:-$root/build/rom_test}"
dir="${MOONEYE_DIR:-$root/roms/mooneye}"
cycles="${MOONEYE_CYCLES:-40000000}"
expected_file="$root/tools/mooneye-expected-failures.txt"
suite="$dir/$VERSION"

if [ ! -x "$rom_test" ]; then
    echo "rom_test not found at $rom_test (build it with: make build/rom_test)" >&2
    exit 2
fi

fetch() {
    local archive
    archive="$(mktemp)"

    echo "Downloading $VERSION ..."
    if ! curl -fsSL -o "$archive" "$URL"; then
        echo "Download failed: $URL" >&2
        rm -f "$archive"
        exit 2
    fi

    if [ "$(sha256sum "$archive" | cut -d' ' -f1)" != "$SHA256" ]; then
        echo "Checksum mismatch for $URL" >&2
        rm -f "$archive"
        exit 2
    fi

    mkdir -p "$dir"
    tar -xzf "$archive" -C "$dir"
    rm -f "$archive"
}

[ -d "$suite" ] || fetch

# A ROM named NAME-SUFFIX.gb targets specific hardware. GS covers the DMG,
# MGB and SGB families, and dmgABC the DMG revisions this emulator models.
applies_to_dmg() {
    local name="${1%.gb}"

    case "$name" in
        *-*)
            case "${name##*-}" in
                GS|dmgABC|dmgABCmgb) return 0 ;;
                *) return 1 ;;
            esac
            ;;
        *) return 0 ;;
    esac
}

declare -A expected
if [ -f "$expected_file" ]; then
    while IFS= read -r line; do
        line="${line%%#*}"
        line="$(echo "$line" | tr -d '\r' | xargs)"
        [ -n "$line" ] && expected["$line"]=1
    done < "$expected_file"
fi

passed=0
failed=0
unexpected_failures=()
unexpected_passes=()

while IFS= read -r rom; do
    rel="${rom#"$suite"/}"

    applies_to_dmg "$(basename "$rom")" || continue

    result="$("$rom_test" "$rom" "$cycles" 2>&1 | head -1)"

    case "$result" in
        "[PASS]"*)
            passed=$((passed + 1))
            if [ -n "${expected[$rel]+x}" ]; then
                unexpected_passes+=("$rel")
            fi
            ;;
        *)
            failed=$((failed + 1))
            verdict="${result%%]*}"
            verdict="${verdict#[}"
            if [ -z "${expected[$rel]+x}" ]; then
                unexpected_failures+=("$rel ($verdict)")
            fi
            ;;
    esac
done < <(find "$suite/acceptance" "$suite/emulator-only" -name '*.gb' | sort)

total=$((passed + failed))

echo "Mooneye $VERSION, DMG ROMs: $passed of $total pass"

if [ "${#unexpected_passes[@]}" -ne 0 ]; then
    echo
    echo "Now passing, remove from tools/mooneye-expected-failures.txt:"
    printf '  %s\n' "${unexpected_passes[@]}"
fi

if [ "${#unexpected_failures[@]}" -ne 0 ]; then
    echo
    echo "Not on the expected-failures list:"
    printf '  %s\n' "${unexpected_failures[@]}"
    exit 1
fi

exit 0
