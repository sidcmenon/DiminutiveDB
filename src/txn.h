#ifndef KHB_TXN_H
#define KHB_TXN_H

#include "khabibdb.h"
#include "internal.h"
#include "btree.h"

khb_status db_open(khb_db *db, const char *path, int create);
khb_status db_close(khb_db *db);

khb_status txn_begin(khb_db *db, int read_only);
khb_status txn_commit(khb_db *db);
khb_status txn_rollback(khb_db *db);
txn_state  txn_current(const khb_db *db);

khb_status txn_create_table(khb_db *db, const char *name,
                            const column_def_t *cols, int ncols,
                            table_def_t **out);
khb_status txn_find_table(khb_db *db, const char *name, table_def_t **out);

khb_status txn_open_tree(khb_db *db, table_def_t *t, btree_t *out);
khb_status txn_close_tree(khb_db *db, table_def_t *t, btree_t *bt);

#endif /* KHB_TXN_H */
