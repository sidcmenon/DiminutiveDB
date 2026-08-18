#ifndef KHB_INTERNAL_H
#define KHB_INTERNAL_H

#include "khabibdb.h"
#include "pager.h"
#include "lockmgr.h"
#include "journal.h"
#include "catalog.h"

typedef enum {
    TXN_NONE  = 0,
    TXN_READ  = 1,
    TXN_WRITE = 2
} txn_state;

struct khb_db {
    pager_t   pager;
    lock_t    lock;
    journal_t journal;
    catalog_t catalog;
    txn_state txn;
};

#endif /* KHB_INTERNAL_H */
