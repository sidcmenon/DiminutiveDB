# DiminutiveDB Benchmarks

Two harnesses and a driver. Everything writes CSV so results can be diffed
across changes.

```bash
make bench                 # builds both harnesses and runs the full suite
sh bench/run_bench.sh      # same, if the binaries are already built
KHB_NO_FULLFSYNC=1 make bench   # CPU-bound ceiling, NOT a durability number
```

Output lands in `bench/results/ops.csv` and `bench/results/lock.csv`.

---

## Harnesses

### `bench_ops` — throughput, latency, I/O wait

```
bench_ops <mode> <rows> <db-path> [batch]
```

| mode | what it measures |
|---|---|
| `insert_auto` | each insert in its own implicit transaction — the fsync-bound worst case |
| `insert_batched` | all inserts inside one explicit transaction, random keys |
| `insert_seq` | batched, ascending keys — worst case for leaf occupancy |
| `lookup` | random point lookups |
| `scan` | one full table scan |
| `scan_range` | repeated range scans of `batch` rows |
| `delete` | batched random deletes |

Collects every per-op duration into an array and sorts it, so percentiles are
exact rather than approximated by a streaming histogram.

Uses a fixed-seed xorshift PRNG, so a run is reproducible on any machine.

### `bench_lock` — contention, livelock, hold time

```
bench_lock <readers> <writers> <seconds> <db-path> [backoff_us]
```

Forks the requested readers and writers, runs them for `seconds`, and
aggregates their counters. `backoff_us 0` means retry with no delay, which is the livelock case.

---

## Instrumentation

`src/stats.h` / `src/stats.c` hold the counters. The library is instrumented by
*wrapping* `full_pread`, `full_pwrite`, `pager_fsync_fd`, `khb_crc32`,
`jrnl_append_record`, `lock_acquire` and `lock_release`

I/O wait is measured by timing each syscall with two clocks:

```c
uint64_t w0 = khb_now_ns();   /* CLOCK_MONOTONIC          */
uint64_t c0 = khb_cpu_ns();   /* CLOCK_PROCESS_CPUTIME_ID */
```

Wall elapsed minus CPU elapsed over the same span is time the process was not
running. For a span containing only a syscall that is I/O wait. It also absorbs
time descheduled for unrelated reasons, so treat it as a good proxy on an idle
machine rather than an exact figure.

Timer overhead is measured every run and reported in the `timer_ns` column
(~170–200 ns per wall+cpu pair, about 2.5% of a point lookup).

### `ops.csv` columns

| column | meaning |
|---|---|
| `wall_s`, `ops_per_s` | total elapsed, derived throughput |
| `p50_us` … `max_us` | latency percentiles, microseconds |
| `reads`, `writes`, `fsyncs` | syscall counts for the measured phase |
| `jrnl_records` | distinct pages captured by the journal |
| `io_ms`, `wait_ms` | total syscall time, and the blocked portion |
| `io_frac`, `wait_frac` | those as a fraction of wall time |
| `syncs_per_op`, `reads_per_op` | the causal variables|
| `crc_mb` | megabytes checksummed |
| `space_amp` | file size ÷ (rows × 40)|

### `lock.csv` columns

`txn_per_s`, `conflict_rate` (conflicts ÷ attempts), `max_retries` (worst single
`begin`), `acquire_mean_us` / `acquire_max_us`, `hold_mean_us`.

---

## Results

Apple M4 Pro, macOS 26.6.1, APFS, `F_FULLFSYNC` enabled. Schema is 4 columns,
40-byte rows. Checksums use the ARMv8 hardware CRC-32 instructions
(see `docs/hardware-crc32.md`).

### Write path

| mode | rows | ops/s | p50 | syncs/op | io_frac | space_amp |
|---|---|---|---|---|---|---|
| `insert_auto` | 2,000 | **50** | 19.7 ms | 5.02 | **94%** | 2.56 |
| `insert_batched` | 10,000 | 123,571 | 5 µs | 0.0007 | 70% | 1.53 |
| `insert_batched` | 100,000 | 202,489 | 4 µs | 0.0001 | 55% | 1.57 |
| `insert_seq` | 100,000 | 197,638 | 4 µs | 0.0001 | 54% | **2.45** |

