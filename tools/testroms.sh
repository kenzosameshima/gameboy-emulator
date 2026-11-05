#!/usr/bin/env bash
#
# Runs ROM suites from the Game Boy test ROM collection
# (https://github.com/c-sp/game-boy-test-roms, MIT licence) through rom_test
# and compares the results with the failures this emulator is known to have.
#
# Usage: tools/testroms.sh <suite> [rom_test binary]   (default: build/rom_test)
#
# Suites:
#   microtest   the gbmicrotest ROMs, checked on a real DMG. Each leaves a
#               verdict in high RAM (0x01 at FF82 for a pass, 0xFF for a
#               failure). A few dozen are visual and never give a verdict, so
#               they are listed as failing and stay that way.
#
# The collection is downloaded once into roms/testroms/ (ignored by git), from
# a fixed release and checked against a SHA-256, so every run sees the same
# ROMs. Needs bash, curl, unzip and sha256sum.
#
# tools/<suite>-expected-failures.txt lists the ROMs that are known to fail,
# one path per line relative to the release (# starts a comment). The script
# exits 1 only when a ROM fails that is not on the list, so a regression shows
# up while known gaps do not; a listed ROM that now passes is reported so the
# line can be removed.
#
# Environment:
#   TESTROMS_DIR     where the collection lives (default roms/testroms)
#   TESTROMS_CYCLES  T-cycle budget per ROM (default: the suite's own)
#   TESTROMS_UPDATE  set to 1 to delete ROMs that now pass from the
#                    expected-failures list

set -u

VERSION="v7.0"
URL="https://github.com/c-sp/game-boy-test-roms/releases/download/$VERSION/game-boy-test-roms-$VERSION.zip"
SHA256="b9a9d7a1075aa35a3d07c07c34974048672d8520dca9e07a50178f5860c3832c"

root="$(cd "$(dirname "$0")/.." && pwd)"
suite_name="${1:-}"
rom_test="${2:-$root/build/rom_test}"
dir="${TESTROMS_DIR:-$root/roms/testroms}"
release="$dir/$VERSION"

case "$suite_name" in
    microtest)
        rom_dir="gbmicrotest"
        rom_args=(--microtest)
        ;;
    *)
        echo "Usage: $0 microtest [rom_test binary]" >&2
        exit 2
        ;;
esac

expected_file="$root/tools/$suite_name-expected-failures.txt"

if [ ! -x "$rom_test" ]; then
    echo "rom_test not found at $rom_test (build it with: make build/rom_test)" >&2
    exit 2
fi

fetch() {
    local archive
    archive="$(mktemp)"

    echo "Downloading game-boy-test-roms $VERSION ..."
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

    mkdir -p "$release"
    unzip -q -o "$archive" 'gbmicrotest/*' -d "$release"
    rm -f "$archive"
}

[ -d "$release/$rom_dir" ] || fetch

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

cycle_args=()
[ -n "${TESTROMS_CYCLES:-}" ] && cycle_args=("${TESTROMS_CYCLES}")

while IFS= read -r rom; do
    rel="${rom#"$release"/}"

    result="$("$rom_test" "$rom" ${cycle_args[@]+"${cycle_args[@]}"} "${rom_args[@]}" 2>&1 | head -1)"

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
done < <(find "$release/$rom_dir" -name '*.gb' | sort)

total=$((passed + failed))

echo "$suite_name $VERSION: $passed of $total pass"

if [ "${#unexpected_passes[@]}" -ne 0 ]; then
    echo
    echo "Now passing, remove from tools/$suite_name-expected-failures.txt:"
    printf '  %s\n' "${unexpected_passes[@]}"

    if [ "${TESTROMS_UPDATE:-0}" = 1 ]; then
        for rel in "${unexpected_passes[@]}"; do
            grep -vxF "$rel" "$expected_file" > "$expected_file.new"
            mv "$expected_file.new" "$expected_file"
        done
        echo "Removed them from the list."
    fi
fi

if [ "${#unexpected_failures[@]}" -ne 0 ]; then
    echo
    echo "Not on the expected-failures list:"
    printf '  %s\n' "${unexpected_failures[@]}"
    exit 1
fi

exit 0
