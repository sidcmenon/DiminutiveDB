/*
 * Multi-process lock benchmark: contention rate, livelock behaviour, and
 * lock hold time.
 *
 * usage: bench_lock <readers> <writers> <seconds> <db-path> [backoff_us]
 *
 * backoff_us 0 means spin with no delay -- that is the livelock case, and it
 * is worth running deliberately to see the retry counts explode.
 * Otherwise retries use exponential backoff capped at 64x the base.
 *
 * NOTE: deadlock is structurally impossible here. There is one whole-file
 * lock and lock_acquire always passes LOCK_NB, so no process ever waits while
 * holding anything. What can happen is livelock -- mutual starvation between
 * processes retrying -- which is what this measures.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "internal.h"
#include "txn.h"
#include "stats.h"

#define TABLE "bench"

struct result {
    char     role[8];
    uint64_t txns;
    uint64_t attempts;
    uint64_t conflicts;
    uint64_t max_retries;
    uint64_t acquire_ns_total;
    uint64_t acquire_ns_max;
    uint64_t hold_ns_total;
};

static volatile sig_atomic_t stop_now;

static void on_alarm(int sig)
{
    (void)sig;
    stop_now = 1;
}

/*
 * The caller-side retry policy. lock_acquire fails fast, so waiting is
 * entirely the caller's business -- which is exactly what makes livelock
 * possible and worth measuring.
 */
static khb_status begin_with_retry(khb_db *db, int read_only,
                                   unsigned backoff_us, uint64_t *retries)
{
    unsigned delay = backoff_us;
    khb_status rc;

    *retries = 0;
    for (;;) {
        rc = txn_begin(db, read_only);
        if (rc != KHB_ERR_LOCKED)
            return rc;
        if (stop_now)
            return rc;

        (*retries)++;
        if (backoff_us > 0) {
            usleep(delay);
            if (delay < backoff_us * 64)
                delay *= 2;
        }
    }
}

static void child(const char *path, int is_writer, int seconds,
                  unsigned backoff_us, const char *out_path)
{
    khb_db        db;
    struct result r;
    int           fd;
    char          line[256];
    int64_t       key = 0;

    memset(&r, 0, sizeof r);
    snprintf(r.role, sizeof r.role, "%s", is_writer ? "writer" : "reader");

    signal(SIGALRM, on_alarm);
    alarm((unsigned)seconds);

    if (db_open(&db, path, 0) != KHB_OK)
        _exit(3);

    khb_stats_reset();

    while (!stop_now) {
        uint64_t   retries = 0;
        uint64_t   t0      = khb_now_ns();
        uint64_t   wait_ns;
        khb_status rc;

        rc = begin_with_retry(&db, !is_writer, backoff_us, &retries);
        wait_ns = khb_now_ns() - t0;

        if (rc != KHB_OK)
            break;

        r.acquire_ns_total += wait_ns;
        if (wait_ns > r.acquire_ns_max)
            r.acquire_ns_max = wait_ns;
        if (retries > r.max_retries)
            r.max_retries = retries;

        {
            table_def_t *t = NULL;
            btree_t      bt;

            if (txn_find_table(&db, TABLE, &t) == KHB_OK &&
                txn_open_tree(&db, t, &bt) == KHB_OK) {
                if (is_writer) {
                    uint8_t rowbuf[KHB_MAX_ROW_SIZE];

                    memset(rowbuf, 0, sizeof rowbuf);
                    memcpy(rowbuf, &key, sizeof key);
                    (void)btree_insert(&bt, rowbuf);
                    (void)txn_close_tree(&db, t, &bt);
                    key++;
                } else {
                    uint8_t out[KHB_MAX_ROW_SIZE];
                    (void)btree_lookup(&bt, key % 1000, out);
                }
            }
        }

        if (is_writer)
            (void)txn_commit(&db);
        else
            (void)txn_commit(&db);

        r.txns++;
    }

    db_close(&db);

    r.attempts   = khb_stat.lock_attempts;
    r.conflicts  = khb_stat.lock_conflicts;
    r.hold_ns_total = khb_stat.lock_hold_ns;

    snprintf(line, sizeof line,
             "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
             r.role,
             (unsigned long long)r.txns,
             (unsigned long long)r.attempts,
             (unsigned long long)r.conflicts,
             (unsigned long long)r.max_retries,
             (unsigned long long)r.acquire_ns_total,
             (unsigned long long)r.acquire_ns_max,
             (unsigned long long)r.hold_ns_total);

    fd = open(out_path, O_WRONLY | O_APPEND);
    if (fd >= 0) {
        ssize_t ignored = write(fd, line, strlen(line));
        (void)ignored;
        close(fd);
    }
    _exit(0);
}