**Batching is worth roughly 4,000×.** Auto-wrapped inserts spend 94% of wall
time blocked in `fsync` at 5.02 syncs per row. Batched, the journal's dedup
drops that to 0.0001 — one sync per distinct page touched, amortised across the
transaction. This is the single most important number in the system.

**Sequential keys cost 56% more space** (2.45 vs 1.57). Ascending inserts always
split the rightmost leaf, leaving every leaf about half full. A sequential-insert
special case in `split_leaf` would fix it; not implemented.

### Read path (100k-row table)

| mode | ops/s | p50 | reads/op | io_frac |
|---|---|---|---|---|
| `lookup` | 171,847 | 5 µs | 5.0 | 47% |
| `scan` | 39M rows/s | 2.3 ms | — | — |
| `scan_range` (100 rows) | 162,338 | 6 µs | 5.4 | 74% |

**Reads are now I/O-bound rather than CPU-bound.** Before hardware CRC, a point
lookup was 8.6% I/O and the rest checksum arithmetic; it is now 47% I/O. The
remaining cost is genuinely reading pages, so a buffer pool is the next lever —
avoiding the read is now worth more than making verification cheaper.

**Two of the 5 reads per lookup are transaction setup** — `pager_reload_header`
plus `catalog_load`, reloaded at every `begin`. That is the measured price of
the catalog-freshness decision.

### Deletes

Deleting 50,000 of 100,000 rows freed **zero bytes** and left the scan at
2.5 ms versus 2.3 ms. That is the "never reclaim nodes" tradeoff, quantified:
space and scan cost track pages, not live rows.

### Scaling

| rows | lookup p50 | reads/op |
|---|---|---|
| 1,000 | 5 µs | 4 |
| 10,000 | 4 µs | 4 |
| 100,000 | 5 µs | 5 |

Flat, as a B+tree should be — reads/op is tree height plus the two fixed
transaction-setup reads.

### Locking

| config | txn/s | conflict | max retries | hold |
|---|---|---|---|---|
| 1 reader | 224,168 | 0% | 0 | 4 µs |
| 4 readers | 375,607 | 0% | 0 | 10 µs |
| 2 writers | **78** | 49% | 0 | **12.8 ms** |
| 4r + 1w | 369,013 | 0.02% | 223 | 10 µs |
| 4r + 4w | 381,275 | 0.08% | 223 | 10 µs |
| 4r + 4w, **no backoff** | 9,544 | **99%** | **31,491** | 132 µs |

Readers scale 1.7× from 1 to 4. The gain is sublinear because a read transaction
is now only a few microseconds, so process scheduling and lock syscalls dominate.

**Writers serialize, and the model predicts the measurement:** 12.8 ms mean hold
time implies 1/0.0128 = 78 write transactions/second. Measured: 78. Writer hold
time is dominated by the commit fsync, so write concurrency is capped by storage
latency, not by code.

**Livelock is real and reproducible.** With backoff disabled, 99% of lock
attempts fail, a single `begin` retried 31,491 times, and throughput drops 40×.
Callers need a backoff policy; fail-fast pushes that responsibility onto them.
Note this got *worse* as the engine got faster — shorter transactions mean more
attempts per second and a tighter contention window.

**Watch the tail, not the mean.** Under 4r+4w the maximum acquire latency runs
into whole seconds against a mean of tens of microseconds — a process can starve
for essentially an entire run. Mean acquire latency is not a useful summary
under contention.

## Methodology limits

**Everything here is warm-cache.** A 6 MB database lives entirely in the OS page
cache, so `pread` is a memcpy and read "I/O wait" is near zero.

**`KHB_NO_FULLFSYNC=1`** measures a database with no durability. The header line of every run
states which mode was used.

**Instrumentation costs ~2.5% on read-heavy work** — two `clock_gettime` calls
per syscall. Reported per run as `timer_ns` so the bias is visible rather than
assumed.

**Single machine, single filesystem.** Numbers are for an M4 Pro on APFS. The
*ratios* (batching gain, CRC share, read amplification) should travel; the
absolute figures will not.
