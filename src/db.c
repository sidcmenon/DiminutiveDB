#include "khabibdb.h"
#include "internal.h"
#include "txn.h"
#include "row.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(table_def_t) <= KHB_ROW_SCHEMA_BYTES,
               "khb_row schema buffer too small for table_def_t");
_Static_assert(KHB_MAX_ROW_SIZE <= KHB_ROW_MAX,
               "khb_row data buffer too small for a maximum row");
_Static_assert(KHB_NAME_MAX == KHB_MAX_NAME, "public name limit must match");
_Static_assert(KHB_COLUMNS_MAX == KHB_MAX_COLUMNS,
               "public column limit must match");
_Static_assert((int)KHB_INT64  == (int)COL_INT64,  "type enums must match");
_Static_assert((int)KHB_UINT64 == (int)COL_UINT64, "type enums must match");
_Static_assert((int)KHB_DOUBLE == (int)COL_DOUBLE, "type enums must match");
_Static_assert((int)KHB_BOOL   == (int)COL_BOOL,   "type enums must match");
_Static_assert((int)KHB_TEXT   == (int)COL_TEXT,   "type enums must match");
_Static_assert((int)KHB_BLOB   == (int)COL_BLOB,   "type enums must match");

static const table_def_t *row_schema(const khb_row *row)
{
    return (const table_def_t *)(const void *)row->_schema;
}

static void bind_row(row_t *r, const khb_row *row)
{
    row_bind(r, row_schema(row), (uint8_t *)(void *)row->_data);
}

static khb_status auto_begin(khb_db *db, int read_only, int *wrapped)
{
    txn_state st = txn_current(db);

    if (st != TXN_NONE) {
        if (!read_only && st != TXN_WRITE)
            return KHB_ERR_STATE;
        *wrapped = 0;
        return KHB_OK;
    }

    *wrapped = 1;
    return txn_begin(db, read_only);
}

static khb_status auto_end(khb_db *db, int wrapped, khb_status rc)
{
    if (!wrapped)
        return rc;

    if (rc != KHB_OK) {
        (void)txn_rollback(db);
        return rc;
    }
    return txn_commit(db);
}

khb_status khb_open(khb_db **out, const char *path, int create)
{
    khb_db    *db;
    khb_status rc;

    if (out == NULL || path == NULL)
        return KHB_ERR_INVALID;

    db = malloc(sizeof *db);
    if (db == NULL)
        return KHB_ERR_NOMEM;

    rc = db_open(db, path, create);
    if (rc != KHB_OK) {
        free(db);
        return rc;
    }

    *out = db;
    return KHB_OK;
}

khb_status khb_close(khb_db *db)
{
    khb_status rc;

    if (db == NULL)
        return KHB_ERR_INVALID;

    rc = db_close(db);
    free(db);
    return rc;
}

khb_status khb_begin(khb_db *db, int read_only)
{
    return txn_begin(db, read_only);
}

khb_status khb_commit(khb_db *db)
{
    return txn_commit(db);
}

khb_status khb_rollback(khb_db *db)
{
    return txn_rollback(db);
}

khb_status khb_create_table(khb_db *db, const char *table,
                            const khb_column *cols, int ncols)
{
    column_def_t defs[KHB_MAX_COLUMNS];
    khb_status   rc;
    int          wrapped = 0, i;

    if (db == NULL || table == NULL || cols == NULL)
        return KHB_ERR_INVALID;
    if (ncols <= 0 || ncols > KHB_MAX_COLUMNS)
        return KHB_ERR_INVALID;

    for (i = 0; i < ncols; i++) {
        if (cols[i].name == NULL)
            return KHB_ERR_INVALID;
        column_def_set(&defs[i], cols[i].name, (uint8_t)cols[i].type,
                       (uint16_t)cols[i].size);
    }

    rc = auto_begin(db, 0, &wrapped);
    if (rc != KHB_OK)
        return rc;

    rc = txn_create_table(db, table, defs, ncols, NULL);
    return auto_end(db, wrapped, rc);
}

khb_status khb_table_exists(khb_db *db, const char *table)
{
    khb_status rc;
    int        wrapped = 0;

    if (db == NULL || table == NULL)
        return KHB_ERR_INVALID;

    rc = auto_begin(db, 1, &wrapped);
    if (rc != KHB_OK)
        return rc;

    rc = txn_find_table(db, table, NULL);

    if (wrapped) {
        khb_status end = txn_commit(db);
        if (rc == KHB_OK && end != KHB_OK)
            return end;
    }
    return rc;
}

