#ifndef KHB_STATS_H
#define KHB_STATS_H

#include <stdint.h>

typedef struct {
    uint64_t reads, writes, fsyncs, dir_fsyncs, journal_records;
    uint64_t crc_calls, crc_bytes;
    uint64_t read_ns,  write_ns,  fsync_ns;
    uint64_t read_wait_ns, write_wait_ns, fsync_wait_ns;
    uint64_t lock_attempts, lock_conflicts, lock_acquired, lock_hold_ns;
} khb_stats;

extern khb_stats khb_stat;

uint64_t khb_now_ns(void);
uint64_t khb_cpu_ns(void);
void     khb_stat_span(uint64_t *ns, uint64_t *wait, uint64_t w0, uint64_t c0);
void     khb_stats_reset(void);
uint64_t khb_timer_overhead_ns(void);

#endif /* KHB_STATS_H */
