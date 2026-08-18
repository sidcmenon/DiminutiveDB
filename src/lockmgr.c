#include "lockmgr.h"
#include <errno.h>
#include <sys/file.h>

#include "stats.h"

static uint64_t lock_held_since;

static khb_status lock_acquire_raw(lock_t *l, lock_state want);
static khb_status lock_release_raw(lock_t *l);

khb_status lock_acquire(lock_t *l, lock_state want)
{
    khb_status rc = lock_acquire_raw(l, want);

    khb_stat.lock_attempts++;
    if (rc == KHB_ERR_LOCKED) {
        khb_stat.lock_conflicts++;
    } else if (rc == KHB_OK) {
        khb_stat.lock_acquired++;
        lock_held_since = khb_now_ns();
    }
    return rc;
}

khb_status lock_release(lock_t *l)
{
    khb_status rc = lock_release_raw(l);

    if (rc == KHB_OK && lock_held_since != 0) {
        khb_stat.lock_hold_ns += khb_now_ns() - lock_held_since;
        lock_held_since = 0;
    }
    return rc;
}

void lock_init(lock_t *l, int fd){
    l->fd = fd;
    l-> state = LOCK_NONE;
}

static khb_status lock_acquire_raw(lock_t *l, lock_state want){
    int op;
    if (l == NULL || l->fd < 0){
        return KHB_ERR_INVALID;
    }
    if (want != LOCK_SHARED && want != LOCK_EXCLUSIVE){
        return KHB_ERR_INVALID;
    }

    if (l->state != LOCK_NONE){
        return KHB_ERR_STATE;
    }

    op = (want == LOCK_SHARED) ? LOCK_SH : LOCK_EX;

    if (flock(l->fd, op | LOCK_NB)!=0){
        if (errno == EWOULDBLOCK || errno == EAGAIN){
            return KHB_ERR_LOCKED;
        }
        return KHB_ERR_IO;
    }
    l-> state = want;
    return KHB_OK;
}

static khb_status lock_release_raw(lock_t *l){
    if (l == NULL || l->fd < 0){
        return KHB_ERR_INVALID;
    }
    if (l->state == LOCK_NONE){
        return KHB_ERR_STATE;
    }
    if (flock(l->fd, LOCK_UN)!=0){
        return KHB_ERR_IO;
    }
    l->state = LOCK_NONE;
    return KHB_OK;
}

lock_state lock_current(const lock_t *l){
    return l->state;
}

const char *lock_state_name(lock_state s){
    switch(s){
        case LOCK_NONE: return "LOCK_NONE";
        case LOCK_SHARED: return "LOCK_SHARED";
        case LOCK_EXCLUSIVE: return "LOCK_EXCLUSIVE";
    }
    return "LOCK_UNKNOWN";
}