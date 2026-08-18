#!/bin/sh
# Kill the writer at every instrumented crash point and assert that recovery
# leaves the database either fully rolled back or fully committed.
#
# A single fixed kill point will pass while the code is still wrong, so this
# sweeps all of them.

set -u

BIN="${1:-build/crash_child}"
DB="${TMPDIR:-/tmp}/khb_crash_$$.db"

if [ ! -x "$BIN" ]; then
    echo "crash_child not found at $BIN" >&2
    exit 1
fi

cleanup() { rm -f "$DB" "$DB-journal"; }
trap cleanup EXIT

TOTAL=$("$BIN" count) || { echo "failed to enumerate crash points" >&2; exit 1; }
echo "sweeping $TOTAL crash points"

rolled=0
committed=0
fail=0

i=1
while [ "$i" -le "$TOTAL" ]; do
    cleanup
    "$BIN" setup "$DB" || { echo "setup failed at point $i" >&2; exit 1; }

    KHB_CRASH_AT="$i" "$BIN" mutate "$DB"
    rc=$?

    # 137 = 128 + SIGKILL(9): the crash point fired, which is what we want.
    if [ "$rc" -ne 137 ] && [ "$rc" -ne 9 ]; then
        echo "point $i: expected SIGKILL, got exit $rc" >&2
        fail=1
        i=$((i + 1))
        continue
    fi

    out=$("$BIN" verify "$DB")
    if [ $? -ne 0 ]; then
        echo "point $i: VERIFY FAILED" >&2
        fail=1
    else
        case "$out" in
            ROLLED_BACK) rolled=$((rolled + 1)) ;;
            COMMITTED)   committed=$((committed + 1)) ;;
        esac
        printf 'point %d: %s\n' "$i" "$out"
    fi

    i=$((i + 1))
done

echo "---"
echo "rolled back: $rolled   committed: $committed   of $TOTAL points"

if [ "$fail" -ne 0 ]; then
    echo "CRASH TESTS FAILED"
    exit 1
fi
echo "CRASH TESTS PASSED"