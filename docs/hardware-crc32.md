# Hardware CRC-32

**Branch:** `perf/hardware-crc32`
**Status:** implemented, measured

## Why

Benchmarking showed point lookups were bound by checksum computation, not by
disk or by tree logic.

The pager verifies a page's CRC-32 on every read, and there is no buffer pool,
so a point lookup on a 3-level tree checksums roughly 20 KB to return a 40-byte
row. The measurements were unambiguous:

- 780 MB checksummed across 50,000 lookups
- ~545 MB/s throughput, which is exactly what a byte-at-a-time table-driven
  CRC-32 achieves
- only **8.6%** of lookup wall time was I/O — the rest was CPU, almost all of it
  in `khb_crc32`

Predicted cost per lookup was 19.5 KB ÷ 545 MB/s ≈ 36 µs against a measured p50
of 31 µs. The two agreed closely enough to conclude that essentially all lookup
latency was checksum arithmetic.

Any B-tree tuning would have been optimising the 8%.

## What changed

`khb_crc32` now uses the ARMv8 CRC-32 instructions when the compiler reports
them available, and falls back to the original table implementation otherwise.

The hot loop consumes 8 bytes per instruction with `__crc32d`, then finishes any
remaining bytes with `__crc32b`:

```c
while (len >= 8) {
    uint64_t chunk;
    memcpy(&chunk, p, sizeof chunk);
    c = __crc32d(c, chunk);
    p += 8; len -= 8;
}
while (len-- > 0)
    c = __crc32b(c, *p++);
```

Selection happens at compile time:

```c
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
```

No build flags are needed on Apple Silicon; the CRC extension is mandatory from
ARMv8.1 and clang defines the macro by default. On any other target the table
implementation is used unchanged, so the library still builds and behaves
identically everywhere.

Two functions were added:

- `khb_crc32_table` — the original implementation, kept and exported so tests
  can compare the two.
- `khb_crc32_impl` — returns `"hardware"` or `"table"`, for diagnostics.

## Correctness

**This change does not alter the on-disk format.** That is the whole point of
the design, and it needs stating precisely, because getting it wrong would make
every existing database file unreadable.

ARMv8 provides two families of instruction:

| instruction | polynomial | matches our format |
|---|---|---|
| `crc32b/h/w/x` | IEEE 802.3, `0x04C11DB7` | **yes** |
| `crc32cb/ch/cw/cx` | Castagnoli, `0x1EDC6F41` | no |

We use the first family. It computes the same reflected IEEE CRC-32 as the
table implementation, with the same `0xFFFFFFFF` pre-conditioning and final
XOR, so results are bit-identical.

> **Do not substitute the `crc32c*` intrinsics.** They are faster on some
> hardware and appear in most CRC examples online, but they compute a different
> value. Every page in every existing database would fail verification.

Three levels of verification back this up:

1. **Standard check vector.** `khb_crc32("123456789", 9) == 0xCBF43926` for both
   implementations.
2. **Equivalence test** (`test_crc_impl_agreement` in `test/test_page.c`).
   Compares hardware against table output for every length from 0 to 64, for
   500 pseudo-random lengths up to a full page, and for the exact page-sized
   cases the pager uses. Development testing also compared 20,000 random
   buffers of random lengths; all identical.
3. **Cross-version file test.** A database was created with the table build,
   then opened and fully verified with the hardware build. All 62 pages passed,
   both table indexes validated, no problems reported.

The equivalence test runs as part of `make test`, so a future change that breaks
format compatibility fails the suite rather than corrupting data silently.

## Results

Apple M4 Pro, macOS 26.6.1, APFS, `F_FULLFSYNC` enabled. Both runs taken on the
same machine in the same session.

### Throughput

| operation | before | after | change |
|---|---|---|---|
| point lookup | 31,994/s | **171,847/s** | 5.4× |
| range scan (100 rows) | 29,236/s | **162,338/s** | 5.6× |
| full scan (88k rows) | 12.2 ms | **2.3 ms** | 5.4× |
| insert, batched | 34,114/s | **202,489/s** | 5.9× |
| insert, sequential keys | 32,607/s | **197,638/s** | 6.1× |
| delete, batched | 23,225/s | **48,172/s** | 2.1× |
| read transactions (1 process) | 40,897/s | **224,168/s** | 5.5× |
| insert, auto-wrapped | 51/s | 51/s | **unchanged** |

### Latency

| operation | p50 before | p50 after |
|---|---|---|
| point lookup | 31 µs | **5 µs** |
| range scan | 31 µs | **6 µs** |
| batched insert | 30 µs | **4 µs** |

### CRC throughput

| | before | after |
|---|---|---|
| checksum rate | ~545 MB/s | **~5,600 MB/s** |

Roughly 10× on the primitive itself, which converts to 5–6× end to end because
the remaining time is real I/O and tree work.

## Reading the results

**Auto-wrapped inserts did not move, and that is the control.** They are bound
by `fsync`, spending 94% of wall time blocked on the disk, so a faster checksum
cannot help them. That the number is unchanged confirms the change affects only
CPU-bound paths and did nothing unexpected elsewhere.

**Deletes gained less (2.1×) than lookups** because a batched delete still pays
for journal writes and their flushes; only its read half was CRC-bound.

**The profile has inverted.** Point lookups were 8.6% I/O before and are 47% I/O
now. The workload is no longer checksum-bound — the remaining cost is genuinely
reading pages. That changes what the next optimisation should be: a buffer pool
would now help, because avoiding the read is worth more than making the
verification cheaper.

**Full-scan measurements are near the instrumentation floor.** A scan is now
about 2 ms while making 1,527 timed syscalls, and the two `clock_gettime` calls
wrapping each one cost roughly 0.35 µs. That is why the reported I/O fraction
for scans exceeds 100% — the instrumentation overhead is now a significant part
of a very short measurement. Treat scan timings below ~10 ms as indicative
rather than precise.

## Limitations

- The speedup applies to ARM64 targets with the CRC extension. x86-64 has
  equivalent SSE4.2 instructions but they implement Castagnoli only, so the
  same trick does not transfer; an x86 build keeps the table implementation and
  the original performance.
- The fallback path is compiled but not exercised on this machine, beyond the
  equivalence test that calls it directly on every run.
- Checksum verification still happens on every single page read. This change
  makes that cheap; it does not make it unnecessary.
