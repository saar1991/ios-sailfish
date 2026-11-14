#!/bin/bash
# Usage: bash resolve-callers.sh /path/to/dyld_shared_cache_arm64 callers-slow.txt callers-fast.txt
# Resolves unslid shared-cache addresses (from the sweetgl profiler) to function names with ipsw.
set -u
cache=$1; shift
for f in "$@"; do
    echo "=== $f"
    while read -r addr count; do
        name=$(ipsw dyld a2s "$cache" "$addr" 2>/dev/null | grep '^0x' | tail -1 | cut -d' ' -f2-)
        printf '%8s  %s  %s\n' "$count" "$addr" "${name:-?}"
    done < "$f"
done
