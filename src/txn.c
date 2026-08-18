#include "txn.h"

#include <string.h>
#include <unistd.h>

static khb_status require_txn(const khb_db *db, int need_write)
{
    if (db == NULL)
        return KHB_ERR_INVALID;
    if (db->txn == TXN_NONE)
        return KHB_ERR_STATE;
    if (need_write && db->txn != TXN_WRITE)
        return KHB_ERR_STATE;
    return KHB_OK;
}

static int journal_present(const khb_db *db)
{
    char jpath[PATH_MAX];

    journal_path_for(db->pager.path, jpath, sizeof jpath);
    return access(jpath, F_OK) == 0;
}

static khb_status finish_txn(khb_db *db)
{
    catalog_init(&db->catalog);
    db->txn = TXN_NONE;
    return lock_release(&db->lock);
}

khb_status db_open(khb_db *db, const char *path, int create)
{
    khb_status rc;

    if (db == NULL || path == NULL)
        return KHB_ERR_INVALID;

    memset(db, 0, sizeof *db);

    rc = pager_open(&db->pager, path, create);
    if (rc != KHB_OK)
        return rc;

    lock_init(&db->lock, db->pager.fd);
    journal_init(&db->journal);
    catalog_init(&db->catalog);
    db->txn = TXN_NONE;

    if (journal_present(db)) {
        rc = lock_acquire(&db->lock, LOCK_EXCLUSIVE);
        if (rc != KHB_OK) {
            pager_close(&db->pager);
            return rc;
        }

        rc = journal_recover(&db->pager);
        (void)lock_release(&db->lock);

        if (rc != KHB_OK) {
            pager_close(&db->pager);
            return rc;
        }
    }

    return KHB_OK;
}

khb_status db_close(khb_db *db)
{
    if (db == NULL)
        return KHB_ERR_INVALID;

    if (db->txn != TXN_NONE)
        (void)txn_rollback(db);

    journal_free(&db->journal);
    return pager_close(&db->pager);
}

khb_status txn_begin(khb_db *db, int read_only)
{
    khb_status rc;

    if (db == NULL)
        return KHB_ERR_INVALID;
    if (db->txn != TXN_NONE)
        return KHB_ERR_STATE;

    rc = lock_acquire(&db->lock, read_only ? LOCK_SHARED : LOCK_EXCLUSIVE);
    if (rc != KHB_OK)
        return rc;

    rc = pager_reload_header(&db->pager);
    if (rc != KHB_OK) {
        (void)lock_release(&db->lock);
        return rc;
    }

    rc = catalog_load(&db->catalog, &db->pager);
    if (rc != KHB_OK) {
        (void)lock_release(&db->lock);
        return rc;
    }

    if (read_only) {
        db->txn = TXN_READ;
        return KHB_OK;
    }

    rc = journal_begin(&db->journal, &db->pager);
    if (rc != KHB_OK) {
        catalog_init(&db->catalog);
        (void)lock_release(&db->lock);
        return rc;
    }

    db->txn = TXN_WRITE;
    return KHB_OK;
}

khb_status txn_commit(khb_db *db)
{
    khb_status rc;

    rc = require_txn(db, 0);
    if (rc != KHB_OK)
        return rc;

    if (db->txn == TXN_READ)
        return finish_txn(db);

    rc = catalog_flush(&db->catalog, &db->pager, &db->journal);
    if (rc != KHB_OK)
        return rc;

    rc = journal_commit(&db->journal, &db->pager);
    if (rc != KHB_OK)
        return rc;

    return finish_txn(db);
}

khb_status txn_rollback(khb_db *db)
{
    khb_status rc, rc2;

    rc = require_txn(db, 0);
    if (rc != KHB_OK)
        return rc;

    if (db->txn == TXN_READ)
        return finish_txn(db);

    rc  = journal_rollback(&db->journal, &db->pager);
    rc2 = finish_txn(db);

    return (rc != KHB_OK) ? rc : rc2;
}

txn_state txn_current(const khb_db *db)
{
    return (db == NULL) ? TXN_NONE : db->txn;
}

khb_status txn_create_table(khb_db *db, const char *name,
                            const column_def_t *cols, int ncols,
                            table_def_t **out)
{
    table_def_t *t = NULL;
    uint32_t     root;
    khb_status   rc;

    rc = require_txn(db, 1);
    if (rc != KHB_OK)
        return rc;

    rc = catalog_create_table(&db->catalog, name, cols, ncols, &t);
    if (rc != KHB_OK)
        return rc;

    rc = btree_create(&db->pager, &db->journal, t, &root);
    if (rc != KHB_OK)
        return rc;

    catalog_set_root_page(&db->catalog, t, root);

    if (out != NULL)
        *out = t;
    return KHB_OK;
}

khb_status txn_find_table(khb_db *db, const char *name, table_def_t **out)
{
    khb_status rc;

    rc = require_txn(db, 0);
    if (rc != KHB_OK)
        return rc;

    return catalog_find_table(&db->catalog, name, out);
}

khb_status txn_open_tree(khb_db *db, table_def_t *t, btree_t *out)
{
    khb_status rc;

    rc = require_txn(db, 0);
    if (rc != KHB_OK)
        return rc;
    if (t == NULL || out == NULL)
        return KHB_ERR_INVALID;
    if (t->root_page == 0)
        return KHB_ERR_CORRUPT;

    btree_open(out, &db->pager,
               (db->txn == TXN_WRITE) ? &db->journal : NULL,
               t, t->root_page);
    return KHB_OK;
}

khb_status txn_close_tree(khb_db *db, table_def_t *t, btree_t *bt)
{
    khb_status rc;

    rc = require_txn(db, 0);
    if (rc != KHB_OK)
        return rc;
    if (t == NULL || bt == NULL)
        return KHB_ERR_INVALID;

    if (btree_root(bt) != t->root_page) {
        if (db->txn != TXN_WRITE)
            return KHB_ERR_STATE;
        catalog_set_root_page(&db->catalog, t, btree_root(bt));
    }

    return KHB_OK;
}
