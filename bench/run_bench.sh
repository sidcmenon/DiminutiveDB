#!/bin/sh
# Drives both harnesses and emits two CSV files.
#
#   sh bench/run_bench.sh [outdir]
#
# Set KHB_NO_FULLFSYNC=1 to measure the CPU-bound ceiling instead of real
# durability. Numbers taken that way are NOT durability numbers -- label them.

set -u

OPS=build/bench_ops
LOCK=build/bench_lock
OUT="${1:-bench/results}"
TMP="${TMPDIR:-/tmp}/khb_bench_$$"

# Rebuild first. The harnesses statically link libkhabibdb.a, so a stale
# binary silently benchmarks old code -- which is exactly how you convince
# yourself a fix did nothing.
make -s "$OPS" "$LOCK" || { echo "build failed" >&2; exit 1; }

mkdir -p "$OUT"
cleanup() { rm -f "$TMP"*.db "$TMP"*.db-journal "$TMP"*.results; }
trap cleanup EXIT
cleanup

SYNC_MODE="F_FULLFSYNC"
[ "${KHB_NO_FULLFSYNC:-}" = "1" ] && SYNC_MODE="fsync-disabled"
echo "sync mode: $SYNC_MODE"

OPS_CSV="$OUT/ops.csv"
printf 'mode,rows,batch,wall_s,ops_per_s,p50_us,p90_us,p99_us,p999_us,max_us,reads,writes,fsyncs,jrnl_records,io_ms,wait_ms,io_frac,wait_frac,syncs_per_op,reads_per_op,crc_mb,space_amp,file_bytes,visited,timer_ns\n' > "$OPS_CSV"

run() {
    mode=$1; rows=$2; db=$3; batch=${4:-100}
    printf '  %-15s rows=%-8s ' "$mode" "$rows"
    "$OPS" "$mode" "$rows" "$db" "$batch" >> "$OPS_CSV" || return 1
    tail -1 "$OPS_CSV" | awk -F, '{printf "%10.0f ops/s  p50=%7.2fus  p99=%8.2fus  io=%4.1f%%\n", $5, $6, $8, $17*100}'
}

echo
echo "== write path =="
# Auto-wrapped is deliberately small; it is orders of magnitude slower.
run insert_auto 2000 "$TMP-auto.db"

for n in 10000 100000; do
    run insert_batched "$n" "$TMP-batch-$n.db"
done

run insert_seq 100000 "$TMP-seq.db"

echo
echo "== read path (against the 100k batched table) =="
run lookup     50000 "$TMP-batch-100000.db"
run scan           1 "$TMP-batch-100000.db"
run scan_range  2000 "$TMP-batch-100000.db" 100

echo
echo "== delete (batched), then rescan to expose unreclaimed leaves =="
run delete 50000 "$TMP-batch-100000.db"
run scan       1 "$TMP-batch-100000.db"

echo
echo "== scaling curve =="
for n in 1000 10000 100000; do
    db="$TMP-scale-$n.db"
    "$OPS" insert_batched "$n" "$db" >/dev/null
    run lookup 20000 "$db"
done

LOCK_CSV="$OUT/lock.csv"
printf 'readers,writers,backoff_us,secs,txns,txn_per_s,attempts,conflicts,conflict_rate,max_retries,acquire_mean_us,acquire_max_us,hold_mean_us\n' > "$LOCK_CSV"

lockrun() {
    printf '  r=%-2s w=%-2s backoff=%-5s ' "$1" "$2" "$4"
    "$LOCK" "$1" "$2" "$3" "$TMP-lock.db" "$4" >> "$LOCK_CSV" || return 1
    tail -1 "$LOCK_CSV" | awk -F, '{printf "%8.0f txn/s  conflict=%5.1f%%  maxretry=%-6s hold=%7.2fus\n", $6, $9*100, $10, $13}'
}

echo
echo "== lock contention (each run takes the stated seconds) =="
lockrun 1 0 3 200
lockrun 4 0 3 200
lockrun 0 2 3 200
lockrun 4 1 3 200
lockrun 4 4 3 200

echo
echo "== livelock: same load, no backoff =="
lockrun 4 4 3 0

echo
echo "wrote $OPS_CSV and $LOCK_CSV"