khb_status khb_row_init(khb_row *row, khb_db *db, const char *table)
{
    table_def_t *t = NULL;
    khb_status   rc;
    int          wrapped = 0;

    if (row == NULL || db == NULL || table == NULL)
        return KHB_ERR_INVALID;

    rc = auto_begin(db, 1, &wrapped);
    if (rc != KHB_OK)
        return rc;

    rc = txn_find_table(db, table, &t);
    if (rc == KHB_OK) {
        memset(row, 0, sizeof *row);
        memcpy(row->_schema, t, sizeof *t);
    }

    if (wrapped) {
        khb_status end = txn_commit(db);
        if (rc == KHB_OK && end != KHB_OK)
            return end;
    }
    return rc;
}

int khb_row_columns(const khb_row *row)
{
    row_t r;

    if (row == NULL)
        return 0;
    bind_row(&r, row);
    return row_column_count(&r);
}

khb_type khb_row_type(const khb_row *row, int col)
{
    row_t r;

    bind_row(&r, row);
    return (khb_type)row_column_type(&r, col);
}

#define KHB_ROW_FWD(fn, inner, argtype)                                      \
    khb_status fn(khb_row *row, int col, argtype v)                          \
    {                                                                        \
        row_t r;                                                             \
        if (row == NULL) return KHB_ERR_INVALID;                             \
        bind_row(&r, row);                                                   \
        return inner(&r, col, v);                                            \
    }

#define KHB_ROW_FWD_GET(fn, inner, argtype)                                  \
    khb_status fn(const khb_row *row, int col, argtype out)                  \
    {                                                                        \
        row_t r;                                                             \
        if (row == NULL) return KHB_ERR_INVALID;                             \
        bind_row(&r, row);                                                   \
        return inner(&r, col, out);                                          \
    }

KHB_ROW_FWD(khb_row_set_int,    row_set_int,    int64_t)
KHB_ROW_FWD(khb_row_set_uint,   row_set_uint,   uint64_t)
KHB_ROW_FWD(khb_row_set_double, row_set_double, double)
KHB_ROW_FWD(khb_row_set_bool,   row_set_bool,   int)
KHB_ROW_FWD(khb_row_set_text,   row_set_text,   const char *)

KHB_ROW_FWD_GET(khb_row_get_int,    row_get_int,    int64_t *)
KHB_ROW_FWD_GET(khb_row_get_uint,   row_get_uint,   uint64_t *)
KHB_ROW_FWD_GET(khb_row_get_double, row_get_double, double *)
KHB_ROW_FWD_GET(khb_row_get_bool,   row_get_bool,   int *)

khb_status khb_row_get_text(const khb_row *row, int col, char *out, size_t cap)
{
    row_t r;

    if (row == NULL)
        return KHB_ERR_INVALID;
    bind_row(&r, row);
    return row_get_text(&r, col, out, cap);
}

khb_status khb_row_set_blob(khb_row *row, int col, const void *data, size_t len)
{
    row_t r;

    if (row == NULL)
        return KHB_ERR_INVALID;
    bind_row(&r, row);
    return row_set_blob(&r, col, data, len);
}

khb_status khb_row_get_blob(const khb_row *row, int col, void *out, size_t cap)
{
    row_t r;

    if (row == NULL)
        return KHB_ERR_INVALID;
    bind_row(&r, row);
    return row_get_blob(&r, col, out, cap);
}

khb_status khb_row_key(const khb_row *row, int64_t *out)
{
    row_t r;

    if (row == NULL)
        return KHB_ERR_INVALID;
    bind_row(&r, row);
    return row_key(&r, out);
}

khb_status khb_insert(khb_db *db, const char *table, const khb_row *row)
{
    table_def_t *t = NULL;
    btree_t      bt;
    khb_status   rc;
    int          wrapped = 0;

    if (db == NULL || table == NULL || row == NULL)
        return KHB_ERR_INVALID;

    rc = auto_begin(db, 0, &wrapped);
    if (rc != KHB_OK)
        return rc;

    rc = txn_find_table(db, table, &t);
    if (rc == KHB_OK)
        rc = txn_open_tree(db, t, &bt);
    if (rc == KHB_OK) {
        rc = btree_insert(&bt, row->_data);
        if (rc == KHB_OK)
            rc = txn_close_tree(db, t, &bt);
        else
            (void)txn_close_tree(db, t, &bt);
    }

    return auto_end(db, wrapped, rc);
}

