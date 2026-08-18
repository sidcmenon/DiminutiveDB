/*
 * Single-process benchmark: throughput, latency distribution, and disk I/O
 * wait, broken down by operation type.
 *
 * usage: bench_ops <mode> <rows> <db-path> [batch]
 *
 * modes:
 *   insert_auto     each insert in its own implicit transaction
 *   insert_batched  all inserts inside one explicit transaction
 *   insert_seq      batched, ascending keys (worst case for leaf occupancy)
 *   lookup          random point lookups over an existing table
 *   scan            full table scan
 *   scan_range      repeated range scans of `batch` rows
 *   delete          random deletes
 *
 * Emits one CSV row on stdout. Header comes from run_bench.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "khabibdb.h"
#include "stats.h"

#define TABLE "bench"

static const khb_column SCHEMA[] = {
    { "id",    KHB_INT64,  0 },
    { "name",  KHB_TEXT,  16 },
    { "n",     KHB_INT64,  0 },
    { "score", KHB_DOUBLE, 0 }
};
#define NCOLS 4

static uint64_t *lat;
static long      lat_n;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y);
}

static uint64_t pct(double p)
{
    long i;

    if (lat_n == 0)
        return 0;
    i = (long)(p * (double)(lat_n - 1) + 0.5);
    return lat[i];
}

/* Deterministic PRNG so runs are reproducible across machines. */
static uint64_t rng_state = 0x2545F4914F6CDD1DULL;

static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void die(const char *what, khb_status rc)
{
    fprintf(stderr, "bench_ops: %s: %s\n", what, khb_strerror(rc));
    exit(1);
}

static void fill(khb_row *r, int64_t key)
{
    khb_row_set_int(r, 0, key);
    khb_row_set_text(r, 1, "benchmark");
    khb_row_set_int(r, 2, key * 7);
    khb_row_set_double(r, 3, (double)key * 1.5);
}

static khb_status count_visit(const khb_row *row, void *ctx)
{
    (void)row;
    (*(long *)ctx)++;
    return KHB_OK;
}

