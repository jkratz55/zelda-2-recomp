#!/bin/sh
# verify.sh - check the recompiled code against the reference interpreter.
#
# Builds the strict verification binary, runs every input script in tests/
# both ways and compares per-frame RAM + picture checksums. Any difference
# means the recompiler translated something incorrectly.
#
# usage: tools/verify.sh [frames]
set -e
cd "$(dirname "$0")/.."
FRAMES=${1:-1500}
cmake --build build --target zelda2 zelda2_verify -j8 >/dev/null
TMP=$(mktemp -d)
status=0
for script in tests/*.txt; do
    name=$(basename "$script" .txt)
    ./build/zelda2 --no-mods --interpret --headless "$FRAMES" --input "$script" --hash-log "$TMP/$name.interp" >/dev/null
    ./build/zelda2_verify --no-mods --no-discover --headless "$FRAMES" --input "$script" --hash-log "$TMP/$name.recomp" >"$TMP/$name.out"
    if cmp -s "$TMP/$name.interp" "$TMP/$name.recomp"; then
        echo "PASS  $name ($FRAMES frames; $(sed -n 's/.*interpreted instructions: \([0-9]*\).*/\1/p' "$TMP/$name.out") instructions still interpreted)"
    else
        first=$(diff "$TMP/$name.interp" "$TMP/$name.recomp" | sed -n '2s/^< \([0-9]*\).*/\1/p')
        echo "FAIL  $name: first difference at frame $first"
        status=1
    fi
done
rm -rf "$TMP"
exit $status
