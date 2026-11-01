#!/usr/bin/env bash
# Export canonical commits; never modify the checkout or maintain a patch copy.
set -euo pipefail
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] || { [ "$#" = 2 ] && [ "$2" != --with-kho ]; }; then
    echo "usage: $0 EMPTY_OUTPUT_DIR [--with-kho]" >&2
    exit 2
fi
root=$(git -C "$(dirname "$0")/../.." rev-parse --show-toplevel)
out=$(realpath -m "$1")
if [ -e "$out" ] && { [ ! -d "$out" ] || [ -n "$(ls -A "$out")" ]; }; then
    echo "Output must be a new or empty directory: $out" >&2
    exit 1
fi
kho=${2:-}
commits=()
while read -r kind sha rest; do
    case "$kind" in
        base) base=$sha ;;
        core) commits+=("$sha") ;;
        kho) if [ -n "$kho" ]; then commits+=("$sha"); fi ;;
    esac
done < "$root/tools/modelos/trim-series"
# Shallow boundaries make format-patch treat a normal commit as a root patch.
# Refuse that instead of emitting a multi-gigabyte whole-kernel patch.
for sha in "${commits[@]}"; do
    if ! git -C "$root" rev-parse --verify "$sha^" >/dev/null 2>&1; then
        echo "Missing series ancestry for $sha; fetch --deepen=64 from the fork branch." >&2
        exit 1
    fi
done
git -C "$root" cat-file -e "$base^{commit}"
mkdir -p "$out"
printf '%s\n' "$base" > "$out/base-commit"
: > "$out/series"
count=0
for sha in "${commits[@]}"; do
    count=$((count + 1))
    printf -v name '%04d-%s.patch' "$count" "${sha:0:12}"
    git -C "$root" format-patch -1 --stdout --full-index --no-signature "$sha" > "$out/$name"
    printf '%s\n' "$name" >> "$out/series"
done
printf 'Exported %d patches on base %s to %s\n' "$count" "$base" "$out"