khb_status khb_get(khb_db *db, const char *table, int64_t key, khb_row *out)
{
    table_def_t *t = NULL;
    btree_t      bt;
    khb_status   rc;
    int          wrapped = 0;

    if (db == NULL || table == NULL || out == NULL)
        return KHB_ERR_INVALID;

    rc = auto_begin(db, 1, &wrapped);
    if (rc != KHB_OK)
        return rc;

    rc = txn_find_table(db, table, &t);
    if (rc == KHB_OK)
        rc = txn_open_tree(db, t, &bt);
    if (rc == KHB_OK) {
        memset(out, 0, sizeof *out);
        memcpy(out->_schema, t, sizeof *t);
        rc = btree_lookup(&bt, key, out->_data);
    }

    if (wrapped) {
        khb_status end = txn_commit(db);
        if (rc == KHB_OK && end != KHB_OK)
            return end;
    }
    return rc;
}

khb_status khb_delete(khb_db *db, const char *table, int64_t key)
{
    table_def_t *t = NULL;
    btree_t      bt;
    khb_status   rc;
    int          wrapped = 0;

    if (db == NULL || table == NULL)
        return KHB_ERR_INVALID;

    rc = auto_begin(db, 0, &wrapped);
    if (rc != KHB_OK)
        return rc;

    rc = txn_find_table(db, table, &t);
    if (rc == KHB_OK)
        rc = txn_open_tree(db, t, &bt);
    if (rc == KHB_OK) {
        rc = btree_delete(&bt, key);
        (void)txn_close_tree(db, t, &bt);
    }

    return auto_end(db, wrapped, rc);
}

khb_status khb_count(khb_db *db, const char *table, uint32_t *out)
{
    table_def_t *t = NULL;
    btree_t      bt;
    khb_status   rc;
    int          wrapped = 0;

    if (db == NULL || table == NULL || out == NULL)
        return KHB_ERR_INVALID;

    rc = auto_begin(db, 1, &wrapped);
    if (rc != KHB_OK)
        return rc;

    rc = txn_find_table(db, table, &t);
    if (rc == KHB_OK)
        rc = txn_open_tree(db, t, &bt);
    if (rc == KHB_OK)
        rc = btree_count(&bt, out);

    if (wrapped) {
        khb_status end = txn_commit(db);
        if (rc == KHB_OK && end != KHB_OK)
            return end;
    }
    return rc;
}

static khb_status scan_impl(khb_db *db, const char *table, int ranged,
                            int64_t lo, int64_t hi, khb_predicate pred,
                            khb_visitor visit, void *ctx)
{
    table_def_t   *t = NULL;
    btree_t        bt;
    btree_cursor_t cur;
    khb_row        row;
    khb_status     rc;
    int            wrapped = 0;

    if (db == NULL || table == NULL || visit == NULL)
        return KHB_ERR_INVALID;

    rc = auto_begin(db, 1, &wrapped);
    if (rc != KHB_OK)
        return rc;

    rc = txn_find_table(db, table, &t);
    if (rc == KHB_OK)
        rc = txn_open_tree(db, t, &bt);

    if (rc == KHB_OK) {
        memset(&row, 0, sizeof row);
        memcpy(row._schema, t, sizeof *t);

        rc = ranged ? btree_cursor_seek(&bt, lo, &cur)
                    : btree_cursor_first(&bt, &cur);

        while (rc == KHB_OK) {
            int64_t key;

            rc = btree_cursor_next(&bt, &cur, row._data);
            if (rc == KHB_ERR_NOTFOUND) {
                rc = KHB_OK;
                break;
            }
            if (rc != KHB_OK)
                break;

            memcpy(&key, row._data, sizeof key);
            if (ranged && key > hi)
                break;

            if (pred != NULL && !pred(&row, ctx))
                continue;

            rc = visit(&row, ctx);
            if (rc != KHB_OK)
                break;
        }
    }

    if (wrapped) {
        khb_status end = txn_commit(db);
        if (rc == KHB_OK && end != KHB_OK)
            return end;
    }
    return rc;
}

khb_status khb_scan(khb_db *db, const char *table, khb_predicate pred,
                    khb_visitor visit, void *ctx)
{
    return scan_impl(db, table, 0, 0, 0, pred, visit, ctx);
}

khb_status khb_scan_range(khb_db *db, const char *table, int64_t lo,
                          int64_t hi, khb_predicate pred, khb_visitor visit,
                          void *ctx)
{
    if (lo > hi)
        return KHB_ERR_INVALID;
    return scan_impl(db, table, 1, lo, hi, pred, visit, ctx);
}
