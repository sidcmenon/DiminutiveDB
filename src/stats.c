#include "stats.h"

#include <string.h>
#include <time.h>

khb_stats khb_stat;

uint64_t khb_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t khb_cpu_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * Wall time minus CPU time over the same span is time the process was not
 * running -- for a span containing only a syscall, that is I/O wait. It also
 * absorbs time descheduled for unrelated reasons, so it is a good proxy on an
 * idle machine rather than an exact figure.
 */
void khb_stat_span(uint64_t *ns, uint64_t *wait, uint64_t w0, uint64_t c0)
{
    uint64_t w = khb_now_ns() - w0;
    uint64_t c = khb_cpu_ns() - c0;

    *ns   += w;
    *wait += (w > c) ? (w - c) : 0;
}

void khb_stats_reset(void)
{
    memset(&khb_stat, 0, sizeof khb_stat);
}

/* Cost of one wall+cpu clock pair, so callers can judge instrumentation bias. */
uint64_t khb_timer_overhead_ns(void)
{
    const int N = 10000;
    uint64_t  t0, t1;
    int       i;

    t0 = khb_now_ns();
    for (i = 0; i < N; i++) {
        (void)khb_now_ns();
        (void)khb_cpu_ns();
    }
    t1 = khb_now_ns();

    return (t1 - t0) / (uint64_t)N;
}
