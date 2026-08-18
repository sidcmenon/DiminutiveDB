#ifndef KHB_LOCKMGR_H
#define KHB_LOCKMGR_H

#include "khabibdb.h"

typedef enum{
    LOCK_NONE = 0,
    LOCK_SHARED = 1,
    LOCK_EXCLUSIVE = 2,
} lock_state;

typedef struct{
    int fd;
    lock_state state;
} lock_t;

void lock_init(lock_t *l, int fd);

khb_status lock_acquire(lock_t *l, lock_state want);

khb_status lock_release(lock_t *t);

lock_state lock_current(const lock_t *t);

const char *lock_state_name(lock_state s);

#endif /* KHB_LOCKMGR_H*/