static void seed_table(const char *path)
{
    khb_db       db;
    column_def_t cols[4];
    table_def_t *t = NULL;
    btree_t      bt;
    uint8_t      rowbuf[KHB_MAX_ROW_SIZE];
    int          i;

    column_def_set(&cols[0], "id",   COL_INT64, 0);
    column_def_set(&cols[1], "name", COL_TEXT, 16);
    column_def_set(&cols[2], "n",    COL_INT64, 0);

    if (db_open(&db, path, 1) != KHB_OK)
        exit(1);
    if (txn_begin(&db, 0) != KHB_OK)
        exit(1);
    if (txn_create_table(&db, TABLE, cols, 3, &t) != KHB_OK)
        exit(1);
    if (txn_open_tree(&db, t, &bt) != KHB_OK)
        exit(1);

    for (i = 0; i < 1000; i++) {
        int64_t k = i;

        memset(rowbuf, 0, sizeof rowbuf);
        memcpy(rowbuf, &k, sizeof k);
        (void)btree_insert(&bt, rowbuf);
    }

    txn_close_tree(&db, t, &bt);
    txn_commit(&db);
    db_close(&db);
}

int main(int argc, char **argv)
{
    int         readers, writers, seconds, i, n;
    unsigned    backoff;
    const char *path;
    char        out_path[256];
    FILE       *f;
    int         fd;
    char        line[256];

    uint64_t tot_txn = 0, tot_att = 0, tot_conf = 0, tot_maxret = 0;
    uint64_t tot_acq = 0, max_acq = 0, tot_hold = 0;

    if (argc < 5) {
        fprintf(stderr,
                "usage: bench_lock <readers> <writers> <secs> <db> [backoff_us]\n");
        return 2;
    }
    readers = atoi(argv[1]);
    writers = atoi(argv[2]);
    seconds = atoi(argv[3]);
    path    = argv[4];
    backoff = (argc > 5) ? (unsigned)atoi(argv[5]) : 200;

    unlink(path);
    snprintf(out_path, sizeof out_path, "%s.results", path);
    unlink(out_path);

    seed_table(path);

    fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 1;
    close(fd);

    for (i = 0; i < readers; i++)
        if (fork() == 0)
            child(path, 0, seconds, backoff, out_path);
    for (i = 0; i < writers; i++)
        if (fork() == 0)
            child(path, 1, seconds, backoff, out_path);

    for (i = 0; i < readers + writers; i++)
        wait(NULL);

    f = fopen(out_path, "r");
    if (f == NULL)
        return 1;

    n = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char     role[8];
        unsigned long long txn, att, conf, maxret, acq, acqmax, hold;

        if (sscanf(line, "%7[^,],%llu,%llu,%llu,%llu,%llu,%llu,%llu",
                   role, &txn, &att, &conf, &maxret, &acq, &acqmax, &hold) != 8)
            continue;

        tot_txn  += txn;
        tot_att  += att;
        tot_conf += conf;
        tot_acq  += acq;
        tot_hold += hold;
        if (maxret > tot_maxret) tot_maxret = maxret;
        if (acqmax > max_acq)    max_acq    = acqmax;
        n++;
    }
    fclose(f);
    unlink(out_path);
    unlink(path);

    printf("%d,%d,%u,%d,%llu,%.1f,%llu,%llu,%.4f,%llu,%.3f,%.3f,%.3f\n",
           readers, writers, backoff, seconds,
           (unsigned long long)tot_txn,
           seconds > 0 ? (double)tot_txn / seconds : 0.0,
           (unsigned long long)tot_att,
           (unsigned long long)tot_conf,
           tot_att > 0 ? (double)tot_conf / (double)tot_att : 0.0,
           (unsigned long long)tot_maxret,
           tot_txn > 0 ? (double)tot_acq / (double)tot_txn / 1e3 : 0.0,
           (double)max_acq / 1e3,
           tot_txn > 0 ? (double)tot_hold / (double)tot_txn / 1e3 : 0.0);

    (void)n;
    return 0;
}