int main(int argc, char **argv)
{
    const char *mode, *path;
    long        rows, batch, i;
    khb_db     *db = NULL;
    khb_row     row;
    khb_status  rc;
    uint64_t    t0, wall, overhead;
    struct stat st;
    long        visited = 0;
    double      secs, io_ns, wait_ns;

    if (argc < 4) {
        fprintf(stderr, "usage: bench_ops <mode> <rows> <db> [batch]\n");
        return 2;
    }
    mode  = argv[1];
    rows  = atol(argv[2]);
    path  = argv[3];
    batch = (argc > 4) ? atol(argv[4]) : 100;

    overhead = khb_timer_overhead_ns();

    lat = malloc(sizeof *lat * (size_t)rows);
    if (lat == NULL)
        return 1;

    rc = khb_open(&db, path, 1);
    if (rc != KHB_OK)
        die("open", rc);

    if (khb_table_exists(db, TABLE) != KHB_OK) {
        rc = khb_create_table(db, TABLE, SCHEMA, NCOLS);
        if (rc != KHB_OK)
            die("create_table", rc);
    }

    rc = khb_row_init(&row, db, TABLE);
    if (rc != KHB_OK)
        die("row_init", rc);

    /* Counters start after setup so the measured phase is clean. */
    khb_stats_reset();
    t0 = khb_now_ns();

    if (strcmp(mode, "insert_auto") == 0) {
        for (i = 0; i < rows; i++) {
            uint64_t s = khb_now_ns();
            fill(&row, (int64_t)i);
            rc = khb_insert(db, TABLE, &row);
            if (rc != KHB_OK)
                die("insert", rc);
            lat[lat_n++] = khb_now_ns() - s;
        }
    } else if (strcmp(mode, "insert_batched") == 0 ||
               strcmp(mode, "insert_seq") == 0) {
        int seq = (strcmp(mode, "insert_seq") == 0);

        rc = khb_begin(db, 0);
        if (rc != KHB_OK)
            die("begin", rc);
        for (i = 0; i < rows; i++) {
            uint64_t s   = khb_now_ns();
            int64_t  key = seq ? i : (int64_t)(rng() % (uint64_t)(rows * 4));

            fill(&row, key);
            rc = khb_insert(db, TABLE, &row);
            if (rc != KHB_OK && rc != KHB_ERR_EXISTS)
                die("insert", rc);
            lat[lat_n++] = khb_now_ns() - s;
        }
        rc = khb_commit(db);
        if (rc != KHB_OK)
            die("commit", rc);
    } else if (strcmp(mode, "lookup") == 0) {
        for (i = 0; i < rows; i++) {
            uint64_t s = khb_now_ns();
            khb_row  out;

            rc = khb_get(db, TABLE, (int64_t)(rng() % (uint64_t)rows), &out);
            if (rc != KHB_OK && rc != KHB_ERR_NOTFOUND)
                die("get", rc);
            lat[lat_n++] = khb_now_ns() - s;
            if (rc == KHB_OK)
                visited++;
        }
    } else if (strcmp(mode, "scan") == 0) {
        uint64_t s = khb_now_ns();

        rc = khb_scan(db, TABLE, NULL, count_visit, &visited);
        if (rc != KHB_OK)
            die("scan", rc);
        lat[lat_n++] = khb_now_ns() - s;
    } else if (strcmp(mode, "scan_range") == 0) {
        for (i = 0; i < rows; i++) {
            uint64_t s  = khb_now_ns();
            int64_t  lo = (int64_t)(rng() % (uint64_t)rows);

            rc = khb_scan_range(db, TABLE, lo, lo + batch, NULL,
                                count_visit, &visited);
            if (rc != KHB_OK)
                die("scan_range", rc);
            lat[lat_n++] = khb_now_ns() - s;
        }
    } else if (strcmp(mode, "delete") == 0) {
        rc = khb_begin(db, 0);
        if (rc != KHB_OK)
            die("begin", rc);
        for (i = 0; i < rows; i++) {
            uint64_t s = khb_now_ns();

            rc = khb_delete(db, TABLE, (int64_t)(rng() % (uint64_t)rows));
            if (rc != KHB_OK && rc != KHB_ERR_NOTFOUND)
                die("delete", rc);
            lat[lat_n++] = khb_now_ns() - s;
            if (rc == KHB_OK)
                visited++;
        }
        rc = khb_commit(db);
        if (rc != KHB_OK)
            die("commit", rc);
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }

    wall = khb_now_ns() - t0;
    khb_close(db);

    if (stat(path, &st) != 0)
        st.st_size = 0;

    qsort(lat, (size_t)lat_n, sizeof *lat, cmp_u64);

    secs    = (double)wall / 1e9;
    io_ns   = (double)(khb_stat.read_ns + khb_stat.write_ns + khb_stat.fsync_ns);
    wait_ns = (double)(khb_stat.read_wait_ns + khb_stat.write_wait_ns
                       + khb_stat.fsync_wait_ns);

    printf("%s,%ld,%ld,%.3f,%.1f,"
           "%.2f,%.2f,%.2f,%.2f,%.2f,"
           "%llu,%llu,%llu,%llu,"
           "%.3f,%.3f,%.3f,%.3f,%.4f,"
           "%.4f,%.4f,%.2f,%lld,%.2f,%llu\n",
           mode, rows, batch, secs,
           secs > 0 ? (double)rows / secs : 0.0,
           (double)pct(0.50) / 1e3, (double)pct(0.90) / 1e3,
           (double)pct(0.99) / 1e3, (double)pct(0.999) / 1e3,
           (double)pct(1.0) / 1e3,
           (unsigned long long)khb_stat.reads,
           (unsigned long long)khb_stat.writes,
           (unsigned long long)khb_stat.fsyncs,
           (unsigned long long)khb_stat.journal_records,
           io_ns / 1e6, wait_ns / 1e6,
           wall > 0 ? io_ns / (double)wall : 0.0,
           wall > 0 ? wait_ns / (double)wall : 0.0,
           rows > 0 ? (double)khb_stat.fsyncs / (double)rows : 0.0,
           rows > 0 ? (double)khb_stat.reads / (double)rows : 0.0,
           (double)khb_stat.crc_bytes / 1048576.0,
           rows > 0 ? (double)st.st_size / (double)(rows * 40) : 0.0,
           (long long)st.st_size,
           (double)visited,
           (unsigned long long)overhead);

    free(lat);
    return 0;
}
