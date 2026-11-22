#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
result=$(
    "$root/pgn-eco" --check --per-game \
        --eco-book "$root/tests/data/public-smoke-book.pgn" \
        "$root/tests/data/public-smoke-game.pgn" 2>&1
) || status=$?
status=${status:-0}
if [ "$status" -ne 0 ]; then
    printf '%s\n' "$result" >&2
    echo "pgn-eco smoke check exited with status $status" >&2
    exit 1
fi
printf '%s\n' "$result" | grep -F 'calculated=C20 | status=exact' >/dev/null
